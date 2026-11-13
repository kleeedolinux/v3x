/* frame_pacer: adaptive frame cap + timer sleep instead of the engine's spin-wait + sub-tick
 * render throttle. RE notes: docs/re/graphics.md section 6.
 *
 * 1. The engine's frame cap (Settings > Framerate cap) is a busy-wait: at the end of every main-
 *    thread frame it spins with _mm_pause until the frame time is reached (1.15.1 fn 0x379a540,
 *    loop at 0x379a710). That keeps one core at 100%, which on a 15 W laptop chip takes power from
 *    the iGPU. We hook that function, pace with a high-resolution waitable timer ourselves and clear
 *    the cap flag for the duration of the original call, so the engine's spin does nothing.
 * 2. On top of the user's cap: idle_fps when nobody touches the input (shorter timeout while the
 *    simulation is paused), background_fps when another window is in front, minimized_fps when
 *    minimised. Any input restores the normal rate on the next frame.
 * 3. Simulation-running detection: a hook on the guard object every tick task constructs before
 *    it may yield the game state to the renderer (fn 0x1468a60) only stamps the time.
 * 4. GameStateTick.MinLockTime (int ms, default 1) := 1000 / tick_render_hz: the tick holds the
 *    game state at least that long before letting the main thread render a frame mid-tick.
 * Every part fails closed on its own: an anchor that does not verify byte-for-byte = that part off.
 * Config: <game>/binaries/v3x/frame_pacer.ini (defaults = frame_pacer.ini.example). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "v3x.h"
#include "fp_logic.h"
#include "fp_resolve.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

V3X_EXPORT const char *v3x_plugin_name = "frame_pacer";

typedef void (*frame_end_fn)(void *self);
typedef void *(*tick_guard_fn)(void *self, unsigned char active, unsigned char check_time, void *name);

static const V3X_API *api;
static fp_config g_cfg;
static fp_anchors g_a;
static frame_end_fn orig_frame_end;
static tick_guard_fn orig_tick_guard;
static double g_qpf;                       /* QPC ticks per second */
static volatile LONG64 g_tick_qpc;         /* last tick activity */
static LONG64 g_last_pace;                 /* QPC at the end of the last paced wait */
static HANDLE g_timer;
static int g_hires;
static HWND g_hwnd;
static ULONGLONG g_hwnd_check;
static int g_lock_written;
static int g_lock_orig = -1;
static ULONGLONG g_last_status;
/* status (main thread writes, console thread reads: torn values are harmless) */
static volatile double g_cur_fps;
static const char *volatile g_cur_why = "none";
static volatile double g_engine_cap;
static volatile LONG64 g_frames, g_waits;
/* true engine frames (frame_end to frame_end), window since the last status line */
static LONG64 g_prev_frame_qpc;
static volatile LONG64 g_win_frames, g_win_hitch33, g_win_hitch100;
static volatile double g_win_sum_s, g_win_max_s;
static volatile double g_wait_s_total;

#define TAG "frame_pacer"

static LONG64 qpc(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

/* ------------------------------------------------------------------ waiting */

static void wait_until(LONG64 deadline)
{
    LONG64 now = qpc();
    if (now >= deadline) return;
    double margin = g_hires ? 0.0007 : 0.0025;     /* finish with a short spin */
    double rem = (double)(deadline - now) / g_qpf;
    if (g_timer && rem > margin) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)((rem - margin) * 1e7);   /* relative, 100 ns units */
        if (due.QuadPart < 0 && SetWaitableTimerEx(g_timer, &due, 0, NULL, NULL, NULL, 0))
            WaitForSingleObject(g_timer, 1000);
    } else if (!g_timer && rem > margin) {
        Sleep((DWORD)((rem - margin) * 1000));
    }
    while (qpc() < deadline) YieldProcessor();
}

static void pace(double fps)
{
    LONG64 now = qpc();
    if (fps <= 0) { g_last_pace = now; return; }
    LONG64 interval = (LONG64)(g_qpf / fps);
    LONG64 deadline = g_last_pace + interval;
    if (g_last_pace == 0 || deadline < now - interval) {   /* first frame or far behind: resync */
        g_last_pace = now;
        return;
    }
    if (deadline > now) {
        wait_until(deadline);
        g_waits++;
        g_wait_s_total += (double)(deadline - now) / g_qpf;
        g_last_pace = deadline;
    } else {
        g_last_pace = now;
    }
}

/* ------------------------------------------------------------------ window / input state */

typedef struct { DWORD pid; HWND best; LONG area; HWND skip; } find_ctx;

