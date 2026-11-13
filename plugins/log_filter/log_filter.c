/* log_filter: drop / dedupe noisy engine log messages before spdlog formats and writes them.
 *
 * Hook point: spdlog::logger::sink_it_(const details::log_msg &) - the one virtual every engine
 * log category goes through before its sinks (error.log, debug.log, in-game log viewer, ...).
 * Found without hardcoded addresses:
 *   RTTI ".?AVlogger@spdlog@@" -> TypeDescriptor -> CompleteObjectLocator -> vtable -> slot 2.
 * Before hooking we byte-check sink_it_ and the spdlog flag formatters that prove the log_msg
 * layout (see CHECKS, mirrored in tools/re_log_check.py). Any mismatch = no hook (fail closed).
 *
 * Config: <game>/binaries/v3x/log_filter.ini (built-in defaults = log_filter.ini.example).
 * Stats:  <game>/binaries/v3x/log_filter_stats.txt, every summary_interval s, on "load_transition"
 *         Game->Empty and on the core's "process_exit" (API v3, exiting thread, all threads alive).
 *         Nothing at DLL_PROCESS_DETACH: the detour and DllMain must not lock, allocate or wait
 *         once ExitProcess has killed the other threads. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "v3x.h"
#include "lf_rules.h"

V3X_EXPORT const char *v3x_plugin_name = "log_filter";
V3X_PLUGIN_MANIFEST("log_filter", "0.5.0", "v3x", "drops and dedupes noisy engine log messages", "release/1.15.1");

/* spdlog::details::log_msg (spdlog >= 1.12, MSVC x64) */
#define MSG_LEVEL    0x10
#define MSG_TIME     0x18
#define MSG_FILE     0x38
#define MSG_LINE     0x40
#define MSG_PAYLOAD  0x50
#define MSG_PAYLEN   0x58
#define MSG_SIZE     0x60

typedef void (*sink_it_fn)(void *logger, const void *msg);

typedef struct snap { void *logger; unsigned char msg[MSG_SIZE]; } snap;  /* dedupe user data */

static const V3X_API *api;
static sink_it_fn orig_sink_it;
static volatile LONG g_ready;
static SRWLOCK g_lock = SRWLOCK_INIT;
static lf_config g_cfg;
static char g_stats_path[MAX_PATH];
static ULONGLONG g_last_tick;
static volatile LONG g_dirty;

/* ntdll RtlDllShutdownInProgress: TRUE once ExitProcess has killed the other threads and runs DLL
 * detach. Engine code still logs then (DLL static destructors, TLS callbacks), and a dead thread may
 * own g_lock, so the detour must pass straight through. */
typedef BOOLEAN(NTAPI *shutdown_fn)(void);
static shutdown_fn g_shutdown_in_progress;
static int process_exiting(void) { return g_shutdown_in_progress && g_shutdown_in_progress(); }

/* ------------------------------------------------------------------ RTTI -> vtable */

typedef struct { const char *rtti; int slot; const char *pats[3]; int window; const char *what; } check_t;
static const check_t CHECKS[] = {   /* keep in sync with tools/re_log_check.py */
    { ".?AVlogger@spdlog@@", 2, { "FF5008", "FF5018", "83F906" }, 0xC0, "logger::sink_it_" },
    { ".?AV?$v_formatter@Unull_scoped_padder@details@spdlog@@@details@spdlog@@", 1,
      { "4C8B4258", "488B5250", 0 }, 0x20, "v_formatter (payload +0x50/+0x58)" },
    { ".?AV?$source_linenum_formatter@Unull_scoped_padder@details@spdlog@@@details@spdlog@@", 1,
      { "8B4A40", 0, 0 }, 0x10, "source_linenum_formatter (line +0x40)" },
    { ".?AV?$short_filename_formatter@Unull_scoped_padder@details@spdlog@@@details@spdlog@@", 1,
      { "488B4A38", 0, 0 }, 0x20, "short_filename_formatter (filename +0x38)" },
};

static int section(const char *name, unsigned char **p, size_t *n)
{
    unsigned char *base = (unsigned char *)api->image_base();
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        if (!strncmp((const char *)s->Name, name, 8)) {
            *p = base + s->VirtualAddress; *n = s->Misc.VirtualSize;
            return 1;
        }
    }
    return 0;
}

