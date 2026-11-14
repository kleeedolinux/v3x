/* Offline test of plugins/enhance (no game run). #includes the plugin source.
 * Part A (synthetic, always): the save-site scanner and the speed-table anchor are fail-closed
 *   (exact counts, right flag, vanilla table content), config parsing and clamping.
 * Part B (with victoria3.exe): maps the real exe as an image (LoadLibraryEx, no imports, no code
 *   run), loads the plugin with a fake V3X_API and a temp enhance.ini (all features on), and
 *   records patch()/hook() instead of writing. Checks every anchor against the 1.15.1 RVAs found
 *   by tools/re_enh_lib.py, the original bytes under each patch, the bytes written, the
 *   `speed` console command path (run_on_main -> patch), the save_begin/save_end events (once
 *   per save, with the name), lean_debug redirects (all to the shadow byte, runtime mirror), and
 *   that the withdrawn fast_speed5 key installs nothing.
 * Build (from v3x/, see README for TEMP/ZIG cache variables):
 *   python -m ziglang cc -target x86_64-windows-gnu -O2 -Iinclude -Isdk -o $V3X_TMP/enhance/enhance_test.exe test/enhance_test.c
 * Run:
 *   $V3X_TMP/enhance/enhance_test.exe $V3X_TMP/enhance/ini [../binaries/victoria3.exe]
 *   (first argument: an existing scratch directory for enhance.ini) */
#include "../plugins/enhance/enhance.c"
#include <stdarg.h>

static int fails;
#define CHECK(c, ...) do { if (c) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
                           else { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* ------------------------------------------------------------------ fake API over a mapped image */
static uint8_t *g_base; static size_t g_size;
static uint8_t *g_text; static size_t g_text_size;
static struct { uint8_t *p; size_t n; } g_data[8]; static int g_ndata;
static char g_dir[MAX_PATH];

typedef struct { uintptr_t addr; uint8_t orig[16]; uint8_t bytes[16]; size_t n; } Patch;
static Patch g_patch[64]; static int g_npatch;
static uintptr_t g_hooks[8]; static int g_nhooks;
static char g_ev_name[8][32]; static char g_ev_save[8][64]; static int g_nev;

static void t_log(const char *tag, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); printf("  [%s] ", tag); vprintf(fmt, ap); printf("\n"); va_end(ap);
}
static int t_print(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); printf("  > "); int r = vprintf(fmt, ap); printf("\n"); va_end(ap); return r;
}
static uintptr_t t_base(void) { return (uintptr_t)g_base; }
static size_t t_size(void) { return g_size; }
static const char *t_ver(void) { return "test"; }
static void *t_scan(const char *p) { (void)p; return NULL; }
static void *t_find_string(const char *s)
{
    size_t n = strlen(s) + 1;
    for (int r = 0; r < g_ndata; r++)
        for (size_t i = 0; i + n <= g_data[r].n; i++)
            if (g_data[r].p[i] == (uint8_t)s[0] && !memcmp(g_data[r].p + i, s, n) && (i == 0 || g_data[r].p[i - 1] == 0))
                return g_data[r].p + i;
    return NULL;
}
static void *t_find_xref(const void *target, int index)
{
    for (size_t i = 0; i + 7 <= g_text_size; i++) {
        uint8_t rex = g_text[i], op = g_text[i + 1];
        if ((rex != 0x48 && rex != 0x4C) || (op != 0x8D && op != 0x8B) || (g_text[i + 2] & 0xC7) != 0x05) continue;
        int32_t d; memcpy(&d, g_text + i + 3, 4);
        if (g_text + i + 7 + d == (const uint8_t *)target && index-- == 0) return g_text + i;
    }
    return NULL;
}
static void *t_function_start(const void *addr)
{
    DWORD64 image = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry((DWORD64)addr, &image, NULL);
    if (!rf) return NULL;
    for (int guard = 0; guard < 32; guard++) {
        uint8_t *ui = (uint8_t *)image + rf->UnwindData;
        if (!((ui[0] >> 3) & UNW_FLAG_CHAININFO)) break;
        rf = (PRUNTIME_FUNCTION)(ui + 4 + ((ui[2] + 1) & ~1u) * 2);
    }
    return (uint8_t *)image + rf->BeginAddress;
}
static int t_hook(void *t, void *d, void **o) { (void)d; *o = t; if (g_nhooks < 8) g_hooks[g_nhooks++] = (uintptr_t)t; return 0; }
static int t_unhook(void *t) { (void)t; return 0; }
static int t_patch(void *addr, const void *bytes, size_t n)
{
    if (n > 16 || g_npatch >= 64) return -1;
    Patch *p = &g_patch[g_npatch++];
    p->addr = (uintptr_t)addr; p->n = n;
    memcpy(p->orig, addr, n); memcpy(p->bytes, bytes, n);   /* record only: the mapped image is not written */
    return 0;
}
static int t_on_frame(v3x_frame_fn f, void *u) { (void)f; (void)u; return 0; }
static const char *t_dir(void) { return g_dir; }
static char g_cmd[8][32]; static v3x_command_fn g_cmdfn[8]; static int g_ncmd;
static int t_register(const char *name, const char *help, v3x_command_fn fn, void *user)
{
    (void)help; (void)user;
    if (g_ncmd >= 8) return -1;
    snprintf(g_cmd[g_ncmd], 32, "%s", name); g_cmdfn[g_ncmd++] = fn; return 0;
}
static int t_run_on_main(v3x_frame_fn fn, void *user) { fn(user); return 0; }
static int t_exec(const char *line) { (void)line; return -1; }
static int t_on_event(const char *n, void (*fn)(const char *, void *, void *), void *u) { (void)n; (void)fn; (void)u; return 0; }
static int t_emit(const char *name, void *data)
{
    if (g_nev < 8) {
        const V3X_SaveBegin *e = (const V3X_SaveBegin *)data;
        snprintf(g_ev_name[g_nev], 32, "%s", name);
        snprintf(g_ev_save[g_nev], 64, "%s", e && e->name ? e->name : "(null)");
        g_nev++;
    }
    return 1;
}

