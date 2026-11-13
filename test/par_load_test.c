/* Offline test of plugins/par_load (no game needed).
 *   python -m ziglang cc -target x86_64-windows-gnu -O2 -Iinclude -Isdk -o D:/v3x_tmp/fastload2/plt.exe test/par_load_test.c
 *   D:/v3x_tmp/fastload2/plt.exe                       -> core tests, ALL OK
 *   D:/v3x_tmp/fastload2/plt.exe ../binaries/victoria3.exe   also resolves every anchor on the real exe
 * Covers: mode parsing, step/inner attribution, the generated database thunks (argument
 * forwarding, return value, unwind info = stack walks pass through them), the db_loop pre/post
 * patching on a fake ready-loop (pointers restored, names, order, per-slot records), ordered
 * fork/join determinism (serial == parallel merged output on fake files, 30 rounds), anchors. */
#include "v3x_testkit.h"
#include "../plugins/par_load/pl_core.c"
#include "../plugins/par_load/par_load.c"

/* ---------------------------------------------------------------- thunk */
static uint64_t seen[4];
static int walk_ok;
static void *g_thunk_lo, *g_caller_lo, *g_caller_hi;
static __attribute__((noinline)) uint64_t fake_cb(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    seen[0] = a0; seen[1] = a1; seen[2] = a2; seen[3] = a3;
    /* walk like crash_logger does (RtlLookupFunctionEntry + RtlVirtualUnwind; dynamic function tables
     * are honoured). RtlCaptureStackBackTrace stops at the thunk: it does not unwind code outside images. */
    void *fr[32]; int n = 0;
    CONTEXT ctx; RtlCaptureContext(&ctx);
    while (n < 32 && ctx.Rip) {
        fr[n++] = (void *)(uintptr_t)ctx.Rip;
        DWORD64 base = 0; PVOID hd; DWORD64 est;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(ctx.Rip, &base, NULL);
        if (!fe) { ctx.Rip = *(DWORD64 *)(uintptr_t)ctx.Rsp; ctx.Rsp += 8; continue; }
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fe, &ctx, &hd, &est, NULL);
    }
    int in_thunk = 0, in_caller = 0;
    for (int i = 0; i < n; i++) {
        if ((uint8_t *)fr[i] >= (uint8_t *)g_thunk_lo && (uint8_t *)fr[i] < (uint8_t *)g_thunk_lo + PL_THUNK_STRIDE) {
            in_thunk = 1;
            /* the frame above the thunk must be the test exe (call_thunk or main): unwinding passed it */
            in_caller = i + 1 < n && (uint8_t *)fr[i + 1] >= (uint8_t *)g_caller_lo && (uint8_t *)fr[i + 1] < (uint8_t *)g_caller_hi;
        }
    }
    walk_ok = in_thunk && in_caller;
    return 0x1234567890ull;
}
typedef uint64_t (*call4)(uint64_t, uint64_t, uint64_t, uint64_t);
static __attribute__((noinline)) uint64_t call_thunk(void *th) { return ((call4)th)(111, 222, 333, 444); }

/* ---------------------------------------------------------------- fake ready-loop */
static int order_log[16], order_n;
static void cb_a0(void) { order_log[order_n++] = 0; }
static void cb_b0(void) { order_log[order_n++] = 1; Sleep(2); }
static void cb_a1(void) { order_log[order_n++] = 2; }
static void cb_b1(void) { order_log[order_n++] = 3; }
static void cb_c1(void) { order_log[order_n++] = 4; }
typedef struct { const char *name; void *a, *ready, *dtor, *b, *c, *pad; } entry_t;   /* 0x38 bytes */
typedef struct { entry_t *data; int cap, count; } vec_t;

