/* enhance: engine-level single-player enhancements (V3X plugin). RE notes: docs/re/enhance.md.
 *
 * Config: v3x/enhance.ini, section [enhance] (defaults: enhance.ini.example). Each feature has
 * its own flag and its own fail-closed anchor: if any byte or count differs from what was
 * reversed for 1.15.1, that feature changes nothing and logs why. The other features still apply.
 *
 * 1. binary_saves_in_debug (default 1)
 *    With debug_mode on (console `debug_mode`, or the -debug_mode launch argument), every save
 *    (autosave, exit save, console `save`, save dialog) is written as uncompressed TEXT (header
 *    "SAV0100"). In the lead's session that was 156 MB per autosave with a ~3.2 s main-thread
 *    freeze each time. Without debug_mode the same code writes format 3 (zipped binary,
 *    "SAV0103", the normal release format), or the save_file_format setting for the dialog.
 *    This feature removes only the debug_mode override, so saves are exactly what a game without
 *    debug_mode writes. Nothing in the write path (temp file, rename, cloud) is changed.
 *    Anchors:
 *      D = debug_mode flag byte: `call; test al,al; je; mov byte [rip+D],1` right after an xref
 *          to "debug_mode" (launch argument parser), cross-checked: the "Turning debug ON" toggle
 *          must `sete byte [rip+D]`.
 *      exactly 2x `xor eax,eax; mov r8d|r9d,3; cmp [D],al; cmovne r8d|r9d,eax` -> cmovne = nop4
 *         (autosave rotation; save-by-name: console `save`, probably the exit save)
 *      exactly 1x `movzx eax,byte [D]; test al,al; je; cmp sil,1; jne; mov dil,5; jmp;
 *         mov edi,3; test al,al; cmovne edi,r14d` -> je = jmp (save dialog: keep the setting)
 *
 * 2. deflate_level (default 0 = keep the engine's level)
 *    1..9 = zlib level for zip writing (saves). Lower = faster, bigger files; any level is a
 *    valid deflate stream, so loading is unaffected. The engine already compresses in 1 MB
 *    chunks on several threads. The first engine level seen is logged.
 *    Anchor: the only `mov r9d,-15; mov [rsp+20h],9; mov r8d,8` (deflateInit2_ windowBits/memLevel/
 *    method) whose function also loads the string "1.2.11" just before and takes the level in ecx.
 *
 * 3. speed_table (default 0)
 *    Seconds per simulation tick (4 ticks = 1 day) for game speeds 1-4: speed1..speed4
 *    (engine: 2.0 1.0 0.5 0.25). Speed 5 is 0.0001 s, i.e. already "as fast as possible":
 *    the next tick is issued on the first frame after the previous tick finished, so a speed
 *    6/7 cannot be faster (see docs/re/enhance.md). Console: `speed` and `speed <1-4> <seconds>`.
 *    Anchor: the only `vmovdqu xmm0,[T]; vmovups [rbp+x],xmm0; vmovss xmm0,[T5]; vmovss [rbp+y],
 *    xmm0; movsxd rcx,[rax+70h]; vmovss xmm1,[rbp+rcx*4+x]` (the per-frame tick issuer), with
 *    T = {2,1,0.5,0.25}, T5 = 0.0001f and T referenced by exactly one instruction in .text.
 *
 * 4. SaveGameState hook (always installed when its anchor matches) + save_timing (default 1)
 *    Emits the API v3 events "save_begin" right before the engine saves and "save_end" right after
 *    (data = V3X_SaveBegin {name}, see v3x.h; sim_opt subscribes). save_timing=1 additionally logs
 *    every save to v3x.log: "save #3 'autosave_2' format 3 (zipped binary): 812 ms".
 *    Anchor: the one function that references "SaveGameState" (savegamemanagerbase.cpp,
 *    SaveGameState(this, name, format, flag)), with its 1.15.1 prologue and
 *    `movzx esi,r9b; movzx r13d,r8b` in the first 0x40 bytes.
 *
 * 5. lean_debug (default 0) + lean_gui / lean_tooltips / lean_theater_cache (default 1 each)
 *    debug_mode stays ON (the console and cheats need it: the console-blocked predicate and the
 *    console open check read the flag), but selected readers of the flag are redirected to a
 *    shadow byte S (in the zero slack after .data, end of its last page) that reads 0 = "debug off":
 *      lean_gui:           the GUI function InDebugMode (`movzx eax,byte [D]; ret`): hides the 25
 *                          `visible = "[InDebugMode]"` widgets (error deer/lion, FPS line, debug
 *                          buttons, debug parts of tooltips and panels)
 *      lean_tooltips:      12 reads that add debug text to tooltips / localization
 *                          (functions referencing DEBUG_JOURNAL_ENTRY_ID, NEUTRAL_SCRIPT_VALUE_FORMAT,
 *                          DEBUG_BUILDING_TOOLTIP, PLAYED_BY_AI_DESC, EVENT_DEBUG_TOOLTIP, POP_DEBUG_INFO,
 *                          MILITARY_THEATER_DEBUG_TOOLTIP, JOMINI_DEBUG_FORMAT)
 *      lean_theater_cache: the read that bypasses a per-frame cache in debug mode (the only caller of
 *                          the MILITARY_THEATER_DEBUG_TOOLTIP function: `cmp [D],sil; je cached`)
 *    Each group must match its exact 1.15.1 count, otherwise that group is skipped. Console
 *    `lean on|off` switches at run time (off = S mirrors the real flag every frame = vanilla).
 *
 * 6. fast_speed5: WITHDRAWN (key is read and ignored, logged). It posted the next tick from the
 *    tick-done message inside CPartialDailyTickCommand::Execute on the Network/Session thread. That
 *    let the session start the next tick while the main thread ran a console command. Console
 *    commands take the global random scope BEFORE they wait for Logic access (add_relations
 *    0x88b8a0: random scope 0x37879e0, then 0x3689380(2)). So both threads shared the single random
 *    pointer 0x540ae80, and the session's scope exit cleared it -> NULL read at 0x3787360 (crash
 *    13:37:44, "Random is blocked by thread 5052" = Network/Session thread). Vanilla only posts ticks
 *    from the main thread (CIngameIdlerLogic::Update), so no safe variant with a gain exists.
 *    See docs/re/enhance.md section 5.
 *
 * Console: `enhance` prints what was applied. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "v3x.h"

V3X_EXPORT const char *v3x_plugin_name = "enhance";

#define TAG "enhance"

static const V3X_API *api;

typedef struct EnhCfg {
    int binary_saves_in_debug;
    int deflate_level;
    int speed_table;
    float speed[4];
    int save_timing;
    int lean_debug, lean_gui, lean_tooltips, lean_theater_cache;
    int fast_speed5;
} EnhCfg;

static EnhCfg cfg = {1, 0, 0, {2.0f, 1.0f, 0.5f, 0.25f}, 1, 0, 1, 1, 1, 0};
static const float SPEED_VANILLA[4] = {2.0f, 1.0f, 0.5f, 0.25f};

/* image view used by the resolvers (the test feeds synthetic ones) */
typedef struct EnhImg {
    const uint8_t *text; size_t text_size;
} EnhImg;