static BOOL CALLBACK enum_cb(HWND h, LPARAM lp)
{
    find_ctx *c = (find_ctx *)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid || h == c->skip || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    RECT r;
    if (!GetWindowRect(h, &r)) return TRUE;
    LONG area = (r.right - r.left) * (r.bottom - r.top);
    if (IsIconic(h)) area = 1;          /* still a candidate while minimised */
    if (!c->best || area > c->area) { c->best = h; c->area = area; }
    return TRUE;
}

static void refresh_window(void)
{
    ULONGLONG t = GetTickCount64();
    if (g_hwnd && IsWindow(g_hwnd) && t - g_hwnd_check < 5000) return;
    find_ctx c = {GetCurrentProcessId(), NULL, 0, GetConsoleWindow()};
    EnumWindows(enum_cb, (LPARAM)&c);
    g_hwnd = c.best;
    g_hwnd_check = t;
}

static void read_state(fp_state *s, double engine_cap)
{
    refresh_window();
    s->minimized = g_hwnd ? IsIconic(g_hwnd) != 0 : 0;
    s->foreground = g_hwnd ? GetForegroundWindow() == g_hwnd : 1;
    LASTINPUTINFO li = {sizeof li, 0};
    s->input_idle_s = GetLastInputInfo(&li) ? (double)(DWORD)(GetTickCount() - li.dwTime) / 1000.0 : 0.0;
    LONG64 tq = g_tick_qpc;
    s->sim_running = g_a.tick_guard ? (tq && (double)(qpc() - tq) / g_qpf * 1000.0 < g_cfg.sim_idle_ms) : 1;
    s->engine_cap_fps = engine_cap;
}

/* ------------------------------------------------------------------ hooks */

static void hk_frame_end(void *self)
{
    uint8_t *owner = self ? *(uint8_t **)((uint8_t *)self + FP_SELF_SETTINGS) : NULL;
    double engine_cap = 0;
    if (owner && owner[FP_CAP_ENABLED]) {
        const uint8_t *vs = g_a.vsync_setting ? (const uint8_t *)*g_a.vsync_setting : NULL;
        double ft = *(double *)(owner + FP_CAP_FRAMETIME);
        if (!(vs && fp_read_bool_setting(vs)) && ft > 0) engine_cap = 1.0 / ft;
    }
    fp_state st;
    read_state(&st, engine_cap);
    const char *why;
    double fps = fp_choose_fps(&g_cfg, &st, &why);
    int take = g_cfg.replace_busy_wait && engine_cap > 0;
    if (!take && fps == engine_cap) fps = 0;   /* engine paces itself (replace_busy_wait = 0) */
    pace(fps);
    g_cur_fps = fps; g_cur_why = why; g_engine_cap = engine_cap; g_frames++;
    {
        LONG64 t = qpc();
        if (g_prev_frame_qpc) {
            double dt = (double)(t - g_prev_frame_qpc) / g_qpf;
            g_win_frames++; g_win_sum_s += dt;
            if (dt > g_win_max_s) g_win_max_s = dt;
            if (dt > 0.033) g_win_hitch33++;
            if (dt > 0.100) g_win_hitch100++;
        }
        g_prev_frame_qpc = t;
    }

    if (take) owner[FP_CAP_ENABLED] = 0;
    orig_frame_end(self);
    if (take && owner[FP_CAP_ENABLED] == 0) owner[FP_CAP_ENABLED] = 1;
}

static void *hk_tick_guard(void *self, unsigned char active, unsigned char check_time, void *name)
{
    InterlockedExchange64(&g_tick_qpc, qpc());
    return orig_tick_guard(self, active, check_time, name);
}

/* ------------------------------------------------------------------ frame tick / status */

static void apply_min_lock(void)
{
    int ms = fp_min_lock_ms(&g_cfg);
    if (!g_a.min_lock_ms || !ms) return;
    if (g_lock_orig < 0) g_lock_orig = *g_a.min_lock_ms;
    if (*g_a.min_lock_ms != ms) *g_a.min_lock_ms = ms;   /* .data, writable */
}

static void status_line(char *buf, size_t n)
{
    double avg_wait = g_waits ? g_wait_s_total / (double)g_waits * 1000.0 : 0;
    double avg_ft = g_win_frames ? g_win_sum_s / (double)g_win_frames * 1000.0 : 0;
    snprintf(buf, n, "engine frames=%lld avg %.1f ms (%.1f fps) max %.1f ms, >33ms %lld, >100ms %lld | "
             "pace=%.1f fps (%s), engine cap=%.1f, waits=%lld (avg %.2f ms), MinLockTime=%d ms, "
             "hooks: frame_end=%s tick=%s",
             (long long)g_win_frames, avg_ft, avg_ft > 0 ? 1000.0 / avg_ft : 0, g_win_max_s * 1000.0,
             (long long)g_win_hitch33, (long long)g_win_hitch100,
             g_cur_fps, g_cur_why, g_engine_cap, (long long)g_waits, avg_wait,
             g_a.min_lock_ms ? *g_a.min_lock_ms : -1, orig_frame_end ? "on" : "off",
             orig_tick_guard ? "on" : "off");
    g_win_frames = 0; g_win_sum_s = 0; g_win_max_s = 0; g_win_hitch33 = 0; g_win_hitch100 = 0;
}

