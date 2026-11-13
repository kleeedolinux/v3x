/* frame_pacer: finding the engine code it needs, through the V3X_API scanners only, so the
 * offline test can run the exact same code against the real victoria3.exe mapped as data. */
#ifndef FP_RESOLVE_H
#define FP_RESOLVE_H
#include <stdint.h>
#include "v3x.h"

/* Offsets proven by the byte patterns below (1.15.1). */
#define FP_SELF_SETTINGS   0x20   /* frame-end object -> graphics-settings owner */
#define FP_CAP_ENABLED     0x37   /* owner: u8, framerate cap active */
#define FP_CAP_FRAMETIME   0x38   /* owner: double, 1.0 / cap fps (seconds) */

typedef struct fp_anchors {
    uint8_t *frame_end;     /* main-thread end-of-frame fn holding the engine's spin-wait cap */
    uint8_t *spin;          /* the matched spin-wait block inside it */
    void **vsync_setting;   /* global CPdxSetting* read by that block (vsync => no cap) */
    uint8_t *tick_guard;    /* CRenderFrameIfNeeded-style guard ctor, run by sim tick tasks */
    int32_t *min_lock_ms;   /* console var GameStateTick.MinLockTime (int ms) */
    uint8_t *present;       /* CGfxDX11SwapChain::Present wrapper (informational, not hooked) */
    char err_pace[96], err_tick[96], err_lock[96], err_present[96];
} fp_anchors;

/* Fills what it can; a NULL member means that feature must stay off. Returns number found (0..3,
 * present not counted). */
int fp_resolve(const V3X_API *api, fp_anchors *a);

/* Engine semantics of a CPdxSetting<bool>-style value read, as done by the spin block. */
int fp_read_bool_setting(const uint8_t *setting);

#endif
