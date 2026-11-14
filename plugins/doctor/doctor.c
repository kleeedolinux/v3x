/* doctor: in-game health check of V3X (docs/doctor.md).
 *
 *   doctor              run every check, print OK / WARN / PROBLEM with one "what to do" line each
 *   doctor fix <id>     apply the safe fix of a check (ini edits only, atomic, comments kept, the first
 *                       original kept as <file>.doctor-bak); `doctor fix D007` = every D007.* fix.
 *                       Changes apply at the next game start.
 *   doctor report       write binaries/v3x/doctor_report.txt for bug reports (user name masked)
 *
 * Automatic run: once, on the first load_transition Empty->Game (fast_load emits it), or 60 s after the
 * first frame when fast_load is not loaded. The checks run on a background thread; the main thread only
 * starts it and, via run_on_main, logs one line: "doctor: 0 problems, 2 warnings (type doctor)".
 *
 * The checks themselves are in dr_core.c (pure, tested offline by test/doctor_test.c); this file only
 * collects the inputs. Manifest "*": the doctor must run on unknown game builds to explain the gate. */
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2   /* GetProcessMemoryInfo / GetPerformanceInfo = the K32* exports of kernel32 */
#endif
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "dr_core.h"
#include "v3x.h"
#include "v3x_util.h"
#include "v3x_safe.h"
#include <psapi.h>
#include <tlhelp32.h>

#define TAG "doctor"

V3X_EXPORT const char *v3x_plugin_name = "doctor";
V3X_PLUGIN_MANIFEST("doctor", "0.5.0", "v3x", "health check: doctor | doctor fix <id> | doctor report", "*");

static const V3X_API *api;

/* L10 (the security review): log lines carry mod / file / engine text; control characters,
 * C1 controls and bidi overrides are replaced (v3x_sanitize_line) so a crafted name cannot forge
 * v3x.log lines (parsed by doctor and the installer). */
static void slog(const char *fmt, ...) V3X_PRINTF(1, 2);
static void slog(const char *fmt, ...)
{
    if (!api) return;
    char raw[1536], clean[1536];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(raw, sizeof raw, fmt, ap);
    va_end(ap);
    v3x_sanitize_line(clean, sizeof clean, raw);
    api->log(TAG, "%s", clean);
}
static CRITICAL_SECTION g_lock;          /* one run at a time (auto run vs console command) */
static volatile LONG g_auto_started;
static double g_first_frame_ms;

static const char *const g_ini_files[] = {"v3x.ini", "alloc_tracker.ini", "devtools.ini", "lazy_gfx.ini", "fast_lookup.ini",
                                          "par_load.ini", "map_cache.ini", "mod_loader.ini", "tick_par.ini", "ai_par.ini",
                                          "log_filter.ini", NULL};

/* ------------------------------------------------------------------ input collection */

typedef struct gathered {
    dr_inputs in;
    char *log, *plugins_out, *crash, *crash_prev, *json;
    char *ini[DR_MAX_INIS];
    char stems[DR_MAX_PLUGINS][64];
    const char *stem_ptr[DR_MAX_PLUGINS];
    char *mods[512];
    char loader_path[MAX_PATH], exe_dir[MAX_PATH], packs[MAX_PATH], user[128], profile[MAX_PATH];
    char game_version[96], build_id[64], loader_version[32];
} gathered;

static char *read_file(const char *path, size_t max)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    char *buf = NULL;
    if (GetFileSizeEx(h, &sz)) {
        size_t n = (size_t)sz.QuadPart;
        if (n > max) n = max;   /* load records and crash headers are at the start */
        buf = (char *)malloc(n + 1);
        DWORD got = 0, total = 0;
        while (buf && total < n && ReadFile(h, buf + total, (DWORD)(n - total), &got, NULL) && got) total += got;
        if (buf) buf[total] = 0;
    }
    CloseHandle(h);
    return buf;
}

typedef struct capbuf { char *p; size_t n, cap; } capbuf;
static void cap_sink(const char *text, void *user)
{
    capbuf *b = (capbuf *)user;
    size_t k = strlen(text);
    if (b->n + k + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 8192;
        while (cap < b->n + k + 1) cap *= 2;
        char *q = (char *)realloc(b->p, cap);
        if (!q) return;
        b->p = q; b->cap = cap;
    }
    memcpy(b->p + b->n, text, k + 1);
    b->n += k;
}

