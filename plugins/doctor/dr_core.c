/* dr_core.c - doctor checks over collected inputs (see dr_core.h). */
#include "dr_core.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "v3x_util.h"
#include "v3x_safe.h"

#define GB (1024.0 * 1024.0 * 1024.0)

/* ------------------------------------------------------------------ text helpers */

static int dr_ieq(const char *a, const char *b) { return a && b && !_stricmp(a, b); }
static int dr_istarts(const char *s, const char *p) { return s && p && !_strnicmp(s, p, strlen(p)); }
static const char *dr_istr(const char *h, const char *n)
{
    size_t k = strlen(n);
    if (!h || !k) return NULL;
    for (; *h; h++)
        if (!_strnicmp(h, n, k)) return h;
    return NULL;
}
static void cpy(char *d, size_t n, const char *s, size_t len)
{
    if (!n) return;
    if (len >= n) len = n - 1;
    memcpy(d, s, len);
    d[len] = 0;
}
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    return s;
}
/* next line of text into buf (no newline / \r); returns the start of the line or NULL at the end */
static const char *next_line(const char **p, char *buf, size_t n)
{
    const char *s = *p;
    if (!s || !*s) return NULL;
    const char *e = strchr(s, '\n');
    size_t len = e ? (size_t)(e - s) : strlen(s);
    *p = e ? e + 1 : s + len;
    size_t k = len;
    if (k && s[k - 1] == '\r') k--;
    cpy(buf, n, s, k);
    return s;
}

int dr_truthy(const char *v)
{
    if (!v) return 0;
    while (*v == ' ' || *v == '\t') v++;
    if ((*v >= '0' && *v <= '9') || *v == '-' || *v == '+') return atoi(v) != 0;
    return dr_istarts(v, "true") || dr_istarts(v, "yes") || dr_ieq(v, "on");
}

static int bool_word(const char *v, int *b)
{
    static const char *t[] = {"1", "true", "yes", "on"}, *f[] = {"0", "false", "no", "off"};
    for (int i = 0; i < 4; i++) {
        if (dr_ieq(v, t[i])) { *b = 1; return 1; }
        if (dr_ieq(v, f[i])) { *b = 0; return 1; }
    }
    return 0;
}
static int val_eq(const char *a, const char *b)
{
    int x, y;
    if (dr_ieq(a, b)) return 1;
    return bool_word(a, &x) && bool_word(b, &y) && x == y;
}

int dr_list_has(const char *list, const char *item)
{
    if (!list || !item || !*item) return 0;
    size_t k = strlen(item);
    const char *p = list;
    while (*p) {
        while (*p == ';' || *p == ' ') p++;
        const char *e = p;
        while (*e && *e != ';') e++;
        const char *t = e;
        while (t > p && t[-1] == ' ') t--;
        if ((size_t)(t - p) == k && !_strnicmp(p, item, k)) return 1;
        p = e;
    }
    return 0;
}

static const char *ini_text(const dr_inputs *in, const char *name)
{
    for (int i = 0; i < in->ninis; i++)
        if (dr_ieq(in->inis[i].name, name)) return in->inis[i].text;
    return NULL;
}

static V3X_PRINTF(5, 6) dr_result *add(dr_results *o, const char *id, const char *name, int sev, const char *fmt, ...)
{
    static dr_result spare;   /* table full: write into a scratch row */
    dr_result *r = o->n < DR_MAX_RESULTS ? &o->r[o->n++] : &spare;
    memset(r, 0, sizeof *r);
    snprintf(r->id, sizeof r->id, "%s", id);
    r->name = name;
    r->sev = sev;
    va_list ap;
    va_start(ap, fmt);
    char raw[sizeof r->msg];
    vsnprintf(raw, sizeof raw, fmt, ap);
    va_end(ap);
    v3x_sanitize_line(r->msg, sizeof r->msg, raw);   /* L10: details quote mod / log / file text */
    return r;
}
static V3X_PRINTF(2, 3) void todo(dr_result *r, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char raw[sizeof r->todo];
    vsnprintf(raw, sizeof raw, fmt, ap);
    va_end(ap);
    v3x_sanitize_line(r->todo, sizeof r->todo, raw);
}
static void set_fix(dr_result *r, const char *file, const char *section, const char *key, const char *value, int any)
{
    r->has_fix = 1;
    snprintf(r->fix.file, sizeof r->fix.file, "%s", file);
    snprintf(r->fix.section, sizeof r->fix.section, "%s", section);
    snprintf(r->fix.key, sizeof r->fix.key, "%s", key);
    snprintf(r->fix.value, sizeof r->fix.value, "%s", value);
    r->fix.any_section = any;
}
static void fix_disable_plugin(dr_result *r, const char *stem) { set_fix(r, "v3x.ini", "plugins", stem, "0", 0); }

/* ------------------------------------------------------------------ ini read / edit */

typedef struct ini_line {
    const char *s, *e;    /* content [s, e) without \r\n */
    const char *next;     /* start of the next line */
    int kind;             /* 0 blank/comment/other, 1 section header, 2 key */
    char section[64];     /* section the line is in (for a header: its own name) */
    const char *ks, *ke;  /* key */
    const char *vs, *ve;  /* value (before an inline comment, trimmed) */
} ini_line;

