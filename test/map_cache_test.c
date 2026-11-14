/* Offline test of plugins/map_cache (no game needed).
 *   (normally run by test/run_all.sh; scratch root = $V3X_TMP, else %TEMP%, see test/test_tmp.h)
 *   python -m ziglang cc -target x86_64-windows-gnu -O2 -DMC_TEST -Iinclude -o $V3X_TMP/mct.exe test/map_cache_test.c
 *   $V3X_TMP/mct.exe                          -> ALL OK (writes only under $V3X_TMP/mapmode/t/)
 *   $V3X_TMP/mct.exe ../binaries/victoria3.exe   also maps the real exe (no code runs) and resolves the
 *                                                          3 anchors with the plugin's own resolve_all() + fail-closed check
 * Covers: config parse + example ini, path hash, MSVC string reader, the pin table (add/touch/replace/LRU budget),
 * big-reserve attribution, summary, and the detours end to end against a fake engine that has the same weak
 * texture cache semantics as GetOrLoadTexture 0x3c5b4f0 (refcount at +8, owner at +0x10, final release vslot 1). */
#include "../plugins/map_cache/mc_core.c"
#include "../plugins/map_cache/map_cache.c"
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
static char g_tdir[MAX_PATH];   /* <scratch>/mapmode/t/, set in main */
static const char *t_dir(void) { return g_tdir; }
static int t_on_frame(v3x_frame_fn fn, void *u) { (void)fn; (void)u; return 0; }
static int t_run_on_main(v3x_frame_fn fn, void *u) { fn(u); return 0; }
static uintptr_t t_base(void) { return 0x10000; }
static size_t t_size(void) { return 0x1000; }

/* ------------------------------------------------------------------ fake engine */
typedef struct ftex { void **vt; volatile LONG rc; int pad; void *owner; int destroyed; int id; } ftex;
static int g_destroyed, g_owner_calls, g_next_id;
static void f_destroy(void *t, void *u) { (void)u; ((ftex *)t)->destroyed = 1; g_destroyed++; }
static void f_owner_rel(void *o, void *t) { (void)o; (void)t; g_owner_calls++; }
static void *vt_tex[2] = { NULL, (void *)f_destroy };
static void *vt_owner[2] = { NULL, (void *)f_owner_rel };
static struct { void *vt; } g_owner = { vt_owner };

typedef struct { union { char buf[16]; char *ptr; } u; uint64_t size, cap; } mstr;
static void set_str(mstr *m, const char *s)
{
    memset(m, 0, sizeof *m);
    size_t n = strlen(s);
    if (n <= 15) { memcpy(m->u.buf, s, n + 1); m->cap = 15; } else { m->u.ptr = _strdup(s); m->cap = n; }
    m->size = n;
}

#define NC 16
static struct { char path[128]; ftex *tex; } cache[NC];
static int ct_calls;
static uint64_t g_file_size = 1 << 20;     /* file size the fake CreateTexture "reads" */

static uintptr_t fake_res(uintptr_t a, uintptr_t n, uintptr_t c, uintptr_t d) { (void)a; (void)n; (void)c; (void)d; return 0; }
static __attribute__((noinline)) uintptr_t fake_ct(uintptr_t out, uintptr_t path, uintptr_t a3, uintptr_t flags)
{
    (void)path; (void)a3; (void)flags;
    ct_calls++;
    h_res(0, (uintptr_t)(uint32_t)g_file_size, 0, 0);          /* "Reserve(&buf, file size)" at the known site */
    ftex *t = calloc(1, sizeof *t);
    t->vt = vt_tex; t->rc = 1; t->owner = (g_next_id & 1) ? &g_owner : NULL; t->id = ++g_next_id;
    *(ftex **)out = t;
    return out;
}
static void drop(ftex *t)   /* a user releasing its reference */
{
    if (InterlockedDecrement(&t->rc) == 0) {
        if (t->owner) f_owner_rel(t->owner, t);
        f_destroy(t, t);
    }
}
static uintptr_t fake_gol(uintptr_t out, uintptr_t path, uintptr_t flags, uintptr_t d)
{
    (void)d;
    char p[128]; mc_msvc_str((void *)path, p, sizeof p, NULL);
    int i, freei = -1;
    for (i = 0; i < NC; i++) { if (!strcmp(cache[i].path, p)) break; if (!cache[i].path[0] && freei < 0) freei = i; }
    if (i < NC && cache[i].tex && cache[i].tex->rc > 0) { InterlockedIncrement(&cache[i].tex->rc); *(ftex **)out = cache[i].tex; return out; }
    ftex *tmp = NULL;
    h_ct((uintptr_t)&tmp, path, 0, flags);   /* engine calls the (hooked) CreateTexture */
    if (i == NC) { i = freei; strcpy(cache[i].path, p); }
    cache[i].tex = tmp;
    InterlockedIncrement(&tmp->rc); *(ftex **)out = tmp;   /* insert + assign */
    drop(tmp);                                             /* release tmp */
    return out;
}
static ftex *request(const char *path)
{
    mstr m; set_str(&m, path);
    ftex *out = NULL;
    h_gol((uintptr_t)&out, (uintptr_t)&m, 0, 0);
    return out;
}

