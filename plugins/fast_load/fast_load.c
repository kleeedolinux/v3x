/* fast_load: load-time measurement for Victoria 3 (step 1 of docs/re/loading.md section 3).
 *
 * Timing hooks (no behaviour change; each one is found by a log-string anchor and byte-checked,
 * any mismatch = that hook is not installed, fail closed):
 *   transition  "Transition {} took: {} seconds"      pdx_transition_task finalizer (RVA 03deb0b0 in 1.15.1)
 *   db_load     "LoadNodesParallelFor"                 database dependency graph load (03dc72e0)
 *   db_report   "Database Node Init Time: ..."        per-database times, read from the engine's own
 *                                                      node records before it logs them (03dc7640)
 *   mesh        "Total time \"{}\" in {} ms"            mesh type loader, IterateTypesThreaded (03ae4d80)
 *   mesh_item   per-item vcall thunk used by that parallel-for (02322020), entry-only hook that
 *               records which threads run mesh items (only while the mesh loader is active)
 *   cs_start    "Starting checksum calculation"        (0377b120)
 *   cs_feed     "Feeding into checksum: {}"            game checksum builder (0377b5f0)
 * The same table is mirrored in tools/re_load_anchors.py (offline check on the real exe).
 *
 * Output: <game>/binaries/v3x/load_timeline.txt (rewritten after every phase) and one summary
 * line in v3x.log per transition. Config: <game>/binaries/v3x/fast_load.ini (fl_core.h).
 *
 * mesh_threads=N (2..15, default 0 = off) is the only behaviour change: it sets the engine's
 * mesh thread-count setting ([[05c726e8]+0x648] in 1.15.1, clamped by the engine to 2..15) while
 * the mesh loader runs, then restores it.
 *
 * checksum_cache=1 is accepted but fails closed: the engine streams file contents into a
 * running MD5 per manifest entry (pdx_core/checksum.cpp, 03dbe9d0), so a per-file hash cache
 * cannot reproduce the same digest. See docs/re/loading.md. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <psapi.h>
#include "v3x.h"
#include "fl_core.h"

V3X_EXPORT const char *v3x_plugin_name = "fast_load";
V3X_PLUGIN_MANIFEST("fast_load", "0.5.0", "v3x", "load-time measurement and load-state events", "release/1.15.1");

typedef uintptr_t (*fn4)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);

static const V3X_API *api;
static SRWLOCK g_lock = SRWLOCK_INIT;
static fl_config g_cfg;
static fl_timeline g_tl;
static fl_thrset g_thr;
static LARGE_INTEGER g_freq, g_qpc0;
static double g_start_ms;          /* ms between process creation and plugin load */
static char g_out_path[MAX_PATH];

/* ------------------------------------------------------------------ anchors (keep in sync with tools/re_load_anchors.py) */

typedef struct { int off; const char *pat; } shape_t;
typedef struct { const char *name; const char *str; shape_t shape[4]; uint8_t *fn; } anchor_t;

enum { A_TRANSITION, A_DB_REPORT, A_DB_LOAD, A_MESH, A_CS_START, A_CS_FEED, A_COUNT };
static anchor_t ANCHORS[A_COUNT] = {
    { "transition", "Transition {} took: {} seconds",
      { { 0x00, "48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 55" },
        { 0x23, "48 8B 01 48 8B 90 50 01 00 00" },
        { 0xFB, "48 8D 87 68 01 00 00" },          /* lea rax,[task+0x168]: name "From->To" */
        { 0xDD, "48 8B 87 50 01 00 00 C5 FA 11 90 28 01 00 00" } }, NULL },  /* [[task+0x150]+0x128] = seconds (float) */
    { "db_report", "Database Node Init Time: {} - {} ms - {} ms including dependencies",
      { { 0x29, "48 8B 41 08 48 8B 58 70 48 63 40 7C" },
        { 0x148, "4D 8B 8D A0 00 00 00" },
        { 0x152, "49 8B 85 C0 00 00 00 49 8D 8D B0 00 00 00" } }, NULL },
    { "db_load", "LoadNodesParallelFor",
      { { 0x00, "48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57" } }, NULL },
    { "mesh", "Total time \"{}\" in {} ms",
      { { 0x41, "48 8B 05 ?? ?? ?? ?? 44 8B A0 48 06 00 00" } }, NULL },
    { "cs_start", "Starting checksum calculation",
      { { 0x00, "48 89 5C 24 08 48 89 74 24 20 55 57 41 56" } }, NULL },
    { "cs_feed", "Feeding into checksum: {}",
      { { 0x00, "48 89 5C 24 08 44 88 44 24 18 48 89 54 24 10" } }, NULL },
};
#define THUNK_STR      "IterateTypesThreaded"
#define THUNK_LEA_BACK 0x24
#define THUNK_LEA      "48 8D 05 ?? ?? ?? ?? 48 89 45 F0"
#define THUNK_BODY     "48 8B 01 48 FF 60 30"
#define SHAPE_SPAN     0x400