/* ---------------------------------------------------------------- fork/join */
#define NF 240
static char *files[NF];
typedef struct { uint32_t h[512]; int n; } res_t;
static res_t res[NF];
static char outbuf[1 << 20]; static size_t outn;
static uint32_t keys[NF * 512]; static int nkeys, ndup;
static void tokenize(int i, void *ctx)
{
    (void)ctx;
    res_t *r = &res[i]; r->n = 0;
    const char *p = files[i];
    if ((i * 7919) % 5 == 0) Sleep(1);                     /* scramble the completion order */
    while (*p && r->n < 512) {
        while (*p == ' ' || *p == '\n') p++;
        if (!*p) break;
        uint32_t h = 2166136261u;
        if (*p == '{' || *p == '}' || *p == '=') h = (h ^ (uint8_t)*p++) * 16777619u;
        else while (*p && *p != ' ' && *p != '\n' && *p != '{' && *p != '}' && *p != '=') h = (h ^ (uint8_t)*p++) * 16777619u;
        r->h[r->n++] = h;
    }
}
static void merge(int i, void *ctx)
{
    (void)ctx;
    /* "register" keys in order: the first definition wins, later ones are duplicates (engine-like) */
    for (int k = 0; k < res[i].n; k++) {
        uint32_t h = res[i].h[k]; int dup = 0;
        for (int j = nkeys - 1; j >= 0 && j >= nkeys - 64; j--) dup |= keys[j] == h;
        if (dup) ndup++; else keys[nkeys++] = h;
        outn += (size_t)snprintf(outbuf + outn, sizeof outbuf - outn, "%d:%08x%s;", i, h, dup ? "d" : "");
    }
}
static uint64_t run_fj(int threads, int *started)
{
    outn = 0; nkeys = 0; ndup = 0; memset(res, 0, sizeof res);
    *started = pl_forjoin(NF, threads, tokenize, merge, NULL);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < outn; i++) h = (h ^ (uint8_t)outbuf[i]) * 1099511628211ull;
    return h ^ (uint64_t)ndup << 1;
}

static long long fake_t;
static long long fake_now(void) { return fake_t += 10; }

