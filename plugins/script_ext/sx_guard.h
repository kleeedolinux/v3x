/* sx_guard.h - ownership and fault containment of script_ext (audit N5).
 *
 * Owners: every pack and every native keyword belongs to one module (plugin DLL):
 *   - a pack belongs to the module that first called begin_pack for it in this session, or, if the
 *     pack's manifest from an earlier session names an owner ("#owner <file name>" line), only a module
 *     with that file name may claim it; begin_pack / write_file / end_pack from any other module fail;
 *   - a keyword belongs to the module that contains its callback.
 *   The calling module is found from the return address (GetModuleHandleExW FROM_ADDRESS). Code that
 *   is in no module (JIT, shellcode) owns nothing and is refused.
 * Disabled owners: the core emits V3X_EVENT_PLUGIN_DISABLED when it disables a plugin (fault, rollback);
 *   from then on script_ext never calls that module's keyword callbacks again and refuses its pack calls.
 * Fault guard: keyword callbacks run on engine (tick worker) threads, outside the core's guard. Each call
 *   runs under a per-thread guard: a vectored handler (added last, so crash_logger still sees the fault
 *   first) catches a hardware fault (access violation, illegal instruction, divide by zero, ...) raised
 *   while the guard is armed on that thread, resumes the thread at a landing function that longjmps
 *   back to the wrapper; the keyword is switched off for the rest of the session and the fault is
 *   logged as module+offset. C++ exceptions are not caught (the engine may catch its own). Plugins are
 *   compiled without -fms-extensions, so this is the frame-free equivalent of __try/__except: no
 *   unwinding of the faulting frames (C code has no destructors; a lock the callback held stays held). */
#ifndef SX_GUARD_H
#define SX_GUARD_H

#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "v3x.h"

#ifndef V3X_EVENT_PLUGIN_DISABLED   /* loaders before the trust-model API: same layout (agreed with the core) */
#define V3X_EVENT_PLUGIN_DISABLED "plugin_disabled"
typedef struct V3X_PluginDisabled {
    uint32_t struct_size;
    uint32_t reserved;
    const char *name;     /* file stem */
    void *module;         /* HMODULE */
    const char *reason;
} V3X_PluginDisabled;
#endif

/* error for a pack call by a module that does not own the pack (sdk/v3x_script.h may define it) */
#ifdef V3X_SCRIPT_EOWNER
#define SX_E_OWNER V3X_SCRIPT_EOWNER
#else
#define SX_E_OWNER V3X_SCRIPT_ESTATE
#endif

/* ------------------------------------------------------------------ module identity */

/* Test seam: when set, every pack call is attributed to this module instead of the return address. */
static HMODULE sx_test_caller;

/* module containing addr (no refcount change), NULL if none */
static HMODULE sx_module_of(const void *addr)
{
    HMODULE h = NULL;
    if (!addr || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     (LPCWSTR)addr, &h))
        return NULL;
    return h;
}

