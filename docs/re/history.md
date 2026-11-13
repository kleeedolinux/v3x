# Country history graphs ("trends") and why they reset on a player switch

Victoria 3 1.15.1 (`binaries/victoria3.exe`, no symbols). RVAs are image-relative.
**V** = verified by reading the disassembly / running the offline check. **I** = inferred.

## How the graphs are stored

- GUI graphs read trends: `Country.GetGDPTrend` etc. (`game/gui/politics_panel_types.gui:1381`). (V)
- Trends live in `logic/trend_manager.cpp`; the engine asserts they are presentation-only:
  "Illegal access to trends from synchronized thread. Trends must be used only for the visual
  presentation" (string 046d3c30, used in 00c2ec00). (V) So trends are not part of the synced
  simulation / checksum. (I, from that assert)
- A country holds 7 trends at `country+0x2320 .. +0x2470` (stride 0x38). (V, 00916600)
  Each owned state holds trends at `state+0x108/+0x140/+0x178/+0x1b0/+0x1e8`. (V, 01465ef0)
- `Trend::Resample(trend, rate, capacity)` = **00cb76c0** (anchor string "Trying to fill samples in
  trend but no space was reserved!" 046d7128, xref 00cb7b25). Returns early if the rate is unchanged;
  otherwise it rebuckets the existing samples. (V for the shape, I for exact semantics)
- `Trend::Clear(trend)` = **00cb7650**: frees every channel buffer and zeroes the counts. (V)
- `SetCountryTrendRate(country, days)` = **00916600**: Resample on the 7 country trends (capacity
  0x23a50 days) then on every owned state's trends (via 0085f310). (V)

## Who gets sampled

- `UpdateCountryTrends` (00cb6519) collects countries for which `ShouldUpdateTrends(country)` =
  **00916500** is true. It looks up `CountryManager+0xe0[id & 0xffffff]` (control state):
  1 or 2 -> every day; otherwise only on the week of the month equal to `id & 3` (00cbd260). (V)
- Sample rate: 28 days for control state 1/2, 120 days for the rest (post-load pass at 01468310..
  01468355: `cmovne edx, 0x1c`, else 0x78). (V) State 1/2 = player-controlled (I: they are set from
  `ingameidler.cpp`, the player-assignment code).

## The reset (cause)

- `CountryManager::SetControlState(mgr, id, state)` = **01465900** writes `mgr+0xe0[id] = state`
  (0146598d) and dispatches: state 0 -> **01465ef0** (call at 014659e9); 1 -> 014658b0 + 01465a00;
  2 -> 01465a00. (V)
- Callers of 01465900: `OnCreateCountry` (008e71e0) and two sites in the in-game idler
  (00f8b9db, 00f8ba59; function 00f8b320 refs `ingameidler.cpp`). (V) The idler is where a player is
  attached to / detached from a country. (I, from the source file name and the dispatch)
- **01465ef0 ("demote to AI")** calls `Trend::Clear` on 6 of the 7 country trends (01466022,
  0146602e, 0146603a, 01466046, 01466052, 0146605e; `+0x2390` is skipped), on 5 trends of every owned
  state (0146612a, 01466136, 01466142, 0146614e, 0146615a), then `SetCountryTrendRate(country, 120)`
  (0146617c). (V) **This is the reset the lead sees**: switching away from a country sets its control
  state to 0, which wipes its graphs. (V for code path; the "player switch -> state 0" link is I, needs
  an in-game test.)

## Fix (plugins/keep_history)

NOP (5-byte `0F 1F 44 00 00`) the 11 `call Trend::Clear` sites in 01465ef0. Everything else in that
routine still runs, including the switch to the 120-day rate, which resamples the kept data instead
of starting from an empty trend.

Anchors (no hardcoded addresses; any mismatch -> nothing patched, load returns 1):
1. Resample string -> xref -> `function_start` = Trend::Resample.
2. Unique `.text` block: 6x `lea rcx,[r14+d32]; call C`, `lea rcx,[r14+d32]; call`, `lea rcx,[r14+d32];
   mov edx,28; mov r8d,...`. All 6 targets equal; C is a function start and not Resample.
3. Within 0x200 bytes after it, up to `mov edx,120; mov rcx,r14; call S`: exactly 5 calls to C; S must
   call Trend::Resample within its first 0x40 bytes.

`tools/re_hist_check.py` runs the same logic on the exe file (PASS on 1.15.1, prints the 11 sites);
`test/keep_history_test.c` runs the real plugin DLL against the exe mapped with LoadLibraryEx.

### Safety / compatibility

- Multiplayer: trends are presentation-only (assert above), so no desync expected. (I)
- Saves: trends are saved (keys `channels`, `sample_rate` exist, 04966930/04966940; I that they
  belong to trends). Kept data is just more samples; a vanilla game loads it. (I)
- `OnCreateCountry` also reaches 01465ef0 with state 0; for a newly created country the trends are
  empty, so skipping Clear is a no-op. (I) If the engine ever reused a country object with stale
  trends, that data would now survive.
- Memory: AI countries you used to play keep their history (bounded by the trend capacity). (I)

## Needs an in-game test

Play country A for a year, switch with `play B` (or the tag switch), open A's GDP / population graph:
it should still show A's history. Check `v3x.log` for `keep_history: patched 11 Trend::Clear calls`.

## record_all (never-played countries get the player's sampling)

Lead feedback (in game): the fix works for played countries, but a country you switch to for the
first time shows little or no GDP history. Cause (V for code, I for the visual effect): AI countries
use a 120-day rate (demote routine 0146617c, post-load pass 01468347) and their country trends are
pushed only ~1 week per month: `ShouldUpdateTrends` 00916500 falls back to 00cbd260
(`id & 3 == day_of_month / 7`). Resample 00cb76c0 going 120 -> 28 on promotion only repeats the
coarse samples (`rep stosq` at 00cb7b91), so the graph is a few flat steps.

`record_all=1` (default; `v3x/keep_history.ini`, section `[keep_history]`):
- hook `SetCountryTrendRate` 00916600: `days = min(days, rate_days)` (default 28, clamped 7..120).
  It also resamples every owned state's trends (via 0085f310), so state trends follow. (V)
  Reached for every country by the post-load pass 01466a20 and on demotion/creation. (V for the call
  sites; I that 01466a20 runs at new game as well as at load.)
- hook `ShouldUpdateTrends` 00916500: returns true when the return address is 00cb654b (its call in
  `UpdateCountryTrends`), so every country is sampled daily like a player. The other caller (pop
  trends, 00cb68d6) keeps vanilla, to save CPU. Anchor: the only `call; test al,al; je` in the 0x300
  bytes before the "UpdateCountryTrends" xref (00cb6652), target must contain `cmp edx,1; je; cmp edx,2`.
- If these anchors fail, record_all is skipped and logged; the clear fix still applies.

Memory estimate (I: assumes 8 bytes per sample and 1 channel per trend; channel count not measured):
capacity is 0x23a50 days, so at 28 days a channel holds at most 5214 samples (41 KB). In a 100-year
game that is 1304 samples = 10.4 KB per channel. With ~250 countries x 7 trends and ~900 states x 5
trends that comes to ~65 MB at 1936, against ~15 MB at the vanilla AI rate: about **+50 MB per channel
per trend**. `rate_days=56` cuts the extra to ~20 MB. The game's own capacity is the cap; no extra cap
was added. Not measured: real channel counts, CPU cost of daily sampling for all countries, save size.