static V3X_API fake;
static void setup(const char *ini)
{
    memset(&fake, 0, sizeof fake);
    fake.api_version = 3; fake.struct_size = sizeof fake;
    fake.log = x_log; fake.print = x_print; fake.v3x_dir = t_dir; fake.on_frame = t_on_frame;
    fake.run_on_main = t_run_on_main; fake.image_base = t_base; fake.image_size = t_size;
    api = &fake;
    mc_config c; mc_config_defaults(&c);
    char err[160];
    CHECK(mc_config_parse(&c, ini, err, sizeof err) == 0);
    mc_init(&g_st, &c);
    g_log_left = 1000;
    QueryPerformanceFrequency(&g_freq);
    snprintf(g_report_path, sizeof g_report_path, "%smap_cache_report.txt", t_dir());
    if (g_tls_gol == TLS_OUT_OF_INDEXES) { g_tls_gol = TlsAlloc(); g_tls_ct = TlsAlloc(); }
    o_gol = NULL; o_ct = fake_ct; o_res = fake_res;
    g_armed_pin = c.mode == MC_ON;
    memset(cache, 0, sizeof cache);
    ct_calls = g_destroyed = g_owner_calls = 0;
    g_file_size = 1 << 20;
    /* calibrate the Reserve return-address check against fake_ct's call site */
    ftex *t = NULL; g_res_ret = NULL; fake_ct((uintptr_t)&t, 0, 0, 0); g_res_ret = g_test_last_ret; drop(t);
    ct_calls = g_destroyed = g_owner_calls = 0;
}

static void core_tests(void)
{
    mc_config c; mc_config_defaults(&c);
    char err[160];
    CHECK(c.mode == MC_MEASURE);
    CHECK(mc_config_parse(&c, "mode = on # x\nbudget_mb=64\n; c\n[s]\nrepeat_min=3\nmax_pins=99999\nbogus=1\nspike_ms=abc\n", err, sizeof err) == 3);
    CHECK(c.mode == MC_ON && c.budget_mb == 64 && c.repeat_min == 3 && c.max_pins == 1024);
    CHECK(strstr(err, "line 6") != NULL);
    CHECK(mc_config_parse(&c, "mode=off", err, sizeof err) == 0 && c.mode == MC_OFF);
    FILE *f = fopen("plugins/map_cache/map_cache.ini.example", "rb");
    CHECK(f != NULL);
    if (f) {
        char b[8192]; size_t n = fread(b, 1, sizeof b - 1, f); b[n] = 0; fclose(f);
        mc_config e; mc_config_defaults(&e);
        CHECK(mc_config_parse(&e, b, err, sizeof err) == 0);
        if (err[0]) printf("example: %s\n", err);
        CHECK(e.mode == MC_MEASURE);
    }
    CHECK(mc_hash("GFX\\Map/a.dds", 13) == mc_hash("gfx/map/A.DDS", 13));
    CHECK(mc_hash("a", 1) != mc_hash("b", 1));
    mstr m; char o[16];
    set_str(&m, "short.dds"); CHECK(mc_msvc_str(&m, o, sizeof o, NULL) == 9 && !strcmp(o, "short.dds"));
    set_str(&m, "gfx/interface/very_long_name.dds"); CHECK(mc_msvc_str(&m, o, sizeof o, NULL) == 15 && !strcmp(o, "y_long_name.dds"));
    m.size = 99; m.cap = 3; CHECK(mc_msvc_str(&m, o, sizeof o, NULL) < 0);
    CHECK(mc_msvc_str(NULL, o, sizeof o, readable) < 0);

    static mc_state s; mc_config pc; mc_config_defaults(&pc);
    pc.mode = MC_ON; pc.budget_mb = 2; pc.max_pins = 8; pc.repeat_min = 2; pc.max_texture_mb = 1;
    mc_init(&s, &pc);
    CHECK(!mc_should_pin(&s, 1, 100));
    CHECK(mc_should_pin(&s, 2, 1 << 20));
    CHECK(!mc_should_pin(&s, 2, (1 << 20) + 1));
    void *rep; int x1, x2, x3, x4;
    CHECK(mc_pin_add(&s, &x1, 11, 1 << 20, 1, &rep) == 1 && !rep);
    CHECK(mc_pin_add(&s, &x1, 11, 1 << 20, 2, &rep) == 0 && !rep);     /* already pinned */
    CHECK(mc_pin_add(&s, &x2, 22, 1 << 20, 3, &rep) == 1 && !rep);
    CHECK(mc_pin_add(&s, &x3, 33, 1 << 20, 4, &rep) == 1);
    CHECK(mc_pin_touch(&s, &x1, 5) == 1 && mc_pin_touch(&s, &x4, 5) == 0);
    void *v[8]; int n = mc_pin_collect(&s, v, 8);
    CHECK(n == 1 && v[0] == &x2);                                       /* LRU: x2 (last=3) */
    CHECK(mc_pin_add(&s, &x4, 33, 0, 6, &rep) == 1 && rep == &x3);      /* same path, new object */
    CHECK(s.npins == 2 && s.pin_bytes == (1u << 20) + (1024u << 10));
    CHECK(mc_pin_take_all(&s, v, 8) == 2 && s.npins == 0 && s.pin_bytes == 0);
    mc_big_record(&s, 0x3c5b134, 100 << 20); mc_big_record(&s, 0x3c5b134, 50 << 20); mc_big_record(&s, 0x1234, 20 << 20);
    CHECK(s.nbig == 2 && s.big[0].count == 2 && s.big[0].max == (100u << 20));
    mc_record_load(&s, mc_hash("p.dds", 5), "p.dds", 5, 1000, 12.5, 1);
    CHECK(mc_record_load(&s, mc_hash("p.dds", 5), "p.dds", 5, 1000, 7.5, 0) == 2);
    mc_frame_snap fs; mc_frame_take(&s, &fs);
    CHECK(fs.loads == 2 && fs.reloads == 1 && fs.ms == 20.0 && !strcmp(fs.slow, "p.dds"));
    mc_frame_take(&s, &fs); CHECK(fs.loads == 0 && !fs.slow[0]);
    char buf[4096]; mc_format_summary(&s, buf, sizeof buf, 5);
    CHECK(strstr(buf, "p.dds") && strstr(buf, "ret 03c5b134"));
    char tiny[8]; CHECK(mc_format_summary(&s, tiny, sizeof tiny, 5) <= (int)sizeof tiny && strlen(tiny) < sizeof tiny);
}