/* what was found / applied, for the status command and the test */
typedef struct EnhState {
    const uint8_t *debug_flag;
    const uint8_t *cmov_site[2];
    const uint8_t *je_site;
    int saves;                 /* 1 applied, 0 off, <0 anchor failure code */
    const uint8_t *deflate_fn;
    int deflate;               /* 1 hooked, 0 off, <0 failure */
    volatile LONG engine_level_seen; /* engine level + 100 once seen, 0 = not yet */
    float *speed_tab;          /* T in .rdata */
    int speed;                 /* 1 applied, 0 off, <0 failure */
    const uint8_t *save_fn;
    int save_hook;             /* 1 hooked, <0 failure */
    int timing;                /* save_timing logging: 1 on, 0 off, <0 hook failure */
    volatile LONG saves_seen;
    volatile LONG events_sent; /* save_begin + save_end emits */
    double last_save_ms;
    /* lean_debug */
    uint8_t *shadow;           /* S */
    int lean;                  /* 1 applied (at least one group), 0 off, <0 failure */
    int lean_gui, lean_tooltips, lean_cache;   /* per group: patched reads, 0 off, <0 failure */
    volatile LONG lean_active; /* 1: S = 0, 0: S mirrors D */
} EnhState;

static EnhState st;

static const uint8_t *rip_target(const uint8_t *insn_end, const uint8_t *disp_at)
{
    int32_t d; memcpy(&d, disp_at, 4);
    return insn_end + d;
}

/* pattern with wildcards: mask byte 0 = any */
static int match_at(const uint8_t *p, const uint8_t *pat, const uint8_t *mask, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (mask[i] && p[i] != pat[i]) return 0;
    return 1;
}

/* ------------------------------------------------------------------ 1. debug flag + save sites */

/* D from the launch-argument parser: within 0x20 bytes after the xref (lea rdx,"debug_mode"),
 * `E8 rel32 / 84 C0 / 74 xx / C6 05 disp32 01`. Every xref that has the shape must agree. */
static const uint8_t *find_debug_flag_from_arg(void)
{
    const uint8_t *s = api->find_string("debug_mode");
    if (!s) return NULL;
    const uint8_t *found = NULL;
    for (int i = 0; i < 16; i++) {
        const uint8_t *x = api->find_xref(s, i);
        if (!x) break;
        const uint8_t *p = x + 7;
        for (int k = 0; k < 0x20; k++) {
            const uint8_t *q = p + k;
            if (q[0] == 0xE8 && q[5] == 0x84 && q[6] == 0xC0 && q[7] == 0x74 &&
                q[9] == 0xC6 && q[10] == 0x05 && q[15] == 0x01) {
                const uint8_t *d = rip_target(q + 16, q + 11);
                if (found && found != d) return NULL;   /* ambiguous */
                found = d;
                break;
            }
        }
    }
    return found;
}

/* cross-check: the debug toggle ("Turning debug ON") writes D with `sete byte [rip+D]` */
static int debug_flag_crosscheck(const uint8_t *d)
{
    const uint8_t *s = api->find_string("Turning debug ON");
    if (!s) return 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *x = api->find_xref(s, i);
        if (!x) break;
        for (int k = 0; k < 0x100; k++) {
            const uint8_t *q = x + k;
            if (q[0] == 0x0F && q[1] == 0x94 && q[2] == 0x05 && rip_target(q + 7, q + 3) == d) return 1;
        }
    }
    return 0;
}

