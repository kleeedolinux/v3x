/* keep_history: the GDP / population / SoL / ... graphs of a country you stop playing are no
 * longer wiped when you switch to another country.
 *
 * Cause (see docs/re/history.md): when a country's control state goes to "AI"
 * (CountryManager::SetControlState, called from ingameidler.cpp on a player switch), the game
 * runs a demote routine that calls Trend::Clear on 6 country trends and on 5 trends of every
 * state the country owns, then sets the trend sample rate from 28 to 120 days. The fix NOPs
 * those 11 Trend::Clear calls; the sample-rate change stays (it resamples, keeping the data).
 * Trends are presentation-only in the engine ("Trends must be used only for the visual
 * presentation"), so this cannot affect the simulation or multiplayer checksums.
 *
 * Anchors, all verified offline by tools/re_hist_check.py (same steps, run it after a patch):
 *   1. "Trying to fill samples in trend but no space was reserved!" -> xref -> Trend::Resample
 *   2. unique byte block in .text: 6x `lea rcx,[r14+d32]; call Clear`, `lea rcx,[r14+d32]; call`,
 *      `lea rcx,[r14+d32]; mov edx,28; mov r8d,..` (the country clears in the demote routine)
 *   3. after it, before `mov edx,120; mov rcx,r14; call SetCountryTrendRate`: exactly 5 calls to
 *      Clear (state trends), and SetCountryTrendRate must call Trend::Resample in its first 0x40 bytes.
 * Any mismatch: nothing is patched and v3x_plugin_load returns non-zero.
 *
 * record_all (v3x/keep_history.ini, [keep_history], default 1): countries you never played also
 * get a usable history. Vanilla records AI countries at a 120-day sample rate and pushes their
 * country trends only one week per month (ShouldUpdateTrends); players get 28 days, daily.
 *   - hook SetCountryTrendRate: rate = min(rate, rate_days) (default 28; also covers the country's
 *     state trends, which that function resamples too). Called for every country after load / new
 *     game, on demotion to AI and on creation (via demotion).
 *   - hook ShouldUpdateTrends (only for its call from UpdateCountryTrends, recognised by return
 *     address; the pop-trend caller keeps vanilla): return true, so every country samples daily.
 *     Anchor: the one `call P; test al,al; je` within 0x300 bytes before the "UpdateCountryTrends"
 *     xref, and P must contain `cmp edx,1; je; cmp edx,2` in its first 0x80 bytes.
 *   rate_days=56 roughly halves the extra memory (docs/re/history.md has the estimate).
 *   If record_all anchors fail, the clear-NOP fix still applies (logged). */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "v3x.h"

V3X_EXPORT const char *v3x_plugin_name = "keep_history";

#define TAG "keep_history"
#define N_COUNTRY 6
#define N_STATE 5
#define STATE_WINDOW 0x200

static const V3X_API *api;
static int cfg_rate = 28;

typedef void (*setrate_fn)(void *country, int days);
typedef _Bool (*should_fn)(void *country);
static setrate_fn orig_setrate;
static should_fn orig_should;
static const uint8_t *should_ret;   /* return address of the UpdateCountryTrends call */

static void hk_setrate(void *country, int days)
{
    if (days > cfg_rate) days = cfg_rate;
    orig_setrate(country, days);
}

static _Bool hk_should(void *country)
{
    if (__builtin_return_address(0) == (void *)should_ret) return 1;
    return orig_should(country);
}

static const uint8_t *text_lo, *text_hi;

static int find_text(void)
{
    uint8_t *base = (uint8_t *)api->image_base();
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++)
        if (!memcmp(s->Name, ".text", 5)) {
            text_lo = base + s->VirtualAddress;
            text_hi = text_lo + s->Misc.VirtualSize;
            return 0;
        }
    return -1;
}

static const uint8_t *rel_call(const uint8_t *p)
{
    if (p < text_lo || p + 5 > text_hi || p[0] != 0xE8) return NULL;
    int32_t d; memcpy(&d, p + 1, 4);
    return p + 5 + d;
}

