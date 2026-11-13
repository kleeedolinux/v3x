# Victoria 3 simulation tick: reverse-engineering notes

Binary: `binaries/victoria3.exe`, 100221048 bytes, sha1 prefix `cde54311854ee0b0`, ImageBase 0x140000000.
Every address below is an **RVA** (add the module base at runtime). It is valid for this build only; re-anchor using the strings given.
Tools: `tools/re_sim_lib.py` (`str|rva|dis|fn|calls|callers|strs`, index cached in `tools/re_sim_index.pkl`),
`tools/re_sim_ticktasks.py` (task registry extractor), `tools/re_sim_vtslots.py` (vtable slot dump), `tools/re_sim_anyref.py`.
Scratch data: `tools/re_sim_tmp/` (tt.txt = raw registry, vtslots.txt, range.txt, table.md).

## Call chain (static, from .pdata + direct calls)

| RVA | role | anchor |
|---|---|---|
| 00ca8690 | caller of the game-state tick (no direct callers; reached via vtable/fn pointer) | asserts `NGameState::GameStateAccess._Value == EGameStateAccess::Logic ...` (0475f830) |
| 01465690 | **GameState tick** (one call per game tick) | string `Processing Tick: ` (0475efc0); single call site 00ca86e2 |
| 014736c0 | **Tick-task runner**: runs all tasks due this tick, with per-task timing | strings `Tick Tasks for {}` (0475fed8), `  Task {:>32s}: {:>5}    {}:{:.3f}min ` (0475fef0), `Total: {}, Highest: {}` (0475ff18); called once from 01465833 |
| 0146d010 | **Run one task** (Prepare, then Execute, then range/context workers) | strings `Prepare` (046b2c78), `gamestatetick.cpp:1253` (0475f3e0), `Context %d` (0475f558), `gamestatetick.cpp:1287` (0475f570); callers 01473c78 (in the runner) and 01472e20 |
| 0146d0ee | **virtual dispatch of task Execute**: `call qword ptr [rax+0x30]` (vtable slot 6), rcx = task object | inside 0146d010 |
| 01468a60 | scope guard for render/lock time ("RenderFrameIfNeeded"), constructed around long tasks; 13 callers | `gamestatetick.cpp:262` (0475fb30) |
| 01468d10 | `RenderFrameIfNeeded` helper, 29 callers | string 0475fbe8 |
| 01472e20 | game-state access helper, 61 callers, also calls 0146d010 | same assert string as above |

## Task objects

272 `CTickTask_*` classes are registered (strings `CTickTask_<Name>`). Rates: TICK 43, DAY 83, WEEK 120, MONTH 20, QUARTER 2, YEAR 4.
Priorities: DEFAULT, or LATE (runs after the DEFAULT tasks).

Vtable layout (all tasks share slots 1-5: 03e25080 006781c0 03e250f0 03582620 006781c0):
- **7-slot vtables** (all 76 range-parallel tasks plus others): slot 6 is Execute, dispatched at 0146d0ee. For range tasks it only sets up the stage (see Stage protocol); the per-item work is in the range functor.
- **8-slot vtables** (69 tasks): slot 6 is a shared wrapper 0084fb00 that builds a std::function and calls it; slot 7 is the task body.
- So "work fn" = the last code slot. Hooking it catches one task; hooking 0146d0ee catches all of them.
- 4 tasks (AutoWithdrawFromTreaties, CheckCompanyChartersPossible, EffectuateTreatyObligations, StartTreatyBindingPeriods): the vtable was not resolved by the extractor.

## Stage protocol and Step layout (RunTask 0146d010)

A task runs as a chain of **stages**. Before the call at 0146d0ee, RunTask zero-fills a `Step` on its
stack (rsp+0x50; impl pointers zeroed by `4C 89 65 98 4C 89 65 D8`). It then calls the task's
**Execute** (vtable slot 6) exactly once, with rcx = task and rdx = Step.
The correction to "Task objects" above: for range tasks, slot 6 is the stage setup ("Prepare"),
not the work.

| Step offset | type | meaning |
|---|---|---|
| +0x00 | u32 flags | bit0 = run the range in parallel (0146c960 `RunRangeExecutionParallel`); bit1 = run the whole range in one call; neither = serial per-item loop, with `RenderFrameIfNeeded` 01468d10 between items |
| +0x08 | ptr | runner context (`[ctx+0x120]` = game state) |
| +0x10..+0x48 | std::function | "single" body; impl pointer at +0x48 (MSVC small buffer) |
| +0x50..+0x88 | std::function | range body; impl pointer at +0x88. Do_call = impl vtable slot 2, called as `(impl, next_step_out, int32 range[2], bool *is_last)` |
| +0x90 / +0x94 | i32 | range begin / end |
| +0x98 / +0x9c | i32 / u8 | batch mode / mode-set flag. Batch = n/(threads+1), n/3/(threads+1), or decided by the cost heuristic 0146c010 `MaybeRunRangeExecutionParallelWithHeuristic` ("Expensive"/"Affordable") |
| +0xa0..+0xb0 | ptr, ptr, u8 | per-item cost array (begin, end), has-heuristic flag |

What RunTask does after Execute:
- If both impl pointers are still NULL, it ends the task (`48 8B 4D D8 4C 8B 45 98 4D 85 C0 75 ?? 48 85 C9 0F 84`).
- Otherwise it runs the range part according to the flags, then the single body.
- It then copies the next stage, which the bodies wrote to `rbp+0x30`, and loops back to 0146d104. Execute is not called again.

Functor vtables are MSVC `_Func_base`: slot 0 `_Copy(where)`, slot 1 `_Move`, slot 2 `_Do_call`, slot 3 `_Target_type`, slot 4 `_Delete_this(bool dealloc)`.

The runner 014736c0 calls RunTask for each due task **one after another** (loop at 01473be0, call at 01473c78). Parallelism exists only inside a task, through its range stages.
This conclusion is static only; the plugin's `sim_opt diag=1` reports the main-thread share per task at runtime.

The shared 8-slot wrapper 0084fb00 only stores "call slot 7" as the single body. Those classes therefore run their body serially, unless the body starts its own jobs.

## Hot tasks (4-hour profile, binaries/v3x/tick_profile.csv) and what they are

| task (share of task time) | Execute | how it runs | notes |
|---|---|---|---|
| HandleAutonomousInvestment 11.1% | 00979890 | parallel (flags \|= 3, range = all countries, cost per country = `country+0xbe4 + country+0xb6c`) | uses script RNG (03787300). A max of 1.9 s means one heavy country, not missing parallelism |
| UpdateAI 10.6% | 01647910 | single serial body | 01669340 -> 01605eb0 (database accessors 00980610, 007319a0) |
| RecalculateModifierNodes 6.8% | 0146b5f0 | serial | 0157e310 ("Modifier update already in progress") -> 01590c90 `UpdateNodesByCategory`. Must never be skipped |
| UpdatePopGrowth 6.0% | 00e7af10 | game-rule bit check, then 00e5b8d0 -> 00e5bbd0 `UpdatePopGrowthParallel` (pop_growth.cpp, its own job group) | already parallel |
| UpdateStateTrades 5.8% (max 3.1 s) | 00dcedd0 -> 00dd1650 | serial, 4.2 KB (statemanager.cpp, `RenderFrameIfNeeded` inside) | best fork/join target; it mutates market state, so its loop must be mapped first |
| UpdateBuildingSpendingPriorities_Stage2 3.5% | 01699380 | parallel (\|= 1) | |
| MonthlyPulseJournalEntries (avg 490 ms, max 4.5 s) | 01016500 -> 010162a0(step, pulse=1) | serial, 2 stages | see below |