static int ini_lines(const char *text, ini_line **out)
{
    int n = 0, cap = 0;
    ini_line *v = NULL;
    char sec[64] = "";
    const char *p = text;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
    while (*p) {
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            ini_line *q = (ini_line *)realloc(v, (size_t)cap * sizeof *v);
            if (!q) { free(v); return -1; }
            v = q;
        }
        ini_line *L = &v[n++];
        memset(L, 0, sizeof *L);
        const char *nl = strchr(p, '\n');
        L->s = p;
        L->next = nl ? nl + 1 : p + strlen(p);
        L->e = nl ? nl : L->next;
        if (L->e > L->s && L->e[-1] == '\r') L->e--;
        const char *s = L->s;
        while (s < L->e && (*s == ' ' || *s == '\t')) s++;
        if (s < L->e && *s == '[') {
            const char *c = memchr(s, ']', (size_t)(L->e - s));
            if (c) {
                const char *a = s + 1, *b = c;
                while (a < b && (*a == ' ' || *a == '\t')) a++;
                while (b > a && (b[-1] == ' ' || b[-1] == '\t')) b--;
                cpy(sec, sizeof sec, a, (size_t)(b - a));
                L->kind = 1;
            }
        } else if (s < L->e && *s != ';' && *s != '#') {
            const char *eq = memchr(s, '=', (size_t)(L->e - s));
            if (eq) {
                const char *ke = eq;
                while (ke > s && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
                if (ke > s) {
                    L->kind = 2;
                    L->ks = s; L->ke = ke;
                    const char *vs = eq + 1;
                    while (vs < L->e && (*vs == ' ' || *vs == '\t')) vs++;
                    const char *ve = L->e;
                    for (const char *c = vs; c < L->e; c++)
                        if ((*c == ';' || *c == '#') && c > vs && (c[-1] == ' ' || c[-1] == '\t')) { ve = c; break; }
                    while (ve > vs && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
                    L->vs = vs; L->ve = ve;
                }
            }
        }
        snprintf(L->section, sizeof L->section, "%s", sec);
        p = L->next;
    }
    *out = v;
    return n;
}

static int line_in_scope(const ini_line *L, const char *section, int any)
{
    return any || !_stricmp(L->section, section ? section : "");
}
static int key_is(const ini_line *L, const char *key)
{
    size_t k = strlen(key);
    return L->kind == 2 && (size_t)(L->ke - L->ks) == k && !_strnicmp(L->ks, key, k);
}

int dr_ini_get(const char *text, const char *section, const char *key, int any_section, char *out, size_t n)
{
    ini_line *v = NULL;
    int found = 0;
    if (out && n) out[0] = 0;
    if (!text || !key) return 0;
    int c = ini_lines(text, &v);
    for (int i = 0; i < c; i++)
        if (key_is(&v[i], key) && line_in_scope(&v[i], section, any_section)) {
            cpy(out, n, v[i].vs, (size_t)(v[i].ve - v[i].vs));
            found = 1;
        }
    free(v);
    return found;
}

typedef struct sbuf { char *p; size_t n, cap; int oom; } sbuf;
static void sb_put(sbuf *b, const char *s, size_t n)
{
    if (b->oom) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        while (cap < b->n + n + 1) cap *= 2;
        char *q = (char *)realloc(b->p, cap);
        if (!q) { b->oom = 1; return; }
        b->p = q; b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void sb_str(sbuf *b, const char *s) { sb_put(b, s, strlen(s)); }

int dr_ini_set(const char *text, const char *section, const char *key, const char *value, int any_section,
               char **out, char *old, size_t oldn)
{
    ini_line *v = NULL;
    if (old && oldn) old[0] = 0;
    *out = NULL;
    if (!text) text = "";
    if (!section) section = "";
    const char *nl = strstr(text, "\r\n") ? "\r\n" : "\n";
    int c = ini_lines(text, &v);
    if (c < 0) return -1;
    int matches = 0, differ = 0, have_section = 0, first_header = -1;
    int target_last = -1;   /* last non-blank line of the target region (insertion point) */
    for (int i = 0; i < c; i++) {
        if (v[i].kind == 1 && first_header < 0) first_header = i;
        if (v[i].kind == 1 && *section && !_stricmp(v[i].section, section)) have_section = 1;
        if (key_is(&v[i], key) && line_in_scope(&v[i], section, any_section)) {
            if (!matches && old) cpy(old, oldn, v[i].vs, (size_t)(v[i].ve - v[i].vs));
            char cur[256];
            cpy(cur, sizeof cur, v[i].vs, (size_t)(v[i].ve - v[i].vs));
            matches++;
            if (!val_eq(cur, value)) differ++;
        }
    }
    if (matches && !differ) { free(v); return 0; }
    /* insertion target: [section] if it exists (or named and not any), else the top of the file */
    int into_named = *section && (have_section || !any_section);
    if (!matches) {
        int last_key = -1;   /* last key line (or the header) of the target region */
        for (int i = 0; i < c; i++) {
            int in_region = into_named ? !_stricmp(v[i].section, section) : (first_header < 0 || i < first_header);
            if (in_region && (v[i].kind == 2 || (into_named && v[i].kind == 1))) last_key = i;
        }
        if (into_named || last_key >= 0) target_last = last_key;
        else if (first_header > 0) target_last = first_header - 1;   /* top has only comments: before the first [section] */
        else if (first_header < 0) target_last = c - 1;              /* flat file of comments: at the end */
        else target_last = -1;                                        /* [section] on line 1: at the very top */
    }
    sbuf b = {0};
    const char *body = text;
    if ((unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
        sb_put(&b, text, 3);
        body = text + 3;
    }
    char kv[256];
    snprintf(kv, sizeof kv, "%s = %s", key, value);
    if (!matches && !into_named && target_last < 0) {   /* top region empty: the line goes first */
        sb_str(&b, kv); sb_str(&b, nl);
    }
    if (c == 0) sb_str(&b, body);
    for (int i = 0; i < c; i++) {
        const ini_line *L = &v[i];
        if (matches && key_is(L, key) && line_in_scope(L, section, any_section)) {
            sb_put(&b, L->s, (size_t)(L->vs - L->s));
            sb_str(&b, value);
            sb_put(&b, L->ve, (size_t)(L->next - L->ve));
        } else {
            sb_put(&b, L->s, (size_t)(L->next - L->s));
        }
        if (!matches && i == target_last) {
            if (L->next[-1] != '\n') sb_str(&b, nl);   /* last line had no newline (also a bare CR end; found by fuzz_doctor) */
            sb_str(&b, kv); sb_str(&b, nl);
        }
    }
    if (!matches && into_named && !have_section) {
        if (b.n && b.p[b.n - 1] != '\n') sb_str(&b, nl);
        if (b.n > (size_t)(body - text)) sb_str(&b, nl);
        sb_str(&b, "["); sb_str(&b, section); sb_str(&b, "]"); sb_str(&b, nl);
        sb_str(&b, kv); sb_str(&b, nl);
    }
    free(v);
    if (b.oom || !b.p) { free(b.p); return -1; }
    *out = b.p;
    return 1;
}

static char *read_all(const char *path, size_t max, int *exists)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (exists) *exists = h != INVALID_HANDLE_VALUE;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return NULL; }
    size_t n = (size_t)sz.QuadPart, skip = 0;
    if (n > max) { skip = n - max; n = max; }   /* keep the tail */
    if (skip) { LARGE_INTEGER o; o.QuadPart = (LONGLONG)skip; SetFilePointerEx(h, o, NULL, FILE_BEGIN); }
    char *buf = (char *)malloc(n + 1);
    DWORD got = 0, total = 0;
    while (buf && total < n && ReadFile(h, buf + total, (DWORD)(n - total), &got, NULL) && got) total += got;
    CloseHandle(h);
    if (buf) buf[total] = 0;
    return buf;
}

/* L11: every writer of the v3x inis (mod_menu, doctor fix, V3X-Setup) holds V3X_CONFIG_MUTEX
 * (sdk/v3x_util.h v3x_config_lock) around its read-modify-write. On a 5 s timeout the caller goes
 * on (and logs it): losing the user's change would be worse than a rare race. */

static int dr_apply_fix_locked(const char *v3x_dir, const dr_fix *f, char *msg, size_t n);
int dr_apply_fix(const char *v3x_dir, const dr_fix *f, char *msg, size_t n)
{
    HANDLE lk = v3x_config_lock(5000);          /* NULL after a 5 s timeout: apply anyway */
    int rc = dr_apply_fix_locked(v3x_dir, f, msg, n);
    v3x_config_unlock(lk);
    return rc;
}
static int dr_apply_fix_locked(const char *v3x_dir, const dr_fix *f, char *msg, size_t n)
{
    char path[MAX_PATH], bak[MAX_PATH + 16], old[128], where[64];
    int exists = 0;
    snprintf(path, sizeof path, "%s%s", v3x_dir, f->file);
    where[0] = 0;
    if (f->section[0] && !f->any_section) snprintf(where, sizeof where, "[%s] ", f->section);
    char *text = read_all(path, 1 << 20, &exists);
    if (exists && !text) { snprintf(msg, n, "could not read %s (error %lu); nothing changed", f->file, GetLastError()); return -1; }
    char *nt = NULL;
    int rc = dr_ini_set(text ? text : "", f->section, f->key, f->value, f->any_section, &nt, old, sizeof old);
    free(text);
    if (rc == 0) { snprintf(msg, n, "%s %s%s is already %s: nothing to change", f->file, where, f->key, f->value); return 0; }
    if (rc < 0) { snprintf(msg, n, "out of memory; nothing changed"); return -1; }
    snprintf(bak, sizeof bak, "%s.doctor-bak", path);
    int made_bak = exists && CopyFileA(path, bak, TRUE);   /* keeps the FIRST original, never overwritten */
    int bak_there = GetFileAttributesA(bak) != INVALID_FILE_ATTRIBUTES;
    if (v3x_file_write_atomic(path, nt, strlen(nt))) {
        DWORD e = GetLastError();
        free(nt);
        snprintf(msg, n, "could not write %s (error %lu); nothing changed", f->file, e);
        return -1;
    }
    free(nt);
    if (old[0])
        snprintf(msg, n, "changed %s %s%s: %s -> %s. Undo: set %s = %s again%s%s. Takes effect at the next game start.",
                 f->file, where, f->key, old, f->value, f->key, old, bak_there ? " (original kept as " : "",
                 bak_there ? (made_bak ? "the new .doctor-bak file)" : "the existing .doctor-bak file)") : "");
    else
        snprintf(msg, n, "%s %s %s%s = %s. Undo: delete that line again%s. Takes effect at the next game start.",
                 exists ? "added to" : "created", f->file, where, f->key, f->value,
                 bak_there ? " (original kept as .doctor-bak)" : "");
    return 1;
}

/* ------------------------------------------------------------------ plugin status from v3x.log */

static dr_plugin *plug_find(dr_plugins *ps, const char *key)
{
    for (int i = 0; i < ps->n; i++)
        if (dr_ieq(ps->p[i].stem, key) || (ps->p[i].name[0] && dr_ieq(ps->p[i].name, key))) return &ps->p[i];
    return NULL;
}
static dr_plugin *plug_get(dr_plugins *ps, const char *stem)
{
    dr_plugin *p = plug_find(ps, stem);
    if (p || ps->n >= DR_MAX_PLUGINS) return p;
    p = &ps->p[ps->n++];
    memset(p, 0, sizeof *p);
    snprintf(p->stem, sizeof p->stem, "%s", stem);
    return p;
}
/* "x.dll" -> "x" */
static void stem_of(const char *file, size_t len, char *out, size_t n)
{
    if (len >= 4 && !_strnicmp(file + len - 4, ".dll", 4)) len -= 4;
    cpy(out, n, file, len);
}

/* "[hh:mm:ss.mmm][tag] msg" -> tag, msg. Returns 0 if the line has no tag. */
static int split_log(char *line, char **tag, char **msg)
{
    if (line[0] != '[') return 0;
    char *a = strstr(line, "][");
    if (!a) return 0;
    char *t = a + 2, *e = strchr(t, ']');
    if (!e) return 0;
    *e = 0;
    *tag = t;
    *msg = e + 1;
    if (**msg == ' ') (*msg)++;
    return 1;
}

static void parse_core_line(dr_plugins *ps, const char *m, const char *pos)
{
    char a[128], b[128];
    const char *q, *r;
    if (dr_istarts(m, "loaded plugin '")) {
        q = m + 15; r = strchr(q, '\'');
        if (!r) return;
        cpy(a, sizeof a, q, (size_t)(r - q));
        const char *ver = NULL; size_t vl = 0;
        if (r[1] == ' ' && r[2] == 'v') { ver = r + 3; vl = strcspn(ver, " "); }
        const char *f = strrchr(r, '(');
        if (!f) return;
        f++;
        const char *fe = strchr(f, ')');
        if (!fe) return;
        stem_of(f, (size_t)(fe - f), b, sizeof b);
        dr_plugin *p = plug_get(ps, b);
        if (!p) return;
        p->state = DR_PS_LOADED;
        if (_stricmp(a, b)) snprintf(p->name, sizeof p->name, "%s", a);
        if (ver) cpy(p->version, sizeof p->version, ver, vl);
        return;
    }
    if (dr_istarts(m, "WARNING: plugin '") && strstr(m, "lists game builds")) {
        q = m + 17; r = strchr(q, '\'');
        if (!r) return;
        cpy(a, sizeof a, q, (size_t)(r - q));
        dr_plugin *p = plug_get(ps, a);
        if (p) p->manifest_warn = 1;
        return;
    }
    if (dr_istarts(m, "plugin '")) {
        q = m + 8; r = strchr(q, '\'');
        if (!r) return;
        cpy(a, sizeof a, q, (size_t)(r - q));
        if (dr_istarts(r, "' returned ")) {
            dr_plugin *p = plug_find(ps, a);
            if (!p) p = plug_get(ps, a);
            if (!p) return;
            p->state = DR_PS_FAILED_RC;
            p->rc_pos = pos;
            snprintf(p->why, sizeof p->why, "v3x_plugin_load returned %d", atoi(r + 11));
            return;
        }
        if (dr_istarts(r, "' (")) {   /* plugin 'name' (file.dll) why */
            const char *f = r + 3, *fe = strchr(f, ')');
            if (!fe) return;
            stem_of(f, (size_t)(fe - f), b, sizeof b);
            const char *why = fe + 1;
            while (*why == ' ') why++;
            dr_plugin *p = plug_get(ps, b);
            if (!p) return;
            if (_stricmp(a, b)) snprintf(p->name, sizeof p->name, "%s", a);
            snprintf(p->why, sizeof p->why, "%s", why);
            p->state = dr_istarts(why, "skipped: unknown game build") ? DR_PS_GATED
                       : dr_istarts(why, "refused: needs loader API") ? DR_PS_REFUSED_API
                       : dr_istarts(why, "refused:") ? DR_PS_REFUSED_BUILD : DR_PS_LOAD_ERROR;
        }
        return;
    }
    if (dr_istarts(m, "plugin ") && (q = strstr(m, " disabled (v3x.ini)"))) {
        cpy(a, sizeof a, m + 7, (size_t)(q - (m + 7)));
        dr_plugin *p = plug_get(ps, a);
        if (p) { p->state = DR_PS_DISABLED_INI; snprintf(p->why, sizeof p->why, "disabled in v3x.ini [plugins]"); }
        return;
    }
    if (dr_istarts(m, "FAILED to load ")) {
        q = m + 15; r = strstr(q, " (error");
        if (!r) r = q + strlen(q);
        stem_of(q, (size_t)(r - q), a, sizeof a);
        dr_plugin *p = plug_get(ps, a);
        if (p) { p->state = DR_PS_LOAD_ERROR; snprintf(p->why, sizeof p->why, "Windows could not load the DLL%s", r); }
        return;
    }
    if ((q = strstr(m, ": no v3x_plugin_load export"))) {
        stem_of(m, (size_t)(q - m), a, sizeof a);
        dr_plugin *p = plug_get(ps, a);
        if (p) { p->state = DR_PS_LOAD_ERROR; snprintf(p->why, sizeof p->why, "not a V3X plugin (no v3x_plugin_load export)"); }
        return;
    }
    if (dr_istarts(m, "FAULT in plugin ")) {
        q = m + 16; r = strstr(q, " during ");
        if (!r) return;
        stem_of(q, (size_t)(r - q), a, sizeof a);
        dr_plugin *p = plug_get(ps, a);
        if (!p) return;
        if (dr_istarts(r + 8, "v3x_plugin_load")) {
            p->state = DR_PS_FAULT_LOAD;
            snprintf(p->why, sizeof p->why, "%s", r + 8);
        } else {
            p->faulted = 1;
            snprintf(p->fault, sizeof p->fault, "%s", r + 8);
        }
        return;
    }
    if (dr_istarts(m, "ERROR: the game crashed while loading plugin ")) {
        q = m + strlen("ERROR: the game crashed while loading plugin "); r = strstr(q, " last start");
        if (!r) r = q + strcspn(q, " :");
        cpy(a, sizeof a, q, (size_t)(r - q));
        dr_plugin *p = plug_get(ps, a);
        if (p) { p->state = DR_PS_CRASH_SKIP; snprintf(p->why, sizeof p->why, "crashed the game while loading last start"); }
        return;
    }
    if (strstr(m, " plugins loaded") && isdigit((unsigned char)m[0]) && strchr(m, '/')) { ps->log_summary = 1; return; }
    if (dr_istarts(m, "WARNING: game build ") && strstr(m, "not one this loader was tested on")) {
        if (strstr(m, "unknown_build = load:")) ps->gate_forced = 1;
        else if (strstr(m, "Safe mode")) ps->gate_active = 1;
    }
}

static void parse_tag_line(dr_plugins *ps, const char *tag, const char *m, const char *pos)
{
    dr_plugin *p = plug_find(ps, tag);
    const char *q;
    if (ps->nignored < DR_MAX_INIS && (q = strstr(m, "lines ignored")) && strstr(m, ".ini")) {
        snprintf(ps->ignored[ps->nignored++], sizeof ps->ignored[0], "%s", m);
    }
    if (!p) return;
    if (p->state == DR_PS_FAILED_RC && p->rc_pos && pos < p->rc_pos) snprintf(p->why, sizeof p->why, "%s", m);
    if (dr_istarts(m, "disabled by V3X_") || dr_istarts(m, "inactive") || dr_istarts(m, "mode=off") ||
        dr_istarts(m, "disabled (set enabled=1"))
        p->inactive = 1;
    const char *an = dr_istr(m, "anchor");
    if (an && (dr_istr(an, " failed") || dr_istr(an, " not found")) && !p->anchor_fail) {
        p->anchor_fail = 1;
        snprintf(p->anchor, sizeof p->anchor, "%s", m);
    } else if (!p->anchor_fail && dr_istr(m, "shape mismatch")) {
        p->anchor_fail = 1;
        snprintf(p->anchor, sizeof p->anchor, "%s", m);
    }
}

void dr_parse_log(const char *log, dr_plugins *ps)
{
    char buf[2048];
    const char *p = log, *s;
    /* pass 1: the core's own lines (load records); pass 2: plugin lines (need the records) */
    for (int pass = 0; pass < 2; pass++) {
        p = log;
        while ((s = next_line(&p, buf, sizeof buf))) {
            char *tag, *msg;
            if (!split_log(buf, &tag, &msg)) continue;
            if (!strcmp(tag, "v3x")) { if (!pass) parse_core_line(ps, msg, s); }
            else if (pass) parse_tag_line(ps, tag, msg, s);
        }
    }
}

/* `plugins` output: "  name  version  file.dll  status" (+ description lines indented 22) */
void dr_parse_plugins_cmd(const char *out, dr_plugins *ps)
{
    char buf[1024];
    const char *p = out;
    while (next_line(&p, buf, sizeof buf)) {
        if (buf[0] != ' ' || buf[1] != ' ' || buf[2] == ' ' || !buf[2]) continue;
        char name[96], ver[48], file[160];
        int used = 0;
        if (sscanf(buf, " %95s %47s %159s %n", name, ver, file, &used) < 3 || !used) continue;
        size_t fl = strlen(file);
        if (fl < 5 || _stricmp(file + fl - 4, ".dll")) continue;
        char stem[64];
        stem_of(file, fl, stem, sizeof stem);
        const char *st = buf + used;
        dr_plugin *pl = plug_get(ps, stem);
        if (!pl) continue;
        if (strcmp(name, "?") && _stricmp(name, stem)) snprintf(pl->name, sizeof pl->name, "%s", name);
        if (strcmp(ver, "-")) snprintf(pl->version, sizeof pl->version, "%s", ver);
        if (dr_istarts(st, "loaded")) pl->state = DR_PS_LOADED;
        else if (dr_istarts(st, "disabled (v3x.ini")) pl->state = DR_PS_DISABLED_INI;
        else if (dr_istarts(st, "disabled: crashed")) pl->state = DR_PS_CRASH_SKIP;
        else if (dr_istarts(st, "skipped: unknown game build")) pl->state = DR_PS_GATED;
        else if (dr_istarts(st, "refused: needs loader API")) pl->state = DR_PS_REFUSED_API;
        else if (dr_istarts(st, "refused:")) pl->state = DR_PS_REFUSED_BUILD;
        else if (dr_istarts(st, "FAULTED in v3x_plugin_load")) pl->state = DR_PS_FAULT_LOAD;
        else if (dr_istarts(st, "FAULTED")) { pl->state = DR_PS_LOADED; pl->faulted = 1; snprintf(pl->fault, sizeof pl->fault, "%s", st); }
        else if (dr_istarts(st, "FAILED: v3x_plugin_load returned")) pl->state = DR_PS_FAILED_RC;
        else pl->state = DR_PS_LOAD_ERROR;
        if (pl->state != DR_PS_LOADED && !pl->why[0]) snprintf(pl->why, sizeof pl->why, "%s", st);
        if (pl->state == DR_PS_FAILED_RC && !pl->why[0]) snprintf(pl->why, sizeof pl->why, "%s", st);
    }
    ps->log_summary = 1;
}

/* the plugin is doing work this session */
static int plug_active(const dr_plugins *ps, const char *stem)
{
    for (int i = 0; i < ps->n; i++)
        if (dr_ieq(ps->p[i].stem, stem))
            return (ps->p[i].state == DR_PS_LOADED || (ps->p[i].state == DR_PS_UNKNOWN && ps->p[i].file_present)) &&
                   !ps->p[i].inactive;
    return 0;
}

/* ------------------------------------------------------------------ crash log */

void dr_crash_parse(const char *text, const char *const *stems, int nstems, dr_crash *c)
{
    char buf[1024];
    memset(c, 0, sizeof *c);
    c->commit_avail_mb = -1;
    if (!text) return;
    const char *p = text, *s, *last = NULL;
    /* find the last fatal line */
    while ((s = next_line(&p, buf, sizeof buf))) {
        const char *m = strchr(buf, ']');
        m = m ? m + 1 : buf;
        while (*m == ' ') m++;
        if (dr_istarts(m, "UNHANDLED EXCEPTION") || dr_istarts(m, "abort called") ||
            (dr_istarts(m, "EXCEPTION ") && !dr_istarts(m, "EXCEPTION UNHANDLED")) || dr_istarts(m, "STACK_OVERFLOW"))
            last = s;
    }
    if (!last) return;
    c->found = 1;
    p = last;
    next_line(&p, buf, sizeof buf);
    const char *m = strchr(buf, ']');
    m = m ? m + 1 : buf;
    while (*m == ' ') m++;
    snprintf(c->line, sizeof c->line, "%s", m);
    c->unhandled = dr_istarts(m, "UNHANDLED") || dr_istarts(m, "abort") || dr_istarts(m, "STACK_OVERFLOW");
    const char *at = strstr(m, " at ");
    if (at) {
        at += 4;
        size_t k = strcspn(at, "+ ");
        cpy(c->module, sizeof c->module, at, k);
    } else if ((at = strstr(m, "module="))) {
        at += 7;
        cpy(c->module, sizeof c->module, at, strcspn(at, " "));
    }
    /* the exception block: registers, memory line, stack frames; anything else ends it */
    int tail = 0;
    while ((s = next_line(&p, buf, sizeof buf))) {
        const char *x = strchr(buf, ']');
        x = x ? x + 1 : buf;
        int indented = x[0] == ' ' && x[1] == ' ';
        while (*x == ' ') x++;
        if (indented && x[0] == '#') {
            const char *mod = strchr(x, ' ');
            if (mod && !c->plugin_stack[0]) {
                mod++;
                char name[64];
                cpy(name, sizeof name, mod, strcspn(mod, "+ "));
                for (int i = 0; i < nstems; i++) {
                    char st[64];
                    stem_of(name, strlen(name), st, sizeof st);
                    if (dr_ieq(st, stems[i])) { snprintf(c->plugin_stack, sizeof c->plugin_stack, "%s", stems[i]); break; }
                }
            }
            continue;
        }
        if (indented && dr_istarts(x, "memory:")) {
            snprintf(c->memory, sizeof c->memory, "%s", x);
            const char *ca = strstr(x, "commit avail=");
            if (ca) c->commit_avail_mb = atoll(ca + 13);
            continue;
        }
        if (indented) continue;   /* registers, "(truncated ...)" */
        if (!*x) continue;
        tail++;
    }
    c->at_end = tail <= 2;   /* at most a summary/flush line after the block */
    char st[64];
    stem_of(c->module, strlen(c->module), st, sizeof st);
    for (int i = 0; i < nstems; i++)
        if (dr_ieq(st, stems[i])) snprintf(c->plugin_fault, sizeof c->plugin_fault, "%s", stems[i]);
}

/* ------------------------------------------------------------------ the checks */

static const char *v3x_ini(const dr_inputs *in) { return ini_text(in, "v3x.ini"); }

static void check_build(const dr_inputs *in, dr_results *o, dr_plugins *ps)
{
    const char *id = in->build_id ? in->build_id : "?", *ver = in->game_version ? in->game_version : "?";
    char ub[32] = "";
    dr_ini_get(v3x_ini(in), "", "unknown_build", 0, ub, sizeof ub);
    if (in->env_unknown_build && *in->env_unknown_build) snprintf(ub, sizeof ub, "%s", in->env_unknown_build);
    int known = in->known_builds ? dr_list_has(in->known_builds, id) : -1;
    if (known == 1) {
        add(o, "D001", "game_build", DR_OK, "game %s (build %s) is a build this V3X (loader %s) was tested on", ver, id,
            in->loader_version ? in->loader_version : "?");
        return;
    }
    if (known < 0) {
        add(o, "D001", "game_build", DR_OK, "game %s (build %s); the loader does not export its tested builds (loader older than 0.5.0)", ver, id);
        return;
    }
    if (dr_ieq(ub, "load") || ps->gate_forced) {
        dr_result *r = add(o, "D001", "game_build", DR_WARN,
                           "game build %s (%s) is not one V3X was tested on (%s), and unknown_build = load makes every plugin load anyway",
                           id, ver, in->known_builds);
        todo(r, "If the game crashes or acts strange: doctor fix D001 (back to safe mode), and update V3X (V3X-Setup.exe --update)");
        if (!in->env_unknown_build || !*in->env_unknown_build) set_fix(r, "v3x.ini", "", "unknown_build", "safe", 0);
        return;
    }
    int skipped = 0;
    char list[200] = "";
    for (int i = 0; i < ps->n; i++)
        if (ps->p[i].state == DR_PS_GATED) {
            skipped++;
            size_t l = strlen(list);
            snprintf(list + l, sizeof list - l, "%s%s", l ? ", " : "", ps->p[i].stem);
        }
    dr_result *r = add(o, "D001", "game_build", DR_WARN,
                       "the game was updated (build %s, %s): V3X runs in safe mode, %d plugin(s) that do not declare this build were skipped%s%s",
                       id, ver, skipped, skipped ? ": " : "", list);
    todo(r, "Update V3X (V3X-Setup.exe --update, or the latest release). Only if you accept crashes: unknown_build = load in v3x.ini");
}

static void check_loader(const dr_inputs *in, dr_results *o)
{
    if (in->api_version < 5) {
        dr_result *r = add(o, "D002", "loader_files", DR_WARN, "V3X loader API v%d is older than this doctor expects (v5)", in->api_version);
        todo(r, "Reinstall or update V3X (V3X-Setup.exe) so the loader and the plugins match");
        return;
    }
    add(o, "D002", "loader_files", DR_OK, "loader %s, API v%d", in->loader_version ? in->loader_version : "?", in->api_version);
}

static const char *base_name(const char *path)
{
    const char *b = path;
    for (const char *s = path; *s; s++)
        if (*s == '\\' || *s == '/') b = s + 1;
    return b;
}
static int same_dir(const char *path, const char *dir)
{
    if (!path || !dir) return 0;
    size_t dl = strlen(dir);
    while (dl && (dir[dl - 1] == '\\' || dir[dl - 1] == '/')) dl--;
    const char *b = base_name(path);
    size_t pl = (size_t)(b - path);
    while (pl && (path[pl - 1] == '\\' || path[pl - 1] == '/')) pl--;
    return pl == dl && !_strnicmp(path, dir, dl);
}

static void check_modules(const dr_inputs *in, dr_results *o)
{
    static const char *proxies[] = {"dinput8.dll", "dxgi.dll", "d3d11.dll", "d3d9.dll", "d3d12.dll", "winmm.dll",
                                    "winhttp.dll", "wininet.dll", "xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll",
                                    "dsound.dll", "opengl32.dll", "version.dll", NULL};
    static const struct { const char *dll, *what; } overlays[] = {
        {"RTSSHooks64.dll", "RivaTuner/MSI Afterburner overlay"}, {"GameOverlayRenderer64.dll", "Steam overlay"},
        {"DiscordHook64.dll", "Discord overlay"}, {"nvspcap64.dll", "NVIDIA overlay/ShadowPlay"},
        {"owclient.dll", "Overwolf"}, {"ReShade64.dll", "ReShade"}, {"SpecialK64.dll", "Special K"},
        {"obs-graphics-hook64.dll", "OBS game capture"}, {"graphics-hook64.dll", "OBS game capture"},
        {"bdcamvk64.dll", "Bandicam"}, {"fraps64.dll", "Fraps"}, {"MedalHook64.dll", "Medal"}, {NULL, NULL}};
    char prox[300] = "", ov[300] = "";
    int np = 0, no = 0;
    for (int i = 0; i < in->nmodules; i++) {
        const char *m = in->modules[i], *b = base_name(m);
        if (in->loader_path && dr_ieq(m, in->loader_path)) continue;
        for (int k = 0; proxies[k]; k++)
            if (dr_ieq(b, proxies[k]) && same_dir(m, in->exe_dir)) {
                size_t l = strlen(prox);
                snprintf(prox + l, sizeof prox - l, "%s%s", np++ ? ", " : "", b);
            }
        for (int k = 0; overlays[k].dll; k++)
            if (dr_ieq(b, overlays[k].dll)) {
                size_t l = strlen(ov);
                snprintf(ov + l, sizeof ov - l, "%s%s (%s)", no++ ? ", " : "", overlays[k].what, b);
            }
    }
    if (np) {
        dr_result *r = add(o, "D003", "foreign_proxy", DR_WARN,
                           "another DLL loader / mod tool sits next to victoria3.exe and is loaded: %s (it may hook the same game code as V3X)", prox);
        todo(r, "If you get crashes, move that DLL out of the binaries folder for one test session (V3X does not need it)");
    }
    if (no)
        add(o, np ? "D003.overlays" : "D003", "foreign_proxy", DR_OK,
            "overlays in the game (usually fine; if crashes point at d3d11/dxgi, test once without them): %s", ov);
    if (!np && !no) add(o, "D003", "foreign_proxy", DR_OK, "no other DLL loader or overlay found in the game process");
}

static void check_manifests(dr_results *o, dr_plugins *ps)
{
    int any = 0;
    char id[64];
    for (int i = 0; i < ps->n; i++) {
        dr_plugin *p = &ps->p[i];
        snprintf(id, sizeof id, "D004.%s", p->stem);
        dr_result *r = NULL;
        if (p->state == DR_PS_GATED) {
            r = add(o, id, "plugin_manifests", DR_WARN,
                    "%s was not loaded: the game was updated and it does not declare this build (safe mode)", p->stem);
            todo(r, "Update V3X / this plugin. Only if you accept crashes: unknown_build = load in v3x.ini");
        } else if (p->state == DR_PS_REFUSED_API) {
            r = add(o, id, "plugin_manifests", DR_WARN, "%s was not loaded: it needs a newer V3X loader (%s)", p->stem, p->why);
            todo(r, "Update V3X (V3X-Setup.exe --update)");
        } else if (p->state == DR_PS_REFUSED_BUILD) {
            r = add(o, id, "plugin_manifests", DR_WARN, "%s was not loaded: it only runs on the game builds it lists (%s)", p->stem, p->why);
            todo(r, "Update the plugin for this game version, or turn it off: doctor fix %s", id);
            fix_disable_plugin(r, p->stem);
        } else if (p->manifest_warn && p->state == DR_PS_LOADED) {
            r = add(o, id, "plugin_manifests", DR_WARN, "%s loaded on a game build its manifest does not list", p->stem);
            todo(r, "Works as long as its anchors match; if it misbehaves turn it off: doctor fix %s", id);
            fix_disable_plugin(r, p->stem);
        }
        if (r) any++;
    }
    if (!any) add(o, "D004", "plugin_manifests", DR_OK, "every plugin's manifest accepts this game build");
}

static void check_status(const dr_inputs *in, dr_results *o, dr_plugins *ps)
{
    int loaded = 0, disabled = 0;
    char dis[300] = "", id[64];
    for (int i = 0; i < ps->n; i++) {
        dr_plugin *p = &ps->p[i];
        snprintf(id, sizeof id, "D005.%s", p->stem);
        dr_result *r;
        switch (p->state) {
        case DR_PS_LOADED:
            loaded++;
            if (p->anchor_fail) {
                r = add(o, id, "plugin_status", DR_WARN,
                        "%s runs, but part of it did not find its game code and stays off: %s", p->stem, p->anchor);
                todo(r, "Nothing to do now (it failed safe); update V3X when a new release is out");
            }
            break;
        case DR_PS_DISABLED_INI: {
            size_t l = strlen(dis);
            snprintf(dis + l, sizeof dis - l, "%s%s", disabled++ ? ", " : "", p->stem);
            break;
        }
        case DR_PS_FAILED_RC:
            r = add(o, id, "plugin_status", DR_WARN, "%s did not start and stays off (the game is not affected): %s", p->stem,
                    p->why[0] ? p->why : "v3x_plugin_load failed");
            todo(r, "Usually its game code was not found on this build: update V3X. To stop the message: doctor fix %s", id);
            fix_disable_plugin(r, p->stem);
            break;
        case DR_PS_LOAD_ERROR:
            r = add(o, id, "plugin_status", DR_PROBLEM, "%s.dll in v3x/plugins could not be loaded: %s", p->stem, p->why);
            todo(r, "Repair V3X with V3X-Setup.exe; if the file is not from V3X, remove it from binaries/v3x/plugins");
            break;
        case DR_PS_FAULT_LOAD:
            r = add(o, id, "plugin_status", DR_PROBLEM, "%s crashed while starting and was rolled back (%s)", p->stem, p->why);
            todo(r, "Turn it off (doctor fix %s) and attach doctor_report.txt to a bug report", id);
            fix_disable_plugin(r, p->stem);
            break;
        case DR_PS_UNKNOWN:
            if (p->file_present && ps->log_summary) {
                r = add(o, id, "plugin_status", DR_WARN, "%s.dll is in v3x/plugins but has no load record this session", p->stem);
                todo(r, "It was probably added while the game was running: restart the game");
            }
            break;
        default: break;   /* gated / refused: D004; crash skip: D012 */
        }
    }
    (void)in;
    add(o, "D005", "plugin_status", DR_OK, "%d plugin(s) loaded%s%s%s", loaded, disabled ? ", turned off in v3x.ini: " : "", dis,
        ps->n ? "" : " (no load records found in v3x.log)");
}

static void check_faults(dr_results *o, dr_plugins *ps)
{
    int any = 0;
    char id[64];
    for (int i = 0; i < ps->n; i++) {
        dr_plugin *p = &ps->p[i];
        if (!p->faulted) continue;
        any++;
        snprintf(id, sizeof id, "D006.%s", p->stem);
        dr_result *r = add(o, id, "plugin_faults", DR_PROBLEM,
                           "%s crashed this session and V3X switched it off to keep the game running: %s", p->stem, p->fault);
        todo(r, "Restart the game to get it back; if it happens again turn it off (doctor fix %s) and send doctor_report.txt", id);
        fix_disable_plugin(r, p->stem);
    }
    if (!any) add(o, "D006", "plugin_faults", DR_OK, "no plugin faulted this session");
}

typedef struct perf_rule {
    const char *plugin, *file, *section;
    int any;
    const char *key, *def, *bad, *good, *why;
} perf_rule;

static const perf_rule g_perf[] = {
    {"alloc_tracker", "alloc_tracker.ini", "alloc_tracker", 0, "enabled", "0", "1", "0",
     "the allocation tracker hooks every allocation and writes a report every 30 s on the main thread (about 0.5 s freeze each time)"},
    {"devtools", "devtools.ini", "", 1, "profile", "0", "1", "0",
     "the devtools CPU profiler samples the game's threads all the time"},
    {"lazy_gfx", "lazy_gfx.ini", "", 1, "csv", "1", "1", "0", "lazy_gfx writes a CSV row for every texture it loads"},
    {"fast_lookup", "fast_lookup.ini", "fast_lookup", 1, "mode", "measure", "measure", "off",
     "fast_lookup in measure mode does every lookup twice to compare (mode = on is the fast one, see docs/plugins.md)"},
    {"par_load", "par_load.ini", "par_load", 1, "mode", "measure", "measure", "off",
     "par_load in measure mode only records load statistics, its hooks still cost time"},
    {"lazy_gfx", "lazy_gfx.ini", "", 1, "mode", "measure", "measure", "off",
     "lazy_gfx in measure mode only records texture statistics, its hooks still cost time"},
    {"map_cache", "map_cache.ini", "", 1, "mode", "measure", "measure", "off",
     "map_cache in measure mode only records statistics, its hooks still cost time"},
    {"mod_loader", "mod_loader.ini", "", 1, "mode", "measure", "measure", "off",
     "mod_loader in measure mode only records file statistics, its hooks still cost time (mode = on is the fast one)"},
    {"tick_par", "tick_par.ini", "tick_par", 0, "mode", "measure", "measure", "off",
     "tick_par only analyses the simulation tick (reports), its hooks still cost time"},
    {"ai_par", "ai_par.ini", "ai_par", 1, "mode", "measure", "measure", "off",
     "ai_par only analyses AI tick tasks (reports), its hooks still cost time"},
    {NULL, NULL, NULL, 0, NULL, NULL, NULL, NULL, NULL}};

static int rule_bad(const perf_rule *r, const char *v)
{
    if (!strcmp(r->bad, "1")) return dr_truthy(v);
    return dr_ieq(v, r->bad);
}

static void check_perf(const dr_inputs *in, dr_results *o, dr_plugins *ps)
{
    int any = 0;
    char id[64], v[64];
    for (const perf_rule *r = g_perf; r->plugin; r++) {
        if (!plug_active(ps, r->plugin)) continue;
        const char *text = ini_text(in, r->file);
        if (!dr_ini_get(text, r->section, r->key, r->any, v, sizeof v)) snprintf(v, sizeof v, "%s", r->def);
        if (!rule_bad(r, v)) continue;
        if (!strcmp(r->plugin, "ai_par")) {
            char fc[16];
            if (dr_ini_get(text, r->section, "fine_chunks", 1, fc, sizeof fc) && dr_truthy(fc)) continue;   /* the opt-in lever needs it */
        }
        snprintf(id, sizeof id, "D007.%s%s%s", r->plugin, strcmp(r->key, "enabled") && strcmp(r->key, "mode") ? "." : "",
                 strcmp(r->key, "enabled") && strcmp(r->key, "mode") ? r->key : "");
        dr_result *x = add(o, id, "perf_settings", DR_WARN, "%s.ini %s = %s: %s", r->plugin, r->key, v, r->why);
        todo(x, "Recommended %s = %s unless you are measuring: doctor fix %s", r->key, r->good, id);
        set_fix(x, r->file, r->section, r->key, r->good, r->any);
        any++;
    }
    if (in->env_profile && plug_active(ps, "devtools")) {
        dr_result *x = add(o, "D007.devtools.env", "perf_settings", DR_WARN,
                           "the environment variable V3X_PROFILE=1 turns the devtools CPU profiler on");
        todo(x, "Remove V3X_PROFILE from your environment / launch options (doctor cannot change it)");
        any++;
    }
    if (plug_active(ps, "tick_profiler") && !in->env_no_tickprof) {
        dr_result *x = add(o, "D007.tick_profiler", "perf_settings", DR_WARN,
                           "tick_profiler (a developer tool) times every simulation task and writes reports");
        todo(x, "Recommended off unless you are measuring: doctor fix D007.tick_profiler ([plugins] tick_profiler = 0)");
        fix_disable_plugin(x, "tick_profiler");
        any++;
    }
    if (plug_active(ps, "log_filter")) {
        char e[16];
        if (dr_ini_get(ini_text(in, "log_filter.ini"), "log_filter", "enabled", 1, e, sizeof e) && !dr_truthy(e)) {
            dr_result *x = add(o, "D007.log_filter", "perf_settings", DR_WARN,
                               "log_filter.ini enabled = 0: the filter is loaded but lets every noisy engine log line through to disk");
            todo(x, "Recommended enabled = 1: doctor fix D007.log_filter");
            set_fix(x, "log_filter.ini", "log_filter", "enabled", "1", 1);
            any++;
        }
    }
    if (!any)
        add(o, "D007", "perf_settings", DR_OK, "no diagnostic or measure-only setting that costs game time is on%s",
            plug_active(ps, "log_filter") ? "" : " (optional: log_filter cuts engine log writes)");
}

static void check_memory(const dr_inputs *in, dr_results *o)
{
    if (!in->have_mem || !in->commit_limit) { add(o, "D008", "memory_commit", DR_OK, "memory not measured"); return; }
    double pct = 100.0 * (double)in->commit_total / (double)in->commit_limit;
    int sev = pct > 95.0 ? DR_PROBLEM : pct > 85.0 ? DR_WARN : DR_OK;
    dr_result *r = add(o, "D008", "memory_commit", sev,
                       "game uses %.1f GB; Windows commit %.1f of %.1f GB (%.0f%%); RAM %.1f GB, %.1f GB free",
                       in->private_bytes / GB, in->commit_total / GB, in->commit_limit / GB, pct, in->phys_total / GB,
                       in->phys_avail / GB);
    if (sev)
        todo(r, "Close other programs (browsers, launchers, overlays) and/or give Windows a bigger pagefile; mem_tuner lowers the game's own commit");
}

static void check_pagefile(const dr_inputs *in, dr_results *o)
{
    int any = 0;
    if (in->have_mem && in->phys_total) {
        double pf = in->commit_limit > in->phys_total ? (double)(in->commit_limit - in->phys_total) : 0.0;
        if ((double)in->commit_limit < 1.5 * (double)in->phys_total) {
            dr_result *r = add(o, "D009", "pagefile", DR_WARN,
                               "pagefile about %.1f GB: the commit limit is only %.1f GB for %.1f GB RAM (the game alone can need 20 GB)",
                               pf / GB, in->commit_limit / GB, in->phys_total / GB);
            todo(r, "Let Windows manage the pagefile on a drive with free space, or set it to at least 16 GB");
            any++;
        }
    }
    /* pagefile growth this session: "of limit N MB" changes in this session's crash.log */
    const char *t = in->crash_log;
    char pidtag[48];
    snprintf(pidtag, sizeof pidtag, "pid %lu,", in->pid);
    if (t && (!in->pid || strstr(t, pidtag))) {
        long long lo = -1, hi = -1;
        for (const char *s = strstr(t, "of limit "); s; s = strstr(s + 9, "of limit ")) {
            long long v = atoll(s + 9);
            if (v <= 0) continue;
            if (lo < 0 || v < lo) lo = v;
            if (v > hi) hi = v;
        }
        if (lo > 0 && hi - lo >= 256) {
            dr_result *r = add(o, any ? "D009.growth" : "D009", "pagefile", DR_WARN,
                               "Windows grew the pagefile during this session (commit limit %lld -> %lld MB): memory ran short (stutters while it grows)",
                               lo, hi);
            todo(r, "Set a fixed pagefile of at least 16 GB on a drive with free space, or close other programs before playing");
            any++;
        }
    }
    if (!any) {
        if (in->have_mem && in->phys_total)
            add(o, "D009", "pagefile", DR_OK, "commit limit %.1f GB for %.1f GB RAM", in->commit_limit / GB, in->phys_total / GB);
        else
            add(o, "D009", "pagefile", DR_OK, "pagefile not measured");
    }
}

static void disk_row(dr_results *o, const char *id, const char *drive, uint64_t fr, uint64_t tot, int is_c)
{
    double f = fr / GB;
    int sev = f < 1.0 ? DR_PROBLEM : f < 5.0 ? DR_WARN : DR_OK;
    dr_result *r = add(o, id, "disk_free", sev, "%s %.1f GB free of %.1f GB%s", drive, f, tot / GB,
                       is_c ? " (Windows drive: pagefile, temp files, crash dumps)" : " (game drive: saves, caches, logs)");
    if (sev) todo(r, "Free at least 5 GB on %s (Disk Cleanup, empty Downloads / Recycle Bin)", drive);
}

static void check_disk(const dr_inputs *in, dr_results *o)
{
    int game_is_c = !_strnicmp(in->game_drive, "C:", 2);
    if (in->have_game_disk) disk_row(o, game_is_c ? "D010" : "D010.game", in->game_drive, in->game_free, in->game_total, game_is_c);
    if (in->have_c_disk && !game_is_c) disk_row(o, "D010.c", "C:", in->c_free, in->c_total, 1);
    if (!in->have_game_disk && !in->have_c_disk) add(o, "D010", "disk_free", DR_OK, "disk space not measured");
}

/* enabledMods[].path from the launcher's content_load.json (same rules as mod_menu mm_json_mod_paths) */
static int json_mod_paths(const char *json, char (*out)[MAX_PATH], int max)
{
    const char *a = json ? strstr(json, "\"enabledMods\"") : NULL;
    if (!a || !(a = strchr(a, '['))) return 0;
    int n = 0, depth = 0;
    for (const char *p = a; *p && n < max; p++) {
        if (*p == '[') depth++;
        else if (*p == ']') { if (--depth == 0) break; }
        else if (*p == '"') {
            const char *q = p + 1;
            while (*q && *q != '"') q += (*q == '\\' && q[1]) ? 2 : 1;
            if (!*q) break;
            int is_path = q - p - 1 == 4 && !strncmp(p + 1, "path", 4);
            p = q;
            if (!is_path) continue;
            const char *v = p + 1;
            while (isspace((unsigned char)*v)) v++;
            if (*v++ != ':') continue;
            while (isspace((unsigned char)*v)) v++;
            if (*v != '"') continue;
            size_t k = 0;
            int bad = 0;
            for (v++; *v && *v != '"'; v++) {
                char c = *v;
                if (c == '\\') {
                    v++;
                    if (*v == '\\' || *v == '"' || *v == '/') c = *v;
                    else { bad = 1; if (!*v) break; continue; }
                }
                if (k + 1 < MAX_PATH) out[n][k++] = c; else bad = 1;
            }
            out[n][k] = 0;
            if (!*v) break;
            p = v;
            if (!bad && k) n++;
        }
    }
    return n;
}

/* files of <packs>\<pack>\<sub...> as "sub\...\file" (pack root files skipped) */
#define DR_MAX_OWNED 1024
static void walk_owned(const char *root, const char *rel, int depth, char (*out)[MAX_PATH], int *n)
{
    char pat[MAX_PATH * 2];
    WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof pat, "%s\\%s%s*", root, rel, *rel ? "\\" : "");
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char sub[MAX_PATH];
        int k = snprintf(sub, sizeof sub, "%s%s%s", rel, *rel ? "\\" : "", fd.cFileName);
        if (k < 0 || k >= (int)sizeof sub) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 12) walk_owned(root, sub, depth + 1, out, n);
        } else if (depth >= 3 && *n < DR_MAX_OWNED) {   /* depth 2 = files in the pack root (descriptor etc.) */
            const char *s = strchr(sub, '\\');   /* drop "<pack>\" */
            if (s) snprintf(out[(*n)++], MAX_PATH, "%s", s + 1);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void mod_display_name(const char *mod, char *out, size_t n)
{
    char p[MAX_PATH * 2];
    snprintf(p, sizeof p, "%s\\.metadata\\metadata.json", mod);
    char *j = read_all(p, 1 << 16, NULL);
    const char *s = j ? strstr(j, "\"name\"") : NULL;
    if (s && (s = strchr(s + 6, ':')) && (s = strchr(s, '"'))) {
        const char *e = strchr(s + 1, '"');
        if (e) { cpy(out, n, s + 1, (size_t)(e - s - 1)); free(j); return; }
    }
    free(j);
    snprintf(out, n, "%s", base_name(mod));
}

static void check_mods(const dr_inputs *in, dr_results *o)
{
    if (!in->content_load_json) {
        add(o, "D011", "mod_overrides", DR_OK, "content_load.json not found (no launcher mod list to check)");
        return;
    }
    static char mods[256][MAX_PATH];
    static char owned[DR_MAX_OWNED][MAX_PATH];
    int nm = json_mod_paths(in->content_load_json, mods, 256), no = 0;
    if (in->content_packs_dir) walk_owned(in->content_packs_dir, "", 1, owned, &no);
    int hits = 0;
    char id[64];
    for (int i = 0; i < nm; i++) {
        for (int k = 0; k < no; k++) {
            char p[MAX_PATH * 2];
            snprintf(p, sizeof p, "%s\\%s", mods[i], owned[k]);
            for (char *c = p; *c; c++) if (*c == '/') *c = '\\';
            if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) continue;
            char name[128];
            mod_display_name(mods[i], name, sizeof name);
            snprintf(id, sizeof id, "D011.%d", hits + 1);
            int menu = dr_istr(owned[k], "frontend_main.gui") != NULL;
            dr_result *r = add(o, id, "mod_overrides", DR_WARN,
                               "enabled mod \"%s\" has its own %s; mods load above V3X's content packs, so %s", name, owned[k],
                               menu ? "the V3X button in the main menu is hidden" : "V3X's version of that file is not used");
            todo(r, menu ? "Use the V3X console instead (modmenu, modmenu toggle <plugin>), or disable that mod in the launcher"
                         : "If that V3X feature matters to you, disable that mod in the launcher");
            hits++;
            break;   /* one row per mod */
        }
    }
    if (!hits) add(o, "D011", "mod_overrides", DR_OK, "%d enabled mod(s), none overrides a V3X file (%d files checked)", nm, no);
}

