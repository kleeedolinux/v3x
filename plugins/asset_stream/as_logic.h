/* asset_stream config + the pure "edit the graphics init descriptor" logic: no Windows or engine
 * dependencies, so test/asset_stream_test.c can compile it directly. RE: docs/re/streaming.md. */
#ifndef AS_LOGIC_H
#define AS_LOGIC_H
#include <stddef.h>
#include <stdint.h>

/* Graphics init descriptor, built on the stack by the settings reader (1.15.1 fn 0x3783730) and
 * passed to the gfx master-context init (fn 0x3c3fdc0, rdx). Offsets verified in both functions
 * and pinned by the byte patterns in as_resolve.c. */
#define AS_DESC_TEX_QUALITY   0x10   /* i32: texture_quality value */
#define AS_DESC_STREAMING     0x14   /* u8 : texture_streaming setting (engine: Start streamer if != 0) */
#define AS_DESC_CACHE_BYTES   0x18   /* u64: texture_streaming_cache setting in MB << 20 */
/* CGfxTextureStreamer lives at master context + 0x10 */
#define AS_CTX_STREAMER       0x10
#define AS_STREAMER_BUDGET    0x1a0  /* u64: byte budget (Start writes it; update task compares) */
#define AS_STREAMER_ENABLED   0x1b1  /* u8 : 1 after Start (texture create checks ctx+0x1c1) */

#define AS_CACHE_MIN_MB  128         /* engine setting range (registration fn 0x610100: 0x80..0x1000) */
#define AS_CACHE_MAX_MB  4096

typedef struct as_config {
    int enabled;            /* 0 = install nothing (default) */
    int texture_streaming;  /* 1 = force the engine's texture streamer on */
    int cache_mb;           /* streamer budget in MB; 0 = keep the value from the game settings */
} as_config;

typedef struct as_apply_result {
    int changed;            /* descriptor was modified */
    int rejected;           /* descriptor looked wrong -> left untouched */
    int old_streaming, new_streaming;
    uint64_t old_cache, new_cache;   /* bytes */
} as_apply_result;

extern const char AS_DEFAULT_INI[];

void as_config_defaults(as_config *c);
/* Parses ini text over the current values. Returns number of bad lines; first error into err. */
int as_config_parse(as_config *c, const char *text, char *err, size_t errn);
void as_config_clamp(as_config *c);

/* Plausibility of the descriptor as the 1.15.1 settings reader fills it: streaming is 0/1 and the
 * cache is a whole number of MB in 1..65536 MB. */
int as_desc_plausible(const uint8_t *desc);

/* Applies the config to the descriptor (fail closed: implausible = untouched, rejected = 1). */
as_apply_result as_apply(const as_config *c, uint8_t *desc);

#endif