static int in_image(const void *p, size_t n)
{
    uintptr_t b = api->image_base(), a = (uintptr_t)p;
    return a >= b && a + n <= b + api->image_size();
}

static int shape_ok(const uint8_t *fn, const shape_t *sh)
{
    if (!in_image(fn, SHAPE_SPAN)) return 0;
    for (int i = 0; i < 4 && sh[i].pat; i++)
        if (!fl_match(fn, SHAPE_SPAN, (size_t)sh[i].off, sh[i].pat)) return 0;
    return 1;
}

static uint8_t *resolve(anchor_t *a)
{
    void *s = api->find_string(a->str);
    if (!s) { api->log("fast_load", "FAIL-CLOSED %s: anchor string not found", a->name); return NULL; }
    for (int i = 0; i < 16; i++) {
        void *x = api->find_xref(s, i);
        if (!x) break;
        uint8_t *f = api->function_start(x);
        if (f && shape_ok(f, a->shape)) return a->fn = f;
    }
    api->log("fast_load", "FAIL-CLOSED %s: no xref function matches the expected shape", a->name);
    return NULL;
}

/* ------------------------------------------------------------------ helpers */

static int readable(const void *p, size_t n)
{
    uintptr_t a = (uintptr_t)p, end = a + n;
    if (!p || end < a) return 0;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)a, &mi, sizeof mi) || mi.State != MEM_COMMIT) return 0;
        if (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
        if (!(mi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return 0;
        a = (uintptr_t)mi.BaseAddress + mi.RegionSize;
    }
    return 1;
}

static int writable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mi;
    if (!p || !VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & PAGE_GUARD)) return 0;
    if (!(mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) return 0;
    return (uintptr_t)p + n <= (uintptr_t)mi.BaseAddress + mi.RegionSize;
}

static int rd64(const void *p, uint64_t *v) { if (!readable(p, 8)) return 0; memcpy(v, p, 8); return 1; }

static double now_ms(void)
{
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    return g_start_ms + (double)(q.QuadPart - g_qpc0.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
}

static uint32_t rva(const void *p) { return (uint32_t)((uintptr_t)p - api->image_base()); }

#define EVENT(t, ...) do { AcquireSRWLockExclusive(&g_lock); fl_tl_add(&g_tl, (t), GetCurrentThreadId(), __VA_ARGS__); ReleaseSRWLockExclusive(&g_lock); } while (0)

static void write_timeline(void)
{
    size_t cap = (size_t)g_tl.cap * 180 + 4096;
    char *buf = malloc(cap);
    if (!buf) return;
    char hdr[256];
    snprintf(hdr, sizeof hdr, "# V3X fast_load timeline (%s). Times: ms since process creation.\n"
             "# Plugin loaded at %+.1f ms.", api->game_version(), g_start_ms);
    AcquireSRWLockExclusive(&g_lock);
    size_t n = fl_tl_format(&g_tl, hdr, buf, cap);
    ReleaseSRWLockExclusive(&g_lock);
    HANDLE h = CreateFileA(g_out_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, buf, (DWORD)n, &w, NULL); CloseHandle(h); }
    free(buf);
}

/* ------------------------------------------------------------------ phase totals for the summary line */

static volatile LONG g_db_graphs, g_db_nodes, g_cs_calls;
static volatile LONG g_counters_on = 1;               /* 1 from plugin load to the end of the first transition, then during each transition */
static volatile LONG64 g_ws_empty, g_ws_pause, g_ws_yield, g_ws_sleep;   /* worker_spin stats */
static volatile LONG64 g_pump_iters, g_pump_ticks;    /* loading pump step: calls, QPC ticks inside */
static double g_db_ms, g_mesh_ms, g_cs_ms, g_mesh_wall_last;
static int g_mesh_setting = -1;
static volatile LONG g_mesh_active;

/* ------------------------------------------------------------------ hooks */

static fn4 o_transition, o_db_report, o_db_load, o_mesh, o_cs_start, o_cs_feed;

static double g_last_transition_ms;