static void check_crash_loop(dr_results *o, dr_plugins *ps)
{
    int any = 0;
    char id[64];
    for (int i = 0; i < ps->n; i++) {
        if (ps->p[i].state != DR_PS_CRASH_SKIP) continue;
        snprintf(id, sizeof id, "D012.%s", ps->p[i].stem);
        dr_result *r = add(o, id, "crash_loop", DR_PROBLEM,
                           "%s crashed the game while loading at the last start; V3X skipped it this start (it tries again next start)",
                           ps->p[i].stem);
        todo(r, "Turn it off until it is fixed: doctor fix %s, and send doctor_report.txt + crash.prev.log", id);
        fix_disable_plugin(r, ps->p[i].stem);
        any++;
    }
    if (!any) add(o, "D012", "crash_loop", DR_OK, "no plugin crashed the game while loading at the last start");
}

static void check_last_crash(const dr_inputs *in, dr_results *o)
{
    char pidtag[48];
    snprintf(pidtag, sizeof pidtag, "pid %lu,", in->pid);
    /* crash_logger rotates crash.log -> crash.prev.log when it loads: if crash.log is this process's,
     * the previous session is crash.prev.log; otherwise crash.log is still the previous one. */
    const char *text = in->crash_log && in->pid && strstr(in->crash_log, pidtag) ? in->crash_prev : in->crash_log;
    const char *which = text == in->crash_prev ? "crash.prev.log" : "crash.log";
    if (!text) { add(o, "D013", "last_crash", DR_OK, "no crash log from the last session (crash_logger off or first start)"); return; }
    dr_crash c;
    dr_crash_parse(text, in->plugin_files, in->nplugin_files, &c);
    if (!c.found) { add(o, "D013", "last_crash", DR_OK, "the last session (%s) recorded no crash", which); return; }
    if (!c.unhandled && !c.at_end) {
        add(o, "D013", "last_crash", DR_OK, "the last session logged an exception that the game handled and kept running: %s", c.line);
        return;
    }
    char oom[96] = "";
    if (c.commit_avail_mb >= 0 && c.commit_avail_mb < 2048) snprintf(oom, sizeof oom, " Memory was almost exhausted (%lld MB commit left).", c.commit_avail_mb);
    if (c.plugin_fault[0]) {
        dr_result *r = add(o, "D013", "last_crash", DR_PROBLEM, "the last session crashed inside V3X plugin %s: %s.%s", c.plugin_fault, c.line, oom);
        todo(r, "Turn it off (doctor fix D013) and send doctor_report.txt + %s in a bug report", which);
        fix_disable_plugin(r, c.plugin_fault);
    } else if (c.plugin_stack[0]) {
        dr_result *r = add(o, "D013", "last_crash", DR_WARN,
                           "the last session crashed in %s, with V3X plugin %s on the call stack (it may be involved): %s.%s", c.module,
                           c.plugin_stack, c.line, oom);
        todo(r, "If it happens again: turn %s off (doctor fix D013) and send doctor_report.txt + %s", c.plugin_stack, which);
        fix_disable_plugin(r, c.plugin_stack);
    } else {
        dr_result *r = add(o, "D013", "last_crash", DR_WARN, "the last session crashed in %s (no V3X plugin on the stack): %s.%s",
                           c.module[0] ? c.module : "?", c.line, oom);
        todo(r, oom[0] ? "Free memory before playing (see D008/D009); send %s if it repeats" : "Send %s with a bug report if it repeats", which);
    }
}

