/* db_core.h - pure (OS-free) part of plugins/devbridge: request line splitting, line parsing, the
 * command allow list and the token check. Included by devbridge.c, test/devbridge_test.c and
 * test/fuzz/fuzz_devbridge.c. */
#ifndef DB_CORE_H
#define DB_CORE_H
#include <stddef.h>
#include <string.h>

#define DB_REQ_MAX 4096
#define DB_TOKEN_HEX 64                       /* 32 random bytes as lowercase hex */
#define DB_ALLOW_MAX 64
#define DB_ALLOW_LEN 48

/* Read-only by default: inspection commands only (no command that changes the game, a file or a
 * setting). "x$" = the bare command without arguments. `allow = *` in devbridge.ini allows all. */
#define DB_ALLOW_DEFAULT "help, version, plugins, mods, dis, fn, xref, mem, scan, memstat, profile report, ticks$, doctor$"

typedef struct {
    int all;                                  /* allow = * */
    int n;
    char e[DB_ALLOW_MAX][DB_ALLOW_LEN];       /* lowercased words separated by one space; "$" suffix = exact */
} db_allow;

static int db_lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int db_ws(int c) { return c == ' ' || c == '\t'; }

/* Parses "a, b c, d$" (also ';' as separator). Overlong / surplus entries are dropped. */
static void db_allow_parse(db_allow *a, const char *s)
{
    memset(a, 0, sizeof *a);
    while (s && *s) {
        while (*s == ',' || *s == ';' || db_ws(*s)) s++;
        if (!*s) break;
        char buf[DB_ALLOW_LEN];
        size_t n = 0;
        int over = 0, sp = 0;
        for (; *s && *s != ',' && *s != ';'; s++) {
            if (db_ws(*s)) { sp = 1; continue; }
            if (sp && n) { if (n + 1 < sizeof buf) buf[n++] = ' '; else over = 1; }
            sp = 0;
            if (n + 1 < sizeof buf) buf[n++] = (char)db_lc((unsigned char)*s); else over = 1;
        }
        buf[n] = 0;
        if (!n || over) continue;
        if (!strcmp(buf, "*")) { a->all = 1; continue; }
        if (a->n < DB_ALLOW_MAX) memcpy(a->e[a->n++], buf, n + 1);
    }
}

/* 1 if the command line (leading blanks skipped) is allowed: its leading words equal an entry's
 * words (case-insensitive) and the next char is a blank or the end ("$" entries: the end only). */
static int db_allowed(const db_allow *a, const char *cmd)
{
    if (a->all) return 1;
    while (db_ws(*cmd)) cmd++;
    if (!*cmd) return 0;
    for (int i = 0; i < a->n; i++) {
        const char *e = a->e[i], *c = cmd;
        int exact = 0;
        for (;;) {
            if (*e == '$' && !e[1]) { exact = 1; e++; break; }
            if (!*e) break;
            if (*e == ' ') {
                if (!db_ws(*c)) goto next;
                while (db_ws(*c)) c++;
                e++;
                continue;
            }
            if (db_lc((unsigned char)*c) != *e) goto next;
            c++; e++;
        }
        if (exact) { while (db_ws(*c)) c++; if (!*c) return 1; }
        else if (!*c || db_ws(*c)) return 1;
    next:;
    }
    return 0;
}

/* Splits "<id> <command>" in place (trailing CR/blanks removed). Returns 0 = blank line (ignore),
 * 1 = ok (*id, *cmd set; *cmd may be ""), 2 = id longer than 32 chars (*id = "0", *cmd = ""). */
static int db_parse_line(char *line, char **id, char **cmd)
{
    size_t L = strlen(line);
    while (L && (line[L - 1] == '\r' || db_ws(line[L - 1]))) line[--L] = 0;
    char *s = line;
    while (db_ws(*s)) s++;
    if (!*s) return 0;
    size_t i = 0;
    while (s[i] && !db_ws(s[i])) i++;
    if (i > 32) { static char zero[] = "0", none[] = ""; *id = zero; *cmd = none; return 2; }
    char *c = s + i;
    if (*c) { *c++ = 0; while (db_ws(*c)) c++; }
    *id = s; *cmd = c;
    return 1;
}

/* Feeds the bytes buf[0..have) to on_line for every complete '\n'-terminated line (the '\n' is
 * replaced by NUL), then moves the incomplete rest to the front. Returns the bytes kept, or
 * (size_t)-1 if on_line asked to stop (returned < 0) or the kept rest reached DB_REQ_MAX (line
 * too long; *too_long = 1). buf must have room for have + 1 bytes. */
typedef int (*db_line_fn)(char *line, void *u);
static size_t db_feed(char *buf, size_t have, db_line_fn on_line, void *u, int *too_long)
{
    size_t start = 0;
    *too_long = 0;
    for (size_t i = 0; i < have; i++) {
        if (buf[i] != '\n') continue;
        buf[i] = 0;
        if (on_line(buf + start, u) < 0) return (size_t)-1;
        start = i + 1;
    }
    if (start) { memmove(buf, buf + start, have - start); have -= start; }
    if (have >= DB_REQ_MAX) { *too_long = 1; return (size_t)-1; }
    return have;
}

/* The first line of a connection must be "auth <64 hex>"; constant time over the token. 1 = ok. */
static int db_auth_ok(const char *line, const char *token)
{
    const char *s = line;
    while (db_ws(*s)) s++;
    if (db_lc((unsigned char)s[0]) != 'a' || db_lc((unsigned char)s[1]) != 'u' || db_lc((unsigned char)s[2]) != 't' ||
        db_lc((unsigned char)s[3]) != 'h' || !db_ws(s[4]))
        return 0;
    s += 4;
    while (db_ws(*s)) s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || db_ws(s[n - 1]))) n--;
    if (n != DB_TOKEN_HEX || strlen(token) != DB_TOKEN_HEX) return 0;
    unsigned d = 0;
    for (size_t i = 0; i < DB_TOKEN_HEX; i++) d |= (unsigned)(db_lc((unsigned char)s[i]) ^ (unsigned char)token[i]);
    return d == 0;
}
#endif