/* scans .text once for both site shapes that reference d.
 * returns 0 if exactly 2 cmov sites and 1 je site, else a negative code */
static int find_save_sites(const EnhImg *im, const uint8_t *d, const uint8_t *cmov[2], const uint8_t **je)
{
    /* 33 C0 41 B8|B9 03 00 00 00 38 05 d32 44 0F 45 C0|C8 */
    static const uint8_t p1[] = {0x33, 0xC0, 0x41, 0x00, 0x03, 0x00, 0x00, 0x00, 0x38, 0x05, 0, 0, 0, 0, 0x44, 0x0F, 0x45, 0x00};
    static const uint8_t m1[] = {1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 0};
    /* 0F B6 05 d32 84 C0 74 xx 40 80 FE 01 75 05 40 B7 05 EB xx BF 03 00 00 00 84 C0 41 0F 45 FE */
    static const uint8_t p2[] = {0x0F, 0xB6, 0x05, 0, 0, 0, 0, 0x84, 0xC0, 0x74, 0, 0x40, 0x80, 0xFE, 0x01, 0x75, 0x05,
                                 0x40, 0xB7, 0x05, 0xEB, 0, 0xBF, 0x03, 0x00, 0x00, 0x00, 0x84, 0xC0, 0x41, 0x0F, 0x45, 0xFE};
    static const uint8_t m2[] = {1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1,
                                 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    int n1 = 0, n2 = 0;
    const uint8_t *t = im->text;
    size_t n = im->text_size;
    for (size_t i = 0; i + sizeof p2 <= n; i++) {
        uint8_t b = t[i];
        if (b == 0x33 && match_at(t + i, p1, m1, sizeof p1)) {
            uint8_t reg = t[i + 3], mod = t[i + 17];
            if (!((reg == 0xB8 && mod == 0xC0) || (reg == 0xB9 && mod == 0xC8))) continue;
            if (rip_target(t + i + 14, t + i + 10) != d) continue;
            if (n1 < 2) cmov[n1] = t + i + 14;
            n1++;
        } else if (b == 0x0F && match_at(t + i, p2, m2, sizeof p2)) {
            if (rip_target(t + i + 7, t + i + 3) != d) continue;
            if (n2 < 1) *je = t + i + 9;
            n2++;
        }
    }
    if (n1 != 2) return -10 - n1;
    if (n2 != 1) return -20 - n2;
    return 0;
}

static int apply_saves(const EnhImg *im)
{
    const uint8_t *d = find_debug_flag_from_arg();
    if (!d) { api->log(TAG, "binary_saves_in_debug: debug_mode flag not found, skipped"); return -1; }
    if (!debug_flag_crosscheck(d)) {
        api->log(TAG, "binary_saves_in_debug: debug toggle does not write exe+0x%llx, skipped",
                 (unsigned long long)(d - (const uint8_t *)api->image_base()));
        return -2;
    }
    st.debug_flag = d;
    int r = find_save_sites(im, d, st.cmov_site, &st.je_site);
    if (r) {
        api->log(TAG, "binary_saves_in_debug: save-format sites differ from 1.15.1 (code %d), skipped", r);
        return -3;
    }
    static const uint8_t nop4[4] = {0x0F, 0x1F, 0x40, 0x00};
    static const uint8_t jmp8 = 0xEB;
    uintptr_t b = api->image_base();
    for (int i = 0; i < 2; i++)
        if (api->patch((void *)st.cmov_site[i], nop4, 4)) { api->log(TAG, "binary_saves_in_debug: patch failed"); return -4; }
    if (api->patch((void *)st.je_site, &jmp8, 1)) { api->log(TAG, "binary_saves_in_debug: patch failed"); return -4; }
    api->log(TAG, "binary_saves_in_debug: on (debug flag exe+0x%llx; autosave exe+0x%llx, save-by-name exe+0x%llx, "
             "save dialog exe+0x%llx): debug_mode no longer forces uncompressed text saves",
             (unsigned long long)(d - (const uint8_t *)b), (unsigned long long)((uintptr_t)st.cmov_site[0] - b),
             (unsigned long long)((uintptr_t)st.cmov_site[1] - b), (unsigned long long)((uintptr_t)st.je_site - b));
    return 1;
}

/* ------------------------------------------------------------------ 2. deflate level */

typedef uintptr_t (*deflate_job_fn)(uintptr_t level, uintptr_t job, uintptr_t a3, uintptr_t a4);
static deflate_job_fn orig_deflate_job;

static uintptr_t hk_deflate_job(uintptr_t level, uintptr_t job, uintptr_t a3, uintptr_t a4)
{
    int eng = (int)(uint32_t)level;
    if (InterlockedCompareExchange(&st.engine_level_seen, eng + 100, 0) == 0)
        api->log(TAG, "deflate_level: engine asked for level %d, using %d", eng, cfg.deflate_level);
    return orig_deflate_job((uintptr_t)(uint32_t)cfg.deflate_level, job, a3, a4);
}

static const uint8_t *find_deflate_job(const EnhImg *im)
{
    static const uint8_t pat[] = {0x41, 0xB9, 0xF1, 0xFF, 0xFF, 0xFF, 0xC7, 0x44, 0x24, 0x20, 0x09, 0x00, 0x00, 0x00,
                                  0x41, 0xB8, 0x08, 0x00, 0x00, 0x00};
    const uint8_t *hit = NULL;
    int n = 0;
    for (size_t i = 0; i + sizeof pat <= im->text_size; i++)
        if (im->text[i] == 0x41 && !memcmp(im->text + i, pat, sizeof pat)) { hit = im->text + i; n++; }
    if (n != 1) return NULL;
    const uint8_t *fn = api->function_start(hit);
    if (!fn || hit - fn > 0x80) return NULL;
    /* "1.2.11" loaded by lea r64,[rip+x] between fn and hit; level arrives in ecx (mov edx,ecx) */
    int ver = 0, lvl = 0;
    for (const uint8_t *q = fn; q < hit; q++) {
        if ((q[0] == 0x48 || q[0] == 0x4C) && q[1] == 0x8D && (q[2] & 0xC7) == 0x05) {
            const uint8_t *s = rip_target(q + 7, q + 3);
            if ((uintptr_t)s > api->image_base() && (uintptr_t)s < api->image_base() + api->image_size() - 8 &&
                !memcmp(s, "1.2.11", 7)) ver = 1;
        }
        if (q[0] == 0x8B && q[1] == 0xD1) lvl = 1;
    }
    return ver && lvl ? fn : NULL;
}

static int apply_deflate(const EnhImg *im)
{
    const uint8_t *fn = find_deflate_job(im);
    if (!fn) { api->log(TAG, "deflate_level: zip deflateInit site not found, skipped"); return -1; }
    st.deflate_fn = fn;
    int r = api->hook((void *)fn, (void *)hk_deflate_job, (void **)&orig_deflate_job);
    if (r) { api->log(TAG, "deflate_level: hook failed (%d)", r); return -2; }
    api->log(TAG, "deflate_level: %d (zip deflate job exe+0x%llx)", cfg.deflate_level,
             (unsigned long long)((uintptr_t)fn - api->image_base()));
    return 1;
}

/* ------------------------------------------------------------------ 3. speed table */

/* number of rip-relative operands in .text that point at target (disp32 followed by 0/1/4 imm bytes) */
static int count_riprel(const EnhImg *im, const uint8_t *target)
{
    int n = 0;
    const uint8_t *t = im->text;
    for (size_t i = 0; i + 4 <= im->text_size; i++) {
        int32_t d; memcpy(&d, t + i, 4);
        uintptr_t e = (uintptr_t)(t + i + 4) + (uintptr_t)(intptr_t)d, g = (uintptr_t)target;
        if (e == g || e + 1 == g || e + 4 == g) n++;
    }
    return n;
}

static const uint8_t *find_speed_hit(const EnhImg *im, int *why);

static float *find_speed_table(const EnhImg *im, int *why)
{
    const uint8_t *hit = find_speed_hit(im, why);
    if (!hit) return NULL;
    float *tab = (float *)rip_target(hit + 8, hit + 4);
    const float *t5 = (const float *)rip_target(hit + 21, hit + 17);
    float v[4], f5;
    memcpy(v, tab, 16); memcpy(&f5, t5, 4);
    if (memcmp(v, SPEED_VANILLA, 16) || f5 != 0.0001f) { *why = -2; return NULL; }
    if (count_riprel(im, (const uint8_t *)tab) != 1) { *why = -3; return NULL; }
    *why = 0;
    return tab;
}

/* the per-frame tick issuer's speed-table load (unique) */
static const uint8_t *find_speed_hit(const EnhImg *im, int *why)
{
    static const uint8_t pat[] = {0xC5, 0xFA, 0x6F, 0x05, 0, 0, 0, 0, 0xC5, 0xF8, 0x11, 0x45, 0, 0xC5, 0xFA, 0x10, 0x05, 0, 0, 0, 0,
                                  0xC5, 0xFA, 0x11, 0x45, 0, 0x48, 0x63, 0x48, 0x70, 0xC5, 0xFA, 0x10, 0x4C, 0x8D, 0};
    static const uint8_t msk[] = {1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 1, 1, 1, 1, 0, 0, 0, 0,
                                  1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0};
    const uint8_t *hit = NULL;
    int n = 0;
    for (size_t i = 0; i + sizeof pat <= im->text_size; i++)
        if (im->text[i] == 0xC5 && match_at(im->text + i, pat, msk, sizeof pat)) { hit = im->text + i; n++; }
    if (n != 1) { *why = -10 - n; return NULL; }
    *why = 0;
    return hit;
}

static int write_speed(const float v[4])
{
    return st.speed_tab ? api->patch(st.speed_tab, v, 16) : -1;
}

static int apply_speed(const EnhImg *im)
{
    int why = 0;
    float *tab = find_speed_table(im, &why);
    if (!tab) { api->log(TAG, "speed_table: tick issuer / table differs from 1.15.1 (code %d), skipped", why); return -1; }
    st.speed_tab = tab;
    if (write_speed(cfg.speed)) { api->log(TAG, "speed_table: patch failed"); return -2; }
    api->log(TAG, "speed_table: on (table exe+0x%llx): seconds per tick speed1..4 = %.4g %.4g %.4g %.4g, speed5 unchanged (0.0001)",
             (unsigned long long)((uintptr_t)tab - api->image_base()), cfg.speed[0], cfg.speed[1], cfg.speed[2], cfg.speed[3]);
    return 1;
}

/* ------------------------------------------------------------------ 4. save timing */

typedef struct { const char *ptr; int len; } SaveName;   /* {char *, int len, bool} on the caller's stack */
typedef uintptr_t (*save_state_fn)(uintptr_t self, const SaveName *name, uintptr_t format, uintptr_t flag);
static save_state_fn orig_save_state;

static const char *format_name(unsigned f)
{
    switch (f) {
    case 0: return "uncompressed text";
    case 3: return "zipped binary";
    case 4: return "zipped text (zip_text_all)";
    case 5: return "zipped binary (zip_binary_all)";
    default: return "other";
    }
}

static int has_events(void)
{
    return api->api_version >= 3 && api->struct_size >= offsetof(V3X_API, emit_event) + sizeof(api->emit_event) &&
           api->emit_event;
}

static uintptr_t hk_save_state(uintptr_t self, const SaveName *name, uintptr_t format, uintptr_t flag)
{
    char nm[260];
    int len = name && name->ptr && name->len > 0 && name->len < (int)sizeof nm ? name->len : 0;
    if (len) memcpy(nm, name->ptr, (size_t)len);
    nm[len] = 0;
    V3X_SaveBegin ev = {len ? nm : NULL};
    int ev_ok = has_events();
    if (ev_ok) { api->emit_event("save_begin", &ev); InterlockedIncrement(&st.events_sent); }
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    uintptr_t r = orig_save_state(self, name, format, flag);
    QueryPerformanceCounter(&b);
    double ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
    st.last_save_ms = ms;
    LONG n = InterlockedIncrement(&st.saves_seen);
    if (ev_ok) { api->emit_event("save_end", &ev); InterlockedIncrement(&st.events_sent); }
    unsigned fm = (unsigned)(uint8_t)format;
    if (cfg.save_timing)
        api->log(TAG, "save #%ld '%s' format %u (%s): %.0f ms", (long)n, nm, fm, format_name(fm), ms);
    return r;
}

static const uint8_t *find_save_state(void)
{
    const uint8_t *s = api->find_string("SaveGameState");
    if (!s) return NULL;
    const uint8_t *x = api->find_xref(s, 0);
    if (!x || api->find_xref(s, 1)) return NULL;          /* exactly one reference */
    const uint8_t *fn = api->function_start(x);
    static const uint8_t pro[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x48, 0x89, 0x50, 0x10};
    static const uint8_t args[] = {0x41, 0x0F, 0xB6, 0xF1, 0x45, 0x0F, 0xB6, 0xE8};
    if (!fn || memcmp(fn, pro, sizeof pro)) return NULL;
    for (int k = 0; k < 0x40; k++)
        if (!memcmp(fn + k, args, sizeof args)) return fn;
    return NULL;
}

static int apply_save_hook(void)
{
    const uint8_t *fn = find_save_state();
    if (!fn) { api->log(TAG, "save hook: SaveGameState not found / differs from 1.15.1: no save_begin/save_end events, no save_timing"); return -1; }
    st.save_fn = fn;
    int r = api->hook((void *)fn, (void *)hk_save_state, (void **)&orig_save_state);
    if (r) { api->log(TAG, "save hook: hook failed (%d)", r); return -2; }
    api->log(TAG, "save hook: on (SaveGameState exe+0x%llx): events save_begin/save_end %s, save_timing %s",
             (unsigned long long)((uintptr_t)fn - api->image_base()), has_events() ? "on" : "off (API < v3)",
             cfg.save_timing ? "on" : "off");
    return 1;
}

/* ------------------------------------------------------------------ 5. lean_debug */

/* [start, end) of the primary .pdata entry containing addr */
static int fn_range(const uint8_t *addr, const uint8_t **s, const uint8_t **e)
{
    DWORD64 image = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry((DWORD64)(uintptr_t)addr, &image, NULL);
    if (!rf) return -1;
    for (int guard = 0; guard < 32; guard++) {
        const uint8_t *ui = (const uint8_t *)(uintptr_t)image + rf->UnwindData;
        if (!((ui[0] >> 3) & UNW_FLAG_CHAININFO)) break;
        rf = (PRUNTIME_FUNCTION)(ui + 4 + ((ui[2] + 1) & ~1u) * 2);
    }
    *s = (const uint8_t *)(uintptr_t)image + rf->BeginAddress;
    *e = (const uint8_t *)(uintptr_t)image + rf->EndAddress;
    return 0;
}

typedef struct { uint8_t *disp; int k; } DRef;   /* disp32 position, immediate bytes after it */

/* byte reads of [rip+D] in [s,e): cmp m8,imm8 (80 3D), cmp m8,r8 (38 /r), cmp r8,m8 (3A /r),
 * mov r8,m8 (8A /r), movzx r32,m8 (0F B6 /r); modrm must be rip-relative */
static int drefs_in(const uint8_t *s, const uint8_t *e, const uint8_t *d, DRef *out, int max)
{
    int n = 0;
    for (const uint8_t *p = s + 3; p + 4 <= e; p++) {
        int k;
        if (p[-2] == 0x80 && p[-1] == 0x3D) k = 1;
        else if ((p[-1] & 0xC7) == 0x05 && (p[-2] == 0x38 || p[-2] == 0x3A || p[-2] == 0x8A || (p[-2] == 0xB6 && p[-3] == 0x0F))) k = 0;
        else continue;
        if (rip_target(p + 4 + k, p) != d) continue;
        if (n < max) { out[n].disp = (uint8_t *)p; out[n].k = k; }
        n++;
    }
    return n;
}

static int add_unique(const uint8_t **set, int n, int max, const uint8_t *v)
{
    for (int i = 0; i < n; i++) if (set[i] == v) return n;
    if (n < max) set[n++] = v;
    return n;
}

#define LEAN_TOOLTIP_READS 12
static const char *const LEAN_TOOLTIP_STRINGS[] = {
    "DEBUG_JOURNAL_ENTRY_ID", "NEUTRAL_SCRIPT_VALUE_FORMAT", "DEBUG_BUILDING_TOOLTIP", "PLAYED_BY_AI_DESC",
    "EVENT_DEBUG_TOOLTIP", "POP_DEBUG_INFO", "MILITARY_THEATER_DEBUG_TOOLTIP", "JOMINI_DEBUG_FORMAT",
};

static int lean_tooltip_refs(const uint8_t *d, DRef *out, int max)
{
    const uint8_t *fns[64]; int nf = 0;
    for (size_t i = 0; i < sizeof LEAN_TOOLTIP_STRINGS / sizeof *LEAN_TOOLTIP_STRINGS; i++) {
        const uint8_t *str = api->find_string(LEAN_TOOLTIP_STRINGS[i]);
        if (!str) return -1;
        for (int x = 0; x < 64; x++) {
            const uint8_t *xr = api->find_xref(str, x);
            if (!xr) break;
            const uint8_t *fs, *fe;
            if (!fn_range(xr, &fs, &fe)) nf = add_unique(fns, nf, 64, fs);
        }
    }
    int n = 0;
    for (int i = 0; i < nf; i++) {
        const uint8_t *fs, *fe;
        if (fn_range(fns[i], &fs, &fe)) continue;
        n += drefs_in(fs, fe, d, out + (n < max ? n : max), n < max ? max - n : 0);
    }
    return n;
}

/* the theater-cache read: in the only caller of the MILITARY_THEATER_DEBUG_TOOLTIP function,
 * exactly one D read, shaped `40 38 35 D 74 xx` (cmp [D],sil; je cached) */
static int lean_cache_ref(const EnhImg *im, const uint8_t *d, DRef *out)
{
    const uint8_t *str = api->find_string("MILITARY_THEATER_DEBUG_TOOLTIP");
    const uint8_t *xr = str ? api->find_xref(str, 0) : NULL, *fs, *fe;
    if (!xr || fn_range(xr, &fs, &fe)) return -1;
    const uint8_t *site = NULL; int ns = 0;
    for (size_t i = 0; i + 5 <= im->text_size; i++)
        if (im->text[i] == 0xE8 && rip_target(im->text + i + 5, im->text + i + 1) == fs) { site = im->text + i; ns++; }
    if (ns != 1) return -2;
    const uint8_t *cs, *ce;
    if (fn_range(site, &cs, &ce)) return -3;
    DRef r[4];
    int n = drefs_in(cs, ce, d, r, 4);
    if (n != 1) return -4;
    const uint8_t *p = r[0].disp;
    if (!(p[-3] == 0x40 && p[-2] == 0x38 && p[-1] == 0x35 && p[4] == 0x74)) return -5;
    *out = r[0];
    return 1;
}

/* InDebugMode GUI getter: the only `0F B6 05 D C3` */
static int lean_gui_ref(const EnhImg *im, const uint8_t *d, DRef *out)
{
    int n = 0;
    for (size_t i = 0; i + 8 <= im->text_size; i++) {
        const uint8_t *p = im->text + i;
        if (p[0] == 0x0F && p[1] == 0xB6 && p[2] == 0x05 && p[7] == 0xC3 && rip_target(p + 7, p + 3) == d) {
            out->disp = (uint8_t *)p + 3; out->k = 0; n++;
        }
    }
    return n == 1 ? 1 : -10 - n;
}

/* S: 16 bytes before the end of the last page of .data, inside the zero slack after VirtualSize */
static uint8_t *find_shadow(void)
{
    const uint8_t *b = (const uint8_t *)api->image_base();
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(b + ((const IMAGE_DOS_HEADER *)b)->e_lfanew);
    const IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sh++) {
        if (memcmp(sh->Name, ".data", 6)) continue;
        if (!(sh->Characteristics & IMAGE_SCN_MEM_WRITE)) return NULL;
        uintptr_t end = (uintptr_t)b + sh->VirtualAddress + sh->Misc.VirtualSize;
        uintptr_t page_end = (end + 0xFFF) & ~(uintptr_t)0xFFF;
        if (page_end - end < 64) return NULL;
        uint8_t *sp = (uint8_t *)(page_end - 16);
        for (int k = 0; k < 16; k++) if (sp[k]) return NULL;
        return sp;
    }
    return NULL;
}