static void gather(gathered *g)
{
    char path[MAX_PATH * 2];
    dr_inputs *in = &g->in;
    const char *v3x = api->v3x_dir();
    memset(g, 0, sizeof *g);
    in = &g->in;

    snprintf(g->game_version, sizeof g->game_version, "%s", api->game_version ? api->game_version() : "?");
    snprintf(g->build_id, sizeof g->build_id, "%s", V3X_HAS(api, game_build_id) ? api->game_build_id() : "?");
    snprintf(g->loader_version, sizeof g->loader_version, "%s", V3X_HAS(api, loader_version) ? api->loader_version() : "<0.5.0");
    in->game_version = g->game_version;
    in->build_id = g->build_id;
    in->loader_version = g->loader_version;
    in->api_version = (int)api->api_version;

    HMODULE loader = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(void *)api->log, &loader) && loader) {
        GetModuleFileNameA(loader, g->loader_path, sizeof g->loader_path);
        in->loader_path = g->loader_path;
        in->known_builds = (const char *)(void *)GetProcAddress(loader, "v3x_loader_known_builds");
    }
    snprintf(g->exe_dir, sizeof g->exe_dir, "%s", api->game_dir());
    in->exe_dir = g->exe_dir;
    in->pid = GetCurrentProcessId();

    /* this session's v3x.log (the per-process fallback name first) */
    snprintf(path, sizeof path, "%sv3x.%lu.log", v3x, in->pid);
    g->log = read_file(path, 64u << 20);
    if (!g->log) { snprintf(path, sizeof path, "%sv3x.log", v3x); g->log = read_file(path, 64u << 20); }
    in->v3x_log = g->log;
    if ((!g->log || !strstr(g->log, " plugins loaded")) && V3X_HAS(api, exec_capture)) {
        capbuf b = {0};
        api->exec_capture("plugins", cap_sink, &b);   /* fallback (log level above info): prints the list */
        g->plugins_out = b.p;
        in->plugins_out = b.p;
    }

    /* plugin files */
    WIN32_FIND_DATAA fd;
    snprintf(path, sizeof path, "%splugins\\*.dll", v3x);
    HANDLE h = FindFirstFileA(path, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (in->nplugin_files >= DR_MAX_PLUGINS) break;
            char *s = g->stems[in->nplugin_files];
            snprintf(s, 64, "%s", fd.cFileName);
            char *dot = strrchr(s, '.');
            if (dot) *dot = 0;
            g->stem_ptr[in->nplugin_files++] = s;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    in->plugin_files = g->stem_ptr;

    for (int i = 0; g_ini_files[i] && i < DR_MAX_INIS; i++) {
        snprintf(path, sizeof path, "%s%s", v3x, g_ini_files[i]);
        g->ini[i] = read_file(path, 1u << 20);
        in->inis[i].name = g_ini_files[i];
        in->inis[i].text = g->ini[i];
        in->ninis = i + 1;
    }
    char env[64];
    in->env_profile = GetEnvironmentVariableA("V3X_PROFILE", env, sizeof env) && env[0] == '1';
    in->env_no_tickprof = GetEnvironmentVariableA("V3X_NO_TICKPROF", env, sizeof env) && env[0] == '1';
    static char ub[32];
    DWORD k = GetEnvironmentVariableA("V3X_UNKNOWN_BUILD", ub, sizeof ub);
    in->env_unknown_build = k && k < sizeof ub ? ub : NULL;

    /* memory */
    PROCESS_MEMORY_COUNTERS_EX pm;
    PERFORMANCE_INFORMATION pi;
    memset(&pm, 0, sizeof pm);
    pm.cb = sizeof pm;
    pi.cb = sizeof pi;
    if (GetPerformanceInfo(&pi, sizeof pi)) {
        uint64_t pg = pi.PageSize;
        in->have_mem = 1;
        in->commit_total = (uint64_t)pi.CommitTotal * pg;
        in->commit_limit = (uint64_t)pi.CommitLimit * pg;
        in->phys_total = (uint64_t)pi.PhysicalTotal * pg;
        in->phys_avail = (uint64_t)pi.PhysicalAvailable * pg;
    }
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm)) in->private_bytes = pm.PrivateUsage;

    /* disk */
    ULARGE_INTEGER fr, tot;
    snprintf(in->game_drive, sizeof in->game_drive, "%.2s", g->exe_dir);
    if (GetDiskFreeSpaceExA(g->exe_dir, &fr, &tot, NULL)) { in->have_game_disk = 1; in->game_free = fr.QuadPart; in->game_total = tot.QuadPart; }
    if (GetDiskFreeSpaceExA("C:\\", &fr, &tot, NULL)) { in->have_c_disk = 1; in->c_free = fr.QuadPart; in->c_total = tot.QuadPart; }

    /* mods: the launcher's content_load.json (same places as mod_menu) */
    const char *roots[2] = {"USERPROFILE", "OneDrive"};
    for (int r = 0; r < 2 && !g->json; r++) {
        char base[MAX_PATH];
        DWORD n = GetEnvironmentVariableA(roots[r], base, sizeof base);
        if (!n || n >= sizeof base) continue;
        snprintf(path, sizeof path, "%s\\Documents\\Paradox Interactive\\Victoria 3\\content_load.json", base);
        g->json = read_file(path, 4u << 20);
    }
    in->content_load_json = g->json;
    snprintf(g->packs, sizeof g->packs, "%scontent_packs", v3x);
    in->content_packs_dir = g->packs;

    /* modules of this process */
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32 me;
        me.dwSize = sizeof me;
        int n = 0;
        if (Module32First(snap, &me)) {
            do {
                if (n < 512 && (g->mods[n] = _strdup(me.szExePath))) n++;
            } while (Module32Next(snap, &me));
        }
        CloseHandle(snap);
        in->modules = (const char *const *)g->mods;
        in->nmodules = n;
    }

    snprintf(path, sizeof path, "%scrash.log", v3x);
    g->crash = read_file(path, 8u << 20);
    snprintf(path, sizeof path, "%scrash.prev.log", v3x);
    g->crash_prev = read_file(path, 8u << 20);
    in->crash_log = g->crash;
    in->crash_prev = g->crash_prev;

    DWORD un = GetEnvironmentVariableA("USERNAME", g->user, sizeof g->user);
    if (!un || un >= sizeof g->user) g->user[0] = 0;
    un = GetEnvironmentVariableA("USERPROFILE", g->profile, sizeof g->profile);
    if (!un || un >= sizeof g->profile) g->profile[0] = 0;
}

