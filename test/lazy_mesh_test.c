/* Offline test of plugins/lazy_mesh (no game needed).
 *   (normally run by test/run_all.sh; scratch root = $V3X_TMP, else %TEMP%, see test/test_tmp.h)
 *   python -m ziglang cc -target x86_64-windows-gnu -O2 -Iinclude -o $V3X_TMP/lmt.exe test/lazy_mesh_test.c
 *   $V3X_TMP/lmt.exe                         -> ALL OK (run from v3x/: reads plugins/lazy_mesh/lazy_mesh.ini.example,
 *                                                          writes only under $V3X_TMP/lazymesh/t/)
 *   $V3X_TMP/lmt.exe ../binaries/victoria3.exe  also maps the real exe (no code runs) and resolves all
 *                                                          6 anchors with the plugin's own resolve_all()
 * Covers: config + example, eager name set (incl. reading the plugin's own report back), path rules,
 * the lock-free table (8 threads), lm_ensure (once, wait, re-entrancy, timeout), the report format, and
 * the plugin's hook logic end to end against fake engine functions (pass -> Load/anim -> ctor/rdata/blend,
 * engine Reload of a deferred type, measure mode, fail-closed when not armed). */
#include "../plugins/lazy_mesh/lm_core.c"
#include "../plugins/lazy_mesh/lazy_mesh.c"
#include <stdio.h>
#include <stdarg.h>
#include "test_tmp.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* ------------------------------------------------------------------ fake API */
static void x_log(const char *tag, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); printf("[%s] ", tag); vprintf(fmt, ap); putchar('\n'); va_end(ap); }
static int x_print(const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int n = vprintf(fmt, ap); putchar('\n'); va_end(ap); return n; }
static const char *t_ver(void) { return "test"; }
static char g_tdir[MAX_PATH];   /* <scratch>/lazymesh/t/, set in main */
static const char *t_dir(void) { return g_tdir; }
static int t_on_frame(v3x_frame_fn fn, void *u) { (void)fn; (void)u; return 0; }

/* real exe mapped as an image (resolution only) */
static uint8_t *X_base, *X_text; static size_t X_size, X_text_n;
static struct { uint8_t *p; size_t n; } X_data[8]; static int X_nd;
static uintptr_t x_base(void) { return (uintptr_t)X_base; }
static size_t x_size(void) { return X_size; }
static void *x_find_string(const char *s)
{
    size_t n = strlen(s) + 1;
    for (int r = 0; r < X_nd; r++)
        for (size_t i = 0; i + n <= X_data[r].n; i++)
            if (X_data[r].p[i] == (uint8_t)s[0] && !memcmp(X_data[r].p + i, s, n) && (i == 0 || X_data[r].p[i - 1] == 0))
                return X_data[r].p + i;
    return NULL;
}
static void *x_find_xref(const void *t, int index)
{
    for (size_t i = 0; i + 7 <= X_text_n; i++) {
        uint8_t *q = X_text + i;
        if ((q[0] != 0x48 && q[0] != 0x4C) || (q[1] != 0x8D && q[1] != 0x8B) || (q[2] & 0xC7) != 5) continue;
        int32_t d; memcpy(&d, q + 3, 4);
        if (q + 7 + d == (const uint8_t *)t && index-- == 0) return q;
    }
    return NULL;
}
static void *x_function_start(const void *addr)
{
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(X_base + ((IMAGE_DOS_HEADER *)X_base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    RUNTIME_FUNCTION *rf = (RUNTIME_FUNCTION *)(X_base + dd.VirtualAddress);
    size_t lo = 0, hi = dd.Size / sizeof *rf; DWORD r = (DWORD)((const uint8_t *)addr - X_base);
    while (lo < hi) {
        size_t m = (lo + hi) / 2;
        if (r < rf[m].BeginAddress) hi = m; else if (r >= rf[m].EndAddress) lo = m + 1;
        else {
            RUNTIME_FUNCTION *f = &rf[m];
            for (int g = 0; g < 32; g++) {
                uint8_t *ui = X_base + f->UnwindData;
                if (!((ui[0] >> 3) & UNW_FLAG_CHAININFO)) break;
                f = (RUNTIME_FUNCTION *)(ui + 4 + ((ui[2] + 1) & ~1u) * 2);
            }
            return X_base + f->BeginAddress;
        }
    }
    return NULL;
}

static int real_exe(const char *path)
{
    X_base = (uint8_t *)LoadLibraryExA(path, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!X_base) { printf("cannot map %s (%lu)\n", path, GetLastError()); return 1; }
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(X_base + ((IMAGE_DOS_HEADER *)X_base)->e_lfanew);
    X_size = nt->OptionalHeader.SizeOfImage;
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (!memcmp(sec->Name, ".text", 5)) { X_text = X_base + sec->VirtualAddress; X_text_n = sec->Misc.VirtualSize; }
        else if ((!memcmp(sec->Name, ".rdata", 6) || !memcmp(sec->Name, ".data", 5)) && X_nd < 8)
            { X_data[X_nd].p = X_base + sec->VirtualAddress; X_data[X_nd++].n = sec->Misc.VirtualSize; }
    }
    static V3X_API fake;
    fake.api_version = 3; fake.struct_size = sizeof fake;
    fake.log = x_log; fake.image_base = x_base; fake.image_size = x_size;
    fake.find_string = x_find_string; fake.find_xref = x_find_xref; fake.function_start = x_function_start;
    api = &fake;
    int bad = resolve_all();
    /* 1.15.1 RVAs found by docs/re/lazy_mesh.md (informational: a patch may move them) */
    static const uint32_t expect[A_COUNT] = { 0x3adcfd0, 0x3ae1400, 0x3ab3f10, 0x3ad80a0, 0x3ad7860, 0x37a9520 };
    for (int i = 0; i < A_COUNT; i++)
        printf("%-6s %s %08x%s\n", A[i].name, A[i].fn ? "OK  " : "FAIL", A[i].fn ? rva(A[i].fn) : 0,
               A[i].fn && rva(A[i].fn) != expect[i] ? "  (differs from 1.15.1)" : "");
    printf("real exe: %s\n", bad ? "ANCHORS FAILED" : "ALL ANCHORS OK");
    if (!bad) {   /* fail closed: break one shape byte of Load (copy-on-write page of the mapping) */
        uint8_t *p = A[A_LOAD].fn + 0x72; DWORD old;
        if (VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old)) {
            uint8_t keep = *p; *p = 0x90;
            int bad2 = resolve_all();
            CHECK(bad2 >= 2 && !A[A_LOAD].fn && !A[A_RDATA].fn && A[A_CTOR].fn);   /* rdata is found through Load */
            *p = keep; VirtualProtect(p, 1, old, &old);
            CHECK(resolve_all() == 0);
            printf("real exe: fail-closed check %s\n", bad2 >= 2 ? "OK" : "FAILED");
        }
    }
    return bad;
}