static int redirect(const DRef *r, int n, const uint8_t *target)
{
    for (int i = 0; i < n; i++) {
        intptr_t nd = (intptr_t)target - (intptr_t)(r[i].disp + 4 + r[i].k);
        if (nd != (int32_t)nd) return -1;
        int32_t v = (int32_t)nd;
        if (api->patch(r[i].disp, &v, 4)) return -2;
    }
    return 0;
}

static void lean_frame(void *u)
{
    (void)u;
    if (st.shadow && st.debug_flag) {
        uint8_t want = st.lean_active ? 0 : *st.debug_flag;
        if (*st.shadow != want) *st.shadow = want;
    }
}

static int apply_lean(const EnhImg *im)
{
    const uint8_t *d = st.debug_flag;
    if (!d) {
        d = find_debug_flag_from_arg();
        if (!d || !debug_flag_crosscheck(d)) { api->log(TAG, "lean_debug: debug_mode flag not found, skipped"); return -1; }
        st.debug_flag = d;
    }
    uint8_t *sp = find_shadow();
    if (!sp) { api->log(TAG, "lean_debug: no free shadow byte after .data, skipped"); return -2; }
    st.shadow = sp;
    st.lean_active = 1;
    int any = 0;
    uintptr_t b = api->image_base();
    if (cfg.lean_gui) {
        DRef r;
        int g = lean_gui_ref(im, d, &r);
        st.lean_gui = g < 0 ? g : redirect(&r, 1, sp) ? -3 : 1;
        api->log(TAG, "lean_gui: %s", st.lean_gui > 0 ? "on (GUI InDebugMode reads the shadow flag)" : "skipped (anchor mismatch)");
        any |= st.lean_gui > 0;
    }
    if (cfg.lean_tooltips) {
        DRef r[32];
        int n = lean_tooltip_refs(d, r, 32);
        st.lean_tooltips = n != LEAN_TOOLTIP_READS ? -10 - (n < 0 ? 0 : n) : redirect(r, n, sp) ? -3 : n;
        if (st.lean_tooltips > 0) api->log(TAG, "lean_tooltips: on (%d debug-tooltip reads)", n);
        else api->log(TAG, "lean_tooltips: skipped (%d debug-tooltip reads, want %d)", n, LEAN_TOOLTIP_READS);
        any |= st.lean_tooltips > 0;
    }
    if (cfg.lean_theater_cache) {
        DRef r;
        int c = lean_cache_ref(im, d, &r);
        st.lean_cache = c < 0 ? c : redirect(&r, 1, sp) ? -3 : 1;
        if (st.lean_cache > 0) api->log(TAG, "lean_theater_cache: on (read at exe+0x%llx)", (unsigned long long)((uintptr_t)r.disp - 3 - b));
        else api->log(TAG, "lean_theater_cache: skipped (code %d)", st.lean_cache);
        any |= st.lean_cache > 0;
    }
    if (!any) return -4;
    api->on_frame(lean_frame, NULL);
    api->log(TAG, "lean_debug: on (shadow flag exe+0x%llx = 0 while lean; console `lean off` = vanilla)",
             (unsigned long long)((uintptr_t)sp - b));
    return 1;
}