int main(int argc, char **argv)
{
    V3X_API tk;
    /* mode */
    V3X_TEST_CHECK(pl_mode_parse("Measure") == PL_MODE_MEASURE && pl_mode_parse("on") == PL_MODE_ON &&
                   pl_mode_parse("off") == PL_MODE_OFF && pl_mode_parse("fast") == -1, "mode parsing");

    /* step + inner attribution */
    pl_stats s; pl_stats_init(&s, 1000);
    int a = pl_step_add(&s, "init", "", 0), b = pl_step_add(&s, "asset", "", 1), c = pl_step_add(&s, "gui", "", 1);
    s.init_tid = 7;
    pl_step_enter(&s, a, 7); pl_step_enter(&s, b, 7);
    pl_inner_add(&s, PL_IN_VFS, 7, 5);      /* init thread inside asset: counts for init + asset */
    pl_inner_add(&s, PL_IN_VFS, 9, 50);     /* other thread: only the thread totals */
    pl_step_leave(&s, b, 7, 100);
    pl_step_enter(&s, c, 7); pl_inner_add(&s, PL_IN_LEXER, 7, 3); pl_step_leave(&s, c, 7, 40);
    pl_step_leave(&s, a, 7, 300);
    V3X_TEST_CHECK(s.steps[a].in_ticks[PL_IN_VFS] == 5 && s.steps[b].in_ticks[PL_IN_VFS] == 5 && s.steps[c].in_ticks[PL_IN_VFS] == 0,
                   "vfs time attributed to the active init-thread steps only");
    V3X_TEST_CHECK(s.in_ticks_thr[PL_IN_VFS][0] == 5 && s.in_ticks_thr[PL_IN_VFS][1] == 50 && s.steps[c].in_ticks[PL_IN_LEXER] == 3,
                   "thread split: init 5, other 50; lexer in gui 3");
    V3X_TEST_CHECK(s.depth == 0 && s.steps[a].ticks == 300 && s.steps[b].max_ticks == 100, "stack balanced, ticks/max");

    /* path set */
    {
        static pl_pathset ps;
        uint64_t h1 = pl_hash_path("gfx/Models/A.mesh", 17), h2 = pl_hash_path("GFX\\models\\a.MESH", 17);
        V3X_TEST_CHECK(h1 == h2, "path hash: case and slash insensitive");
        int r1 = pl_pathset_add(&ps, h1), r2 = pl_pathset_add(&ps, h2), r3 = pl_pathset_add(&ps, pl_hash_path("b", 1));
        for (int i = 0; i < 50000; i++) { char b[32]; int n = snprintf(b, sizeof b, "p%d", i % 20000); pl_pathset_add(&ps, pl_hash_path(b, (size_t)n)); }
        V3X_TEST_CHECK(r1 == 1 && r2 == 0 && r3 == 1 && ps.total == 50003 && ps.distinct == 20002 && ps.full == 0,
                       "path set: 50003 adds, 20002 distinct (got %lld/%lld)", ps.total, ps.distinct);
    }

    /* thunk */
    static pl_dbtab tt; memset(&tt, 0, sizeof tt);
    pl_db_bind(&tt);
    uint8_t *pool = pl_thunk_pool_create(PL_DB_MAX, (const void *)pl_db_dispatch);
    V3X_TEST_CHECK(pool != NULL && pl_thunk_pool_size() == PL_DB_MAX, "thunk pool + RtlAddFunctionTable");
    if (pool) {
        uint8_t ref[PL_THUNK_STRIDE]; pl_thunk_emit(ref, 5, (const void *)pl_db_dispatch);
        V3X_TEST_CHECK(!memcmp(ref, pl_thunk_at(5), PL_THUNK_STRIDE) && ref[0] == 0x48 && ref[25] == 0xC3, "thunk bytes");
        DWORD64 ib; V3X_TEST_CHECK(RtlLookupFunctionEntry((DWORD64)(uintptr_t)pl_thunk_at(3) + 9, &ib, NULL) != NULL, "unwind entry found");
        tt.rec[3].orig = (void *)fake_cb; tt.nrec = 4;
        g_thunk_lo = pl_thunk_at(3);
        {   /* the test exe image */
            uint8_t *mb = (uint8_t *)GetModuleHandleA(NULL);
            IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(mb + ((IMAGE_DOS_HEADER *)mb)->e_lfanew);
            g_caller_lo = mb; g_caller_hi = mb + nt->OptionalHeader.SizeOfImage;
        }
        uint64_t r = call_thunk(pl_thunk_at(3));
        V3X_TEST_CHECK(r == 0x1234567890ull && seen[0] == 0 && seen[1] == 222 && seen[2] == 333 && seen[3] == 444,
                       "thunk forwards rdx/r8/r9 + return value (rcx = 0)");
        V3X_TEST_CHECK(tt.rec[3].calls == 1 && tt.rec[3].order == 1, "thunk records call + order");
        V3X_TEST_CHECK(walk_ok, "RtlVirtualUnwind walk from the callback passes the thunk and reaches its caller");
    }

    /* db_loop pre/post on a fake vector, with the test exe itself as the "game image" */
    v3x_testkit_use_image((uint8_t *)GetModuleHandleA(NULL));
    v3x_testkit_api(&tk); api = &tk;
    QueryPerformanceFrequency(&g_freq);
    setup_steps();
    pl_thunk_pool_create(PL_DB_MAX, (const void *)pl_db_dispatch);   /* already exists: reused */
    entry_t E[2] = {{"CFirstDatabase", (void *)cb_a0, NULL, NULL, (void *)cb_b0, NULL, NULL},
                    {"CSecondDatabase", (void *)cb_a1, NULL, NULL, (void *)cb_b1, (void *)cb_c1, NULL}};
    vec_t v = {E, 2, 2};
    V3X_HookCall hc; memset(&hc, 0, sizeof hc);
    hc.args[0] = (uint64_t)(uintptr_t)&v; hc.user = (void *)(intptr_t)A_DBLOOP;
    db_pre(&hc);
    int patched = E[0].a != (void *)cb_a0 && E[0].b != (void *)cb_b0 && E[1].c != (void *)cb_c1 && E[0].c == NULL;
    V3X_TEST_CHECK(patched && T.nrec == 5, "5 callbacks patched to thunks (NULL slot left alone)");
    for (int i = 0; i < 2; i++) { ((void (*)(void))E[i].a)(); ((void (*)(void))E[i].b)(); }
    for (int i = 0; i < 2; i++) if (E[i].c) ((void (*)(void))E[i].c)();
    db_post(&hc);
    V3X_TEST_CHECK(E[0].a == (void *)cb_a0 && E[0].b == (void *)cb_b0 && E[1].a == (void *)cb_a1 && E[1].c == (void *)cb_c1,
                   "engine pointers restored after the loop");
    V3X_TEST_CHECK(order_n == 5 && order_log[0] == 0 && order_log[1] == 1 && order_log[4] == 4, "callbacks ran in engine order");
    V3X_TEST_CHECK(!strcmp(T.rec[0].name, "CFirstDatabase") && T.rec[1].slot == PL_DB_SLOT_B && T.rec[4].slot == PL_DB_SLOT_C &&
                   T.rec[4].order == 5 && T.rec[1].ticks > 0 && T.nlists == 1, "records: names, slots, order, time");
    V3X_TEST_CHECK(S.steps[g_step_of[A_DBLOOP]].calls == 1, "db_loop step timed");
    {
        char path[MAX_PATH]; snprintf(path, sizeof path, "%s\\plt_report.txt", getenv("TEMP") ? getenv("TEMP") : ".");
        FILE *f = fopen(path, "w");
        if (f) { pl_report(f, &S, &T, &P, "test", 10); fclose(f); }
        f = fopen(path, "r"); char buf[8192] = {0};
        if (f) { fread(buf, 1, sizeof buf - 1, f); fclose(f); }
        V3X_TEST_CHECK(strstr(buf, "CFirstDatabase") && strstr(buf, "2 databases") && strstr(buf, "db_loop"), "report lists databases");
    }

    /* fork/join determinism */
    for (int i = 0; i < NF; i++) {
        files[i] = malloc(4096); size_t n = 0;
        for (int k = 0; k < 40 + i % 37; k++)
            n += (size_t)snprintf(files[i] + n, 4096 - n, "key_%d = { value_%d %d }\n", (i * 31 + k * 7) % 300, k, i);
    }
    int st = 0;
    uint64_t ref = run_fj(1, &st);
    int same = 1, any_par = 0;
    for (int round = 0; round < 30; round++) { uint64_t h = run_fj(2 + round % 11, &st); same &= h == ref; any_par |= st > 0; }
    V3X_TEST_CHECK(same && any_par, "fork/join: 30 parallel runs (2..12 threads) == serial merged output");
    V3X_TEST_CHECK(pl_forjoin(0, 4, tokenize, merge, NULL) == 0, "fork/join: empty input");

    /* blend-shape prefetch: recorder roots, save/load, identical bytes, window, engine accounting */
    {
        static pl_pf rec, ld, run;
        char dir[MAX_PATH], path[MAX_PATH];
        snprintf(dir, sizeof dir, "%s\\plt_pf", getenv("TEMP") ? getenv("TEMP") : ".");
        CreateDirectoryA(dir, NULL);
        char sub[MAX_PATH]; snprintf(sub, sizeof sub, "%s\\gfx", dir); CreateDirectoryA(sub, NULL);
        enum { NPF = 60 };
        static uint8_t data[NPF][3 << 19];
        size_t len[NPF];
        uint64_t want = 1469598103934665603ull;
        pl_pf_reset(&rec);
        srand(7);
        for (int i = 0; i < NPF; i++) {
            len[i] = i == 3 ? 0 : (size_t)(rand() % 4096) * (i % 7 == 0 ? 300 : 3) + (size_t)(i % 5);
            if (len[i] > sizeof data[0]) len[i] = sizeof data[0];
            for (size_t k = 0; k < len[i]; k++) data[i][k] = (uint8_t)rand();
            char vfs[64]; snprintf(vfs, sizeof vfs, "gfx/f%02d.mesh", i);
            snprintf(path, sizeof path, "%s\\gfx\\f%02d.mesh", dir, i);
            FILE *f = fopen(path, "wb"); if (f) { fwrite(data[i], 1, len[i], f); fclose(f); }
            want = pl_fnv_bytes(want, data[i], len[i]);
            /* engine order of one blend call: base read, root check, blend read; even = base, odd = blend */
            pl_pf_rec_read(&rec, vfs, strlen(vfs));
            if (i % 2 == 0) { char r[MAX_PATH]; snprintf(r, sizeof r, "%s%s", dir, i % 4 ? "" : "\\"); pl_pf_rec_root(&rec, r); }
        }
        snprintf(path, sizeof path, "%s\\list.txt", dir);
        int resolved = 0;
        V3X_TEST_CHECK(pl_pf_save(&rec, path, &resolved) == NPF && resolved == NPF, "recorder: %d/%d native paths resolved", resolved, NPF);
        pl_pf_reset(&ld);
        V3X_TEST_CHECK(pl_pf_load(&ld, path) == NPF && ld.e[5].size == (long long)len[5] && ld.e[3].size == 0 &&
                       ld.e[9].vfs_hash == pl_hash_path("gfx/f09.mesh", 12), "list save/load round trip");
        /* run: the engine advances slowly; prefetch must stay inside the window and read every byte */
        run = ld;                                   /* shallow copy: same strings */
        run.window = 1 << 20;
        HANDLE th = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)(void *)pl_pf_run, &run, 0, NULL);
        long long worst = 0;
        for (long k = 0; k < NPF; k++) {
            Sleep(3);
            long long engine_bytes = 0, maxsz = 0;
            for (long j = 0; j < k; j++) engine_bytes += run.e[j].size;
            for (long j = 0; j < NPF; j++) if (run.e[j].size > maxsz) maxsz = run.e[j].size;
            long long ahead = run.bytes_read - engine_bytes - maxsz;
            if (ahead > worst) worst = ahead;
            pl_pf_engine_read(&run, k, run.e[k].vfs_hash);
        }
        pl_pf_engine_read(&run, NPF, 1);            /* a read the list does not have */
        WaitForSingleObject(th, 10000); CloseHandle(th);
        V3X_TEST_CHECK(run.files_read + run.files_failed + (long long)0 <= NPF && run.files_failed == 0, "prefetch: no failures");
        V3X_TEST_CHECK(worst <= run.window, "prefetch stays within the window (worst excess %lld)", worst);
        V3X_TEST_CHECK(run.hit + run.inflight + run.miss == NPF && run.beyond == 1 && run.mismatch == 0 && run.hit > NPF / 2,
                       "engine accounting: %lld hit %lld inflight %lld miss, 1 beyond", run.hit, run.inflight, run.miss);
        /* identical bytes: everything the prefetcher read == the files' content, in order; files unchanged */
        static pl_pf full; full = ld; full.window = 1ll << 40;
        pl_pf_run(&full);
        V3X_TEST_CHECK((uint64_t)full.content_hash == want && full.files_read == NPF, "prefetch read exactly the file bytes (hash match)");
        int same = 1;
        static uint8_t back[3 << 19];
        for (int i = 0; i < NPF; i++) {
            snprintf(path, sizeof path, "%s\\gfx\\f%02d.mesh", dir, i);
            FILE *f = fopen(path, "rb"); size_t n = f ? fread(back, 1, sizeof back, f) : (size_t)-1; if (f) fclose(f);
            same &= n == len[i] && !memcmp(back, data[i], len[i]);
        }
        V3X_TEST_CHECK(same, "files byte-identical after prefetch (an engine read sees the same bytes)");
        /* the engine already past an entry: it is skipped, not read */
        static pl_pf late; late = ld; late.engine_pos = NPF; late.window = 1 << 20;
        V3X_TEST_CHECK(pl_pf_run(&late) == 0, "entries the engine already passed are not read");
        static pl_pf stopped; stopped = ld; stopped.stop = 1;
        V3X_TEST_CHECK(pl_pf_run(&stopped) == 0, "stop flag honoured");
    }

    /* the real exe */
    if (argc > 1) {
        if (v3x_testkit_map_exe(argv[1], &tk) == 0) {
            api = &tk;
            int ok = pl_resolve();
            static const unsigned long long WANT[A_N] = {0x6aff80, 0x3ad4300, 0x39247e0, 0x38a3b20, 0x3794de0, 0x6b21a0,
                                                         0x2dd29c0, 0x1021ff0, 0x3dbb980, 0x3e1c4f0, 0x3e1e490, 0x3ab67c0, 0x3dd1190,
                                                         0x3dffd50};
            for (int i = 0; i < A_N; i++)
                V3X_TEST_CHECK(v3x_testkit_rva(g_fn[i]) == WANT[i], "anchor %-12s -> 0x%llx (want 0x%llx) %s", A_NAME[i],
                               v3x_testkit_rva(g_fn[i]), WANT[i], g_err[i]);
            V3X_TEST_CHECK(ok == A_N, "%d/%d anchors", ok, A_N);
            V3X_TEST_CHECK(v3x_testkit_rva(g_rd_ret) == 0x3ab484d, "getRealDir return site in the blend loader 0x%llx",
                           v3x_testkit_rva(g_rd_ret));
        } else V3X_TEST_CHECK(0, "map %s", argv[1]);
    }
    (void)fake_now;
    return v3x_testkit_done();
}
