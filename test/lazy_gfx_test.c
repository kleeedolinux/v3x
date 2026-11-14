/* Offline test of plugins/lazy_gfx (no game needed).
 *   (normally run by test/run_all.sh; scratch root = $V3X_TMP, else %TEMP%, see test/test_tmp.h)
 *   python -m ziglang cc -target x86_64-windows-gnu -O2 -Iinclude -Isdk -o $V3X_TMP/lgt.exe test/lazy_gfx_test.c
 *   $V3X_TMP/lgt.exe                               -> ALL OK (writes only under $V3X_TMP/lazygfx/t*)
 *   $V3X_TMP/lgt.exe ../binaries/victoria3.exe     also maps the real exe (no code runs): resolves all
 *                                                            anchors with the plugin's own lg_resolve(), checks the
 *                                                            1.15.1 RVAs, fail-closed on a broken byte, and runs
 *                                                            v3x_plugin_load end to end (7 hook_safe calls)
 * Covers: config parse + example ini, ext/hash, the fake texture-request path (a fake GetOrLoadTexture ->
 * CreateTexture that fires the hooked stage functions in the engine's order with the engine's call-site return
 * addresses: opts, getRealDir, VFSOpen-returned, decode, pixel buffer, object + stamp), stage attribution,
 * foreign callers ignored, missing file = incomplete record, worker thread, phases, report + CSV, manifest
 * (unique, real files, size+mtime), prefetch (reads, skip on change, prefetched flag), and the processed-texture
 * cache with fake DDS/FreeImage decoders (layout learning, build, verify, on = DDS fed to decode, mismatch ->
 * entry deleted + cache off, contradicting DDS keeps it inactive, stale key). */
#include "../plugins/lazy_gfx/lg_core.c"
#include "../plugins/lazy_gfx/lg_cache.c"
#include "../plugins/lazy_gfx/lazy_gfx.c"
#include "v3x_testkit.h"
#include <stdarg.h>
#include "test_tmp.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static char g_tdir[MAX_PATH], g_tdir2[MAX_PATH];   /* <scratch>/lazygfx/t/ and t2/, set in main */
#define TDIR g_tdir

/* ------------------------------------------------------------------ fake API */
static void x_log(const char *tag, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); printf("[%s] ", tag); vprintf(fmt, ap); putchar('\n'); va_end(ap); }
static int x_print(const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int n = vprintf(fmt, ap); putchar('\n'); va_end(ap); return n; }
static const char *t_dir(void) { return TDIR; }
static uintptr_t t_base(void) { return 0x10000; }
static size_t t_size(void) { return 0x1000; }
static uint32_t t_main(void) { return GetCurrentThreadId(); }
static V3X_API fake;

/* ------------------------------------------------------------------ fake engine */
typedef struct { void *opaque; const char *dirName; } fake_dirhandle;
static char g_root_mod[MAX_PATH], g_root_game[MAX_PATH];
static fake_dirhandle g_dh_mod, g_dh_game;
static uint8_t g_fake_ret[H_N];
static int g_sleep_in;                      /* ms to spend inside the next hooked call */             /* g_a.ret[h] = &g_fake_ret[h] */

static void hook_call(int h, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t ret, int with_post, const void *retaddr)
{
    V3X_HookCall c; memset(&c, 0, sizeof c);
    c.args[0] = a0; c.args[1] = a1; c.args[2] = a2; c.args[3] = a3;
    c.user = (void *)(intptr_t)h;
    c.return_address = (void *)(retaddr ? retaddr : g_a.ret[h]);
    pre(&c);
    if (g_sleep_in) { Sleep((DWORD)g_sleep_in); g_sleep_in = 0; }
    c.ret = ret;
    if (with_post) post(&c);
}

typedef struct { const char *p; int len; int pad; } view_t;