static void check_ini(const dr_inputs *in, dr_results *o, dr_plugins *ps)
{
    int any = 0;
    char id[64];
    const char *t = v3x_ini(in);
    if (t) {
        ini_line *v = NULL;
        int c = ini_lines(t, &v);
        for (int i = 0; i < c; i++) {
            if (v[i].kind != 2 || _stricmp(v[i].section, "plugins")) continue;
            char k[64];
            cpy(k, sizeof k, v[i].ks, (size_t)(v[i].ke - v[i].ks));
            int found = 0;
            for (int f = 0; f < in->nplugin_files; f++) found |= dr_ieq(in->plugin_files[f], k);
            if (found || !in->nplugin_files) continue;
            snprintf(id, sizeof id, "D014.%s", k);
            dr_result *r = add(o, id, "ini_syntax", DR_WARN, "v3x.ini [plugins] lists %s, but v3x/plugins/%s.dll does not exist", k, k);
            todo(r, "Harmless; remove the line, or repair V3X with V3X-Setup.exe if the plugin should be there");
            any++;
        }
        free(v);
    }
    for (int i = 0; i < ps->nignored; i++) {
        snprintf(id, sizeof id, "D014.%d", i + 1);
        dr_result *r = add(o, id, "ini_syntax", DR_WARN, "a plugin ignored lines of its ini: %s", ps->ignored[i]);
        todo(r, "Check that file against its .ini.example (a typo makes the plugin use the default)");
        any++;
    }
    if (!any) add(o, "D014", "ini_syntax", DR_OK, "v3x.ini and the plugin inis were read without complaints");
}

