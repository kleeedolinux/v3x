/* Offline test of asset_stream (no game run).
 *   python -m ziglang cc -target x86_64-windows-gnu -O2 -Iinclude -o $V3X_TMP/ast.exe test/asset_stream_test.c
 *   $V3X_TMP/ast.exe plugins/asset_stream/asset_stream.ini.example [path/to/victoria3.exe] [scratch dir]
 * Part 1: config parser, clamp, descriptor logic, built-in ini == the .example file.
 * Part 2 (with the exe): the plugin's real anchor resolution (as_resolve) against victoria3.exe
 * mapped as an image (read-only, nothing executes) through a fake V3X_API like src/core.c.
 * Part 3 (with the exe and a scratch dir): end to end. Writes <scratch>/asset_stream.ini, runs the
 * real v3x_plugin_load with a fake hook() that captures the detour, then calls the detour with a
 * fake descriptor and a fake "original" that behaves like the engine gate (Start if desc+0x14). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../plugins/asset_stream/as_logic.c"
#include "../plugins/asset_stream/as_resolve.c"
#include "../plugins/asset_stream/asset_stream.c"
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


/* ------------------------------------------------------------------ fake hook / engine */
static void *g_hook_target, *g_detour;
static char g_v3x_dir[MAX_PATH];
static const char *f_v3x_dir(void) { return g_v3x_dir; }
static int g_cmds;
static int f_register_command(const char *n, const char *h, v3x_command_fn fn, void *u)
{ (void)n; (void)h; (void)fn; (void)u; g_cmds++; return 0; }
static int f_print(const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = vprintf(fmt, ap); printf("\n"); va_end(ap); return r; }

/* behaves like 1.15.1 fn 0x3c3fdc0 around the gate: if (desc[0x14]) Start(ctx+0x10, desc[0x18]) */
static int g_engine_calls;
static uintptr_t fake_master_init(void *ctx, void *desc)
{
    g_engine_calls++;
    uint8_t *d = (uint8_t *)desc, *sr = (uint8_t *)ctx + AS_CTX_STREAMER;
    if (d[AS_DESC_STREAMING]) { sr[AS_STREAMER_ENABLED] = 1; memcpy(sr + AS_STREAMER_BUDGET, d + AS_DESC_CACHE_BYTES, 8); }
    return 0x101;   /* al = 1 (success); upper bits must pass through untouched */
}
static int f_hook(void *target, void *detour, void **orig)
{ g_hook_target = target; g_detour = detour; *orig = (void *)fake_master_init; return 0; }

static void fill_api(V3X_API *a)
{
    memset(a, 0, sizeof *a);
    a->api_version = 3; a->struct_size = sizeof *a;
    a->log = f_log; a->image_base = f_base; a->image_size = f_size;
    a->pattern_scan = f_pattern_scan; a->find_string = f_find_string;
    a->find_xref = f_find_xref; a->function_start = f_function_start;
    a->hook = f_hook; a->v3x_dir = f_v3x_dir; a->register_command = f_register_command; a->print = f_print;
}

static void desc_init(uint8_t *d, int streaming, uint64_t cache_mb)
{
    memset(d, 0, 0x20);
    d[AS_DESC_STREAMING] = (uint8_t)streaming;
    uint64_t b = cache_mb << 20; memcpy(d + AS_DESC_CACHE_BYTES, &b, 8);
}

static void resolve_real(const char *exe)
{
    if (!map_exe(exe)) { fails++; return; }
    V3X_API a; fill_api(&a);
    as_anchors an;
    int ok = as_resolve(&a, &an);
#define R(p) ((p) ? (unsigned long long)((uint8_t *)(p) - g_base) : 0ull)
    printf("resolve: %s  master_init %#llx gate %#llx Start %#llx %s\n", ok ? "OK" : "FAIL",
           R(an.master_init), R(an.gate), R(an.streamer_start), an.err);
    CHECK(ok);
    f_pattern_scan(AS_GATE_PAT); printf("  gate pattern matches: %d\n", g_scan_hits); CHECK(g_scan_hits == 1);
    if (R(an.master_init) != 0x3c3fdc0 || R(an.gate) != 0x3c407af || R(an.streamer_start) != 0x3c613e0)
        printf("  note: RVAs differ from the 1.15.1 notes (fine on another game version if all checks pass)\n");
}