static void plugin_logic(void)
{
    /* measure: every re-request after the last user dropped the texture reads the file again */
    setup("mode=measure");
    o_gol = fake_gol;
    ftex *a = request("gfx/a.dds"); CHECK(a && ct_calls == 1); drop(a);
    CHECK(g_destroyed == 1);
    a = request("gfx/a.dds"); CHECK(ct_calls == 2);
    ftex *a2 = request("gfx/a.dds"); CHECK(ct_calls == 2 && a2 == a);    /* alive -> cache hit */
    drop(a); drop(a2);
    CHECK(g_st.loads == 2 && g_st.reloads == 1 && g_st.gol_calls == 3 && g_st.gol_misses == 2 && g_st.npins == 0);
    mc_path *p = path_slot(&g_st, mc_hash("gfx/a.dds", 9), 0);
    CHECK(p && p->loads == 2 && p->via_cache == 2 && p->bytes == (1u << 20));
    h_res(0, 64u << 20, 0, 0);                       /* big reserve from another site */
    CHECK(g_st.nbig == 1 && g_st.big[0].count == 1);
    g_file_size = 32u << 20;
    a = request("gfx/b.dds"); drop(a);               /* fake_ct's Reserve is >= 16 MB too */
    CHECK(g_st.nbig == 1 && g_st.big[0].count == 2);   /* fake code is outside the fake image: both ret 0 */

    /* on: second disk read pins it; later requests hit the cache without CreateTexture */
    setup("mode=on\nrepeat_min=2\nbudget_mb=3\nmax_texture_mb=2\n");
    o_gol = fake_gol;
    a = request("gfx/a.dds"); drop(a); CHECK(ct_calls == 1 && g_st.npins == 0);
    a = request("gfx/a.dds"); CHECK(ct_calls == 2 && g_st.npins == 1 && a->rc == 2);
    int before = g_destroyed;
    drop(a); CHECK(g_destroyed == before && a->rc == 1);              /* kept alive by the pin */
    ftex *a3 = request("gfx/a.dds"); CHECK(ct_calls == 2 && a3 == a && g_st.pin_hits == 1);
    drop(a3);
    /* too big: never pinned */
    g_file_size = 3u << 20;
    for (int i = 0; i < 3; i++) { ftex *b = request("gfx/big.dds"); drop(b); }
    CHECK(ct_calls == 5 && g_st.npins == 1);
    /* budget: 3 MB, four 1 MB textures -> on_frame evicts the least recently used (a) */
    g_file_size = 1u << 20;
    const char *names[3] = { "gfx/c.dds", "gfx/d.dds", "gfx/e.dds" };
    for (int k = 0; k < 3; k++) for (int i = 0; i < 2; i++) { ftex *t = request(names[k]); drop(t); Sleep(20); }
    CHECK(g_st.npins == 4);
    before = g_destroyed;
    on_frame(NULL);
    CHECK(g_st.npins == 3 && g_destroyed == before + 1 && a->destroyed);
    /* replaced: engine made a new object for a pinned path (cache entry overwritten) */
    int ci; for (ci = 0; ci < NC; ci++) if (!strcmp(cache[ci].path, "gfx/c.dds")) break;
    ftex *oldc = cache[ci].tex; cache[ci].tex = NULL;
    ftex *nc = request("gfx/c.dds"); drop(nc);        /* 3rd read -> pin swap, old released */
    CHECK(oldc->destroyed && !nc->destroyed && g_st.pins_replaced == 1 && g_st.npins == 3);
    /* leaving the game releases everything */
    V3X_LoadTransition tr = { "Game", "Empty", 1.0 };
    on_transition("load_transition", &tr, NULL);
    CHECK(g_st.npins == 0 && nc->destroyed);
    CHECK(g_owner_calls > 0);
    cmd_mapcache("status", NULL);
    FILE *f = fopen(g_report_path, "rb");
    CHECK(f != NULL); if (f) fclose(f);

    /* measure never pins even with repeats */
    setup("mode=measure\nrepeat_min=1");
    o_gol = fake_gol;
    for (int i = 0; i < 3; i++) { ftex *t = request("gfx/z.dds"); drop(t); }
    CHECK(ct_calls == 3 && g_st.npins == 0);
}