static void **find_vtable(const char *rtti)
{
    unsigned char *base = (unsigned char *)api->image_base(), *rd; size_t rn;
    char *name = (char *)api->find_string(rtti);
    if (!name || !section(".rdata", &rd, &rn)) return NULL;
    uint32_t td = (uint32_t)((unsigned char *)name - 0x10 - base);
    unsigned char *col = NULL;
    for (size_t off = 0; off + 24 <= rn; off += 4) {
        uint32_t *c = (uint32_t *)(rd + off);
        if (c[0] == 1 && c[1] == 0 && c[3] == td && c[5] == (uint32_t)(rd + off - base)) {
            if (col) return NULL;           /* ambiguous */
            col = rd + off;
        }
    }
    if (!col) return NULL;
    void **vt = NULL;
    for (size_t off = 0; off + 16 <= rn; off += 8) {
        if (*(unsigned char **)(rd + off) == col) {
            if (vt) return NULL;
            vt = (void **)(rd + off + 8);
        }
    }
    return vt;
}

static int has_bytes(const unsigned char *code, int window, const char *hex)
{
    unsigned char pat[8]; int n = 0;
    for (; hex[0] && hex[1] && n < 8; hex += 2) {
        unsigned v; sscanf(hex, "%2x", &v); pat[n++] = (unsigned char)v;
    }
    for (int i = 0; i + n <= window; i++)
        if (!memcmp(code + i, pat, n)) return 1;
    return 0;
}

/* ------------------------------------------------------------------ hook */

static void make_loc(const unsigned char *msg, char *loc, size_t n)
{
    const char *file = *(const char *const *)(msg + MSG_FILE);
    int line = *(const int *)(msg + MSG_LINE);
    const char *b = file ? file : "?";
    for (const char *q = b; *q; q++) if (*q == '\\' || *q == '/') b = q + 1;
    snprintf(loc, n, "%s:%d", b, line);
}

static void hk_sink_it(void *logger, const void *m)
{
    if (!g_ready || process_exiting()) { orig_sink_it(logger, m); return; }
    const unsigned char *msg = (const unsigned char *)m;
    int level = *(const int *)(msg + MSG_LEVEL);
    const char *pl = *(const char *const *)(msg + MSG_PAYLOAD);
    size_t len = *(const size_t *)(msg + MSG_PAYLEN);
    char loc[128];
    make_loc(msg, loc, sizeof loc);
    lf_dup *d = NULL;
    AcquireSRWLockExclusive(&g_lock);
    int r = lf_decide(&g_cfg, level, loc, pl ? pl : "", pl ? len : 0, &d);
    if (r == LF_DROP_DUP && d && !d->user) {
        snap *s = (snap *)malloc(sizeof *s);
        if (s) { s->logger = logger; memcpy(s->msg, msg, MSG_SIZE); d->user = s; }
    }
    if (r == LF_DROP_RULE || r == LF_DROP_DUP) g_dirty = 1;
    ReleaseSRWLockExclusive(&g_lock);
    if (r == LF_DROP_RULE || r == LF_DROP_DUP) return;
    orig_sink_it(logger, m);
}

/* ------------------------------------------------------------------ stats + summaries */

static void write_stats(int locked)
{
    size_t cap = 256 * 1024;
    char *buf = (char *)malloc(cap);
    if (!buf) return;
    if (locked) AcquireSRWLockShared(&g_lock);
    size_t n = lf_stats_report(&g_cfg, buf, cap);
    if (locked) ReleaseSRWLockShared(&g_lock);
    HANDLE h = CreateFileA(g_stats_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, buf, (DWORD)n, &w, NULL); CloseHandle(h); }
    free(buf);
}