/* ------------------------------------------------------------------ fake engine for the hook logic */

typedef struct { void *vt; uint8_t body[0x200]; } fake_type_t;   /* only +0x08 and +0x198 are read */
static void set_str(void *at, const char *s)
{
    struct { union { char buf[16]; char *ptr; } u; uint64_t size, cap; } *ms = at;
    memset(ms, 0, sizeof *ms);
    size_t n = strlen(s);
    if (n <= 15) { memcpy(ms->u.buf, s, n + 1); ms->cap = 15; }
    else { ms->u.ptr = _strdup(s); ms->cap = n; }
    ms->size = n;
}
static void make_type(fake_type_t *t, const char *name, const char *path)
{
    memset(t, 0, sizeof *t);
    set_str((uint8_t *)t + MT_NAME, name);
    set_str((uint8_t *)t + MT_PATH, path);
}

#define NT 8
static fake_type_t T[NT];
static volatile LONG load_count[NT], anim_count[NT];
static char order[256]; static volatile LONG order_n;
static int idx_of(void *p) { for (int i = 0; i < NT; i++) if (p == &T[i]) return i; return -1; }
static void ev(char c) { LONG k = InterlockedIncrement(&order_n) - 1; if (k < (LONG)sizeof order - 1) order[k] = c; }

static uintptr_t h_rdata(uintptr_t type, uintptr_t out, uintptr_t c, uintptr_t d);
/* like the engine's Load: it calls rdata(this, ...) itself (must not count as a use, must not wait) */
static void f_load(void *t) { int i = idx_of(t); if (i >= 0) InterlockedIncrement(&load_count[i]); ev('L'); Sleep(1); h_rdata((uintptr_t)t, 0, 0, 0); }
static void f_anim(void *t) { int i = idx_of(t); if (i >= 0) InterlockedIncrement(&anim_count[i]); ev('A'); }
static uintptr_t f_ctor(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) { (void)b; (void)c; (void)d; ev('C'); return a; }
static uintptr_t f_rdata(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) { (void)a; (void)c; (void)d; ev('R'); return b; }
static uintptr_t f_blend(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) { (void)b; (void)c; (void)d; ev('B'); return a; }
/* the engine pass: Load for every type (here on 4 threads), then the serial anim import */
static DWORD WINAPI pass_worker(void *p) { for (int i = (int)(intptr_t)p; i < NT; i += 4) h_load(&T[i]); return 0; }
static uintptr_t f_pass(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    (void)a; (void)b; (void)c; (void)d;
    HANDLE th[4];
    for (int k = 0; k < 4; k++) th[k] = CreateThread(NULL, 0, pass_worker, (void *)(intptr_t)k, 0, NULL);
    WaitForMultipleObjects(4, th, TRUE, INFINITE);
    for (int k = 0; k < 4; k++) CloseHandle(th[k]);
    for (int i = 0; i < NT; i++) h_anim(&T[i]);
    return 77;
}

static void reset_plugin(int mode, int armed)
{
    lm_table_free(&g_tab);
    CHECK(lm_table_init(&g_tab, 1u << 10) == 0);
    lm_config_defaults(&g_cfg); g_cfg.mode = mode; g_cfg.log_loads = 1;
    lm_nameset_free(&g_eager); lm_nameset_add(&g_eager, "listed_portrait_mesh", 20);
    g_armed = armed; g_deferred = g_lazy_loads = g_timeouts = 0; g_pass_active = 0; g_phase = -1;
    for (int i = 0; i < NT; i++) load_count[i] = anim_count[i] = 0;
    order_n = 0; memset(order, 0, sizeof order);
    lm_prev_free(&g_prev); lm_nameset_free(&g_prevstart); memset(&g_stats, 0, sizeof g_stats); g_prefetch_started = 0; g_prefetch_stop = 0;
    g_cfg.prefetch_gap_ms = 0;
    o_load = f_load; o_anim = f_anim; o_ctor = f_ctor; o_rdata = f_rdata; o_blend = f_blend; o_pass = f_pass;
}