/* fake CreateTexture: fires the hooked callees in engine order. root: 0 = mod, 1 = game, -1 = not found */
static void fake_create(const char *path, int root, uint32_t size, int decode_ms)
{
    view_t v = { path, (int)strlen(path), 0 };
    hook_call(H_OPTS, 0, (uint64_t)&v, 0, 0, 0, 1, NULL);
    fake_dirhandle *dh = root == 0 ? &g_dh_mod : root == 1 ? &g_dh_game : NULL;
    g_sleep_in = 2;                                              /* "probing" inside getRealDir */
    hook_call(H_RDIR, (uint64_t)&v, 0, 0, 0, (uint64_t)dh, 1, NULL);
    hook_call(H_MOVE, 0, 0, 0, 0, 0, 1, NULL);
    if (!dh) return;                                             /* engine: "Could not find texture" path, no object */
    hook_call(H_DEC, 0, (uint64_t)&v, 0, size, 0, 0, NULL);      /* pre only, then the decode work, then post */
    Sleep((DWORD)decode_ms);
    { V3X_HookCall c; memset(&c, 0, sizeof c); c.user = (void *)(intptr_t)H_DEC; c.return_address = g_a.ret[H_DEC]; c.ret = 1; post(&c); }
    hook_call(H_PIX, 0, 0, 0, 0, 0, 1, NULL);
    void *tex = (void *)0x1234;
    hook_call(H_OBJ, 0, (uint64_t)&tex, 0, 0, 0, 0, NULL);
    hook_call(H_STAMP, 0, 0, 0, 0, 0, 1, NULL);
    { V3X_HookCall c; memset(&c, 0, sizeof c); c.user = (void *)(intptr_t)H_OBJ; c.return_address = g_a.ret[H_OBJ];
      c.args[1] = (uint64_t)&tex; post(&c); }
}

static void write_file(const char *path, size_t n)
{
    char dir[MAX_PATH]; snprintf(dir, sizeof dir, "%s", path);
    for (char *q = dir + 3; *q; q++) if (*q == '/') { *q = 0; CreateDirectoryA(dir, NULL); *q = '/'; }
    FILE *f = fopen(path, "wb");
    if (!f) { printf("cannot write %s\n", path); fails++; return; }
    for (size_t i = 0; i < n; i++) fputc((int)(i & 0xff), f);
    fclose(f);
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    size_t r = fread(b, 1, (size_t)n, f); b[r] = 0; fclose(f);
    return b;
}

static int count_lines(const char *s, const char *needle)
{
    int n = 0;
    for (const char *p = s; p && (p = strstr(p, needle)); p++) n++;
    return n;
}

static DWORD WINAPI worker_req(void *u) { (void)u; fake_create("gfx/models/x_diffuse.dds", 1, 4096, 0); return 0; }

static void setup(const char *ini)
{
    memset(&fake, 0, sizeof fake);
    fake.api_version = V3X_API_VERSION; fake.struct_size = sizeof fake;
    fake.log = x_log; fake.print = x_print; fake.v3x_dir = t_dir; fake.image_base = t_base; fake.image_size = t_size;
    fake.main_thread_id = t_main;
    api = &fake;
    snprintf(g_dir, sizeof g_dir, "%s", TDIR);
    lg_config c; lg_config_defaults(&c);
    char err[160];
    CHECK(lg_config_parse(&c, ini, err, sizeof err) == 0);
    lg_free(&g_st);
    CHECK(lg_init(&g_st, &c) == 0);
    g_log_left = 100;
    g_main_tid = GetCurrentThreadId();
    if (g_tls == TLS_OUT_OF_INDEXES) g_tls = TlsAlloc();
    tls_t *t = TlsGetValue(g_tls); if (t) memset(t, 0, sizeof *t), t->ridx = 255;
    memset(&g_a, 0, sizeof g_a);
    for (int i = 0; i < H_N; i++) g_a.ret[i] = &g_fake_ret[i];
    g_manifest_done = 0;
    lg_pf_free(&g_pf);
}

static void core_tests(void)
{
    lg_config c, d; lg_config_defaults(&c); lg_config_defaults(&d);
    char err[160];
    CHECK(c.mode == LG_MEASURE && c.csv == 1 && c.prefetch_threads == 4);
    CHECK(lg_config_parse(&c, "mode = on # x\nprefetch_threads=99\n; c\n[s]\nbogus=1\nslow_log_ms=abc\nmode=sometimes\n", err, sizeof err) == 3);
    CHECK(c.mode == LG_ON && c.prefetch_threads == 16);
    CHECK(strstr(err, "line 5") != NULL);
    CHECK(lg_config_parse(&c, "mode=off", err, sizeof err) == 0 && c.mode == LG_OFF);
    char *ex = slurp("plugins/lazy_gfx/lazy_gfx.ini.example");
    CHECK(ex != NULL);
    if (ex) {
        lg_config e; lg_config_defaults(&e);
        CHECK(lg_config_parse(&e, ex, err, sizeof err) == 0);
        CHECK(!memcmp(&e, &d, sizeof e));                       /* example == built-in defaults */
        free(ex);
    }
    CHECK(lg_ext_of("gfx/a/b.DDS", 11) == 0 && lg_ext_of("x.png", 5) == 1 && lg_ext_of("x.jpeg", 6) == 2);
    CHECK(lg_ext_of("x.tga", 5) == 3 && lg_ext_of("gfx.d/x", 7) == 4 && lg_ext_of("noext", 5) == 4);
    CHECK(lg_hash("GFX\\A.dds", 9) == lg_hash("gfx/a.DDS", 9) && lg_hash("", 0) != 0);
}

