# Tick fork/join (tick_par): how the engine schedules tick tasks, and what could run in parallel

Binary: `binaries/victoria3.exe` 1.15.1, ImageBase 0x140000000. All addresses are RVAs.
Tools: `tools/re_tickpar_check.py` (anchor and layout check, ALL OK), `tools/re_tickpar_graph.py` (static dependency graph, engine-order emulation, overlap estimate from `tick_profile.csv`).
Plugin: `plugins/tick_par/` (measure only). Test: `test/tick_par_test.c`.

Legend: **[V]** = verified statically in the disassembly (and by `re_tickpar_check.py`). **[I]** = inferred. **[R]** = needs a runtime check (none was run here; the game was not started).

## 1. How the engine schedules the due tasks: strictly serial, in one fixed order

| RVA | role | anchor |
|---|---|---|
| 014736c0 | tick-task runner | only xref of `Tick Tasks for {}` |
| 0146bce0 | due-list builder, called once per tick from 01473826 (rcx = runner ctx, rdx = out `CPdxArray<instance*>`, r8 = date info) | call shape `4C 8B C3 48 8D 55 F0 49 8B CD E8` |
| 02fd5e70 | global task order, built once (a static flag at 0x2fd5ea1 guards it) | owns `Two tick tasks with the same name, not allowed!` |
| 02fd5ca0 | dependency DFS (recursive) | reads `type+0x30` / `type+0x3c` |
| 01466770 | type to instance lookup: `ctx+0x128` array, or create through type vtable slot 1 | |
| 0146d010 | RunTask. Called from the runner at **01473c78** (the only site in the runner) and from 01472ec3 (in 01472e20) | first xref of `Context %d` |

- **[V] The runner is serial.** It loops over the builder's list (01473be0 .. 0147405d) and calls RunTask once per item. Between calls it does its own bookkeeping: it resets the global current-task strings (01473be3-01473c2c), times the task, and records metrics (01468d70, 01472ac0). Nothing is forked at the task level. Parallelism exists only inside a task, through its range stages (see simulation.md).
- **[V] The order** comes from 02fd5e70:
  1. Take the registry (02fd5be0).
  2. Stable insertion sort by `(priority type+0x2c, name type+0x08)`.
  3. Repeatedly pop the front and DFS it (02fd5ca0). For each dependency name, the DFS removes the matching task from the remaining list and visits it first, then appends itself.
  4. Write the order index to `type+0x50/+0x54`.

  Dependency names that are unknown, or already emitted, are skipped. Result: a topological order with ties broken by priority, then name.
- **[V] The builder** (0146bd60 loop) keeps that order. It filters by rate (`type+0x28`: 1..5, via 01475a70/1475b10/1475bf0/1475cb0/1475e10; TICK is always due), then maps each type to its instance.
- **[V] Type layout** (`CJominiTickTaskType`, ctor 02fd59d0, 272 callers):

  | offset | field |
  |---|---|
  | +0x08 | name (MSVC string: 16-byte buffer or pointer, size at +0x10, capacity at +0x18) |
  | +0x28 | rate, default 0 |
  | +0x2c | priority, default 1 (0 EARLY, 1 DEFAULT, 2 LATE) |
  | +0x30 | dependencies: `CPdxArray<CString>` of **task names** (0x20-byte strings), count at +0x3c |
  | +0x48 | bool, default 1, meaning not identified |
  | +0x50 | order index |

  The task instance holds its type at +0x08 (the runner reads it at 01473d6b).
- **[V] `TickTask.Graph` and `TickTask.List`** (02accf00 / 02accc90) only open debug GUI windows (`CTickTaskGraphView` over `CJominiTickTaskType`). There is no other graph, no concurrency groups, and no "parallel-safe" flag at task level.

### The graph

- **Static [I]:** 272 tasks with 193 declared edges (`re_tickpar_graph.py`).
  - The older heuristic in simulation.md found 176. It misses short names that are built from immediates, for example `FlushNavyAIData -> UpdateAI`, `PrepareNavyAIData -> UpdateNavyAI`, `MoveFleets -> MoveArmies` (17 such edges).
  - Every candidate is validated against the task-name set.
- **Authoritative [R]:** in measure mode the plugin reads the live type objects. It writes the graph to `binaries/v3x/tick_par_graph.csv` (direct dependencies plus the count of unresolved names).

## 2. Why "declared independent" is not a safety contract

The dependency list only orders a single-threaded runner. Every task was written and tested under the assumption that no other task runs at the same time. Concretely:

