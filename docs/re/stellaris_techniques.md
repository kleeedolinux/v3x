# Stellaris optimization techniques, mapped to Victoria 3 (research, 2026-11-12)

Scope: (A) what the code of `Yidhar/stellaris-perf` actually does, (B) what the Stellaris team says it did in dev
diaries and patch notes, (C) what carries over to Victoria 3 1.15.1 and how V3X could do it, with a ranked top 10.

How the claims are marked:
- **[V-code]**: read in the cloned source (`%TEMP%\v3x_research\<repo>`, `git clone --depth 1`, 2026-11-12).
- **[V-exe]**: checked by static scripts on `binaries\victoria3.exe` (1.15.1).
  - PE TimeDateStamp is `0x6AF3094F`.
  - The scripts are in `...\v3x_research\scripts\` (`rtti.py`, `rtti2.py`, `strgrep.py`, `ctx.py`, `col.py`).
- **[V-news]**: quoted from the Steam News API, full text, `feeds=steam_community_announcements`.
  - Stellaris: appid 281990, 920 items. Victoria 3: appid 529340, 380 items. Raw JSON is in `...\v3x_research\news\`.
  - Links have the form `https://store.steampowered.com/news/app/<appid>/view/<gid>`.
- **[Inferred]**: my reasoning, not checked against code or binary.

Nothing was built, run or injected. The game was not launched.

---

## 0. Headline findings

1. **stellaris-perf has static addresses but no Windows symbols.** [V-code]
   - Every address is a static RVA in `sdk/stellaris_sdk.hpp`, gated by an exact PE timestamp (`sdk::kExeTimestamp = 0x6AF23393`).
   - The RVAs come from a dumper in `Yidhar/stellaris-mcp/tools/sdk_dumper`.
   - The dumper is driven by a **symbolized Linux decompile** (`source/stellaris_4.5_source.cpp`, `linux_index.py`).
   - Victoria 3 has no Linux build, so V3X cannot copy this workflow. **[Inferred]**
   - V3X has to name functions from RTTI, assert/source-path strings and profiler-scope strings. That route is well supplied (point 2).
2. **victoria3.exe is rich in anchors.** [V-exe]
   - 47,022 RTTI type descriptors (`.?AV/.?AU`), 35,314 complete object locators, and 35,314 vtables resolved statically.
   - 986 `C:\mnt\gsg\caligula\...\*.cpp|h` source-path strings.
   - The tick is fully named: **265 distinct `CTickTask_*` classes**, each with its own vtable. The per-task body is in slot 6 (188 distinct targets among 255 vtables); 68 tasks also have a slot 7.
   - Lambda RTTI leaks method names, e.g. `CTickTask_UpdateEmployment::CollectEmployment / SortEmployment / Prepare`.