void dr_run(const dr_inputs *in, dr_results *out, dr_plugins *ps)
{
    memset(out, 0, sizeof *out);
    memset(ps, 0, sizeof *ps);
    if (in->v3x_log) dr_parse_log(in->v3x_log, ps);
    if (!ps->log_summary && in->plugins_out) dr_parse_plugins_cmd(in->plugins_out, ps);
    for (int i = 0; i < in->nplugin_files; i++) {
        dr_plugin *p = plug_get(ps, in->plugin_files[i]);
        if (p) p->file_present = 1;
    }
    check_build(in, out, ps);
    check_loader(in, out);
    check_modules(in, out);
    check_manifests(out, ps);
    check_status(in, out, ps);
    check_faults(out, ps);
    check_perf(in, out, ps);
    check_memory(in, out);
    check_pagefile(in, out);
    check_disk(in, out);
    check_mods(in, out);
    check_crash_loop(out, ps);
    check_last_crash(in, out);
    check_ini(in, out, ps);
}

void dr_count(const dr_results *r, int *problems, int *warnings)
{
    int p = 0, w = 0;
    for (int i = 0; i < r->n; i++) {
        if (r->r[i].sev == DR_PROBLEM) p++;
        else if (r->r[i].sev == DR_WARN) w++;
    }
    if (problems) *problems = p;
    if (warnings) *warnings = w;
}