static void request_path_tests(void)
{
    setup("mode=measure\nslow_log_ms=1\n");
    snprintf(g_root_mod, sizeof g_root_mod, "%sroot_mod", TDIR);
    snprintf(g_root_game, sizeof g_root_game, "%sroot_game/", TDIR);     /* with and without trailing slash */
    g_dh_mod.dirName = g_root_mod; g_dh_game.dirName = g_root_game;
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s/gfx/interface/sakuya/a.png", g_root_mod); write_file(p, 3000);
    snprintf(p, sizeof p, "%sgfx/interface/icons/b.dds", g_root_game); write_file(p, 5000);
    snprintf(p, sizeof p, "%sgfx/models/x_diffuse.dds", g_root_game); write_file(p, 4096);

    /* main-menu style requests on the main thread */
    fake_create("gfx/interface/sakuya/a.png", 0, 3000, 12);
    fake_create("gfx/interface/icons/b.dds", 1, 5000, 0);
    fake_create("gfx/interface/icons/b.dds", 1, 5000, 0);           /* re-read: one manifest line only */
    fake_create("gfx/interface/missing.dds", -1, 0, 0);             /* not found: finished by the next request */
    /* a foreign caller of the hooked functions (wrong return address) must be ignored */
    view_t v = { "gfx/other.txt", 13, 0 };
    hook_call(H_OPTS, 0, (uint64_t)&v, 0, 0, 0, 1, (void *)0x777);
    hook_call(H_DEC, 0, 0, 0, 99, 0, 1, (void *)0x777);
    CHECK(g_st.nrecs == 3);
    fake_create("gfx/interface/icons/b.dds", 1, 5000, 0);
    CHECK(g_st.nrecs == 5);                                          /* + the incomplete missing.dds + this one */
    HANDLE th = CreateThread(NULL, 0, worker_req, NULL, 0, NULL);
    WaitForSingleObject(th, INFINITE); CloseHandle(th);
    CHECK(g_st.nrecs == 6);

    const lg_rec *a = &g_st.recs[0], *miss = &g_st.recs[3], *w = &g_st.recs[5];
    CHECK(!strcmp(a->path, "gfx/interface/sakuya/a.png") && a->ext == 1 && a->bytes == 3000 && a->main && a->ok && a->complete);
    CHECK(a->st[LG_ST_DECODE] >= 10.0f && a->st[LG_ST_DECODE] <= a->total_ms + 0.01);
    CHECK(a->st[LG_ST_STAT] >= 1.0f && a->st[LG_ST_OPTS] < a->st[LG_ST_STAT]);   /* the sleep inside getRealDir */
    float sum = 0; for (int i = 0; i < LG_ST_N; i++) sum += a->st[i];
    CHECK(sum <= a->total_ms + 0.05 && sum >= a->total_ms - 0.5);    /* stages add up to the total */
    CHECK(a->root < g_st.nroots && !strcmp(g_st.roots[a->root], g_root_mod));
    CHECK(g_st.recs[1].root < g_st.nroots && !strcmp(g_st.roots[g_st.recs[1].root], g_root_game) && g_st.nroots == 2);
    CHECK(!strcmp(miss->path, "gfx/interface/missing.dds") && !miss->complete && !miss->ok && miss->root == 255);
    CHECK(!w->main && w->tid != g_main_tid && w->ext == 0 && w->complete);
    CHECK(g_st.agg[0][1][1].n == 1 && g_st.agg[0][1][0].n == 4 && g_st.agg[0][1][0].failed == 1 && g_st.agg[0][0][0].n == 1);

    /* extract_req: frames above CreateTexture's call-site return address, exe image only */
    void *fr[6] = { (void *)0x5, g_a.ret[H_OPTS], (void *)0x10010, (void *)0x99999999, (void *)0x10020, (void *)0x10030 };
    uint32_t req[LG_FRAMES] = { 0 };
    CHECK(extract_req(fr, 6, g_a.ret[H_OPTS], req) == 3 && req[0] == 0x10 && req[1] == 0x20 && req[2] == 0x30);
    CHECK(extract_req(fr, 6, (void *)0x4242, req) == 0);

    /* phases + manifest when the first Game load begins */
    V3X_LoadTransition tr = { "Empty", "Game", 0 };
    on_begin("load_transition_begin", &tr, NULL);
    CHECK(g_st.cur_phase == 1 && !strcmp(g_st.phases[1], "load:Game"));
    fake_create("gfx/interface/sakuya/a.png", 0, 3000, 0);
    CHECK(g_st.recs[6].phase == 1);
    on_end("load_transition", &tr, NULL);
    CHECK(!strcmp(g_st.phases[g_st.cur_phase], "Game"));
    char mp[MAX_PATH]; snprintf(mp, sizeof mp, "%slazy_gfx_prefetch.txt", TDIR);
    char *m = slurp(mp);
    CHECK(m != NULL);
    if (m) {
        CHECK(count_lines(m, "\tgfx/") == 3);                        /* a.png + b.dds + the worker's x_diffuse; not missing */
        CHECK(strstr(m, "root_mod/gfx/interface/sakuya/a.png") && strstr(m, "root_game/gfx/interface/icons/b.dds"));
        CHECK(strstr(m, "3000\t") && strstr(m, "5000\t") && strstr(m, "root_game/gfx/models/x_diffuse.dds") && !strstr(m, "missing"));
        free(m);
    }
    char rp[MAX_PATH]; snprintf(rp, sizeof rp, "%slazy_gfx_report.txt", TDIR);
    char *r = slurp(rp);
    CHECK(r && strstr(r, "texture loads: 7") && strstr(r, "share of total load time") && strstr(r, "gfx/interface/sakuya/a.png"));
    free(r);
    snprintf(rp, sizeof rp, "%slazy_gfx_textures.csv", TDIR);
    r = slurp(rp);
    CHECK(r && !strncmp(r, "t_ms,phase,tid,main,ext,bytes", 29) && count_lines(r, "\n") == 8 && strstr(r, ",png,3000,1,1,"));
    free(r);

    /* prefetch: read the manifest's files on 2 threads; a changed file is skipped */
    CHECK(lg_pf_load(&g_pf, mp, 0) == 3);
    snprintf(p, sizeof p, "%sgfx/interface/icons/b.dds", g_root_game); write_file(p, 5001);
    CHECK(lg_pf_run(&g_pf, 2, 0) == 2);
    CHECK(g_pf.done == 2 && g_pf.skipped == 1 && g_pf.bytes_read == 3000 + 4096);
    CHECK(lg_pf_was_read(&g_pf, lg_hash("gfx/interface/sakuya/a.png", 26)) && !lg_pf_was_read(&g_pf, lg_hash("gfx/interface/icons/b.dds", 25)));
    fake_create("gfx/interface/sakuya/a.png", 0, 3000, 0);
    fake_create("gfx/interface/icons/b.dds", 1, 5001, 0);
    CHECK(g_st.recs[7].prefetched == 1 && g_st.recs[8].prefetched == 0);
    /* byte budget: max_bytes reached -> the rest is skipped */
    lg_pf_free(&g_pf);
    snprintf(p, sizeof p, "%sgfx/interface/icons/b.dds", g_root_game); write_file(p, 5000);
    CHECK(lg_pf_load(&g_pf, mp, 1) == 3);
    CHECK(lg_pf_run(&g_pf, 1, 1) == 1 && g_pf.skipped == 2);
    lg_pf_free(&g_pf);
    /* record cap: aggregates keep counting */
    setup("max_records=2\n");
    for (int i = 0; i < 4; i++) fake_create("gfx/interface/icons/b.dds", 1, 5000, 0);
    CHECK(g_st.nrecs == 2 && g_st.recs_dropped == 2 && g_st.total_loads == 4);
}

