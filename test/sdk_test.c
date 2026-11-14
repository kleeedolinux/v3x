/* Test of the SDK helpers in sdk/ (no game run).
 *   python -m ziglang cc -target x86_64-windows-gnu -O2 -Iinclude -Isdk -o $T/sdkt.exe test/sdk_test.c
 *   $T/sdkt.exe [path/to/victoria3.exe]
 * Part 1 (always): v3x_ini (sections, flat keys, comments, bools, hex), v3x_mem (readable/unreadable
 *   pages, NULL page, cstr cut at a page end, pointer chains), v3x_scan (pattern compile, counts,
 *   rel32/rip targets, xrefs, callers), v3x_anchor against THIS test exe through v3x_testkit (a
 *   string anchor, ambiguity, shape selection, prologue mismatch, unique/duplicate patterns,
 *   check callback), v3x_hook macros and the manifest macro, v3x_util (paths, atomic write, QPC,
 *   leveled logging on v5 and pre-v5 loaders, reload filter), v3x_safe (V3xStr builder incl. sizes 0/1,
 *   exact fit, overflow and UTF-8 cuts, V3xSpan readers, checked arithmetic, path validation/join,
 *   reparse refusal, log sanitising, atomics, V3X_ASSERT_MAIN); audit #2: V3xStr/V3xSpan invariant
 *   checks + accessors, UTF-16 / CP_ACP paths with non-ASCII folders and junctions, race-free open,
 *   atomic writes (CREATE_NEW temp, held parent), bounded rel32/rip/scan_near, strict patterns,
 *   checked V3X_ARG, NULL-safe + range ini getters, v3x_script_get (never loads, full-path match).
 * Part 2 (with the exe): v3x_testkit maps victoria3.exe; v3x_game_resolve finds the script-RNG
 *   owner global (1.15.1: exe+0x540ae80) and v3x_script_random_allowed is 0 / 1 for a fake owner.
 * Exit code 0 = ALL OK. */
#define V3X_SAFE_STRICT 1   /* every fallible SDK helper is V3X_MUST_USE here (also V3xStr appends) */
#include "v3x_sdk.h"
#include "v3x_testkit.h"
#include "v3x_safe.h"
#include "v3x_script.h"
/* NULL is passed on purpose below to test the run-time checks behind V3X_NN (which makes clang warn
 * about a literal NULL argument at compile time). */