static void on_frame(void *user)
{
    (void)user;
    if (!g_lock_written) { apply_min_lock(); g_lock_written = 1; }
    if (g_cfg.log_interval_s > 0) {
        ULONGLONG t = GetTickCount64();
        if (t - g_last_status >= (ULONGLONG)g_cfg.log_interval_s * 1000) {
            g_last_status = t;
            char b[320]; status_line(b, sizeof b);
            api->log(TAG, "%s", b);
        }
    }
}

static void cmd_status(const char *args, void *user)
{
    (void)args; (void)user;
    char b[320]; status_line(b, sizeof b);
    api->print("frame_pacer: %s", b);
}

/* ------------------------------------------------------------------ load */

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = (n >= 0 && n < (1 << 20)) ? (char *)malloc((size_t)n + 1) : NULL;
    if (b) { b[fread(b, 1, (size_t)n, f)] = 0; }
    fclose(f);
    return b;
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    if (a->api_version < 1 || a->struct_size < offsetof(V3X_API, v3x_dir) + sizeof(a->v3x_dir)) return 1;

    fp_config_defaults(&g_cfg);
    char path[MAX_PATH], err[128];
    snprintf(path, sizeof path, "%sframe_pacer.ini", a->v3x_dir());
    char *txt = read_file(path);
    int bad = fp_config_parse(&g_cfg, txt ? txt : FP_DEFAULT_INI, err, sizeof err);
    if (bad) {   /* fail closed: a broken config means no changes at all */
        a->log(TAG, "%s: %d bad line(s), first: %s -> plugin disabled", txt ? path : "built-in config", bad, err);
        free(txt);
        return 0;
    }
    free(txt);
    fp_config_clamp(&g_cfg);
    if (!g_cfg.enabled) { a->log(TAG, "disabled in config"); return 0; }

    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpf = (double)f.QuadPart;

    int found = fp_resolve(a, &g_a);
    uintptr_t base = a->image_base();
#define RVA(p) ((unsigned long long)((uintptr_t)(p) - base))
    a->log(TAG, "anchors: frame_end=%#llx tick_guard=%#llx MinLockTime=%#llx present=%#llx (%d/3)",
           g_a.frame_end ? RVA(g_a.frame_end) : 0, g_a.tick_guard ? RVA(g_a.tick_guard) : 0,
           g_a.min_lock_ms ? RVA(g_a.min_lock_ms) : 0, g_a.present ? RVA(g_a.present) : 0, found);
    if (g_a.err_pace[0]) a->log(TAG, "frame pacing OFF: %s", g_a.err_pace);
    if (g_a.err_tick[0]) a->log(TAG, "sim detection OFF (paused-idle never triggers): %s", g_a.err_tick);
    if (g_a.err_lock[0]) a->log(TAG, "tick render throttle OFF: %s", g_a.err_lock);

    if (g_a.tick_guard) {
        int r = a->hook(g_a.tick_guard, (void *)hk_tick_guard, (void **)&orig_tick_guard);
        if (r) { a->log(TAG, "tick guard hook failed (%d)", r); orig_tick_guard = NULL; g_a.tick_guard = NULL; }
    }
    if (g_a.frame_end) {
        g_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        g_hires = g_timer != NULL;
        if (!g_timer) g_timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
        int r = a->hook(g_a.frame_end, (void *)hk_frame_end, (void **)&orig_frame_end);
        if (r) { a->log(TAG, "frame_end hook failed (%d)", r); orig_frame_end = NULL; }
        else a->log(TAG, "pacing on (%s timer): idle %d fps after %d s (paused: %d s), background %d, minimized %d",
                    g_hires ? "high-resolution" : "standard", g_cfg.idle_fps, g_cfg.idle_after_s,
                    g_cfg.paused_idle_after_s, g_cfg.background_fps, g_cfg.minimized_fps);
    }
    apply_min_lock();   /* .data initialiser is already in place; on_frame re-applies once */
    if (g_a.min_lock_ms && fp_min_lock_ms(&g_cfg))
        a->log(TAG, "GameStateTick.MinLockTime %d -> %d ms (tick_render_hz %d)", g_lock_orig,
               *g_a.min_lock_ms, g_cfg.tick_render_hz);

    a->on_frame(on_frame, NULL);
    if (a->api_version >= 2 && a->struct_size >= offsetof(V3X_API, print) + sizeof(a->print) && a->register_command)
        a->register_command("frame_pacer", "frame_pacer status", cmd_status, NULL);
    return 0;
}