/* ------------------------------------------------------------------ processed-texture cache (fake engine decode)
 * The fakes reproduce what the 1.15.1 loaders write (lg_cache.h): DDS 0x3c37c50 = zero-copy subresources
 * pointing into the buffer; FreeImage 0x3c5cc10 = one subresource, owned pixel buffer at img+0x40. */
static int g_fake_dds_calls, g_fake_png_calls, g_fake_dds_wrong_w, g_fake_dds_flip;
typedef struct { uint8_t b[0x50]; uint8_t subs[16][LG_SUB_SIZE]; } fimg;   /* img + its array storage */

static void fimg_init(fimg *m)
{
    memset(m, 0, sizeof *m);
    uint32_t one = 1; memcpy(m->b + 8, &one, 4); memcpy(m->b + 0xc, &one, 4);
    void *arr = m->subs; memcpy(m->b + LG_IMG_ARR, &arr, 8);
}
static void push_sub(fimg *m, uint32_t w, uint32_t h, uint32_t bytes, const uint8_t *p)
{
    int32_t n; memcpy(&n, m->b + LG_IMG_ARRN, 4);
    uint8_t *s = m->subs[n];
    memset(s, 0, LG_SUB_SIZE);
    uint32_t one = 1;
    memcpy(s + LG_SUB_W, &w, 4); memcpy(s + LG_SUB_H, &h, 4); memcpy(s + LG_SUB_D, &one, 4);
    memcpy(s + LG_SUB_BYTES, &bytes, 4); memcpy(s + LG_SUB_PTR, &p, 8);
    n++; memcpy(m->b + LG_IMG_ARRN, &n, 4);
}
static uint64_t fake_decode(uint64_t img_, uint64_t view, uint64_t buf_, uint64_t n, uint64_t opts)
{
    (void)view; (void)opts;
    fimg *m = (fimg *)img_; const uint8_t *buf = (const uint8_t *)buf_;
    lg_dds d;
    if (lg_dds_parse(buf, (size_t)n, &d)) {                       /* 'DDS ' -> DDS loader */
        g_fake_dds_calls++;
        uint32_t w = d.w + (g_fake_dds_wrong_w ? 1 : 0), h = d.h;
        memcpy(m->b + 0, &w, 4); memcpy(m->b + 4, &h, 4);
        m->b[LG_IMG_FMT] = d.pf[12] == 32 ? 0x21 : 0x12; m->b[0x13] = 1; m->b[0x16] = 2;
        const uint8_t *p = buf + d.data_off;
        for (uint32_t i = 0; i < d.mips; i++) {
            uint32_t mw = d.w >> i ? d.w >> i : 1, mh = d.h >> i ? d.h >> i : 1, bytes = mw * mh * (d.pf[12] / 8);
            if (p + bytes > buf + n) return 0;
            push_sub(m, mw, mh, bytes, p);
            p += bytes;
        }
        if (g_fake_dds_flip) ((uint8_t *)(buf + d.data_off))[0] ^= 0xff;   /* "loader converts differently" */
        return 1;
    }
    g_fake_png_calls++;                                           /* FreeImage: 4x3 RGBA from the file bytes */
    uint32_t w = 4, h = 3, bytes = w * h * 4;
    uint8_t *px = malloc(bytes);
    for (uint32_t i = 0; i < bytes; i++) px[i] = (uint8_t)(buf[i % n] * 7 + i);
    memcpy(m->b + 0, &w, 4); memcpy(m->b + 4, &h, 4);
    m->b[LG_IMG_FMT] = 0x21; m->b[0x13] = 1; m->b[0x16] = 2;
    memcpy(m->b + 0x40, &px, 8);
    push_sub(m, w, h, bytes, px);
    return 1;
}