void dr_summary_line(const dr_results *r, char *out, size_t n)
{
    int p, w;
    dr_count(r, &p, &w);
    snprintf(out, n, "doctor: %d problem%s, %d warning%s (type doctor)", p, p == 1 ? "" : "s", w, w == 1 ? "" : "s");
}

static const char *sev_word(int s) { return s == DR_PROBLEM ? "PROBLEM" : s == DR_WARN ? "WARN" : "OK"; }

size_t dr_format_result(const dr_result *r, char *out, size_t n)
{
    int k = snprintf(out, n, "[%-7s] %s %s: %s%s%s%s", sev_word(r->sev), r->id, r->name, r->msg, r->todo[0] ? "\n          what to do: " : "",
                     r->todo, r->has_fix && r->todo[0] && !strstr(r->todo, "doctor fix") ? " (doctor fix available)" : "");
    return k < 0 ? 0 : (size_t)k >= n ? n - 1 : (size_t)k;
}

int dr_id_matches(const char *rid, const char *id)
{
    size_t k = strlen(id);
    return !_stricmp(rid, id) || (!_strnicmp(rid, id, k) && rid[k] == '.');
}

/* ------------------------------------------------------------------ masking */

static void replace_ci(sbuf *b, const char *text, const char *what, const char *with, int word)
{
    size_t k = what ? strlen(what) : 0;
    const char *p = text;
    while (*p) {
        if (k && !_strnicmp(p, what, k) &&
            (!word || ((p == text || !isalnum((unsigned char)p[-1])) && !isalnum((unsigned char)p[k])))) {
            sb_str(b, with);
            p += k;
        } else {
            sb_put(b, p, 1);
            p++;
        }
    }
}

