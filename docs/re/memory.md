# Victoria 3 1.15.1: memory management RE (worker re-memory)

Binary: `binaries/victoria3.exe` (release/1.15.1, ReleaseLto). RVAs are relative to the image base 0x140000000.
Tags: **[V]** means I saw it in the disassembly or data. **[D]** means it comes from crash dumps. **[I]** means it is inferred (from mimalloc upstream source or by reasoning) and is not verified.
Helpers: `v3x/tools/re_mem_lib.py` (calls/iat/func/dis/str/callers), `re_mem_count.py` (raw E8/E9 counts, works for leaf
functions), `re_mem_calltargets.py` (call histogram), `re_mem_fnstr.py` (strings and imports per function),
`re_mem_catch.py` (FH4 catch-handler enumeration), `re_mem_dmp.py` (minidump memory summary).

## 1. Allocators in the process

| layer | what | evidence |
|---|---|---|
| engine `operator new` | **mimalloc `mi_new`, statically linked** at `0x43d95c0`. It has **15,195** direct call sites. `std::_Allocate` `0x67b430` (15,965 sites) also calls it. | [V] TLS default heap (`gs:[0x58]`, +0x38 heap, +0x44 init flag), tail-jumps to `mi_heap_alloc_new` `0x43d9540` |
| engine `operator delete` / `free` | **`mi_free` at `0x43d94b0`**: 77,494 calls + 511 jmps. It is a leaf function with no .pdata entry. | [V] masks the pointer with `0xfffffffffe000000`, which means **32 MiB segments** and 64 KiB slices: mimalloc **v2.x** |
| engine `malloc` | `mi_malloc` at `0x43d8c20`. 440 calls + 2 jmps. Returns NULL on failure. | [V] |
| aligned new | `mi_new_aligned` at `0x43d9610`. Only 4 sites, all in `pdx_task.cpp` (worker thread pool, `0x3df8a50`, `0x3df9ce0`, `0x3dfa400`). | [V] |
| UCRT internal heap | `_malloc_base` `0x4424ac0` (34 sites), `_calloc_base` `0x4423324` (51), `_free_base` `0x442339c` (280), `_realloc_base` `0x44273a0`, `_msize_base` `0x44303a0` -> HeapAlloc/HeapFree/HeapReAlloc/HeapSize on the process heap | [V] these are CRT-internal only (stdio, locale, env) |
| raw VirtualAlloc | 315 functions. **311** of them follow one "protected global" pattern: `VirtualAlloc(0, ~0x48, MEM_COMMIT|RESERVE, RW)` + VirtualProtect, with error "Couldn't allocate protected memory" (`.rdata 0x4b05140`). Example: `0x6fc470`. The other 4 are mimalloc os.c (`0x43df0b0`, `0x43df2f0`, `0x43df9f0`, `0x43dfbf0`). | [V]. The pattern costs about 1.2 MiB commit and 20 MiB of address space in total, which is negligible. |
| Paradox container allocators | `CPdxHybridInlineBufferAllocator`, `CPdxPoolAllocator`/`CPdxThreadedPoolAllocator` (`NPdxPoolAllocatorInternal`), `CPdxPreAllocatedAllocator`, `CPdxTypedInlineLinearAllocator`, `CrtAllocator`, `CHandleAllocator` (RTTI names) | [V] names. [I] they sit on top of operator new, because the engine has no other VirtualAlloc or HeapCreate user. |

There is no HeapCreate import. The exe imports no ucrtbase or vcruntime DLL, so the CRT is static [V]. No tbb, jemalloc or rpmalloc strings are present.
**Conclusion: the allocator has already been swapped.** A mimalloc-swap plugin is pointless. Section 5 lists the pitfalls in case someone revisits the idea.

### mimalloc version
The binary is **v2.0.x** (I estimate 2.0.7 to 2.0.9). This contradicts the "1.x" claim from web research. Evidence [V]:
- The 32 MiB segment mask in `mi_free`.
- The string "commit mask overflow: ..." (`0x463e950`). Segment commit masks exist only in v2.
- The option set includes `segment_decommit_delay`, `decommit_extend_delay` and `destroy_on_exit`.

## 2. mimalloc control points (RVAs)

