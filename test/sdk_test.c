/* Test of the SDK helpers in sdk/ (no game run).
 *   python -m ziglang cc -target x86_64-windows-gnu -O2 -Iinclude -Isdk -o $T/sdkt.exe test/sdk_test.c
 *   $T/sdkt.exe [path/to/victoria3.exe]
 * Part 1 (always): v3x_ini (sections, flat keys, comments, bools, hex), v3x_mem (readable/unreadable
 *   pages, NULL page, cstr cut at a page end, pointer chains), v3x_scan (pattern compile, counts,
 *   rel32/rip targets, xrefs, callers), v3x_anchor against THIS test exe through v3x_testkit (a
 *   string anchor, ambiguity, shape selection, prologue mismatch, unique/duplicate patterns,
 *   check callback), v3x_hook macros and the manifest macro.
 * Part 2 (with the exe): v3x_testkit maps victoria3.exe; v3x_game_resolve finds the script-RNG
 *   owner global (1.15.1: exe+0x540ae80) and v3x_script_random_allowed is 0 / 1 for a fake owner.
 * Exit code 0 = ALL OK. */
#include "v3x_sdk.h"
#include "v3x_testkit.h"

#define CHECK V3X_TEST_CHECK
#define NOINL __attribute__((noinline))

V3X_PLUGIN_MANIFEST("sdk_test", "0.1.0", "v3x", "SDK self test", "release/1.15.1;6af3094f-60fb000");

/* ---- anchor targets inside this exe */
static volatile int sink_;
NOINL void use_str(const char *s) { sink_ += s[0]; }
NOINL void anchor_one(void) { use_str("v3x_sdk_test_unique_anchor_string"); sink_++; }
NOINL void anchor_two_a(void) { use_str("v3x_sdk_test_shared_string"); sink_ += 2; }
NOINL void anchor_two_b(void) { volatile char big[300]; big[sink_ & 255] = 1; use_str("v3x_sdk_test_shared_string"); sink_ += big[7]; }

static int check_is_anchor_one(const V3X_API *api, void *r, void *u) { (void)api; (void)u; return r == (void *)anchor_one ? 0 : 7; }
static int check_fail(const V3X_API *api, void *r, void *u) { (void)api; (void)r; (void)u; return 5; }

static void hex_of(const void *p, int n, char *out)
{
    const uint8_t *b = (const uint8_t *)p;
    out[0] = 0;
    for (int i = 0; i < n; i++) sprintf(out + strlen(out), "%s%02X", i ? " " : "", b[i]);
}

static void test_ini(void)
{
    V3xIni ini;
    const char *txt = "\xEF\xBB\xBF; comment\nflat = 5\n[My_Plugin]\nenabled=0\nrate = 2.5 ; inline\nmode = lazy\n"
                      "hex=0x10\nflag = yes\nbad = 12abc\n#x=1\n[other]\nenabled = 1\nmode=a;b\n";
    CHECK(v3x_ini_parse(&ini, txt) == 0, "ini parse");
    CHECK(v3x_ini_int(&ini, "my_plugin", "enabled", 1) == 0, "ini int in section (case-insensitive section)");
    CHECK(v3x_ini_int(&ini, "other", "enabled", 0) == 1, "ini same key in another section");
    CHECK(v3x_ini_double(&ini, "my_plugin", "rate", 0) == 2.5, "ini double with inline comment");
    char b[32];
    CHECK(!strcmp(v3x_ini_str(&ini, "my_plugin", "mode", "x", b, sizeof b), "lazy"), "ini string");
    CHECK(!strcmp(v3x_ini_str(&ini, "other", "mode", "x", b, sizeof b), "a;b"), "ini ';' without space is not a comment");
    CHECK(v3x_ini_int(&ini, "my_plugin", "hex", 0) == 16, "ini hex int");
    CHECK(v3x_ini_bool(&ini, "my_plugin", "flag", 0) == 1, "ini bool yes");
    CHECK(v3x_ini_int(&ini, "my_plugin", "bad", 42) == 42, "ini malformed int -> default");
    CHECK(v3x_ini_int(&ini, "my_plugin", "x", 9) == 9, "ini '#' comment line ignored");
    CHECK(v3x_ini_int(&ini, "my_plugin", "flat", 0) == 5, "ini flat key is a fallback for any section");
    CHECK(v3x_ini_int(&ini, "my_plugin", "missing", -3) == -3, "ini missing key -> default");
    v3x_ini_free(&ini);
    CHECK(v3x_ini_load(&ini, "D:/v3x_tmp/sdk/does_not_exist.ini") == 0 && !ini.found && v3x_ini_int(&ini, "a", "b", 7) == 7,
          "ini missing file = defaults");
    v3x_ini_free(&ini);
}