static V3X_API make_api(void)
{
    V3X_API a = {0};
    a.api_version = 3; a.struct_size = sizeof(V3X_API);
    a.log = t_log; a.image_base = t_base; a.image_size = t_size; a.game_version = t_ver;
    a.pattern_scan = t_scan; a.find_string = t_find_string; a.find_xref = t_find_xref;
    a.function_start = t_function_start; a.hook = t_hook; a.unhook = t_unhook; a.patch = t_patch;
    a.on_frame = t_on_frame; a.game_dir = t_dir; a.v3x_dir = t_dir;
    a.register_command = t_register; a.print = t_print; a.run_on_main = t_run_on_main; a.exec_command = t_exec;
    a.on_event = t_on_event; a.emit_event = t_emit;
    return a;
}

static void write_ini(const char *body)
{
    char p[MAX_PATH]; snprintf(p, sizeof p, "%senhance.ini", g_dir);
    FILE *f = fopen(p, "w"); if (!f) { printf("FAIL cannot write %s\n", p); fails++; return; }
    fputs(body, f); fclose(f);
}

/* ------------------------------------------------------------------ Part A: synthetic */
static size_t put(uint8_t *buf, size_t at, const uint8_t *b, size_t n) { memcpy(buf + at, b, n); return at + n; }
static void put_rel(uint8_t *buf, size_t disp_at, size_t insn_end, const uint8_t *target)
{
    int32_t d = (int32_t)(target - (buf + insn_end)); memcpy(buf + disp_at, &d, 4);
}