static void gather_free(gathered *g)
{
    free(g->log); free(g->plugins_out); free(g->crash); free(g->crash_prev); free(g->json);
    for (int i = 0; i < DR_MAX_INIS; i++) free(g->ini[i]);
    for (int i = 0; i < 512; i++) free(g->mods[i]);
}

typedef struct run { gathered g; dr_results res; dr_plugins ps; } run;

static run *run_checks(void)
{
    run *r = (run *)calloc(1, sizeof *r);
    if (!r) return NULL;
    EnterCriticalSection(&g_lock);
    gather(&r->g);
    dr_run(&r->g.in, &r->res, &r->ps);
    LeaveCriticalSection(&g_lock);
    return r;
}
static void run_free(run *r)
{
    if (!r) return;
    gather_free(&r->g);
    free(r);
}

/* ------------------------------------------------------------------ report */

static const char *state_word(int s)
{
    static const char *w[] = {"no load record", "loaded", "off (v3x.ini)", "skipped: unknown build", "refused: loader too old",
                              "refused: game build", "did not start", "load error", "faulted while loading", "skipped: crashed last start"};
    return s >= 0 && s <= DR_PS_CRASH_SKIP ? w[s] : "?";
}

static V3X_PRINTF(2, 3) void sb_add(capbuf *b, const char *fmt, ...)
{
    char line[2600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    cap_sink(line, b);
}

static int write_report(run *r, char *msg, size_t n)
{
    capbuf b = {0};
    SYSTEMTIME t;
    GetLocalTime(&t);
    char sum[128], line[2400];
    dr_summary_line(&r->res, sum, sizeof sum);
    sb_add(&b, "V3X doctor report %04u-%02u-%02u %02u:%02u\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    sb_add(&b, "loader %s (api v%d), game %s, build %s, known builds %s\r\n", r->g.loader_version, r->g.in.api_version,
           r->g.game_version, r->g.build_id, r->g.in.known_builds ? r->g.in.known_builds : "?");
    sb_add(&b, "game folder %s\r\n%s\r\n\r\n", r->g.exe_dir, sum);
    sb_add(&b, "== checks (machine lines: doctor: <ID> <name> <SEV> <detail>)\r\n");
    for (int i = 0; i < r->res.n; i++) {
        const dr_result *x = &r->res.r[i];
        sb_add(&b, "doctor: %s %s %s %s\r\n", x->id, x->name, x->sev == DR_PROBLEM ? "PROBLEM" : x->sev == DR_WARN ? "WARN" : "OK", x->msg);
        if (x->todo[0]) sb_add(&b, "    what to do: %s\r\n", x->todo);
    }
    sb_add(&b, "\r\n== plugins\r\n");
    for (int i = 0; i < r->ps.n; i++) {
        const dr_plugin *p = &r->ps.p[i];
        sb_add(&b, "  %-20s %-10s %-26s%s%s%s\r\n", p->stem, p->version[0] ? p->version : "-", state_word(p->state),
               p->faulted ? " FAULTED" : "", p->why[0] && p->state != DR_PS_LOADED ? " - " : "",
               p->why[0] && p->state != DR_PS_LOADED ? p->why : "");
    }
    /* the warning/error lines of v3x.log (last 60) */
    sb_add(&b, "\r\n== v3x.log warnings and errors (last 60)\r\n");
    if (r->g.log) {
        const char *hits[60];
        int nh = 0;
        for (const char *p = r->g.log; p && *p;) {
            const char *e = strchr(p, '\n');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            if (len < sizeof line) {
                memcpy(line, p, len); line[len] = 0;
                if (strstr(line, "WARNING") || strstr(line, "ERROR") || strstr(line, "FAULT") || strstr(line, "FAILED")) {
                    if (nh == 60) { memmove(hits, hits + 1, sizeof hits[0] * 59); nh = 59; }
                    hits[nh++] = p;
                }
            }
            p = e ? e + 1 : p + len;
        }
        for (int i = 0; i < nh; i++) {
            size_t len = strcspn(hits[i], "\r\n");
            if (len >= sizeof line) len = sizeof line - 1;
            memcpy(line, hits[i], len); line[len] = 0;
            sb_add(&b, "  %s\r\n", line);
        }
    }
    char pidtag[48];
    snprintf(pidtag, sizeof pidtag, "pid %lu,", r->g.in.pid);
    const char *crash = r->g.crash && strstr(r->g.crash, pidtag) ? r->g.crash_prev : r->g.crash;   /* = check_last_crash */
    if (crash) {
        dr_crash c;
        dr_crash_parse(crash, r->g.in.plugin_files, r->g.in.nplugin_files, &c);
        sb_add(&b, "\r\n== last session crash\r\n  %s\r\n  module %s, V3X plugin faulting: %s, on stack: %s\r\n  %s\r\n",
               c.found ? c.line : "(none recorded)", c.module[0] ? c.module : "-", c.plugin_fault[0] ? c.plugin_fault : "-",
               c.plugin_stack[0] ? c.plugin_stack : "-", c.memory);
    }
    if (!b.p) { snprintf(msg, n, "doctor report: out of memory"); return -1; }
    char *masked = dr_mask(b.p, r->g.user, r->g.profile);
    free(b.p);
    if (!masked) { snprintf(msg, n, "doctor report: out of memory"); return -1; }
    char path[MAX_PATH];
    v3x_path(api, path, sizeof path, "doctor_report.txt");
    int rc = v3x_file_write_atomic(path, masked, strlen(masked));
    free(masked);
    if (rc) snprintf(msg, n, "doctor report: could not write %s (error %lu)", path, GetLastError());
    else snprintf(msg, n, "doctor report written: %s (your Windows user name is masked; attach it to a bug report)", path);
    return rc;
}

/* ------------------------------------------------------------------ console */

static void print_results(const run *r)
{
    char line[1200], sum[128];
    api->print("V3X doctor - loader %s, game %s (build %s)", r->g.loader_version, r->g.game_version, r->g.build_id);
    for (int i = 0; i < r->res.n; i++) {
        dr_format_result(&r->res.r[i], line, sizeof line);
        api->print("%s", line);
    }
    dr_summary_line(&r->res, sum, sizeof sum);
    int p, w;
    dr_count(&r->res, &p, &w);
    api->print("%.*s%s", (int)(strstr(sum, " (type") ? strstr(sum, " (type") - sum : (ptrdiff_t)strlen(sum)), sum,
               p || w ? ". Safe fixes: doctor fix <ID>; for a bug report: doctor report" : "");
}

static void do_fix(const char *id)
{
    run *r = run_checks();
    if (!r) { api->print("doctor: out of memory"); return; }
    int matched = 0, fixed = 0;
    char msg[700];
    for (int i = 0; i < r->res.n; i++) {
        const dr_result *x = &r->res.r[i];
        if (!dr_id_matches(x->id, id)) continue;
        matched++;
        if (!x->has_fix) {
            if (x->sev) api->print("%s: no safe automatic fix. What to do: %s", x->id, x->todo);
            continue;
        }
        EnterCriticalSection(&g_lock);
        int rc = dr_apply_fix(api->v3x_dir(), &x->fix, msg, sizeof msg);
        LeaveCriticalSection(&g_lock);
        api->print("%s: %s", x->id, msg);
        slog("fix %s: %s", x->id, msg);
        if (rc > 0) fixed++;
    }
    if (!matched) {
        char list[600] = "";
        for (int i = 0; i < r->res.n; i++)
            if (r->res.r[i].has_fix) {
                size_t l = strlen(list);
                snprintf(list + l, sizeof list - l, " %s", r->res.r[i].id);
            }
        api->print("doctor fix: no current check matches '%s'. Fixes available now:%s", id, list[0] ? list : " none");
    } else if (fixed) {
        api->print("doctor: %d change(s) written; they take effect at the next game start", fixed);
    }
    run_free(r);
}

static void cmd_doctor(const char *args, void *user)
{
    (void)user;
    while (*args == ' ') args++;
    if (!_strnicmp(args, "fix", 3) && (args[3] == ' ' || !args[3])) {
        const char *id = args + 3;
        while (*id == ' ') id++;
        if (!*id) { api->print("usage: doctor fix <ID>   (IDs are shown by 'doctor', e.g. doctor fix D007.alloc_tracker)"); return; }
        do_fix(id);
        return;
    }
    if (!_stricmp(args, "report")) {
        run *r = run_checks();
        char msg[MAX_PATH + 200];
        if (!r) { api->print("doctor: out of memory"); return; }
        write_report(r, msg, sizeof msg);
        api->print("%s", msg);
        run_free(r);
        return;
    }
    if (*args) { api->print("usage: doctor | doctor fix <ID> | doctor report"); return; }
    run *r = run_checks();
    if (!r) { api->print("doctor: out of memory"); return; }
    print_results(r);
    run_free(r);
}

/* ------------------------------------------------------------------ automatic run */

static void log_summary_main(void *u)   /* main thread: one log line, nothing else */
{
    char *s = (char *)u;
    slog("%s", s);
    free(s);
}

static DWORD WINAPI auto_thread(void *u)
{
    (void)u;
    run *r = run_checks();
    if (!r) return 0;
    char sum[128];
    dr_summary_line(&r->res, sum, sizeof sum);
    size_t cap = sizeof sum + 400;
    char *s = (char *)malloc(cap);
    if (s) {
        V3xStr sb; v3x_str_init(&sb, s, cap);
        v3x_str_append(&sb, sum);
        int first = 1;
        for (int i = 0; i < r->res.n; i++)   /* name the problems so they are visible in v3x.log */
            if (r->res.r[i].sev == DR_PROBLEM) {
                v3x_str_appendf(&sb, "%s%s", first ? ": " : ", ", r->res.r[i].id);
                first = 0;
            }
        if (!V3X_HAS(api, run_on_main) || api->run_on_main(log_summary_main, s)) log_summary_main(s);
    }
    run_free(r);
    return 0;
}

static void start_auto(void)
{
    if (InterlockedCompareExchange(&g_auto_started, 1, 0)) return;
    HANDLE t = CreateThread(NULL, 256 * 1024, auto_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

static void on_transition(const char *name, void *data, void *user)
{
    (void)name; (void)user;
    const V3X_LoadTransition *t = (const V3X_LoadTransition *)data;
    if (t && t->from && t->to && !_stricmp(t->from, "Empty") && !_stricmp(t->to, "Game")) start_auto();
}

static void on_frame(void *user)
{
    (void)user;
    if (g_auto_started) return;
    double now = v3x_now_ms();
    if (!g_first_frame_ms) { g_first_frame_ms = now; return; }
    if (now - g_first_frame_ms > 60000.0 && !GetModuleHandleA("fast_load.dll")) start_auto();
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    InitializeCriticalSection(&g_lock);
    if (api->api_version >= 2 && api->register_command)
        api->register_command("doctor", "health check: doctor | doctor fix <ID> | doctor report", cmd_doctor, NULL);
    if (api->api_version >= 3 && api->on_event) api->on_event(V3X_EVENT_LOAD_TRANSITION, on_transition, NULL);
    api->on_frame(on_frame, NULL);
    slog("ready: type 'doctor' in the V3X console (auto check at the first game load)");
    return 0;
}