| function / data | RVA | notes |
|---|---|---|
| `_mi_process_init` CRT initializer | `0x43de790` | [V] Its pointer is at `.rdata 0x457d670`, the last entry of the `.CRT$XI*` table `0x457d628..0x457d670`. That table runs from `_initterm_e` during CRT startup, so **after V3X's entry-point hook loads plugins**. It calls `mi_heap_main_init` `0x43ded00`, registers `atexit(mi_process_done 0x43ded80)`, calls `_mi_options_init` `0x43d9c40`, then `mi_process_init` `0x43dee20`. |
| `_mi_options_init` | `0x43d9c40` | [V] Loops over the option table and calls `mi_option_init` `0x43d9ef0` for each entry whose `init==UNINIT`. |
| `mi_option_init` (reads env) | `0x43d9ef0` | [V] Builds the name `"mimalloc_"+name` and calls `_mi_getenv` `0x43df400`, which calls **GetEnvironmentVariableA** (IAT `0x45475a0`). It also accepts legacy names. |
| `mi_option_get` / `is_enabled` | `0x43da560` | [V] 16 sites. Lazily initializes options that are still UNINIT. |
| **option table** | `.data 0x5407bb0..0x5407ef0` | [V] 26 entries of 0x20 bytes: `{long value; int init; int option; const char* name; const char* legacy;}`. Values are below. |
| `_mi_error_message` | `0x43d9a10` | [V] Prints only if `show_errors` or `verbose` is set. Then, **if `mi_error_handler` (`.data 0x5bb9520`, arg at `0x5bb9528`) is non-NULL, it calls `handler(err, arg)`**. The release build has no default abort. |
| `mi_heap_try_new` (OOM path of new) | `0x43d9390` | [V] Loop: calls `std::get_new_handler` `0x43ed638`. If a handler exists, calls it and retries. Otherwise `_mi_error_message(ENOMEM=12, "out of memory in 'new'")`, then constructs `bad_alloc` (`0x3db7a70`) and throws it with `_CxxThrowException` `0x43f102c`. |
| `std::get_new_handler` | `0x43ed638` | [V] Reads the raw global `.data 0x5c0ac48`. **Nothing in the exe writes that global**: my rip-relative scan found only this read. So the engine never installs a new_handler. |
| `mi_collect(force)` | `0x43e46b0` -> `mi_heap_collect_ex` `0x43e4390` | [V] **Only 1 call site, in `mi_process_done`.** The game never collects at runtime. |
| `mi_stats_print` | `0x43e1590` | [V] Called from `mi_process_done` when `show_stats` or `verbose` is set. |
| `_mi_malloc_generic` | `0x43dd540` | [V] Contains "unable to allocate memory (%zu bytes)". |
| large-page privilege | xref of "SeLockMemoryPrivilege" at `0x43dfb30` | [V] Uses the static imports OpenProcessToken `0x4547018`, LookupPrivilegeValueA `0x4547048` and AdjustTokenPrivileges `0x4547040`. Also resolves VirtualAlloc2 and NtAllocateVirtualMemoryEx dynamically. |

Option defaults compiled into this exe [V] (from the `.data` table):
`show_errors 0, show_stats 0, verbose 0, eager_commit 1, large_os_pages 0, reserve_huge_os_pages 0,
reserve_huge_os_pages_at -1, reserve_os_memory 0, page_reset 0, abandoned_page_decommit 0 (legacy name abandoned_page_reset),
eager_commit_delay 4, decommit_delay 25 (legacy name reset_delay), use_numa_nodes 0, limit_os_alloc 0, os_tag 100, max_errors 16,
max_warnings 16, max_segment_reclaim 8, allow_decommit 1, segment_decommit_delay 500, decommit_extend_delay 1, destroy_on_exit 0`.

### When options are read, and how a plugin sets them
- [V] Options are read **at the CRT XI initializer, or lazily on the first `mi_option_get`**, never earlier. The env lookup uses the live process environment block (GetEnvironmentVariableA).
- [V, from README/v3x.h] `v3x_plugin_load` runs before CRT startup and static constructors, so it runs **before** `_mi_options_init`. Calling `SetEnvironmentVariableA("MIMALLOC_EAGER_COMMIT","0")` there takes effect.
- [I] The exe has TLS callbacks `0x43ee5c4` and `0x43ee64c` (CRT `__dyn_tls_init`/dtor) that run before the entry point. mimalloc's thread_local uses on-demand init (`0x43ee62c`), so I expect no allocation there. Setting the variables in the proxy DllMain would be earlier still, and SetEnvironmentVariable is safe under the loader lock.
- **Preferred: write the option table directly** instead of using env vars. Env vars leak to child processes and to any other mimalloc copy in the process; per the coordinator, NVIDIA's driver reads MIMALLOC_*. The table write is targeted:
  1. `s = find_string("eager_commit")`.
  2. Scan `.data` for the 8-byte absolute pointer `s` (needs a small PE-section scan in the plugin; there is no API for it yet).
  3. The entry is at `ptr_location - 0x10`. Set `value`, and set `init = 2` (INITIALIZED [I]: mi_init_t is UNINIT=0, DEFAULTED=1, INITIALIZED=2).
  
  `.data` is writable, so no `patch()` call is needed. Anchoring by name pointer survives patches.