#pragma clang diagnostic ignored "-Wnonnull"

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
    CHECK(v3x_ini_load(&ini, "v3x_sdk_test_no_such_dir/does_not_exist.ini") == 0 && !ini.found && v3x_ini_int(&ini, "a", "b", 7) == 7,
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

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"   /* the old unbounded names keep working */
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
#pragma clang diagnostic pop

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

/* ---- v3x_util.h: a fake API whose log / log_at record what they got */
static char g_logged[4096];
static int g_logged_at = -1;
static void cap_log(const char *tag, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    size_t n = strlen(g_logged);
    n += snprintf(g_logged + n, sizeof g_logged - n, "[%s] ", tag);
    vsnprintf(g_logged + n, sizeof g_logged - n, fmt, ap);
    strncat(g_logged, "|", sizeof g_logged - strlen(g_logged) - 1);
    va_end(ap);
}
static void cap_log_at(int level, const char *tag, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    g_logged_at = level;
    size_t n = strlen(g_logged);
    n += snprintf(g_logged + n, sizeof g_logged - n, "<%d>[%s] ", level, tag);
    vsnprintf(g_logged + n, sizeof g_logged - n, fmt, ap);
    strncat(g_logged, "|", sizeof g_logged - strlen(g_logged) - 1);
    va_end(ap);
}
static int cap_level(const char *tag) { return tag && !strcmp(tag, "loud") ? V3X_LOG_TRACE : V3X_LOG_INFO; }

static void test_util(const V3X_API *tk)
{
    char p[MAX_PATH], small[12];
    char dir[MAX_PATH];
    snprintf(dir, sizeof dir, "%s", tk->v3x_dir());
    CHECK(v3x_plugin_file(tk, "my_plugin", ".ini", p, sizeof p) == p && !strncmp(p, dir, strlen(dir)) &&
              !strcmp(p + strlen(dir), "my_plugin.ini"),
          "util: v3x_plugin_file -> %s", p);
    CHECK(v3x_path(tk, p, sizeof p, "rep_%d.csv", 7) && !strcmp(p + strlen(dir), "rep_7.csv"), "util: v3x_path with format");
    CHECK(v3x_path(tk, small, sizeof small, "a_much_too_long_name_%d", 1) == NULL && small[0] == 0, "util: v3x_path refuses truncation");

    /* atomic write: create, replace, no temp file left behind */
    char f[MAX_PATH], buf[64] = "";
    GetTempPathA(sizeof f, f);
    strncat(f, "v3x_util_atomic_test.txt", sizeof f - strlen(f) - 1);
    CHECK(v3x_file_write_atomic(f, "first", 5) == 0 && v3x_file_write_atomic(f, "second!", 7) == 0, "util: atomic write + replace");
    FILE *fp = fopen(f, "rb");
    size_t n = fp ? fread(buf, 1, sizeof buf - 1, fp) : 0;
    if (fp) fclose(fp);
    buf[n] = 0;
    char tmp[MAX_PATH + 24];
    snprintf(tmp, sizeof tmp, "%s.*.v3xtmp", f);
    WIN32_FIND_DATAA tfd;
    HANDLE th = FindFirstFileA(tmp, &tfd);
    if (th != INVALID_HANDLE_VALUE) FindClose(th);
    CHECK(!strcmp(buf, "second!") && th == INVALID_HANDLE_VALUE, "util: file has the new content, temp removed");
    DeleteFileA(f);
    CHECK(v3x_file_write_atomic("Z:/no/such/dir/x.txt", "x", 1) == -1, "util: atomic write into a missing dir fails cleanly");

    double t0 = v3x_now_ms();
    int64_t q0 = v3x_qpc();
    Sleep(30);
    double dt = v3x_now_ms() - t0, dq = v3x_qpc_ms(v3x_qpc() - q0);
    CHECK(dt >= 20 && dt < 2000 && dq >= 20 && v3x_now_s() > 0, "util: QPC clock (%.1f ms for Sleep(30))", dt);

    /* leveled logging: v5 path (log_at) and fallback (loader without v5) */
    V3X_API a = *tk;
    a.log = cap_log; a.log_at = cap_log_at; a.log_level = cap_level;
    g_logged[0] = 0;
    V3X_LOGW(&a, "me", "w%d", 1);
    V3X_LOGD(&a, "me", "hidden");
    V3X_LOGT(&a, "loud", "t%d", 2);
    CHECK(!strcmp(g_logged, "<2>[me] w1|<5>[loud] t2|"), "util: v5 loader -> log_at, filtered by log_level (%s)", g_logged);
    CHECK(v3x_log_enabled(&a, "loud", V3X_LOG_TRACE) && !v3x_log_enabled(&a, "me", V3X_LOG_DEBUG), "util: v3x_log_enabled");
    a.struct_size = (uint32_t)offsetof(V3X_API, log_at);   /* a v4 loader */
    g_logged[0] = 0;
    V3X_LOGE(&a, "me", "e%d", 3);
    V3X_LOGI(&a, "me", "i");
    V3X_LOGD(&a, "loud", "dropped");
    CHECK(!strcmp(g_logged, "[me] ERROR: e3|[me] i|"), "util: v4 loader -> api->log with prefix, debug dropped (%s)", g_logged);

    CHECK(v3x_reload_is_for(NULL, "me") && v3x_reload_is_for("ME", "me") && !v3x_reload_is_for("other", "me"),
          "util: v3x_reload_is_for (NULL = all, case-insensitive)");
    CHECK(V3X_HAS(tk, log_at) && V3X_HAS(tk, build_matches) && tk->build_matches("*") == 1, "testkit fills the API v5 fields");
    HANDLE lk = v3x_config_lock(1000);
    CHECK(lk != NULL, "util: v3x_config_lock acquires the v3x.ini mutex");
    HANDLE again = v3x_config_lock(1000);   /* a mutex is recursive for its owner thread */
    CHECK(again != NULL, "util: v3x_config_lock is recursive on the same thread");
    v3x_config_unlock(again);
    v3x_config_unlock(lk);
    v3x_config_unlock(NULL);
}

/* ---- v3x_safe.h */
static char g_safe_log[512];
static void safe_log(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t n = strlen(g_safe_log);
    snprintf(g_safe_log + n, sizeof g_safe_log - n, "[%s] ", tag);
    n = strlen(g_safe_log);
    vsnprintf(g_safe_log + n, sizeof g_safe_log - n, fmt, ap);
    va_end(ap);
}
static uint32_t g_fake_main;
static uint32_t fake_main_id(void) { return g_fake_main; }
static const V3X_API *g_safe_api;
static int g_off_main_result = -1;
static DWORD WINAPI off_main_thread(LPVOID p) { (void)p; g_off_main_result = V3X_ASSERT_MAIN(g_safe_api); return 0; }

static void test_safe_str(void)
{
    char b[8];
    V3xStr s;
    /* size 0 and NULL buffer */
    v3x_str_init(&s, b, 0);
    CHECK(v3x_str_append(&s, "x") == V3X_R_EARG && s.truncated && !strcmp(v3x_str_cstr(&s), ""), "str: cap 0 never writes");
    v3x_str_init(&s, NULL, 10);
    CHECK(s.cap == 0 && v3x_str_appendf(&s, "%d", 1) == V3X_R_EARG, "str: NULL buffer = cap 0");
    /* size 1: only the NUL */
    memset(b, 'Z', sizeof b);
    v3x_str_init(&s, b, 1);
    CHECK(v3x_str_append(&s, "") == V3X_R_OK && v3x_str_appendc(&s, 'a') == V3X_R_ETRUNC && b[0] == 0 && b[1] == 'Z',
          "str: cap 1 holds only the NUL, byte after untouched");
    /* exact fit: 7 chars + NUL in 8 */
    memset(b, 'Z', sizeof b);
    v3x_str_init(&s, b, sizeof b);
    CHECK(v3x_str_append(&s, "abc") == V3X_R_OK && v3x_str_appendf(&s, "%04d", 12) == V3X_R_OK && s.len == 7 &&
          !s.truncated && !strcmp(b, "abc0012"), "str: exact fit is not truncation (%s)", b);
    CHECK(v3x_str_appendc(&s, 'x') == V3X_R_ETRUNC && s.truncated && !strcmp(b, "abc0012"), "str: one more byte -> truncated");
    CHECK(v3x_str_append(&s, "") == V3X_R_ETRUNC && s.len == 7, "str: appends after truncation are no-ops");
    v3x_str_clear(&s);
    CHECK(!s.truncated && s.len == 0 && b[0] == 0, "str: clear resets");
    /* overflow through appendf: the old n += snprintf idiom case */
    char big[16 + 4];
    memset(big, 'Z', sizeof big);
    v3x_str_init(&s, big, 16);
    for (int i = 0; i < 100; i++) (void)v3x_str_appendf(&s, "%d,", i);
    CHECK(s.truncated && s.len == 15 && strlen(big) == 15 && big[16] == 'Z' && !strncmp(big, "0,1,2,3,4,5,6,7", 15),
          "str: 100 appendf into 16 bytes stays bounded (%s)", big);
    v3x_str_init(&s, big, 16);
    CHECK(v3x_str_appendf(&s, "%s", "0123456789abcdefghij") == V3X_R_ETRUNC && s.len == 15 && !strcmp(big, "0123456789abcde"),
          "str: one long appendf keeps the prefix");
    /* UTF-8: never cut a codepoint in half */
    char u[6];
    v3x_str_init(&s, u, sizeof u);   /* room 5 */
    CHECK(v3x_str_append(&s, "ab\xE2\x82\xAC") == V3X_R_OK && !strcmp(u, "ab\xE2\x82\xAC"), "str: euro sign fits exactly");
    v3x_str_init(&s, u, sizeof u);
    CHECK(v3x_str_append(&s, "abc\xE2\x82\xAC") == V3X_R_ETRUNC && !strcmp(u, "abc") && s.len == 3,
          "str: 3-byte codepoint cut -> dropped whole (%d)", (int)s.len);
    v3x_str_init(&s, u, sizeof u);
    CHECK(v3x_str_appendf(&s, "abcd%s", "\xC3\xA9") == V3X_R_ETRUNC && !strcmp(u, "abcd"), "str: appendf 2-byte cut");
    v3x_str_init(&s, u, sizeof u);
    CHECK(v3x_str_append(&s, "a\xF0\x9F\x98\x80" "b") == V3X_R_ETRUNC && !strcmp(u, "a\xF0\x9F\x98\x80"), "str: 4-byte fits, next cut");
    v3x_str_init(&s, u, sizeof u);
    CHECK(v3x_str_append(&s, "ab") == V3X_R_OK && v3x_str_append(&s, "c\xF0\x9F\x98\x80") == V3X_R_ETRUNC && !strcmp(u, "abc"),
          "str: cut only inside the new append");
    CHECK(v3x_utf8_trim_tail("ab\x80\x80\x80\x80", 6, 0) == 6, "utf8: stray continuation bytes are kept (not a cut)");
    CHECK(v3x_utf8_trim_tail("ab\xE2\x82", 4, 0) == 2 && v3x_utf8_trim_tail("ab\xE2\x82", 4, 3) == 4, "utf8: trim respects floor");
}

static void test_safe_span(void)
{
    const uint8_t d[] = {0x01, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12, 1, 2, 3, 4, 5, 6, 7, 8, 0xAA};
    V3xSpan r;
    uint8_t b8; uint16_t b16; uint32_t b32; uint64_t b64;
    v3x_span_init(&r, d, sizeof d);
    CHECK(v3x_span_u8(&r, &b8) == V3X_R_OK && b8 == 1 && v3x_span_u16le(&r, &b16) == V3X_R_OK && b16 == 0x1234 &&
          v3x_span_u32le(&r, &b32) == V3X_R_OK && b32 == 0x12345678u && v3x_span_u64le(&r, &b64) == V3X_R_OK &&
          b64 == 0x0807060504030201ull && v3x_span_remaining(&r) == 1, "span: u8/u16le/u32le/u64le");
    CHECK(v3x_span_u16le(&r, &b16) == V3X_R_EBOUNDS && b16 == 0 && r.err && v3x_span_remaining(&r) == 0, "span: short read fails, out=0");
    CHECK(v3x_span_u8(&r, &b8) == V3X_R_EBOUNDS && v3x_span_take(&r, 0) == NULL, "span: error is sticky");
    v3x_span_init(&r, d, sizeof d);
    CHECK(v3x_span_take(&r, sizeof d) == d && v3x_span_remaining(&r) == 0 && !r.err, "span: take exactly everything");
    CHECK(v3x_span_take(&r, 0) == d + sizeof d && !r.err, "span: take(0) at the end is fine");
    v3x_span_init(&r, d, sizeof d);
    CHECK(v3x_span_take(&r, SIZE_MAX) == NULL && r.err, "span: take(SIZE_MAX) does not wrap");
    v3x_span_init(&r, d, sizeof d);
    CHECK(v3x_span_skip(&r, 3) == V3X_R_OK && v3x_span_skip(&r, 14) == V3X_R_EBOUNDS && r.err, "span: skip bounds");
    v3x_span_init(&r, d, sizeof d);
    CHECK(v3x_span_skip(&r, 1) == V3X_R_OK, "span: skip 1");
    V3xSpan sub = v3x_span_sub(&r, 2);
    CHECK(!sub.err && sub.len == 2 && v3x_span_u16le(&sub, &b16) == V3X_R_OK && b16 == 0x1234 && v3x_span_u8(&sub, &b8) != V3X_R_OK &&
          !r.err && v3x_span_remaining(&r) == 13, "span: sub-span is bounded to its own length");
    sub = v3x_span_sub(&r, 99);
    CHECK(sub.err && sub.len == 0 && r.err, "span: oversized sub fails both");
    v3x_span_init(&r, NULL, 5);
    CHECK(r.len == 0 && v3x_span_u8(&r, &b8) == V3X_R_EBOUNDS, "span: NULL data = empty");
}

static void test_safe_math(void)
{
    size_t o = 7;
    CHECK(v3x_add_ok(1, 2, &o) && o == 3, "math: add");
    CHECK(!v3x_add_ok(SIZE_MAX, 1, &o) && o == 0, "math: add overflow");
    CHECK(v3x_add_ok(SIZE_MAX - 1, 1, &o) && o == SIZE_MAX, "math: add at the edge");
    CHECK(v3x_mul_ok(0, SIZE_MAX, &o) && o == 0 && v3x_mul_ok(1u << 16, 1u << 16, &o) && o == (size_t)1 << 32, "math: mul");
    CHECK(!v3x_mul_ok(SIZE_MAX / 2 + 1, 2, &o) && o == 0, "math: mul overflow");
    uint32_t u = 1;
    int64_t i = 1;
    CHECK(!v3x_add_ok_u32(0xFFFFFFFFu, 1, &u) && u == 0 && v3x_add_ok_u32(0xFFFFFFFEu, 1, &u) && u == 0xFFFFFFFFu, "math: u32");
    CHECK(!v3x_add_ok_i64(INT64_MAX, 1, &i) && !v3x_add_ok_i64(INT64_MIN, -1, &i) && v3x_add_ok_i64(-5, 3, &i) && i == -2, "math: i64");
    void *p = v3x_alloc_array(SIZE_MAX / 4, 8);
    CHECK(p == NULL, "alloc_array: overflowing count -> NULL");
    int *a = (int *)v3x_alloc_array(10, sizeof(int));
    CHECK(a && a[9] == 0, "alloc_array: zeroed");
    int *a2 = (int *)v3x_realloc_array(a, SIZE_MAX / 2, sizeof(int));
    CHECK(a2 == NULL && a[0] == 0, "realloc_array: overflow keeps the old block");
    a2 = (int *)v3x_realloc_array(a, 20, sizeof(int));
    CHECK(a2 != NULL, "realloc_array: grows");
    free(a2 ? a2 : a);
    void *z = v3x_alloc_array(0, 8);
    CHECK(z != NULL, "alloc_array: 0 elements = 1-byte block");
    free(z);
}

static void test_safe_path(void)
{
    static const char *const good[] = {"a.txt", "dir/sub/file.ini", "dir\\x.dll", "icon.d/x", "CONSOLE", "COM10", "lpt", "a b",
                                       ".hidden", "x..y", "\xC3\xA9t\xC3\xA9.txt", "COM\xC2\xB9x"};
    static const char *const bad[] = {"", "..", ".", "../x", "a/../b", "a/..", "/abs", "\\abs", "C:\\x", "c:x", "a:b", "x/",
                                      "a//b", "a\\\\b", "\\\\?\\C:\\x", "\\\\server\\share", "CON", "con.txt", "Nul", "aux.ini",
                                      "prn", "con.d/x", "COM1", "lpt9.log", "com0", "CONIN$", "conout$", "dir/NUL.txt", "NUL .txt",
                                      "COM\xC2\xB9", "LPT\xC2\xB3.x", "trail.", "trail ", "dir./x", "a\x01" "b", "a<b", "a>b",
                                      "a|b", "a?b", "a*b", "a\"b", "a\x7f"};
    int ok_good = 1, ok_bad = 1;
    for (size_t k = 0; k < sizeof good / sizeof good[0]; k++)
        if (v3x_path_rel_ok(good[k]) != V3X_R_OK) { ok_good = 0; printf("  wrongly rejected: %s\n", good[k]); }
    for (size_t k = 0; k < sizeof bad / sizeof bad[0]; k++)
        if (v3x_path_rel_ok(bad[k]) == V3X_R_OK) { ok_bad = 0; printf("  wrongly accepted: %s\n", bad[k]); }
    CHECK(ok_good, "path: %d valid relative paths accepted", (int)(sizeof good / sizeof good[0]));
    CHECK(ok_bad, "path: %d traversal/absolute/device/bad-char paths rejected", (int)(sizeof bad / sizeof bad[0]));
    CHECK(v3x_path_rel_ok(NULL) == V3X_R_EARG, "path: NULL -> EARG");
    char longp[V3X_PATH_REL_MAX + 2];
    memset(longp, 'a', sizeof longp);
    longp[V3X_PATH_REL_MAX + 1] = 0;
    CHECK(v3x_path_rel_ok(longp) == V3X_R_EPATH, "path: overlong rejected");
    for (int k = 200; k < V3X_PATH_REL_MAX; k += 200) longp[k] = '/';
    longp[V3X_PATH_REL_MAX] = 0;
    CHECK(v3x_path_rel_ok(longp) == V3X_R_OK, "path: exactly V3X_PATH_REL_MAX with components <= 255 accepted");
    char comp[300];
    memset(comp, 'b', 256);
    comp[256] = 0;
    CHECK(v3x_path_rel_ok(comp) == V3X_R_EPATH, "path: component of 256 bytes rejected");
    comp[255] = 0;
    CHECK(v3x_path_rel_ok(comp) == V3X_R_OK, "path: component of 255 bytes accepted");
    CHECK(v3x_path_rel_ok_ex("a/b", V3X_PATH_SINGLE) == V3X_R_EPATH && v3x_path_rel_ok_ex("ab", V3X_PATH_SINGLE) == V3X_R_OK,
          "path: SINGLE flag");

    char out[32];
    CHECK(v3x_path_join_checked(out, sizeof out, "C:\\g\\v3x\\", "a/b.txt", 0) == V3X_R_OK && !strcmp(out, "C:\\g\\v3x\\a\\b.txt"),
          "join: base with separator, '/' normalised (%s)", out);
    CHECK(v3x_path_join_checked(out, sizeof out, "C:\\g", "x", 0) == V3X_R_OK && !strcmp(out, "C:\\g\\x"), "join: separator added");
    CHECK(v3x_path_join_checked(out, sizeof out, "", "x/y", 0) == V3X_R_OK && !strcmp(out, "x\\y"), "join: empty base");
    strcpy(out, "junk");
    CHECK(v3x_path_join_checked(out, sizeof out, "C:\\g\\", "..\\version.dll", 0) == V3X_R_EPATH && out[0] == 0,
          "join: traversal -> EPATH, out empty");
    CHECK(v3x_path_join_checked(out, 12, "C:\\g\\", "abcdefg", 0) == V3X_R_ETRUNC && out[0] == 0, "join: overlong output -> ETRUNC");
    CHECK(v3x_path_join_checked(out, 12, "C:\\g\\", "abcdef", 0) == V3X_R_OK && !strcmp(out, "C:\\g\\abcdef"), "join: exact fit ok");
    CHECK(v3x_path_join_checked(NULL, 5, "a", "b", 0) == V3X_R_EARG && v3x_path_join_checked(out, 0, "a", "b", 0) == V3X_R_EARG &&
          v3x_path_join_checked(out, sizeof out, NULL, "b", 0) == V3X_R_EARG, "join: bad args");

    /* reparse refusal: a junction inside the base */
    char base[MAX_PATH], tgt[MAX_PATH], jn[MAX_PATH], cmd[3 * MAX_PATH], p[MAX_PATH];
    GetTempPathA(sizeof base, base);
    snprintf(base + strlen(base), sizeof base - strlen(base), "v3x_safe_%lu", (unsigned long)GetCurrentProcessId());
    snprintf(tgt, sizeof tgt, "%s\\real", base);
    snprintf(jn, sizeof jn, "%s\\link", base);
    CreateDirectoryA(base, NULL);
    CreateDirectoryA(tgt, NULL);
    snprintf(cmd, sizeof cmd, "cmd /c mklink /J \"%s\" \"%s\" >NUL 2>&1", jn, tgt);
    DWORD ja = (system(cmd), GetFileAttributesA(jn));
    int made = ja != INVALID_FILE_ATTRIBUTES && (ja & FILE_ATTRIBUTE_REPARSE_POINT);
    CHECK(v3x_path_join_checked(p, sizeof p, base, "real/x.txt", V3X_PATH_CHECK_REPARSE) == V3X_R_OK, "join: plain dir passes the reparse check");
    CHECK(v3x_path_join_checked(p, sizeof p, base, "missing/deeper/x", V3X_PATH_CHECK_REPARSE) == V3X_R_OK, "join: missing path passes");
    if (made) {
        CHECK(v3x_path_join_checked(p, sizeof p, base, "link/x.txt", V3X_PATH_CHECK_REPARSE) == V3X_R_EREPARSE && p[0] == 0,
              "join: junction component -> EREPARSE");
        CHECK(v3x_path_join_checked(p, sizeof p, base, "link", V3X_PATH_CHECK_REPARSE) == V3X_R_EREPARSE, "join: junction as last component");
        CHECK(v3x_path_join_checked(p, sizeof p, base, "link/x.txt", 0) == V3X_R_OK, "join: without the flag no check");
        RemoveDirectoryA(jn);
    } else printf("(mklink /J failed: junction checks skipped)\n");
    RemoveDirectoryA(tgt);
    RemoveDirectoryA(base);
}

static void test_safe_misc(void)
{
    char d[32];
    CHECK(v3x_sanitize_line(d, sizeof d, "ok\nFAKE [12:00][core] x\r\t\x1b[31m") == 30 &&
          !strcmp(d, "ok?FAKE [12:00][core] x???[31m"), "sanitize: control chars -> '?' (%s)", d);
    CHECK(v3x_sanitize_line(d, sizeof d, "a\xC2\x85" "b\xE2\x80\xAE" "c\xE2\x81\xA6" "d\xC3\xA9") == 9 && !strcmp(d, "a?b?c?d\xC3\xA9"),
          "sanitize: C1 / bidi override -> '?', other UTF-8 kept");
    CHECK(v3x_sanitize_line(d, 5, "abc\xC3\xA9") == 3 && !strcmp(d, "abc"), "sanitize: cut at a codepoint boundary");
    CHECK(v3x_sanitize_line(d, 1, "abc") == 0 && d[0] == 0 && v3x_sanitize_line(d, sizeof d, NULL) == 0 && v3x_sanitize_line(NULL, 4, "x") == 0,
          "sanitize: cap 1 / NULL src / NULL dst");
    CHECK(v3x_sanitize_linen(d, sizeof d, "abcdef", 3) == 3 && !strcmp(d, "abc"), "sanitize: n bounds the source");
    CHECK(v3x_sanitize_linen(d, sizeof d, "a\xE2\x80", 3) == 3, "sanitize: truncated bidi prefix at n is copied, not over-read");

    volatile int x = 1;
    void *volatile pp = NULL;
    int dummy;
    CHECK(v3x_atomic_cas_int(&x, 1, 5) && v3x_atomic_load_int(&x) == 5 && !v3x_atomic_cas_int(&x, 1, 9) && x == 5 &&
          v3x_atomic_add_int(&x, 2) == 7, "atomic: int load/cas/add");
    v3x_atomic_store_int(&x, -1);
    CHECK(x == -1 && v3x_atomic_cas_ptr(&pp, NULL, &dummy) && v3x_atomic_load_ptr(&pp) == &dummy &&
          !v3x_atomic_cas_ptr(&pp, NULL, (void *)&x), "atomic: ptr cas/load");
    v3x_atomic_store_ptr(&pp, NULL);
    CHECK(pp == NULL, "atomic: ptr store");

    V3X_API a;
    memset(&a, 0, sizeof a);
    a.api_version = 5;
    a.struct_size = (uint32_t)sizeof a;
    a.log = safe_log;
    a.main_thread_id = fake_main_id;
    g_fake_main = (uint32_t)GetCurrentThreadId();
    g_safe_api = &a;
    CHECK(V3X_ASSERT_MAIN(&a) == 1 && g_safe_log[0] == 0, "assert_main: on the main thread -> 1, silent");
    HANDLE th = CreateThread(NULL, 0, off_main_thread, NULL, 0, NULL);
    WaitForSingleObject(th, 5000);
    CloseHandle(th);
    CHECK(g_off_main_result == 0 && strstr(g_safe_log, "must run on the main thread") && strstr(g_safe_log, "sdk_test.c"),
          "assert_main: other thread -> 0 + ERROR log (%s)", g_safe_log);
    a.struct_size = (uint32_t)offsetof(V3X_API, hook_safe);
    CHECK(V3X_ASSERT_MAIN(&a) == 1 && V3X_ASSERT_MAIN(NULL) == 1, "assert_main: pre-v4 loader / NULL -> 1 (unknown)");
}

/* ---- audit #2 actions 10-13 (N6, N10, N11, 12) */

/* N10: invariant checks and accessors */
static void test_safe_invariants(void)
{
    char b[8], guard[8];
    memset(guard, 'G', sizeof guard);
    V3xStr s;
    v3x_str_init(&s, b, sizeof b);
    CHECK(v3x_str_append(&s, "abc") == V3X_R_OK && v3x_str_len(&s) == 3 && v3x_str_cap(&s) == 8 && !v3x_str_truncated(&s),
          "str accessors: len / cap / truncated");
    s.len = 50;   /* corrupted: len > cap would have made room = cap - 1 - len wrap */
    CHECK(v3x_str_append(&s, "x") == V3X_R_EARG && v3x_str_truncated(&s) && !strcmp(v3x_str_cstr(&s), "") && v3x_str_len(&s) == 0 &&
              guard[0] == 'G',
          "str: len > cap is refused before any arithmetic (EARG, nothing written)");
    CHECK(v3x_str_appendf(&s, "%d", 5) == V3X_R_EARG, "str: appendf refuses the corrupted builder too");
    v3x_str_init(&s, b, sizeof b);
    s.len = sizeof b;   /* len == cap: no room for the NUL */
    CHECK(v3x_str_appendc(&s, 'x') == V3X_R_EARG, "str: len == cap refused");
    s.buf = NULL; s.cap = 8; s.len = 0;   /* cap > 0 with no buffer */
    CHECK(v3x_str_append(&s, "x") == V3X_R_EARG && !strcmp(v3x_str_cstr(&s), "") && v3x_str_cap(&s) == 0,
          "str: NULL buf with cap > 0 refused");
    v3x_str_clear(&s);
    CHECK(s.cap == 0 && v3x_str_append(&s, "x") == V3X_R_EARG, "str: clear of a buffer-less builder keeps it unusable");
    CHECK(v3x_str_len(NULL) == 0 && v3x_str_truncated(NULL) && !strcmp(v3x_str_cstr(NULL), ""), "str accessors: NULL safe");
    /* overlapping append (t inside buf): memmove */
    v3x_str_init(&s, b, sizeof b);
    CHECK(v3x_str_append(&s, "ab") == V3X_R_OK && v3x_str_appendn(&s, b, 2) == V3X_R_OK && !strcmp(b, "abab"),
          "str: appending its own content");

    const uint8_t d[] = {1, 2, 3, 4};
    V3xSpan r;
    uint8_t x = 9;
    v3x_span_init(&r, d, sizeof d);
    CHECK(v3x_span_u8(&r, &x) == V3X_R_OK && v3x_span_pos(&r) == 1 && v3x_span_len(&r) == 4 && !v3x_span_failed(&r),
          "span accessors: pos / len / failed");
    r.pos = 99;   /* corrupted: pos > len */
    CHECK(v3x_span_u8(&r, &x) == V3X_R_EBOUNDS && x == 0 && v3x_span_failed(&r) && v3x_span_remaining(&r) == 0 &&
              v3x_span_take(&r, 0) == NULL && v3x_span_pos(&r) == 0,
          "span: pos > len refused before any arithmetic (sticky)");
    v3x_span_init(&r, d, sizeof d);
    r.p = NULL;   /* corrupted: len > 0 with no data */
    CHECK(v3x_span_skip(&r, 1) == V3X_R_EBOUNDS && v3x_span_failed(&r) && v3x_span_len(&r) == 0, "span: NULL p with len > 0 refused");
    CHECK(v3x_span_failed(NULL) && v3x_span_remaining(NULL) == 0 && v3x_span_pos(NULL) == 0, "span accessors: NULL safe");
}

/* a character of `cands` that round-trips through the ANSI code page, as UTF-16; 0 if none */
static wchar_t acp_char(void)
{
    static const wchar_t cands[] = {0x00E9, 0x00E4, 0x00F1, 0x00DF, 0x0436, 0x00E7, 0xAC00, 0x3042, 0};
    for (int i = 0; cands[i]; i++) {
        BOOL lossy = FALSE;
        char a[8];
        wchar_t w2[4];
        int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, &cands[i], 1, a, sizeof a, NULL, &lossy);
        if (n <= 0 || lossy) continue;
        if (MultiByteToWideChar(CP_ACP, 0, a, n, w2, 4) == 1 && w2[0] == cands[i]) return cands[i];
    }
    return 0;
}

