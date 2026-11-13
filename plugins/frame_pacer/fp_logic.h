/* frame_pacer policy + config: pure C, no Windows or engine dependencies, so
 * test/frame_pacer_test.c can compile it directly (#include "../plugins/frame_pacer/fp_logic.c"). */
#ifndef FP_LOGIC_H
#define FP_LOGIC_H
#include <stddef.h>

typedef struct fp_config {
    int enabled;              /* 0 = install nothing */
    int replace_busy_wait;    /* 1 = the engine's spin-wait frame cap is replaced by a timer sleep */
    int idle_fps;             /* cap while idle (see fp_choose_fps); 0 = no idle cap */
    int idle_after_s;         /* no input for this long -> idle, even while the game runs */
    int paused_idle_after_s;  /* no input for this long while the simulation is paused -> idle */
    int background_fps;       /* cap while the game window is not in the foreground; 0 = off */
    int minimized_fps;        /* cap while minimised; 0 = off */
    int sim_idle_ms;          /* no tick activity for this long = simulation paused */
    int tick_render_hz;       /* max sub-tick renders per second while a tick runs; 0 = engine default */
    int log_interval_s;       /* status line in v3x.log every N s; 0 = never */
} fp_config;

typedef struct fp_state {
    int minimized;            /* game window iconic */
    int foreground;           /* game window is the foreground window */
    int sim_running;          /* tick activity seen within sim_idle_ms */
    double input_idle_s;      /* seconds since the last keyboard/mouse input */
    double engine_cap_fps;    /* the user's setting_framerate_cap as the engine would apply it, 0 = none */
} fp_state;

extern const char FP_DEFAULT_INI[];

void fp_config_defaults(fp_config *c);
/* Parses ini text over the current values. Returns number of bad lines; first error into err. */
int fp_config_parse(fp_config *c, const char *text, char *err, size_t errn);
/* Clamps values into safe ranges (after parsing). */
void fp_config_clamp(fp_config *c);

/* Frame rate to pace to this frame, 0 = do not pace. Lowest positive of the engine cap and the
 * policy cap that applies (minimized > background > idle). */
double fp_choose_fps(const fp_config *c, const fp_state *s, const char **why);

/* GameStateTick.MinLockTime value (ms) for tick_render_hz; returns 0 to leave the engine value. */
int fp_min_lock_ms(const fp_config *c);

#endif