1. **[V] RunTask writes unsynchronized globals.**
   - The profiler scope record at 0x5436898 (written at 0146d0d7 and 0146d10b).
   - The RenderFrameIfNeeded guard 01468a60/01468be0. It resets global `CString`s (01468c27-01468c50), updates timers, and, when more than 10 ms have passed, calls 01489f40 (01468c17), [I] the frame render, from inside the task. Two RunTasks in flight would race on these strings (heap reuse) and render from two threads.
2. **[V] Script RNG has a single global owner.**
   - The owner pointer is `*0x540ae80`; the owner's thread id is at +0x10. The counter at `owner+8` is incremented non-atomically (03787398).
   - 38 functions swap the owner with `xchg` (02df6f8a, 01085c8e `ExecuteGameCommand`, 03586ebf, ...). 03787280 removes it while it is owned by the current thread.
   - A parallel range removes the owner globally for its duration (0146ca14). The assert at 03787330 reads `Calling global random function without having called ALLOW_RANDOM_IN_SCOPE in this thread`, but the storage is global, not per-thread.
   - So two script-running tasks cannot overlap. In addition, a range-parallel task on one lane would pull the RNG scope from a task on the other lane: the crash #2 pattern, a NULL owner at 03787360.
   - Even without a crash, the RNG sequence would depend on thread timing. That changes results and would desync multiplayer.
3. **[V] Parallel ranges set more global flags** (0146c9b9: byte 0x5c56249, saved and set; 0146c9e1: dword 0x5c79fe8 = 1) and assume they are the only parallel section.
4. **[I] Undeclared ordering is relied on.** Ties are broken by name, and that order is stable, so code may silently depend on it. Example: `RecalculateModifierNodes` (LATE, no deps) runs before `UpdateAI` (LATE) only because "R" < "U", yet UpdateAI reads modifiers. The graph declares them independent.

Consequence: concurrent execution through RunTask cannot be made safe for any task pair without engine-level changes (thread-local profiler/guard state, per-lane RNG owners, and mapped read/write sets). `mode=on` is therefore **refused**. The vetted list is empty (`VETTED` in tick_par.c) and no concurrent executor is shipped. The policy itself is implemented and tested in `tick_par_sched.h`, so an executor could be added if it is ever vetted.

## 3. Measured potential (tick_profile.csv, 4-hour session; tasks' average times)

Method: `re_tickpar_graph.py --profile`. For each due set it emulates the engine order and applies the same policy as the plugin:
- One extra lane; the main lane keeps the engine order.
- Join when the in-flight task is done, before any task that transitively depends on it, and at the end.
- `critpath` = unlimited lanes.

| due set | ticks (4 h) | serial ms | 2-lane, all tasks | 2-lane, without range-parallel tasks | critpath |
|---|---|---|---|---|---|
| TICK only | 9612 | 80.7 | 59.1 | 78.3 | 29.3 |
| +DAY | 2746 | 278.3 | 219.9 | 253.9 | 47.4 |
| +WEEK | 353 | 1318.4 | 1151.2 | 1258.3 | 519.4 |
| +MONTH | 105 | 1900.6 | 1324.6 | 1820.0 | 519.4 |

Weighted over the session (the model's serial total of 2205 s matches the profile's 2201 s):
- All tasks treated as safe: about 22 % of task time could overlap (upper bound).
- Without the 77 tasks that are already parallel inside (they use every worker, so overlapping them gains little): about **5 %**.

The biggest single "overlaps" come from tasks that run script: MonthlyPulseJournalEntries 489 ms, SyncedAIPostUpdate 14 ms, HandleAutonomousInvestment 19 ms (already parallel, uses RNG). Those are exactly the tasks that rule 2.2 excludes.

**[I]** The safe gain is therefore close to zero. The plugin reports the real per-tick numbers ([R]).

## 4. Inside the top serial tasks