static void part_a(void)
{
    printf("== part A: synthetic\n");
    static uint8_t buf[4096];
    memset(buf, 0xCC, sizeof buf);
    const uint8_t *D = buf + 4000, *other = buf + 4004;
    static const uint8_t p1a[] = {0x33, 0xC0, 0x41, 0xB8, 0x03, 0, 0, 0, 0x38, 0x05, 0, 0, 0, 0, 0x44, 0x0F, 0x45, 0xC0};
    static const uint8_t p1b[] = {0x33, 0xC0, 0x41, 0xB9, 0x03, 0, 0, 0, 0x38, 0x05, 0, 0, 0, 0, 0x44, 0x0F, 0x45, 0xC8};
    static const uint8_t p1bad[] = {0x33, 0xC0, 0x41, 0xB8, 0x03, 0, 0, 0, 0x38, 0x05, 0, 0, 0, 0, 0x44, 0x0F, 0x45, 0xC8}; /* r8d mov, r9d cmov */
    static const uint8_t p2[] = {0x0F, 0xB6, 0x05, 0, 0, 0, 0, 0x84, 0xC0, 0x74, 0x16, 0x40, 0x80, 0xFE, 0x01, 0x75, 0x05,
                                 0x40, 0xB7, 0x05, 0xEB, 0x0B, 0xBF, 0x03, 0, 0, 0, 0x84, 0xC0, 0x41, 0x0F, 0x45, 0xFE};
    size_t a1 = 100, a2 = 300, a3 = 500, a4 = 700, a5 = 900;
    put(buf, a1, p1a, sizeof p1a); put_rel(buf, a1 + 10, a1 + 14, D);
    put(buf, a2, p1b, sizeof p1b); put_rel(buf, a2 + 10, a2 + 14, D);
    put(buf, a3, p2, sizeof p2); put_rel(buf, a3 + 3, a3 + 7, D);
    EnhImg im = {buf, 3900};
    const uint8_t *cm[2] = {0}, *je = NULL;
    int r = find_save_sites(&im, D, cm, &je);
    CHECK(r == 0 && cm[0] == buf + a1 + 14 && cm[1] == buf + a2 + 14 && je == buf + a3 + 9,
          "save sites: 2 cmov + 1 je found at the cmovne / je bytes (r=%d)", r);
    r = find_save_sites(&im, other, cm, &je);
    CHECK(r == -10, "save sites: wrong flag address -> fail-closed (r=%d)", r);
    put(buf, a4, p1a, sizeof p1a); put_rel(buf, a4 + 10, a4 + 14, D);
    r = find_save_sites(&im, D, cm, &je);
    CHECK(r == -13, "save sites: a 3rd cmov site -> fail-closed (r=%d)", r);
    memset(buf + a4, 0xCC, sizeof p1a);
    put(buf, a5, p2, sizeof p2); put_rel(buf, a5 + 3, a5 + 7, D);
    r = find_save_sites(&im, D, cm, &je);
    CHECK(r == -22, "save sites: a 2nd je site -> fail-closed (r=%d)", r);
    memset(buf + a5, 0xCC, sizeof p2);
    put(buf, a2, p1bad, sizeof p1bad); put_rel(buf, a2 + 10, a2 + 14, D);
    r = find_save_sites(&im, D, cm, &je);
    CHECK(r == -11, "save sites: register mismatch (mov r8d / cmovne r9d) is not accepted (r=%d)", r);

    /* speed table anchor */
    memset(buf, 0xCC, sizeof buf);
    static const uint8_t sp[] = {0xC5, 0xFA, 0x6F, 0x05, 0, 0, 0, 0, 0xC5, 0xF8, 0x11, 0x45, 0xE7, 0xC5, 0xFA, 0x10, 0x05, 0, 0, 0, 0,
                                 0xC5, 0xFA, 0x11, 0x45, 0xF7, 0x48, 0x63, 0x48, 0x70, 0xC5, 0xFA, 0x10, 0x4C, 0x8D, 0xE7};
    uint8_t *T = buf + 3008, *T5 = buf + 3040;
    float van[4] = {2.0f, 1.0f, 0.5f, 0.25f}, f5 = 0.0001f;
    memcpy(T, van, 16); memcpy(T5, &f5, 4);
    put(buf, 200, sp, sizeof sp); put_rel(buf, 204, 208, T); put_rel(buf, 217, 221, T5);
    EnhImg im2 = {buf, 2900};
    int why = 1;
    float *tab = find_speed_table(&im2, &why);
    CHECK(tab == (float *)T && why == 0, "speed table: found (why=%d)", why);
    T[0] ^= 1;
    tab = find_speed_table(&im2, &why);
    CHECK(!tab && why == -2, "speed table: non-vanilla content -> fail-closed (why=%d)", why);
    T[0] ^= 1;
    static const uint8_t ref[] = {0x48, 0x8B, 0x05, 0, 0, 0, 0};    /* mov rax,[rip+T]: a 2nd reader */
    put(buf, 600, ref, sizeof ref); put_rel(buf, 603, 607, T);
    tab = find_speed_table(&im2, &why);
    CHECK(!tab && why == -3, "speed table: a 2nd instruction reads the table -> fail-closed (why=%d)", why);

    /* config */
    write_ini("[enhance]\nbinary_saves_in_debug=0\ndeflate_level=12\nspeed_table=1\nspeed1=-5\nspeed2=100\nspeed3=abc\n");
    char ini[MAX_PATH]; snprintf(ini, sizeof ini, "%senhance.ini", g_dir);
    load_cfg(ini);
    CHECK(cfg.binary_saves_in_debug == 0 && cfg.deflate_level == 0 && cfg.speed_table == 1 && cfg.save_timing == 1,
          "config: flags parsed, deflate_level 12 -> 0, save_timing default 1");
    CHECK(cfg.speed[0] == 0.0001f && cfg.speed[1] == 10.0f && cfg.speed[2] == 0.0001f && cfg.speed[3] == 0.25f,
          "config: speeds clamped to 0.0001..10, missing key = vanilla (%g %g %g %g)", cfg.speed[0], cfg.speed[1], cfg.speed[2], cfg.speed[3]);
    DeleteFileA(ini);
    load_cfg(ini);
    CHECK(cfg.binary_saves_in_debug == 1 && cfg.deflate_level == 0 && cfg.speed_table == 0 && cfg.save_timing == 1 &&
          !memcmp(cfg.speed, van, 16),
          "config: no ini -> defaults (saves on, deflate keep, speed table off, save timing on)");
}

