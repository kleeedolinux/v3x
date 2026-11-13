/* frame_pacer policy + config. See fp_logic.h. */
#include "fp_logic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Built-in configuration, used when v3x/frame_pacer.ini does not exist.
 * Must stay byte-identical to plugins/frame_pacer/frame_pacer.ini.example (the test checks it). */
const char FP_DEFAULT_INI[] =
"# frame_pacer configuration. Copy to <game>/binaries/v3x/frame_pacer.ini and edit.\n"
"# Without that file the plugin uses exactly these built-in defaults.\n"
"#\n"
"# enabled             1/0. 0 = the plugin installs nothing.\n"
"# replace_busy_wait   1 = the engine's own frame cap (Settings > Framerate cap) spins a CPU core\n"
"#                     at 100% until the next frame; replace that with a high-resolution timer\n"
"#                     sleep. Same frame rate, less CPU heat, which leaves more of the shared\n"
"#                     power budget to the integrated GPU.\n"
"# idle_fps            cap while idle. Idle = no keyboard/mouse input for idle_after_s, or the\n"
"#                     simulation is paused and no input for paused_idle_after_s. Any input\n"
"#                     ends idle on the next frame. 0 = no idle cap.\n"
"# idle_after_s        see idle_fps.\n"
"# paused_idle_after_s see idle_fps.\n"
"# background_fps      cap while another window is in the foreground. 0 = off.\n"
"# minimized_fps       cap while the game is minimised. 0 = off.\n"
"# sim_idle_ms         the simulation counts as paused after this long without tick activity.\n"
"# tick_render_hz      while a simulation tick runs, the engine hands the game state to the\n"
"#                     renderer whenever the tick held it for GameStateTick.MinLockTime ms\n"
"#                     (default 1 ms). This sets MinLockTime = 1000 / tick_render_hz, so the\n"
"#                     tick runs longer between renders: faster ticks, choppier frames while\n"
"#                     the simulation is busy. 0 = keep the engine value (default; 10 gave\n"
"#                     >100 ms frames while ticks ran; try 30 if you want more tick speed).\n"
"# log_interval_s      status line in v3x.log every N seconds with the true engine frame\n"
"#                     times (measured once per engine frame). 0 = never.\n"
"\n"
"[frame_pacer]\n"
"enabled = 1\n"
"replace_busy_wait = 1\n"
"idle_fps = 20\n"
"idle_after_s = 30\n"
"paused_idle_after_s = 3\n"
"background_fps = 20\n"
"minimized_fps = 5\n"
"sim_idle_ms = 3000\n"
"tick_render_hz = 0\n"
"log_interval_s = 10\n";

void fp_config_defaults(fp_config *c)
{
    c->enabled = 1;
    c->replace_busy_wait = 1;
    c->idle_fps = 20;
    c->idle_after_s = 30;
    c->paused_idle_after_s = 3;
    c->background_fps = 20;
    c->minimized_fps = 5;
    c->sim_idle_ms = 3000;
    c->tick_render_hz = 0;
    c->log_interval_s = 10;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

int fp_config_parse(fp_config *c, const char *text, char *err, size_t errn)
{
    static const struct { const char *k; size_t off; } keys[] = {
        {"enabled", offsetof(fp_config, enabled)},
        {"replace_busy_wait", offsetof(fp_config, replace_busy_wait)},
        {"idle_fps", offsetof(fp_config, idle_fps)},
        {"idle_after_s", offsetof(fp_config, idle_after_s)},
        {"paused_idle_after_s", offsetof(fp_config, paused_idle_after_s)},
        {"background_fps", offsetof(fp_config, background_fps)},
        {"minimized_fps", offsetof(fp_config, minimized_fps)},
        {"sim_idle_ms", offsetof(fp_config, sim_idle_ms)},
        {"tick_render_hz", offsetof(fp_config, tick_render_hz)},
        {"log_interval_s", offsetof(fp_config, log_interval_s)},
    };
    int bad = 0, line_no = 0;
    if (err && errn) err[0] = 0;
    const char *p = text;
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

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void fp_config_clamp(fp_config *c)
{
    c->enabled = c->enabled ? 1 : 0;
    c->replace_busy_wait = c->replace_busy_wait ? 1 : 0;
    /* fps caps: 0 = off, otherwise 1..1000 */
    c->idle_fps = c->idle_fps <= 0 ? 0 : clampi(c->idle_fps, 1, 1000);
    c->background_fps = c->background_fps <= 0 ? 0 : clampi(c->background_fps, 1, 1000);
    c->minimized_fps = c->minimized_fps <= 0 ? 0 : clampi(c->minimized_fps, 1, 1000);
    c->idle_after_s = clampi(c->idle_after_s, 1, 86400);
    c->paused_idle_after_s = clampi(c->paused_idle_after_s, 0, 86400);
    c->sim_idle_ms = clampi(c->sim_idle_ms, 100, 600000);
    c->tick_render_hz = c->tick_render_hz <= 0 ? 0 : clampi(c->tick_render_hz, 1, 1000);
    c->log_interval_s = clampi(c->log_interval_s, 0, 86400);
}

static double lowest(double a, double b)
{
    if (a <= 0) return b;
    if (b <= 0) return a;
    return a < b ? a : b;
}

double fp_choose_fps(const fp_config *c, const fp_state *s, const char **why)
{
    double policy = 0;
    const char *w = "engine";
    if (s->minimized && c->minimized_fps > 0) { policy = c->minimized_fps; w = "minimized"; }
    else if (!s->foreground && c->background_fps > 0) { policy = c->background_fps; w = "background"; }
    else if (c->idle_fps > 0 &&
             (s->input_idle_s >= c->idle_after_s ||
              (!s->sim_running && s->input_idle_s >= c->paused_idle_after_s))) {
        policy = c->idle_fps; w = s->sim_running ? "idle" : "paused-idle";
    }
    double fps = lowest(s->engine_cap_fps, policy);
    if (fps == s->engine_cap_fps && fps > 0 && (policy <= 0 || s->engine_cap_fps <= policy)) w = "engine";
    if (fps <= 0) w = "none";
    if (why) *why = w;
    return fps;
}

int fp_min_lock_ms(const fp_config *c)
{
    if (c->tick_render_hz <= 0) return 0;
    int ms = 1000 / c->tick_render_hz;
    return ms < 1 ? 1 : ms;
}
