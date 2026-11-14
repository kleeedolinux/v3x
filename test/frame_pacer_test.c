/* Offline test of frame_pacer (no game run).
 *   python -m ziglang cc -target x86_64-windows-gnu -O2 -Iinclude -o $V3X_TMP/fpt.exe test/frame_pacer_test.c
 *   $V3X_TMP/fpt.exe plugins/frame_pacer/frame_pacer.ini.example [path/to/victoria3.exe]
 * Part 1: config parser + policy. Part 2 (with the exe): runs the plugin's real anchor
 * resolution (fp_resolve) against victoria3.exe mapped as an image (read-only, nothing executes)
 * through a fake V3X_API that implements the scanners like src/core.c, and prints the RVAs. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../plugins/frame_pacer/fp_logic.c"
#include "../plugins/frame_pacer/fp_resolve.c"
#include <stdio.h>
#include <stdarg.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* ------------------------------------------------------------------ fake API over a mapped exe */
static uint8_t *g_base, *g_text; static size_t g_size, g_text_size;
static struct { uint8_t *p; size_t n; } g_data[8]; static int g_ndata;
static IMAGE_RUNTIME_FUNCTION_ENTRY *g_pdata; static size_t g_npdata;

static uintptr_t f_base(void) { return (uintptr_t)g_base; }
static size_t f_size(void) { return g_size; }
static void f_log(const char *tag, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); printf("[%s] ", tag); vprintf(fmt, ap); printf("\n"); va_end(ap); }

static int g_scan_hits;   /* matches of the last pattern_scan (uniqueness check) */
static void *f_pattern_scan(const char *pat)
{
    uint8_t b[256], m[256]; int n = 0;
    for (const char *s = pat; *s && n < 256;) {
        if (*s == ' ') { s++; continue; }
        if (*s == '?') { b[n] = 0; m[n++] = 0; s++; if (*s == '?') s++; continue; }
        char h[3] = {s[0], s[1], 0}; b[n] = (uint8_t)strtoul(h, NULL, 16); m[n++] = 1; s += 2;
    }
    void *first = NULL; g_scan_hits = 0;
    for (size_t i = 0; i + n <= g_text_size; i++) {
        if (m[0] && g_text[i] != b[0]) continue;
        int k = 1;
        while (k < n && (!m[k] || g_text[i + k] == b[k])) k++;
        if (k == n) { if (!first) first = g_text + i; g_scan_hits++; }
    }
    return first;
}
static void *f_find_string(const char *s)
{
    size_t n = strlen(s) + 1;
    for (int r = 0; r < g_ndata; r++)
        for (size_t i = 0; i + n <= g_data[r].n; i++)
            if (g_data[r].p[i] == (uint8_t)s[0] && !memcmp(g_data[r].p + i, s, n) && (i == 0 || g_data[r].p[i - 1] == 0))
                return g_data[r].p + i;
    return NULL;
}
static void *f_find_xref(const void *target, int index)
{
    for (size_t i = 0; i + 7 <= g_text_size; i++) {
        uint8_t rex = g_text[i], op = g_text[i + 1];
        if ((rex != 0x48 && rex != 0x4C) || (op != 0x8D && op != 0x8B) || (g_text[i + 2] & 0xC7) != 0x05) continue;
        int32_t d; memcpy(&d, g_text + i + 3, 4);
        if (g_text + i + 7 + d == (const uint8_t *)target && index-- == 0) return g_text + i;
    }
    return NULL;
}
static void *f_function_start(const void *addr)
{
    uint32_t rva = (uint32_t)((const uint8_t *)addr - g_base);
    size_t lo = 0, hi = g_npdata;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (rva < g_pdata[mid].BeginAddress) hi = mid;
        else if (rva >= g_pdata[mid].EndAddress) lo = mid + 1;
        else {
            IMAGE_RUNTIME_FUNCTION_ENTRY *rf = &g_pdata[mid];
            for (int guard = 0; guard < 32; guard++) {
                uint8_t *ui = g_base + rf->UnwindData;
                if (!((ui[0] >> 3) & UNW_FLAG_CHAININFO)) break;
                rf = (IMAGE_RUNTIME_FUNCTION_ENTRY *)(ui + 4 + ((ui[2] + 1) & ~1u) * 2);
            }
            return g_base + rf->BeginAddress;
        }
    }
    return NULL;
}

