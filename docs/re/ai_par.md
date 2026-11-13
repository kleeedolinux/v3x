# AI / economy task parallelism (ai_par): what already runs in parallel, what is serial, what could change

Binary: `binaries/victoria3.exe` 1.15.1, ImageBase 0x140000000. All addresses are RVAs.
Tools: `tools/re_aipar_check.py` (56 checks: every plugin anchor plus every static fact below, ALL OK).
Plugin: `plugins/ai_par/` (measure by default). Test: `test/ai_par_test.c`.
Legend: **[V]** verified in the disassembly (most of them by `re_aipar_check.py`); **[I]** inferred; **[R]** needs the runtime probe (the game was not started for this note); **[D]** taken from the existing session data (`binaries/v3x/tick_profile.csv`, `sim_opt_diag.csv`, `v3x.log`).

The question was: can the AI tasks run with more fork/join? Tasks cannot run concurrently with each other (tick_par.md section 2). So this note looks inside the tasks: which loops already go through the engine's parallel-for, which are serial, and whether a serial loop or the parallel-for itself can be changed safely.

## 1. The engine's parallel-for

### 1.1 Range stages: RunRangeExecutionParallel (RRP) 0146c960

A task stage with `Step.flags` bit0 is run by RRP. Its only caller is RunTask 0146d010, at 0146d2c2 [V]. Arguments: rcx = task instance (`[rcx+8]` = type, name std::string at `type+8`), rdx = Step, r8 = next-stage Step at RunTask `rbp+0x30`. The next stage does not overlap the Step at `rsp+0x50` [V].