/* ------------------------------------------------------------------ real exe (resolution only) */
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
            RUNTIME_FUNCTION *fn = &rf[m];
            for (int g = 0; g < 32; g++) {
                uint8_t *ui = X_base + fn->UnwindData;
                if (!((ui[0] >> 3) & UNW_FLAG_CHAININFO)) break;
                fn = (RUNTIME_FUNCTION *)(ui + 4 + ((ui[2] + 1) & ~1u) * 2);
            }
            return X_base + fn->BeginAddress;
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
    static V3X_API rx;
    rx.api_version = 3; rx.struct_size = sizeof rx;
    rx.log = x_log; rx.image_base = x_base; rx.image_size = x_size;
    rx.find_string = x_find_string; rx.find_xref = x_find_xref; rx.function_start = x_function_start;
    api = &rx;
    int bad = resolve_all();
    static const uint32_t expect[A_COUNT] = { 0x3c5af20, 0x3c5b4f0, 0x847fe0 };
    for (int i = 0; i < A_COUNT; i++)
        printf("%-7s %s %08x%s\n", A[i].name, A[i].fn ? "OK  " : "FAIL", A[i].fn ? rva(A[i].fn) : 0,
               A[i].fn && rva(A[i].fn) != expect[i] ? "  (differs from 1.15.1)" : "");
    CHECK(!bad && g_res_ret && rva(g_res_ret) == 0x3c5b134);
    printf("real exe: %s\n", bad ? "ANCHORS FAILED" : "ALL ANCHORS OK");
    if (!bad) {   /* fail closed: break the refcount-release shape in GetOrLoadTexture */
        uint8_t *q = A[A_GOL].fn + 0x86 + 3; DWORD old;
        if (VirtualProtect(q, 1, PAGE_EXECUTE_READWRITE, &old)) {
            uint8_t keep = *q; *q = 0x4B;
            int bad2 = resolve_all();
            CHECK(bad2 == 1 && !A[A_GOL].fn && A[A_CT].fn && A[A_RES].fn);
            *q = keep;
            uint8_t *q2 = A[A_CT].fn + 0x204; uint8_t k2 = 0; DWORD o2;
            if (VirtualProtect(q2, 1, PAGE_EXECUTE_READWRITE, &o2)) {
                k2 = *q2; *q2 = 0x90;
                int bad3 = resolve_all();
                CHECK(bad3 == 3);    /* everything is found through CreateTexture */
                *q2 = k2; VirtualProtect(q2, 1, o2, &o2);
            }
            VirtualProtect(q, 1, old, &old);
            CHECK(resolve_all() == 0);
            printf("real exe: fail-closed check %s\n", bad2 == 1 ? "OK" : "FAILED");
        }
    }
    return bad;
}

int main(int argc, char **argv)
{
    test_tmp_dir(g_tdir, sizeof g_tdir, "mapmode/t");
    core_tests();
    plugin_logic();
    if (argc > 1) fails += real_exe(argv[1]);
    printf(fails ? "%d FAILED\n" : "ALL OK\n", fails);
    return fails ? 1 : 0;
}