static void end_to_end(const char *dir, const char *ini, int expect_hook)
{
    snprintf(g_v3x_dir, sizeof g_v3x_dir, "%s\\", dir);
    char p[MAX_PATH]; snprintf(p, sizeof p, "%sasset_stream.ini", g_v3x_dir);
    FILE *f = fopen(p, "wb"); if (!f) { printf("cannot write %s\n", p); fails++; return; }
    fputs(ini, f); fclose(f);
    /* reset plugin globals */
    g_hooked = 0; g_calls = 0; orig_master_init = NULL; g_after_enabled = -1; g_after_budget = 0;
    g_hook_target = g_detour = NULL; g_engine_calls = 0;
    V3X_API a; fill_api(&a);
    CHECK(v3x_plugin_load(&a) == 0);
    CHECK((g_detour != NULL) == expect_hook);
    if (!g_detour) { DeleteFileA(p); return; }
    CHECK((uint8_t *)g_hook_target - g_base == 0x3c3fdc0);
    uint8_t ctx[0x400]; memset(ctx, 0, sizeof ctx);
    uint8_t desc[0x20]; desc_init(desc, 0, 128);   /* what the presets give: streaming off, 128 MB */
    uintptr_t r = ((master_init_fn)g_detour)(ctx, desc);
    CHECK(r == 0x101 && g_engine_calls == 1);
    CHECK(desc[AS_DESC_STREAMING] == 1);
    CHECK(ctx[AS_CTX_STREAMER + AS_STREAMER_ENABLED] == 1);
    uint64_t b; memcpy(&b, ctx + AS_CTX_STREAMER + AS_STREAMER_BUDGET, 8);
    printf("  e2e: streamer enabled=%d budget=%llu MB\n", ctx[AS_CTX_STREAMER + AS_STREAMER_ENABLED], (unsigned long long)(b >> 20));
    CHECK(b == (g_cfg.cache_mb ? (uint64_t)g_cfg.cache_mb << 20 : 128ull << 20));
    /* implausible descriptor: untouched, engine still called */
    memset(ctx, 0, sizeof ctx); desc_init(desc, 7, 128);
    ((master_init_fn)g_detour)(ctx, desc);
    CHECK(desc[AS_DESC_STREAMING] == 7 && g_engine_calls == 2);
    cmd_status("", NULL);
    DeleteFileA(p);
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)n + 1); if (fread(b, 1, (size_t)n, f) != (size_t)n) n = 0; b[n] = 0; fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    as_config c, d; char err[128];
    as_config_defaults(&d);
    CHECK(d.enabled == 0 && d.texture_streaming == 1 && d.cache_mb == 512);

    /* built-in ini parses to the defaults and equals the .example file */
    c = d; c.enabled = 5; c.cache_mb = 1;
    CHECK(as_config_parse(&c, AS_DEFAULT_INI, err, sizeof err) == 0);
    CHECK(c.enabled == d.enabled && c.texture_streaming == d.texture_streaming && c.cache_mb == d.cache_mb);
    if (argc > 1) {
        char *ex = slurp(argv[1]);
        CHECK(ex && !strcmp(ex, AS_DEFAULT_INI));
        if (ex && strcmp(ex, AS_DEFAULT_INI)) printf("  built-in ini differs from %s\n", argv[1]);
        free(ex);
    }
    /* parser errors */
    c = d; CHECK(as_config_parse(&c, "enabled = yes\n", err, sizeof err) == 1 && strstr(err, "integer"));
    c = d; CHECK(as_config_parse(&c, "bogus = 1\nenabled=1\n", err, sizeof err) == 1 && strstr(err, "line 1"));
    c = d; CHECK(as_config_parse(&c, "\xEF\xBB\xBF[asset_stream]\r\nenabled = 1 # on\r\ncache_mb=1024\r\n", err, sizeof err) == 0);
    CHECK(c.enabled == 1 && c.cache_mb == 1024);
    /* clamp */
    c = d; c.cache_mb = 5; as_config_clamp(&c); CHECK(c.cache_mb == AS_CACHE_MIN_MB);
    c = d; c.cache_mb = 100000; as_config_clamp(&c); CHECK(c.cache_mb == AS_CACHE_MAX_MB);
    c = d; c.cache_mb = -3; c.enabled = 9; as_config_clamp(&c); CHECK(c.cache_mb == 0 && c.enabled == 1);

    /* descriptor logic */
    uint8_t desc[0x20]; as_apply_result r;
    c = d;                       desc_init(desc, 0, 128); r = as_apply(&c, desc);   /* disabled: no-op */
    CHECK(!r.changed && !r.rejected && desc[AS_DESC_STREAMING] == 0);
    c.enabled = 1;               desc_init(desc, 0, 128); r = as_apply(&c, desc);
    CHECK(r.changed && desc[AS_DESC_STREAMING] == 1 && r.new_cache == 512ull << 20 && r.old_cache == 128ull << 20);
    c.cache_mb = 0;              desc_init(desc, 0, 256); r = as_apply(&c, desc);   /* keep settings budget */
    CHECK(r.changed && desc[AS_DESC_STREAMING] == 1 && r.new_cache == 256ull << 20);
    c = d; c.enabled = 1;        desc_init(desc, 1, 512); r = as_apply(&c, desc);   /* already as wanted */
    CHECK(!r.changed && !r.rejected);
    c.texture_streaming = 0;     desc_init(desc, 0, 128); r = as_apply(&c, desc);
    CHECK(!r.changed && desc[AS_DESC_STREAMING] == 0);
    c = d; c.enabled = 1;        desc_init(desc, 2, 128); r = as_apply(&c, desc);   /* implausible flag */
    CHECK(r.rejected && desc[AS_DESC_STREAMING] == 2);
    desc_init(desc, 0, 0); r = as_apply(&c, desc); CHECK(r.rejected);              /* zero cache */
    desc_init(desc, 0, 128); desc[AS_DESC_CACHE_BYTES] = 1; r = as_apply(&c, desc); CHECK(r.rejected); /* not whole MB */

    if (argc > 2) resolve_real(argv[2]);
    if (argc > 3 && g_base) {
        end_to_end(argv[3], "[asset_stream]\nenabled = 1\ntexture_streaming = 1\ncache_mb = 768\n", 1);
        end_to_end(argv[3], "[asset_stream]\nenabled = 0\n", 0);
        end_to_end(argv[3], "enabled = 1\nnot_a_key = 1\n", 0);   /* bad config: fail closed */
        CHECK(g_cmds >= 1);
    }

    printf(fails ? "%d FAILED\n" : "ALL OK\n", fails);
    return fails != 0;
}