/* ------------------------------------------------------------------ config + commands */

static float clamp_speed(double v)
{
    if (!(v >= 0.0001)) v = 0.0001;   /* also catches NaN */
    if (v > 10.0) v = 10.0;
    return (float)v;
}

static void load_cfg(const char *ini)
{
    cfg.binary_saves_in_debug = GetPrivateProfileIntA("enhance", "binary_saves_in_debug", 1, ini) != 0;
    int lv = (int)GetPrivateProfileIntA("enhance", "deflate_level", 0, ini);
    cfg.deflate_level = lv < 0 || lv > 9 ? 0 : lv;
    cfg.speed_table = GetPrivateProfileIntA("enhance", "speed_table", 0, ini) != 0;
    cfg.save_timing = GetPrivateProfileIntA("enhance", "save_timing", 1, ini) != 0;
    cfg.lean_debug = GetPrivateProfileIntA("enhance", "lean_debug", 0, ini) != 0;
    cfg.lean_gui = GetPrivateProfileIntA("enhance", "lean_gui", 1, ini) != 0;
    cfg.lean_tooltips = GetPrivateProfileIntA("enhance", "lean_tooltips", 1, ini) != 0;
    cfg.lean_theater_cache = GetPrivateProfileIntA("enhance", "lean_theater_cache", 1, ini) != 0;
    cfg.fast_speed5 = GetPrivateProfileIntA("enhance", "fast_speed5", 0, ini) != 0;
    for (int i = 0; i < 4; i++) {
        char key[16], buf[64];
        snprintf(key, sizeof key, "speed%d", i + 1);
        GetPrivateProfileStringA("enhance", key, "", buf, sizeof buf, ini);
        cfg.speed[i] = buf[0] ? clamp_speed(strtod(buf, NULL)) : SPEED_VANILLA[i];
    }
}