3. **The Jomini modifier-node system Stellaris uses also exists in Vic3, in its newer form.** [V-exe]
   - `NJominiModifierNode::CModifierNodeManager<NJominiModifier::CObjectModifier, EModifierNodeCategory>`, `CGameModifierNodeManager`.
   - `SSimultaneousUpdate`, `UpdateNodesByCategory` (a parallel-for over node arrays with `SParallelForOverArrayRandomLogCallTraits`).
   - Strings: `modifier_node_manager.h`, `"Missing modifier node dependency"`, `"Modifier update already in progress"`, `"Invalidated {} modifier nodes"`, `RecalculateModifierNodes`, `GameStateTick.ForceFullModifierNodeRecalc`.
   - Stellaris 4.2/4.3 got its big gains by **back-porting "Modifier System" and "Thread Pool System" from newer Clausewitz** ([V-news] DD #406).
   - Vic3 already had dependency-batched parallel node updates (DD #76) and a full modifier rewrite in 1.7 (DD #120/#122).
   - **Much of Stellaris' official 2025-26 speed-up is code Vic3 already has.** [Inferred, but strongly supported]
4. **The overall stellaris-perf gain is modest.** [V-code] (`bench/RESULTS.md`)
   - All options on vs all off: **-6.2 % wall time per day on a late-game save** (95 % CI -9.8..-2.5), and **-23.1 % in a 16k-ship battle**.
   - The largest single wins were frame/render capping (-9.9 % between ticks, ~-4.4 % inside ticks) and the save writer (autosave phase 677 → 375 ms).
   - Ideas with "no gain" are documented honestly: LPT scheduling, nested parallel-for inline, outliner throttle.
5. **Vic3's biggest official levers are data-oriented: the number of pops and the number of modifier-node invalidations.** [V-news] (DD #76, #105, #120, #159)
   - These are the same levers as Stellaris' 4.0 pop groups and its cascade work.

---

## Part A: Yidhar repositories, code-level

### A.1 Build gating and safety (applies to all hooks) [V-code]

- **Exact build pin.** The launcher manifest `plugin/stl-plugin.json` has `"game": {"exe_timestamps": ["0x6AF23393"]}`.
  - `perf_tweaks.cpp: ExeMatchesSdk()` compares `IMAGE_NT_HEADERS.FileHeader.TimeDateStamp` with `sdk::kExeTimestamp`.
  - On a mismatch it **installs nothing**, then idles until the unload event.
- **Per-feature fail-closed.** The core hooks (`HandleTurnTick`, rule/opinion caches, flags, `AddInvalid`, the manager `Update`, scope hooks) must all succeed, or `Install` returns false.
  - Feature modules (`tooltip::Install`, `nodes::Install`, `render::Install`, ...) each verify their own preconditions and disable only themselves.
  - Call-site patches are re-verified at runtime: `nodes::Install` checks `p[0]==0xE8 && site+5+rel32 == InvalidateChildren` for each of its three gated sites.
  - `colony::Sites(...)` does the same for every call site it keys on.
- **Return-address keyed detours.** Many optimizations hook a widely used callee (`CModifierNodeBase::Update`, `CFleet::CalcMilitaryPower`, `NFleetUtil::CanAffordUpgrade`).
  - They change behaviour only when `_ReturnAddress()` equals a specific SDK call site (`sdk::site::*` + 5).
  - Other callers, such as the AI and the simulation, pass through untouched.
- **Context gating by thread-local depth.** For example, `t_in_fm` is set inside `CFleetManagerView::Update`, and `phase::GuiDepth` marks "inside a per-frame GUI update".
  - A tick suspends the GUI context, so simulation threads never see the cached path.
- **Patch only while workers are idle.** Byte patches (`combat_no_copy`, `fleet_parallel_grain1`) and late hooks (learned delegate hooks, per-AI-class vtable hooks) are applied **between ticks on the main thread**.
- **SEH-guarded raw reads** (`__try/__except`) everywhere. A bad pointer means bypass and a counter bump.
- **Self-unload.** A named event `Local\stellaris_perf_unload_<pid>` triggers `MH_DisableHook(MH_ALL_HOOKS)`.
  - The DLL then drains in-flight detours (waits for `g_in_tick==0` plus two more ticks, plus 500 ms) and calls `FreeLibraryAndExitThread`.
  - DllMain only creates the event and a worker thread.
- **Hot-reloaded INI.** The file stamp is polled every 2 s. Every optimization has `0` = engine path, plus usually a **verify mode** (`2`/`3`/`4`).
  - Verify mode runs the engine's computation as well and counts mismatches.
- **MP stance.** "every player runs it with the same settings". Default-on features must either return the engine's answer or be order- and thread-independent.
  - Per-day caches (`opinion_cache`, `rule_cache`) and the serial modifier flush stay **off**, "because their results can depend on which worker thread filled a cache first" (README, Multiplayer).
- **Benchmark discipline.**
  - ABBA/BAAB segments in one session, with a log-linear model per group and segment (`bench/scripts/regress_ab.py`).
  - `phase_profile` uses rdtsc at phase entry points, writes one JSON record per game day, and separates normal days, month starts and autosave days.
  - `phase_extra=0xRVA` adds timing to any function at runtime.
  - There is a stress-mod generator (`bench/stress_mod/gen_stress_mod.py`).

### A.2 How functions are located (stellaris-mcp `tools/sdk_dumper/functions.py`) [V-code]

There is one structural rule per function, and **the rule must match exactly one function or the dump fails**. Rule kinds:
- **`strings`**: the function referencing all given C strings.
  - `CGameState::HandleTurnTick` ← `"It is a new day: "`.
  - `CGameState::DailyUpdate` ← `"DailyUpdate"`.
  - `CGameState::MonthlyUpdate` ← `"CGameState::MonthlyUpdate generated %i random numbers, last one is %i"`.
  - `CScriptedRule::Evaluate` ← `"CGameRules::"` + `".Evaluate"`, plus `require` regexes on the instructions.
- **`pattern`**: an ordered regex list over the disassembly, with immediate or byte prefilters.
  - Examples: `CModifierNodeManager::AddInvalid` by `xor eax, 0x811c9dc5` (FNV); `CPdxParallelForDescriptor` ctor by `cmovne rbx, rax; cdq; idiv ecx`; `CMemoryFile::WriteData` by `cmovl esi, eax`.
  - The CRT `sprintf`/`snprintf` thunks are matched by `or rcx, 1` / `or rcx, 2` (the UCRT legacy option bits).
- **Relational rules**: `caller_of`, `callee_of`, `call_in` (n-th call inside a located function), `common_callee`, `tail_target`.
  - Also `vtable_sibling` (a slot ±N from a located virtual), `call_seq`, and `site` (the n-th call to X inside Y, or a virtual call whose surrounding instructions match a regex).
- **Layouts are read from the code that uses them** (`anchors.py`, `perf_layouts`), e.g. node dirty byte `+0x108`, `CModifier` entries `+0x38/+0x44`, `CWriter` depth `+8`.
  - The plugin therefore carries no hand-written offsets.
- **`.pdata`-aware.** Function bodies include chained `.pdata` fragments, because MSVC splits functions.

**RTTI tooling:**
- stellaris-mcp has ad-hoc runtime scripts: `scripts/decode_rtti.py`, `resolve_rtti.py`, `pe_decode_rtti.py`, `rtti_vt.py`.
  - These follow vtable-8 → `RTTICompleteObjectLocator` → `TypeDescriptor` name via `ReadProcessMemory`.
- There is no generic static RTTI dumper. I wrote a static one for Vic3 (`scripts/col.py`, see C.0); it is the more useful idea for V3X.

### A.3 Each optimization (stellaris-perf 0.6.0 for Stellaris 4.5.4) [V-code unless noted]

Each entry gives the hooked function, how it is located, what the detour does, determinism, the measured result (`README.md` / `bench/RESULTS.md`), and the Vic3 analogue.

1. **`modifier_cascade_gate` (default 2)** (`perf_nodes.cpp`)
   - **Hooks:**
     - `CModifierNodeBase::InvalidateChildren(node, mask)`, located by `call_in` from `CModifierNodeBase::Clear`, call index 1.
     - The three call sites that cascade the country node and the relay-network node (`site::CCountry_InvalidateModifier_InvalidateChildren`, ...).
     - `CModifierNodeManager::AddInvalid`.
     - Each node owner's `std::function` rebuild delegate `_Do_call`. These are learned at runtime from `node+0x48` (impl vtable slot 2) and MinHook-ed at the next day change, through 48 template stubs.
   - **Detour:**
     - When a full-mask invalidation at a gated site arrives, it records `pre_hash = NodeHash(node)` and suppresses the children cascade.
     - After the engine registers the node (`AddInvalid`), it rebuilds the node at once (`CModifierNodeBase::Update`).
     - It then calls the real `InvalidateChildren(node, ~0)` **only if the content hash changed**.
     - The hash is order-independent: a sum of mixed hashes over (value, type) entries and over parent records (multiplier, `CPdxModifierName::CalcHash` virtual, category).
   - **Determinism:** deterministic; each client makes the same decision from the same content.
     - Known divergence from the engine: a child whose inputs changed without its own invalidation keeps a stale value until the next real cascade (about 7 nodes per day).
   - **Measured:** about 11 held invalidations a day, 93 % unchanged.
     - Daily flush: 10,100 → 3,500 rebuilds, 15 → 7.5 ms. `DailyUpdate` 75.2 → 67.3 ms. Month start 294 → 284 ms.
   - **Vic3:** the same pattern applies. Anchors are in C.0 (`CModifierNodeManager<CObjectModifier>`, `modifier_node_manager.h` asserts, `RecalculateModifierNodes`).
2. **`colony_monthly_batch` (2)** (`perf_colony.cpp`)
   - **Hooks:**
     - `CColony::MonthlyUpdateSerial` (`caller_of` `NColonyUpdate_MonthlyOrDailyUpdatePopJobsInColony`, ≥150 instructions).
     - `CModifierNodeBase::Update` and `CColony::MicroUpdateCachedValues` (`call_seq`), keyed on five return-address sites (`OnAddPops`, `OnRemovePops`, `AddSize`/`MoveSize` to a new pop group).
   - **Detour:** inside a colony's own pass:
     - The forced refreshes are skipped and replayed once at the end.
     - The manager flush is held.
     - In mode 2 the colony's node is also frozen (readers see the pre-pass node), then rebuilt and invalidated at the end.
   - **Determinism:** deterministic. Known semantic difference: amenity/stability tier comes from before the pass, and `on_pop_growth` sees pre-pass cached values.
   - **Measured:** month start 370/397 ms → 345/349 ms.
   - **Vic3:** the analogue would be the serial apply steps of `CTickTask_UpdateEmployment`, `UpdatePopGrowth` and migration, which change pops one at a time and invalidate nodes each time. **[Inferred]**
3. **`pop_jobs_single_pass` (1)** (`perf_jobs.cpp`)
   - **Hooks:** `NPlanetJobs::UpdatePopJobs`. The second of the two identical calls in `NColonyUpdate::MonthlyUpdate` (`site::*_UpdatePopJobs_1`) is skipped.
   - **Determinism:** deterministic, but gameplay-visible: one fewer fixed-point iteration per month.
   - **Measured:** month start 346 → 310 ms.
   - **Vic3:** look for duplicated calls inside tick tasks with the same technique, a `site` rule plus a return-address check.
4. **`pop_faction_batch` (2)**
   - **Hooks:** `CPopFaction::UpdateSupportAndApproval` (pattern `movups [rbp+0x40], xmm0`). Calls inside a window (each pop-job pass, or the whole monthly colony update) are recorded and run once per faction at the end.
   - **Verify:** modes 3/4 recompute and compare (0 mismatches in 1,400 checks).
   - **Measured:** first pop-job pass 69 → 51 ms.
   - **Vic3:** `CTickTask_UpdatePopSupportForPoliticalMovements`, `UpdateInterestGroupPoliticalStrength`, `UpdatePopsInterestGroupsAndMovements`. Whether they recompute per pop move is unknown. **[Inferred]**
5. **`pop_jobs_node_batch` (2)**
   - **Detour:** skips two forced `CModifierNodeBase::Update` refreshes per newly created pop group (sites inside the inlined `AccessOrAddPopGroupMoveSizeFrom`) and leaves the nodes dirty for the next reader.
   - **Measured:** pass 49.8 → 41.4 ms; the monthly total was within noise.
6. **`ai_weight_cache` (1)** (`perf_weight.cpp`)
   - **Hooks:** `CMeanTimeToHappen::GetRawFactor(mtth, out, scope)`.
   - **Detour:** a per-thread 4-way set-associative table, keyed by a 64-bit hash of (weight ptr, scope this/from/root/prev identity). It is valid only inside one country's update in the parallel AI phase; that phase is read-only because the engine forbids RNG and global writes.
   - Scopes with parameters or saved event targets bypass the cache.
   - **Measured:** 90.6 % hits, but only 15.5 → 15.4 ms per phase. Small.
7. **`ai_parallel_lpt`, `ai_nested_inline` (off, experimental)**
   - **Hooks:** `UpdateAIParallel` (`callee_of ProcessAI` that calls `CRandomLog_Get`) and the `CPdxParallelForDescriptor` ctor (sets the task count to 1 inside a country update).
   - **Measured:** no gain. The phase length equals the heaviest country.
8. **`rule_cache`, `opinion_cache` (off; MP-unsafe)**
   - **Hooks:** `CScriptedRule::Evaluate` and `CCountry::CalcOurOpinionOfOther`.
   - **Detour:** a per-day generation-tagged cache with hash-only entries (16 B, 4 per cache line). The table is sized to ¼ of the L2, read with `GetLogicalProcessorInformationEx`.
   - **Adaptive mode** learns per-rule hit/miss cost against eval cost and enables the cache below 85 %, disables it above 95 %.
9. **`modifier_flush` (off, experimental)**
   - **Detour:** rebuilds a small known dirty set serially, in category order, instead of the ~100-task parallel graph job.
   - It sets `CRandom_Forbidden=1` and the `CRandomLog` config bit around the rebuild to mimic the engine flush.
10. **`flag_simd` (1), `flag_expiry_skip` (1)**
    - **Hooks:** `CHasFlagTrigger::ActualEvaluate` (an SSE2 8-wide scan of the u16 id array, verify mode 2) and `CPdxIntegerFlags::UpdateFlags` (skips containers without timers).
    - The engine answer is unchanged.
11. **`fleet_parallel_grain1` (1)**
    - A 5-byte patch of the chunk-size clamp in `UpdateShipParallel` (`glob::UpdateShipParallel_GrainClamp`), giving one fleet per chunk.
    - This is safe because the phase has RNG disabled and fleets are independent.
12. **`combat_no_copy` (1)**
    - An 11-byte patch, found by a byte pattern that must match exactly once.
    - It passes the combat-participants table entry by reference instead of deep-copying 7 arrays per ship. It is applied and removed only between ticks.
13. **`alloc_cache` (1)** (`perf_alloc.cpp`)
    - **Hooks:** `operator new` and `free`, found by shape: `push rbx; sub rsp,20h; mov rbx,rcx` → thunks → malloc, which loads the CRT heap with `mov rcx,[rip+disp]`.
    - **Detour:** caches up to 8 freed blocks ≥128 KB per thread, sized in 64 KB steps, up to 8 MB per thread. A 64 KB open-addressing table identifies "ours". The heap's reported size is checked before reuse.
    - **Measured (arena):** -7..-8 % wall time, -16..-27 % CPU.
    - **Vic3:** mimalloc is statically linked ([V-exe] `mimalloc:` strings; DD #76). mimalloc already keeps thread-local segment caches, so the expected gain is much smaller. **[Inferred]**
14. **`save_fast_format` (1)**
    - **Hooks:** the engine's `sprintf`/`snprintf` thunks.
    - **Detour:** handles `"%d" "%u" "%s" "%lld" "%s%lld.%0.5d" "%0*lld" "%lld.%s"` with integer code.
    - **Self-test:** at load, every fast format is compared byte-for-byte with the CRT on a value battery. Any mismatch disables it.
    - **Measured:** autosave 617 → 572 ms.
    - **Vic3:** these printf formats are **absent** from victoria3.exe [V-exe]; it uses `{}` fmt-style strings. The patch does not apply directly.
15. **`save_fast_writer` (1)** (`perf_writer.cpp`)
    - **Hooks:** five functions: `CMemoryFile::WriteData`, `CWriter::WriteIndent`, `WriteTokenText`, `WriteString`, `WriteCString`.
    - **Detour:** in-place blob append, tokens straight from the static lexer table, indentation as one tab run, and quoted strings without an escape scan.
    - Binary writers and non-memory files keep the engine path.
    - **Byte-identical check:** 100.6 MB gamestate compared with `save_compare.py`.
    - **Measured:** autosave 677 → 375 ms.
    - **Vic3:** `.?AVCWriter@@` and `.?AVCMemoryFile@@` have vtables (C.0). But Vic3 already writes databases in parallel (`WriteArrayThreaded`, `pdx_threaded_array_writer.h`, `SWriterTask<TPdxRefDatabase<X>>` RTTI) and supports binary saves (`savegame_check_binary_*.txt`). [V-exe] The gain is likely smaller. **[Inferred]**
16. **`tick_render_fps` (10), `render_cap_between_ticks` (1)** (`perf_render.cpp`)
    - **Hooks:** `RenderFrame`, found from its 35 inlined `RenderFrameIfNeeded` call sites by a byte pattern (`cmp byte [reg+5D0h],0; je; cmp g_bFrameSmoothing; ...; call RenderFrame`). All 35 sites must target the same function.
    - **Detour:** drops calls closer than 1/fps (rdtsc) while inside a tick, or while the sim ran in the last 250 ms.
    - **Measured:** -9.9 % wall time for the between-tick cap. In-tick 30→10 fps is about -4.6 points of main-thread samples.
    - **Rejected variant:** dropping the draw step made the game *slower* (the GUI update re-ran more often).
17. **`tooltip_cache` (1, 250 ms)** (`perf_tooltip.cpp`)
    - **Hooks:** the two **virtual call instructions** inside `CTooltipManager::Update` (`site` rules with `indirect` regex). MinHook is placed *on the call instruction*, jumping to a thunk `push [next]; jmp fn`.
    - The thunk is refused when user shadow stacks (CET) are enabled for the process.
    - **Detour:** keeps the provider's last `CToolTip` (via the engine's own `CString::Assign`) and its return value. The entry is reused while there was no key or button event (none within the last 100 ms), the mouse has not moved, it is the same game day, the settings are the same, and age ≤ 250 ms.
    - Verify mode 2 exists, and `tests/tooltip_test.cpp` runs against a stand-in engine.
18. **GUI caches**: `fleet_manager_cache`, `ui_power_cache`, `fleet_upgrade_cache`, `cursor_path_cache`, `fleet_manager_reinforce_ms`.
    - Same pattern: a display-only memo keyed by object id plus array pointer and count, ≤ 1 s and never across a game day, active only in GUI context or at a specific return address.
    - **Measured:** fleet window entry 0.95 → 0.34 ms.
19. **Diagnostics:** `phase_profile`, `phase_extra`, `phase_callers`, `node_trace` (invalidations by call site and cascade root, with `RtlCaptureStackBackTrace`), `rule_profile`, `scope_profile`. Shared-memory stats are in `perf_shared.hpp`.

**Other repositories:**
- `stellaris-launcher`: plugin spec v2.
  - It injects with `LoadLibraryW` in a remote thread after the window appears plus `delay_ms`, and refuses builds outside `exe_timestamps`.
  - Updates check SHA-256, and the index mirror pins SHA-256 (`docs/PLUGINS.md`).
- `stellaris-plugins`: only the plugin index (JSONL plus schema check).
- `stellaris-mcp`: the bridge, the SDK dumper (A.2), and `docs/REVERSE_ENGINEERING_CHEATSHEET.md` §16 on the modifier-node layout.
  - That section covers 35 categories, a per-thread invalid-id set merged by `CollectInvalid`, and `CPdxGraphJob` in category order. Note that a full-mask `InvalidateChildren` *unlinks* children, which re-register when they rebuild.

---

## Part B: the Stellaris team's techniques (official) [V-news]

Links have the form `https://store.steampowered.com/news/app/281990/view/<gid>`.

| # | Technique | What they did / numbers | Source (gid) |
|---|---|---|---|
| B1 | **Thread pops and jobs**, consolidate per-country results after the planet pass | 2.2.3–2.2.7 "huge effort to thread jobs and pops". "will no longer calculate the country modifiers at the end of the planet daily update serial, but have a pass to calculate all countries that were modified after all planet updates" | 2.2.3 `2426778849020066254`, 2.2.4 `2461683368384776042`, Tech improvements `2408775863519025908` |
| B2 | **Modifier nodes: lazy, pull-based dependency recalculation** | 2.3 Wolfe: "each node register what node they follow, and is recalculated when used". 10–30 % faster than 2.2.7, fewer "big freezes" | `2408775863519025908` |
| B3 | **Skip pointless work** (early-outs) | Ships skip regen at full HP; off-screen icons not updated; uninhabitable planets skip the populated-planet evaluations; pops evaluate fewer jobs | `2408775863519025908` |
| B4 | **Event-driven instead of daily** recompute; aggregate-level triggers | 2.6: job distribution "only done on demand" (was daily); species-level and fleet-level triggers instead of scanning every pop or ship. 15–30 % late game | DD #170 `2578811485591668531` |
| B5 | **Determinism discipline for threading**: per-thread accumulation then merge; record now, apply in a later phase | DD #181 explains the sequencing and order-dependency OOS issues and the "store per-thread outputs, consolidate at the end" pattern. Threaded asset loading | DD #181 `3788045105523752466` |
| B6 | **Daily caching of hot script rules**; precomputed "possible" checks | `can_vote_in_democratic_election` was evaluated per pop × per faction per day; now once per day per pop (or per species). `possible_precalc` per job class cut those checks by about ⅔. `planet_resource_compare` became selective (about 75 % cost). Trigger reordering; removed always-false events | DD #232 `4971405952701095591` |
| B7 | **Share computation across identical entities** (memo by equivalence class) | Job weights computed once per (species, job) per planet instead of per pop ("40 miners → 1 calc"). 75 % fewer weight calcs from removing redundant calls. Monthly update with a hysteresis of ±1 pop per job per update | DD #239 `4237326104319463204` |
| B8 | **More ticks per render frame** on fast machines | 3.3: "changing some settings in the engine which allows more powerful computers to do extra 'ticks' per render frame"; up to 50 % less time per year early game | 3.3 Libra `4246335832503873416` |
| B9 | **Parallelised batch modifier updates**; parallelize many daily subsystems; cache colonies-in-system, starbase types and wars | 3.5: "large reduction of lag spike potential"; auto-migration monthly cost halved | 3.5 Fornax `6258337460543548588` |
| B10 | **Data-oriented aggregation: pop groups** (species × strata × ethics × faction), workforce instead of individual pops | 4.0 Phoenix. "Instead of iterating through and calculating production for every individual Pop, the game now only checks once per planet". Effects act on groups or percentages. Removed per-pop modifier cascades. A further 4.3 experiment mixed ethics within a group to cut group counts more | DD #366 `1789039014333023`, #370 `1791580425962120`, #372 `1792751526006161`, #411 `1825727806707319` |
| B11 | **Design-level load shedding**: fewer ships (stronger hull tiers), trade simplified to a plain resource | Ship counts about -25 % in the experiment; economic/ship changes about +7 % performance | DD #397 `1811772772472983`, #406 `1818118366172757` |
| B12 | **Back-port the newer Clausewitz modifier system and thread pool** | 4.2: about 17 % faster overnights than 4.1.7; reaches 1 s/day about 100 years later | DD #406 `1818118366172757` |
| B13 | **Dependency tracking in the economy update** to cut CPU wait; **smaller threaded batches** for many, uneven objects | 4.3: "reduce the cpu wait time ... 'monthly stutter'". Batch sizes decreased "for better distribution over multiple cores". Fixed a bug that had disabled economy threading | DD #410 `1823825466496336`, #412 `1826992588590347` |
| B14 | **Memory reuse in the modifier update**; allocation reduction everywhere | "reusing memory and reduce time spent on allocations/deallocations"; string allocation 76 % faster; alert manager and topbar allocations | DD #412, #406 |
| B15 | **Move trigger work out of modifier calculation into pulses** (micro, daily, weekly, monthly, yearly); move modifier *name* building to tooltip time | "Moved modifier name calculations to tooltip generation where this was done in the game simulation"; "Moving trigger dependent calculations out to suitable pulses" | DD #411, #412 |
| B16 | **Small-buffer storage for event scope parameters** | 95 % of EventScope uses without heap allocation; `AddOrUpdateScopeParameter` 71×, `CreateEventScope` 48×, `AddScopeParameter` 83×; Ship `CalculateModifier` -68 %; frame time about -10 % | DD #412 |
| B17 | **Size the thread pool to physical cores**; `Threading.TaskThreadCount`; `-threads=XX` | "Reduced the amount of worker tasks ... in line of the amount of physical cores instead of logical" | DD #412 |
| B18 | **Render loop as if V-Sync off, without tearing** | 4.3 | DD #412 |
| B19 | **Stagger and defer bursts** | Tech completion no longer rebuilds all ship designs at once (deferred to daily, AI staggered); idle automated fleets retry on a cooldown, staggered; daily stutter from science-ship automation fixed | DD #429 `1841579228665167`, DD #428 `1836506165583034` |
| B20 | **Cache, and don't recompute per tooltip or per country-modifier update** | Faction attraction, eligible pops and monthly pop changes cached instead of recomputed per tooltip. Ethics pop changes cached per empire and colony, monthly only. Galactic Community resolutions cached instead of re-collected at every country modifier update. Bypass update ~150 µs → ~5 µs | DD #431 `1842846814442931`, #433 `1844115010491664`, #412 |
| B21 | **Telemetry and process**: `one_year`/`thirty_years`, `-gamestatetimer` TSV, `-incrementalsaves`, overnight runs, script profiler, a perf subteam with tick-time targets (375 → 350 → 300 ms per day) | 3.14 30.5 s/yr vs 4.3 11.5 s/yr at start | DD #397, #399 `1813674388934932`, #410 |

---

## Part C: mapping to Victoria 3

### C.0 Vic3 anchors verified for this report [V-exe]

- **Tick:**
  - `C:\mnt\gsg\caligula\caligula\source\logic\gamestatetick.cpp`.
  - Console variables `GameStateTick.RenderFrameIfNeeded`, `GameStateTick.MinLockTime`, `GameStateTick.ForceFullModifierNodeRecalc`, `GameStateTick.TicksPerValidation`, `GameStateTick.UIValidation`.
  - Profiler scopes `RenderFrameIfNeeded`, `RecalculateModifierNodes`, `RemoveDeadObjects`, `UpdateTimedModifiers`.
  - `CScopedGameStateRelease::CScopedGameStateRelease` (the session-thread → UI handoff named in DD #76).
- **Tick tasks:** 265 `CTickTask_*` classes. Examples: `CTickTask_UpdateEmployment`, `CTickTask_UpdateStatePopNeedCache`, `CTickTask_RecalculateModifierNodes`, `CTickTask_MergeSmallPops`, `CTickTask_UpdatePopGrowth`, `CTickTask_UpdateAI*`.
  - Base-class slots 1/3/5 are shared across all 255 resolved vtables; slot 6 is the task body.
  - Built-in timing: `dump_ticktask_timings`, `clear_ticktask_timings`, `ticktask_timings.csv`, `log_ticktask_performance`.
- **Modifiers:**
  - `.?AV?$CModifierNodeManager@VCObjectModifier@NJominiModifier@@W4EModifierNodeCategory@@@NJominiModifierNode@@`, `.?AVCGameModifierNodeManager@@` (vtables at RVA `0x47617e8`, `0x4769898`).
  - `SSimultaneousUpdate`, `UpdateNodesByCategory`, `modifier_node_manager.h` asserts, `"Modifier update already in progress"`, `"Missing modifier node dependency"`.
  - Debug UI `ModifierNode.Graph` / `ModifierNode.List`, `modifier-node-statistics`.
- **RNG guard:** `"Calling random inside forbidden area:"`, `"Random is blocked by thread {}"` (the analogue of Stellaris' `CRandom_Forbidden`).
- **Threads:** `Threading.TaskThreadCount`, `Threading.EnableTaskThreads`, `pdx_task_scheduler.cpp`, `CPdxGraphJob::ScheduleTask`.
- **GUI/tooltip:**
  - `.?AVCJominiTooltipManager@@` (vtable `0x49fd558`), `CPdxGuiTooltipHandler`, `SJominiTooltipInfo`, `jomini\modules\tooltip_manager\source\tooltip_manager.cpp`, `pdx_gui_tooltip.cpp`.
  - Frame cap settings: `Graphics.CapFramerate`, `setting_framerate_cap`.
- **Save:** `.?AVCWriter@@` (vtable `0x4b06018`), `.?AVCMemoryFile@@` (`0x4b06e28`), `WriteArrayThreaded`.
- **Pops:** `POP_MERGE_START_POPS`, `POP_MERGE_MAX_TARGET_POPS`, `POP_MERGE_MAX_WORKFORCE_{MINOR,MODERATE,AGGRESSIVE,MAX}_CONSOLIDATION`, `POP_MERGE_MIN_NUM_POPS_SAME_PROFESSION_*`.

### C.1 Does Vic3 already do it? [V-news; links `https://store.steampowered.com/news/app/529340/view/<gid>`]

| Stellaris technique | Vic3 status | Source |
|---|---|---|
| B1/B5 threading with deterministic merge | **Yes.** Parallel pre-step plus serial apply plus a sorting step for MP order in `UpdateEmployment`; task threads; session/main/render threads | DD #76 `5059268543357158387` |
| B2 modifier nodes, dependency batches | **Yes.** "similar to Stellaris ... modifier nodes"; batches that depend only on previous batches, parallelised in early 2022; full rewrite in 1.7 "avoiding unnecessary work in modifier node recalculations" | DD #76; DD #120 `5746109972548989913`; DD #122 `5759622039976317029` |
| B10 data-oriented aggregation (fewer objects) | **Yes, by merging pops.** "relevant factor ... is not the size of the population, but rather the number of Pops". Merge defines, Pop Consolidation game rule (1.3.5), migration without splitting (1.6), about -25 % late-game pops (1.10), pop memory footprint reduced for cache use (1.6) | DD #76; DD #90 `5124581515254858730`; DD #105 `8033928544492755458`; DD #106 `5688680204370415083`; DD #159 `1811138915393324` |
| Cost-aware scheduling (vs Stellaris' failed LPT experiment) | **Yes.** Heuristic cost per item; expensive countries scheduled separately via standard deviation | DD #76 |
| B12/B17 modern thread pool, thread-count console | **Yes** (`Threading.TaskThreadCount` [V-exe]). Physical-vs-logical sizing is unknown; there was a 1.15 many-core crash fix (prior research) | DD #76; [V-exe] |
| B14 allocation work | **Partly.** mimalloc since 1.2 (+4 %); 1.13.9 "memory usage -7-10 %, total allocations -15 %" | DD #76; 1.13.9 `1836506165548520` |
| B6/B20 caching of hot script/system values | **Ongoing, piecemeal.** 1.13.5: cached modifier names, AI war willingness, diplo-play variables. 1.15: caching of sway scoring, laws and legitimacy, building modifier re-evaluation | 1.13.5 `1832065502823987`; DD #195 `1847920302235107` |
| B21 perf tooling | **Yes.** Script profiler (1.5), Performance Dashboard with heat-map and slowest events (2024) | DD #95 `5141476355657728365`; DD #142 `1789039014337403` |
| UI/graphics blocking the sim | **Known issue.** "session thread ... blocked from modifying the game state because the interface or graphics updates need to read from it"; 1.2 graphics-update work; outliner flattening (1.6) | DD #76; DD #106 |
| B19 staggering | Not discussed for Vic3. Tick tasks have fixed frequencies (tick/daily/weekly/monthly/yearly) | DD #76 |
| B16 event scope small-buffer | Unknown; engine-internal | none |

### C.2 What V3X can add via hooks: feasibility and determinism

Classification:
- **Display-only**: no simulation effect; MP-safe even if only one player runs it.
- **Deterministic-sim**: changes the simulation in an order-independent way. MP-safe only if every client runs the same build and settings, which mirrors the stellaris-perf policy.
- **Gameplay**: changes outcomes; needs explicit opt-in and identical settings for MP.

1. **Session-thread / UI contention.** Display-only.
   - Stellaris' biggest measured win is not rendering while the sim runs (-9.9 %, -4.4 %). In Vic3 the UI runs on another thread, but the session thread **stops** at `CScopedGameStateRelease` points so the UI can read state (DD #76).
   - The lever is how often the tick yields and renders: `GameStateTick.RenderFrameIfNeeded` / `GameStateTick.MinLockTime` and the `RenderFrameIfNeeded` scope.
   - **Step 1, no hook:** A/B the console variables with `dump_ticktask_timings`.
   - **Step 2, hook:** find the function referencing `"RenderFrameIfNeeded"` in `gamestatetick.cpp` (string anchor plus `.pdata`) and rate-limit it with rdtsc while unpaused (the stellaris-perf `render` module pattern).
   - Risk: low; the game feels less responsive at speed 5.
2. **Thread pool sizing and affinity.** Display-only (thread count does not change results by design, or Paradox's MP would break).
   - Set `Threading.TaskThreadCount` to the physical P-core count and keep workers off E-cores (SetThreadAffinityMask or CPU-set APIs on task threads, found by thread description or start address).
   - Low effort, hardware-dependent gain. Stellaris B17 is the precedent.
3. **Modifier cascade gate.** Deterministic-sim.
   - Hook the Jomini node invalidate/children cascade. Hash node content before and after rebuild; propagate only on change.
   - DD #120 names "number of modifier nodes and invalidation events" the #1 cost.
   - **Unknown:** whether the 1.7 rewrite already compares content. RE `CModifierNodeManager<CObjectModifier>` first; anchors are in C.0. The `node_trace`-style counting comes first, and it is the go/no-go test.
   - Effort is high: layouts differ from Stellaris, so read them from `UpdateNodesByCategory` and its callees.
4. **Pop count, data-oriented.** Gameplay, low-risk.
   - Ship a V3X preset of the `POP_MERGE_*` defines or the Pop Consolidation rule.
   - No hook is needed. It changes the checksum; same as Stellaris B10 in spirit.
5. **Redundant forced refreshes and duplicated passes inside serial tick tasks.** Deterministic-sim.
   - Profile per tick task by hooking slot 6 of all `CTickTask_*` vtables; RTTI gives names and patch-robustness. Then `node_trace`-style counts inside the top tasks (`UpdateEmployment` serial part, `UpdatePopGrowth`, migration).
   - Batch only where verify mode shows identical results (stellaris-perf `colony_monthly_batch` / `pop_faction_batch`).
6. **GUI/tooltip memo.** Display-only.
   - Vic3 tooltips run through Jomini data bindings (`CJominiTooltipManager`, `CPdxGuiTooltipHandler`, data functions like `GetTooltip*`). There is no single provider call like Stellaris has. **[Inferred]**
   - Better targets are expensive data functions called per frame: hook the function behind a data-function name string, memoize per (object, frame-day) for ≤250 ms, and invalidate on input.
   - Medium-high effort.
7. **Read-only-phase memo** for AI and parallel pre-steps. Deterministic only if each thread has its own table and the phase really is read-only.
   - Use the engine's own RNG-forbidden region (`"Calling random inside forbidden area:"`) as the proof that the phase is read-only.
   - Key: (trigger/value object, scope identity). Stellaris measured a tiny gain; Vic3's AI is heavier (many 1.13–1.15 AI perf notes), but this is unproven.
8. **mimalloc tuning.** Display-only.
   - `MIMALLOC_*` environment variables are read at process init, so this needs launcher-side environment, not DLL injection.
   - Options: large/huge OS pages and arena reserve. Prior research noted a 5 % claim for Stellaris large pages, which needs SeLockMemoryPrivilege.
9. **Autosave writer.** Display-only if byte-identical.
   - First check whether Vic3 autosaves are text or binary, and whether `CWriter` is on the hot path given `WriteArrayThreaded`. Port the stellaris-perf approach (in-place append, token table, byte-compare harness) only if a profile shows the main-thread stall is large.
10. **Staggering and pulses (B15/B19).** Gameplay.
    - Re-timing tick-task frequency changes results. Only as opt-in.
11. **Low value for Vic3:**
    - The 128 KB recycler (mimalloc already in place).
    - The sprintf fast path (formats absent).
    - LPT scheduling (Vic3 already cost-schedules; Stellaris measured no gain).
    - Flag SIMD (Vic3 variable lookups are untested; low priority).

### C.3 Top 10 for Vic3, ranked by expected gain / effort

Gains are **[Inferred]** estimates for late game unless measured. Step 0 is a prerequisite, not ranked.

**0. Measurement harness (do first):**
- Tick-task timing via the built-in `dump_ticktask_timings` / `ticktask_timings.csv`, plus a V3X detour on `CTickTask_*` vtable slot 6 for all 255 tasks (RTTI-located).
- Per-day JSON records like `phase_profile`, and ABBA A/B like `regress_ab.py`.

| Rank | Optimization | Class | Expected gain | Effort | Main risk |
|---|---|---|---|---|---|
| 1 | Throttle in-tick render/UI handoff while running (`GameStateTick.RenderFrameIfNeeded` / `MinLockTime`, then hook) | Display-only | 5–15 % tick throughput (Stellaris: -9.9 % and -4.4 %) | Low (console A/B first) | Laggier UI at speed 5 |
| 2 | Thread count = physical P-cores, workers off E-cores | Display-only | 0–10 %, hardware-dependent (Stellaris DD #412) | Low | Too few threads on small CPUs |
| 3 | Pop-merge preset (`POP_MERGE_*`, consolidation rule) | Gameplay | 10–25 % late game (pops are the top cost; 1.10 cut pops 25 %) | Very low (defines) | Changes simulation and checksum |
| 4 | Modifier cascade gate (propagate only on content change) | Deterministic-sim | 5–10 % if Vic3 lacks it; 0 if 1.7 already has it | High | Stale children; layout RE |
| 5 | Batch forced refreshes and duplicated passes in the top serial tick tasks (after step 0) | Deterministic-sim | 3–10 % on weekly/monthly spikes | High | Semantic drift (verify modes needed) |
| 6 | Map/graphics update throttling while running (province, map names, city graphics; DD #76 says these blocked logic) | Display-only | 2–8 % | Medium | Visual staleness |
| 7 | GUI data-function / tooltip memo (≤250 ms, input-invalidated) | Display-only | UI fps plus less session blocking, 1–5 % | Medium-high | Stale UI text |
| 8 | Read-only-phase memo of triggers/script values (AI and parallel pre-steps), per-thread tables | Deterministic-sim | 0–5 % (Stellaris: ~0.6 %) | Medium | Hidden state reads |
| 9 | mimalloc large pages / options via launcher environment | Display-only | 0–5 % | Low-medium | Privilege, VRAM-driver interplay |
| 10 | Autosave writer fast path (only if profile shows a big main-thread stall) | Display-only | Autosave spike -20..-45 % (Stellaris), little on average | Medium-high | Byte-identity harness required |

**Techniques to borrow regardless of feature:**
- The exactly-one-match rule; failing closed per feature.
- Runtime re-verification of every patched call site (`E8 rel32 → expected target`).
- Return-address keyed detours.
- Patching only between ticks.
- Verify modes that run the engine path too and count mismatches.
- Self-unload with in-flight draining.
- Per-thread tables sized from L2.

These are all [V-code] in stellaris-perf and match V3X's string-anchor plus `.pdata` design.
