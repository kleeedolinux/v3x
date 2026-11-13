/* asset_stream: finding the engine code through the V3X_API scanners only, so the offline test
 * runs the exact same code against the real victoria3.exe mapped as data. */
#ifndef AS_RESOLVE_H
#define AS_RESOLVE_H
#include <stdint.h>
#include "v3x.h"

typedef struct as_anchors {
    uint8_t *master_init;     /* gfx master-context init (this=ctx, rdx=init descriptor) */
    uint8_t *gate;            /* "if (desc->streaming) streamer.Start(desc->cache)" inside it */
    uint8_t *streamer_start;  /* CGfxTextureStreamer::Start (this, budget bytes) */
    char err[128];
} as_anchors;

/* Returns 1 if every check passed (then all members are set), else 0 with err filled. */
int as_resolve(const V3X_API *api, as_anchors *a);

/* Offsets of the checks, exported for the offline test / tools/re_stream_check.py. */
extern const char AS_ANCHOR_STRING[];
extern const char AS_GATE_PAT[];

#endif