static const char *state_str(int s) { return s > 0 ? "on" : s == 0 ? "off" : "SKIPPED (anchor mismatch, see v3x.log)"; }

static void cmd_enhance(const char *args, void *user)
{
    (void)args; (void)user;
    api->print("enhance: binary_saves_in_debug %s, deflate_level %s (%d; engine level %s), speed_table %s",
               state_str(st.saves), state_str(st.deflate), cfg.deflate_level,
               st.engine_level_seen ? "seen" : "not seen yet", state_str(st.speed));
    if (st.engine_level_seen) api->print("  engine deflate level: %d", (int)st.engine_level_seen - 100);
    if (st.debug_flag) api->print("  debug_mode is %s", *st.debug_flag ? "ON" : "off");
    api->print("  save hook %s, save_timing %s: %ld saves, last %.0f ms, %ld save events sent", state_str(st.save_hook),
               state_str(st.timing), (long)st.saves_seen, st.last_save_ms, (long)st.events_sent);
    api->print("  lean_debug %s (%s): gui %d, tooltip reads %d, theater cache %d", state_str(st.lean),
               st.lean_active ? "active" : "mirroring debug_mode", st.lean_gui, st.lean_tooltips, st.lean_cache);
}

static void cmd_lean(const char *args, void *user)
{
    (void)user;
    if (st.lean <= 0) { api->print("lean: lean_debug is %s (set lean_debug=1 in enhance.ini)", state_str(st.lean)); return; }
    if (!_stricmp(args, "on")) InterlockedExchange(&st.lean_active, 1);
    else if (!_stricmp(args, "off")) InterlockedExchange(&st.lean_active, 0);
    else if (args[0]) { api->print("usage: lean [on|off]"); return; }
    api->print("lean_debug %s (debug_mode is %s)", st.lean_active ? "ACTIVE: debug-only GUI/tooltips/cache bypass off"
               : "off: vanilla debug_mode behaviour", st.debug_flag && *st.debug_flag ? "ON" : "off");
}