## 3. Lead's crash: what the dumps show [D]

There are 5 minidumps in `Documents/Paradox Interactive/Victoria 3/crashes/victoria3_01261012_*`. They contain MemoryInfoList and ProcessVmCounters streams (`re_mem_dmp.py`). All are "Abnormal Program Termination" through RaiseException, which matches an uncaught `bad_alloc` thrown by `mi_heap_try_new`. I infer the exact exception from the path; the dumps have no exception stream.

| dump | process commit (PrivateUsage) | working set | sys RAM avail | **commit avail** (meta.yml SwapSizeAvailable) | commit limit (SwapSize) |
|---|---|---|---|---|---|
| 153748 | 13,951 MiB | 11,203 | 785 | **10 MiB** | 19,943 |
| 155009 | 12,942 | 9,546 | 2,133 | **5** | 19,900 |
| 155452 | 12,894 | 6,803 | 3,888 | **6** | 19,948 |
| 160004 | 12,914 | 10,329 | 1,765 | **9** | 19,932 |
| 160422 | 13,644 | 11,634 | 624 | **20** | 19,887 |

- System commit is exhausted (5 to 20 MiB left) while physical RAM is still free (0.6 to 3.9 GB). That points to **commit-limit exhaustion, not RAM exhaustion**. The commit limit of about 19.9 GB means 16 GB RAM plus a pagefile of only about 3.8 GB.
- Committed private memory by reservation: 32 MiB-aligned reservations (consistent with mimalloc segments) hold **4.1 to 6.6 GB**, and the rest holds **7.0 to 8.5 GB**. Among the rest, 129 reservations are exactly 32.0 MiB, fully committed, with unaligned bases. They cannot be mimalloc segments, because `mi_free` requires 32 MiB alignment. [I] The most likely owner is the Intel iGPU UMD/D3D11 resource backing: GPUDedicatedMemory is 128 MB, so textures and buffers live in system memory and are charged to commit.
- 167 mimalloc-aligned 32 MiB reservations are **fully committed**. This matches `eager_commit=1` with `eager_commit_delay=4` [I upstream semantics: after a thread's first 4 segments, each new 32 MiB segment is committed completely up front].
- About 2 GB of commit is not in the working set (13.6 GB commit vs 11.6 GB working set). That is an upper bound on committed-but-untouched or paged-out memory, and it is the most that eager-commit savings can recover.

## 4. Lower-commit tuning: recommendations (ordered by effect)

1. **Not code: raise the pagefile.** Setting it to system-managed or 16 GB or more raises the commit limit by several GB. This is the only change sure to fix New Game on this machine. Also lower texture quality: the "medium" preset with render_scale 0.5 is already set, and texture streaming settings exist (`SETTING_TEXTURE_STREAMING`, `SETTING_TEXTURE_STREAMING_CACHE`). `gfx.texture_limit` is in the Vulkan render context only (`0x4aedbf0`).
2. **mimalloc options from a plugin (table write, pre-CRT):**
   - `eager_commit=0`, or a large `eager_commit_delay` such as 64. Segments then commit on demand per commit-mask chunk [I]. Expected saving: up to about 1 to 2 GB of commit [I, bounded by the dump gap above]. Cost: more VirtualAlloc(MEM_COMMIT) syscalls during heavy allocation, probably a 0 to 2% load-time cost [I].
   - `abandoned_page_decommit=1`: decommits free pages in segments left behind by exited threads [I].
   - `decommit_delay` 25 -> 5 to 10 ms, and `segment_decommit_delay` 500 -> 100 ms: free memory goes back to the OS sooner [I]. Small cost.
   - Keep `allow_decommit=1` and `page_reset=0`.
3. **Call `mi_collect(true)` (`0x43e46b0`)** at quiet points, such as after New Game or load completes, or from the OOM handler below. [I] It collects only the calling thread's heap, plus abandoned segments when called on the main thread, so the effect is partial.
4. **Large pages (`large_os_pages=1`, `reserve_huge_os_pages`): not recommended for the lead.** Large pages need the "Lock pages in memory" user right (secpol.msc, then log off and on). Without it, mimalloc's AdjustTokenPrivileges fails and it falls back to normal pages. They are non-pageable and always fully committed, so they make commit and RAM pressure worse on 16 GB with a shared-memory iGPU. Offer them only as an opt-in for 32 GB+ machines (prior art: LargePageInjectorMods). [V] for the code path existing. [I] for the effect.

## 5. OOM: where it is thrown and handled

- [V] The C++ new path always throws `std::bad_alloc` from `mi_heap_try_new` `0x43d9390` (or `0x43d9610` for aligned new). It has **no new_handler** (section 2). The `bad_alloc` constructor `0x3db7a70` is referenced at 15,854 sites; it is inlined into `std::_Allocate`-style helpers.
- [V] Catch handlers (FH4 metadata, `re_mem_catch.py`): only **3** functions catch `bad_alloc`, and all are third-party importers (Assimp Quick3D `0x3f1df20`, plus `0x3e818d0` and `0x4127680`). 11 catch `std::exception` and about 67 catch `...`. **No game or simulation code catches OOM.** A bad_alloc on New Game propagates to std::terminate and becomes "Abnormal Program Termination".
- [V] The C `malloc` path (`mi_malloc` `0x43d8c20`) returns NULL. Callers probably do not check it [I].
- Other OOM strings: "Out of memory - aborting" + "Fatal Error" in `0x4435d20` (startup helper using GetProcessHeap/HeapAlloc) [V].
- Memory telemetry: `pdxsysinfo_windows.cpp` GlobalMemoryStatusEx `0x3df6d10` -> system.log "RAM Total/Free, Swap Total/Free" (`0x378c740`). K32GetProcessMemoryInfo is used in `0x146b7a0` (gamestatetick profiling) and `0x368cef0` (automation stats). `logs/ref_lookup_memory.log` is a console dump. There is no general memory.log [V].

### Plugin plan "graceful OOM"
1. Hook `std::get_new_handler` (`0x43ed638`). Anchor: `find_string("out of memory in 'new'")` -> `find_xref(s,0)` -> `function_start` = `mi_heap_try_new`. Its first E8 call is get_new_handler. Return our handler when the original returns NULL. This covers all 15k new sites and aligned new.
   An alternative is to write our function pointer into the global at `0x5c0ac48`. That is simpler, but the address is less patch-stable.
2. At boot, commit a reserve ("ballast") with VirtualAlloc, for example 256 to 512 MiB. The handler's first call:
   - releases the reserve with VirtualFree, which frees that much commit;
   - calls `mi_collect(true)`;
   - logs size, GlobalMemoryStatusEx and process commit to v3x.log;
   - returns, so mimalloc retries.
   The handler's second call, when no reserve is left: write a clear log line and MessageBox ("Windows ran out of commit memory: enlarge the pagefile"), then return. mimalloc then throws as before, or the handler could call `ExitProcess` cleanly.
   Trade-off: the reserve itself costs commit while unused. It moves the failure later; it does not raise the ceiling.
3. Optionally hook `_mi_error_message` (`0x43d9a10`), or set `mi_error_handler` at `0x5bb9520`, to log every ENOMEM including the NULL-returning malloc path.

## 6. Allocator swap: pitfalls (only if ever reconsidered)
- The swap is redundant: everything already goes through mimalloc, and it is linked statically and inlined. 15k new sites, 77k free sites and the fast paths are inlined into callers (for example `std::string` `_Tidy_deallocate` `0x67b890` calls `0x43d94b0`), so a swap needs **hooking the mimalloc entry points themselves**. Allocations made before the plugin loads: none in exe code [I], because plugins load before the CRT starts.
- mimalloc and UCRT memory must never be mixed. CRT-internal blocks (`_malloc_base` and friends) are on the process heap. `_msize`/`_realloc_base` belong to the UCRT heap, not to mimalloc.
- Aligned new (`mi_new_aligned`) and sized delete all funnel into `mi_free`. A replacement must accept interior or aligned pointers the way mimalloc does (segment lookup by mask).
- DLLs (fmod, steam, the Intel driver) use their own CRTs and heaps. Do not free their memory with the exe allocator or the reverse.

## Not verified
- I did not run anything in-game. The effect of the option changes on commit is estimated, not measured.
- The exact upstream semantics of the 2.0.x options are taken from mimalloc source knowledge.
- I did not locate `mi_realloc`, `mi_calloc` or `mi_usable_size`.
- The owner of the 7 to 8.5 GB of non-mimalloc commit is inferred (iGPU driver).

## 7. Implementation: plugins/mem_tuner
- `plugins/mem_tuner/mt_core.h`: the pure table walk and byte patterns. `mem_tuner.c`: the plugin. `mem_tuner.ini.example`: config with built-in defaults.
- Defaults written to the option table (init=2): `eager_commit=0`, `abandoned_page_decommit=1`, `decommit_delay=10` (was 25), `segment_decommit_delay=100` (was 500), `decommit_extend_delay=0` (was 1). `large_os_pages` is opt-in through the ini.
- The plugin writes nothing if any of the 26 entries has an unexpected name or index, if there is not exactly one table, or if any entry is already initialized.
- `mi_collect(true)` runs on the main thread 30 frames after each load transition. With API v3 the trigger is the `load_transition` event that fast_load emits (fast_load owns the hook on 0x3deb0b0). Older loaders, or `own_hook=1`, use mem_tuner's own hook on that function. It also runs on the `memcollect` console command (API v2, run_on_main).
- OOM guard: hook on `std::get_new_handler` with a 128 MiB reserve, and logging through the `mi_error_handler` global.
- Offline checks:
  - `python tools/re_mem_check.py ../binaries/victoria3.exe` resolves every anchor on the real exe.
  - `test/mem_tuner_test.c` runs a fake-table unit test, plus a real-exe mapping test when given the exe path.

## 8. Diagnostic: plugins/alloc_tracker (default enabled=0)
- **Hooks**, all resolved by anchor, verified on 1.15.1 in `test/alloc_tracker_test.c`:
  - `mi_new` 0x43d95c0: "out of memory in 'new'" -> `mi_heap_try_new` <-jmp- `mi_heap_alloc_new` <-jmp- `mi_new`, with the TLS-heap prologue checked.
  - `mi_new_aligned` 0x43d9610: the function of the second xref of the same string.
  - `mi_malloc` 0x43d8c20: TLS-heap prologue, a tail jmp to `_mi_malloc_generic`, `xor r9d,r9d; xor r8d,r8d`, and `cmp rbx,400h`. The `cmp` excludes `mi_malloc_small` 0x43d8cb0.
  - `mi_free` 0x43d94b0: found by unique pattern.
- **Not hooked:** the calloc/realloc variants (0x43d8b50..0x43d8f00). They are only reachable through mimalloc's pointer table at `.data` 0x5407b50 and have 0 direct calls.
- **Recording:**
  - Blocks of 64 KiB or more are recorded exactly and kept in a live table until `mi_free`, which gives LIVE bytes per site.
  - Smaller blocks are size-weighted samples: one per 256 KiB per thread. The countdown is kept in a TEB TLS slot read with gs directly, so GetLastError is untouched.
  - Each site is the first stack frame outside thin wrappers. A wrapper is any .pdata function of 256 bytes or less that calls an allocator: 6,981 functions, including std::_Allocate 0x67b430 and 0x677e60.
- **Measured overhead** (offline, CRT stub allocator, 1,000 live big blocks): +5 to 6 ns per alloc+free pair. The real MinHook trampoline is not included.
- **Output:** console `allocs [top N]`, and `<v3x>/alloc_report.txt` every 30 s and on `load_transition`.

## 9. 4-hour session (2026-11-13, pid 8852): where the commit is, and the periodic collect (worker mem2)
Sources: `binaries/v3x/crash.prev.log` (crash_logger `mem (changed)`, 122 lines, 00:26-03:35), `devtools.log`
(last `##### devtools session ... pid 8852` block, `[memory]` every ~10 s), the last `alloc_report.txt` (#389,
03:39:52, written during Game -> Empty teardown). The session's own v3x.log was overwritten by the next start (03:53).

**Growth curve [D]** (crash_logger private, MB):

| time | private | note |
|---|---|---|
| 00:29:02 | 11,496 | load Empty -> Game done (163 s); mem_tuner mi_collect(true): 11,496 -> 11,487 MiB in 0 ms |
| 00:31:20 | 15,850 | post-load settle (game start allocations) |
| 01:31:02 | 18,072 | +2.2 GB in hour 1 |
| 02:32:23 | 19,154 | +1.1 GB in hour 2 |
| 03:33:03 | 20,271 | +1.1 GB in hour 3; 03:05-03:35 nearly flat (20,094 -> 20,301) |
| 03:40:18 | 12,605 | devtools, during Game -> Empty: committed fell 8 GB, reserved-only rose +6.2 GB (decommit, reservations kept: mimalloc-like [I]) |

Every few minutes the private bytes jump by +250..500 MB and fall back within ~5 s (transient buffers). The baseline grows
sub-linearly and flattens near 20 GB: **slow growth, not a runaway leak**. Working set stays 5.5-7.3 GB, so about 13 GB
of the commit is cold (paged out or never touched). With the 54 GB commit limit this is a pagefile cost, not an OOM risk.

**Attribution at 03:37:46 [D] unless marked** (devtools; committed private 20,193 MB, mapped 31 MB, image 367 MB, 1,692 allocations):
- Win32 heaps (process heap = CRT + DLLs): 1,034 MB committed / 886 MB allocated. Flat since 01:50. Not the growth.
- mimalloc huge blocks (32 MiB-aligned, high addresses, in the top-10 list): 4 x 120 MB, 180 MB, 524 MB.
  The 524 MB one is a growing **slot pool**: `0x39304d0` (only caller `0x391a160`, a std::string-keyed registry on the
  `pdx_gui_widget.cpp` path `0x385b910`). 776-byte (0x308) elements, free list of indices, array grows x1.5 when full:
  160 MB (00:30) -> 236 (00:33) -> 352 (01:08) -> 524 (03:00). So more than 475k live entries by 03:00, and it never
  shrinks. It is alloc_tracker's top live site (`039305ec`, 518 MiB). This is the one leak-like structure found [V growth, I purpose].
- Non-mimalloc private regions (unaligned, low addresses, no module): 171.7 MB (`8192x5461x4` RGBA-sized), 162.6, 128,
  2 x 113.5, 3 x 79 MB. The 170.7 MB ones are created and freed repeatedly (map-texture/staging sized) [I: GPU UMD / D3D11].
- alloc_tracker: 2,150 MiB of live mimalloc blocks >= 64 KiB at teardown. Small blocks are only sampled (no live count).
- **Unattributed: about 16 GB** = mimalloc blocks < 64 KiB + mimalloc free-but-committed pages + non-mimalloc private
  memory below the top-10 cut. The crash dumps (section 3) split a 13 GB process into about 50% mimalloc-aligned and 50% other [D, other session].
- mimalloc slack looked small: the forced main-thread collect after the load returned 9 MiB.
- The GPU "local" usage (2.0-3.3 GB, iGPU shared memory) fluctuates without a trend. Whether it is charged to process commit is unknown [I].

**Options check [V]:** the exe's 2.0.x table has no `purge_*` names (those are 2.1). The purge-related ones are
`page_reset` (11), `abandoned_page_decommit` (12), `decommit_delay` (15), `segment_decommit_delay` (23) and
`decommit_extend_delay` (24). `page_reset` uses MEM_RESET, which does **not** lower commit, so it stays 0.
`decommit_delay=0` (decommit immediately) is possible from the ini but is not the default, because of decommit/commit
churn at the game's allocation rate (more than 1 TB allocated per session).

**Why the collect must run on a game thread [V]:** `mi_collect` `0x43e46b0` takes the TLS default heap, and
`mi_heap_collect_ex` `0x43e4390` returns at once when heap == `_mi_heap_empty` (`cmp rcx,[rip+..]; je ret`). That is the
case for any thread that never allocated through the exe's mimalloc, such as a plugin thread. On the main thread a
non-forced collect covers the main heap, abandoned segments (`0x43da640`) and the segment cache (`0x43e36a0`).
force=1 also reclaims all abandoned segments into the main heap (`0x43da720`).

**mem_tuner section 5 (new, default on):**
- A monitor thread (below-normal priority) samples commit every 30 s.
- At +256 MiB over the lowest point since the last collect, at most every 5 min, the main thread runs
  `mi_collect(false)` in on_frame and logs `commit before -> after (+delta) in N ms; session total`.
- If a call takes more than 100 ms, the periodic collect turns itself off.
- A census line runs every 10 min, after each collect, and on the console command `memstat`. It splits private commit into:
  - mimalloc 32 MiB segments (and how many are fully committed);
  - huge segments;
  - other allocations by size bucket, which include the 128 MiB OOM reserve.
- Expected saving: small, tens to a few hundred MiB per collect [I, from the 9 MiB measurement]. The census exists so the
  next session can split the 16 GB measured, not guessed.
- Offline test: `test/mem_tuner_test.c` (trigger logic, census classification, and the plugin under a fake API on the
  real exe, where `memstat` must see a fully committed 32 MiB-aligned test reservation as a segment).

## 10. The growing 524 MiB block: the GUI animation-state pool (1.15.1, worker mem2)
**What it is [V].** It is the slot pool of the pdx_gui **animation runtime-state manager**.
- Strings in the callees: `clausewitz\pdx_gui\pdx_gui_animation_runtime_state.cpp` (`0x3913e50`, "Property cannot be a data property for animation."); `pdx_gui_animation_types.cpp` (via `0x398df90`).
- The manager is found as `widget+0xe8 -> +0xc0` (call it `M`).
- Per-widget table: `M+0x88` holds 0x70-byte records, indexed by `widget+0x368`, free list at `M+0xa0`. Each record holds a hash map from animation name (std::string, 0x30-byte nodes, slot index at +0x28) to slot index, plus a cache of 17 per-type slots at `+0x28`.
- Pool at `M+8`, layout `{elem*, int cap, int used, allocator*, mutex @+0x18, int* free_idx @+0x68, int free_n @+0x74}`, element 0x308 = 776 bytes.
- Pool insert `0x39304d0`: pops a free index, else appends; when full it allocates `max(used+1, cap*1.5)` through the allocator vtable +8, moves the elements and frees the old array. So during a regrow the old and new arrays are both live (352 + 524 MiB at 03:00).
- Create/get `0x391a160` (callers `0x385b620` x3, `0x398df90`, `0x39906a0`):
  1. `0x391b4c0` looks up (widget, animation name). If found, it reuses the slot.
  2. Otherwise it creates a slot (copying a 0x2f8-byte animation description whose name string is at element +0x2c0).
  3. It registers the slot in `M+0xb8` (hash map keyed by `0x3e806f0(name)`) and initializes it with `0x3913e50`.
- Release [V]: widget teardown `0x391ab80` (callers `0x38076e0` x3, `0x338ca30`, `0x38552f0`) walks the widget's name map and calls `0x3915340`. That removes the slot from `M+0xb8` and from the active lists (`0x3914b70`, `0x3914780`, `0x3914c10`), then pushes the index onto the pool free list (`M+0x70` = pool+0x68, under the pool mutex). It then frees the widget record and sets `widget+0x368 = -1`. **So slots are recycled when a widget is destroyed.**

**Why it grows.** The array grows only when the free list is empty and `used == cap`. Each regrow therefore marks a new
peak of *simultaneously live* states: >= 206k (00:30, 160 MiB), >= 318k (01:08, 236 MiB), >= 475k (03:00, 352 MiB),
which is about 80k more live states per hour. Two explanations fit, and the static analysis cannot tell them apart:
- widgets that are kept alive while hidden (pooled list items, cached windows) hold their states;
- widget records that are never torn down through `0x391ab80` (a real leak in the engine).

It is not a cache of tooltip strings: the key is (widget, animation name from .gui).

**Cap / eviction / shrink: not safe, not implemented.**
- A slot index is held by the widget's name map, the 17-slot cache, the `M+0xb8` map and the active-animation lists. Some readers (for example `0x393b280`) index `elem*` without the lock.
- Evicting a live slot would leave dangling indices into a recycled slot: wrong animation or a crash.
- The states are not re-creatable on demand without the widget (only `0x391a160` creates them, called by GUI code with a widget).
- Shrinking the array would require moving live slots (indices are stable handles) and would reallocate under unlocked readers.
- The savings would be bounded anyway: the waste is only the x1.5 slack, on average about 25% of the array (~130 MiB at 524 MiB). The ~400 MiB of live states would remain.

**Diagnostic in mem_tuner section 6 (`[gui_anim_pool] diag=1`, default on, changes nothing):**
- The diagnostic is a pass-through hook on `0x39304d0`, found by a unique 34-byte pattern at fn+0x58 (pop free index ; `imul rbx,rbp,0x308`). The checks are the .pdata function start and the prologue. Scanning 69 MiB of .text costs about 58 ms at load.
- The hook records up to 4 pool pointers. The census line then appends `gui anim pool N: live X of Y slots, array Z MiB`.
- The header is read with ReadProcessMemory and sanity-checked, so a freed pool is reported as `gone` and never faults.
- Next session: if `live` keeps rising during steady play with the same windows open, it is a leak in the engine. Then the next step is to RE which widget type keeps its record (count records in `M+0x88` by widget vtable).

## 11. Where the 17 GB are, and the attribution/fix tools (worker mem3, 2026-11-13)
Session: `crash3-20261113-133830` (local session folder, not in the repo) (about 10 min: load, then 7 min of play, 17.5 GB private, 0.4-2.4 GB RAM free).

**Split at 13:35:39 [D]** (mem_tuner census, private commit 17,469 MiB):

| part | MiB | what |
|---|---|---|
| mimalloc 32 MiB segments | 7,159 | 245 segments, 196 fully committed (with `eager_commit=0`, so the pages were touched) |
| mimalloc huge segments | 1,802 | 25 blocks > 16 MiB |
| other private, 16-64 MiB allocations | 5,386 | 170 allocations. In the older dump `victoria3_01261012_153748`: 133 are **exactly 32 MiB, unaligned, PAGE_READWRITE** (4,256 MiB). Not mimalloc (`mi_free` needs 32 MiB alignment), not the exe (it has no other VirtualAlloc user). |
| other private, >= 64 MiB | 1,291 | 12 allocations: the 128 MiB OOM reserve, 171.7/162.6 MB staging-sized buffers |
| other private, < 16 MiB | 1,809 | includes the process heap: 971 MB committed, 830 MB allocated (devtools) |

- Load end (13:28:09): other = 7,665 MiB, mimalloc = 4,881. In the next 7 minutes, mimalloc grew by +4.1 GB and other by +0.8 GB.
- alloc_tracker, live blocks >= 64 KiB: 3,548 MiB in total [D]. The largest sites:
  - `0x84800b`: 760 MiB, max block 233 MiB. The caller chain runs `0x3c5b134` ("Could not find texture") and `0x3c3c2b1` (gfx_effect.cpp), so this is CPU-side texture data.
  - `0x3bf85ca`: 305 MiB ("Error reading image").
  - `0x6c20d5`: 284 MiB.
  - `0xc9426e`: 283 MiB (mapgenerator.cpp GenerateLookups).
  - GUI animation pool: 230 MiB. 249k of 311k slots were live after only 7 min of play.
  - `0xcabce3`: 202 MiB (ProvinceIndex).
- So about **5.4 GB of the mimalloc commit is small blocks or free-but-committed pages**, and about **5-6 GB is non-mimalloc 32 MiB RW chunks**. Both were unattributed until now.

**What mimalloc 2.0.x does not do [V]:** a freed span is only *scheduled* for decommit (the segment decommit_mask). The decommit runs at the next free or allocation in the same segment after the delay. `mi_collect` (`0x43e4390`) has no pass over the caller's own segments: it calls page collect, abandoned collect `0x43da640` and segment cache `0x43e36a0` only. It also works only on the calling thread's heap. So the main-thread collects (5-9 MiB) never reach the worker heaps.

**New measurement (default on), `plugins/mem_tuner/mt_mi.h`.** It is shared by both plugins, and the layout was verified from the code:
- `_mi_stats_main` is at `0x5bb96c0` (size 0x240). Found through the mi_stats_print pattern; os.c updates it atomically. `committed.current` (+0x78) is mimalloc's exact commit, and `reserved` is at +0x40.
- The segment cookie key is at `0x5409628`, found through the segment-init pattern at `0x43db203`.
- The deferred-free slot is at `0x5bb9648` (arg at `0x5bb9650`). It is called inline by `_mi_malloc_generic` `0x43dd58a` and by `0x43dd260`, and has no writer in the exe.
- A segment walk reads each 32 MiB-aligned header with ReadProcessMemory. Each header must pass the cookie, slice, kind and walk checks. The walk then reports **IN USE** (page.used x block size), page slack, empty pages, committed free spans and pending decommit. It also reports totals by block size and, with `memstat threads`, by owner thread (TEB to name).
- Output goes to the census log line, to `memstat`, and to alloc_report.txt.

**alloc_tracker:**
- Sampled small blocks now stay in the live table until `mi_free`, so the report has a LIVE small estimate per site. Offline, the estimate was +2% off the true value.
- A new table groups LIVE bytes by source hint.
- `vm_owners=1` hooks ntdll `NtAllocateVirtualMemory(Ex)` (MEM_RESERVE only, 8 args forwarded). For each allocation it records the first frame outside ntdll/kernelbase/kernel32/CRT, plus the first game frame. The report splits non-mimalloc commit by module. Committed private memory with no record = mapped by the kernel (GPU allocations on the iGPU) or reserved before the hook. Unrecorded memory is also broken down by page protection.

**Fixes (mem_tuner, flags in the ini):**
1. `[thread_collect]` (default on): our callback goes into the deferred-free slot. Each thread runs `mi_collect(false)` on its own heap once per generation: every 120 s, after each load, and on `memcollect`. It runs at the start of the slow path, where the heap is consistent and the recurse flag guards re-entry. It is armed at the first frame, and turns itself off if one call takes more than 50 ms. Expected saving: the walk's "empty pages + free spans" in worker heaps; estimated at 0.1-1 GB [I].
2. `[heap_compact]` (default on): HeapCompact on all process heaps every 10 min. Expected saving: at most about 140 MB (heap committed minus allocated).
3. If the walk shows large "pending decommit", set `decommit_delay=0` in the ini. That saves the logged amount and costs more decommit/commit calls (the `commit calls` counter shows the churn).
4. The 32 MiB RW chunks, if `vm_owners` shows "no user-mode reserve" or the Intel UMD: the only lever is fewer GPU resources. That means texture quality or the texture streamer (streaming.md item 1), and lazy meshes. A mem_tuner option cannot fix it.

Offline checks: `test/mem_tuner_test.c` (parser on a synthetic header, real-exe anchors 0x5bb96c0/0x5409628/0x5bb9648, layout bytes in `mi_free` and segment init, plugin v2/v3) and `test/alloc_tracker_test.c` (small-live estimate, VM table, VM-owner attribution through the detour). Both print ALL OK.

**Next in-game run:** enable alloc_tracker (`enabled=1`) and play for 10 min, then run `memstat threads` and read alloc_report.txt. Check four things:
- (a) the "mimalloc os committed" figure vs the walk's IN USE;
- (b) the walk's slack, empty and pending figures, overall and per thread;
- (c) the VM owners modules for the 5 GB of 32 MiB chunks;
- (d) the "thread collect gen" lines, before vs after.