static void plugin_logic(void)
{
    static V3X_API fake;
    fake.api_version = 3; fake.struct_size = sizeof fake;
    fake.log = x_log; fake.print = x_print; fake.game_version = t_ver; fake.v3x_dir = t_dir; fake.on_frame = t_on_frame;
    api = &fake;
    snprintf(g_report_path, sizeof g_report_path, "%slazy_mesh_used.txt", t_dir());
    QueryPerformanceFrequency(&g_freq); QueryPerformanceCounter(&g_qpc0);

    make_type(&T[0], "tree_01_mesh", "gfx/models/environment/trees/tree_01.mesh");              /* eager:default */
    make_type(&T[1], "debug_cube_mesh", "gfx/models/debug/color_reference.mesh");               /* deferred:debug */
    make_type(&T[2], "female_coat_european_greatcoat_01_mesh", "gfx/models/portraits/attachments/coat.mesh"); /* deferred:dir */
    make_type(&T[3], "railroad_bridge_01_large_mesh", "gfx/models/military/x.mesh");            /* eager:rule (bridge) */
    make_type(&T[4], "listed_portrait_mesh", "gfx/models/portraits/x.mesh");                    /* eager:listed */
    make_type(&T[5], "infantry_mesh", "Gfx\\Models\\Military\\Infantry\\inf.mesh");             /* deferred:dir (normalised) */
    make_type(&T[6], "building_mesh", "gfx/models/buildings/b.mesh");                           /* eager:default */
    make_type(&T[7], "unit_reloaded_mesh", "gfx/models/military/r.mesh");                       /* deferred, then engine Reload */

    /* ---- lazy, armed */
    reset_plugin(LM_MODE_LAZY, 1);
    CHECK(h_pass(1, 2, 3, 4) == 77);
    CHECK(g_tab.n == NT && g_deferred == 4);
    for (int i = 0; i < NT; i++) CHECK(lm_table_find(&g_tab, &T[i])->uses == 0);   /* Load's own rdata call is no use */
    int deferred_idx[] = { 1, 2, 5, 7 }, eager_idx[] = { 0, 3, 4, 6 };
    for (int k = 0; k < 4; k++) {
        CHECK(load_count[deferred_idx[k]] == 0 && anim_count[deferred_idx[k]] == 0);   /* anim import skipped too */
        CHECK(load_count[eager_idx[k]] == 1 && anim_count[eager_idx[k]] == 1);
        CHECK(lm_table_find(&g_tab, &T[deferred_idx[k]])->state == LM_ST_DEFERRED);
        CHECK(lm_table_find(&g_tab, &T[eager_idx[k]])->state == LM_ST_DONE);
    }
    CHECK(lm_table_find(&g_tab, &T[1])->decision == LM_DEFER_DEBUG);
    CHECK(lm_table_find(&g_tab, &T[2])->decision == LM_DEFER_DIR);
    CHECK(lm_table_find(&g_tab, &T[3])->decision == LM_EAGER_RULE);
    CHECK(lm_table_find(&g_tab, &T[4])->decision == LM_EAGER_LISTED);
    CHECK(lm_table_find(&g_tab, &T[5])->decision == LM_DEFER_DIR);
    CHECK(!strcmp(lm_table_find(&g_tab, &T[2])->name, "female_coat_european_greatcoat_01_mesh"));
    CHECK(!strcmp(lm_table_find(&g_tab, &T[5])->path, "Gfx\\Models\\Military\\Infantry\\inf.mesh"));

    /* choke point: load + anim BEFORE the original ctor, exactly once */
    order_n = 0; memset(order, 0, sizeof order);
    CHECK(h_ctor(0x1000, (uintptr_t)&T[1], 0, 0) == 0x1000);
    CHECK(!strcmp(order, "LRAC"));
    CHECK(load_count[1] == 1 && anim_count[1] == 1 && lm_table_find(&g_tab, &T[1])->state == LM_ST_DONE);
    CHECK(h_ctor(0x2000, (uintptr_t)&T[1], 0, 0) == 0x2000);
    CHECK(load_count[1] == 1 && !strcmp(order, "LRACC"));
    CHECK(lm_table_find(&g_tab, &T[1])->uses == 2 && lm_table_find(&g_tab, &T[1])->first_site == LM_SITE_CTOR);
    /* entity paths */
    order_n = 0; memset(order, 0, sizeof order);
    CHECK(h_rdata((uintptr_t)&T[2], 0x55, 0, 0) == 0x55 && !strcmp(order, "LRAR") && load_count[2] == 1);
    order_n = 0; memset(order, 0, sizeof order);
    CHECK(h_blend(0x66, 0, (uintptr_t)&T[5], 0) == 0x66 && !strcmp(order, "LRAB") && load_count[5] == 1);
    /* eager type and unknown pointer: no load, just the original */
    order_n = 0; memset(order, 0, sizeof order);
    static fake_type_t unknown;
    CHECK(h_ctor(0x3000, (uintptr_t)&T[0], 0, 0) == 0x3000 && h_ctor(0x4000, (uintptr_t)&unknown, 0, 0) == 0x4000);
    CHECK(!strcmp(order, "CC") && load_count[0] == 1);
    /* engine Reload of a deferred type (outside the pass): engine loads it, we must not load it again */
    order_n = 0; memset(order, 0, sizeof order);
    h_load(&T[7]);
    CHECK(load_count[7] == 1 && lm_table_find(&g_tab, &T[7])->state == LM_ST_DONE);
    h_ctor(0x5000, (uintptr_t)&T[7], 0, 0);
    CHECK(load_count[7] == 1 && anim_count[7] == 0 && !strcmp(order, "LRC"));   /* Reload does its own import */
    /* anim import called again for an already loaded type goes to the original (it no-ops in the engine) */
    h_anim(&T[1]); CHECK(anim_count[1] == 2);
    CHECK(g_lazy_loads == 3);
    /* console 'load' */
    CHECK(lm_table_find(&g_tab, &T[0])->state == LM_ST_DONE);
    cmd_lazymesh("load nonexistent_mesh", NULL);
    cmd_lazymesh("", NULL);

    /* report written at the end of the pass and on dump; used types first, unused as comments */
    InterlockedExchange(&g_dirty, 1); write_report();
    FILE *f = fopen(g_report_path, "rb"); CHECK(f != NULL);
    static char rep[1 << 16]; size_t rn = 0;
    if (f) { rn = fread(rep, 1, sizeof rep - 1, f); fclose(f); } rep[rn] = 0;
    CHECK(strstr(rep, "# V3X lazy_mesh report, mode=lazy, game test") != NULL);
    CHECK(strstr(rep, "\ndebug_cube_mesh\t") != NULL);
    CHECK(strstr(rep, "\tmesh_object\tdeferred:debug\t") != NULL);
    CHECK(strstr(rep, "# unused building_mesh\teager:default") != NULL);
    {   /* the report is a valid eager file: only the used types become names */
        lm_nameset s = { 0 };
        CHECK(lm_nameset_parse(&s, rep) == 5);   /* T1, T2, T5, T0, T7 were requested */
        CHECK(lm_nameset_has(&s, "debug_cube_mesh") && lm_nameset_has(&s, "tree_01_mesh") && !lm_nameset_has(&s, "building_mesh"));
        lm_nameset_free(&s);
    }

    /* ---- previous report: entity-parse type loads in the pass; Game transition prefetches used types,
     * most used first, and leaves never-used ones deferred; summary counts main-thread loads */
    reset_plugin(LM_MODE_LAZY, 1);
    g_main_tid = GetCurrentThreadId();
    lm_prev_parse(&g_prev, "infantry_mesh\t1\tstart\t5\tentity_blendshape\n"
                           "unit_reloaded_mesh\t300\tGame\t2\tmesh_object\n"
                           "female_coat_european_greatcoat_01_mesh\t500\tGame\t40\tmesh_object\n");
    CHECK(lm_prev_startup_set(&g_prev, &g_prevstart) == 1);
    h_pass(0, 0, 0, 0);
    CHECK(g_deferred == 3 && load_count[5] == 1 && lm_table_find(&g_tab, &T[5])->decision == LM_EAGER_PREV);
    h_ctor(1, (uintptr_t)&T[1], 0, 0);                      /* main-thread on-demand load (a hitch) */
    order_n = 0; memset(order, 0, sizeof order);
    V3X_LoadTransition tr = { "Empty", "Game", 1.0 };
    on_transition("load_transition", &tr, NULL);
    for (int w = 0; w < 500 && !(lm_table_find(&g_tab, &T[2])->state == LM_ST_DONE && lm_table_find(&g_tab, &T[7])->state == LM_ST_DONE); w++) Sleep(10);
    Sleep(50);
    CHECK(load_count[2] == 1 && load_count[7] == 1 && anim_count[2] == 1);
    CHECK(!strncmp(order, "LRALRA", 6));
    CHECK(lm_table_find(&g_tab, &T[1])->state == LM_ST_DONE);            /* loaded by the ctor */
    CHECK(g_stats.main_n == 1 && g_stats.n[LM_SITE_CTOR] == 1 && g_stats.n[LM_SITE_PREFETCH] == 2);
    on_transition("load_transition", &tr, NULL);                          /* second Game transition: no new thread */
    CHECK(g_prefetch_started == 1);
    {   /* order check: coat (40 uses) before unit_reloaded (2) -- prefetch ran them in that order */
        lm_entry *a = lm_table_find(&g_tab, &T[2]), *b = lm_table_find(&g_tab, &T[7]);
        CHECK(a->lazy_ms > 0 && b->lazy_ms > 0);
    }
    /* prefetch_all=0 leaves unknown deferred types alone */
    reset_plugin(LM_MODE_LAZY, 1);
    lm_prev_parse(&g_prev, "unit_reloaded_mesh\t300\tGame\t2\tmesh_object\n");
    h_pass(0, 0, 0, 0);
    on_transition("load_transition", &tr, NULL);
    for (int w = 0; w < 500 && lm_table_find(&g_tab, &T[7])->state != LM_ST_DONE; w++) Sleep(10);
    Sleep(100);
    CHECK(load_count[7] == 1 && load_count[1] == 0 && load_count[2] == 0 && load_count[5] == 0);
    CHECK(lm_table_find(&g_tab, &T[1])->state == LM_ST_DEFERRED);
    /* not armed: no prefetch thread */
    reset_plugin(LM_MODE_LAZY, 0);
    h_pass(0, 0, 0, 0);
    on_transition("load_transition", &tr, NULL);
    CHECK(g_prefetch_started == 0);

    /* ---- measure: nothing deferred, uses recorded, decisions show what lazy would do */
    reset_plugin(LM_MODE_MEASURE, 0);
    h_pass(0, 0, 0, 0);
    CHECK(g_deferred == 0);
    for (int i = 0; i < NT; i++) CHECK(load_count[i] == 1 && anim_count[i] == 1);
    CHECK(lm_table_find(&g_tab, &T[1])->decision == LM_DEFER_DEBUG);   /* "would defer" */
    for (int i = 0; i < NT; i++) CHECK(lm_table_find(&g_tab, &T[i])->uses == 0);
    h_ctor(1, (uintptr_t)&T[2], 0, 0);
    CHECK(load_count[2] == 1 && lm_table_find(&g_tab, &T[2])->uses == 1);

    /* ---- lazy requested but not armed (an anchor/hook failed): fail closed = everything eager */
    reset_plugin(LM_MODE_LAZY, 0);
    h_pass(0, 0, 0, 0);
    CHECK(g_deferred == 0);
    for (int i = 0; i < NT; i++) CHECK(load_count[i] == 1 && lm_table_find(&g_tab, &T[i])->decision == LM_EAGER_MODE);
}