static void test_mem(void)
{
    uint8_t *pg = (uint8_t *)VirtualAlloc(NULL, 0x2000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD old;
    VirtualProtect(pg + 0x1000, 0x1000, PAGE_NOACCESS, &old);
    memset(pg, 'A', 0x1000);
    uint64_t v = 0;
    CHECK(v3x_mem_readable((uintptr_t)pg, 0x1000) && !v3x_mem_readable((uintptr_t)pg + 0xff8, 16), "mem readable stops at a NOACCESS page");
    CHECK(!v3x_mem_read_u64(0, &v) && !v3x_mem_read_u64(8, &v) && !v3x_mem_read_u64((uintptr_t)pg + 0x1000, &v),
          "mem reads of NULL / small / NOACCESS fail without faulting");
    char s[64];
    int n = v3x_mem_read_cstr((uintptr_t)pg + 0x1000 - 5, s, sizeof s);
    CHECK(n == 5 && !strcmp(s, "AAAAA"), "mem cstr cut at the end of the readable range (%d)", n);
    pg[10] = 0;
    CHECK(v3x_mem_read_cstr((uintptr_t)pg, s, sizeof s) == 10, "mem cstr stops at NUL");
    uint64_t chain[3];
    chain[0] = (uint64_t)(uintptr_t)&chain[1]; chain[1] = (uint64_t)(uintptr_t)&chain[2]; chain[2] = 0x1234567;
    uintptr_t out = 0;
    CHECK(v3x_mem_chain((uintptr_t)chain, (const uint32_t[]){0, 0}, 2, &out) && out == (uintptr_t)&chain[2], "mem pointer chain");
    CHECK(!v3x_mem_chain((uintptr_t)chain, (const uint32_t[]){0, 0, 0, 0}, 4, &out), "mem chain through a non-pointer fails");
    CHECK(v3x_mem_is_code((uintptr_t)(void *)test_mem) && !v3x_mem_is_code((uintptr_t)pg), "mem is_code");
    CHECK(v3x_mem_in_module(0x1000, 0x100, 0x10f8, 8) && !v3x_mem_in_module(0x1000, 0x100, 0x10f9, 8), "mem in_module bounds");
    VirtualFree(pg, 0, MEM_RELEASE);
}

static void test_scan(V3X_API *api)
{
    V3xPattern p;
    CHECK(v3x_pattern_compile("48 8B ?? ? 05", &p) == 5 && !p.mask[2] && !p.mask[3] && p.bytes[4] == 5, "pattern compile");
    CHECK(v3x_pattern_compile("48 8G", &p) == 0 && v3x_pattern_compile("4", &p) == 0 && v3x_pattern_compile("", &p) == 0,
          "malformed patterns refused");
    uint8_t buf[32] = {0xE8, 0x10, 0, 0, 0, 0x48, 0x8B, 0x05, 0xF0, 0xFF, 0xFF, 0xFF, 0x0F, 0x84, 1, 0, 0, 0};
    CHECK(v3x_rel32_target(buf) == buf + 0x15 && v3x_rip_target(buf + 5, 3, 7) == buf + 12 - 0x10 &&
              v3x_rel32_target(buf + 12) == buf + 19 && !v3x_rel32_target(buf + 5),
          "rel32 / rip targets");
    V3xSection sec = {buf, sizeof buf};
    void *first = NULL;
    CHECK(v3x_scan_count(&sec, "48 8B 05", &first) == 1 && first == buf + 5, "scan count unique");
    CHECK(v3x_scan_count(&sec, "00 00", NULL) > 1, "scan count many");
    CHECK(v3x_scan_near(buf, sizeof buf, "0F 84") == buf + 12 && !v3x_scan_near(buf, 12, "0F 84"), "scan near bounded");
    V3xSection text;
    CHECK(v3x_section(api->image_base(), ".text", &text), "section .text of this exe");
    CHECK(v3x_callers(&text, (void *)use_str, NULL, 0) >= 3, "callers of use_str found (%d)", v3x_callers(&text, (void *)use_str, NULL, 0));
}

/* Anchor strings are assembled at run time on the stack: a literal in this function would be one
 * more xref (and the "missing" string would exist in .rdata). */
static char *mk(char *buf, const char *suffix) { strcpy(buf, "v3x_sdk_test_"); strcat(buf, suffix); return buf; }

static void test_anchor(V3X_API *api)
{
    char s_one[64], s_shared[64], s_missing[64];
    mk(s_one, "unique_anchor_string"); mk(s_shared, "shared_string"); mk(s_missing, "no_such_string_anywhere");
    char pro[64];
    hex_of((void *)anchor_one, 4, pro);
    V3xAnchor a[] = {
        {"one", s_one, NULL, V3X_ANCHOR_ALL_SAME},
        {"one_pro", s_one, pro, V3X_ANCHOR_ALL_SAME, 0, check_is_anchor_one},
        {"shared_all_same", s_shared, NULL, V3X_ANCHOR_ALL_SAME},
        {"missing", s_missing, NULL, V3X_ANCHOR_ALL_SAME},
        {"bad_pro", s_one, "CC CC CC CC CC CC", V3X_ANCHOR_ALL_SAME},
        {"check_fails", s_one, NULL, V3X_ANCHOR_ALL_SAME, 0, check_fail},
        {"pattern_many", NULL, "CC", 0},
        {"shape_needs_pattern", s_shared, NULL, V3X_ANCHOR_SHAPE},
    };
    int n = (int)(sizeof a / sizeof a[0]);
    int failed = v3x_anchor_resolve_all(api, "sdk_test", a, n);
    CHECK(a[0].result == (void *)anchor_one, "string anchor -> its function (%s)", a[0].err);
    CHECK(a[1].result == (void *)anchor_one, "string anchor + prologue + check callback");
    CHECK(!a[2].result && strstr(a[2].err, "ambiguous"), "shared string is ambiguous: %s", a[2].err);
    CHECK(!a[3].result && strstr(a[3].err, "not found"), "missing string: %s", a[3].err);
    CHECK(!a[4].result && strstr(a[4].err, "does not match"), "prologue mismatch: %s", a[4].err);
    CHECK(!a[5].result && strstr(a[5].err, "check failed"), "check callback failure: %s", a[5].err);
    CHECK(!a[6].result && strstr(a[6].err, "times"), "non-unique pattern refused: %s", a[6].err);
    CHECK(!a[7].result, "SHAPE without pattern refused: %s", a[7].err);
    CHECK(failed == 6, "resolve_all counts %d failures (want 6)", failed);
    /* shape: the prologue of anchor_two_b selects it among the two users of the shared string,
       unless both functions start with the same bytes (then it must be refused as ambiguous) */
    char pa[64], pb[64];
    hex_of((void *)anchor_two_a, 6, pa); hex_of((void *)anchor_two_b, 6, pb);
    V3xAnchor s = {"shape", s_shared, pb, V3X_ANCHOR_SHAPE};
    int rc = v3x_anchor_resolve(api, &s);
    if (strcmp(pa, pb)) CHECK(rc == 0 && s.result == (void *)anchor_two_b, "shape selects anchor_two_b (%s)", s.err);
    else CHECK(rc != 0 && strstr(s.err, "ambiguous"), "same prologue -> ambiguous (%s)", s.err);
    /* unique pattern: the first 12 bytes of test_anchor itself are very likely unique */
    char me[64];
    hex_of((void *)test_anchor, 12, me);
    V3xAnchor u = {"unique", NULL, me, 0, 2};
    V3xSection text; v3x_section(api->image_base(), ".text", &text);
    if (v3x_scan_count(&text, me, NULL) == 1)
        CHECK(v3x_anchor_resolve(api, &u) == 0 && u.result == (uint8_t *)(void *)test_anchor + 2, "unique pattern + offset");
}

static void test_hook_macros(void)
{
    V3X_HookCall c; memset(&c, 0, sizeof c);
    c.args[2] = (uint64_t)(uintptr_t)&c; c.xmm[1].d = 2.5; c.xmm[3].f = 1.5f;
    CHECK(V3X_ARG(&c, 2, V3X_HookCall *) == &c && V3X_ARGF(&c, 1) == 2.5 && V3X_ARGF32(&c, 3) == 1.5f, "V3X_ARG macros");
    V3X_API old; memset(&old, 0, sizeof old); old.api_version = 3; old.struct_size = offsetof(V3X_API, hook_safe);
    CHECK(v3x_hook_observe(&old, (void *)use_str, NULL, NULL, NULL) == -1 && !V3X_HAS(&old, hook_safe),
          "v3 loader: no hook_safe, observe refuses");
    CHECK(v3x_plugin_manifest.struct_size == sizeof(V3X_PluginManifest) && !strcmp(v3x_plugin_manifest.name, "sdk_test") &&
              v3x_plugin_manifest.flags == 0,
          "manifest macro");
    void *exp = (void *)GetProcAddress(GetModuleHandleA(NULL), "v3x_plugin_manifest");
    CHECK(exp == (void *)&v3x_plugin_manifest, "manifest is exported as data (GetProcAddress)");
}

static void test_game(const char *exe)
{
    V3X_API api;
    if (v3x_testkit_map_exe(exe, &api)) { CHECK(0, "map %s", exe); return; }
    V3xGame g;
    int rc = v3x_game_resolve(&api, &g);
    CHECK(rc == 0, "game: RNG owner global resolved at exe+0x%llx with %d votes %s", v3x_testkit_rva(g.rng_owner_global),
          g.rng_votes, g.err);
    if (rc == 0 && v3x_testkit_rva(g.rng_owner_global) != 0x540ae80)
        printf("  note: RVA differs from 1.15.1 (0x540ae80): fine on another build if the checks pass\n");
    /* the mapped image's global is 0 (static data, never run) -> no scope */
    CHECK(v3x_script_random_allowed(&g) == 0, "mapped exe: no RNG scope -> not allowed");
    /* fake a scope: owner record with our thread id at +0x10 (copy-on-write image page) */
    struct { uint64_t pad[2]; uint32_t tid; } owner = {{0, 0}, GetCurrentThreadId()};
    void *holder = &owner;
    DWORD old;
    if (rc == 0 && VirtualProtect(g.rng_owner_global, 8, PAGE_READWRITE, &old)) {
        *g.rng_owner_global = &holder;
        CHECK(v3x_script_random_allowed(&g) == 1, "fake scope owned by this thread -> allowed");
        owner.tid ^= 1;
        CHECK(v3x_script_random_allowed(&g) == 0, "scope owned by another thread -> not allowed");
        *g.rng_owner_global = NULL;
    } else
        printf("  (could not make the mapped .data writable: error %lu; fake-scope checks skipped)\n", GetLastError());
    V3xGame none; memset(&none, 0, sizeof none);
    CHECK(v3x_script_random_allowed(&none) == -1, "unresolved -> -1 (fail closed)");
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_ini();
    test_mem();
    /* the testkit over this very exe (real .pdata, real strings) */
    V3X_API api;
    CHECK(v3x_testkit_use_image((uint8_t *)GetModuleHandleA(NULL)) == 0, "testkit over this exe");
    v3x_testkit_api(&api);
    CHECK(v3x_on_main_thread(&api) == 1, "on_main_thread via main_thread_id");
    test_scan(&api);
    test_anchor(&api);
    test_hook_macros();
    if (argc > 1) test_game(argv[1]);
    else printf("(no exe given: game checks skipped)\n");
    return v3x_testkit_done();
}
