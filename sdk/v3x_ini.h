/* v3x_ini.h - tiny INI reader for V3X plugin config (header-only, C).
 *
 * Convention: a plugin reads <game>/binaries/v3x/<plugin>.ini, every key is optional, and a
 * missing file means the defaults. Ship a commented <plugin>.ini.example with the defaults.
 *
 *   V3xIni ini;
 *   v3x_ini_load_plugin(&ini, api, "my_plugin");          // v3x_dir() + "my_plugin.ini"; ok if missing
 *   int enabled = v3x_ini_int(&ini, "my_plugin", "enabled", 1);
 *   int hz = (int)v3x_ini_int_range(&ini, "my_plugin", "rate_hz", 30, 1, 240);   // out of range -> 30
 *   double f = v3x_ini_double(&ini, "my_plugin", "factor", 2.5);
 *   char mode[32]; v3x_ini_str(&ini, "my_plugin", "mode", "measure", mode, sizeof mode);
 *   v3x_ini_free(&ini);
 *
 * Syntax: "[section]" lines, "key = value" lines, comments start with ';' or '#' (whole line,
 * or after whitespace in a value: "x = 3   ; note"). Keys and sections are case-insensitive,
 * surrounding whitespace is trimmed. Keys before any [section] are in section "" and are also
 * found when looking up any section that does not define the key (so flat files work too).
 * A later duplicate key wins. Booleans: 1/0, true/false, yes/no, on/off.
 * The whole file is held in memory (max 1 MB; the rest is ignored); lookups are linear.
 *
 * Safety contract (audit action 12):
 *   - every getter is NULL-safe: a NULL ini, NULL key or a key that is not set returns the default
 *     (section NULL = ""); a NULL `out` / cap 0 for v3x_ini_str writes nothing;
 *   - integers that do not parse completely, or overflow long long, are the default; use
 *     v3x_ini_int_range / v3x_ini_double_range to also refuse out-of-range values (-> default,
 *     never clamped silently);
 *   - v3x_ini_load_plugin FAILS (-1, ini empty: all defaults) when the path does not fit or the
 *     plugin name is not a single plain file name, instead of reading a truncated path;
 *   - lifetime: the strings returned by v3x_ini_get point into the V3xIni and DANGLE after
 *     v3x_ini_free (copy them, or use v3x_ini_str). Call v3x_ini_free also after a failed load. */
#ifndef V3X_INI_H
#define V3X_INI_H

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "v3x.h"
#include "v3x_safe.h"   /* v3x_path_rel_ok_ex, V3X_MUST_USE, V3X_NN */

V3X_NN_BEGIN

typedef struct V3xIniKv { const char *section, *key, *value; } V3xIniKv;   /* file scope: same in C and C++ */
typedef struct V3xIni {
    char *text;            /* parsed in place: NUL-terminated section/key/value strings */
    V3xIniKv *kv;
    int n, cap;
    int found;             /* 1 if a file was read */
} V3xIni;

static char *v3x_ini__trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    return s;
}

/* Parses text (copied; NULL = ""). 0, or -1 when out of memory (call v3x_ini_free anyway). */
static int v3x_ini_parse(V3xIni *V3X_NN ini, const char *text)
{
    if (!ini) return -1;
    memset(ini, 0, sizeof *ini);
    if (!text) text = "";
    size_t len = strlen(text);
    ini->text = (char *)v3x_alloc_array(len + 1, 1);
    if (!ini->text) return -1;
    memcpy(ini->text, text, len + 1);
    const char *section = "";
    char *line = ini->text;
    if (len >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) line += 3;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        char *s = v3x_ini__trim(line);
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (e) { *e = 0; section = v3x_ini__trim(s + 1); }
        } else if (*s && *s != ';' && *s != '#') {
            char *eq = strchr(s, '=');
            if (eq) {
                *eq = 0;
                char *k = v3x_ini__trim(s), *v = eq + 1;
                for (char *c = v; *c; c++)   /* inline comment: whitespace then ; or # */
                    if ((*c == ';' || *c == '#') && c > v && (c[-1] == ' ' || c[-1] == '\t')) { *c = 0; break; }
                v = v3x_ini__trim(v);
                if (*k) {
                    if (ini->n == ini->cap) {
                        if (ini->cap > (1 << 24)) return -1;   /* cannot happen with a 1 MB file */
                        int cap = ini->cap ? ini->cap * 2 : 32;
                        void *p = v3x_realloc_array(ini->kv, (size_t)cap, sizeof *ini->kv);
                        if (!p) return -1;
                        ini->kv = (struct V3xIniKv *)p; ini->cap = cap;
                    }
                    ini->kv[ini->n].section = section; ini->kv[ini->n].key = k; ini->kv[ini->n].value = v;
                    ini->n++;
                }
            }
        }
        line = next;
    }
    return 0;
}

/* Reads and parses a file. A missing file (or path NULL) is not an error (0, ini->found = 0, all
 * defaults). -1 = out of memory. */