static uint8_t *make_dds(uint32_t w, uint32_t h, uint32_t mips, size_t *n)
{
    size_t data = 0;
    for (uint32_t i = 0; i < mips; i++) data += (size_t)(w >> i ? w >> i : 1) * (h >> i ? h >> i : 1) * 4;
    uint8_t *b = calloc(1, 128 + data);
    memcpy(b, "DDS ", 4); uint32_t v;
    v = 124; memcpy(b + 4, &v, 4); v = 0x2100f; memcpy(b + 8, &v, 4);
    memcpy(b + 12, &h, 4); memcpy(b + 16, &w, 4); memcpy(b + 28, &mips, 4);
    v = 32; memcpy(b + 76, &v, 4); v = 0x41; memcpy(b + 80, &v, 4); v = 32; memcpy(b + 88, &v, 4);   /* RGB|ALPHAPIXELS, 32 bpp */
    v = 0xff0000; memcpy(b + 92, &v, 4); v = 0xff00; memcpy(b + 96, &v, 4); v = 0xff; memcpy(b + 100, &v, 4); v = 0xff000000u; memcpy(b + 104, &v, 4);
    v = 0x1000; memcpy(b + 108, &v, 4);
    for (size_t i = 0; i < data; i++) b[128 + i] = (uint8_t)(i * 13);
    *n = 128 + data;
    return b;
}