/* "load_transition" event (API v3): name "From->To" split into from/to; NULL if not "->"-shaped. */
static void emit_named(const char *event, const char *name, double secs);
static void emit_transition(const char *name, double secs) { emit_named("load_transition", name, secs); }
static void emit_named(const char *event, const char *name, double secs)
{
    if (api->api_version < 3 || api->struct_size < offsetof(V3X_API, emit_event) + sizeof(void *) || !api->emit_event) return;
    char from[96] = {0}, to[96] = {0};
    const char *arrow = strstr(name, "->");
    V3X_LoadTransition info = { NULL, NULL, secs };
    if (arrow && arrow - name < (ptrdiff_t)sizeof from) {
        memcpy(from, name, (size_t)(arrow - name));
        snprintf(to, sizeof to, "%s", arrow + 2);
        info.from = from; info.to = to;
    }
    api->emit_event(event, &info);
}

static uintptr_t h_transition(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    double t0 = now_ms();
    {   /* transition BEGIN: this finalizer runs the whole transition (it took 163 s for Empty->Game) */
        char bname[96] = "?"; uint64_t btask;
        if (rd64((void *)a, &btask)) fl_msvc_str((void *)(uintptr_t)(btask + 0x168), bname, sizeof bname, readable);
        InterlockedExchange(&g_counters_on, 1);
        EVENT(t0, "TRANSITION %s BEGIN", bname);
        write_timeline();
        emit_named("load_transition_begin", bname, 0.0);
    }
    uintptr_t r = o_transition(a, b, c, d);
    double t1 = now_ms();
    char name[96] = "?"; uint64_t task = 0, st;
    float esec = -1;
    if (rd64((void *)a, &task)) {
        fl_msvc_str((void *)(uintptr_t)(task + 0x168), name, sizeof name, readable);
        if (rd64((void *)(uintptr_t)(task + 0x150), &st) && readable((void *)(uintptr_t)(st + 0x128), 4))
            memcpy(&esec, (void *)(uintptr_t)(st + 0x128), 4);
    }
    double secs = (esec >= 0 && esec < 1e6) ? esec : (t1 - g_last_transition_ms) / 1000.0;
    g_last_transition_ms = t1;
    char thr[160];
    AcquireSRWLockShared(&g_lock);
    fl_thr_summary(&g_thr, g_mesh_wall_last, thr, sizeof thr);
    ReleaseSRWLockShared(&g_lock);
    EVENT(t1, "TRANSITION %s took %.2f s (%s; finalizer %.0f ms)", name, secs, esec >= 0 ? "engine timer" : "fast_load estimate", t1 - t0);
    api->log("fast_load", "transition %s took %.1f s, at %+.1f s | db graphs %ld (%.1f s, %ld nodes) | mesh %.1f s (setting %d, %s) | checksum %.1f s",
             name, secs, t1 / 1000.0, g_db_graphs, g_db_ms / 1000.0, g_db_nodes, g_mesh_ms / 1000.0, g_mesh_setting, thr, g_cs_ms / 1000.0);
    if (g_pump_iters) {
        double in_ms = (double)g_pump_ticks * 1000.0 / (double)g_freq.QuadPart;
        EVENT(t1, "loading pump (main thread 03775600): %lld steps, %.0f ms inside the step, %.0f ms outside (spin/poll)%s",
              (long long)g_pump_iters, in_ms, secs * 1000.0 - in_ms,
              g_cfg.pump_yield == 1 ? ", pump_yield=1 SwitchToThread" : g_cfg.pump_yield == 2 ? ", pump_yield=2 Sleep(1)" : "");
        g_pump_iters = 0; g_pump_ticks = 0;
    }
    if (g_ws_empty) {
        EVENT(t1, "idle workers (worker_spin=%d): %lld empty polls, %lld pause, %lld SwitchToThread, %lld Sleep(1)",
              g_cfg.worker_spin, (long long)g_ws_empty, (long long)g_ws_pause, (long long)g_ws_yield, (long long)g_ws_sleep);
        g_ws_empty = g_ws_pause = g_ws_yield = g_ws_sleep = 0;
    }
    InterlockedExchange(&g_counters_on, 0);
    write_timeline();
    emit_transition(name, secs);
    return r;
}

static uintptr_t h_db_load(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    void *caller = __builtin_return_address(0);
    double t0 = now_ms();
    EVENT(t0, "db graph load START (caller %08x)", rva(caller));
    uintptr_t r = o_db_load(a, b, c, d);
    double t1 = now_ms();
    InterlockedIncrement(&g_db_graphs);
    AcquireSRWLockExclusive(&g_lock); g_db_ms += t1 - t0; ReleaseSRWLockExclusive(&g_lock);
    EVENT(t1, "db graph load END (caller %08x) %.0f ms", rva(caller), t1 - t0);
    write_timeline();
    return r;
}

