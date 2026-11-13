# Data structures: where Clausewitz really spends lookup time (1.15.1)

Question from the lead: "fix the enums, indexes, lists etc. - everything heavy and unoptimized in Clausewitz".
This page ranks the hottest small/generic engine functions from real sampling profiles, says what
each one is, and lists which ones can get a side cache that returns bit-identical answers.

**Short answer.** Enum<->string scans, string-keyed database lookups and `dynamic_cast` are NOT where
the time goes. The engine already uses FNV-1a open-addressing hash maps and sorted-vector binary
searches, and each of those costs <1% of samples. The one large data-structure hot spot is the
**persistent token registry's global spin lock**. Every token-id->string lookup made while writing a
save takes that lock, and many threads do this at once. It costs 8.0% of all exe samples in the
4-hour session. `plugins/fast_lookup` removes it. The other big rows are job waits, GUI widget-tree
walks (cache-miss bound) and allocator/memcpy. None of these is a lookup that a cache could answer.

## Data and method

- Profile: `D:/v3x_tmp/sessions/20261113-042101/profile_Game_to_Empty_2.csv` + `profile_edges_Game_to_Empty_2.csv`.
  This is devtools sampling of all threads over a 4 h game: 10,030,624 exe self samples. The edge
  table dropped 1.97 M inserts (table full), so the caller columns are lower bounds.
  devtools gives samples without unwind info as **64-byte blocks** (`rva & 63 == 0`, not a function
  start). Those rows are leaf code and are named here by disassembly.
- Load profile (`crash3-*/profile_Empty_to_Game_2.csv`): the top entries are the loading-screen
  QPC polling loop (0x3dbfd00 / 0x67df20, 12%+), job waits, memcpy and the lexer. No lookup rows.
- Tools: `tools/re_ds_rank.py` (top N by self samples, heuristic class, top caller) and
  `tools/re_ds_lib.py` (`dis` / `up` / `down`, `refs()` = every RIP-relative reference into a
  range). Names come from string hints (`tools/symbolize.py`) plus manual disassembly.

## Ranked table (Game_to_Empty_2, % of exe self samples)