/* file name of the module (lowercase ASCII letters, UTF-8), "" if unknown */
static void sx_module_name(HMODULE h, char *out, size_t cap)
{
    if (!cap) return;
    out[0] = 0;
    wchar_t w[1024];
    DWORD n = h ? GetModuleFileNameW(h, w, 1024) : 0;
    if (!n || n >= 1024) return;
    const wchar_t *b = w + n;
    while (b > w && b[-1] != L'\\' && b[-1] != L'/') b--;
    int k = WideCharToMultiByte(CP_UTF8, 0, b, -1, out, (int)cap, NULL, NULL);
    if (k <= 0) { out[0] = 0; return; }
    for (char *p = out; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
}

/* ------------------------------------------------------------------ disabled modules */

#define SX_MAX_DISABLED 64
static void *volatile sx_disabled_mod[SX_MAX_DISABLED];
static volatile LONG sx_ndisabled;

static int sx_module_disabled(HMODULE h)
{
    if (!h) return 0;
    LONG n = sx_ndisabled;
    if (n > SX_MAX_DISABLED) n = SX_MAX_DISABLED;
    for (LONG i = 0; i < n; i++)
        if (sx_disabled_mod[i] == (void *)h) return 1;
    return 0;
}

/* returns 1 if h was newly added */
static int sx_module_disable(HMODULE h)
{
    if (!h || sx_module_disabled(h)) return 0;
    LONG i = InterlockedIncrement(&sx_ndisabled) - 1;
    if (i >= SX_MAX_DISABLED) { InterlockedDecrement(&sx_ndisabled); return 0; }
    sx_disabled_mod[i] = (void *)h;
    return 1;
}

/* ------------------------------------------------------------------ per-thread fault guard */

typedef struct SxGuard {
    jmp_buf jb;
    struct SxGuard *prev;
    volatile LONG armed;
    DWORD code;
    void *addr;          /* ExceptionAddress */
    ULONG_PTR info[2];   /* access violation: 0 read / 1 write / 8 DEP, address */
} SxGuard;

/* _setjmp with a NULL frame: longjmp then restores the registers without unwinding (an unwind from the
 * landing function would walk a fabricated frame). */
#define SX_SETJMP(b) _setjmp((b), NULL)

static _Thread_local SxGuard *sx_guard_cur;
static void *sx_guard_veh;

static __attribute__((noinline, noreturn)) void sx_guard_land(void)
{
    longjmp(sx_guard_cur->jb, 1);
}

static LONG CALLBACK sx_guard_filter(EXCEPTION_POINTERS *ep)
{
    SxGuard *g = sx_guard_cur;
    if (!g || !g->armed) return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    switch (er->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_IN_PAGE_ERROR: case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION: case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: case EXCEPTION_DATATYPE_MISALIGNMENT:
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
    g->armed = 0;
    g->code = er->ExceptionCode;
    g->addr = er->ExceptionAddress;
    g->info[0] = er->NumberParameters > 0 ? er->ExceptionInformation[0] : 0;
    g->info[1] = er->NumberParameters > 1 ? er->ExceptionInformation[1] : 0;
    /* resume at the landing function, as if called, on the faulting thread's stack below the fault */
    ep->ContextRecord->Rsp = ((ep->ContextRecord->Rsp - 512) & ~(DWORD64)15) - 8;
    ep->ContextRecord->Rip = (DWORD64)(uintptr_t)(void *)sx_guard_land;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void sx_guard_install(void)
{
    if (!sx_guard_veh) sx_guard_veh = AddVectoredExceptionHandler(0, sx_guard_filter);
}

/* The guarded calls. 0 = returned normally (*out = result for triggers), 1 = faulted (g filled).
 * noinline + compiler barriers: the guard must be armed in memory before the callback runs. */
static __attribute__((noinline)) int sx_guard_effect(SxGuard *g, v3x_script_effect_fn fn, const V3X_ScriptScope *sc,
                                                     const char *value, void *user)
{
    g->prev = sx_guard_cur;
    g->code = 0;
    if (SX_SETJMP(g->jb)) { sx_guard_cur = g->prev; return 1; }
    sx_guard_cur = g;
    g->armed = 1;
    __asm__ volatile("" ::: "memory");
    fn(sc, value, user);
    __asm__ volatile("" ::: "memory");
    g->armed = 0;
    sx_guard_cur = g->prev;
    return 0;
}

static __attribute__((noinline)) int sx_guard_trigger(SxGuard *g, v3x_script_trigger_fn fn, const V3X_ScriptScope *sc,
                                                      const char *value, void *user, volatile int *out)
{
    g->prev = sx_guard_cur;
    g->code = 0;
    *out = 0;
    if (SX_SETJMP(g->jb)) { sx_guard_cur = g->prev; return 1; }
    sx_guard_cur = g;
    g->armed = 1;
    __asm__ volatile("" ::: "memory");
    *out = fn(sc, value, user);
    __asm__ volatile("" ::: "memory");
    g->armed = 0;
    sx_guard_cur = g->prev;
    return 0;
}

/* "<module>+0x<offset>" of an address, or "0x<address>" outside any module */
static void sx_where(const void *addr, char *out, size_t cap)
{
    HMODULE h = sx_module_of(addr);
    char nm[128];
    sx_module_name(h, nm, sizeof nm);
    if (h && nm[0]) snprintf(out, cap, "%s+0x%llx", nm, (unsigned long long)((uintptr_t)addr - (uintptr_t)h));
    else snprintf(out, cap, "0x%llx", (unsigned long long)(uintptr_t)addr);
}

#endif
