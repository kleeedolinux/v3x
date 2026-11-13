/* frame_pacer anchors. See fp_resolve.h and docs/re/graphics.md section 6. */
#include "fp_resolve.h"
#include <stdio.h>
#include <string.h>

/* The engine frame cap (1.15.1 RVA 0x379a6b1, fn 0x379a540):
 *   rdx = self->settings; if (rdx->cap_enabled && !vsync) {
 *       deadline = rdx->frametime + self->last_frame_start;
 *       while (timer_now(self->timer) < deadline) _mm_pause();   // 100% CPU spin
 *   }
 * The pattern pins every offset the plugin relies on (+0x20, +0x37, +0x38, the vsync setting
 * read through +0xc5/+0xc8/+0xd0/+0xf8/+0x100) and the pause loop itself. */
static const char SPIN_PAT[] =
    "48 8B 56 20 C5 7B 10 05 ?? ?? ?? ?? 80 7A 37 00 0F 84 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? "
    "80 B9 C5 00 00 00 02 75 10 48 8D 81 D0 00 00 00 F6 81 C8 00 00 00 01 EB 0E 48 8D 81 00 01 00 00 "
    "F6 81 F8 00 00 00 01 74 03 48 8B 00 80 38 00 75 54 C5 FB 10 42 38 C5 FB 58 7E 38 "
    "0F 1F 80 00 00 00 00 48 8B 5E 30 C5 FB 10 73 08 80 7B 10 00 75 2A 48 8D 8C 24 A0 00 00 00 "
    "E8 ?? ?? ?? ?? 48 8B 84 24 A0 00 00 00 48 2B 03 C5 F8 57 C0 C4 E1 FB 2A C0 C4 C1 7B 5E C8 "
    "C5 CB 58 F1 C5 F9 2F FE 76 04 F3 90 EB BD";
#define SPIN_VSYNC_MOV   0x16      /* 48 8B 0D rel32: mov rcx, [vsync setting] */
#define SPIN_FROM_START  0x171     /* spin block offset inside the frame-end function */
static const uint8_t FRAME_END_PROLOGUE[] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20,
    0x41, 0x56, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00};

/* Guard ctor (1.15.1 fn 0x1468a60): (this, bool active, bool check_min_lock_time, name) -> this.
 * Only constructed by simulation tick tasks; reads MinLockTime at +0xbf. */
static const uint8_t TICK_GUARD_PROLOGUE[] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x4C, 0x24, 0x08,
    0x57, 0x48, 0x83, 0xEC, 0x20, 0x41, 0x0F, 0xB6, 0xD8, 0x48, 0x8B, 0xF9, 0x88, 0x11};
#define TICK_GUARD_MINLOCK_READ 0xBF   /* 48 63 05 rel32: movsxd rax, [MinLockTime] */

static const char PRESENT_PAT[] =
    "48 89 5C 24 10 55 48 8D 6C 24 F0 48 81 EC 10 01 00 00 C5 F8 29 B4 24 00 01 00 00 33 DB 89 5D 20 "
    "38 1D ?? ?? ?? ?? 74 08 41 B8 01 00 00 00 EB 13";

static uint8_t *rip_target(uint8_t *insn, int disp_off, int len)
{
    int32_t d; memcpy(&d, insn + disp_off, 4);
    return insn + len + d;
}

int fp_read_bool_setting(const uint8_t *s)
{
    const uint8_t *p;
    if (s[0xc5] == 2) p = (s[0xc8] & 1) ? *(const uint8_t *const *)(s + 0xd0) : s + 0xd0;
    else p = (s[0xf8] & 1) ? *(const uint8_t *const *)(s + 0x100) : s + 0x100;
    return p && *p != 0;
}

static int in_image(const V3X_API *api, const void *p)
{
    uintptr_t b = api->image_base();
    return (uintptr_t)p >= b && (uintptr_t)p < b + api->image_size();
}