static int map_exe(const char *path)
{
    HMODULE h = LoadLibraryExA(path, NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!h) { printf("cannot map %s (error %lu)\n", path, GetLastError()); return 0; }
    g_base = (uint8_t *)((uintptr_t)h & ~(uintptr_t)3);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    g_size = nt->OptionalHeader.SizeOfImage;
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        if (!memcmp(s->Name, ".text", 5)) { g_text = g_base + s->VirtualAddress; g_text_size = s->Misc.VirtualSize; }
        else if ((!memcmp(s->Name, ".rdata", 6) || !memcmp(s->Name, ".data", 5)) && g_ndata < 8) {
            g_data[g_ndata].p = g_base + s->VirtualAddress; g_data[g_ndata++].n = s->Misc.VirtualSize;
        }
    }
    IMAGE_DATA_DIRECTORY *ex = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    g_pdata = (IMAGE_RUNTIME_FUNCTION_ENTRY *)(g_base + ex->VirtualAddress);
    g_npdata = ex->Size / sizeof *g_pdata;
    return g_text && g_npdata;
}

static void resolve_real(const char *exe)
{
    if (!map_exe(exe)) { fails++; return; }
    V3X_API api; memset(&api, 0, sizeof api);
    api.api_version = 1; api.struct_size = sizeof api;
    api.log = f_log; api.image_base = f_base; api.image_size = f_size;
    api.pattern_scan = f_pattern_scan; api.find_string = f_find_string;
    api.find_xref = f_find_xref; api.function_start = f_function_start;
    fp_anchors a;
    int n = fp_resolve(&api, &a);
#define R(p) ((p) ? (unsigned long long)((uint8_t *)(p) - g_base) : 0ull)
    printf("anchors found %d/3\n", n);
    printf("  frame_end      %#llx  (spin block %#llx, vsync setting global %#llx) %s\n",
           R(a.frame_end), R(a.spin), R(a.vsync_setting), a.err_pace);
    printf("  tick_guard     %#llx %s\n", R(a.tick_guard), a.err_tick);
    printf("  MinLockTime    %#llx = %d ms %s\n", R(a.min_lock_ms), a.min_lock_ms ? *a.min_lock_ms : -1, a.err_lock);
    printf("  present        %#llx %s\n", R(a.present), a.err_present);
    CHECK(n == 3);
    CHECK(a.present != NULL);
    /* uniqueness of both byte patterns */
    f_pattern_scan(SPIN_PAT); printf("  spin pattern matches: %d\n", g_scan_hits); CHECK(g_scan_hits == 1);
    f_pattern_scan(PRESENT_PAT); printf("  present pattern matches: %d\n", g_scan_hits); CHECK(g_scan_hits == 1);
    /* the 1.15.1 values from docs/re/graphics.md */
    if (R(a.frame_end) != 0x379a540 || R(a.tick_guard) != 0x1468a60 || R(a.min_lock_ms) != 0x5414c40 ||
        R(a.present) != 0x3c7a8d0 || R(a.vsync_setting) != 0x5c629a8)
        printf("  note: RVAs differ from the 1.15.1 notes (fine on another game version if all checks pass)\n");
}

/* ------------------------------------------------------------------ policy tests */

