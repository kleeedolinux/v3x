/* asset_stream anchors (1.15.1 RVAs in comments). Mirrored by tools/re_stream_check.py.
 *
 * 1. String "gfx-check-resource-category" is referenced exactly once, at master_init+0x23
 *    (fn 0x3c3fdc0, gfx_master_context.cpp). Its prologue saves rdx (the init descriptor) in r13
 *    and rcx (the master context) in r12.
 * 2. Gate (0x3c407af, unique in .text, must lie inside master_init):
 *        cmp byte [r13+0x14],0 ; je +0e ; lea rcx,[r12+0x10] ; mov rdx,[r13+0x18] ; call Start
 *    = desc+0x14 is the streaming flag, desc+0x18 the cache bytes, ctx+0x10 the streamer.
 * 3. Start (0x3c613e0) begins with  mov byte [rcx+0x1b1],1  (enabled) and stores the budget
 *    with  mov [rdi+0x1a0],rsi  at +0xa9. */
#include "as_resolve.h"
#include "as_logic.h"
#include <stdio.h>
#include <string.h>

const char AS_ANCHOR_STRING[] = "gfx-check-resource-category";
#define AS_STRING_XREF_OFF 0x23

static const uint8_t INIT_PROLOGUE[] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xF0, 0x02, 0x00, 0x00,
    0x4C, 0x8B, 0xEA,       /* mov r13, rdx  (descriptor) */
    0x4C, 0x8B, 0xE1};      /* mov r12, rcx  (master context) */

const char AS_GATE_PAT[] = "41 80 7D 14 00 74 0E 49 8D 4C 24 10 49 8B 55 18 E8";
#define AS_GATE_CALL 16                    /* E8 rel32 */

static const uint8_t START_HEAD[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30,
    0xC6, 0x81, 0xB1, 0x01, 0x00, 0x00, 0x01,  /* mov byte [rcx+0x1b1], 1 */
    0x48, 0x8B, 0xF2};                         /* mov rsi, rdx (budget) */
#define AS_START_BUDGET_OFF 0xA9
static const uint8_t START_BUDGET[] = {0x48, 0x89, 0xB7, 0xA0, 0x01, 0x00, 0x00};  /* mov [rdi+0x1a0], rsi */

int as_resolve(const V3X_API *api, as_anchors *a)
{
    memset(a, 0, sizeof *a);
    uintptr_t base = api->image_base(), end = base + api->image_size();
    const char *s = (const char *)api->find_string(AS_ANCHOR_STRING);
    if (!s) { snprintf(a->err, sizeof a->err, "string '%s' not found", AS_ANCHOR_STRING); return 0; }
    uint8_t *x = (uint8_t *)api->find_xref(s, 0);
    if (!x) { snprintf(a->err, sizeof a->err, "no xref to '%s'", AS_ANCHOR_STRING); return 0; }
    if (api->find_xref(s, 1)) { snprintf(a->err, sizeof a->err, "'%s' has more than one xref", AS_ANCHOR_STRING); return 0; }
    uint8_t *fn = (uint8_t *)api->function_start(x);
    if (!fn || x - fn != AS_STRING_XREF_OFF) {
        snprintf(a->err, sizeof a->err, "string xref at fn+%#llx, expected +%#x",
                 fn ? (unsigned long long)(x - fn) : 0ull, AS_STRING_XREF_OFF);
        return 0;
    }
    if (memcmp(fn, INIT_PROLOGUE, sizeof INIT_PROLOGUE)) { snprintf(a->err, sizeof a->err, "master init prologue mismatch"); return 0; }

    uint8_t *g = (uint8_t *)api->pattern_scan(AS_GATE_PAT);
    if (!g) { snprintf(a->err, sizeof a->err, "streaming gate pattern not found"); return 0; }
    if ((uint8_t *)api->function_start(g) != fn) { snprintf(a->err, sizeof a->err, "streaming gate is not inside master init"); return 0; }

    int32_t d; memcpy(&d, g + AS_GATE_CALL + 1, 4);
    uint8_t *st = g + AS_GATE_CALL + 5 + d;
    if ((uintptr_t)st < base || (uintptr_t)st + 0x100 > end) { snprintf(a->err, sizeof a->err, "Start target outside image"); return 0; }
    if ((uint8_t *)api->function_start(st) != st) { snprintf(a->err, sizeof a->err, "Start target is not a function start"); return 0; }
    if (memcmp(st, START_HEAD, sizeof START_HEAD)) { snprintf(a->err, sizeof a->err, "Start head mismatch (enabled flag offset)"); return 0; }
    if (memcmp(st + AS_START_BUDGET_OFF, START_BUDGET, sizeof START_BUDGET)) { snprintf(a->err, sizeof a->err, "Start budget store mismatch"); return 0; }

    a->master_init = fn; a->gate = g; a->streamer_start = st;
    return 1;
}