#define MAX_SUMMARIES 64
static void emit_summaries(void)
{
    static struct { snap s; char text[LF_SAMPLE_LEN + 160]; } out[MAX_SUMMARIES];
    int k = 0;
    AcquireSRWLockExclusive(&g_lock);
    for (uint32_t i = 0; i < LF_DEDUPE_CAP && k < MAX_SUMMARIES; i++) {
        lf_dup *d = &g_cfg.dups[i];
        if (!d->user || !d->sample) continue;
        uint32_t supp = d->count - (uint32_t)g_cfg.dedupe_first;
        if (supp <= d->reported) continue;
        out[k].s = *(snap *)d->user;
        snprintf(out[k].text, sizeof out[k].text, "[log_filter] suppressed %u more like this (%u total so far): %s",
                 supp - d->reported, supp, d->sample);
        d->reported = supp;
        k++;
    }
    ReleaseSRWLockExclusive(&g_lock);
    FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
    long long now = (long long)(((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000LL;
    for (int i = 0; i < k; i++) {   /* through the trampoline: not filtered again */
        unsigned char *msg = out[i].s.msg;
        *(long long *)(msg + MSG_TIME) = now;           /* system_clock: 100 ns since 1970 */
        *(const char **)(msg + MSG_PAYLOAD) = out[i].text;
        *(size_t *)(msg + MSG_PAYLEN) = strlen(out[i].text);
        orig_sink_it(out[i].s.logger, msg);
    }
}

static void on_frame(void *user)
{
    (void)user;
    if (!g_ready) return;
    int iv = g_cfg.summary_interval > 0 ? g_cfg.summary_interval : 60;
    ULONGLONG t = GetTickCount64();
    if (t - g_last_tick < (ULONGLONG)iv * 1000) return;
    g_last_tick = t;
    if (g_cfg.summary_interval > 0 && g_cfg.dedupe_first > 0) emit_summaries();
    if (InterlockedExchange(&g_dirty, 0)) write_stats(1);
}

/* "load_transition" (fast_load) Game->Empty and "process_exit" (core): stats while every thread is
 * still alive (API v3) */
static void lf_on_event(const char *name, void *data, void *user)
{
    const V3X_LoadTransition *t = (const V3X_LoadTransition *)data;
    int exiting = user != 0;
    if (!g_ready) return;
    if (exiting || (t && t->from && t->to && !strcmp(t->from, "Game") && !strcmp(t->to, "Empty"))) {
        InterlockedExchange(&g_dirty, 0);
        write_stats(1);
    }
    (void)name;
}

/* Process exit (reserved != NULL): other threads are dead and may have held g_lock, the heap lock
 * or a CRT lock, so only a plain store here (the detour then passes through). A FreeLibrary-style
 * unload (reserved == NULL, threads alive) writes the stats normally. */
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)h;
    if (reason == DLL_PROCESS_DETACH && g_ready) {
        g_ready = 0;
        if (!reserved) write_stats(1);
    }
    return TRUE;
}

/* ------------------------------------------------------------------ load */

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = (n >= 0 && n < (1 << 20)) ? (char *)malloc((size_t)n + 1) : NULL;
    if (b) { n = (long)fread(b, 1, (size_t)n, f); b[n] = 0; }
    fclose(f);
    return b;
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    g_shutdown_in_progress = (shutdown_fn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlDllShutdownInProgress");
    void *targets[sizeof CHECKS / sizeof CHECKS[0]];
    for (size_t i = 0; i < sizeof CHECKS / sizeof CHECKS[0]; i++) {
        const check_t *c = &CHECKS[i];
        void **vt = find_vtable(c->rtti);
        unsigned char *fn = vt ? (unsigned char *)vt[c->slot] : NULL;
        int ok = fn != NULL;
        for (int p = 0; ok && p < 3 && c->pats[p]; p++) ok = has_bytes(fn, c->window, c->pats[p]);
        if (ok && c->slot == 2 && i == 0) ok = api->function_start(fn) == fn;
        if (!ok) {
            api->log("log_filter", "anchor check failed: %s (vtable %p, fn %p) - not hooking, game version unsupported",
                     c->what, (void *)vt, (void *)fn);
            return 1;
        }
        targets[i] = fn;
    }
    if (lf_config_init(&g_cfg)) { api->log("log_filter", "out of memory"); return 2; }
    char path[MAX_PATH], err[160];
    snprintf(path, sizeof path, "%slog_filter.ini", api->v3x_dir());
    snprintf(g_stats_path, sizeof g_stats_path, "%slog_filter_stats.txt", api->v3x_dir());
    char *text = read_file(path);
    int bad = lf_config_parse(&g_cfg, text ? text : LF_DEFAULT_INI, err, sizeof err);
    if (bad) api->log("log_filter", "%s: %d bad line(s), first: %s (ignored)", text ? path : "built-in config", bad, err);
    api->log("log_filter", "config: %s, %d rules, dedupe_first=%d, summary_interval=%d, enabled=%d",
             text ? path : "built-in defaults", g_cfg.nrules, g_cfg.dedupe_first, g_cfg.summary_interval, g_cfg.enabled);
    free(text);
    int rc = api->hook(targets[0], (void *)hk_sink_it, (void **)&orig_sink_it);
    api->log("log_filter", "spdlog::logger::sink_it_ at exe+0x%llx, hook %s",
             (unsigned long long)((char *)targets[0] - (char *)api->image_base()), rc == 0 ? "installed" : "FAILED");
    if (rc) return rc;
    g_last_tick = GetTickCount64();
    g_ready = 1;
    api->on_frame(on_frame, 0);
    if (api->api_version >= 3 && api->struct_size >= offsetof(V3X_API, on_event) + sizeof(void *))
    {
        api->on_event("load_transition", lf_on_event, 0);
        api->on_event("process_exit", lf_on_event, (void *)1);
    }
    return 0;
}
