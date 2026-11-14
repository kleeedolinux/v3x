/* v3x_testkit.h - offline tests for V3X plugins without starting the game (header-only, C).
 *
 * Maps victoria3.exe as an image (LOAD_LIBRARY_AS_IMAGE_RESOURCE: sections at their RVAs, no
 * code runs, no relocations) and gives you a V3X_API whose scanners behave like src/core.c on
 * it, so your plugin's real anchor code can be checked against the real exe:
 *
 *   #include "v3x_testkit.h"
 *   #include "../my_plugin.c"          // or link your resolve function
 *   int main(int argc, char **argv) {
 *       V3X_API api;
 *       if (argc > 1 && v3x_testkit_map_exe(argv[1], &api) == 0) {
 *           V3X_TEST_CHECK(my_resolve(&api) == 0, "anchors resolve on %s", argv[1]);
 *       }
 *       return v3x_testkit_done();     // prints "N checks, M failed" + ALL OK/FAILED, exit code
 *   }
 *
 * Hooks, patches, on_frame, console and events are stubs that record calls (they return 0 and
 * do nothing: the image is not executable). log/print go to stdout. Addresses the plugin
 * computes are inside the mapping; use v3x_testkit_rva(p) to compare with notes / tools output
 * (RVA = address - base). Absolute pointers stored inside the image are NOT relocated: they are
 * relative to the exe's preferred ImageBase (0x140000000), see v3x_testkit_ptr().
 * One mapped image per process (the fake API functions have no context argument). */
#ifndef V3X_TESTKIT_H
#define V3X_TESTKIT_H

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "v3x.h"

static struct {
    uint8_t *base; size_t size;
    uint8_t *text; size_t text_size;
    struct { uint8_t *p; size_t n; } data[8]; int ndata;
    IMAGE_RUNTIME_FUNCTION_ENTRY *pdata; size_t npdata;
    uint64_t pref_base;
    int hooks, patches, frames, commands, checks, fails;
    int log_level;   /* API v5 log_level() of the fake API (0 = V3X_LOG_INFO) */
    char dir[260];
} v3x_tk;

