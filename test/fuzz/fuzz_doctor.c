/* Fuzz harness: plugins/doctor/dr_core.c - the comment-preserving ini editor (dr_ini_get / dr_ini_set),
 * the v3x.log and `plugins` output parsers, the crash.log parser and the report masker.
 * Invariants: dr_ini_set either reports "nothing to change" or returns new text in which dr_ini_get
 * finds exactly the new value; setting the same value again changes nothing; every parsed plugin row
 * and result text is NUL-terminated and free of control characters (L10). */
#include "v3x_fuzz.h"
#include "../../plugins/doctor/dr_core.c"

static void fuzz_ini(const uint8_t *d, size_t n)
{
    const char *t = v3x_fuzz_cstr(d, n);
    /* section / key / value picked from a few realistic ones, by the first byte */
    static const char *const sec[] = {"plugins", "", "alloc_tracker", "log"};
    static const char *const key[] = {"console_plus", "unknown_build", "enabled", "level"};
    static const char *const val[] = {"0", "safe", "1", "debug"};
    int i = n ? d[0] & 3 : 0, any = n > 1 && (d[1] & 1);
    char *out = NULL, old[64], got[128];
    int r = dr_ini_set(t, sec[i], key[i], val[i], any, &out, old, sizeof old);
    V3X_FUZZ_ASSERT(r >= -1 && r <= 1);
    V3X_FUZZ_ASSERT(memchr(old, 0, sizeof old) != NULL);
    if (r == 1) {
        V3X_FUZZ_ASSERT(out != NULL);
        V3X_FUZZ_ASSERT(dr_ini_get(out, sec[i], key[i], any, got, sizeof got) == 1);
        char *again = NULL;
        int r2 = dr_ini_set(out, sec[i], key[i], val[i], any, &again, old, sizeof old);
        V3X_FUZZ_ASSERT(r2 == 0 && again == NULL);
        free(out);
    } else {
        V3X_FUZZ_ASSERT(out == NULL);
    }
    (void)dr_ini_get(t, "plugins", "hello", 0, got, sizeof got);
    (void)dr_ini_get(t, "", "", 1, got, sizeof got);
}
V3X_FUZZ_TARGET(dr_ini, fuzz_ini)
V3X_FUZZ_SEED(dr_ini, "\x00# my settings\r\nconsole=0\r\nunknown_build = load ; was load\r\n\r\n[plugins]\r\nghost=1\r\nconsole_plus = 1 ; c\r\n[plugins]\r\nconsole_plus=1\r\n")
V3X_FUZZ_SEED(dr_ini, "\x02\x01[alloc_tracker]\nenabled=1\n[ALLOC_TRACKER ]\n  Enabled\t=\t1\t#x\n[log]\nlevel\n=\n[\n")

static void check_text(const char *s, size_t cap)
{
    V3X_FUZZ_ASSERT(memchr(s, 0, cap) != NULL);
    for (const char *p = s; *p; p++) V3X_FUZZ_ASSERT((unsigned char)*p >= 0x20 && *p != 0x7f);
}

static void fuzz_logs(const uint8_t *d, size_t n)
{
    const char *t = v3x_fuzz_cstr(d, n);
    static dr_plugins ps;
    memset(&ps, 0, sizeof ps);
    dr_parse_log(t, &ps);
    dr_parse_plugins_cmd(t, &ps);
    V3X_FUZZ_ASSERT(ps.n >= 0 && ps.n <= DR_MAX_PLUGINS);
    for (int i = 0; i < ps.n; i++) {
        V3X_FUZZ_ASSERT(memchr(ps.p[i].stem, 0, sizeof ps.p[i].stem) && memchr(ps.p[i].why, 0, sizeof ps.p[i].why));
    }
    static const char *const stems[] = {"console_plus", "fast_load", "mod_menu"};
    dr_crash c;
    memset(&c, 0, sizeof c);
    dr_crash_parse(t, stems, 3, &c);
    char *m = dr_mask(t, "klee", "C:\\Users\\klee");
    if (m) { V3X_FUZZ_ASSERT(!strstr(m, "C:\\Users\\klee")); free(m); }
    /* result rows quote parsed text: sanitised */
    static dr_results R;
    memset(&R, 0, sizeof R);
    dr_result *r = add(&R, "D005.x", "plugin_status", DR_WARN, "%s: %s", ps.n ? ps.p[0].stem : "none", t);
    todo(r, "%s", t);
    check_text(r->msg, sizeof r->msg);
    check_text(r->todo, sizeof r->todo);
    char line[1200];
    (void)dr_format_result(r, line, sizeof line);
    V3X_FUZZ_ASSERT(memchr(line, 0, sizeof line) != NULL);
}
V3X_FUZZ_TARGET(dr_logs, fuzz_logs)
V3X_FUZZ_SEED(dr_logs, "[20:00:00.000][v3x] V3X 0.6.0 loading\n[20:00:00.001][v3x] plugin console_plus: loaded\n[20:00:00.002][v3x] plugin hello disabled (v3x.ini)\n"
                       "[20:00:00.003][v3x] plugin broken: LoadLibrary failed (error 193)\n[20:00:01.000][v3x] 3/4 plugins loaded\n"
                       "[par_load] anchor realdir FAILED: need 1 shared getRealDir callee (not hooked)\n")
V3X_FUZZ_SEED(dr_logs, "plugins:\n  console_plus 0.5.0 loaded\n  fast_load  disabled (v3x.ini)\n  x refused: loader too old\n"
                       "crash_logger started\nUNHANDLED EXCEPTION 0xc0000005 at console_plus.dll+0x1234 - process is going down\n"
                       "  stack: victoria3.exe+0x10 console_plus.dll+0x20\nmemory: 1949 MB available\nC:\\Users\\klee\\Documents x\n")
