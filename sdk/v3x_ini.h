/* v3x_ini.h - tiny INI reader for V3X plugin config (header-only, C).
 *
 * Convention: a plugin reads <game>/binaries/v3x/<plugin>.ini, every key is optional, and a
 * missing file means the defaults. Ship a commented <plugin>.ini.example with the defaults.
 *
 *   V3xIni ini;
 *   v3x_ini_load_plugin(&ini, api, "my_plugin");          // v3x_dir() + "my_plugin.ini"; ok if missing
 *   int enabled = v3x_ini_int(&ini, "my_plugin", "enabled", 1);
 *   double hz = v3x_ini_double(&ini, "my_plugin", "rate_hz", 2.5);
 *   char mode[32]; v3x_ini_str(&ini, "my_plugin", "mode", "measure", mode, sizeof mode);
 *   v3x_ini_free(&ini);
 *
 * Syntax: "[section]" lines, "key = value" lines, comments start with ';' or '#' (whole line,
 * or after whitespace in a value: "x = 3   ; note"). Keys and sections are case-insensitive,
 * surrounding whitespace is trimmed. Keys before any [section] are in section "" and are also
 * found when looking up any section that does not define the key (so flat files work too).
 * A later duplicate key wins. Booleans: 1/0, true/false, yes/no, on/off.
 * The whole file is held in memory (max 1 MB); lookups are linear (fine for config). */
#ifndef V3X_INI_H
#define V3X_INI_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "v3x.h"

typedef struct V3xIni {
    char *text;            /* parsed in place: NUL-terminated section/key/value strings */
    struct V3xIniKv { const char *section, *key, *value; } *kv;
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

/* Parses text (copied). Always succeeds unless out of memory (-1). */
static int v3x_ini_parse(V3xIni *ini, const char *text)
{
    memset(ini, 0, sizeof *ini);
    size_t len = strlen(text);
    ini->text = (char *)malloc(len + 1);
    if (!ini->text) return -1;
    memcpy(ini->text, text, len + 1);
    const char *section = "";
    char *line = ini->text;
    if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) line += 3;
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
                        int cap = ini->cap ? ini->cap * 2 : 32;
                        void *p = realloc(ini->kv, (size_t)cap * sizeof *ini->kv);
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

/* Reads and parses a file. A missing file is not an error (0, ini->found = 0, all defaults). */
static int v3x_ini_load(V3xIni *ini, const char *path)
{
    memset(ini, 0, sizeof *ini);
    FILE *f = fopen(path, "rb");
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

/* <v3x_dir>/<plugin>.ini */
static int v3x_ini_load_plugin(V3xIni *ini, const V3X_API *api, const char *plugin)
{
    char path[520];
    snprintf(path, sizeof path, "%s%s.ini", api->v3x_dir(), plugin);
    return v3x_ini_load(ini, path);
}

static void v3x_ini_free(V3xIni *ini)
{
    free(ini->kv); free(ini->text);
    memset(ini, 0, sizeof *ini);
}

/* Raw value or NULL. Section "" keys are the fallback for any section. */
static const char *v3x_ini_get(const V3xIni *ini, const char *section, const char *key)
{
    const char *hit = NULL, *flat = NULL;
    for (int i = 0; i < ini->n; i++) {
        if (_stricmp(ini->kv[i].key, key)) continue;
        if (!_stricmp(ini->kv[i].section, section ? section : "")) hit = ini->kv[i].value;
        else if (!*ini->kv[i].section) flat = ini->kv[i].value;
    }
    return hit ? hit : flat;
}

static long long v3x_ini_int(const V3xIni *ini, const char *section, const char *key, long long def)
{
    const char *v = v3x_ini_get(ini, section, key);
    if (!v || !*v) return def;
    char *end = NULL;
    long long x = strtoll(v, &end, 0);   /* 0x.. hex allowed */
    return end && end != v && *end == 0 ? x : def;
}

static double v3x_ini_double(const V3xIni *ini, const char *section, const char *key, double def)
{
    const char *v = v3x_ini_get(ini, section, key);
    if (!v || !*v) return def;
    char *end = NULL;
    double x = strtod(v, &end);
    return end && end != v && *end == 0 ? x : def;
}

static int v3x_ini_bool(const V3xIni *ini, const char *section, const char *key, int def)
{
    const char *v = v3x_ini_get(ini, section, key);
    if (!v) return def;
    if (!_stricmp(v, "1") || !_stricmp(v, "true") || !_stricmp(v, "yes") || !_stricmp(v, "on")) return 1;
    if (!_stricmp(v, "0") || !_stricmp(v, "false") || !_stricmp(v, "no") || !_stricmp(v, "off")) return 0;
    return def;
}

/* Copies the value (or def) into out; returns out. */
static char *v3x_ini_str(const V3xIni *ini, const char *section, const char *key, const char *def, char *out, size_t cap)
{
    const char *v = v3x_ini_get(ini, section, key);
    snprintf(out, cap, "%s", v ? v : (def ? def : ""));
    return out;
}

#endif
