/* core_ini.h - the v3x.ini line scanner of the loader (src/core.c), split out so it can be tested and
 * fuzzed on its own (test/fuzz/fuzz_core_ini.c). No windows.h, no state: plain C99.
 *
 * [plugins] section of v3x/v3x.ini: "<dll file name without .dll> = 0" skips that plugin (written by
 * mod_menu, or by hand). Same rules as plugins/mod_menu/mm_logic.c mm_ini_enabled: section and key
 * names are case-insensitive and trimmed, '#'/';' lines are comments, the last matching line wins,
 * 0/off/no/false = disabled, anything else or no line = enabled.
 * Input: a NUL-terminated text (ini_each reads up to the NUL, never past it). */
#ifndef V3X_CORE_INI_H
#define V3X_CORE_INI_H

#include <stddef.h>
#include <string.h>

static int ini_ieq(const char *a, size_t an, const char *b)
{
    size_t bn = strlen(b);
    if (an != bn) return 0;
    for (size_t i = 0; i < an; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return 1;
}

/* Calls fn(key, key_len, value, value_len, ctx) for every "key = value" line inside [section] of an
 * ini text, in file order (so "last wins" is up to fn). Rules as in ini_plugin_enabled. */
typedef void (*ini_kv_fn)(const char *k, size_t kn, const char *v, size_t vn, void *ctx);
static void ini_each(const char *ini, const char *section, ini_kv_fn fn, void *ctx)
{
    int in = !*section;   /* section "" = the keys before the first [section] */
    for (const char *p = ini; p && *p;) {
        const char *e = p;
        while (*e && *e != '\n') e++;
        const char *a = p, *b = e;
        if (b > a && b[-1] == '\r') b--;
        while (a < b && (*a == ' ' || *a == '\t')) a++;
        while (b > a && (b[-1] == ' ' || b[-1] == '\t')) b--;
        if (a < b && *a == '[') {
            const char *c = memchr(a, ']', (size_t)(b - a));
            if (c) {
                const char *s0 = a + 1, *t = c;
                while (s0 < t && (*s0 == ' ' || *s0 == '\t')) s0++;
                while (t > s0 && (t[-1] == ' ' || t[-1] == '\t')) t--;
                in = ini_ieq(s0, (size_t)(t - s0), section);
            }
        } else if (in && a < b && *a != '#' && *a != ';') {
            const char *eq = memchr(a, '=', (size_t)(b - a));
            if (eq) {
                const char *ke = eq, *vs = eq + 1;
                while (ke > a && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
                while (vs < b && (*vs == ' ' || *vs == '\t')) vs++;
                fn(a, (size_t)(ke - a), vs, (size_t)(b - vs), ctx);
            }
        }
        p = *e ? e + 1 : e;
    }
}

/* 1 unless [plugins] <stem> = 0/off/no/false (last line wins) */
struct plugin_en { const char *stem; int en; };
static void plugin_en_kv(const char *k, size_t kn, const char *v, size_t vn, void *ctx)
{
    struct plugin_en *pe = (struct plugin_en *)ctx;
    if (ini_ieq(k, kn, pe->stem))
        pe->en = !(ini_ieq(v, vn, "0") || ini_ieq(v, vn, "off") || ini_ieq(v, vn, "no") || ini_ieq(v, vn, "false"));
}

static int ini_plugin_enabled(const char *ini, const char *stem)
{
    struct plugin_en pe = {stem, 1};
    ini_each(ini, "plugins", plugin_en_kv, &pe);
    return pe.en;
}

#endif