- **UpdateAI** (Execute 01647910):
  - **[V]** It is not a single serial body:
    - Stage 1 (serial, on the runner thread) loops over all countries (`gs+0x25f8`, count `+0x2604`). For AI-controlled countries it calls 01669340, which loops over the country's AI subsystems (`country+0xf0`, 0x18 bytes each). For each subsystem, 01605eb0 returns its update period. If `(id + index + phase) % period == 0`, the subsystem is pushed to the work list `task+0x10` and its cost to `task+0x28`.
    - Stage 2 sets `Step.flags |= 3`, range = list size, cost array = `task+0x28`. It runs the selected subsystems through the engine's parallel range with the cost heuristic.
  - So the AI work itself is already parallel. The serial part is the period query 01605eb0: database accessors 00980610/007319a0, plus 008ccbf0 (an allocation path).
  - **[I]** Candidate for an exact in-task fork/join: compute all periods in a parallel-for into a buffer, then push serially in the same order. That gives a bit-identical work list, but only if 01605eb0 is free of side effects (not proven: the allocation path and script values are unmapped). Not implemented.
  - **Purity audit of 01605eb0 (follow-up), verdict: NOT provably pure, so not implemented.** Static scan of the direct-call tree, depth 4, about 60 functions:
    - [V] 01605eb0 itself stores only to its own stack. The only rip-relative stores in the tree are MSVC thread-safe function-local static inits (`_Init_thread_header` 043edf38 / footer 043edecc) in 008ccbf0 (008ccc92, 008cccbd: empty default array on a lookup miss) and 00fb4a90 (00fb4b2c, 00fb4b57). These are race-free, but they do write globals once.
    - [V] 7 indirect (virtual) calls in 01605eb0 itself: 01606011, 01606076, 0160608c, 016060a3, 01606183 (`[vt+8]` on database objects returned by 008d0250 / 0097fa40 / 007319a0 / 00980610), plus 016060fa / 01606126 (the stack array's allocator `[alloc+0x10]` = free). The tree below has many more. The targets are not resolved, so their stores are unknown.
    - [V] 0160603f -> 00a73170 fills a stack `CPdxArray` through an allocator singleton obtained in 00a9e100 (magic static). Whether that allocator is thread-safe is not established.
    - [V] 018034c0 -> 017513c0 fans out into a large evaluator (01752bb0, 00fa4b20, 009bde30, ...), which reaches the error reporter 006be580. [I] This is script-trigger evaluation (it asserts GameStateAccess). Whether it uses the RNG owner 0x540ae80 or writes caches below the virtual calls cannot be excluded.
    - Implementation route also blocked: 00e02c90 has one caller (00dd230e) and its job body is hard-wired (00dd26e0), so it is not a generic parallel-for. The country loop runs inside Execute (stage 1), not as a Step range, so Step bit0 cannot be used without injecting a fake `std::function` range stage.
- **MonthlyPulseJournalEntries:** see simulation.md.
  - Stage 1 runs `ExecuteOnPulse` script under an RNG scope.
  - Stage 2 is a per-country range that the engine deliberately runs as a serial loop (not flag bit0). It runs script per country, so rule 2.2 applies.
  - The engine marks no per-country loop of these two as parallel-safe.

## 5. The plugin (`plugins/tick_par`, default `mode=measure`, read-only)

- **Install** (load time, fail closed with a code). The anchors are the same ones checked by `re_tickpar_check.py`:
  - runner = function of the only `Tick Tasks for {}` xref, with its prologue;
  - RunTask (`Context %d`);
  - in the runner, each of these exactly once: the builder call, the RunTask call, the instance->type read;
  - the call must target RunTask (otherwise it was already redirected);
  - the builder prologue, plus its sorted-registry call and rate read;
  - the sorted function must own the duplicate-name assert;
  - the DFS shape (+0x30/+0x3c) must exist.
- **Hooks:**
  - MinHook on the builder (`hk_builder`, PASS8).
  - The runner's RunTask call site 01473c78 is redirected through a near 14-byte jump stub to `hk_run` (PASS8). It chains with tick_profiler's inline hook on RunTask itself and patches nothing else.
- **Runtime checks** (the measurement turns itself off and passes everything through on failure):
  - the registry is read through the engine's own 02fd5e70;
  - names, dependencies, closure and cycles are checked;
  - every due list must be in topological order;
  - each RunTask call must match the next due instance, otherwise that tick is dropped.
- **Output:** console `tickpar` (`tickpar reset`), automatically on Game->Empty, `tick_par_report.csv`, `tick_par_graph.csv`.
  - Per due-set class: serial, 2-lane (all), vetted, critpath.
  - Per task: offloads, overlapped time, and the last co-running partner.
- **Config:** `tick_par.ini` (`enabled`, `mode=off|measure|on`); `on` is refused and logged. `V3X_NO_TICKPAR=1` disables it.

## 6. Open / not verified

- [R] Nothing has been run in game: hook install, stub reach, the live graph dump, and per-tick overhead (O(n²) bit tests per tick, n <= ~250) are all untested there.
- [I] Meaning of `type+0x48` (default 1).
- [I] Whether 01605eb0 (UpdateAI period query) is side-effect free.
- The generic engine job-submit function is still not identified (threading.md section 5). A future executor would need it, plus thread-local RunTask guards.