/* ctx -> graph=[ctx+8]; nodes=[graph+0x70] (node*[]), count=[graph+0x7c] (int32).
 * node: +0x08 name {ptr,len}, +0xa0 own ms (int64), +0xb0 dep list sentinel, +0xc0 first dep,
 *       dep elem: +0x10 next, +0x28 dep node. incl = own + direct deps (as the engine prints). */
static uintptr_t h_db_report(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    double t = now_ms();
    uint64_t graph, nodes, cnt64;
    if (rd64((void *)(a + 8), &graph) && rd64((void *)(uintptr_t)(graph + 0x70), &nodes) &&
        rd64((void *)(uintptr_t)(graph + 0x78), &cnt64)) {
        int32_t n = (int32_t)(cnt64 >> 32);
        int64_t sum = 0;
        for (int32_t i = 0; i < n && i < 4096; i++) {
            uint64_t node, own = 0, it, sent;
            if (!rd64((void *)(uintptr_t)(nodes + 8u * (uint64_t)i), &node) || !rd64((void *)(uintptr_t)(node + 0xa0), &own)) break;
            int64_t incl = (int64_t)own;
            sent = node + 0xb0;
            if (rd64((void *)(uintptr_t)(node + 0xc0), &it)) {
                for (int g = 0; it != sent && g < 4096; g++) {
                    uint64_t dep, dms;
                    if (!rd64((void *)(uintptr_t)(it + 0x28), &dep) || !rd64((void *)(uintptr_t)(dep + 0xa0), &dms)) break;
                    incl += (int64_t)dms;
                    if (!rd64((void *)(uintptr_t)(it + 0x10), &it)) break;
                }
            }
            sum += (int64_t)own;
            if (g_cfg.db_nodes) {
                char name[96];
                if (fl_view_str((void *)(uintptr_t)(node + 8), name, sizeof name, readable) < 0) snprintf(name, sizeof name, "?");
                EVENT(t, "  db node %-40s own %6lld ms  incl deps %6lld ms", name, (long long)own, (long long)incl);
            }
        }
        InterlockedExchangeAdd(&g_db_nodes, n);
        EVENT(t, "db graph report: %d nodes, sum of own times %lld ms", n, (long long)sum);
    } else {
        EVENT(t, "db graph report: unreadable graph (layout changed?)");
    }
    return o_db_report(a, b, c, d);
}

__attribute__((used)) static void *g_item_orig;
__attribute__((used)) static void mesh_item_enter(void)
{
    if (!g_mesh_active) return;
    double t = now_ms();
    AcquireSRWLockExclusive(&g_lock);
    fl_thr_hit(&g_thr, GetCurrentThreadId(), t);
    ReleaseSRWLockExclusive(&g_lock);
}

/* Entry-only detour for `mov rax,[rcx]; jmp [rax+0x30]`: keeps every argument register and the
 * stack untouched (stack args pass through), then tail-jumps to the trampoline. */
__attribute__((naked)) static void mesh_item_stub(void)
{
    __asm__ volatile(
        "push %rcx\n\t" "push %rdx\n\t" "push %r8\n\t" "push %r9\n\t"
        "sub $0x68, %rsp\n\t"
        "movdqu %xmm0, 0x20(%rsp)\n\t" "movdqu %xmm1, 0x30(%rsp)\n\t"
        "movdqu %xmm2, 0x40(%rsp)\n\t" "movdqu %xmm3, 0x50(%rsp)\n\t"
        "call mesh_item_enter\n\t"
        "movdqu 0x20(%rsp), %xmm0\n\t" "movdqu 0x30(%rsp), %xmm1\n\t"
        "movdqu 0x40(%rsp), %xmm2\n\t" "movdqu 0x50(%rsp), %xmm3\n\t"
        "add $0x68, %rsp\n\t"
        "pop %r9\n\t" "pop %r8\n\t" "pop %rdx\n\t" "pop %rcx\n\t"
        "jmp *g_item_orig(%rip)\n\t");
}