static fp_state st(int minimized, int fg, int sim, double idle, double cap)
{
    fp_state s = {minimized, fg, sim, idle, cap};
    return s;
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)n + 1); b[fread(b, 1, (size_t)n, f)] = 0; fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    fp_config c, d; char err[128]; const char *why;
    fp_config_defaults(&d);

    /* built-in ini == defaults, and == the shipped example file */
    fp_config_defaults(&c);
    CHECK(fp_config_parse(&c, FP_DEFAULT_INI, err, sizeof err) == 0);
    CHECK(!memcmp(&c, &d, sizeof c));
    if (argc > 1) {
        char *ex = slurp(argv[1]);
        CHECK(ex && !strcmp(ex, FP_DEFAULT_INI));
        fp_config_defaults(&c);   /* the shipped file itself must parse with zero bad lines */
        CHECK(ex && fp_config_parse(&c, ex, err, sizeof err) == 0 && !memcmp(&c, &d, sizeof c));
        free(ex);
    }
    {   /* user-edited style: CRLF, inline comments, tabs, no spaces around '=' */
        fp_config_defaults(&c);
        CHECK(fp_config_parse(&c, "[frame_pacer]\r\nidle_fps=12   # lower\r\n\tminimized_fps = 2\t# min\r\n"
                                  "tick_render_hz = 0 # keep engine\r\n# full comment\r\n\r\n", err, sizeof err) == 0);
        CHECK(c.idle_fps == 12 && c.minimized_fps == 2 && c.tick_render_hz == 0);
    }

    /* parse errors */
    fp_config_defaults(&c);
    CHECK(fp_config_parse(&c, "idle_fps = x\nnoeq\nbogus = 3\n[frame_pacer]\n# c\nidle_fps = 15 # cmt\n", err, sizeof err) == 3);
    CHECK(!strncmp(err, "line 1:", 7));
    CHECK(c.idle_fps == 15);

    /* clamp */
    c.idle_fps = -5; c.minimized_fps = 5000; c.sim_idle_ms = 1; c.tick_render_hz = -1; c.enabled = 7;
    fp_config_clamp(&c);
    CHECK(c.idle_fps == 0 && c.minimized_fps == 1000 && c.sim_idle_ms == 100 && c.tick_render_hz == 0 && c.enabled == 1);

    /* policy with defaults: idle 20 after 30 s (3 s when paused), background 20, minimized 5 */
    c = d; fp_state s;
    s = st(0, 1, 1, 0.5, 0);   CHECK(fp_choose_fps(&c, &s, &why) == 0 && !strcmp(why, "none"));
    s = st(0, 1, 1, 0.5, 60);  CHECK(fp_choose_fps(&c, &s, &why) == 60 && !strcmp(why, "engine"));
    s = st(0, 1, 1, 10, 60);   CHECK(fp_choose_fps(&c, &s, &why) == 60);           /* running, 10 s idle: no */
    s = st(0, 1, 0, 10, 60);   CHECK(fp_choose_fps(&c, &s, &why) == 20 && !strcmp(why, "paused-idle"));
    s = st(0, 1, 0, 1, 60);    CHECK(fp_choose_fps(&c, &s, &why) == 60);           /* paused but input */
    s = st(0, 1, 1, 31, 0);    CHECK(fp_choose_fps(&c, &s, &why) == 20 && !strcmp(why, "idle"));
    s = st(0, 1, 1, 31, 15);   CHECK(fp_choose_fps(&c, &s, &why) == 15 && !strcmp(why, "engine")); /* user cap lower */
    s = st(0, 0, 1, 0, 0);     CHECK(fp_choose_fps(&c, &s, &why) == 20 && !strcmp(why, "background"));
    s = st(1, 0, 1, 0, 144);   CHECK(fp_choose_fps(&c, &s, &why) == 5 && !strcmp(why, "minimized"));
    c.minimized_fps = 0;
    s = st(1, 0, 1, 0, 144);   CHECK(fp_choose_fps(&c, &s, &why) == 20 && !strcmp(why, "background"));
    c = d; c.idle_fps = 0;
    s = st(0, 1, 0, 999, 0);   CHECK(fp_choose_fps(&c, &s, &why) == 0);

    /* MinLockTime */
    c = d; CHECK(fp_min_lock_ms(&c) == 0);   /* default: engine value */
    c.tick_render_hz = 10; CHECK(fp_min_lock_ms(&c) == 100);
    c.tick_render_hz = 0; CHECK(fp_min_lock_ms(&c) == 0);
    c.tick_render_hz = 1000; CHECK(fp_min_lock_ms(&c) == 1);
    c.tick_render_hz = 3; CHECK(fp_min_lock_ms(&c) == 333);

    /* setting read semantics (mirrors the spin block) */
    uint8_t set[0x110]; memset(set, 0, sizeof set);
    uint8_t ext = 1;
    set[0xc5] = 2; set[0xd0] = 1;                CHECK(fp_read_bool_setting(set) == 1);
    set[0xc8] = 1; *(uint8_t **)(set + 0xd0) = &ext; CHECK(fp_read_bool_setting(set) == 1);
    ext = 0;                                     CHECK(fp_read_bool_setting(set) == 0);
    memset(set, 0, sizeof set); set[0x100] = 1;  CHECK(fp_read_bool_setting(set) == 1);

    if (argc > 2) resolve_real(argv[2]);

    printf(fails ? "%d FAILED\n" : "ALL OK\n", fails);
    return fails != 0;
}