int fp_resolve(const V3X_API *api, fp_anchors *a)
{
    memset(a, 0, sizeof *a);
    int found = 0;

    /* 1. frame-end function + spin block */
    uint8_t *spin = (uint8_t *)api->pattern_scan(SPIN_PAT);
    if (!spin) snprintf(a->err_pace, sizeof a->err_pace, "spin-wait pattern not found");
    else {
        uint8_t *fn = (uint8_t *)api->function_start(spin);
        if (fn != spin - SPIN_FROM_START)
            snprintf(a->err_pace, sizeof a->err_pace, "spin block at fn+%#llx, expected +%#x",
                     fn ? (unsigned long long)(spin - fn) : 0ull, SPIN_FROM_START);
        else if (memcmp(fn, FRAME_END_PROLOGUE, sizeof FRAME_END_PROLOGUE))
            snprintf(a->err_pace, sizeof a->err_pace, "frame-end prologue mismatch");
        else {
            void **vs = (void **)rip_target(spin + SPIN_VSYNC_MOV, 3, 7);
            if (!in_image(api, vs)) snprintf(a->err_pace, sizeof a->err_pace, "vsync global outside image");
            else { a->frame_end = fn; a->spin = spin; a->vsync_setting = vs; found++; }
        }
    }

    /* 2. tick guard ctor: anchor string "...gamestatetick.cpp:262" used only inside it */
    const char *gs = (const char *)api->find_string("C:\\mnt\\gsg\\caligula\\caligula\\source\\logic\\gamestatetick.cpp:262");
    uint8_t *x = gs ? (uint8_t *)api->find_xref(gs, 0) : NULL;
    uint8_t *g = x ? (uint8_t *)api->function_start(x) : NULL;
    if (!gs) snprintf(a->err_tick, sizeof a->err_tick, "gamestatetick.cpp:262 string not found");
    else if (!x || !g) snprintf(a->err_tick, sizeof a->err_tick, "gamestatetick.cpp:262 xref/function not found");
    else if (api->find_xref(gs, 1)) snprintf(a->err_tick, sizeof a->err_tick, "gamestatetick.cpp:262 has >1 xref");
    else if (memcmp(g, TICK_GUARD_PROLOGUE, sizeof TICK_GUARD_PROLOGUE))
        snprintf(a->err_tick, sizeof a->err_tick, "tick guard prologue mismatch");
    else { a->tick_guard = g; found++; }

    /* 3. GameStateTick.MinLockTime: registered as  lea rdx,[var]; mov r8d,INT_MIN; (mov r9d earlier);
     *    lea rcx,["GameStateTick.MinLockTime"]  -> var = target of the lea rdx 13 bytes before. */
    const char *ms = (const char *)api->find_string("GameStateTick.MinLockTime");
    uint8_t *mx = ms ? (uint8_t *)api->find_xref(ms, 0) : NULL;
    static const uint8_t LEA_RDX[] = {0x48, 0x8D, 0x15}, MOV_R8D_MIN[] = {0x41, 0xB8, 0x00, 0x00, 0x00, 0x80};
    if (!mx) snprintf(a->err_lock, sizeof a->err_lock, "GameStateTick.MinLockTime string/xref not found");
    else if (memcmp(mx - 13, LEA_RDX, 3) || memcmp(mx - 6, MOV_R8D_MIN, 6))
        snprintf(a->err_lock, sizeof a->err_lock, "MinLockTime registration shape mismatch");
    else {
        int32_t *v = (int32_t *)rip_target(mx - 13, 3, 7);
        static const uint8_t MOVSXD[] = {0x48, 0x63, 0x05};
        if (!in_image(api, v) || *v < 0 || *v > 10000)
            snprintf(a->err_lock, sizeof a->err_lock, "MinLockTime value implausible");
        else if (a->tick_guard && (memcmp(a->tick_guard + TICK_GUARD_MINLOCK_READ, MOVSXD, 3) ||
                                   (int32_t *)rip_target(a->tick_guard + TICK_GUARD_MINLOCK_READ, 3, 7) != v))
            snprintf(a->err_lock, sizeof a->err_lock, "tick guard does not read this MinLockTime");
        else { a->min_lock_ms = v; found++; }
    }

    /* 4. Present wrapper (logged only) */
    uint8_t *pr = (uint8_t *)api->pattern_scan(PRESENT_PAT);
    if (!pr) snprintf(a->err_present, sizeof a->err_present, "Present wrapper pattern not found");
    else if ((uint8_t *)api->function_start(pr) != pr)
        snprintf(a->err_present, sizeof a->err_present, "Present pattern not at a function start");
    else a->present = pr;

    return found;
}