static int mk_junction_w(const wchar_t *link, const wchar_t *target)
{
    wchar_t cmd[3 * MAX_PATH];
    _snwprintf(cmd, sizeof cmd / sizeof cmd[0], L"cmd /c mklink /J \"%ls\" \"%ls\" >NUL 2>&1", link, target);
    cmd[sizeof cmd / sizeof cmd[0] - 1] = 0;
    _wsystem(cmd);
    DWORD a = GetFileAttributesW(link);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}

static int count_tmp_files(const wchar_t *dir)
{
    wchar_t pat[MAX_PATH];
    _snwprintf(pat, MAX_PATH, L"%ls\\*.v3xtmp", dir);
    pat[MAX_PATH - 1] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do n++; while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

static int read_file_w(const wchar_t *p, char *buf, size_t cap)
{
    FILE *f = _wfopen(p, L"rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return (int)n;
}

/* N6: non-ASCII folders (UTF-16 and the ANSI code page), handle-based reparse checks, junctions,
 * race-free open, atomic writes with CREATE_NEW temp files */
static void test_safe_paths_w(void)
{
    wchar_t tmpw[MAX_PATH], base[MAX_PATH], real[MAX_PATH], link[MAX_PATH], p[V3X_PATH_WMAX];
    GetTempPathW(MAX_PATH, tmpw);
    /* a UTF-16-only name (not representable in most ANSI code pages) + a character of the ANSI code page */
    wchar_t ac = acp_char();
    _snwprintf(base, MAX_PATH, L"%lsv3x_w_%lu_\x0416\x00E9\x4E2D_%lc", tmpw, (unsigned long)GetCurrentProcessId(), ac ? ac : L'a');
    base[MAX_PATH - 1] = 0;
    _snwprintf(real, MAX_PATH, L"%ls\\r\x00E9" L"al", base);
    _snwprintf(link, MAX_PATH, L"%ls\\link", base);
    CreateDirectoryW(base, NULL);
    CreateDirectoryW(real, NULL);
    int made = mk_junction_w(link, real);

    CHECK(v3x_path_rel_ok_exW(L"dir/\x00E9t\x00E9.txt", 0) == V3X_R_OK && v3x_path_rel_ok_exW(L"..\\x", 0) == V3X_R_EPATH &&
              v3x_path_rel_ok_exW(L"a\xD800" L"b", 0) == V3X_R_EPATH && v3x_path_rel_ok_exW(L"CON", 0) == V3X_R_EPATH &&
              v3x_path_rel_ok_exW(L"a/b", V3X_PATH_SINGLE) == V3X_R_EPATH,
          "pathW: rel validation (non-ASCII ok, '..' / unpaired surrogate / device / SINGLE refused)");
    CHECK(v3x_path_join_checkedW(p, V3X_PATH_WMAX, base, L"r\x00E9" L"al/x.txt", V3X_PATH_CHECK_REPARSE) == V3X_R_OK &&
              wcsstr(p, L"\\r\x00E9" L"al\\x.txt") != NULL,
          "joinW: non-ASCII base + rel pass the reparse check");
    CHECK(v3x_path_join_checkedW(p, 8, L"C:\\g", L"abcd", 0) == V3X_R_ETRUNC && p[0] == 0 &&
              v3x_path_join_checkedW(p, 10, L"C:\\g", L"abcd", 0) == V3X_R_OK && !wcscmp(p, L"C:\\g\\abcd"),
          "joinW: exact fit / truncation");
    if (made) {
        CHECK(v3x_path_join_checkedW(p, V3X_PATH_WMAX, base, L"link/x.txt", V3X_PATH_CHECK_REPARSE) == V3X_R_EREPARSE && p[0] == 0 &&
                  v3x_path_join_checkedW(p, V3X_PATH_WMAX, base, L"link", V3X_PATH_CHECK_REPARSE) == V3X_R_EREPARSE,
              "joinW: junction under a non-ASCII base -> EREPARSE (component and last)");
        CHECK(v3x_path_has_reparseW(link, 0) == 1 && v3x_path_has_reparseW(real, wcslen(base)) == 0,
              "has_reparseW: handle-based (FILE_FLAG_OPEN_REPARSE_POINT) check");
    } else printf("(mklink /J failed: junction checks skipped)\n");

    /* ANSI variant: the old code converted ANSI bytes as UTF-8 (MB_ERR_INVALID_CHARS) and failed on
     * non-ASCII folders; now CP_ACP */
    if (ac) {
        wchar_t abasew[MAX_PATH];
        char abase[MAX_PATH * 2], ap[MAX_PATH * 3];
        _snwprintf(abasew, MAX_PATH, L"%lsv3x_a_%lu_%lc", tmpw, (unsigned long)GetCurrentProcessId(), ac);
        abasew[MAX_PATH - 1] = 0;
        CreateDirectoryW(abasew, NULL);
        BOOL lossy = FALSE;
        int an = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, abasew, -1, abase, sizeof abase, NULL, &lossy);
        if (an > 0 && !lossy) {
            char sub[16];
            int sn = WideCharToMultiByte(CP_ACP, 0, &ac, 1, sub, sizeof sub - 4, NULL, NULL);
            sub[sn] = 0;
            strcat(sub, "_d");
            char subw[MAX_PATH * 3];
            snprintf(subw, sizeof subw, "%s\\%s", abase, sub);
            CreateDirectoryA(subw, NULL);
            CHECK(v3x_path_join_checked(ap, sizeof ap, abase, sub, V3X_PATH_CHECK_REPARSE) == V3X_R_OK &&
                      GetFileAttributesA(ap) != INVALID_FILE_ATTRIBUTES,
                  "join (ANSI): non-ASCII CP_ACP base + rel pass the reparse check (was EREPARSE with the UTF-8 conversion)");
            wchar_t jl[MAX_PATH];
            _snwprintf(jl, MAX_PATH, L"%ls\\jl", abasew);
            wchar_t jt[MAX_PATH];
            _snwprintf(jt, MAX_PATH, L"%ls\\jt", abasew);
            CreateDirectoryW(jt, NULL);
            if (mk_junction_w(jl, jt)) {
                CHECK(v3x_path_join_checked(ap, sizeof ap, abase, "jl\\x.txt", V3X_PATH_CHECK_REPARSE) == V3X_R_EREPARSE,
                      "join (ANSI): junction under a non-ASCII CP_ACP base -> EREPARSE");
                RemoveDirectoryW(jl);
            }
            RemoveDirectoryW(jt);
            HANDLE h = INVALID_HANDLE_VALUE;
            CHECK(v3x_file_open_checked(abase, "f.txt", GENERIC_WRITE, 0, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, &h) == V3X_R_OK &&
                      h != INVALID_HANDLE_VALUE,
                  "open_checked (ANSI): non-ASCII base");
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            char fa[MAX_PATH * 3];
            snprintf(fa, sizeof fa, "%s\\f.txt", abase);
            DeleteFileA(fa);
            snprintf(fa, sizeof fa, "%s\\atomic_a.txt", abase);
            CHECK(v3x_file_write_atomic(fa, "ansi", 4) == 0 && GetFileAttributesA(fa) != INVALID_FILE_ATTRIBUTES,
                  "write_atomic (ANSI): non-ASCII CP_ACP folder");
            DeleteFileA(fa);
            RemoveDirectoryA(subw);
        } else printf("(ANSI base not representable: ANSI checks skipped)\n");
        CHECK(RemoveDirectoryW(abasew), "ANSI test folder cleaned up");
    } else printf("(no test character in the ANSI code page: ANSI checks skipped)\n");

    /* race-free open */
    HANDLE h = INVALID_HANDLE_VALUE;
    CHECK(v3x_file_open_checkedW(base, L"r\x00E9" L"al/new.txt", GENERIC_WRITE, 0, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, &h) == V3X_R_OK &&
              h != INVALID_HANDLE_VALUE,
          "open_checkedW: plain non-ASCII chain -> handle");
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    h = INVALID_HANDLE_VALUE;
    CHECK(v3x_file_open_checkedW(base, L"r\x00E9" L"al/NEW.TXT", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, 0, &h) == V3X_R_OK,
          "open_checkedW: case-insensitive final-path verification");
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    CHECK(v3x_file_open_checkedW(base, L"../x", GENERIC_READ, 0, OPEN_EXISTING, 0, &h) == V3X_R_EPATH && h == INVALID_HANDLE_VALUE &&
              v3x_file_open_checkedW(base, L"r\x00E9" L"al/missing.txt", GENERIC_READ, 0, OPEN_EXISTING, 0, &h) == V3X_R_EIO &&
              GetLastError() == ERROR_FILE_NOT_FOUND,
          "open_checkedW: traversal -> EPATH, missing -> EIO (GetLastError)");
    if (made) {
        CHECK(v3x_file_open_checkedW(base, L"link/new.txt", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, 0, &h) == V3X_R_EREPARSE &&
                  h == INVALID_HANDLE_VALUE,
              "open_checkedW: junction in the chain -> EREPARSE");
        CHECK(v3x_file_open_checkedW(base, L"link", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, &h) ==
                  V3X_R_EREPARSE,
              "open_checkedW: junction as the last component -> EREPARSE");
    }
    /* the post-open verification: a short (8.3) name passes the name checks but the opened handle's
     * final path differs -> refused and the file this call created is deleted again */
    wchar_t longdir[MAX_PATH], shortp[MAX_PATH];
    _snwprintf(longdir, MAX_PATH, L"%ls\\a_long_directory_name", base);
    CreateDirectoryW(longdir, NULL);
    DWORD sl = GetShortPathNameW(longdir, shortp, MAX_PATH);
    const wchar_t *sn = sl ? wcsrchr(shortp, L'\\') : NULL;
    if (sn && wcscmp(sn + 1, L"a_long_directory_name") != 0) {
        wchar_t rel[64], created[MAX_PATH];
        _snwprintf(rel, 64, L"%ls\\made.txt", sn + 1);
        _snwprintf(created, MAX_PATH, L"%ls\\made.txt", longdir);
        CHECK(v3x_file_open_checkedW(base, rel, GENERIC_WRITE, 0, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, &h) == V3X_R_EREPARSE &&
                  GetFileAttributesW(created) == INVALID_FILE_ATTRIBUTES,
              "open_checkedW: final path mismatch after the open -> EREPARSE, created file removed");
    } else printf("(no 8.3 names on this volume: post-open mismatch check skipped)\n");
    RemoveDirectoryW(longdir);

    /* atomic writes */
    wchar_t f[MAX_PATH];
    char buf[64];
    _snwprintf(f, MAX_PATH, L"%ls\\r\x00E9" L"al\\\x00E9t\x00E9.txt", base);
    CHECK(v3x_file_write_atomicW(f, "one", 3) == 0 && v3x_file_write_atomicW(f, "second", 6) == 0 && read_file_w(f, buf, sizeof buf) == 6 &&
              !strcmp(buf, "second") && count_tmp_files(real) == 0,
          "write_atomicW: non-ASCII path, replace, no .v3xtmp left");
    CHECK(v3x_file_write_atomic_inW(base, L"r\x00E9" L"al/in.txt", "in", 2) == 0, "write_atomic_inW: plain chain");
    CHECK(v3x_file_write_atomic_inW(base, L"../escape.txt", "x", 1) == -1 && GetLastError() == ERROR_INVALID_NAME,
          "write_atomic_inW: traversal refused");
    if (made) {
        wchar_t lf[MAX_PATH];
        _snwprintf(lf, MAX_PATH, L"%ls\\x.txt", link);
        CHECK(v3x_file_write_atomicW(lf, "x", 1) == -1 && GetLastError() == ERROR_REPARSE_POINT_ENCOUNTERED,
              "write_atomicW: parent directory is a junction -> refused");
        CHECK(v3x_file_write_atomic_inW(base, L"link/x.txt", "x", 1) == -1 && GetLastError() == ERROR_REPARSE_POINT_ENCOUNTERED,
              "write_atomic_inW: junction in the chain -> refused");
        CHECK(v3x_file_write_atomic_inW(link, L"via_base.txt", "x", 1) == 0, "write_atomic_inW: a junction AS the trusted base is allowed");
        wchar_t vb[MAX_PATH];
        _snwprintf(vb, MAX_PATH, L"%ls\\via_base.txt", real);
        DeleteFileW(vb);
    }
    /* a planted file at a temp name is never reused: CREATE_NEW picks the next name */
    CHECK(count_tmp_files(real) == 0, "no temp files left after the failures");
    /* OS assumption the writer relies on: a held directory handle without FILE_SHARE_DELETE blocks
     * renaming that directory and its parent */
    wchar_t ren[MAX_PATH], base2[MAX_PATH];
    _snwprintf(ren, MAX_PATH, L"%ls_renamed", real);
    _snwprintf(base2, MAX_PATH, L"%ls_renamed", base);
    HANDLE hd = CreateFileW(real, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    BOOL mv1 = MoveFileW(real, ren), mv2 = FALSE;
    if (made) RemoveDirectoryW(link);   /* the parent can only be renamed without the junction pointing into it */
    mv2 = MoveFileW(base, base2);
    CHECK(hd != INVALID_HANDLE_VALUE && !mv1 && !mv2, "held directory: it and its parent cannot be renamed (%d %d)", mv1, mv2);
    if (mv1) MoveFileW(ren, real);
    if (mv2) MoveFileW(base2, base);
    if (hd != INVALID_HANDLE_VALUE) CloseHandle(hd);

    /* cleanup */
    _snwprintf(f, MAX_PATH, L"%ls\\new.txt", real); DeleteFileW(f);
    _snwprintf(f, MAX_PATH, L"%ls\\in.txt", real); DeleteFileW(f);
    _snwprintf(f, MAX_PATH, L"%ls\\\x00E9t\x00E9.txt", real); DeleteFileW(f);
    RemoveDirectoryW(link);
    RemoveDirectoryW(real);
    CHECK(RemoveDirectoryW(base), "test folder cleaned up");
}

/* 12: bounded decoders, strict patterns, checked V3X_ARG, NULL-safe ini */
static void test_bounded_scan(void)
{
    uint8_t buf[32] = {0xE8, 0x10, 0, 0, 0, 0x48, 0x8B, 0x05, 0xF0, 0xFF, 0xFF, 0xFF, 0x0F, 0x84, 1, 0, 0, 0};
    V3xSection sec = {buf, sizeof buf}, small = {buf, 12}, tail = {buf + 12, 4};
    CHECK(v3x_rel32_target_in(&sec, buf, NULL) == buf + 0x15 && v3x_rel32_target_in(&small, buf, NULL) == NULL &&
              v3x_rel32_target_in(&small, buf, &sec) == buf + 0x15,
          "rel32_in: target must lie in dest (default: code)");
    CHECK(v3x_rel32_target_in(&tail, buf + 12, &sec) == NULL, "rel32_in: instruction cut by the end of code -> NULL");
    CHECK(v3x_rel32_target_in(&small, buf + 20, &sec) == NULL && v3x_rel32_target_in(&small, &sec, &sec) == NULL &&
              v3x_rel32_target_in(&sec, buf + 5, NULL) == NULL,
          "rel32_in: insn outside code / not a branch -> NULL");
    CHECK(v3x_rip_target_in(&sec, buf + 5, 3, 7, NULL) == NULL && v3x_rip_target_in(&small, buf + 5, 3, 7, NULL) == NULL,
          "rip_in: target before the range -> NULL");
    uint8_t big[64] = {0};
    memcpy(big + 16, buf + 5, 7);   /* mov rax, [rip-0x10] at big+16 -> big+23-16 = big+7 */
    V3xSection bs = {big, sizeof big};
    CHECK(v3x_rip_target_in(&bs, big + 16, 3, 7, NULL) == big + 7 && v3x_rip_target_in(&bs, big + 16, 3, 2, NULL) == NULL &&
              v3x_rip_target_in(&bs, big + 16, -1, 7, NULL) == NULL,
          "rip_in: in range; insn_len < disp_off + 4 / negative offset refused");
    big[60] = 0x0F; big[61] = 0x84;
    CHECK(v3x_scan_near_in(&bs, big + 50, 1000, "0F 84") == big + 60 && v3x_scan_near_in(&bs, big + 50, 1000, "0F 84 00 00 00") == NULL &&
              v3x_scan_near_in(&bs, buf, 4, "00") == NULL,
          "scan_near_in: clamped to the end of the section; from outside -> NULL");
    V3xPattern p;
    CHECK(v3x_pattern_compile("+F", &p) == 0 && v3x_pattern_compile("-1", &p) == 0 && v3x_pattern_compile("0x", &p) == 0 &&
              v3x_pattern_compile("4G", &p) == 0 && v3x_pattern_compile("488B05", &p) == 3 && v3x_pattern_compile(NULL, &p) == 0,
          "pattern: '+F' / '-1' / '0x' refused (strtoul accepted them), runs of hex still ok");
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CHECK(v3x_rel32_target(buf) == buf + 0x15 && v3x_rip_target(buf + 5, 3, 7) == buf + 12 - 0x10 &&
              v3x_scan_near(buf, sizeof buf, "0F 84") == buf + 12 && v3x_rel32_target(NULL) == NULL &&
              v3x_rel32_target((void *)(uintptr_t)0x10) == NULL,
          "deprecated wrappers: same results inside a committed region, NULL / unmapped -> NULL");
    /* in an image: the range is the whole image (a .text call to a .text function, a rip load of .data) */
    V3xSection text;
    void *site = NULL;
    if (v3x_section((uintptr_t)GetModuleHandleA(NULL), ".text", &text) && v3x_callers(&text, (void *)use_str, &site, 1) >= 1)
        CHECK(v3x_rel32_target(site) == (void *)use_str && v3x_rel32_target_in(&text, site, NULL) == (void *)use_str,
              "deprecated rel32 wrapper over this exe's image == bounded result");
#pragma clang diagnostic pop
    /* the end of a committed allocation: the wrapper must not read past it */
    uint8_t *pg = (uint8_t *)VirtualAlloc(NULL, 0x2000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD old;
    VirtualProtect(pg + 0x1000, 0x1000, PAGE_NOACCESS, &old);
    pg[0xFFE] = 0xE8;
    pg[0xFF0] = 0x0F; pg[0xFF1] = 0x84;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CHECK(v3x_rel32_target(pg + 0xFFE) == NULL && v3x_scan_near(pg + 0xFF0, 0x100, "0F 84 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00") == NULL,
          "deprecated wrappers stop at the end of the readable region (no fault)");
#pragma clang diagnostic pop
    VirtualFree(pg, 0, MEM_RELEASE);
}

static char g_longdir[520];
static const char *long_dir(void) { return g_longdir; }

static void test_checked_arg_ini(const V3X_API *tk)
{
    V3X_HookCall c;
    memset(&c, 0, sizeof c);
    c.args[15] = 77;
    volatile int i16 = 16, im1 = -1, i3 = 3, i4 = 4;
    CHECK(V3X_ARG(&c, 15, int) == 77 && V3X_ARG(&c, i16, int) == 0 && V3X_ARG(&c, im1, int) == 0 && V3X_ARG((V3X_HookCall *)NULL, 0, int) == 0,
          "V3X_ARG: run-time index out of range / NULL call -> 0");
    V3X_ARGF(&c, i3) = 1.5;
    V3X_ARGF(&c, i4) = 9.0;   /* goes to the dummy, not past xmm[3] */
    CHECK(c.xmm[3].d == 1.5 && c.ret == 0 && V3X_ARGF(&c, i4) == 0.0, "V3X_ARGF: assignable, out of range -> dummy");

    V3xIni ini;
    CHECK(v3x_ini_parse(&ini, "[s]\nbig = 99999999999999999999\nneg = -5\nr = 2.5\nnan = nan\ninf = 1e999\nk = v\n") == 0, "ini parse");
    CHECK(v3x_ini_int(&ini, "s", "big", 3) == 3, "ini: overflowing integer -> default (was LLONG_MAX)");
    CHECK(v3x_ini_int_range(&ini, "s", "neg", 7, 0, 10) == 7 && v3x_ini_int_range(&ini, "s", "neg", 7, -10, 10) == -5 &&
              v3x_ini_int_range(&ini, "s", "neg", 7, 10, 0) == 7 && v3x_ini_int_range(&ini, "s", "missing", 4, 0, 1) == 4,
          "ini: int_range (out of range / lo > hi / missing -> default)");
    CHECK(v3x_ini_double_range(&ini, "s", "r", 1.0, 0.0, 3.0) == 2.5 && v3x_ini_double_range(&ini, "s", "r", 1.0, 0.0, 2.0) == 1.0 &&
              v3x_ini_double_range(&ini, "s", "nan", 1.0, -1e9, 1e9) == 1.0 && v3x_ini_double_range(&ini, "s", "inf", 1.0, -1e308, 1e308) == 1.0,
          "ini: double_range refuses out of range, nan, inf");
    char out[4] = "zz";
    CHECK(v3x_ini_get(&ini, "s", NULL) == NULL && v3x_ini_int(&ini, "s", NULL, 5) == 5 && v3x_ini_bool(NULL, "s", "k", 1) == 1 &&
              v3x_ini_get(NULL, NULL, "k") == NULL && !strcmp(v3x_ini_str(&ini, "s", NULL, "d", out, sizeof out), "d") &&
              !strcmp(v3x_ini_str(&ini, "s", "k", "d", NULL, 0), ""),
          "ini: NULL ini / key / out -> default, no crash");
    CHECK(!strcmp(v3x_ini_str(&ini, "s", "big", NULL, out, sizeof out), "999"), "ini_str: cut to cap");
    v3x_ini_free(&ini);
    v3x_ini_free(NULL);
    CHECK(v3x_ini_parse(&ini, NULL) == 0 && ini.n == 0 && v3x_ini_load(&ini, NULL) == 0 && !ini.found, "ini: NULL text / path = empty");
    v3x_ini_free(&ini);
    /* load_plugin: refuse instead of truncating */
    char longname[600];
    memset(longname, 'a', sizeof longname - 1);
    longname[sizeof longname - 1] = 0;
    longname[200] = 0;   /* 200 chars: a valid name, but dir + name + ".ini" > 520 once the dir is long */
    CHECK(v3x_ini_load_plugin(&ini, tk, "../evil") == -1 && ini.n == 0 && v3x_ini_int(&ini, "a", "b", 6) == 6, "ini_load_plugin: '../evil' refused");
    v3x_ini_free(&ini);
    CHECK(v3x_ini_load_plugin(&ini, tk, "a/b") == -1 && v3x_ini_load_plugin(&ini, tk, NULL) == -1 && v3x_ini_load_plugin(&ini, NULL, "x") == -1,
          "ini_load_plugin: path separators / NULL refused");
    v3x_ini_free(&ini);
    V3X_API a = *tk;
    memset(g_longdir, 'd', 400);
    g_longdir[400] = '\\';
    g_longdir[401] = 0;
    a.v3x_dir = long_dir;
    CHECK(v3x_ini_load_plugin(&ini, &a, longname) == -1 && ini.n == 0 && !ini.found, "ini_load_plugin: path longer than its buffer -> -1 (never truncated)");
    v3x_ini_free(&ini);
    CHECK(v3x_ini_load_plugin(&ini, &a, "x") == 0 && !ini.found, "ini_load_plugin: a long dir that still fits is fine");
    v3x_ini_free(&ini);
}

/* N11: v3x_script_get never loads the DLL, matches the plugin folder by full path, checks the table */
static void test_script_get(void)
{
    CHECK(GetModuleHandleA(V3X_SCRIPT_DLL) == NULL && v3x_script_get(NULL) == NULL, "script_get: not loaded -> NULL");
    const char *src = "build/plugins/script_ext.dll";
    if (GetFileAttributesA(src) == INVALID_FILE_ATTRIBUTES) { printf("(no %s: script_get load checks skipped)\n", src); return; }
    wchar_t exe[MAX_PATH], here[MAX_PATH], other[MAX_PATH], srcw[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    *wcsrchr(exe, L'\\') = 0;
    _snwprintf(here, MAX_PATH, L"%ls\\%ls", exe, V3X_SCRIPT_DLL_W);
    _snwprintf(other, MAX_PATH, L"%ls\\sdk_test_other_%lu", exe, (unsigned long)GetCurrentProcessId());
    CreateDirectoryW(other, NULL);
    wcscat(other, L"\\" V3X_SCRIPT_DLL_W);
    MultiByteToWideChar(CP_ACP, 0, src, -1, srcw, MAX_PATH);
    int copied = CopyFileW(srcw, here, FALSE) && CopyFileW(srcw, other, FALSE);
    CHECK(copied, "copied script_ext.dll next to the test exe and into another folder");
    if (!copied) return;
    CHECK(v3x_script_get(NULL) == NULL && GetModuleHandleW(here) == NULL,
          "script_get: a script_ext.dll file next to the caller is NOT loaded by the helper");
    HMODULE ho = LoadLibraryW(other);
    CHECK(ho && v3x_script_get(NULL) == NULL, "script_get: a script_ext.dll loaded from another folder is ignored");
    HMODULE hh = LoadLibraryW(here);   /* what the loader does for <v3x>/plugins/script_ext.dll */
    const V3X_ScriptExt *t = v3x_script_get(NULL);
    CHECK(hh && t && t->struct_size >= offsetof(V3X_ScriptExt, packs_dir) + sizeof(t->packs_dir) && t->begin_pack && t->packs_dir &&
              t->version >= 1,
          "script_get: the module loaded from the caller's folder -> its table (v%u, %u bytes)", t ? t->version : 0, t ? t->struct_size : 0);
    CHECK(!t || V3X_SCRIPT_HAS(t, features) == (t->struct_size >= offsetof(V3X_ScriptExt, features) + sizeof(void *)),
          "V3X_SCRIPT_HAS uses offset + size");
    if (hh) FreeLibrary(hh);
    if (ho) FreeLibrary(ho);
    DeleteFileW(here);
    DeleteFileW(other);
    *wcsrchr(other, L'\\') = 0;
    RemoveDirectoryW(other);
}

static void test_safe(void)
{
    test_safe_str();
    test_safe_span();
    test_safe_math();
    test_safe_path();
    test_safe_misc();
    test_safe_invariants();
    test_safe_paths_w();
    test_bounded_scan();
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
    test_util(&api);
    test_safe();
    test_checked_arg_ini(&api);
    test_script_get();
    if (argc > 1) test_game(argv[1]);
    else printf("(no exe given: game checks skipped)\n");
    return v3x_testkit_done();
}