static float g_pending[4];
static void main_write_speed(void *u)
{
    (void)u;
    if (write_speed(g_pending)) api->log(TAG, "speed: write failed");
    else memcpy(cfg.speed, g_pending, 16);
}

static void cmd_speed(const char *args, void *user)
{
    (void)user;
    if (!st.speed_tab) { api->print("speed: speed_table is %s (set speed_table=1 in enhance.ini)", state_str(st.speed)); return; }
    int n = 0; double sec = 0;
    if (sscanf(args, "%d %lf", &n, &sec) == 2 && n >= 1 && n <= 4) {
        memcpy(g_pending, cfg.speed, 16);
        g_pending[n - 1] = clamp_speed(sec);
        if (api->run_on_main(main_write_speed, NULL)) api->print("speed: main-thread queue full, try again");
        else api->print("speed %d -> %.4g s per tick (applied on the next frame)", n, g_pending[n - 1]);
        return;
    }
    if (args[0]) { api->print("usage: speed [<1-4> <seconds per tick>]"); return; }
    float v[4]; memcpy(v, st.speed_tab, 16);
    api->print("seconds per tick (4 ticks = 1 day): speed1 %.4g, speed2 %.4g, speed3 %.4g, speed4 %.4g, speed5 0.0001 (max)",
               v[0], v[1], v[2], v[3]);
}