char *dr_mask(const char *text, const char *user, const char *profile)
{
    sbuf a = {0}, b = {0}, c = {0};
    char alt[MAX_PATH];
    if (!text) return NULL;
    /* 1. the profile path in both slash styles */
    if (profile && strlen(profile) >= 4) {
        replace_ci(&a, text, profile, "%USERPROFILE%", 0);
        snprintf(alt, sizeof alt, "%s", profile);
        for (char *s = alt; *s; s++) if (*s == '\\') *s = '/';
        replace_ci(&b, a.p ? a.p : "", alt, "%USERPROFILE%", 0);
        free(a.p);
    } else {
        sb_str(&b, text);
    }
    /* 2. any "\Users\<name>\" path component */
    const char *p = b.p ? b.p : "";
    while (*p) {
        if ((*p == '\\' || *p == '/') && !_strnicmp(p + 1, "users", 5) && (p[6] == '\\' || p[6] == '/') && p[7] &&
            p[7] != '\\' && p[7] != '/') {
            const char *e = p + 7;
            while (*e && *e != '\\' && *e != '/' && *e != '"' && *e != '\n' && *e != ' ') e++;
            if (_strnicmp(p + 7, "Public", 6) || e - (p + 7) != 6) {
                sb_put(&c, p, 7);
                sb_str(&c, "<user>");
                p = e;
                continue;
            }
        }
        sb_put(&c, p, 1);
        p++;
    }
    free(b.p);
    /* 3. the user name as a whole word (only names long enough not to hit normal words) */
    if (user && strlen(user) >= 3) {
        sbuf d = {0};
        replace_ci(&d, c.p ? c.p : "", user, "<user>", 1);
        free(c.p);
        return d.p ? d.p : _strdup("");
    }
    return c.p ? c.p : _strdup("");
}