static uintptr_t h_mesh(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    double t0 = now_ms();
    /* thread-count setting: mov rax,[rip+X] at fn+0x41 (checked); value = [[X]+0x648], clamp 2..15 */
    const uint8_t *ins = ANCHORS[A_MESH].fn + 0x41; int32_t disp; memcpy(&disp, ins + 3, 4);
    uint64_t g; uint32_t setting = 0xFFFFFFFF;
    if (rd64(ins + 7 + disp, &g) && g && readable((void *)(uintptr_t)(g + 0x648), 4)) memcpy(&setting, (void *)(uintptr_t)(g + 0x648), 4);
    g_mesh_setting = (int)setting;
    /* mesh_threads=N: the loader reads [[X]+0x648] twice (fn+0x41 and the parallel-for cap), both
     * inside this call, so set it here and restore afterwards (the setting may be used elsewhere). */
    int32_t *pset = (setting != 0xFFFFFFFF) ? (int32_t *)(uintptr_t)(g + 0x648) : NULL;
    int overridden = 0;
    if (g_cfg.mesh_threads && pset && writable(pset, 4)) {
        *pset = g_cfg.mesh_threads; overridden = 1;
        EVENT(t0, "mesh_threads=%d: engine setting %d overridden for the mesh loader", g_cfg.mesh_threads, (int)setting);
    } else if (g_cfg.mesh_threads) {
        EVENT(t0, "mesh_threads=%d: setting not writable/readable, left alone (fail closed)", g_cfg.mesh_threads);
    }
    AcquireSRWLockExclusive(&g_lock); memset(&g_thr, 0, sizeof g_thr); ReleaseSRWLockExclusive(&g_lock);
    EVENT(t0, "mesh type load START (thread setting %d -> clamped to [2,15])", (int)setting);
    InterlockedIncrement(&g_mesh_active);
    uintptr_t r = o_mesh(a, b, c, d);
    InterlockedDecrement(&g_mesh_active);
    if (overridden) *pset = (int32_t)setting;
    double t1 = now_ms();
    char thr[160];
    AcquireSRWLockExclusive(&g_lock);
    g_mesh_ms += t1 - t0; g_mesh_wall_last = t1 - t0;
    fl_thr_summary(&g_thr, t1 - t0, thr, sizeof thr);
    ReleaseSRWLockExclusive(&g_lock);
    EVENT(t1, "mesh type load END %.0f ms  %s", t1 - t0, g_item_orig ? thr : "(thread tracking off)");
    if (g_item_orig) {
        AcquireSRWLockShared(&g_lock);
        fl_thrset snap = g_thr;
        ReleaseSRWLockShared(&g_lock);
        for (int i = 0; i < snap.n; i++)
            EVENT(t1, "  mesh thread %u: %u items, first %+.0f ms, last %+.0f ms", snap.t[i].tid, snap.t[i].calls,
                  snap.t[i].first_ms - t0, snap.t[i].last_ms - t0);
    }
    write_timeline();
    return r;
}

static uintptr_t h_cs_start(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    double t0 = now_ms();
    EVENT(t0, "checksum calculation START");
    uintptr_t r = o_cs_start(a, b, c, d);
    double t1 = now_ms();
    EVENT(t1, "checksum calculation function returned after %.0f ms", t1 - t0);
    return r;
}

static uintptr_t h_cs_feed(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    double t0 = now_ms();
    uintptr_t r = o_cs_feed(a, b, c, d);
    double t1 = now_ms();
    InterlockedIncrement(&g_cs_calls);
    AcquireSRWLockExclusive(&g_lock); g_cs_ms += t1 - t0; ReleaseSRWLockExclusive(&g_lock);
    EVENT(t1, "checksum build (manifest files hashed) %.0f ms", t1 - t0);
    write_timeline();
    return r;
}

/* ------------------------------------------------------------------ load */

static void install(int id, void *detour, fn4 *orig)
{
    anchor_t *a = &ANCHORS[id];
    if (!resolve(a)) return;
    int s = api->hook(a->fn, detour, (void **)orig);
    api->log("fast_load", "%s %s at %08x (status %d)", s ? "FAIL" : "hooked", a->name, rva(a->fn), s);
    if (s) *orig = NULL;
}

static void install_item_thunk(void)
{
    uint8_t *fn = ANCHORS[A_MESH].fn;
    void *s = api->find_string(THUNK_STR);
    for (int i = 0; s && i < 64; i++) {
        uint8_t *x = api->find_xref(s, i);
        if (!x) break;
        if (api->function_start(x) != fn || x - THUNK_LEA_BACK < fn) continue;
        uint8_t *lea = x - THUNK_LEA_BACK;
        if (!fl_match(lea, 11, 0, THUNK_LEA)) continue;
        int32_t disp; memcpy(&disp, lea + 3, 4);
        uint8_t *t = lea + 7 + disp;
        if (!in_image(t, 8) || !fl_match(t, 8, 0, THUNK_BODY)) continue;
        int st = api->hook(t, (void *)mesh_item_stub, &g_item_orig);
        api->log("fast_load", "%s mesh_item at %08x (status %d)", st ? "FAIL" : "hooked", rva(t), st);
        if (st) g_item_orig = NULL;
        return;
    }
    api->log("fast_load", "FAIL-CLOSED mesh_item: worker thunk shape mismatch (thread tracking off)");
}