1. **Global state for the whole range** [V].
   - 0146c9b9 saves and clears byte 0x5c56249.
   - 0146c9e1 sets dword 0x5c79fe8 = 1.
   - 0146ca14 takes the **script-RNG owner** with `xchg [0x540ae80], 0`, if the calling thread owns it. 0146cfb7 restores it.
   - So no item of a range can use the global script RNG (it would hit the NULL owner, crash pattern #2).
2. **Threads** [V]: `T = [[0x5c726e8] + 0x648] + 1`. 0x5c726e8 is the scheduler global. +0x648 is the live count of pool kind 1, the NORMAL workers (submit function 014abde8: kind 1 -> 0x648, kind 0 -> 0x908, kind 2 -> 0x388). The `+1` is the calling thread, which helps.
   - [D] This machine has 9 NORMAL workers (`cpu_sched` log `worker normal #0..#8`), 4 BELOW_NORMAL and 1 ABOVE_NORMAL, so [R] T = 10 is expected.
   - The fork passes kind 1 as a constant (0149d328 `mov [rsp+0x28], 1`). **BELOW_NORMAL workers (pinned to E-cores by cpu_sched) never receive range chunks.**
3. **Grain** (0146cae7..0146cb8a) [V]: `mode = Step+0x98` if the optional flag `Step+0x9c` is set, otherwise 1.

   | mode | grain |
   |---|---|
   | 0 | max(n/T, 1) |
   | 1 (default) | max(n/T/3, 1) |
   | 2 (and >= 4) | 1 |
   | 3 | the whole range in one inline call |

4. **Cost heuristic** 0146c010 `MaybeRunRangeExecutionParallelWithHeuristic`. It is called before the grain path (0146cca0) and handles the range itself when it returns 1 [V]. Conditions:
   - the cost-array optional is engaged (`Step+0xb0`, span `+0xa0/+0xa8`);
   - n > 2 * workers;
   - max > min.

   Threshold `t = (int)(2*sigma + mean)`, computed in float with the population variance; it is used only if t < max. Items with cost > t become "Expensive": their own job with **mode 2 / grain 1** (0146c5e9). The rest become "Affordable": grain `max(n_aff/T/3, 1)`. Both jobs are submitted, then both are joined (03df9010 x2). The index lists are function-static (main-thread use only).
5. **Chunks**: `chunks = min(ceil(n/grain), T)` (0146ccdf). With 1 chunk (or mode 3) the functor is called inline on the caller (0146ce2a) [V].
6. **Fork**: 0149d260 builds the chunk tasks and calls 014abd90.
   - 014abd90 is **the generic job-submit function** (scheduler, task array, count, kind, source location). threading.md section 5 had listed it as unidentified.
   - [I] When the pool has no worker of that kind, it runs the tasks inline: task state 0 -> 1 with lock cmpxchg at `task+0x60`, then 03dba2d0, then state 2.
7. **Chunk task** 014d3a50 (`_Do_call` of vtable 047455b0) [V]:

   ```
   for (i = fetch_add(&job[0x130], grain); i < end; i = fetch_add(...)) functor(i, min(i+grain, end))
   ```

   This is **dynamic self-scheduling**. Which thread runs which item is already decided by timing. Within one grab, items run in index order.
8. **is_last**: the wrapper 0149ab70 passes `is_last = (chunk end == Step.end)` [V]. Exactly one call sees it whatever the grain is, and the functors use it to write the next stage.
9. **Join** (0146cdb0..0146cdf0): wait or help on each task (`[vt+8]`), release them (`[vt+0x10]`), then the job dtor 03df8f50.

### 1.2 Hard-wired parallel-for copies (not driven by Step)

Some systems instantiate the same ParallelFor template with their own lambda. The parameter block is `{sched, 1, begin, end, T, grain, mode}`, and the chunk task has the same `lock xadd` loop. The grain is computed **in code** as `max(n/T/3, 1)`, mode 1:
- UpdateStateTrades: 00dd22c0..00dd22ef; parallel-for 00e02c90; chunk task 00e43cd0.
- UpdateNodesByCategory (modifiers): 015912b6..015912e4; parallel-for 015a5960.

Changing their chunking would need a code patch per site. They are not touched.

## 2. The candidate tasks

Session numbers [D]: tick_profile.csv, 2984 TICK runs; total task time about 1018 s.

### 2.1 HandleAutonomousInvestment: 25.8 % (avg 88 ms, max 6.0 s)

- **Execute 00979890** [V].
  - Builds the cost array `cost[i] = [c+0xbe4] + [c+0xb6c]` over all countries (`gs+0x80`, about 267 [D]).
  - Draws ONE script random on the task thread (009799e0 -> 03787300).
  - Sets `flags |= 3` (009799e5), range [0, n), cost optional engaged.
  - Execute itself costs 27 ms per session in total [D].
- **Stage 1, parallel range.** Functor vtable 046b7050, `_Do_call` 009f3410. Per country: `task+0x10[i] = 0x921910(country)` (009f3460). Each item writes only its own slot.
  - 00921910 is the per-country investment-candidate evaluation.
  - Its direct call tree (226 functions, depth 5) has no RNG, job or RenderFrameIfNeeded call [V]. Virtual calls are not followed.
- **Stage 2, SERIAL.** On is_last the functor installs a single-body stage (vtable 046b3a98, body 00a166a0 -> **009b2930**). Per country in order:
  - check the precomputed candidate (`[vt+0x30]`);
  - compute how many builds the budget allows (fixed point, 0x186a0 scale);
  - per build: 008dcea0 (apply the build), then **re-run the full evaluation 00921910 serially** (009b2c2d) for the next build, then RenderFrameIfNeeded (009b2c83/009b2c8d) [V].
  - 008dcea0 reaches the script RNG (-> 00a34160 RNG at 00a347cc; -> 008c24b0 RNG at 008c2831) [V]. 008dd360 starts its own jobs (016a6670/016a6820) [V].
- [I] The 6 s maximum is too long for stage 1 alone (about 267 evaluations on 10 threads). It most likely comes from stage 2: one rich country with many builds, each followed by a serial re-evaluation. [R] The probe splits exactly this: `HAI eval/country (parallel stage 1)` vs `HAI re-eval (serial stage 2)` vs `HAI stage 2 total`.

### 2.2 UpdateAI: 7.6 % (avg 26 ms)

- **Stage 1, SERIAL, inside Execute 01647910** [V].
  - Loops over countries `gs+0x25f8` / `+0x2604`.
  - For AI countries it calls 01669340 (period query 01605eb0) and pushes the due subsystems to `task+0x10` with their cost in `task+0x28`.
  - Execute time is 3.73 s of 77.6 s per session = **4.8 % of UpdateAI, about 0.37 % of all task time** [D].
  - Purity of 01605eb0 is not proven (tick_par.md section 4: 7 unresolved virtual calls, script-trigger evaluation).
- **Stage 2, parallel range with cost heuristic.** `flags |= 3` at 01647a14; n = 1068 avg / 1118 max [D]. Functor vtable 047733b0, `_Do_call` 016cb870. Per item:

  ```
  01604cc0(subsystem, country+0x290, &task[0x40][i], {seed = id + i, 0x5ea6ba9f - seed})
  ```

  - The seed is local and deterministic per item (016cb8c6) [V]. Each item writes its own output slot.
  - 01604cc0 has two more callers (00886281, 00886e93), so the probe filters by return address 016cb8f8.
- **Stage 3, SERIAL** [V]. On is_last: `flags &= ~1` (016cb903). This becomes a serial per-item loop over `task+0x1c` (functor 04771fb8 -> 01647ab0).
  - Each item executes the AI's game commands (01085b90 `ExecuteGameCommand`, which swaps the RNG owner) and calls RenderFrameIfNeeded (01647bc8/01647bd2).
  - Its tree reaches the script RNG 5x (for example 0101f430) [V].

### 2.3 UpdateStateTrades: 17 % (106 runs, avg 1.63 s, max 9.5 s)

Work body 00dcedd0 is a tail jump to 00dd1650. This is the 8-slot class, run as a single body, so Execute (slot 6) is only 0.1 ms [D].

The iterative solver is described in simulation.md. Per iteration:
1. compact;
2. **parallel-for 00e02c90** over the records, body 00dd26e0 per record (inline at 00e02e15 with 1 chunk, otherwise from chunk task 00e43cd0);
3. **SERIAL ordered apply** 00d96a90 per record (00dd23bb);
4. RenderFrameIfNeeded (00dd240c/00dd2418).

**sim_opt diag_trades [D]:**
- 139 944 iterations, 590 389 records: **4.2 records per iteration** (first iteration about 270, then 1-2 records for thousands of iterations: "271 -> 1").
- 1.23 ms per iteration, parallel phase 48 % (0.59 ms per iteration).
- With 1 record the parallel-for is a single inline call. With 2-4 records it forks 2-4 tasks.

The time is a **dependency chain**: iteration k+1 evaluates what apply k changed. The tail is one or two routes being stepped thousands of times.

### 2.4 UpdatePopGrowth: 4.3 %

00e7af10 checks a game rule and tail-jumps to 00e5b8d0, which runs:
- 00e5c8b0, the preparation;
- **00e5bbd0 `UpdatePopGrowthParallel`**: one RNG draw on the task thread (00e5bc10), then its own job (00ea3b60 -> 03df8f50);
- **00e5c260, the SERIAL apply**: RNG through 00dbef10 -> 00b55c50 (00b55dfc), and RenderFrameIfNeeded at 00e5c4ad [V].

### 2.5 RecalculateModifierNodes: 4.9 %

0146b5f0 -> 01553720 -> 0157e310 ("Modifier update already in progress") -> 01590c90 `UpdateNodesByCategory`. It loops over the node categories in dependency order. Inside a category the nodes run through the hard-wired parallel-for 015a5960 (sec. 1.2) [V]. No other job site exists in its direct tree, depth 7 (about 340 functions).

## 3. Verdicts per loop

| loop | where | runs now | verdict |
|---|---|---|---|
| HAI per-country evaluation | 009f3410 -> 00921910 | parallel (RRP + heuristic) | **already parallel**. Chunking lever: SAFE (sec. 4), gain [R] |
| HAI apply / build / re-evaluate | 009b2930 | serial | **UNSAFE**: builds draw script RNG (00a347cc, 008c2831), so the order of builds decides the random sequence. The re-evaluation reads what earlier builds changed ([I] including builds by other countries, e.g. foreign investment). RenderFrameIfNeeded must stay on the task thread |
| UpdateAI period query | 01647910 -> 01669340 | serial | **NEEDS-RUNTIME-PROBE**, and not worth it: at most 0.37 % of task time. Static purity unproven (tick_par.md). Probe: run 01605eb0 twice per subsystem and compare results and global writes (write-watch on .data); not built, because the ceiling is too low |
| UpdateAI subsystem updates | 016cb870 -> 01604cc0 | parallel (RRP + heuristic) | **already parallel**. Lever: SAFE, gain [R] |
| UpdateAI command execution | 01647ab0 | serial | **UNSAFE**: executes game commands (01085b90), uses the script RNG, mutates game state in a fixed order |
| trades per-record evaluation | 00e02c90 -> 00dd26e0 | parallel | **already parallel**. In most iterations there is nothing left to split (1-2 records). Chunking is hard-wired in code |
| trades apply | 00dd1650 -> 00d96a90 | serial | **UNSAFE**: order-dependent greedy apply into shared accumulators and market data (simulation.md). Only an algorithmic change (fewer iterations) would help, and that changes results |
| PopGrowth apply | 00e5c260 | serial | **UNSAFE**: script RNG (00b55dfc) in the item order, RenderFrameIfNeeded |
| modifier categories | 01590c90 | serial over categories, parallel inside | **UNSAFE** [I]: categories are processed in dependency order (the engine already parallelises inside a category) |

Result: **no serial loop is proven safe**. `mode=on` is refused and logged. Every serial stage the engine kept serial after a parallel stage either uses the global script RNG, executes commands, or applies order-dependent writes. The parallel parts are already on the engine's parallel-for.

## 4. Cheap levers on the parallel-for

**Chunk size (implemented, opt-in `fine_chunks=1`).** For listed tasks whose stage did not set a batch mode itself (`Step+0x9c` empty), the RRP pre-hook sets mode 2 (grain 1). With `fine_chunks_heuristic=0` it also disengages the cost optional `Step+0xb0`. The post-hook restores all three fields. Why this is safe:
- [V] Grain 1 is a schedule the engine runs itself: always for Expensive items (0146c5e9), and through mode 1 whenever n < 6T. Example: 267 countries on a machine with T >= 45 (44+ NORMAL workers).
- [V] Item-to-thread assignment is already timing-dependent (atomic counter, 014d3a6b). Only one call sees is_last, independent of grain (0149ab70).
- [V] The range runs with the script-RNG owner removed (0146ca14). UpdateAI items use local per-item seeds (016cb8c6).
- [I] Lockstep multiplayer between machines with different worker counts (`GetNativeSystemInfo`, `Threading.TaskThreadCount`) requires range results that do not depend on grain or chunk count.
- What it changes: only which thread runs which item, and when. Never what is computed.

Expected gain [R]:
- It helps only if a run is tail-bound because a late or heavy item lands in a big Affordable grain.
- The report prints, per item-timed range: wall, item work, longest item, the lower bound `max(longest, work/T)`, and "finer chunking can recover at most X ms". If X is small, leave the lever off.
- [I] With the heuristic kept (the default), the lever acts only when the heuristic declines. For HAI and UpdateAI the heuristic usually triggers, so `fine_chunks_heuristic=0` is the variant that actually changes their schedule. It then loses the "Expensive first" ordering. Graham bound: `wall <= work/T + longest`.

**Worker count.** T comes from the NORMAL pool only (sec. 1.1 point 2). The engine's own `Threading.TaskThreadCount`, or cpu_sched `task_threads=N`, already sets it. The 4 BELOW_NORMAL workers on E-cores never take range chunks, so during range stages up to 4 E-cores idle unless the NORMAL pool is enlarged.
- [I] Not changed by ai_par: worker policy is cpu_sched's.
- Redirecting chunks to kind 0 would need a code patch at 0149d328 and a different T. Not done.

## 5. The plugin (`plugins/ai_par`)

Resolution is fail closed and mirrors `re_aipar_check.py`. RRP is required: if it fails, nothing is installed. Every other group (heuristic, guard, HAI, UpdateAI, trades, pop growth) is resolved independently and switched off alone. Anchors:
- strings: `RunRangeExecutionParallel`, `MaybeRunRangeExecutionParallelWithHeuristic`, `UpdatePopGrowthParallel`;
- one-pass multi-pattern scan for 10 call-site shapes (about 0.1 s);
- cross-checks:
  - HAI eval = target of the unique serial site, and the parallel site must call the same function;
  - stage 2 = the serial site's function;
  - trades eval = the site inside the parallel-for called from the function holding the unique apply site;
  - trades Execute must tail-jump to it.

All 14 hooks are `hook_safe`:
- RRP pre/post;
- heuristic post (its return value is compared with an in-plugin replica of its decision);
- guard dtor 01468be0, timed by return address;
- 00921910, 009b2930, 008dcea0;
- 01647910, 01604cc0, 01647ab0;
- 00dcedd0, 00dd26e0, 00d96a90;
- 00e5bbd0, 00e5c260.

No other plugin hooks these targets. sim_opt's raw hooks on 00dd1650 / 00e02c90 are left alone.

Report:
- triggers: console `aipar`, Game->Empty, process exit;
- output: `binaries/v3x/ai_par_report.txt`.
- Contents:
  - serial vs parallel ms per candidate task;
  - per-item p50/p90/p99/max;
  - every RRP task (runs, wall, n, T, chunks, heuristic %, expensive items per run, lever count);
  - range efficiency and recoverable time;
  - RenderFrameIfNeeded time per call site.

## 6. Not verified

- [R] Nothing was run in game:
  - hook install;
  - per-call overhead: about 3.2 M UpdateAI item calls and 0.6 M trades evaluations per 4-hour session through the hook_safe thunk;
  - the value of `[sched+0x648]` (expected 9);
  - whether the heuristic replica matches the hook every time;
  - the HAI stage split.
- [R] `fine_chunks` has not been run. It is safe by the argument in sec. 4, not by an in-game A/B test. An A/B check of the outcome would be: the same save, N days, lever off vs on, then compare the resulting saves or a game-state hash.
- [I] Whether the HAI 6 s spikes are stage 2 (expected) or one slow evaluation.
- [I] Meaning of 0x5c56249 / 0x5c79fe8 (set by RRP for the range).
- Virtual calls in 00921910 / 01604cc0 / 00dd26e0 are not followed. The RNG statements cover direct calls only, depth 5. Inside a range the RNG owner is absent anyway (sec. 1.1).