| # | rva | what it is (verified by disassembly) | self % | structure / pattern | fix | expected gain |
|---|---|---|---|---|---|---|
| 1 | 0x235be20 | job wait: spin with `pause` backoff, then yield, until job state [+0x60] is 2/3. Top caller: ingameinterfaceidlergfx.cpp:456 (main thread waits for GUI jobs) | 7.91 | spin wait | none (idle time, not work) | 0 |
| 2 | 0x3985b00 block | GUI widget vfuncs `inc/dec dword [rcx+0x44c]` (0x3985b10 / 0x3985b20), called per widget by 0x6bf3f0 from pdxinput_context (0x38a9940) | 6.16 | linked widget list walk, cache-miss bound | none safe (GUI state) | 0 |
| 3 | 0x3e1e000 block | **TokenGetString** 0x3e1e010 entry: `lock xadd` on the token registry spin lock (exe+0x5d13bd0); caller 0x3e24400 = text writer WriteToken | 3.97 | global RW spin lock + array index | **fast_lookup token_cache** | ~all of it |
| 4 | 0x3dfa270 | task system help-while-waiting (threading.md) | 3.44 | spin / steal | none | 0 |
| 5 | 0x3915710 | pdx_gui animation runtime state update | 3.40 | per-widget logic | none | 0 |
| 6 | 0x38afc50 | pdx_gui_data_manager widget-tree update (recursive over children, FetchData) | 2.47 | tree walk | none (mutable) | 0 |
| 7 | 0x3e1e040 block | TokenGetString spin/read/unlock (rest of #3) | 2.36 | same lock | **token_cache** | ~all of it |
| 8 | 0x38a9bc0 | GUI widget update (flags at +0xd8) | 1.58 | logic | none | 0 |
| 9 | 0x38aedf0 | pdx_gui_data_manager FetchData | 1.53 | data binding eval | none | 0 |
| 10 | 0x38568e0 | GUI widget layout/state getter | 1.53 | logic | none | 0 |
| 11 | 0x794aa0 | function-local static getter (thread-safe-static TLS epoch check, always returns exe+0x540ae50); callers 0x794640 / 0x7c1990 (tagged-pointer string reset in pdx_datasystem) | 1.52 | `_Init_thread_header` guard | could return the constant after init | <=1% (not done: hook overhead eats most of it) |
| 12 | 0x3e24100 | **text writer WriteIndent**: takes the same registry spin lock inline to read token 17 ("\t"), then writes 64-char chunks | 1.46 | global spin lock | **fast_lookup indent** | ~all of it |
| 13 | 0x43d9540 | mimalloc | 1.04 | allocator | already mimalloc | - |
| 14 | 0x2ed27e0 | map object manager logic | 1.00 | logic | none | 0 |
| 15 | 0x4436640 | memcpy (CRT) | 0.94 | - | - | - |
| 16 | 0x84c930 | hash map find, 16-bit key, FNV-1a, open addressing, returns default exe+0x468b8a0 | 0.89 | hash map (already O(1)) | none needed | ~0 |
| 17 | 0x392eb50 | vector remove-all-equal (swap-with-last), GUI animation list | 0.79 | linear scan | none (mutable, small n) | 0 |
| 18 | 0x3d46df0 + 0x3d47c80 | pdx_data_callstack (GUI script data evaluation) | 1.21 | interpreter | none | 0 |
| 19 | 0x9a24d0 (+0x803db0, 0x9b64c0) | lower_bound over a sorted vector of pointers by u32/u64 key at +0x10 (budget: governmentexpenses, export_amount) | 0.97 | sorted-vector binary search (already O(log n)) | none needed | ~0 |
| 20 | 0x391b580 (3 blocks) | per-widget state index lookup under a second global spin lock (exe+0x5c3eff0); main-thread GUI only | 1.11 | spin lock + 0x70-byte rows | not cacheable (state changes); lock is uncontended | 0 |
| 21 | 0x3568e80 | event target link evaluation | 0.57 | scope chain | none | 0 |
| 22 | 0x27e5bd0 / 0x27e5dd0 | GameStateAccess-checked accessors (dynamic terrain masks; parallel tasks) | 1.04 | checks | none | 0 |
| 23 | 0x35959b0, 0x3596c80, 0x3596d80, 0x36099c0 + memcmp 0x4436540 | trigger scope checks: virtual "supported scopes" 128-bit mask + `memcmp(mask, 0, 16)`, gamestate-read permission check | 1.97 | bitset tests | inline memcmp (pure); small | ~0.3% |
| 24 | 0x7cc400 | goods DB transaction scope (TLS index, SRW exclusive on first use) | 0.45 | TLS + SRW | none | 0 |
| 25 | 0x43f0c64 (+0x43f06c8, 0x4411540) | `__RTDynamicCast` (incl 0.37%), called mostly from GUI 0x39859c0 | 0.33 self | RTTI string compare | per-(vtable,type) cache possible | <=0.3% (not done) |
| - | 0x3e49fe0, 0x3e1df90 | token string->id (FNV-1a, case-insensitive) | <0.01 | hash map | none needed | 0 |

Rows 3 + 7 + 12 + 0x3e24400 (0.24) = **8.03%** are one lock: the token registry's.
`python tools/re_ds_rank.py --top 100` prints the full heuristic top 100 (its class column is
automatic; the table above was checked by hand).

## The token registry (pdx_core persistence "dynamic tokens")

- Object at exe+0x5d13bd0, created by the getter 0x3e1df20. Pointer in the global exe+0x5c40a38.
  - +0: spin RW lock (`lock xadd +2` read, `lock bts bit0` write).
  - +8: string->id hash (FNV-1a over `c|0x20`).
  - +0x30: dynamic token list (size +0x3c).
  - +0x48: `std::string*` slots (CPdxArray: data +0x48, capacity +0x50, size +0x54, allocator +0x58).
  - +0x60: static token count.
- **TokenGetString 0x3e1e010** (leaf, no .pdata; 13 callers):
  - takes the read lock;
  - `id < 0 || id > size` -> default string exe+0x542c980. Note `id == size` reads one slot past the end;
  - slot NULL -> default;
  - else the slot pointer;
  - releases the lock.
- **Registrar 0x3e1d6f0** ("Creation of dynamic token ..."): the only writer (2487 static-init call
  sites plus runtime dynamic tokens).
  - Under the write lock: new id = dynamic count + static count + 1.
  - The array grows zero-filled: size is set to 0 during the copy, then to id + 1.
  - Then `slots[id] = new heap std::string`.
  - I checked all 2099 functions that reference the global and every `lock bts` in them. The only
    other writes are the static init 0x3e1cf00 and the atexit teardown 0x4544a40. 0x1db83e0 and
    0x3920c80 write-lock other objects.
- So the slots are append-only and never rewritten or freed before exit. A non-default answer for an
  id that was `< size` before the call is permanent. That is the cache invariant. The cache also
  re-checks `id < size` on every hit. Size sits on a different cache line from the lock and is
  written only by registrations, so the re-check costs no contention.
- Hot use: the save writer runs on many task threads (0x3dba2d0 parallel for) and makes one lookup
  per token through WriteToken 0x3e24400. In text mode 0x3e24400 calls 0x3e1e010. In binary mode it
  inlines the lock just to read +0x60. That binary path is not covered yet (0.24% here, because this
  game saved in text mode). WriteIndent 0x3e24100 inlines the lock to read token 17.

## plugins/fast_lookup

Fail-closed anchors and safety:
- Two whole-function byte patterns (TokenGetString, WriteIndent), each unique in .text.
- Cross-check: both read the same registry global and the same default string, and the indent loop
  calls stream vtable slot 0x38.
- Raw PASS8 detours.
- Disables itself on `process_exit`, because static destructors free the registry.
- Nothing runs in DLL_PROCESS_DETACH.

Modes:
- `measure` (default): the engine answers every lookup. The cache is filled and compared with each
  answer, and lookups are timed against cache probes with rdtsc on 1 in 64 calls. Any mismatch turns
  the cache off and logs the first case.
- `on`: hits are returned without the lock; `verify_every=N` re-checks some hits against the engine.
- `indent`: emulated only when token 17 has length 1. Otherwise the engine runs: with a longer token,
  the engine copies at stride 1 and leaves stack bytes in its buffer, which cannot be reproduced.

Reports: console `fastlookup`, a log line after every save (`save_end`), and at exit.

Tests (`test/fast_lookup_test.c`, 36 checks, ALL OK):
- A fake registry: measure and on modes give identical answers (40k lookups with interleaved
  registrations, including ids < 0, == size and > size). Nothing is cached at or past the size. A rewrite
  is detected and disables the cache. Size 0 during growth falls back to the engine. 8 reader threads run
  against 1 registering writer with 0 wrong answers. The indent emulation matches a C model.
- The real engine code, run from the exe mapped with DONT_RESOLVE_DLL_REFERENCES against a fake
  registry in the engine layout:
  - TokenGetString matches the cache (1221 lookups).
  - WriteIndent matches the emulation for indents -1..200 (same calls, bytes and 64-char chunks).
  - The plugin loaded with MinHook on the mapped exe in mode=on gives the same answers and bytes as the
    unhooked engine.
- Bench, real engine code vs hooked cache. The figure is wall time divided by the total number of
  calls, in ns, so lower means more throughput:
  - 1 thread: 19.2 (engine) vs 17.9 (cache).
  - 8 threads: 50-51 (engine) vs 14.4-14.7 (cache). With 8 threads the engine's lock is 2.6x slower
    than with 1 thread; the cache gets faster instead.

Build and run: see the header of `test/fast_lookup_test.c`. The plugin builds through `build.sh`
like the others: `plugins/fast_lookup/fast_lookup.c` plus the header `fl_core.h`.

**Not verified in game.** The plugin was not installed or run. The expected effect is that most of the
8% of samples spent on the token lock during saves goes away, which shortens autosave stalls. The
change in seconds per save is not measured: run `mode=measure`, then `mode=on`, and compare the
`save_end` lines with `enhance`'s save timing.

## Not worth doing (measured)

- Enum<->string and string-keyed DB lookups: none in the top 100. Token string->id is <0.01%.
- `dynamic_cast` 0.37% inclusive. Hash maps (0x84c930) and sorted-vector searches (0x9a24d0 family)
  are already O(1) / O(log n).
- GUI rows (2, 5, 6, 8-10, 20) are widget-tree walks over mutable state, bound by cache misses. Making
  them faster means doing less GUI work (fewer or hidden widgets), not caching lookups.
- Next candidates if wanted:
  - 0x3e24400 binary-mode path: the static count is immutable after init, so the inline lock could be
    skipped. The same emulation approach as indent applies.
  - 0x794aa0: return the constant after init.