/* ------------------------------------------------------------------ Part B: real exe */
static uintptr_t g_stub_args[4];
static int in_window(uintptr_t disp_rva, const uint32_t *ins, int n)
{
    for (int i = 0; i < n; i++) if (disp_rva > ins[i] && disp_rva < ins[i] + 9) return 1;
    return 0;
}
static uintptr_t stub_save(uintptr_t a, const SaveName *b, uintptr_t c, uintptr_t d)
{
    g_stub_args[0] = a; g_stub_args[1] = (uintptr_t)b; g_stub_args[2] = c; g_stub_args[3] = d; Sleep(5); return 0x1234;
}

static const Patch *patch_at(uintptr_t rva)
{
    for (int i = 0; i < g_npatch; i++) if (g_patch[i].addr - (uintptr_t)g_base == rva) return &g_patch[i];
    return NULL;
}

static void part_b(const char *exe)
{
    printf("== part B: %s\n", exe);
    HMODULE m = LoadLibraryExA(exe, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!m) { printf("FAIL cannot map %s (%lu)\n", exe, GetLastError()); fails++; return; }
    g_base = (uint8_t *)m;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    g_size = nt->OptionalHeader.SizeOfImage;
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        if (!memcmp(s->Name, ".text", 5)) { g_text = g_base + s->VirtualAddress; g_text_size = s->Misc.VirtualSize; }
        else if ((!memcmp(s->Name, ".rdata", 6) || !memcmp(s->Name, ".data", 5)) && g_ndata < 8)
            { g_data[g_ndata].p = g_base + s->VirtualAddress; g_data[g_ndata++].n = s->Misc.VirtualSize; }
    }
    V3X_API a = make_api();

    /* all features on */
    write_ini("[enhance]\nbinary_saves_in_debug=1\ndeflate_level=1\nspeed_table=1\nspeed4=0.1\nsave_timing=1\n"
              "lean_debug=1\nfast_speed5=1\n");
    g_npatch = g_nhooks = g_ncmd = 0;
    DWORD t0 = GetTickCount();
    int r = v3x_plugin_load(&a);
    printf("  load took %lu ms\n", GetTickCount() - t0);
    CHECK(r == 0, "v3x_plugin_load returned %d", r);
    CHECK(st.saves == 1 && st.deflate == 1 && st.speed == 1 && st.timing == 1,
          "all four features applied (saves %d, deflate %d, speed %d, timing %d)", st.saves, st.deflate, st.speed, st.timing);
    CHECK(st.debug_flag && (uintptr_t)(st.debug_flag - g_base) == 0x5c0efe7, "debug_mode flag at exe+0x%llx (want 0x5c0efe7)",
          (unsigned long long)(st.debug_flag ? st.debug_flag - g_base : 0));
    static const uint8_t nop4[4] = {0x0F, 0x1F, 0x40, 0x00};
    const Patch *p;
    p = patch_at(0xca96bb);
    CHECK(p && p->n == 4 && !memcmp(p->orig, "\x44\x0F\x45\xC8", 4) && !memcmp(p->bytes, nop4, 4),
          "autosave site exe+0xca96bb: cmovne r9d,eax -> nop4");
    p = patch_at(0x36628af);
    CHECK(p && p->n == 4 && !memcmp(p->orig, "\x44\x0F\x45\xC0", 4) && !memcmp(p->bytes, nop4, 4),
          "save-by-name site exe+0x36628af: cmovne r8d,eax -> nop4");
    p = patch_at(0x2711192);
    CHECK(p && p->n == 1 && p->orig[0] == 0x74 && p->bytes[0] == 0xEB, "save dialog site exe+0x2711192: je -> jmp");
    CHECK(g_nhooks == 2 && g_hooks[0] - (uintptr_t)g_base == 0x3b812c0, "deflate job hooked at exe+0x%llx (want 0x3b812c0)",
          (unsigned long long)(g_nhooks ? g_hooks[0] - (uintptr_t)g_base : 0));
    CHECK(g_nhooks == 2 && g_hooks[1] - (uintptr_t)g_base == 0x3662910, "SaveGameState hooked at exe+0x%llx (want 0x3662910)",
          (unsigned long long)(g_nhooks > 1 ? g_hooks[1] - (uintptr_t)g_base : 0));
    /* fast_speed5=1 is in the ini: withdrawn, so no issuer / tick-done hooks (only deflate + save) */
    int fast_hooked = 0;
    for (int i = 0; i < g_nhooks; i++)
        if (g_hooks[i] - (uintptr_t)g_base == 0xf8d340 || g_hooks[i] - (uintptr_t)g_base == 0xf8b320) fast_hooked = 1;
    CHECK(!fast_hooked, "fast_speed5=1 (withdrawn): tick issuer 0xf8d340 / tick-done handler 0xf8b320 NOT hooked");

    /* lean_debug: every redirected disp now points at the shadow byte */
    CHECK(st.lean == 1 && st.lean_gui == 1 && st.lean_tooltips == 12 && st.lean_cache == 1,
          "lean_debug applied: gui %d, tooltip reads %d (want 12), theater cache %d", st.lean_gui, st.lean_tooltips, st.lean_cache);
    CHECK(st.shadow && (uintptr_t)(st.shadow - g_base) == 0x5d14ff0 && *st.shadow == 0, "shadow flag at exe+0x%llx (want 0x5d14ff0)",
          (unsigned long long)(st.shadow ? st.shadow - g_base : 0));
    static const uint32_t tip_ins[12] = {0x1a3f9fa, 0x1b1fb91, 0x1d54fc0, 0x1dd5435, 0x201c1b2, 0x2059e15, 0x23740e5,
                                         0x355d2f6, 0x355d618, 0x35a644d, 0x35a674b, 0x35a699b};
    int n_lean = 0, n_tip = 0, n_bad = 0, gui_ok = 0, cache_ok = 0;
    for (int i = 0; i < g_npatch; i++) {
        Patch *q = &g_patch[i];
        if (q->n != 4 || q->addr - (uintptr_t)g_base < 0x1000) continue;
        int32_t od, nd; memcpy(&od, q->orig, 4); memcpy(&nd, q->bytes, 4);
        uint8_t *dp = (uint8_t *)q->addr;
        int k = (dp[-2] == 0x80 && dp[-1] == 0x3D) ? 1 : 0;
        if (dp + 4 + k + od != st.debug_flag) continue;            /* not a lean redirect */
        n_lean++;
        if (dp + 4 + k + nd != st.shadow) n_bad++;
        uintptr_t rva = q->addr - (uintptr_t)g_base;
        if (rva == 0x36bc323) gui_ok = 1;
        else if (rva == 0x2503117) cache_ok = 1;
        else if (in_window(rva, tip_ins, 12)) n_tip++;
    }
    CHECK(n_lean == 14 && n_bad == 0 && gui_ok && cache_ok && n_tip == 12,
          "lean redirects: %d (want 14), all to the shadow (%d bad), InDebugMode exe+0x36bc320 %s, theater cache exe+0x2503114 %s, "
          "%d of the 12 known tooltip reads", n_lean, n_bad, gui_ok ? "ok" : "MISSING", cache_ok ? "ok" : "MISSING", n_tip);
    /* runtime: lean off mirrors the real flag, lean on forces 0 */
    *(volatile uint8_t *)st.debug_flag = 1;
    st.lean_active = 0; lean_frame(NULL);
    int mirror = *st.shadow == 1;
    st.lean_active = 1; lean_frame(NULL);
    CHECK(mirror && *st.shadow == 0, "lean off: shadow mirrors debug_mode (1), lean on: shadow 0");
    *(volatile uint8_t *)st.debug_flag = 0;

    p = patch_at(0x4b08610);
    float v[4] = {0};
    if (p) memcpy(v, p->bytes, 16);
    CHECK(p && p->n == 16 && !memcmp(p->orig, SPEED_VANILLA, 16) && v[0] == 2.0f && v[1] == 1.0f && v[2] == 0.5f && v[3] == 0.1f,
          "speed table exe+0x4b08610: 2 1 0.5 0.25 -> %g %g %g %g", v[0], v[1], v[2], v[3]);
    CHECK(g_npatch == 4 + 14, "exactly 4 + 14 patches (%d)", g_npatch);

    /* speed command: run_on_main -> patch with the new value */
    int ci = -1;
    for (int i = 0; i < g_ncmd; i++) if (!strcmp(g_cmd[i], "speed")) ci = i;
    CHECK(ci >= 0, "speed command registered");
    if (ci >= 0) {
        g_npatch = 0;
        g_cmdfn[ci]("3 0.2", NULL);
        p = g_npatch == 1 ? &g_patch[0] : NULL;
        if (p) memcpy(v, p->bytes, 16);
        CHECK(p && (uintptr_t)(p->addr - (uintptr_t)g_base) == 0x4b08610 && v[2] == 0.2f && v[3] == 0.1f,
              "`speed 3 0.2` writes the table on the main thread (%g %g %g %g)", v[0], v[1], v[2], v[3]);
        g_npatch = 0;
        g_cmdfn[ci]("9 1", NULL);
        CHECK(g_npatch == 0, "`speed 9 1` is rejected (no write)");
        g_cmdfn[ci]("", NULL);
    }
    for (int i = 0; i < g_ncmd; i++) if (!strcmp(g_cmd[i], "enhance")) g_cmdfn[i]("", NULL);

    /* everything off */
    write_ini("[enhance]\nbinary_saves_in_debug=0\ndeflate_level=0\nspeed_table=0\nsave_timing=0\n");
    g_npatch = g_nhooks = g_ncmd = 0;
    r = v3x_plugin_load(&a);
    CHECK(r == 0 && g_npatch == 0 && g_nhooks == 1 && g_hooks[0] - (uintptr_t)g_base == 0x3662910 && st.saves == 0 &&
          st.deflate == 0 && st.speed == 0 && st.timing == 0 && st.lean == 0,
          "all flags 0: nothing patched, only the SaveGameState hook (for the save events)");

    /* defaults (no ini): only binary_saves_in_debug */
    char ini[MAX_PATH]; snprintf(ini, sizeof ini, "%senhance.ini", g_dir); DeleteFileA(ini);
    g_npatch = g_nhooks = g_ncmd = 0;
    r = v3x_plugin_load(&a);
    CHECK(r == 0 && g_npatch == 3 && g_nhooks == 1 && st.saves == 1 && st.timing == 1 && st.lean == 0,
          "defaults: the 3 save-format patches + the SaveGameState hook (%d patches, %d hooks)", g_npatch, g_nhooks);
    /* the timing detour passes everything through and logs (orig = a stub here) */
    orig_save_state = stub_save; g_nev = 0;
    SaveName nm = {"autosave_2", 10};
    uintptr_t rr = hk_save_state(1, &nm, 3, 0);
    CHECK(rr == 0x1234 && g_stub_args[0] == 1 && g_stub_args[1] == (uintptr_t)&nm && g_stub_args[2] == 3 && st.saves_seen == 1,
          "save_timing detour: arguments and return value passed through, save counted");
    CHECK(g_nev == 2 && !strcmp(g_ev_name[0], "save_begin") && !strcmp(g_ev_name[1], "save_end") &&
          !strcmp(g_ev_save[0], "autosave_2") && !strcmp(g_ev_save[1], "autosave_2"),
          "one save -> save_begin then save_end, name 'autosave_2' (%d events: %s/%s)", g_nev, g_nev ? g_ev_name[0] : "-", g_nev ? g_ev_save[0] : "-");
    /* name not NUL-terminated in engine memory: the event gets a terminated copy */
    static const char raw[] = {'a', 'u', 't', 'o', 's', 'a', 'v', 'e', 'X', 'Y'};
    SaveName nm2 = {raw, 8};
    g_nev = 0; hk_save_state(1, &nm2, 3, 0);
    CHECK(g_nev == 2 && !strcmp(g_ev_save[0], "autosave"), "event name is a terminated copy of name->len bytes ('%s')", g_nev ? g_ev_save[0] : "-");
    SaveName nm3 = {NULL, 0};
    g_nev = 0; hk_save_state(1, &nm3, 3, 0);
    CHECK(g_nev == 2 && !strcmp(g_ev_save[0], "(null)"), "no name -> V3X_SaveBegin.name == NULL");
    /* N7: the engine struct and the characters are read fault-proof and validated */
    uint8_t *page = (uint8_t *)VirtualAlloc(NULL, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD oldp;
    VirtualProtect(page + 4096, 4096, PAGE_NOACCESS, &oldp);
    memcpy(page + 4096 - 4, "save", 4);
    SaveName bad1 = {(const char *)(page + 4096 - 4), 12};          /* runs into a no-access page */
    SaveName bad2 = {(const char *)(uintptr_t)0x10, 4};             /* garbage pointer */
    SaveName bad3 = {"autosave", -5};
    SaveName bad4 = {"auto\nsave", 9};                            /* control character inside */
    SaveName bad5 = {"x", 100000};
    const SaveName *bads[] = {&bad1, &bad2, &bad3, &bad4, &bad5, (const SaveName *)(page + 4096), (const SaveName *)(uintptr_t)8};
    int all_null = 1;
    for (size_t i = 0; i < sizeof bads / sizeof bads[0]; i++) {
        g_nev = 0;
        rr = hk_save_state(1, bads[i], 3, 0);
        if (g_nev != 2 || strcmp(g_ev_save[0], "(null)") || rr != 0x1234) { all_null = 0; printf("  bad name %d -> '%s'\n", (int)i, g_ev_save[0]); }
    }
    CHECK(all_null, "unreadable / garbage / overlong / control-char save names (and an unreadable SaveName struct) -> name NULL, no fault, save passes through");
    SaveName edge = {(const char *)(page + 4096 - 4), 4};
    g_nev = 0; hk_save_state(1, &edge, 3, 0);
    CHECK(g_nev == 2 && !strcmp(g_ev_save[0], "save"), "a name that ends exactly at a page boundary is read ('%s')", g_nev ? g_ev_save[0] : "-");
    VirtualFree(page, 0, MEM_RELEASE);
    /* API v2 loader: no events, save still passes through */
    V3X_API a2 = a; a2.api_version = 2; api = &a2;
    g_nev = 0; rr = hk_save_state(1, &nm, 3, 0); api = &a;
    CHECK(g_nev == 0 && rr == 0x1234, "API v2: no events, save passes through");
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: %s <scratch dir> [victoria3.exe]\n", argv[0]); return 2; }
    snprintf(g_dir, sizeof g_dir, "%s\\", argv[1]);
    V3X_API a = make_api();
    api = &a;   /* part A uses only log via load_cfg-free paths, but keep api valid */
    part_a();
    if (argc > 2) part_b(argv[2]);
    else printf("(part B skipped: no victoria3.exe given)\n");
    printf(fails ? "FAILED (%d)\n" : "ALL OK\n", fails);
    return fails ? 1 : 0;
}