#define V3X_TEST_CHECK(c, ...)                                                   \
    do {                                                                         \
        int ok_ = !!(c);                                                         \
        v3x_tk.checks++; v3x_tk.fails += !ok_;                                   \
        printf("%s: ", ok_ ? "OK  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); \
    } while (0)

static int v3x_testkit_done(void)
{
    printf("%d checks, %d failed\n%s\n", v3x_tk.checks, v3x_tk.fails, v3x_tk.fails ? "FAILED" : "ALL OK");
    return v3x_tk.fails ? 1 : 0;
}

static unsigned long long v3x_testkit_rva(const void *p) { return p ? (unsigned long long)((const uint8_t *)p - v3x_tk.base) : 0; }
/* An absolute pointer read from the unrelocated image -> address inside the mapping. */
static void *v3x_testkit_ptr(uint64_t abs) { return abs ? v3x_tk.base + (abs - v3x_tk.pref_base) : NULL; }

static void v3x_tk_log(const char *tag, const char *fmt, ...) V3X_PRINTF(2, 3);
static void v3x_tk_log(const char *tag, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); printf("[%s] ", tag ? tag : "?"); vprintf(fmt, ap); printf("\n"); va_end(ap); }
static int v3x_tk_print(const char *fmt, ...) V3X_PRINTF(1, 2);
static int v3x_tk_print(const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int n = vprintf(fmt, ap); va_end(ap); if (!*fmt || fmt[strlen(fmt) - 1] != '\n') printf("\n"); return n; }
static uintptr_t v3x_tk_base(void) { return (uintptr_t)v3x_tk.base; }
static size_t v3x_tk_size(void) { return v3x_tk.size; }
static const char *v3x_tk_version(void) { return "testkit"; }
static const char *v3x_tk_build(void) { return "testkit"; }
static const char *v3x_tk_dir(void) { return v3x_tk.dir; }
static uint32_t v3x_tk_main_tid(void) { return GetCurrentThreadId(); }

static void *v3x_tk_pattern_scan(const char *pat)
{
    uint8_t b[256], m[256]; int n = 0;
    for (const char *s = pat; *s && n < 256;) {
        if (*s == ' ') { s++; continue; }
        if (*s == '?') { b[n] = 0; m[n++] = 0; s++; if (*s == '?') s++; continue; }
        char h[3] = {s[0], (char)(s[1] ? s[1] : 0), 0}; b[n] = (uint8_t)strtoul(h, NULL, 16); m[n++] = 1; s += s[1] ? 2 : 1;
    }
    for (size_t i = 0; n && i + n <= v3x_tk.text_size; i++) {
        if (m[0] && v3x_tk.text[i] != b[0]) continue;
        int k = 1;
        while (k < n && (!m[k] || v3x_tk.text[i + k] == b[k])) k++;
        if (k == n) return v3x_tk.text + i;
    }
    return NULL;
}
static void *v3x_tk_find_string(const char *s)
{
    size_t n = strlen(s) + 1;
    for (int r = 0; r < v3x_tk.ndata; r++)
        for (size_t i = 0; i + n <= v3x_tk.data[r].n; i++)
            if (v3x_tk.data[r].p[i] == (uint8_t)s[0] && !memcmp(v3x_tk.data[r].p + i, s, n) &&
                (i == 0 || v3x_tk.data[r].p[i - 1] == 0))
                return v3x_tk.data[r].p + i;
    return NULL;
}
static void *v3x_tk_find_xref(const void *target, int index)
{
    for (size_t i = 0; i + 7 <= v3x_tk.text_size; i++) {
        const uint8_t *t = v3x_tk.text + i;
        if ((t[0] != 0x48 && t[0] != 0x4C) || (t[1] != 0x8D && t[1] != 0x8B) || (t[2] & 0xC7) != 0x05) continue;
        int32_t d; memcpy(&d, t + 3, 4);
        if (t + 7 + d == (const uint8_t *)target && index-- == 0) return (void *)t;
    }
    return NULL;
}
static void *v3x_tk_function_start(const void *addr)
{
    uint32_t rva = (uint32_t)((const uint8_t *)addr - v3x_tk.base);
    size_t lo = 0, hi = v3x_tk.npdata;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (rva < v3x_tk.pdata[mid].BeginAddress) hi = mid;
        else if (rva >= v3x_tk.pdata[mid].EndAddress) lo = mid + 1;
        else {
            IMAGE_RUNTIME_FUNCTION_ENTRY *rf = &v3x_tk.pdata[mid];
            for (int guard = 0; guard < 32; guard++) {   /* chained unwind info -> primary function */
                uint8_t *ui = v3x_tk.base + rf->UnwindData;
                if (!((ui[0] >> 3) & UNW_FLAG_CHAININFO)) break;
                rf = (IMAGE_RUNTIME_FUNCTION_ENTRY *)(ui + 4 + ((ui[2] + 1) & ~1u) * 2);
            }
            return v3x_tk.base + rf->BeginAddress;
        }
    }
    return NULL;
}
static int v3x_tk_hook(void *t, void *d, void **o) { (void)t; (void)d; if (o) *o = NULL; v3x_tk.hooks++; return 0; }
static int v3x_tk_unhook(void *t) { (void)t; return 0; }
static int v3x_tk_patch(void *a, const void *b, size_t n) { (void)a; (void)b; (void)n; v3x_tk.patches++; return 0; }
static int v3x_tk_on_frame(v3x_frame_fn fn, void *u) { (void)fn; (void)u; v3x_tk.frames++; return 0; }
static int v3x_tk_register(const char *n, const char *h, v3x_command_fn fn, void *u) { (void)n; (void)h; (void)fn; (void)u; v3x_tk.commands++; return 0; }
static int v3x_tk_run_on_main(v3x_frame_fn fn, void *u) { fn(u); return 0; }   /* runs at once */
static int v3x_tk_exec(const char *l) { (void)l; return -1; }
static int v3x_tk_exec_capture(const char *l, void (*s)(const char *, void *), void *u) { (void)l; (void)s; (void)u; return -1; }
static int v3x_tk_on_event(const char *n, void (*fn)(const char *, void *, void *), void *u) { (void)n; (void)fn; (void)u; return 0; }
static int v3x_tk_emit(const char *n, void *d) { (void)n; (void)d; return 0; }
static int v3x_tk_hook_safe(void *t, v3x_hook_fn pre, v3x_hook_fn post, void *u) { (void)t; (void)pre; (void)post; (void)u; v3x_tk.hooks++; return 0; }
static int v3x_tk_enum(void (*fn)(const char *, const char *, void *), void *u) { (void)fn; (void)u; return 0; }
/* API v5: log_at prints like log (level prefix, threshold v3x_tk.log_level, default info) */
static int v3x_tk_log_level(const char *tag) { (void)tag; return v3x_tk.log_level ? v3x_tk.log_level : V3X_LOG_INFO; }
static void v3x_tk_log_at(int level, const char *tag, const char *fmt, ...) V3X_PRINTF(3, 4);
static void v3x_tk_log_at(int level, const char *tag, const char *fmt, ...)
{
    static const char *const pre[] = {"", "ERROR: ", "WARNING: ", "", "debug: ", "trace: "};
    if (level < 1) level = 1;
    if (level > 5) level = 5;
    if (level > v3x_tk_log_level(tag)) return;
    va_list ap; va_start(ap, fmt); printf("[%s] %s", tag ? tag : "?", pre[level]); vprintf(fmt, ap); printf("\n"); va_end(ap);
}
static const char *v3x_tk_loader_version(void) { return "testkit"; }
static int v3x_tk_build_matches(const char *list) { return !list || !*list || strchr(list, '*') != NULL; }

