# Victoria 3 threading: RE notes (pdx_task scheduler)

Binary: `binaries/victoria3.exe`, ImageBase 0x140000000, PE TimeDateStamp 0x6af3094f.
All addresses are RVAs. Tools: `v3x/tools/re_threads_{fn,callers,iat,raw,imm}.py`.
Status: static analysis only. Nothing here has been checked at runtime.

## 1. Facts
- The game never sets thread affinity. It does not import SetThreadAffinityMask, SetThreadGroupAffinity,
  SetThreadIdealProcessor(Ex), SetThreadSelectedCpuSets or GetSystemCpuSetInformation, and there is no
  string for any of them that GetProcAddress could use (`re_threads_iat.py` + strings dump). The only
  scheduling control it uses is SetThreadPriority.
- Topology is not P/E aware. `0x03df6030` (pdxsysinfo_windows.cpp) calls `GetLogicalProcessorInformation`
  (not the Ex version, so it never reads EfficiencyClass) and counts RelationProcessorCore entries
  (physical cores, P and E counted the same).
- The worker count comes from logical CPUs. `0x043e9bd0` = GetNativeSystemInfo().dwNumberOfProcessors.

## 2. Scheduler (pdx_task.cpp)
| RVA | Role | Anchor |
|---|---|---|
| 0x03df8a50 | Task scheduler init. ecx = Threading.TaskThreadCount (-1 = auto). Called from 0x00678f90 at 0x00679a79 | "Spawning {} normal worker threads" (0x04b044f0), "Spawning {} below normal worker threads" (0x04b04518) |
| 0x03df9510 | Scheduler ctor, called at 0x03df8d26 | caller site |
| 0x03df9dc0 | Spawn N workers of one kind (rcx=sched, edx=N, r8d=kind). Thread names come from the format "{}_{} {}" | 0x04b031b8 |
| 0x03df9bb0 | Spawn a single worker of a kind (callers 0x03df9510, 0x03c3fdc0) | |
| 0x03dfa400 | Add one worker: allocates a 0x8180-byte thread object, stores it at pool+0x88+idx*8, caps the pool at 64 | `cmp r13d,0x40` at 0x03dfa45a |
| 0x03dfaca0 | Generic thread start wrapper (_beginthreadex 0x0440b254). Also used by ElasticLogging, texture streamer, and others | callers in c.txt |
| 0x03df9180 | **Worker thread entry** (rcx = thread object) | passed as `lea rcx,[rip-0x14d3]` at 0x03dfa64c |
| 0x03dfac40 | Priority by kind: 0 -> -1 (BELOW_NORMAL), 1 -> 0 (NORMAL), 2 -> +1 (ABOVE_NORMAL) | jmp [SetThreadPriority] x3 |
| 0x03df8710 | Worker "run work for slot" step (callers: 0x03df9259 worker loop, 0x03dfa3a5 helper) | |
| 0x03e38df0 | Worker idle wait (event at thread+0x100) | |
| 0x03dfae40 | Console re-spawn path | "normal priority {}, below normal priority {}", "Use Threading.TaskThreadCount [count]." |

Pool offsets inside the scheduler (from 0x03dfa42c and 0x03df922a): kind 0 -> +0x680, kind 1 -> +0x3c0, kind 2 -> +0x100.
The live count sits at pool+0x288. The per-slot queue array is at [sched+poolOff], 0xa0 bytes per slot.

Thread object (from 0x03dfa59c-0x03dfa5c7 and 0x03df9199-0x03df91bd): +0x00 sched, +0x10..0x12 params (byte +0x10 = dedicated single-task mode -> 0x03dfa750),
+0x13 state (2 = stop, 3 = exited), +0x38 **slot index**, +0x3c **kind**, +0x48 busy-time accumulator, +0x100 wake event.
Task-thread TLS (TEB gs:[0x58] -> slot0, offset 0x6170): byte isTaskThread, +4 index, +8 thread object (written at 0x03df91ae-0x03df91b8).
The asserts "Task thread has invalid thread index" / "AssociateMainThread" (0x046cd6d8 / 0x046cd700) read this TLS.