/* .text range of the main module from its PE headers */
static int text_range(EnhImg *im)
{
    const uint8_t *b = (const uint8_t *)api->image_base();
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(b + ((const IMAGE_DOS_HEADER *)b)->e_lfanew);
    const IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++)
        if (!memcmp(s->Name, ".text", 6)) { im->text = b + s->VirtualAddress; im->text_size = s->Misc.VirtualSize; return 0; }
    return -1;
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    char ini[MAX_PATH];
    snprintf(ini, sizeof ini, "%senhance.ini", api->v3x_dir());
    load_cfg(ini);
    EnhImg im;
    if (text_range(&im)) { api->log(TAG, ".text not found"); return 1; }
    memset((void *)&st, 0, sizeof st);
    st.saves = cfg.binary_saves_in_debug ? apply_saves(&im) : 0;
    st.deflate = cfg.deflate_level ? apply_deflate(&im) : 0;
    st.speed = cfg.speed_table ? apply_speed(&im) : 0;
    st.save_hook = apply_save_hook();
    st.timing = st.save_hook > 0 ? cfg.save_timing : cfg.save_timing ? st.save_hook : 0;
    st.lean = cfg.lean_debug ? apply_lean(&im) : 0;
    if (cfg.fast_speed5)
        api->log(TAG, "fast_speed5: WITHDRAWN, ignored (it let a tick overlap console commands: random-scope crash). "
                 "Remove fast_speed5 from enhance.ini");
    if (api->api_version >= 2 && api->struct_size >= offsetof(V3X_API, exec_command) + sizeof(api->exec_command) &&
        api->register_command) {
        api->register_command("enhance", "enhance: which engine enhancements are active", cmd_enhance, NULL);
        api->register_command("speed", "speed [<1-4> <seconds per tick>]: show / set the game-speed table (enhance)", cmd_speed, NULL);
        api->register_command("lean", "lean [on|off]: debug_mode without its debug-only GUI/tooltips (enhance lean_debug)", cmd_lean, NULL);
    }
    api->log(TAG, "binary_saves_in_debug %s, deflate_level %s, speed_table %s, save hook %s (timing %s), lean_debug %s",
             state_str(st.saves), state_str(st.deflate), state_str(st.speed), state_str(st.save_hook), state_str(st.timing),
             state_str(st.lean));
    return 0;
}