/* one texture request through the fake CreateTexture with a real decode call between pre and post */
static void fake_create_dec(const char *path, int root, const uint8_t *buf, size_t n, fimg *m)
{
    view_t v = { path, (int)strlen(path), 0 };
    hook_call(H_OPTS, 0, (uint64_t)&v, 0, 0, 0, 1, NULL);
    hook_call(H_RDIR, (uint64_t)&v, 0, 0, 0, (uint64_t)(root == 0 ? &g_dh_mod : &g_dh_game), 1, NULL);
    hook_call(H_MOVE, 0, 0, 0, 0, 0, 1, NULL);
    fimg_init(m);
    V3X_HookCall c; memset(&c, 0, sizeof c);
    c.args[0] = (uint64_t)m; c.args[1] = (uint64_t)&v; c.args[2] = (uint64_t)buf; c.args[3] = n; c.args[4] = 0;
    c.user = (void *)(intptr_t)H_DEC; c.return_address = g_a.ret[H_DEC]; c.original = (void *)fake_decode;
    pre(&c);
    c.ret = fake_decode(c.args[0], c.args[1], c.args[2], c.args[3], c.args[4]);
    post(&c);
    hook_call(H_PIX, 0, 0, 0, 0, 0, 1, NULL);
    void *tex = (void *)0x1234;
    hook_call(H_OBJ, 0, (uint64_t)&tex, 0, 0, 0, 0, NULL);
    { V3X_HookCall o; memset(&o, 0, sizeof o); o.user = (void *)(intptr_t)H_OBJ; o.return_address = g_a.ret[H_OBJ];
      o.args[1] = (uint64_t)&tex; post(&o); }
}

static void wait_built(LONG want) { for (int i = 0; i < 200 && g_c_built < want; i++) Sleep(10); }

static void reset_cache_globals(void)
{
    memset(g_fmt, 0, sizeof g_fmt);
    g_lay_good = g_lay_bad = g_cache_off = g_c_built = g_c_hits = g_c_verify_ok = g_c_verify_bad = g_c_fail = g_c_nofmt = 0;
    g_c_bytes = 0; g_c_saved_ms = 0;
}