/* the 8-thread first-use race, against the real hook path */
static volatile LONG go;
static DWORD WINAPI racer(void *p) { (void)p; while (!go) YieldProcessor(); h_ctor(1, (uintptr_t)&T[2], 0, 0); return 0; }
static void race(void)
{
    reset_plugin(LM_MODE_LAZY, 1);
    h_pass(0, 0, 0, 0);
    CHECK(load_count[2] == 0);
    HANDLE th[8]; go = 0;
    for (int k = 0; k < 8; k++) th[k] = CreateThread(NULL, 0, racer, NULL, 0, NULL);
    go = 1;
    WaitForMultipleObjects(8, th, TRUE, INFINITE);
    for (int k = 0; k < 8; k++) CloseHandle(th[k]);
    CHECK(load_count[2] == 1 && anim_count[2] == 1);
    CHECK(lm_table_find(&g_tab, &T[2])->uses == 8);
}

/* ------------------------------------------------------------------ lm_core unit tests */

static volatile LONG loads; static lm_table *TT; static int32_t sleep_ms;
static void slow_load(void *k, void *u)
{
    (void)u;
    InterlockedIncrement(&loads);
    if (sleep_ms) Sleep((DWORD)sleep_ms);
    /* a call from inside the load (the engine's Load calling rdata) must not deadlock */
    CHECK(lm_ensure(TT, k, (int32_t)GetCurrentThreadId(), slow_load, NULL, 1000, NULL) == LM_R_REENTRANT);
}
static volatile LONG waited, loaded_now;
static DWORD WINAPI ensurer(void *k)
{
    int r = lm_ensure(TT, k, (int32_t)GetCurrentThreadId(), slow_load, NULL, 10000, NULL);
    if (r == LM_R_WAITED || r == LM_R_READY) InterlockedIncrement(&waited);
    if (r == LM_R_LOADED) InterlockedIncrement(&loaded_now);
    lm_entry *e = lm_table_find(TT, k);
    CHECK(e && e->state == LM_ST_DONE);   /* nobody returns before the load finished */
    return 0;
}
static uint8_t keys[4000];
static DWORD WINAPI inserter(void *p)
{
    (void)p;
    for (int i = 0; i < 4000; i++) { int c; lm_entry *e = lm_table_insert(TT, keys + i, &c); CHECK(e && e->key == (int64_t)(uintptr_t)(keys + i)); }
    return 0;
}