/* 49 8D 8E ?? ?? 00 00 : lea rcx, [r14 + disp32] (disp < 64 KB) */
static int lea_rcx_r14(const uint8_t *p)
{
    return p[0] == 0x49 && p[1] == 0x8D && p[2] == 0x8E && p[5] == 0 && p[6] == 0;
}

static int is_clear_block(const uint8_t *p)
{
    for (int k = 0; k < N_COUNTRY + 1; k++, p += 12)
        if (!lea_rcx_r14(p) || p[7] != 0xE8) return 0;
    static const uint8_t tail[] = {0xBA, 0x1C, 0x00, 0x00, 0x00, 0x41, 0xB8};
    return lea_rcx_r14(p) && !memcmp(p + 7, tail, sizeof tail);
}

#define BLOCK_LEN (12 * (N_COUNTRY + 1) + 7 + 7)

typedef struct {
    const uint8_t *resample, *clear, *demote, *setrate;
    const uint8_t *sites[N_COUNTRY + N_STATE];
} Found;

#define FAIL(...) do { api->log(TAG, "disabled: " __VA_ARGS__); return -1; } while (0)

static int locate(Found *f)
{
    memset(f, 0, sizeof *f);
    if (find_text()) FAIL("no .text section");

    const void *s = api->find_string("Trying to fill samples in trend but no space was reserved!");
    if (!s) FAIL("Trend::Resample string not found");
    const void *x = api->find_xref(s, 0);
    if (!x) FAIL("Trend::Resample string has no xref");
    f->resample = api->function_start(x);
    if (!f->resample) FAIL("Trend::Resample has no .pdata entry");

    const uint8_t *blk = NULL; int hits = 0;
    for (const uint8_t *p = text_lo; p + BLOCK_LEN <= text_hi; p++) {
        if (p[0] != 0x49 || !is_clear_block(p)) continue;
        if (!hits++) blk = p;
    }
    if (hits != 1) FAIL("clear block found %d times (want 1)", hits);

    for (int k = 0; k < N_COUNTRY; k++) {
        const uint8_t *t = rel_call(blk + 12 * k + 7);
        if (!k) f->clear = t;
        if (!t || t != f->clear) FAIL("country clear call %d has another target", k);
        f->sites[k] = blk + 12 * k + 7;
    }
    if (f->clear == f->resample || api->function_start(f->clear) != f->clear)
        FAIL("Trend::Clear %p is not a distinct function start", (void *)f->clear);
    f->demote = api->function_start(blk);
    if (!f->demote) FAIL("demote routine has no .pdata entry");

    static const uint8_t end_mark[] = {0xBA, 0x78, 0x00, 0x00, 0x00, 0x49, 0x8B, 0xCE, 0xE8};
    const uint8_t *from = blk + 12 * (N_COUNTRY + 1), *end = NULL;
    for (const uint8_t *p = from; p < from + STATE_WINDOW && p + sizeof end_mark <= text_hi; p++)
        if (!memcmp(p, end_mark, sizeof end_mark)) { end = p; break; }
    if (!end) FAIL("end marker (mov edx,120; call SetCountryTrendRate) not found");

    int n = 0;
    for (const uint8_t *p = from; p < end; p++)
        if (p[0] == 0xE8 && rel_call(p) == f->clear) {
            if (n >= N_STATE) FAIL("more than %d state clear calls", N_STATE);
            f->sites[N_COUNTRY + n++] = p;
        }
    if (n != N_STATE) FAIL("%d state clear calls (want %d)", n, N_STATE);

    f->setrate = rel_call(end + sizeof end_mark - 1);
    int ok = 0;
    for (const uint8_t *p = f->setrate; p && p < f->setrate + 0x40; p++)
        if (p[0] == 0xE8 && rel_call(p) == f->resample) { ok = 1; break; }
    if (!ok) FAIL("SetCountryTrendRate %p does not call Trend::Resample", (void *)f->setrate);
    return 0;
}