/* Fills api with the fake functions (base/scanners see whatever v3x_tk describes). */
static void v3x_testkit_api(V3X_API *api)
{
    memset(api, 0, sizeof *api);
    api->api_version = V3X_API_VERSION; api->struct_size = sizeof *api;
    api->log = v3x_tk_log; api->image_base = v3x_tk_base; api->image_size = v3x_tk_size; api->game_version = v3x_tk_version;
    api->pattern_scan = v3x_tk_pattern_scan; api->find_string = v3x_tk_find_string;
    api->find_xref = v3x_tk_find_xref; api->function_start = v3x_tk_function_start;
    api->hook = v3x_tk_hook; api->unhook = v3x_tk_unhook; api->patch = v3x_tk_patch; api->on_frame = v3x_tk_on_frame;
    api->game_dir = v3x_tk_dir; api->v3x_dir = v3x_tk_dir;
    api->register_command = v3x_tk_register; api->print = v3x_tk_print; api->run_on_main = v3x_tk_run_on_main;
    api->exec_command = v3x_tk_exec; api->exec_capture = v3x_tk_exec_capture;
    api->on_event = v3x_tk_on_event; api->emit_event = v3x_tk_emit;
    api->hook_safe = v3x_tk_hook_safe; api->main_thread_id = v3x_tk_main_tid; api->game_build_id = v3x_tk_build;
    api->enum_commands = v3x_tk_enum;
    api->log_at = v3x_tk_log_at; api->log_level = v3x_tk_log_level; api->loader_version = v3x_tk_loader_version;
    api->build_matches = v3x_tk_build_matches;
}

/* Describes a PE image already in memory (a mapped exe, or a synthetic one built by a test). */
static int v3x_testkit_use_image(uint8_t *base)
{
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    v3x_tk.base = base; v3x_tk.size = nt->OptionalHeader.SizeOfImage; v3x_tk.pref_base = nt->OptionalHeader.ImageBase;
    v3x_tk.text = NULL; v3x_tk.ndata = 0;
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        if (!memcmp(s->Name, ".text", 5)) { v3x_tk.text = base + s->VirtualAddress; v3x_tk.text_size = s->Misc.VirtualSize; }
        else if ((!memcmp(s->Name, ".rdata", 6) || !memcmp(s->Name, ".data", 5)) && v3x_tk.ndata < 8) {
            v3x_tk.data[v3x_tk.ndata].p = base + s->VirtualAddress; v3x_tk.data[v3x_tk.ndata++].n = s->Misc.VirtualSize;
        }
    }
    IMAGE_DATA_DIRECTORY *ex = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    v3x_tk.pdata = (IMAGE_RUNTIME_FUNCTION_ENTRY *)(base + ex->VirtualAddress);
    v3x_tk.npdata = ex->Size / sizeof *v3x_tk.pdata;
    return v3x_tk.text ? 0 : -1;
}

/* Maps exe_path read-only and fills api. 0 = ok. v3x_dir()/game_dir() return dir_or_null
 * (default "./") so plugins that read an ini find it there. */
static int v3x_testkit_map_exe_dir(const char *exe_path, const char *dir_or_null, V3X_API *api)
{
    HMODULE h = LoadLibraryExA(exe_path, NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!h) { printf("cannot map %s (error %lu)\n", exe_path, GetLastError()); return -1; }
    snprintf(v3x_tk.dir, sizeof v3x_tk.dir, "%s", dir_or_null ? dir_or_null : ".\\");
    if (v3x_testkit_use_image((uint8_t *)((uintptr_t)h & ~(uintptr_t)3))) return -1;
    v3x_testkit_api(api);
    return v3x_tk.npdata ? 0 : -1;
}
static int v3x_testkit_map_exe(const char *exe_path, V3X_API *api) { return v3x_testkit_map_exe_dir(exe_path, NULL, api); }

#endif