Thread-count formula at 0x03df8a50-0x03df8b28 (constants 0.8 @0x04b07468, 0.4 @0x04b07424):
N = max(hc,4)-1 (auto). normal = ceil(N*0.8), below = floor(N*0.4), each clamped to [4, min(max(hc,4),64)].
It then spawns normal-2 of kind 1 (0x03df8d2c-0x03df8d43) and below-2 of kind 0 (0x03df8d48-0x03df8d5b).
Example: 32 logical CPUs gives 23 NORMAL + 10 BELOW_NORMAL workers, plus the main thread and the kind-2 thread(s). That is more threads than CPUs,
with no affinity, so on hybrid CPUs Windows is free to put sim workers on E-cores.

## 3. Fork/join (game-side parallel range)
| RVA | Role | Anchor |
|---|---|---|
| 0x0146c960 | RunRangeExecutionParallel (gamestatetick.cpp:1193 = 0x4a9) | "RunRangeExecutionParallel" xref 0x0146ccc9 |
| 0x0146c010 | MaybeRunRangeExecutionParallelWithHeuristic (cost-split path) | xrefs 0x0146c57b / 0x0146c6a0 |
| 0x0149d260 | **Fork**: builds edx=N chunk tasks into the job at rcx and enqueues them | call at 0x0146cd8c |
| job vtbl slot +0x8 / +0x10 | **Join**: back-to-front loop over job tasks calling [task->vtbl+8] (wait/help), then [+0x10] (release) | 0x0146cdb0-0x0146cdf0 |
| 0x007a0410 | Clear the job's task-ref vector (not the wait) | 0x0146cdf9 |
| 0x03df8f50 | Job dtor, asserts all tasks are done (507 callers = number of job sites) | "Every task should be done before the job is destroyed, did you forget to call WaitAndClear on the job?" (0x04b045f0) |
| 0x03dfa750 / 0x03dfa270 | Help-while-waiting: pops (0x03df8850) and runs (0x03df8710) other tasks while blocked | |

Chunking: chunks = min(ceil(range/grain), maxThreads) at 0x0146ccdf-0x0146ccf2. With 1 chunk (or mode 3) it calls the functor inline at 0x0146ce2a.

## 4. Hook targets (recommended)
1. **P-core affinity: hook the worker entry 0x03df9180.** Prologue `48 89 5c 24 10 48 89 6c 24 18 56 57 41 54` (14 bytes, no RIP-relative operands, so a 14-byte abs jmp can be relocated safely).
   In the detour, read kind=[rcx+0x3c] and idx=[rcx+0x38], call SetThreadSelectedCpuSets/SetThreadGroupAffinity (kind 1 -> P-cores, kind 0 -> E-cores or any), then tail-call the trampoline. This catches every task worker and no other thread.
   Alternative: hook 0x03dfac40 (priority setter, ecx=kind, runs on the worker thread). The prologue is `48 83 ec 28 85 c9 74 39 ...`, but the `74 39` jcc is RIP-relative and needs fixup. Smaller, but riskier to relocate.
2. **Thread count: hook 0x03df8a50** (ecx = requested count). Pass the P-core count instead of -1, or patch the 0.8/0.4 floats at 0x04b07468/0x04b07424 (.rdata, needs VirtualProtect). Prologue `40 55 53 56 57 41 56 41 57 48 8d 6c 24 d1` (14 bytes, no RIP-relative).
   Main thread: the scheduler init runs on it (from 0x00678f90). Pin it in that same detour.
3. **Fork/join timing: hook 0x0149d260** (fork, prologue `48 89 5c 24 08 48 89 74 24 10 48 89 7c 24 20`) and time until 0x03df8f50 (job dtor, which runs right after the join) to measure per-ParallelRange wall time.
   The 0x03df8f50 hook covers all 507 job sites.

## 5. Not verified / open
- The generic task enqueue (push to slot queue plus wake) is not identified. 0x03e4cf50 / 0x03e4d0b0 (~487 callers each) look like task-handle helpers, not the push.
- The exact meaning of the "-2" in the spawn counts (reserved main + kind-2?) is a hypothesis.
- What the kind-2 (ABOVE_NORMAL) thread at 0x03df9bb0 is used for.
- None of the hooks have been tested at runtime.