static int v3x_ini_load(V3xIni *V3X_NN ini, const char *path)
{
    if (!ini) return -1;
    memset(ini, 0, sizeof *ini);
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) return v3x_ini_parse(ini, "");
    char *buf = (char *)malloc(1 << 20);
    if (!buf) { fclose(f); return -1; }
    size_t n = fread(buf, 1, (1 << 20) - 1, f);
    fclose(f);
    buf[n] = 0;
    int rc = v3x_ini_parse(ini, buf);
    free(buf);
    ini->found = 1;
    return rc;
}

/* <v3x_dir>/<plugin>.ini. -1 (ini empty, all defaults) if api/plugin is NULL, plugin is not a single
 * plain file name, or the path does not fit (never a truncated path). */
static int v3x_ini_load_plugin(V3xIni *V3X_NN ini, const V3X_API *V3X_NN api, const char *V3X_NN plugin)
{
    char path[520];
    if (!ini) return -1;
    memset(ini, 0, sizeof *ini);
    const char *dir = api && api->v3x_dir ? api->v3x_dir() : NULL;
    if (!dir || !plugin || v3x_path_rel_ok_ex(plugin, V3X_PATH_SINGLE) != V3X_R_OK) { (void)v3x_ini_parse(ini, ""); return -1; }
    int k = snprintf(path, sizeof path, "%s%s.ini", dir, plugin);
    if (k < 0 || (size_t)k >= sizeof path) { (void)v3x_ini_parse(ini, ""); return -1; }
    return v3x_ini_load(ini, path);
}

static void v3x_ini_free(V3xIni *ini)
{
    if (!ini) return;
    free(ini->kv); free(ini->text);
    memset(ini, 0, sizeof *ini);
}

/* Raw value or NULL (NULL ini / key -> NULL). Section "" keys are the fallback for any section.
 * The pointer is valid until v3x_ini_free. */
static const char *v3x_ini_get(const V3xIni *ini, const char *section, const char *key)
{
    const char *hit = NULL, *flat = NULL;
    if (!ini || !key || !ini->kv || ini->n < 0 || ini->n > ini->cap) return NULL;
    for (int i = 0; i < ini->n; i++) {
        if (_stricmp(ini->kv[i].key, key)) continue;
        if (!_stricmp(ini->kv[i].section, section ? section : "")) hit = ini->kv[i].value;
        else if (!*ini->kv[i].section) flat = ini->kv[i].value;
    }
    return hit ? hit : flat;
}

/* 1 and *out if the value parses completely as an integer (decimal, 0x hex, 0 octal) without overflow */
static int v3x_ini_parse_int_(const char *v, long long *out)
{
    if (!v || !*v) return 0;
    char *end = NULL;
    errno = 0;
    long long x = strtoll(v, &end, 0);
    if (errno == ERANGE || !end || end == v || *end) return 0;
    *out = x;
    return 1;
}

static long long v3x_ini_int(const V3xIni *ini, const char *section, const char *key, long long def)
{
    long long x;
    return v3x_ini_parse_int_(v3x_ini_get(ini, section, key), &x) ? x : def;
}

/* The value if it parses and lies in [lo, hi], else def (also for lo > hi). */
static V3X_MUST_USE long long v3x_ini_int_range(const V3xIni *ini, const char *section, const char *key, long long def,
                                                long long lo, long long hi)
{
    long long x;
    if (lo > hi || !v3x_ini_parse_int_(v3x_ini_get(ini, section, key), &x)) return def;
    return x >= lo && x <= hi ? x : def;
}

static double v3x_ini_double(const V3xIni *ini, const char *section, const char *key, double def)
{
    const char *v = v3x_ini_get(ini, section, key);
    if (!v || !*v) return def;
    char *end = NULL;
    double x = strtod(v, &end);
    return end && end != v && *end == 0 ? x : def;
}

/* The value if it parses, is finite (no inf / nan) and lies in [lo, hi], else def. */
static V3X_MUST_USE double v3x_ini_double_range(const V3xIni *ini, const char *section, const char *key, double def,
                                                double lo, double hi)
{
    double x = v3x_ini_double(ini, section, key, NAN);
    return isfinite(x) && x >= lo && x <= hi ? x : def;
}

static int v3x_ini_bool(const V3xIni *ini, const char *section, const char *key, int def)
{
    const char *v = v3x_ini_get(ini, section, key);
    if (!v) return def;
    if (!_stricmp(v, "1") || !_stricmp(v, "true") || !_stricmp(v, "yes") || !_stricmp(v, "on")) return 1;
    if (!_stricmp(v, "0") || !_stricmp(v, "false") || !_stricmp(v, "no") || !_stricmp(v, "off")) return 0;
    return def;
}

/* Copies the value (or def) into out (cut to cap-1 bytes at a UTF-8 boundary); returns out
 * ("" for out == NULL / cap 0). */
static char *v3x_ini_str(const V3xIni *ini, const char *section, const char *key, const char *def, char *V3X_NN out, size_t cap)
{
    if (!out || !cap) return (char *)"";
    const char *v = v3x_ini_get(ini, section, key);
    V3xStr s;
    v3x_str_init(&s, out, cap);
    (void)v3x_str_append(&s, v ? v : (def ? def : ""));
    return out;
}

V3X_NN_END

#endif
