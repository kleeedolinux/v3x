/* Fuzz harness: plugins/devbridge/db_core.h (audit M5): the request line splitter (db_feed), the
 * "<id> <command>" parser, the allow list parser / matcher and the auth line check.
 * Invariants: every line handed to the callback lies inside the buffer and has no '\n'; the kept rest
 * is < DB_REQ_MAX and is a suffix of the input; ids are <= 32 chars; an allowed command under the
 * default list starts with one of its words; "auth <token>" is accepted only for the exact token. */
#include "v3x_fuzz.h"
#include "../../plugins/devbridge/db_core.h"

typedef struct { const char *lo, *hi; int lines; db_allow *a; } feed_ctx;

static int on_line(char *line, void *u)
{
    feed_ctx *c = (feed_ctx *)u;
    size_t n = strlen(line);
    V3X_FUZZ_ASSERT(line >= c->lo && line + n < c->hi && !memchr(line, '\n', n));
    c->lines++;
    char *id, *cmd;
    int r = db_parse_line(line, &id, &cmd);
    if (r == 1) {
        V3X_FUZZ_ASSERT(strlen(id) >= 1 && strlen(id) <= 32 && !strchr(id, ' ') && !strchr(id, '\t'));
        V3X_FUZZ_ASSERT(*cmd != ' ' && *cmd != '\t');
        (void)db_allowed(c->a, cmd);
    } else if (r == 2) {
        V3X_FUZZ_ASSERT(!strcmp(id, "0") && !*cmd);
    }
    return c->lines > 200 ? -1 : 0;
}

static void fuzz_feed(const uint8_t *d, size_t n)
{
    /* the bridge's own buffer: DB_REQ_MAX + 1, filled by reads of up to DB_REQ_MAX - have bytes */
    static char buf[DB_REQ_MAX + 1];
    static db_allow a;
    db_allow_parse(&a, DB_ALLOW_DEFAULT);
    size_t have = 0, pos = 0;
    while (pos < n) {
        size_t chunk = 1 + (d[pos] & 127);             /* uneven read sizes */
        if (chunk > n - pos) chunk = n - pos;
        if (chunk > DB_REQ_MAX - have) chunk = DB_REQ_MAX - have;
        memcpy(buf + have, d + pos, chunk);
        pos += chunk;
        have += chunk;
        feed_ctx c = {buf, buf + have + 1, 0, &a};
        int too_long = 0;
        size_t kept = db_feed(buf, have, on_line, &c, &too_long);
        if (kept == (size_t)-1) return;           /* stop or too long: the bridge closes the connection */
        V3X_FUZZ_ASSERT(kept < DB_REQ_MAX && !memchr(buf, '\n', kept));
        have = kept;
    }
}
V3X_FUZZ_TARGET(devbridge_feed, fuzz_feed)
V3X_FUZZ_SEED(devbridge_feed, "auth 0123\n1 help\r\n2 dis +1000 3\n  \n\t9  mods   devbridge.dll  \nxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx cmd\n")
V3X_FUZZ_SEED(devbridge_feed, "a5 version\r\nb6 profile report\n7 profile on\n8 ticks reset\n8b ticks\n9 doctor fix D007\n")

static void fuzz_allow(const uint8_t *d, size_t n)
{
    /* first line = allow= value, the rest = command lines */
    const char *t = v3x_fuzz_cstr(d, n);
    const char *nl = strchr(t, '\n');
    char spec[1024];
    size_t sl = nl ? (size_t)(nl - t) : strlen(t);
    if (sl >= sizeof spec) sl = sizeof spec - 1;
    memcpy(spec, t, sl); spec[sl] = 0;
    static db_allow a;
    db_allow_parse(&a, spec);
    V3X_FUZZ_ASSERT(a.n >= 0 && a.n <= DB_ALLOW_MAX);
    for (int i = 0; i < a.n; i++) V3X_FUZZ_ASSERT(strlen(a.e[i]) > 0 && strlen(a.e[i]) < DB_ALLOW_LEN);
    for (const char *p = nl ? nl + 1 : NULL; p && *p;) {
        const char *e = strchr(p, '\n');
        char line[256];
        size_t ln = e ? (size_t)(e - p) : strlen(p);
        if (ln >= sizeof line) ln = sizeof line - 1;
        memcpy(line, p, ln); line[ln] = 0;
        int ok = db_allowed(&a, line);
        if (ok && !a.all) {                        /* allowed => the first word equals an entry's first word */
            const char *c = line; while (db_ws(*c)) c++;
            int found = 0;
            for (int i = 0; i < a.n && !found; i++) {
                size_t k = strcspn(a.e[i], " $");
                found = !_strnicmp(c, a.e[i], k) && (!c[k] || db_ws(c[k]));
            }
            V3X_FUZZ_ASSERT(found);
        }
        p = e ? e + 1 : NULL;
    }
    /* the default list never allows a state-changing command */
    static db_allow def;
    db_allow_parse(&def, DB_ALLOW_DEFAULT);
    V3X_FUZZ_ASSERT(!db_allowed(&def, "reload x") && !db_allowed(&def, "ticks reset") && !db_allowed(&def, "doctor fix D007") &&
                    !db_allowed(&def, "profile on") && !db_allowed(&def, "helpx") && db_allowed(&def, " HELP "));
}
V3X_FUZZ_TARGET(devbridge_allow, fuzz_allow)
V3X_FUZZ_SEED(devbridge_allow, "help, version, plugins, mods, dis, fn, xref, mem, scan, memstat, profile report, ticks$, doctor$\nhelp\nticks\nticks reset\nprofile  report\nprofile on\ndoctor\n")
V3X_FUZZ_SEED(devbridge_allow, ";;, *, a b c d e f g h i j k l m n o p q r s t u v w x y z a b c d e f g h i j k l m n\nanything\n")

static void fuzz_auth(const uint8_t *d, size_t n)
{
    static const char tok[] = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    const char *t = v3x_fuzz_cstr(d, n);
    int ok = db_auth_ok(t, tok);
    if (ok) {                                       /* only "auth" + blanks + exactly the token (+ blanks / CR) */
        const char *p = t; while (db_ws(*p)) p++;
        V3X_FUZZ_ASSERT(!_strnicmp(p, "auth", 4) && db_ws(p[4]));
        p += 4; while (db_ws(*p)) p++;
        V3X_FUZZ_ASSERT(strlen(p) >= DB_TOKEN_HEX && !_strnicmp(p, tok, DB_TOKEN_HEX));
        for (const char *q = p + DB_TOKEN_HEX; *q; q++) V3X_FUZZ_ASSERT(*q == '\r' || db_ws(*q));
    }
    char good[100];
    snprintf(good, sizeof good, "auth %s", tok);
    V3X_FUZZ_ASSERT(db_auth_ok(good, tok) && !db_auth_ok("auth ", tok) && !db_auth_ok(tok, tok));
}
V3X_FUZZ_TARGET(devbridge_auth, fuzz_auth)
V3X_FUZZ_SEED(devbridge_auth, "auth 00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\r")
V3X_FUZZ_SEED(devbridge_auth, "AUTH\t00112233445566778899AABBCCDDEEFF00112233445566778899aabbccddeef0")