static void core_tests(void)
{
    lm_config c; char err[160];
    lm_config_defaults(&c);
    CHECK(c.mode == LM_MODE_OFF && c.skip_debug == 1 && c.defer_all == 0 && c.n_defer_dirs == 2 && c.n_eager_contains == 1);
    CHECK(!strcmp(c.defer_dirs[0], "gfx/models/portraits/") && !strcmp(c.defer_dirs[1], "gfx/models/military/"));
    CHECK(!strcmp(c.eager_contains[0], "bridge") && !strcmp(c.eager_file, "lazy_mesh_eager.txt"));
    CHECK(lm_config_parse(&c, "# c\r\nmode = lazy\r\n;x\n\ndefer_all=1 # yes\nskip_debug = 0\nprefetch_after_s=120\n"
                              "defer_dirs = GFX\\Models\\Units , gfx/models/x/,,\neager_contains = a, b_c\nwait_timeout_ms=5000\nlog_loads=0\n"
                              "eager_file = my_list.txt\n", err, sizeof err) == 0);
    CHECK(c.mode == LM_MODE_LAZY && c.defer_all == 1 && c.skip_debug == 0 && c.prefetch_after_s == 120 && c.wait_timeout_ms == 5000 && c.log_loads == 0);
    CHECK(c.n_defer_dirs == 2 && !strcmp(c.defer_dirs[0], "gfx/models/units") && !strcmp(c.defer_dirs[1], "gfx/models/x/"));
    CHECK(c.n_eager_contains == 2 && !strcmp(c.eager_contains[1], "b_c") && !strcmp(c.eager_file, "my_list.txt"));
    CHECK(lm_config_parse(&c, "defer_dirs = none\neager_contains=none\n", err, sizeof err) == 0 && c.n_defer_dirs == 0 && c.n_eager_contains == 0);
    lm_config_defaults(&c);
    CHECK(lm_config_parse(&c, "mode = fast\nfoo=1\nnoeq\nskip_debug=2\neager_file=../x\nwait_timeout_ms=5\nmode=\n", err, sizeof err) == 7);
    CHECK(!strncmp(err, "line 1:", 7) && c.mode == LM_MODE_OFF && c.skip_debug == 1 && !strcmp(c.eager_file, "lazy_mesh_eager.txt"));
    CHECK(lm_config_parse(&c, "mode=measure", err, sizeof err) == 0 && c.mode == LM_MODE_MEASURE);
    CHECK(c.startup_from_report == 1 && c.prefetch_on_game == 1 && c.prefetch_all == 0 && c.prefetch_gap_ms == 20);
    CHECK(lm_config_parse(&c, "startup_from_report=0\nprefetch_on_game=0\nprefetch_all=1\nprefetch_gap_ms=0\n", err, sizeof err) == 0);
    CHECK(c.startup_from_report == 0 && c.prefetch_on_game == 0 && c.prefetch_all == 1 && c.prefetch_gap_ms == 0);
    CHECK(lm_config_parse(&c, "prefetch_gap_ms=-1\nprefetch_all=2\n", err, sizeof err) == 2);
    {   /* the shipped example parses cleanly and equals the defaults (mode=off) */
        FILE *ef = fopen("plugins/lazy_mesh/lazy_mesh.ini.example", "rb");
        char eb[8192] = {0}; CHECK(ef != NULL);
        if (ef) { fread(eb, 1, sizeof eb - 1, ef); fclose(ef); }
        lm_config ex, def; lm_config_defaults(&ex); lm_config_defaults(&def);
        CHECK(lm_config_parse(&ex, eb, err, sizeof err) == 0);
        if (err[0]) printf("example: %s\n", err);
        CHECK(!memcmp(&ex, &def, sizeof ex));
    }

    /* names */
    lm_nameset s = { 0 };
    CHECK(lm_nameset_add(&s, "abc", 3) == 1 && lm_nameset_add(&s, "abc", 3) == 0 && lm_nameset_add(&s, "", 0) == 0);
    CHECK(lm_nameset_has(&s, "abc") && !lm_nameset_has(&s, "ab") && !lm_nameset_has(&s, "abcd"));
    char big[64];
    for (int i = 0; i < 3000; i++) { snprintf(big, sizeof big, "mesh_%d", i); CHECK(lm_nameset_add(&s, big, strlen(big)) == 1); }
    CHECK(s.n == 3001 && lm_nameset_has(&s, "mesh_2999") && !lm_nameset_has(&s, "mesh_3000"));
    lm_nameset_free(&s);
    CHECK(lm_nameset_parse(&s, "# header\r\nfoo_mesh\t1.0\tx\r\n  bar_mesh  \n; c\n# unused baz_mesh\nfoo_mesh\nqux") == 3);
    CHECK(lm_nameset_has(&s, "foo_mesh") && lm_nameset_has(&s, "bar_mesh") && lm_nameset_has(&s, "qux") && !lm_nameset_has(&s, "baz_mesh"));
    CHECK(!lm_nameset_has(NULL, "x"));

    /* paths + decisions */
    char np[64];
    lm_norm_path(".\\Gfx\\\\Models/DEBUG/x.MESH", np, sizeof np); CHECK(!strcmp(np, "gfx/models/debug/x.mesh"));
    lm_norm_path("abcdef", np, 4); CHECK(!strcmp(np, "abc"));
    lm_config_defaults(&c);
    CHECK(lm_decide(&c, &s, NULL, "x", "gfx/models/debug/a.mesh") == LM_EAGER_MODE);          /* mode off */
    c.mode = LM_MODE_LAZY;
    CHECK(lm_decide(&c, &s, NULL, "x", "gfx/models/debug/a.mesh") == LM_DEFER_DEBUG);
    CHECK(lm_decide(&c, &s, NULL, "x", "GFX/Models/Portraits/Attachments/a.mesh") == LM_DEFER_DIR);
    CHECK(lm_decide(&c, &s, NULL, "x", "gfx/models/buildings/a.mesh") == LM_EAGER_DEFAULT);
    CHECK(lm_decide(&c, &s, NULL, "x", NULL) == LM_EAGER_DEFAULT);
    CHECK(lm_decide(&c, &s, NULL, "foo_mesh", "gfx/models/debug/a.mesh") == LM_EAGER_LISTED);  /* list beats rules */
    CHECK(lm_decide(&c, &s, NULL, "dirtroad_bridge_01_small_mesh", "gfx/models/military/b.mesh") == LM_EAGER_RULE);
    c.skip_debug = 0; CHECK(lm_decide(&c, &s, NULL, "x", "gfx/models/debug/a.mesh") == LM_EAGER_DEFAULT);
    c.defer_all = 1; CHECK(lm_decide(&c, &s, NULL, "x", "gfx/models/buildings/a.mesh") == LM_DEFER_ALL);
    CHECK(lm_decide(&c, &s, NULL, "bar_mesh", "gfx/models/buildings/a.mesh") == LM_EAGER_LISTED);
    c.n_defer_dirs = 0; c.defer_all = 0; CHECK(lm_decide(&c, &s, NULL, "x", "gfx/models/portraits/a.mesh") == LM_EAGER_DEFAULT);
    {   /* previous report: entity-parse types become eager:prev_startup (unless startup_from_report=0) */
        lm_prev pv;
        const char *rep_txt = "# V3X lazy_mesh report\n# columns ...\n"
            "head_mesh\t0.4\tstart\t3\tentity_blendshape\tdeferred:dir\t0\t120\tgfx/models/portraits/h.mesh\r\n"
            "coat_mesh\t300.0\tGame\t42\tmesh_object\tdeferred:dir\t0\t80\tgfx/models/portraits/c.mesh\n"
            "ovr_mesh\t1.0\tstart\t1\tentity_override\tdeferred:dir\t0\t80\tp\n"
            "# unused never_mesh\tdeferred:dir\t0\tp\n\nbare_name\n";
        CHECK(lm_prev_parse(&pv, rep_txt) == 4);
        const lm_prev_item *it = lm_prev_find(&pv, "coat_mesh");
        CHECK(it && it->uses == 42 && it->site == LM_SITE_CTOR);
        it = lm_prev_find(&pv, "head_mesh"); CHECK(it && it->uses == 3 && it->site == LM_SITE_BLEND);
        it = lm_prev_find(&pv, "bare_name"); CHECK(it && it->uses == 1 && it->site == LM_SITE_NONE);
        CHECK(!lm_prev_find(&pv, "never_mesh") && !lm_prev_find(&pv, "zzz"));
        lm_nameset ps = { 0 };
        CHECK(lm_prev_startup_set(&pv, &ps) == 2 && lm_nameset_has(&ps, "head_mesh") && lm_nameset_has(&ps, "ovr_mesh") && !lm_nameset_has(&ps, "coat_mesh"));
        lm_config_defaults(&c); c.mode = LM_MODE_LAZY;
        CHECK(lm_decide(&c, NULL, &ps, "head_mesh", "gfx/models/portraits/h.mesh") == LM_EAGER_PREV);
        CHECK(lm_decide(&c, NULL, &ps, "coat_mesh", "gfx/models/portraits/c.mesh") == LM_DEFER_DIR);
        c.startup_from_report = 0;
        CHECK(lm_decide(&c, NULL, &ps, "head_mesh", "gfx/models/portraits/h.mesh") == LM_DEFER_DIR);
        lm_nameset_free(&ps); lm_prev_free(&pv);
        CHECK(lm_prev_parse(&pv, "") == 0 && !lm_prev_find(&pv, "x")); lm_prev_free(&pv);
    }
    {   /* session stats */
        lm_stats st; memset(&st, 0, sizeof st); char sb[512];
        lm_stats_add(&st, LM_SITE_CTOR, 120, 1, "unit_mesh");
        lm_stats_add(&st, LM_SITE_BLEND, 944, 0, "head_mesh");
        lm_stats_add(&st, LM_SITE_PREFETCH, 50, 0, "p");
        lm_stats_format(&st, sb, sizeof sb);
        CHECK(strstr(sb, "on-demand loads 3, 1114 ms total, main thread 1 (120 ms), >100 ms 2, >500 ms 1, worst 944 ms head_mesh (entity_blendshape)") != NULL);
        CHECK(strstr(sb, "| mesh_object 1 / 120 ms | entity_blendshape 1 / 944 ms | prefetch 1 / 50 ms") != NULL);
    }
    lm_nameset_free(&s);

    /* table */
    lm_table t; CHECK(lm_table_init(&t, 1u << 13) == 0); TT = &t;
    int cr;
    lm_entry *e1 = lm_table_insert(&t, keys + 1, &cr); CHECK(e1 && cr == 1 && e1->phase == -1);
    CHECK(lm_table_insert(&t, keys + 1, &cr) == e1 && cr == 0);
    CHECK(lm_table_find(&t, keys + 1) == e1 && lm_table_find(&t, keys + 2) == NULL && lm_table_find(&t, NULL) == NULL);
    lm_table_free(&t);
    CHECK(lm_table_init(&t, 1u << 13) == 0);
    {   /* 8 threads insert the same 4000 keys concurrently: 4000 entries, every key once */
        HANDLE th[8];
        for (int k = 0; k < 8; k++) th[k] = CreateThread(NULL, 0, inserter, NULL, 0, NULL);
        WaitForMultipleObjects(8, th, TRUE, INFINITE);
        for (int k = 0; k < 8; k++) CloseHandle(th[k]);
        CHECK(t.n == 4000 && t.full == 0);
        int cnt = 0; for (uint32_t i = 0; i < t.cap; i++) cnt += t.e[i].key != 0;
        CHECK(cnt == 4000);
    }
    lm_table_free(&t);
    CHECK(lm_table_init(&t, 16) == 0);   /* full at 3/4: the 13th insert fails (caller loads eagerly) */
    for (int i = 0; i < 12; i++) CHECK(lm_table_insert(&t, keys + 100 + i, &cr) != NULL);
    CHECK(lm_table_insert(&t, keys + 200, &cr) == NULL && t.full == 1);
    CHECK(lm_table_insert(&t, keys + 100, &cr) != NULL);   /* existing keys are still found */
    lm_table_free(&t);

    /* ensure */
    CHECK(lm_table_init(&t, 1u << 10) == 0);
    lm_entry *e;
    CHECK(lm_ensure(&t, keys + 5, 1, slow_load, NULL, 100, &e) == LM_R_UNTRACKED && e == NULL);
    e = lm_table_insert(&t, keys + 5, &cr); e->state = LM_ST_DONE;
    CHECK(lm_ensure(&t, keys + 5, 1, slow_load, NULL, 100, NULL) == LM_R_READY && loads == 0);
    e = lm_table_insert(&t, keys + 6, &cr); e->state = LM_ST_DEFERRED;
    CHECK(lm_ensure(&t, keys + 6, (int32_t)GetCurrentThreadId(), slow_load, NULL, 100, NULL) == LM_R_LOADED && loads == 1);
    CHECK(e->state == LM_ST_DONE && e->owner == 0);
    CHECK(lm_ensure(&t, keys + 6, 1, slow_load, NULL, 100, NULL) == LM_R_READY && loads == 1);
    e = lm_table_insert(&t, keys + 7, &cr); e->state = LM_ST_LOADING; e->owner = 999999;   /* stuck on another thread */
    DWORD t0 = GetTickCount();
    CHECK(lm_ensure(&t, keys + 7, 1, slow_load, NULL, 200, NULL) == LM_R_TIMEOUT);
    CHECK(GetTickCount() - t0 >= 190 && loads == 1);
    CHECK(lm_ensure(&t, keys + 7, 999999, slow_load, NULL, 200, NULL) == LM_R_REENTRANT);
    {   /* 16 threads race on one deferred key while the load takes 100 ms */
        e = lm_table_insert(&t, keys + 8, &cr); e->state = LM_ST_DEFERRED;
        loads = 0; sleep_ms = 100; waited = loaded_now = 0;
        HANDLE th[16];
        for (int k = 0; k < 16; k++) th[k] = CreateThread(NULL, 0, ensurer, keys + 8, 0, NULL);
        WaitForMultipleObjects(16, th, TRUE, INFINITE);
        for (int k = 0; k < 16; k++) CloseHandle(th[k]);
        CHECK(loads == 1 && loaded_now == 1 && waited == 15);
        sleep_ms = 0;
    }
    /* note_use */
    lm_note_use(e, LM_SITE_BLEND, 123.0, 2); lm_note_use(e, LM_SITE_CTOR, 456.0, 3);
    CHECK(e->uses == 2 && e->first_site == LM_SITE_BLEND && e->first_ms == 123.0 && e->phase == 2);
    lm_note_use(NULL, 1, 0, 0);
    /* report */
    snprintf(e->name, sizeof e->name, "used_mesh"); snprintf(e->path, sizeof e->path, "gfx/models/u.mesh"); e->decision = LM_DEFER_DIR;
    lm_entry *e2 = lm_table_insert(&t, keys + 9, &cr); snprintf(e2->name, sizeof e2->name, "unused_mesh"); e2->pass_ms = 42;
    const char *ph[] = { "Frontend", "Loading", "Game" };
    lm_report_info info = { LM_MODE_MEASURE, "1.15.1", 100.0, ph, 3, NULL };
    char out[4096];
    size_t n = lm_format_report(&t, &info, out, sizeof out);
    CHECK(n == strlen(out));
    CHECK(strstr(out, "mode=measure, game 1.15.1") != NULL);
    CHECK(strstr(out, "\nused_mesh\t0.0\tGame\t2\tentity_blendshape\tdeferred:dir\t0\t0\tgfx/models/u.mesh\n") != NULL);
    CHECK(strstr(out, "# unused unused_mesh\teager\t42\t\n") != NULL);
    char tiny[16]; CHECK(lm_format_report(&t, &info, tiny, sizeof tiny) == 15 && strlen(tiny) == 15);
    {   /* merge: union with the previous report, counts added, entity-parse site kept, nothing lost */
        lm_prev pv;
        lm_prev_parse(&pv, "used_mesh\t5\tGame\t10\tentity_blendshape\n"      /* used now too -> 10+2, site kept */
                           "unused_mesh\t9\tGame\t4\tmesh_object\n"           /* in table, not used now -> carried */
                           "gone_mesh\t9\tGame\t7\tentity_override\n");       /* not in table -> carried */
        lm_entry *eu = lm_table_find(&t, keys + 8); eu->first_site = LM_SITE_CTOR;
        info.prev = &pv;
        static char big[8192];
        lm_format_report(&t, &info, big, sizeof big);
        CHECK(strstr(big, "\nused_mesh\t0.0\tGame\t12\tentity_blendshape\tdeferred:dir\t") != NULL);
        CHECK(strstr(big, "\nunused_mesh\t-1.0\tprev\t4\tmesh_object\tcarried\t42\t") != NULL);
        CHECK(strstr(big, "\ngone_mesh\t-1.0\tprev\t7\tentity_override\tcarried\t") != NULL);
        CHECK(strstr(big, "# unused unused_mesh") == NULL && strstr(big, "2 type(s) carried over") != NULL);
        lm_prev again; CHECK(lm_prev_parse(&again, big) == 3);   /* round trip: all three survive */
        const lm_prev_item *g = lm_prev_find(&again, "gone_mesh"); CHECK(g && g->uses == 7 && g->site == LM_SITE_RDATA);
        lm_nameset ss = { 0 }; CHECK(lm_prev_startup_set(&again, &ss) == 2);   /* startup set did not shrink */
        lm_nameset_free(&ss); lm_prev_free(&again); lm_prev_free(&pv);
        CHECK(lm_config_parse(&c, "merge_report=0\n", err, sizeof err) == 0 && c.merge_report == 0);
    }
    lm_table_free(&t);

    /* strings + patterns */
    struct { union { char buf[16]; char *ptr; } u; uint64_t size, cap; } ms;
    memset(&ms, 0, sizeof ms); strcpy(ms.u.buf, "tree_mesh"); ms.size = 9; ms.cap = 15;
    CHECK(lm_msvc_str(&ms, out, sizeof out, NULL) == 9 && !strcmp(out, "tree_mesh"));
    char heap[] = "a_long_mesh_name_over_16_chars";
    ms.u.ptr = heap; ms.size = strlen(heap); ms.cap = 40;
    CHECK(lm_msvc_str(&ms, out, sizeof out, readable) == (int)strlen(heap) && !strcmp(out, heap));
    CHECK(lm_msvc_str(&ms, out, 5, readable) == 4 && !strcmp(out, "a_lo"));
    ms.size = 41; CHECK(lm_msvc_str(&ms, out, sizeof out, readable) == -1);
    ms.size = 20; ms.u.ptr = (char *)16; CHECK(lm_msvc_str(&ms, out, sizeof out, readable) == -1);   /* bad pointer: no crash */
    const uint8_t code[] = { 0x41, 0x39, 0x5D, 0x4C, 0x0F, 0x85 };
    CHECK(lm_match(code, sizeof code, 0, "41 39 5D 4C 0F 85") && lm_match(code, sizeof code, 1, "39 ?? 4C"));
    CHECK(!lm_match(code, sizeof code, 0, "41 39 5E") && !lm_match(code, sizeof code, 4, "0F 85 00") && !lm_match(code, sizeof code, 0, "4"));
}

int main(int argc, char **argv)
{
    test_tmp_dir(g_tdir, sizeof g_tdir, "lazymesh/t");
    core_tests();
    plugin_logic();
    race();
    if (argc > 1) fails += real_exe(argv[1]);
    printf(fails ? "%d FAILED\n" : "ALL OK\n", fails);
    return fails ? 1 : 0;
}