/* ShouldUpdateTrends and its call site in UpdateCountryTrends */
static int locate_should(const uint8_t **fn, const uint8_t **site)
{
    const void *s = api->find_string("UpdateCountryTrends");
    const uint8_t *x = s ? api->find_xref(s, 0) : NULL;
    if (!x) FAIL("record_all: UpdateCountryTrends string/xref not found");
    static const uint8_t after[] = {0x84, 0xC0, 0x0F, 0x84};
    int n = 0;
    for (const uint8_t *p = x - 0x300; p < x; p++)
        if (p >= text_lo && p[0] == 0xE8 && !memcmp(p + 5, after, sizeof after) && rel_call(p)) {
            if (!n++) { *site = p; *fn = rel_call(p); }
        }
    if (n != 1) FAIL("record_all: %d `call; test al,al; je` before UpdateCountryTrends (want 1)", n);
    static const uint8_t shape[] = {0x83, 0xFA, 0x01, 0x74};
    static const uint8_t shape2[] = {0x83, 0xFA, 0x02};
    for (const uint8_t *p = *fn; p < *fn + 0x80; p++)
        if (!memcmp(p, shape, sizeof shape) && !memcmp(p + 5, shape2, sizeof shape2)) return 0;
    FAIL("record_all: ShouldUpdateTrends %p has an unexpected shape", (void *)*fn);
}

static int record_all(const Found *f)
{
    char ini[MAX_PATH];
    snprintf(ini, sizeof ini, "%skeep_history.ini", api->v3x_dir());
    if (!GetPrivateProfileIntA("keep_history", "record_all", 1, ini)) {
        api->log(TAG, "record_all=0: AI countries keep vanilla sampling");
        return 0;
    }
    cfg_rate = (int)GetPrivateProfileIntA("keep_history", "rate_days", 28, ini);
    if (cfg_rate < 7) cfg_rate = 7;
    if (cfg_rate > 120) cfg_rate = 120;
    const uint8_t *should, *site;
    if (locate_should(&should, &site)) return -1;
    should_ret = site + 5;
    uintptr_t b = api->image_base();
    int r = api->hook((void *)f->setrate, (void *)hk_setrate, (void **)&orig_setrate);
    if (r) { api->log(TAG, "record_all: hook SetCountryTrendRate failed (%d)", r); return -1; }
    r = api->hook((void *)should, (void *)hk_should, (void **)&orig_should);
    if (r) {
        api->log(TAG, "record_all: hook ShouldUpdateTrends failed (%d)", r);
        api->unhook((void *)f->setrate);
        return -1;
    }
    api->log(TAG, "record_all: every country records at <= %d days, daily (ShouldUpdateTrends +%llx, site +%llx)",
             cfg_rate, (unsigned long long)((uintptr_t)should - b), (unsigned long long)((uintptr_t)site - b));
    return 0;
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    Found f;
    if (locate(&f)) return 1;
    uintptr_t b = api->image_base();
    api->log(TAG, "Trend::Resample +%llx, Trend::Clear +%llx, demote +%llx, SetCountryTrendRate +%llx",
             (unsigned long long)((uintptr_t)f.resample - b), (unsigned long long)((uintptr_t)f.clear - b),
             (unsigned long long)((uintptr_t)f.demote - b), (unsigned long long)((uintptr_t)f.setrate - b));

    static const uint8_t nop5[5] = {0x0F, 0x1F, 0x44, 0x00, 0x00};
    for (int i = 0; i < N_COUNTRY + N_STATE; i++) {
        int r = api->patch((void *)f.sites[i], nop5, sizeof nop5);
        if (r) {
            api->log(TAG, "patch %d at +%llx failed (%d); game may be partly patched", i,
                     (unsigned long long)((uintptr_t)f.sites[i] - b), r);
            return 2;
        }
    }
    api->log(TAG, "patched %d Trend::Clear calls: played countries keep their graphs after a switch",
             N_COUNTRY + N_STATE);
    if (record_all(&f)) api->log(TAG, "record_all not active; the keep-on-switch fix is still applied");
    return 0;
}
