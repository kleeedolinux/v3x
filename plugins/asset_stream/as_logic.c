/* asset_stream config + descriptor logic. See as_logic.h. */
#include "as_logic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Built-in configuration, used when v3x/asset_stream.ini does not exist.
 * Must stay byte-identical to plugins/asset_stream/asset_stream.ini.example (the test checks it). */
const char AS_DEFAULT_INI[] =
"# asset_stream configuration. Copy to <game>/binaries/v3x/asset_stream.ini and edit.\n"
"# Without that file the plugin uses exactly these built-in defaults (= does nothing).\n"
"#\n"
"# enabled             1/0. 0 = the plugin installs nothing (default).\n"
"# texture_streaming   1 = force the engine's own texture streamer on (Graphics setting\n"
"#                     texture_streaming, which every quality preset forces off). Textures loaded\n"
"#                     with the engine's \"streamable\" flag (mesh materials: portraits, units,\n"
"#                     buildings, map objects) then get mips added and dropped at runtime within\n"
"#                     cache_mb. Other textures (GUI) load as before. Read once at startup, when\n"
"#                     the renderer is created. Untested in game: watch for blurry/popping models.\n"
"# cache_mb            streamer budget in MB (128..4096, the engine's own setting range).\n"
"#                     0 = keep the texture_streaming_cache value from the game settings.\n"
"\n"
"[asset_stream]\n"
"enabled = 0\n"
"texture_streaming = 1\n"
"cache_mb = 512\n";

void as_config_defaults(as_config *c)
{
    c->enabled = 0;
    c->texture_streaming = 1;
    c->cache_mb = 512;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

int as_config_parse(as_config *c, const char *text, char *err, size_t errn)
{
    static const struct { const char *k; size_t off; } keys[] = {
        {"enabled", offsetof(as_config, enabled)},
        {"texture_streaming", offsetof(as_config, texture_streaming)},
        {"cache_mb", offsetof(as_config, cache_mb)},
    };
    int bad = 0, line_no = 0;
    if (err && errn) err[0] = 0;
    const char *p = text;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        char buf[256];
        if (n >= sizeof buf) n = sizeof buf - 1;
        memcpy(buf, p, n); buf[n] = 0;
        p += nl ? (size_t)(nl - p) + 1 : strlen(p);
        line_no++;
        char *hash = strchr(buf, '#');
        if (hash) *hash = 0;
        char *s = trim(buf);
        if (!*s || *s == '[' || *s == ';') continue;
        char *eq = strchr(s, '=');
        const char *why = NULL;
        if (!eq) why = "missing '='";
        else {
            *eq = 0;
            char *k = trim(s), *v = trim(eq + 1), *end = NULL;
            long val = strtol(v, &end, 10);
            if (!*v || !end || *trim(end)) why = "value must be an integer";
            else {
                size_t i;
                for (i = 0; i < sizeof keys / sizeof keys[0]; i++)
                    if (!strcmp(keys[i].k, k)) { *(int *)((char *)c + keys[i].off) = (int)val; break; }
                if (i == sizeof keys / sizeof keys[0]) why = "unknown key";
            }
        }
        if (why) {
            if (!bad && err && errn) snprintf(err, errn, "line %d: %s", line_no, why);
            bad++;
        }
    }
    return bad;
}

void as_config_clamp(as_config *c)
{
    c->enabled = c->enabled ? 1 : 0;
    c->texture_streaming = c->texture_streaming ? 1 : 0;
    if (c->cache_mb <= 0) c->cache_mb = 0;
    else if (c->cache_mb < AS_CACHE_MIN_MB) c->cache_mb = AS_CACHE_MIN_MB;
    else if (c->cache_mb > AS_CACHE_MAX_MB) c->cache_mb = AS_CACHE_MAX_MB;
}

int as_desc_plausible(const uint8_t *desc)
{
    uint8_t st = desc[AS_DESC_STREAMING];
    uint64_t cache; memcpy(&cache, desc + AS_DESC_CACHE_BYTES, 8);
    if (st > 1) return 0;
    if (cache & ((1ull << 20) - 1)) return 0;
    cache >>= 20;
    return cache >= 1 && cache <= 65536;
}

as_apply_result as_apply(const as_config *c, uint8_t *desc)
{
    as_apply_result r; memset(&r, 0, sizeof r);
    r.old_streaming = r.new_streaming = desc[AS_DESC_STREAMING];
    memcpy(&r.old_cache, desc + AS_DESC_CACHE_BYTES, 8);
    r.new_cache = r.old_cache;
    if (!c->enabled) return r;
    if (!as_desc_plausible(desc)) { r.rejected = 1; return r; }
    if (c->texture_streaming) {
        desc[AS_DESC_STREAMING] = 1;
        if (c->cache_mb > 0) {
            uint64_t b = (uint64_t)c->cache_mb << 20;
            memcpy(desc + AS_DESC_CACHE_BYTES, &b, 8);
        }
    }
    r.new_streaming = desc[AS_DESC_STREAMING];
    memcpy(&r.new_cache, desc + AS_DESC_CACHE_BYTES, 8);
    r.changed = r.new_streaming != r.old_streaming || r.new_cache != r.old_cache;
    return r;
}