static void load_config(void)
{
    fl_config_defaults(&g_cfg);
    char path[MAX_PATH]; snprintf(path, sizeof path, "%sfast_load.ini", api->v3x_dir());
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char buf[4096]; size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
    char err[160];
    int bad = fl_config_parse(&g_cfg, buf, err, sizeof err);
    if (bad) api->log("fast_load", "fast_load.ini: %d bad line(s), first: %s", bad, err);
}

/* ------------------------------------------------------------------ 1 Hz counters (counters=1)
 * Why: the in-game profile shows the load is mostly WAITING (transition thread ~36 s CPU in a 163 s
 * transition; main thread and idle workers spin). CPU sampling cannot see waits, so log what the
 * process and the system do each second: CPU, file I/O, hard page-ins, free RAM. */

typedef LONG (NTAPI *ntqsi_fn)(ULONG, PVOID, ULONG, PULONG);
#define SPI_IO_READ   0x08     /* SYSTEM_PERFORMANCE_INFORMATION.IoReadTransferCount (LARGE_INTEGER) */
#define SPI_AVAIL     0x2C     /* AvailablePages (ULONG), used to validate the layout */
#define SPI_PAGEREAD  0x50     /* PageReadCount: pages read by hard faults */
#define SPI_PAGEREADS 0x54     /* PageReadIoCount: page-in I/O operations */

typedef struct { uint64_t pcpu, sbusy, stot, pio, sio; uint32_t pread, preadio; int sys_ok; } ctr_t;

static uint64_t ft64(FILETIME f) { return (uint64_t)f.dwHighDateTime << 32 | f.dwLowDateTime; }

static void ctr_read(ctr_t *c, ntqsi_fn q, int *spi_ok)
{
    memset(c, 0, sizeof *c);
    FILETIME a, b, k, u;
    if (GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u)) c->pcpu = ft64(k) + ft64(u);
    if (GetSystemTimes(&a, &k, &u)) { c->stot = ft64(k) + ft64(u); c->sbusy = c->stot - ft64(a); }
    IO_COUNTERS io; if (GetProcessIoCounters(GetCurrentProcess(), &io)) c->pio = io.ReadTransferCount;
    if (!q || !*spi_ok) return;
    static uint8_t spi[0x400]; ULONG ret = 0;
    if (q(2 /* SystemPerformanceInformation */, spi, sizeof spi, &ret) != 0 || ret < SPI_PAGEREADS + 4) { *spi_ok = 0; return; }
    if (*spi_ok == 2) {                                   /* first read: validate the layout */
        uint32_t avail; memcpy(&avail, spi + SPI_AVAIL, 4);
        MEMORYSTATUSEX ms = { .dwLength = sizeof ms };
        double a1 = (double)avail * 4096.0, a2 = GlobalMemoryStatusEx(&ms) ? (double)ms.ullAvailPhys : 0;
        *spi_ok = (a2 > 0 && a1 > a2 * 0.75 && a1 < a2 * 1.33) ? 1 : 0;
        if (!*spi_ok) { EVENT(now_ms(), "counters: system performance layout mismatch, system read/page-in columns off (fail closed)"); return; }
    }
    memcpy(&c->sio, spi + SPI_IO_READ, 8);
    memcpy(&c->pread, spi + SPI_PAGEREAD, 4);
    memcpy(&c->preadio, spi + SPI_PAGEREADS, 4);
    c->sys_ok = 1;
}