### MonthlyPulseJournalEntries (010162a0)

The same function serves the yearly pulse (01016510, pulse=2) and pulse 0 (010164f0).

- **Stage 1**, inside Execute: loops over the active journal entries (ids at `gs+0x1000+0xa8`, count `+0xb4`). For each entry:
  - 009f5040 resolves the id to the entry.
  - For each item of `entry->type+0x968` (count `+0x974`): 00edcd00, then 0101e0e0.
  - Then 0101c770 `ExecuteOnPulse` (journal_entry.cpp): effect `type+0x728/0x738/0x748` for pulse 0/1/2, under a script RNG scope.
- **Stage 2**: a serial per-item range over all countries. The container is `gs+0x80`: array at +0, count at +0xc. The functor's vtable is 046fbab0: `_Copy` 0106d270, `_Do_call` 0106d210, `_Delete_this` 007be460. Its captures are +8 container, +0x10 journal manager, +0x18 pulse. Do_call runs 010195c0(journal_mgr, `country+0x18` id, pulse) for each index.
- **Why not stagger (tried as `sim_opt stagger_journal`, removed from the plugin):** It split stage 2 over days, reusing the engine's own per-country functor. That cannot be made save-safe:
  - Saves execute as **session commands on a job**, outside any tick task. Crash #2 stack: worker 03dfac1e -> 03650fc0 -> session.cpp 0364c6e0 ("Replaying recorded commands") -> 0378aa60 -> `ExecuteGameCommand` 01085b90 -> autosave 00ca9580 -> 03663630 -> `SaveGameState` 03662910.
  - Script cannot run there. The script RNG 03787300 requires an `ALLOW_RANDOM_IN_SCOPE` owner (`*[0540ae80]`, thread id at +0x10), which only tick execution sets. Running the pending pulses from `save_begin` dereferenced NULL at 03787360, even though `GameStateAccess` read as Logic.
  - The monthly autosave executes **between the pulse tick and the next tick**; crash #2 hit it while a cycle was pending. Any deferral past the pulse tick, even to the remaining TICK sub-steps of the same day, therefore leaves pulses outside that save. Manual saves can also happen at any pause.
  - Within a single tick, moving the work to a later task does not reduce the tick's length, so there is nothing to gain.