static void cache_tests(void)
{
    /* pure parts */
    size_t dn; uint8_t *dds = make_dds(8, 4, 3, &dn);
    lg_dds d;
    CHECK(lg_dds_parse(dds, dn, &d) == 1 && d.w == 8 && d.h == 4 && d.mips == 3 && d.plain2d && d.data_off == 128);
    CHECK(lg_dds_parse((const uint8_t *)"PNG.....", 8, &d) == 0);
    CHECK(lg_cache_key("D:/A/b.png", 1, 2, "x") == lg_cache_key("d:\\a\\B.PNG", 1, 2, "x"));
    CHECK(lg_cache_key("D:/A/b.png", 1, 2, "x") != lg_cache_key("D:/A/b.png", 1, 3, "x") && lg_cache_key("D:/A/b.png", 1, 2, "x") != lg_cache_key("D:/A/b.png", 1, 2, "y"));

    char ini[400];
    snprintf(ini, sizeof ini, "png_cache=build\nlayout_min=2\ncache_min_kb=0\ncache_dir=%scache\n", TDIR);
    setup(ini);
    reset_cache_globals();
    fake.game_build_id = NULL;
    cache_init();
    CHECK(!strcmp(g_build, "unknown") && strstr(g_cache_dir, "cache"));
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s/gfx/interface/sakuya/c.png", g_root_mod); write_file(p, 700);
    char *png = slurp(p);
    fimg m;
    /* before the layout is confirmed nothing is cached */
    fake_create_dec("gfx/interface/sakuya/c.png", 0, (const uint8_t *)png, 700, &m);
    CHECK(g_c_built == 0 && g_fake_png_calls == 1);
    /* two plain DDS confirm the layout and teach format 0x21 */
    fake_create_dec("gfx/interface/icons/d1.dds", 1, dds, dn, &m);
    fake_create_dec("gfx/interface/icons/d2.dds", 1, dds, dn, &m);
    CHECK(g_lay_good == 2 && g_lay_bad == 0 && layout_ok() && g_fmt[0x21].known);
    /* build: the next png decode writes an entry */
    fake_create_dec("gfx/interface/sakuya/c.png", 0, (const uint8_t *)png, 700, &m);
    wait_built(1);
    CHECK(g_c_built == 1 && g_c_nofmt == 0);
    lg_digest ref; CHECK(lg_digest_img(m.b, &ref, NULL) == 1 && ref.w == 4 && ref.n == 1);
    char abs[600]; snprintf(abs, sizeof abs, "%s/gfx/interface/sakuya/c.png", g_root_mod);
    WIN32_FILE_ATTRIBUTE_DATA fa; GetFileAttributesExA(abs, GetFileExInfoStandard, &fa);
    uint64_t mt = ((uint64_t)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
    char cf[MAX_PATH]; lg_cache_file(cf, sizeof cf, g_cache_dir, lg_cache_key(abs, 700, mt, g_build));
    lg_cache_hdr hh; size_t cn = 0;
    uint8_t *cd = lg_cache_read(cf, abs, 700, mt, g_build, &hh, &cn);
    CHECK(cd && cn == 128 + 48 && lg_digest_eq(&hh.dg, &ref) && !memcmp(cd + 128, (const uint8_t *)*(uint8_t **)(m.b + 0x40), 48));
    CHECK(lg_cache_read(cf, abs, 700, mt + 1, g_build, &hh, &cn) == NULL);     /* stale mtime */
    CHECK(lg_cache_read(cf, abs, 700, mt, "other-build", &hh, &cn) == NULL);   /* other game build */
    free(cd);

    /* verify: the fresh engine decode matches the stored digest */
    g_st.cfg.png_cache = LG_PC_VERIFY;
    fake_create_dec("gfx/interface/sakuya/c.png", 0, (const uint8_t *)png, 700, &m);
    CHECK(g_c_verify_ok == 1 && g_c_verify_bad == 0 && g_c_built == 1);

    /* on: decode gets the DDS bytes (DDS loader path) and the result equals the original */
    g_st.cfg.png_cache = LG_PC_ON;
    int png0 = g_fake_png_calls, dds0 = g_fake_dds_calls;
    fake_create_dec("gfx/interface/sakuya/c.png", 0, (const uint8_t *)png, 700, &m);
    CHECK(g_fake_png_calls == png0 && g_fake_dds_calls == dds0 + 1 && g_c_hits == 1 && !g_cache_off);
    tls_t *tt = TlsGetValue(g_tls); CHECK(tt && tt->sub == NULL);                /* freed after the object */
    CHECK(g_st.recs[g_st.nrecs - 1].ok && g_st.recs[g_st.nrecs - 1].complete);

    /* on, but the loader produces something else -> entry deleted, cache off for the session */
    g_fake_dds_flip = 1;
    fake_create_dec("gfx/interface/sakuya/c.png", 0, (const uint8_t *)png, 700, &m);
    g_fake_dds_flip = 0;
    CHECK(g_cache_off == 1 && g_c_fail == 1 && GetFileAttributesA(cf) == INVALID_FILE_ATTRIBUTES);
    png0 = g_fake_png_calls;
    fake_create_dec("gfx/interface/sakuya/c.png", 0, (const uint8_t *)png, 700, &m);
    CHECK(g_fake_png_calls == png0 + 1);                                       /* off: the engine decodes normally */

    /* a contradicting DDS keeps the cache inactive */
    reset_cache_globals();
    g_st.cfg.png_cache = LG_PC_BUILD;
    fake_create_dec("gfx/interface/icons/d1.dds", 1, dds, dn, &m);
    fake_create_dec("gfx/interface/icons/d2.dds", 1, dds, dn, &m);
    g_fake_dds_wrong_w = 1;
    fake_create_dec("gfx/interface/icons/d3.dds", 1, dds, dn, &m);
    g_fake_dds_wrong_w = 0;
    CHECK(g_lay_good == 2 && g_lay_bad == 1 && !layout_ok());
    fake_create_dec("gfx/interface/sakuya/c.png", 0, (const uint8_t *)png, 700, &m);
    Sleep(50);
    CHECK(g_c_built == 0);
    /* report line */
    char rp[MAX_PATH]; snprintf(rp, sizeof rp, "%slazy_gfx_report.txt", TDIR);
    write_reports("test");
    char *r = slurp(rp);
    CHECK(r && strstr(r, "png_cache=build") && strstr(r, "contradicted 1") && strstr(r, "per file (kept records)"));
    free(r); free(png); free(dds);
}

/* ------------------------------------------------------------------ real exe */
static int real_exe(const char *exe)
{
    static V3X_API rx;
    if (v3x_testkit_map_exe_dir(exe, g_tdir2, &rx)) { printf("cannot map %s\n", exe); return 1; }
    api = &rx;
    int bad = lg_resolve();
    static const uint32_t fn_rva[H_N] = { 0x3c5df50, 0x3dffd50, 0x3e3c720, 0x3c5e0d0, 0x3c6ac30, 0x3c42ef0, 0x3e14180 };
    static const uint32_t ret_rva[H_N] = { 0x3c5b008, 0x3e1c535, 0x3c5b0ec, 0x3c5b16d, 0x3c5b288, 0x3c5b31f, 0x3c42f98 };
    for (int i = 0; i < H_N; i++) {
        printf("%-10s %s fn %08x ret %08x%s\n", H_NAME[i], g_a.fn[i] ? "OK  " : "FAIL", g_a.fn[i] ? rva(g_a.fn[i]) : 0,
               g_a.ret[i] ? rva(g_a.ret[i]) : 0,
               g_a.fn[i] && (rva(g_a.fn[i]) != fn_rva[i] || rva(g_a.ret[i]) != ret_rva[i]) ? "  (differs from 1.15.1)" : "");
        CHECK(g_a.fn[i] && rva(g_a.fn[i]) == fn_rva[i] && rva(g_a.ret[i]) == ret_rva[i]);
    }
    CHECK(!bad && g_a.ct && rva(g_a.ct) == 0x3c5af20 && g_a.gol && rva(g_a.gol) == 0x3c5b4f0 && rva(g_a.vfs) == 0x3e1c4f0);
    printf("real exe: %s\n", bad ? "ANCHORS FAILED" : "ALL ANCHORS OK");
    if (!bad) {
        /* fail closed: one changed byte in each of three places -> nothing resolved */
        uint32_t spots[3] = { 0x3c5af20 + 0x3EA, 0x3c42ef0 + 0x37, 0x3c5e0d0 + 2 };
        for (int k = 0; k < 3; k++) {
            uint8_t *q = (uint8_t *)rx.image_base() + spots[k]; DWORD old;
            if (!VirtualProtect(q, 1, PAGE_READWRITE, &old)) { printf("note: cannot make the image writable (%lu); fail-closed check skipped\n", GetLastError()); break; }
            uint8_t keep = *q; *q ^= 0x01;
            int b2 = lg_resolve();
            CHECK(b2 > 0 && !g_a.ct && !g_a.fn[H_OPTS]);
            *q = keep; VirtualProtect(q, 1, old, &old);
            printf("real exe: fail-closed with byte %08x changed: %d anchor(s) failed, nothing installed\n", spots[k], b2);
        }
        CHECK(lg_resolve() == 0);
        /* end to end through the real v3x_plugin_load (testkit hook_safe only counts) */
        int hooks0 = v3x_tk.hooks;
        CHECK(v3x_plugin_load(&rx) == 0);
        CHECK(v3x_tk.hooks - hooks0 == H_N);
    }
    return bad;
}

int main(int argc, char **argv)
{
    test_tmp_dir(g_tdir, sizeof g_tdir, "lazygfx/t");
    test_tmp_dir(g_tdir2, sizeof g_tdir2, "lazygfx/t2");
    core_tests();
    request_path_tests();
    cache_tests();
    if (argc > 1) fails += real_exe(argv[1]);
    printf(fails ? "%d FAILED\n" : "ALL OK\n", fails);
    return fails ? 1 : 0;
}