static DWORD WINAPI counters_thread(void *unused)
{
    (void)unused;
    ntqsi_fn q = (ntqsi_fn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySystemInformation");
    int spi_ok = 2;
    ctr_t p, c; ctr_read(&p, q, &spi_ok);
    double tp = now_ms();
    int was_on = 1;
    for (;;) {
        Sleep(1000);
        if (!g_counters_on) { was_on = 0; continue; }
        if (!was_on) { ctr_read(&p, q, &spi_ok); tp = now_ms(); was_on = 1; continue; }   /* new window: rebase */
        ctr_read(&c, q, &spi_ok);
        double t = now_ms(), dt = (t - tp) / 1000.0;
        if (dt <= 0) continue;
        PROCESS_MEMORY_COUNTERS_EX pm = { .cb = sizeof pm };
        K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm);
        MEMORYSTATUSEX ms = { .dwLength = sizeof ms }; GlobalMemoryStatusEx(&ms);
        double pcpu = (double)(c.pcpu - p.pcpu) / 1e4 / (t - tp) * 100.0;          /* % of one core */
        double sbusy = c.stot > p.stot ? (double)(c.sbusy - p.sbusy) * 100.0 / (double)(c.stot - p.stot) : 0;
        char sys[112] = "";
        if (c.sys_ok && p.sys_ok)
            snprintf(sys, sizeof sys, " | sys read %.0f MB/s, page-in ios %.0f/s (%.0f MB/s)",
                     (double)(c.sio - p.sio) / 1048576.0 / dt, (double)(c.preadio - p.preadio) / dt,
                     (double)(c.pread - p.pread) * 4096.0 / 1048576.0 / dt);
        EVENT(t, "counters: proc cpu %4.0f%% of 1 core, sys cpu %3.0f%% | proc read %5.1f MB/s%s | RAM free %llu MB, private %llu MB",
              pcpu, sbusy, (double)(c.pio - p.pio) / 1048576.0 / dt, sys,
              (unsigned long long)(ms.ullAvailPhys >> 20), (unsigned long long)(pm.PrivateUsage >> 20));
        p = c; tp = t;
    }
    return 0;
}

/* ------------------------------------------------------------------ loading-screen pump step (03dde580)
 * During a transition the main thread runs 03775600: a loop with no sleep that polls a timer
 * (03dbfd00 -> QPC: 121k of its 155k samples in the Empty->Game profile) and calls the transition's
 * main-thread step 03dde580 for each task. Found by its unique call site in 03775600 (checked
 * offline: one match in .text). */
#define PUMP_SITE   "48 8B 0B E8 ?? ?? ?? ?? 48 83 C3 08 48 3B DF 75 EF 48 8B 0D ?? ?? ?? ?? E8"
#define PUMP_STEP   "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 30 48 8B F9"
static fn4 o_pump_step;
static uintptr_t h_pump_step(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    LARGE_INTEGER q0, q1; QueryPerformanceCounter(&q0);
    uintptr_t r = o_pump_step(a, b, c, d);
    QueryPerformanceCounter(&q1);
    InterlockedIncrement64(&g_pump_iters);
    InterlockedAdd64(&g_pump_ticks, q1.QuadPart - q0.QuadPart);
    if (g_cfg.pump_yield == 1) SwitchToThread();
    else if (g_cfg.pump_yield == 2) Sleep(1);
    return r;
}

static void install_pump(void)
{
    uint8_t *site = api->pattern_scan(PUMP_SITE);
    if (!site || !in_image(site, 32)) { api->log("fast_load", "FAIL-CLOSED pump: call-site pattern not found"); return; }
    int32_t rel; memcpy(&rel, site + 4, 4);
    uint8_t *step = site + 8 + rel;
    if (!in_image(step, 32) || !fl_match(step, 32, 0, PUMP_STEP)) { api->log("fast_load", "FAIL-CLOSED pump: step prologue mismatch"); return; }
    int st = api->hook(step, (void *)h_pump_step, (void **)&o_pump_step);
    api->log("fast_load", "%s pump_step at %08x (status %d, pump_yield=%d)", st ? "FAIL" : "hooked", rva(step), st, g_cfg.pump_yield);
    if (st) o_pump_step = NULL;
}

/* ------------------------------------------------------------------ idle worker polling (worker_spin=1..3)
 * Task-system worker loop 03dfa750 calls the "try get task" function 03dfa270(worker, Task **out) in a
 * tight loop. 03dfa270 only blocks (03df8710: mutex + condition variable) when NO worker of its group is
 * running a task ([group+0x40] == 0) and the queues are empty. While any one worker is busy, all the
 * others return 1 with *out == NULL immediately and re-poll with two lock-prefixed RMWs on the shared
 * counter [group+0x80] per poll, with no pause: 138k of 247k samples in the Empty->Game profile.
 * Hooked here (not 03df9180, which cpu_sched owns). Found by a unique 44-byte prologue (checked offline).
 * Backoff only after empty polls; any poll that returns a task resets it, so task pickup latency is
 * bounded by one backoff step (mode 1: <= 64 pause, a few us; mode 2: + SwitchToThread; mode 3: + Sleep(1)
 * after ~20000 consecutive empty polls, i.e. only when a worker has been idle for a long time). */