- Measurement: `sim_opt diag_journal=1` (default on, read-only) times `ExecuteOnPulse` 0101c770 per journal-entry type (key `entry+0x18`; name = std::string at `type+0x18`, the field the type validation 017ffc30 formats) and the per-country stage 010195c0 (`(journal_mgr, country_id, pulse)`, which itself calls ExecuteOnPulse for that country's entries). 00edcd00 takes a 5th stack argument and is not hooked.
- Whether stage 1 or stage 2 dominates the 4.5 s is **not measured yet**. diag's Execute time for this task is stage 1; the tick_profiler total minus that is stage 2.

### UpdateStateTrades (00dcedd0 -> 00dd1650, statemanager.cpp): iterative solver, evaluation already parallel

Execute 00dcedd0 is a tail call: `StateManager(gs+0x120 -> +0x1c00)::UpdateStateTrades` 00dd1650 (4.2 KB). An early exit at 00dd1ff5 compares a global with `[ctx+0x40]` (not identified).

The work list is an array of 0x140-byte trade-candidate records (pointer at rsp+0x40, count at rsp+0x4c). Bytes +0xc..+0xf of each record are the key. The loop at 00dd209a .. 00dd2427 repeats **until no record changes**:

1. **Compact (serial, cheap):** records with `[rec+4] <= 0` are swap-removed with the last record (loop at 00dd20c7, which copies maps with 00913d20). The tail is then destroyed (00e285e0, 00e15bb0 per record).
2. **Evaluate (parallel):** 00e02c90 is a parallel-for over all records (statemanager.cpp:1061).
   - Batch = n / (threads+1) / 3, where threads comes from scheduler `+0x648`.
   - The job body is 00dd26e0. It reads the per-key map (rbp-0x40, FNV hash of the key, 0xb0-byte buckets) and the four accumulator containers (rbp+0xe0, +0x90, +0x40, -0x10) through 00d96990.
   - It evaluates the trade twice, with 00dc7260 and 00dc7930 (-> 00d953c0 "calculated trade advantage"). The random seed is derived from `[record+8 object]+8`, so it is deterministic per record.
   - It writes **only its own record** (+0xb0..+0x140). The phase is embarrassingly parallel and already runs on the job system. The waiting thread helps with other jobs in 03dba2d0, which is why rendering and UI functions show up below it in the profile.
3. **Apply (serial, ordered):** for each record, 00e1d9d0 looks up the key, then 00d96a90 (1.8 KB) applies the trade. This writes the four accumulators (and through them the state and market data) and returns "changed". The changed flags are OR-ed into r12b.
4. **Iteration boundary:** `RenderFrameIfNeeded` (01468a60/01468be0 at 00dd240c).

Data dependencies:
- The evaluation in iteration k+1 reads what the applies of iteration k wrote. This is a Jacobi-style evaluation followed by an order-dependent greedy apply.
- Profile (Game_to_Empty_2, inside 00dd1650's 62k inclusive samples): job body 00dd26e0 has about 12.7k samples, the apply 00d96a90 about 7.0k. The rest is help-while-waiting noise.

Verdict: **no safe split or parallelization exists without changing results.**
- (a) The expensive part is already parallel.
- (b) Parallelizing the apply would race on the shared accumulators and market, and its results depend on order.
- (c) Re-evaluating only the records affected by the last iteration's applies would be exact only with the complete read set of 00dc7930/00d953c0, which is not mapped.
- (d) Capping iterations or loosening convergence changes the result.

Next steps:
- **Measure the iteration count per call.** 00e02c90 has a single caller, so counting its calls per 00dd1650 call is a cheap probe. This is implemented as `sim_opt diag_trades=1` (default on, read-only, hooks the entries of 00dd1650 and 00e02c90; anchors checked by `tools/re_simopt_check.py` step 7). Then decide whether the 3.1 s spikes come from many iterations or from large n.
- **If many iterations are the cause,** (c) is the only exact optimization candidate. It requires mapping the read set of 00d953c0.

## Ranked hook targets

1. **0146d010 RunTask** (or the call at 0146d0ee). One hook gives the identity and timing of every task: rcx/r14 = task object, `*(vtable)` lets you look up the name through RTTI. Best place for a per-task profiler or to skip or replace tasks.
2. **014736c0 TickTaskRunner**. Wraps all tasks for one tick. Use it for whole-tick timing and pre/post-tick callbacks. It already contains PDX's own timing printer (the `Task {:>32s}` format), so turning that on may be an alternative to hooking.
3. **01465690 GameState tick** (`Processing Tick: `). The outermost per-tick entry point. Good for "on tick N" events, pause/speed control and save-safe points.
4. **Per-task bodies** (work fn column), for targeted economy changes:
   - UpdateWorldMarket (WEEK) 0103ee80
   - UpdateStateTrades (WEEK) 00dcedd0
   - ProcessPopPurchases (WEEK) 0104e9c0
   - UpdateEmployment (WEEK) 00dcf650
   - UpdateBuildingThroughput (WEEK) 00df7a20
   - AdjustWages (WEEK) 00af7120
   - ProcessWagesStage1/2 (WEEK) 00af5400 / 00af54d0
   - UpdatePopGrowth (DAY) 00e7af10
   - UpdateAI (TICK LATE) 01647910
   - RecalculateModifierNodes (TICK LATE) 0146b5f0
   - MoveArmies (TICK) 0158fa40
5. **01468a60 / 01468d10** (RenderFrameIfNeeded). Only useful for UI responsiveness during long ticks. Not a simulation hook.

Hooking advice: prefer the vtable-slot swap (writing to .rdata after VirtualProtect) per task class over inline patches; the slot RVA is `vtable + 8*slot`.

## Full task table

Dependencies come from a heuristic (references to other task-name strings in the init function) and are unverified.

| task | rate | prio | range-parallel | vtable | work fn (last slot) | slots | deps (heuristic) |
|---|---|---|---|---|---|---|---|
| FlushNavyAIData | TICK | LATE |  | 04772530 | 01699c30 | 7 |  |
| HandleAutonomousInvestment | TICK | DEFAULT | yes | 046a1b10 | 00979890 | 7 | RemoveNonexistantCountries, UpdateBuildingSpendingPriorities_Stage2 |
| MoveArmies | TICK | DEFAULT |  | 04763498 | 0158fa40 | 7 | SplitShipsToRetrofit |
| MoveFleets | TICK | DEFAULT |  | 04765110 | 0158fbe0 | 7 |  |
| PrepareNavyAIData | TICK | LATE |  | 04771f40 | 0169a150 | 7 | UpdateNavyAIShipTemplates |
| ProcessAttritionCasualties | TICK | DEFAULT |  | 046c13c8 | 00ad97a0 | 8 |  |
| ProcessEconomicMigration | TICK | DEFAULT | yes | 04768060 | 0157cba0 | 7 | SpawnCulturalCommunities, ProcessMassMigration |
| ProcessMassMigration | TICK | DEFAULT | yes | 04768a08 | 0157ce20 | 7 |  |
| ProcessSlaveTrade | TICK | DEFAULT |  | 04765cd0 | 014d6a20 | 7 | UpdateSlaveImportMarkers |
| PurgeOldCulturalCommunities | TICK | DEFAULT |  | 046c4d30 | 00b7edd0 | 7 |  |
| RecalculateModifierNodes | TICK | LATE |  | 04741258 | 0146b5f0 | 8 |  |
| RefreshCulturalCommunities | TICK | DEFAULT |  | 046cc1f8 | 00b7ec50 | 7 | PurgeOldCulturalCommunities |
| RemoveDeadObjects | TICK | LATE |  | 0472a638 | 01469ba0 | 8 | RemoveNonexistantCountries |
| RemoveNonexistantCountries | TICK | DEFAULT |  | 046b8590 | 008dca30 | 8 | UpdateColonizationProgress, UpdateIncorporation, UpdateCivilWars, DailyUpdateWars |
| SpawnCulturalCommunities | TICK | DEFAULT |  | 046c5a08 | 00b7eb40 | 7 | RefreshCulturalCommunities |
| SpawnInterceptBattles | TICK | DEFAULT | yes | 046f32d8 | 00f5d7b0 | 7 |  |
| SpawnRaidSupplyBattles | TICK | DEFAULT | yes | 046f33c8 | 00f5d3f0 | 7 |  |
| SplitShipsToRetrofit | TICK | DEFAULT |  | 046dd898 | 00d49140 | 8 |  |
| StartHarvestConditions | TICK | DEFAULT | yes | 04742a50 | 01491440 | 7 |  |
| UpdateAI | TICK | LATE |  | 04775100 | 01647910 | 7 | UpdateNavyAI, UpdateNavyAIShipTemplates, PrepareNavyAIData |
| UpdateActiveBattles | TICK | DEFAULT |  | 046c0c18 | 00adebc0 | 7 | UpdateBattlesCachedCombatData |
| UpdateActiveNavalBattles | TICK | DEFAULT | yes | 046f4878 | 00f5dd80 | 7 | SpawnInterceptBattles, SpawnRaidSupplyBattles |
| UpdateAdvanceProgress | TICK | DEFAULT | yes | 046be1f0 | 00aa01d0 | 7 |  |
| UpdateAuthoritySpendingPriorities | TICK | DEFAULT |  | 047739f0 | 016480d0 | 8 | UpdateSpendingVariables |
| UpdateBattlesCachedCombatData | TICK | DEFAULT | yes | 046c1eb0 | 00b0bbf0 | 7 |  |
| UpdateBuildingSpendingPriorities_Stage1 | TICK | DEFAULT |  | 047722f0 | 01699630 | 7 | UpdateSpendingVariables |
| UpdateBuildingSpendingPriorities_Stage2 | TICK | DEFAULT | yes | 04771dc8 | 01699380 | 7 | UpdateSpendingVariables, UpdateBuildingSpendingPriorities_Stage1 |
| UpdateCachedCarryingCapacity | TICK | DEFAULT |  | 046b9820 | 00977d40 | 7 |  |
| UpdateCachedLiteracy | TICK | DEFAULT |  | 046b5af0 | 00977a40 | 7 |  |
| UpdateCachedMilitaryReachCountries | TICK | DEFAULT |  | 046b9190 | 00977bc0 | 7 |  |
| UpdateCivilWarVictories | TICK | DEFAULT |  | 046bcfa8 | 00a71a70 | 8 |  |
| UpdateCountryFormationData | TICK | DEFAULT |  | 046b3e28 | 008ca230 | 8 |  |
| UpdateDiplomaticDemands | TICK | DEFAULT |  | 046cc798 | 00b6ca90 | 8 | ValidateDiplomaticDemands |
| UpdateFormationTravelCosts | TICK | DEFAULT |  | 04766d50 | 01508cb0 | 8 | UpdateFormationTravelRegionAccess |
| UpdateFormationTravelRegionAccess | TICK | DEFAULT |  | 04766ba0 | 01508b80 | 8 | UpdateDiplomaticPlays |
| UpdateHQs | TICK | DEFAULT |  | 046f5d58 | 00f85e80 | 8 | UpdateActiveBattles |
| UpdateIGLawApprovalCache | TICK | DEFAULT |  | 046f78f0 | 00fc45e0 | 7 |  |
| UpdateJobSatisfaction | TICK | DEFAULT |  | 046e8a38 | 00e7e7b0 | 7 | UpdatePopGrowth, ProcessPopPurchases, AdjustWages, UpdateCountryNormalWageCache |
| UpdatePotentialMilitaryStrength | TICK | DEFAULT |  | 046b5e88 | 00977f70 | 7 |  |
| UpdateRichPresence | TICK | DEFAULT |  | 046ebd60 | 00ed1f30 | 8 |  |
| UpdateSpendingVariables | TICK | DEFAULT |  | 04771be8 | 01647ec0 | 8 |  |
| UpdateUnemploymentProfessionTransitions | TICK | DEFAULT | yes | 046e8080 | 00e9a880 | 7 | UpdateEmployment |
| ValidateSways | TICK | DEFAULT |  | 046dfbc0 | 00dd79d0 | 8 |  |
| ActivateLawsAtMaxPhases | DAY | DEFAULT |  | 046b6a38 | 0097a3a0 | 7 | UpdateLawEnactmentProgress, UpdatePoliticalMovements |
| AutoDeclinePendingDiploActions | DAY | DEFAULT |  | 046d15b0 | 00c15430 | 8 |  |
| CheckCareerEnds | DAY | DEFAULT | yes | 046bcce0 | 00aa0500 | 7 | DailyUpdateCharacters |
| CheckTemporaryHostilityExpiry | DAY | DEFAULT |  | 046cf438 | 00c1c3d0 | 8 |  |
| CreateElections | DAY | DEFAULT |  | 046cfac0 | 00c3aaf0 | 7 |  |
| CreateNewPoliticalMovements | DAY | DEFAULT | yes | 046e75a8 | 00e9d0d0 | 7 |  |
| DailyCountryBudgetUpdate | DAY | DEFAULT |  | 046a1e30 | 009413c0 | 8 |  |
| DailyStateRegionUpdate | DAY | DEFAULT | yes | 046dff30 | 00df9aa0 | 7 |  |
| DailyUpdateCharacters | DAY | DEFAULT |  | 046bda00 | 00a4dcf0 | 8 | SpawnCharactersInPool, UpdateAgitatorMovementSupport |
| DailyUpdateDiplomaticPacts | DAY | DEFAULT |  | 046cf4b8 | 00c16310 | 8 | ValidateDiplomaticPacts |
| DailyUpdateWars | DAY | DEFAULT | yes | 046dbdd8 | 00d15ec0 | 7 | UpdateDiplomaticPlays |
| DissolveMicroPops | DAY | DEFAULT | yes | 046e7530 | 00e9a0f0 | 7 |  |
| FinishHarvestConditions | DAY | DEFAULT |  | 0474e570 | 01482320 | 7 |  |
| GenerateLeaders | DAY | DEFAULT |  | 046be318 | 00a4e610 | 8 | RemoveNonexistantCountries |
| LockInStraitSettings | DAY | DEFAULT | yes | 046f3c08 | 00f5be00 | 7 |  |
| MergeSmallPops | DAY | DEFAULT | yes | 046e7a28 | 00e9a5d0 | 7 | UpdateEmployment, ProcessEconomicMigration, UpdateUnemploymentProfessionTransitions |
| PostIGPowerStatusNotifications | DAY | DEFAULT |  | 046f7008 | 00fc3110 | 7 | ProcessMarginalizedIGsPartyMembership |
| PreUpdatePoliticalMovements | DAY | DEFAULT |  | 046e7d50 | 00e9c580 | 7 | UpdateLawEnactmentProgress, UpdatePopSupportForPoliticalMovements |
| ProcessInsurrectionaryIGsInGoverment | DAY | DEFAULT |  | 046f4ff8 | 00fc3310 | 7 | ProcessInterestGroups |
| ProcessInterestGroups | DAY | DEFAULT | yes | 046f54b0 | 00fc3410 | 7 | UpdateInterestGroupPoliticalStrength |
| ProcessJournalEntryTimeout | DAY | DEFAULT |  | 046fbe58 | 010164a0 | 7 |  |
| ProcessMarginalizedIGsPartyMembership | DAY | DEFAULT |  | 046f7670 | 00fc3210 | 7 | ProcessInsurrectionaryIGsInGoverment |
| ProcessProposalTimeout | DAY | DEFAULT |  | 046d4418 | 00c899d0 | 8 | AutoDeclinePendingDiploActions |
| RecalcAIRegionScores | DAY | DEFAULT |  | 04774a38 | 0169a830 | 7 |  |
| RecordElectionVotes | DAY | DEFAULT | yes | 046cf9e8 | 00c3a7a0 | 7 |  |
| RemoveExpiredCatalysts | DAY | DEFAULT |  | 046c7cc0 | 00b6aee0 | 8 |  |
| RunScriptedTests | DAY | LATE |  | 046ecc58 | 00edf840 | 7 |  |
| SpawnCharactersInPool | DAY | DEFAULT | yes | 046be3a0 | 00a9f7a0 | 7 |  |
| SyncedAIPostUpdate | DAY | DEFAULT |  | 047720d8 | 016483f0 | 8 | UpdateAIStrategies, UpdateAICountryGoalsAdjustments, UpdateAICountryGoalsAddRemove |
| UpdateAICountryGoalsAddRemove | DAY | DEFAULT |  | 04774b20 | 016484a0 | 8 |  |
| UpdateAICountryGoalsAdjustments | DAY | DEFAULT |  | 04773be0 | 016486e0 | 7 | UpdateAICountryGoalsStrategic |
| UpdateAICountryGoalsStrategic | DAY | DEFAULT |  | 04774030 | 01648630 | 8 | UpdateAICountryGoalsAddRemove |
| UpdateAIGoals | DAY | DEFAULT |  | 04774800 | 01647c80 | 8 | UpdateAIStrategies, UpdateBuildingSpendingPriorities_Stage2 |
| UpdateAIMemories | DAY | DEFAULT |  | 04773cf8 | 01648a20 | 8 |  |
| UpdateAIRegionalObjectives | DAY | DEFAULT | yes | 046c0548 | 00b0c920 | 7 | SyncedAIPostUpdate |
| UpdateAIStrategies | DAY | DEFAULT |  | 04774c78 | 01647da0 | 8 |  |
| UpdateAppeasement | DAY | DEFAULT |  | 046e8b80 | 00e9e4d0 | 7 |  |
| UpdateAverageEmployeeEarnings | DAY | DEFAULT |  | 047495d0 | 01469bb0 | 7 |  |
| UpdateBlockades | DAY | DEFAULT |  | 046e25b8 | 00df9910 | 7 |  |
| UpdateColonizationProgress | DAY | DEFAULT | yes | 046b8fa0 | 00978840 | 7 |  |
| UpdateCountryFormationCandidates | DAY | DEFAULT | yes | 046b42c8 | 0097a6f0 | 7 | UpdateCountryFormationData |
| UpdateCountryRankings | DAY | DEFAULT |  | 046b2f80 | 008e9ce0 | 8 |  |
| UpdateCountryScorePositions | DAY | DEFAULT |  | 046b3bc8 | 008e9d00 | 7 | UpdateCountryRankings |
| UpdateDiplomaticPlays | DAY | DEFAULT |  | 046cf858 | 00bf88f0 | 8 | DailyUpdateDiplomaticPacts, UpdateCachedDataForWarSupportChange |
| UpdateDynamicCountryNames | DAY | DEFAULT |  | 046a1b50 | 008dd460 | 7 |  |
| UpdateElections | DAY | DEFAULT |  | 046d0918 | 00c2e920 | 7 | RecordElectionVotes |
| UpdateFamines | DAY | DEFAULT |  | 046cd838 | 00bc4e00 | 7 |  |
| UpdateGovernments | DAY | DEFAULT | yes | 046b4198 | 00979340 | 7 | RemoveNonexistantCountries |
| UpdateIncorporation | DAY | DEFAULT | yes | 046e1240 | 00df7c60 | 7 |  |
| UpdateInputGoodsShortage | DAY | DEFAULT |  | 046c1018 | 00b0a0a0 | 7 |  |
| UpdateInstitutions | DAY | DEFAULT |  | 046f5050 | 00f9ac40 | 7 |  |
| UpdateInterestGroupPoliticalStrength | DAY | DEFAULT |  | 046f80d0 | 00fb39b0 | 7 | RecalcInterestGroups, DailyUpdateCharacters, InvalidateCompanyExecutiveModifiers |
| UpdateInvasions | DAY | DEFAULT | yes | 046fdbc0 | 01050060 | 7 | UpdateActiveBattles |
| UpdateJournalEntries | DAY | DEFAULT |  | 046fd8c8 | 01016520 | 8 |  |
| UpdateLawEnactmentProgress | DAY | DEFAULT |  | 046b5e48 | 008cbf60 | 7 |  |
| UpdateMobilization | DAY | DEFAULT |  | 046b6560 | 00978ab0 | 7 |  |
| UpdateMorale | DAY | DEFAULT |  | 04766cb0 | 0157b2a0 | 7 | UpdateInputGoodsShortage |
| UpdateObjectives | DAY | DEFAULT |  | 046f0c78 | 00ef91f0 | 8 |  |
| UpdateObligations | DAY | DEFAULT |  | 046cfe08 | 00c1c380 | 8 |  |
| UpdateOrganization | DAY | DEFAULT |  | 04768770 | 0157b420 | 7 | UpdateSupplyShortageRatio |
| UpdatePoliticalMovements | DAY | DEFAULT | yes | 046e7b50 | 00e9bfc0 | 7 | UpdateLawEnactmentProgress, UpdatePopSupportForPoliticalMovements, PreUpdatePoliticalMovements |
| UpdatePopAcceptanceCache | DAY | DEFAULT |  | 046e71a0 | 00e7af30 | 8 | UpdateCommunityAcceptanceCache, UpdateCountryAcceptanceCache |
| UpdatePopGrowth | DAY | DEFAULT |  | 046e9810 | 00e7af10 | 8 |  |
| UpdatePopsInterestGroupsAndMovements | DAY | DEFAULT |  | 046e5f08 | 00e7aef0 | 8 |  |
| UpdatePortBombardment | DAY | DEFAULT | yes | 046e2980 | 00df96a0 | 7 |  |
| UpdatePresence | DAY | DEFAULT |  | 047662f0 | 0157af10 | 7 |  |
| UpdateRecalledFleets | DAY | DEFAULT | yes | 04767e30 | 0157a8d0 | 7 | ProgressShipConstructions, FindCrewForShips |
| UpdateRelationsFromPiracy | DAY | DEFAULT | yes | 046cffc0 | 00c3af30 | 7 |  |
| UpdateRelationsFromStraits | DAY | DEFAULT | yes | 046f3b28 | 00f5c060 | 7 |  |
| UpdateRelationsFromTreaties | DAY | DEFAULT | yes | 046f2d28 | 00f5a070 | 7 |  |
| UpdateSavedVariables | DAY | DEFAULT |  | 04729ab8 | 0146b190 | 8 |  |
| UpdateStateDevastation | DAY | DEFAULT | yes | 046e1910 | 00df6ee0 | 7 | DailyStateRegionUpdate |
| UpdateStateInfrastructure | DAY | DEFAULT |  | 046e16e0 | 00dcedf0 | 8 |  |
| UpdateStatePollutionCache | DAY | DEFAULT | yes | 046e12f8 | 00df6b20 | 7 |  |
| UpdateStateTurmoilAndLoyalistsCache | DAY | DEFAULT | yes | 046e3430 | 00df6870 | 7 |  |
| UpdateSways | DAY | DEFAULT |  | 046e1178 | 00dd79f0 | 8 |  |
| UpdateTerrainManipulation | DAY | DEFAULT |  | 046e3cc8 | 00dcf220 | 7 |  |
| UpdateTimedModifiers | DAY | DEFAULT |  | 04729740 | 0146a4d0 | 7 |  |
| UpdateTransportMissions | DAY | DEFAULT | yes | 046d85d8 | 00cdf380 | 7 |  |
| ValidateArticles | DAY | DEFAULT | yes | 046f3838 | 00f5aa40 | 7 |  |
| ValidateDiplomaticDemands | DAY | DEFAULT |  | 046c5ca0 | 00b6ca70 | 8 |  |
| ValidateDiplomaticPacts | DAY | DEFAULT |  | 046d0348 | 00c162f0 | 8 |  |
| ValidateLobbies | DAY | DEFAULT |  | 046e78d8 | 00e471e0 | 8 |  |
| AdjustWages | WEEK | DEFAULT |  | 046c0160 | 00af7120 | 8 | ProcessWagesStage2 |
| ApplyFleetMaintenancePenalty | WEEK | DEFAULT |  | 046df198 | 00d71790 | 7 |  |
| AutoWithdrawFromTreaties | WEEK | DEFAULT |  | unresolved | unresolved | 0 | EffectuateTreatyObligations |
| AverageStandardOfLivingUpdate | WEEK | DEFAULT |  | 046b9768 | 008dd430 | 8 |  |
| CheckCharacterNewTrait | WEEK | DEFAULT |  | 046bcd60 | 00a9fc20 | 7 | UpdateCharacterTraitGain |
| CheckCompanyChartersPossible | WEEK | DEFAULT |  | unresolved | unresolved | 0 |  |
| CheckForNewCivilWars | WEEK | DEFAULT | yes | 046bdaa8 | 00a9e3d0 | 7 |  |
| ClearExpiredMigrationBuckets | WEEK | DEFAULT |  | 046e0dd0 | 00df7580 | 7 |  |
| ClearLogisticsData | WEEK | DEFAULT |  | 046bf968 | 00b08dc0 | 7 | UpdateFormationSupply |
| ClearOutlinerData | WEEK | DEFAULT |  | 046f0840 | 00efe170 | 8 |  |
| CollectivizePrivateProperty | WEEK | DEFAULT | yes | 046b8ed0 | 009784b0 | 7 | RemoveNonexistantCountries, UpdateCachedLiteracy |
| DistributeSupplyShips | WEEK | DEFAULT |  | 046b4af8 | 009775e0 | 7 | UpdateSupplyData |
| DownsizeMilitaryBuildingsToUnitCount | WEEK | DEFAULT | yes | 046c0728 | 00b09900 | 7 | UpdateAutoDownsizing |
| EffectuateTreatyObligations | WEEK | DEFAULT |  | unresolved | unresolved | 0 | WeeklyNonFulfillmentChecks, MonthlyNonFulfillmentChecks, YearlyNonFulfillmentChecks |
| EnslaveDebtors | WEEK | DEFAULT |  | 046e9460 | 00e7afc0 | 7 | ProcessPopPurchases |
| FindCrewForShips | WEEK | DEFAULT | yes | 046dec68 | 00d709c0 | 7 | ProgressShipConstructions |
| GenerateCharacterConditions | WEEK | DEFAULT | yes | 046be360 | 00a9ff60 | 7 |  |
| NewUpdateMaxOffenseDefense | WEEK | DEFAULT |  | 046be288 | 00a8bac0 | 7 |  |
| PeriodicDebugLog | WEEK | LATE |  | 0475c530 | 0146b7a0 | 8 |  |
| ProcessAutoExpandingBuildings | WEEK | DEFAULT |  | 046c03e8 | 00af7400 | 8 | ProcessWagesStage2 |
| ProcessDependentWages | WEEK | DEFAULT |  | 046e7458 | 00e9b1e0 | 7 |  |
| ProcessPopDiplomaticIncome | WEEK | DEFAULT |  | 046e9c20 | 00e9ace0 | 7 |  |
| ProcessPopPurchases | WEEK | DEFAULT |  | 046fd148 | 0104e9c0 | 7 | ProcessWagesStage2, ProcessDependentWages, ProcessWelfarePayments, ProcessPopDiplomaticIncome |
| ProcessWagesStage1 | WEEK | DEFAULT |  | 046bf658 | 00af5400 | 7 | UpdateStateTrades, ProcessWelfarePayments, ValidateProductionMethods |
| ProcessWagesStage2 | WEEK | DEFAULT |  | 046c08a0 | 00af54d0 | 7 | ProcessWagesStage1 |
| ProcessWelfarePayments | WEEK | DEFAULT |  | 046e6298 | 00e9b060 | 7 | UpdateStateWelfareMultiplier |
| ProgressConstructionQueues | WEEK | DEFAULT |  | 046b3b08 | 008dd6d0 | 8 | ValidateProductionMethods |
| ProgressShipConstructions | WEEK | DEFAULT | yes | 046de398 | 00d71470 | 7 | ValidateProductionMethods, SplitShipsToRetrofit, ApplyFleetMaintenancePenalty |
| ProgressSupplyShipConstructions | WEEK | DEFAULT | yes | 046f3058 | 00f5b790 | 7 | ValidateProductionMethods, ApplyFleetMaintenancePenalty |
| RecalcCountryPopStatistics | WEEK | DEFAULT |  | 046b6cd0 | 00979100 | 7 | RecalcStatePopStatistics |
| RecalcStatePopStatistics | WEEK | DEFAULT |  | 046e12b8 | 00df7400 | 7 | UpdatePoliticalMovements, RadicalizePoliticalMovementSupporters |
| RecalculateCredit | WEEK | DEFAULT |  | 046b4938 | 009757d0 | 7 |  |
| RecomputeStandardProductionMethods | WEEK | DEFAULT |  | 046bfcb0 | 00af5760 | 7 |  |
| RecordDiplomaticPlayCosts | WEEK | DEFAULT |  | 046d17c0 | 00c3ce80 | 7 | ProcessWagesStage2 |
| RemoveTimedOutInvalidSeaRegions | WEEK | DEFAULT | yes | 046f4460 | 00f5c540 | 7 |  |
| ResetPopMoney | WEEK | DEFAULT |  | 046e6148 | 00e7af50 | 7 |  |
| SnapshotPiracyTracking | WEEK | DEFAULT | yes | 046d0680 | 00c3b170 | 7 |  |
| SpawnAgitators | WEEK | DEFAULT | yes | 046bf2e0 | 00a9f520 | 7 | UpdateCachedLiteracy |
| StartTreatyBindingPeriods | WEEK | DEFAULT |  | unresolved | unresolved | 0 |  |
| UpdateAgitatorMovementSupport | WEEK | DEFAULT |  | 046bd388 | 00a9f3b0 | 7 |  |
| UpdateAutoDownsizing | WEEK | DEFAULT | yes | 046c0ee0 | 00b09bf0 | 7 | UpdateEmployment |
| UpdateAutoPlacedBuildings | WEEK | DEFAULT |  | 046c1a00 | 00af6050 | 8 |  |
| UpdateBasePopBureaucracy | WEEK | DEFAULT |  | 046dfc50 | 00df7700 | 7 |  |
| UpdateBuildingThroughput | WEEK | DEFAULT | yes | 046e03c0 | 00df7a20 | 7 |  |
| UpdateCachedCompanyProductivity | WEEK | DEFAULT |  | 046b57b0 | 0097dbd0 | 7 |  |
| UpdateCachedDataForWarSupportChange | WEEK | DEFAULT |  | 046d0720 | 00c3cce0 | 7 |  |
| UpdateCachedEconomicDependence | WEEK | LATE |  | 046b8ae8 | 008dcc50 | 8 | ProcessPopPurchases, UpdateEmployment, UpdateBuildingThroughput |
| UpdateCachedPowerProjectionAndBlockadeStrength | WEEK | DEFAULT |  | 04762648 | 0157ad90 | 7 |  |
| UpdateCachedRankingsAndGlobalGDP | WEEK | DEFAULT |  | 046b3ef0 | 008dd5b0 | 8 |  |
| UpdateCachedStraitTradeData | WEEK | LATE | yes | 046f3580 | 00f5c2e0 | 7 |  |
| UpdateCharacterTraitGain | WEEK | DEFAULT |  | 046be418 | 00a9faa0 | 7 |  |
| UpdateCivilWarProgress | WEEK | DEFAULT | yes | 046bc8e0 | 00a9e7f0 | 7 |  |
| UpdateCivilWars | WEEK | DEFAULT |  | 046bcc00 | 00a71a90 | 8 | UpdateCivilWarProgress |
| UpdateCombatUnitCultures | WEEK | DEFAULT |  | 046bd7d8 | 00a8adc0 | 7 | UpdateEmployment |
| UpdateCommunityAcceptanceCache | WEEK | DEFAULT |  | 046cb818 | 00b7e810 | 7 | UpdateCountryAcceptanceCache |
| UpdateConscriptionCenters | WEEK | DEFAULT | yes | 046c0020 | 00b095f0 | 7 | UpdateLawEnactmentProgress |
| UpdateCountryAcceptanceCache | WEEK | DEFAULT |  | 046b3370 | 00979b90 | 7 |  |
| UpdateCountryLocalRegions | WEEK | DEFAULT |  | 046f4b60 | 00fc4100 | 7 | UpdateCountryRegionLocality |
| UpdateCountryNormalWageCache | WEEK | DEFAULT |  | 046b5ec8 | 00978da0 | 7 | ProcessWagesStage2 |
| UpdateCountryRegionLocality | WEEK | DEFAULT |  | 046f5c78 | 00fc4280 | 7 |  |
| UpdateCulturalObsessions | WEEK | DEFAULT | yes | 046c56c8 | 00b7e500 | 7 |  |
| UpdateDefeatedFleets | WEEK | DEFAULT | yes | 04763718 | 0157b0b0 | 7 | UpdateEmployment |
| UpdateEmployment | WEEK | DEFAULT |  | 046dfff0 | 00dcf650 | 7 | UpdateLastWeekEmployeeCount, ProcessEconomicMigration, UpdateAutoPlacedBuildings, UpdateConscriptionCenters, AdjustWages, ProgressConstructionQueues |
| UpdateFleetsReturnToPort | WEEK | DEFAULT | yes | 04762c18 | 0157ab00 | 7 |  |
| UpdateFleetsVeterancy | WEEK | DEFAULT |  | 04766a90 | 0157a4e0 | 7 |  |
| UpdateFormationSupply | WEEK | DEFAULT |  | 046b88d0 | 00977460 | 7 | UpdateProducedSupply |
| UpdateGoodsLeaderboards | WEEK | DEFAULT |  | 046b6e00 | 00976e60 | 7 |  |
| UpdateHiringRates | WEEK | DEFAULT | yes | 046c0dd0 | 00b09e50 | 7 | UpdateEmployment |
| UpdateInfamy | WEEK | DEFAULT |  | 046b6f90 | 00979d10 | 7 |  |
| UpdateInterestInvolvement | WEEK | DEFAULT | yes | 046f4a98 | 00fc39d0 | 7 |  |
| UpdateInterests | WEEK | DEFAULT | yes | 046f6f30 | 00fc3e80 | 7 | UpdateCountryRegionLocality, UpdateCountryLocalRegions |
| UpdateLastWeekEmployeeCount | WEEK | DEFAULT |  | 046c10d8 | 00af5fe0 | 8 |  |
| UpdateLibertyDesire | WEEK | DEFAULT | yes | 046d05f0 | 00c3b6e0 | 7 | WeeklyUpdateDiplomaticPacts |
| UpdateMarketCachedPopNeedExpenses | WEEK | DEFAULT |  | 046fd390 | 0104eb40 | 7 | UpdateStatePopNeedCache |
| UpdateMerchantMarineShortage | WEEK | DEFAULT |  | 046e2080 | 00dd6410 | 8 |  |
| UpdateMigrationData | WEEK | DEFAULT |  | 04762508 | 014d6700 | 8 | UpdateMigrationTargets, AverageStandardOfLivingUpdate |
| UpdateMigrationTargets | WEEK | DEFAULT | yes | 04769858 | 0157d430 | 7 | ValidateMigrationTargets |
| UpdateNavalHostilityDamage | WEEK | DEFAULT | yes | 046d1a58 | 00c3b3d0 | 7 |  |
| UpdateNavyAI | WEEK | LATE |  | 04773968 | 0169a3a0 | 7 |  |
| UpdatePartySupport | WEEK | DEFAULT | yes | 046f0010 | 00f0bc00 | 7 |  |
| UpdatePoliticalMovementRadicalism | WEEK | DEFAULT |  | 046e99a8 | 00e9be20 | 7 | UpdatePoliticalMovements |
| UpdatePopLoyalistsAndRadicalsStatisticsForCurrentWeek | WEEK | DEFAULT |  | 046b3990 | 008dd4d0 | 7 |  |
| UpdatePopLoyaltiesFromStandardOfLivingChanges | WEEK | DEFAULT |  | 046a25e0 | 008dd540 | 7 | ProcessPopPurchases |
| UpdatePopPoliticalStrength | WEEK | DEFAULT | yes | 046e6c40 | 00e9b690 | 7 |  |
| UpdatePopSupportForPoliticalMovements | WEEK | DEFAULT | yes | 046e7bf0 | 00e9cd10 | 7 |  |
| UpdatePowerBlocMandates | WEEK | DEFAULT | yes | 046e8468 | 00e98380 | 7 |  |
| UpdatePowerBlocsLeverage | WEEK | DEFAULT |  | 046e8fc8 | 00e985e0 | 7 |  |
| UpdatePowerStruggles | WEEK | DEFAULT | yes | 046e8bc0 | 00e97f80 | 7 |  |
| UpdateProducedSupply | WEEK | DEFAULT |  | 046c02b0 | 00b09140 | 7 | UpdateInputGoodsShortage, DistributeSupplyShips |
| UpdateProsperity | WEEK | DEFAULT | yes | 046a1f10 | 0097d9b0 | 7 | UpdateCachedCompanyProductivity |
| UpdateRegionalHQs | WEEK | DEFAULT | yes | 046b7858 | 0097d3b0 | 7 |  |
| UpdateShipMaintenanceCaches | WEEK | DEFAULT |  | 046c1d38 | 00b0a700 | 7 | ProcessWagesStage2 |
| UpdateSlaveImportMarkers | WEEK | DEFAULT |  | 04765708 | 014d6880 | 8 |  |
| UpdateStandardBuildingModifiers | WEEK | DEFAULT |  | 046bfc10 | 00b0a2a0 | 7 | RecomputeStandardProductionMethods |
| UpdateStateMostAcceptedCulturesCache | WEEK | DEFAULT |  | 046e2d80 | 00df7ff0 | 7 | UpdateCountryAcceptanceCache |
| UpdateStatePopNeedCache | WEEK | DEFAULT |  | 046e1af8 | 00dcf1b0 | 8 |  |
| UpdateStateTrades | WEEK | DEFAULT |  | 046dfdb8 | 00dcedd0 | 8 | UpdateMarketCachedPopNeedExpenses |
| UpdateStateWelfareMultiplier | WEEK | DEFAULT |  | 046e2718 | 00df84c0 | 7 |  |
| UpdateStraitSettings | WEEK | DEFAULT | yes | 046f2f88 | 00f5bbf0 | 7 |  |
| UpdateSupplyData | WEEK | DEFAULT |  | 046b6320 | 00977810 | 7 | ProgressShipConstructions, SpawnInterceptBattles, MoveFleets, UpdateActiveNavalBattles |
| UpdateSupplyShortageRatio | WEEK | DEFAULT |  | 047630b0 | 0157b5a0 | 7 | UpdateWorldMarket |
| UpdateTechnologyProgress | WEEK | DEFAULT |  | 046e1a80 | 00ddd400 | 7 |  |
| UpdateTension | WEEK | DEFAULT |  | 046cf170 | 00c1c300 | 7 |  |
| UpdateTreatyPorts | WEEK | DEFAULT | yes | 046e1a08 | 00df8280 | 7 |  |
| UpdateUnitVeterancy | WEEK | DEFAULT |  | 046beaa8 | 00a9dea0 | 7 |  |
| UpdateUpdatePrestigeGoodsProduction | WEEK | DEFAULT |  | 046b6b78 | 0097d6e0 | 7 | UpdateProsperity |
| UpdateWorldMarket | WEEK | DEFAULT |  | 046fbbf8 | 0103ee80 | 8 | UpdateStateTrades |
| UpgradeUnitTypes | WEEK | DEFAULT |  | 046bc788 | 00a9dda0 | 7 |  |
| ValidateDecreees | WEEK | DEFAULT |  | 046c78d0 | 00b60800 | 8 |  |
| ValidateGoodsTaxesAndTariffs | WEEK | DEFAULT |  | 046a26e8 | 008dd600 | 7 |  |
| ValidateMigrationTargets | WEEK | DEFAULT |  | 04762ea8 | 0157d630 | 7 |  |
| ValidateProductionMethods | WEEK | DEFAULT | yes | 046bffc8 | 00b092e0 | 7 |  |
| WeeklyBuildingBudgetUpdate | WEEK | DEFAULT | yes | 046bfe10 | 00b0a500 | 7 | ProcessWagesStage2, UpdateWorldMarket, UpdateShipMaintenanceCaches |
| WeeklyCountryCounterUpdate | WEEK | DEFAULT |  | 046b7f78 | 008dd3c0 | 7 |  |
| WeeklyNonFulfillmentChecks | WEEK | DEFAULT |  | 046f4158 | 00f66dd0 | 7 |  |
| WeeklyStateBudgetUpdate | WEEK | DEFAULT |  | 046e2878 | 00df7880 | 7 | WeeklyBuildingBudgetUpdate |
| WeeklyUpdateDiplomaticPacts | WEEK | DEFAULT |  | 046d0a38 | 00c16380 | 8 | ValidateDiplomaticPacts |
| WeeklyUpdateJournalEntries | WEEK | DEFAULT |  | 046fddc8 | 010164f0 | 7 | ProcessJournalEntryTimeout |
| WeeklyWarSupportAndEnforcement | WEEK | DEFAULT |  | 046dbf28 | 00d15cc0 | 7 | UpdateCachedDataForWarSupportChange |
| cohesion_update | WEEK | DEFAULT |  | 046e7658 | 00e98190 | 7 |  |
| ClearCharacterInteractionCooldowns | MONTH | DEFAULT |  | 0479e130 | 01868fa0 | 7 |  |
| ClearJournalEntryScriptedButtonCooldowns | MONTH | DEFAULT |  | 046fcb90 | 0104f800 | 7 |  |
| DecayAcceptanceDeltas | MONTH | DEFAULT |  | 046cc5f0 | 00b7e9b0 | 7 |  |
| DecayFrontOccupationProgress | MONTH | DEFAULT |  | 046ced28 | 00bdaa50 | 7 |  |
| InvalidateCompanyExecutiveModifiers | MONTH | DEFAULT |  | 046a1778 | 0097d1c0 | 7 |  |
| MonthlyNonFulfillmentChecks | MONTH | DEFAULT |  | 046f3018 | 00f66fe0 | 7 | WeeklyNonFulfillmentChecks |
| MonthlyPulseJournalEntries | MONTH | DEFAULT |  | 046fbae8 | 01016500 | 7 | ProcessJournalEntryTimeout |
| MonthlyUpdateDiplomaticPacts | MONTH | DEFAULT |  | 046cf3f0 | 00c163f0 | 8 | ValidateDiplomaticPacts |
| RadicalizePoliticalMovementSupporters | MONTH | DEFAULT |  | 046e5ec8 | 00e9bca0 | 7 |  |
| RecalcInterestGroups | MONTH | DEFAULT |  | 046f6dd8 | 00fb3770 | 8 | DailyUpdateCharacters, InvalidateCompanyExecutiveModifiers |
| ReevaluateLawGroups | MONTH | DEFAULT |  | 046b7c18 | 008cbfe0 | 8 |  |
| RemoveTimedOutAmendments | MONTH | DEFAULT |  | 046f0310 | 00ef7a10 | 8 | UpdateLawEnactmentProgress, ActivateLawsAtMaxPhases |
| ReturnExpiredRegionalExiles | MONTH | DEFAULT | yes | 046bcd20 | 00aa0aa0 | 7 | DailyUpdateCharacters |
| UpdateCurrentFervor | MONTH | DEFAULT |  | 046c4860 | 00b7e170 | 7 | UpdateTargetFervorAndCulturalEvents |
| UpdateInterestGroupSupportForPoliticalMovements | MONTH | DEFAULT |  | 046e7c78 | 00e9ca70 | 7 | UpdatePopSupportForPoliticalMovements, UpdatePopPoliticalStrength |
| UpdateNationalAwakenings | MONTH | DEFAULT | yes | 046c4bc0 | 00b7df40 | 7 | UpdateCurrentFervor |
| UpdatePopLiteracy | MONTH | DEFAULT |  | 046e6a98 | 00e9ab60 | 7 |  |
| UpdatePriceReports | MONTH | DEFAULT |  | 046b5920 | 008dd670 | 8 |  |
| UpdateStateObstinanceCache | MONTH | DEFAULT |  | 046e39f0 | 00df6d40 | 7 |  |
| UpdateTargetFervorAndCulturalEvents | MONTH | DEFAULT |  | 046c67f0 | 00b7e320 | 7 |  |
| UpdateNavyAIShipTemplates | QUARTER | LATE |  | 04771a50 | 0169a5e0 | 7 |  |
| UpdateOrphanedExilePools | QUARTER | DEFAULT | yes | 046bc6e8 | 00aa0890 | 7 |  |
| ElasticLoggingWorld | YEAR | DEFAULT |  | 046d0fe8 | 00c2de00 | 8 |  |
| RefreshRecruitableCharacters | YEAR | DEFAULT |  | 046bbc20 | 00a4dcd0 | 8 |  |
| YearlyNonFulfillmentChecks | YEAR | DEFAULT |  | 046f4420 | 00f671f0 | 7 | MonthlyNonFulfillmentChecks |
| YearlyPulseJournalEntries | YEAR | DEFAULT |  | 046fc198 | 01016510 | 7 | ProcessJournalEntryTimeout |