#define WORKER_GET "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 40 48 8B DA 4C 8B F1 48 8B 11 8B 49 3C 83 F9 02 75 07 B8 00 01 00 00 EB 16"
#define WS_YIELD_AFTER 1024
#define WS_SLEEP_AFTER 20000
static fn4 o_worker_get;
static DWORD g_ws_tls = TLS_OUT_OF_INDEXES;

static void ws_backoff(uintptr_t n, int mode)
{
    unsigned k = n < 96 ? 1u << (n / 16) : 64;              /* 1,2,4..64 pause per empty poll */
    for (unsigned i = 0; i < k; i++) __builtin_ia32_pause();
    InterlockedAdd64(&g_ws_pause, k);
    if (mode >= 3 && n >= WS_SLEEP_AFTER) { Sleep(1); InterlockedIncrement64(&g_ws_sleep); }
    else if (mode >= 2 && n >= WS_YIELD_AFTER) { SwitchToThread(); InterlockedIncrement64(&g_ws_yield); }
}

static uintptr_t h_worker_get(uintptr_t w, uintptr_t out, uintptr_t c, uintptr_t d)
{
    uintptr_t r = o_worker_get(w, out, c, d);
    int mode = g_cfg.worker_spin;
    if (!mode) return r;
    if ((uint8_t)r && out && *(void **)out == NULL) {          /* empty poll: worker will re-poll at once */
        uintptr_t n = (uintptr_t)TlsGetValue(g_ws_tls);
        TlsSetValue(g_ws_tls, (void *)(n + 1));
        InterlockedIncrement64(&g_ws_empty);
        ws_backoff(n, mode);
    } else if (TlsGetValue(g_ws_tls)) {
        TlsSetValue(g_ws_tls, NULL);
    }
    return r;
}

static void install_worker_spin(void)
{
    if (!g_cfg.worker_spin) return;
    if ((g_ws_tls = TlsAlloc()) == TLS_OUT_OF_INDEXES) { api->log("fast_load", "FAIL-CLOSED worker_spin: TlsAlloc failed"); return; }
    uint8_t *f = api->pattern_scan(WORKER_GET);
    if (!f || !in_image(f, 0x200) || api->function_start(f) != f) { api->log("fast_load", "FAIL-CLOSED worker_spin: try-get-task pattern not found"); return; }
    int st = api->hook(f, (void *)h_worker_get, (void **)&o_worker_get);
    api->log("fast_load", "%s worker_get at %08x (status %d, worker_spin=%d)", st ? "FAIL" : "hooked", rva(f), st, g_cfg.worker_spin);
    if (st) o_worker_get = NULL;
}

static double process_age_ms(void)
{
    FILETIME c, e, k, u, now;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a = { .LowPart = c.dwLowDateTime, .HighPart = c.dwHighDateTime };
    ULARGE_INTEGER b = { .LowPart = now.dwLowDateTime, .HighPart = now.dwHighDateTime };
    return b.QuadPart > a.QuadPart ? (double)(b.QuadPart - a.QuadPart) / 10000.0 : 0;
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    load_config();
    if (g_cfg.checksum_cache)
        api->log("fast_load", "checksum_cache=1 ignored (fail closed): the engine hashes file contents as one "
                 "running MD5 per manifest entry, so a per-file cache cannot give identical checksums");
    if (!g_cfg.timeline) { api->log("fast_load", "timeline=0: no hooks installed"); return 0; }
    QueryPerformanceFrequency(&g_freq); QueryPerformanceCounter(&g_qpc0);
    g_start_ms = process_age_ms();
    g_tl.cap = 16384; g_tl.ev = calloc((size_t)g_tl.cap, sizeof(fl_event));
    if (!g_tl.ev) return 1;
    snprintf(g_out_path, sizeof g_out_path, "%sload_timeline.txt", api->v3x_dir());
    EVENT(g_start_ms, "fast_load plugin loaded");
    install(A_TRANSITION, h_transition, &o_transition);
    install(A_DB_LOAD, h_db_load, &o_db_load);
    install(A_DB_REPORT, h_db_report, &o_db_report);
    install(A_MESH, h_mesh, &o_mesh);
    if (o_mesh) install_item_thunk();
    install(A_CS_START, h_cs_start, &o_cs_start);
    install(A_CS_FEED, h_cs_feed, &o_cs_feed);
    install_pump();
    install_worker_spin();
    if (g_cfg.counters) { HANDLE th = CreateThread(NULL, 0, counters_thread, NULL, 0, NULL); if (th) CloseHandle(th); }
    write_timeline();
    return 0;
}
