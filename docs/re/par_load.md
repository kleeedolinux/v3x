# par_load: why Empty->Game is serial, and what can be parallelized or cut (Victoria 3 1.15.1)

Plugin: `plugins/par_load` (mode=measure by default). Offline test: `test/par_load_test.c`.
Profile tool: `tools/re_parload_profile.py <profile_edges.csv> --nodes <profile.csv> --root 0x6aff80 --exe ../binaries/victoria3.exe`.
**[V]** = verified in disassembly / string xrefs / an offline test. **[M]** = measured (profiles in
`D:/v3x_tmp/sessions/*/profile*_Empty_to_Game_2.csv`, fast_load timelines). **[I]** = inferred.

## 1. Where the time goes

- [M] Empty->Game 142-227 s. Process CPU about 230 % of one core: one core is the main-thread
  loading pump (fast_load: 392 M pump steps), one is game init, and the task workers are mostly idle
  (65.6 M empty polls).
  The only parallel phase is the mesh pass (IterateTypesThreaded, 10-21 s, est_parallelism 7.5).
- [V] Game init = `0x6aff80` (gameapplication.cpp, "Initializing map and databases..."). It runs on ONE
  task worker, called from the transition finalizer `0x3deb0b0 -> 0x3e3b4a0 -> 0x3e4b0e0 -> 0x3e3b6a0`.
  Call order (direct calls): tex_lookup -> gui_load -> gui_validate -> asset_load(pass 0) -> mesh pass
  `0x3ab3f10` -> asset_load(pass 1) -> db_init -> `0x324d8a0` -> achievements. RenderLoadingScreen is called 6 times in between.
- [M] Profile samples of game-init children in 3 runs (crash2 / crash / crash3). [I] "~s" = crash3 samples x
  4.45 ms. That rate is the serial wall time outside the mesh pass (148 s) divided by 33.3k non-mesh samples. It assumes the sampler covered the init thread evenly.

| step (RVA) | what it is [V] | samples | share | ~s |
|---|---|---|---|---|
| asset_load `0x3794de0` (x2) | all `.asset` files, read twice (pass flag cl=0/1). Each file: lexer `0x3e1e490`, then the streaming parse `0x3e250f0` with the assetfactory.cpp visitor `0x3793db0`. That visitor creates entities and pdxmesh types (`0x3adf6d0`, pdxmeshtype.cpp) **and loads blend shapes synchronously** | 14.9k / 17.1k / 16.4k | 31-43 % | 73 |
| of which blendshape `0x3ab67c0` | pdx_blend_shape_database.cpp `0x3ab47d0`, plus pdx_asset reads `0x3dd1190` (VFSOpen `0x3e13510` -> `0x3e1c4f0`, then binary parse `0x3dd1260`) | 11.2k / 10.4k / 11.7k | 61-75 % of asset_load | 52 |
| mesh pass `0x3ab3f10` (lazy_mesh) | already parallel | 4.9k / 20.9k / 11.2k | | 14.5 (timeline) |
| gui_load `0x39247e0` | every `.gui` file (`0x3924b10` "Loading of GUI Asset"). Templates are registered globally in file order (pdx_gui_assetfactory.cpp "Template '{}' already registered") | 5.9k / 5.8k / 5.5k | 11-17 % | 25 |
| db_init `0x6b21a0` | game databases. `0x6b2de0` builds 2 lists (`0x6b8900`: 40 string refs, map DBs such as CMapDatabase; `0x6b9950`: 122 string refs) and runs the **serial ready-loop** `0x3dbb980` (abstractgamedatabase.cpp) on each, then history `0xf858e0` | 3.9k / 3.6k / 4.9k | 7-11 % | 22 |
| render_ls `0x1021ff0` | RenderLoadingScreen. Most samples are in a kernel wait (KERNELBASE+0x68be0) | 0.8k / 2.1k / 2.3k | 2-5 % | 10 |
| achievements `0x2dd29c0`, gui_validate `0x38a3b20`, tex_lookup `0x3ad4300` | | 1.3k + 1.0k + 0.7-1.1k | ~7 % | 14 |

- [V] `0x3e250f0` is NOT a database. It is the generic streaming script-reader loop (36 callers). It
  reads a token (`0x3e227d0`, lexer core `0x3e1e9e0`) and calls `visitor->vtbl[+0x20]` per entry, then
  `[+0x28]` at the end. Its 21k samples are the visitors' work: asset factory, GUI, node graphs, script databases.
  The lexer itself is small (`0x3e1e9e0` 1.3k self, inflate `0x4228920` 0.75k).
- [V] **The game databases do not use the parallel loader.** `LoadNodesParallelFor` `0x3dc72e0` has 3
  callers: tweaker GUI `0x30c2ae0`, `0x3063090`, and an uncalled `0x2b38be0`. fast_load hooked it and saw
  "db graphs 0" in Empty->Game. The databases go through `0x3dbb980`: 0x38-byte entries
  `{+0 name ("CMapDatabase"...), +8 cb, +0x10 ready(), +0x18 unload, +0x20 load, +0x28 final|NULL}`.
  The loop takes the FIRST entry whose `ready()` is true, calls +8 then +0x20, and rescans from 0.
  The callbacks take no arguments (rcx is garbage at the call site; e.g. `0x6a9960` reads none).
- [M] Checksum (2.0-3.5 s) runs on its own thread, not on the critical path.
- [M] VFS: VFSOpen inclusive 47-55k samples in total, DIR stat `0x3e03e00` 25-26k. These profiles
  were taken without mod_loader in mode=on, so every open paid about 34 misses on 37 roots (docs/re/modloading.md).
  [I] About 80 % of the blend-shape step is VFSOpen (pdx_asset parse `0x3dd1260` is ~2k of ~11k).
  Caveat: that would be ~40 s for only ~2250 opens (section 4), about 18 ms per open. The miss model predicts about 1 ms,
  so either the opens are cold (disk / first-open cost) or the samples-to-seconds factor overstates this step. par_load measure decides.

## 2. Parallel / cut options, ranked by expected gain

1. **mod_loader mode=on** (installed by the CTO at 14:4x; my plugin-folder check at ~14:20 predated it). Its
   measure run (14:31) showed 103.8 s of thread time in VFS misses, 94 % predicted. [I] It cuts most of the
   blend-shape VFS misses. par_load's `vfs_ms` column gives the exact per-step share.
   par_load's `vfs_ms` column gives the exact per-step VFS share.
2. **Duplicate asset reads.** par_load counts distinct vs total pdx_asset paths read on the init thread.
   If many blend-shape files are read more than once (one read per referencing mesh), a per-path result cache in
   `0x3dd1190` or in `0x3ab47d0` saves the duplicate share. That needs the RE of the result object (copy/move). Not built.
3. **Parallel blend-shape prefetch** (fork/join, pure reads). The reads `path -> asset buffer` do not
   depend on each other. They could run on idle workers ahead of the parse: the path list is known from the
   previous run, and the merge consumes the results in the original order. That needs the same result-object RE as item 2. [I] Up to ~45 s.
4. **Parallel database ready-loop** (CK3-style). [I] 22 s at most, minus the critical path. Unsafe without
   per-database RE: the load callbacks parse script into shared script-system registries. They are not proven
   thread-safe and are not designed for it (no LoadNodesParallelFor). Lead-level risk. par_load measure gives the per-DB ms first.
5. **GUI** (25 s): registration order is semantic. Only the lexing could be split, and that needs the lexer ABI. Low ratio of gain to risk.
6. **RenderLoadingScreen** (~10 s, 6 calls): measure first. Skipping frames only changes the visuals, but what it waits for is not RE'd.
- Localization for unused languages: not checked (it is not in the game-init profile; startup only). No parallel script execution is proposed anywhere.

## 3. plugins/par_load (built; offline ALL OK 45/45; no in-game run)

- [V] Anchors are fail-closed and resolved without RVAs (`pl_resolve` in par_load.c). They use strings
  (ALL_SAME + prologue) or "the unique game-init callee that calls X".
  db_loop also checks 6 entry-layout byte shapes and needs exactly 2 call sites in one helper. Every
  anchor resolves to the RVA above on the real exe (test).
- mode=measure: hook_safe pre/post timing of 9 steps plus inner VFSOpen / lexer / blendshape / asset_read
  time per active init-thread step, distinct-path counting, and per-database times. The per-database times come from generated thunks
  (`pl_thunk_pool_create`, unwind info registered). They are written into the entry vector before `0x3dbb980`
  and the engine pointers are put back after it. Report: `binaries/v3x/par_load_report.txt`, console `parload`.
- mode=on: no step is proven safe yet, so it logs that and runs as measure.
  `pl_forjoin` (ordered fork/join: helpers + caller, merge in index order on the caller) is ready. The test
  shows that 30 parallel runs on 2-12 threads give a merged output byte-identical to the serial run.
- Thunk note [V]: RtlCaptureStackBackTrace stops at the thunk (it does not unwind code outside images).
  RtlVirtualUnwind (exceptions, crash_logger) passes through it (tested).

## 4. Blend shapes and the parse cache (follow-up)

**What the blend-shape step reads [V]** (`0x3ab67c0(db, base_view, name_view, entry, force)`):
- It looks the key up in the blend-shape database hash table `[db+0x98]` (0x98-byte slots, 0x3ab68b6-0x3ab6957).
  If the key is already there (and `force` == 0), nothing is read. The step is serialized by the spin lock `[db+0x108]`.
- The base mesh is read through pdx_asset `0x3dd1190` (0x3ab69e1) only if its path differs from the one
  cached in `[db]`/`[db+0x20]` (one-entry cache, 0x3ab696a-0x3ab6a57).
- Then the blend loader `0x3ab47d0` checks the file with getRealDir (`0x3dffd50`, call 0x3ab4848; this build
  returns the PhysFS DirHandle, not dirName: 0x3dffeed) and reads it through `0x3dd1190` (0x3ab4eb1).
- `0x3ab6c10` (after the mesh pass) resets that cache and compacts the database. CMeshType::Load and the
  entity parse `0x37a9520` consume the data (docs/re/lazy_mesh.md).
- asset_load pass 1 handles ONLY the top-level key 0x1b6 (`entity`; 0x3793dd2: `cmp r8d,0x1b6; sete al;
  cmp [visitor+8],al`). So blend shapes are read once (pass 0), but pass 1 re-lexes all 2059 .asset files.

**Duplicates [M, static: `tools/re_parload_blend.py`, 36 roots, current playset]:** 2116 pdxmesh, 1893 blend
refs. The engine model reads 1884 blend files (1879 distinct) plus 363 base meshes (362 distinct): 440 MB.
Only **5 + 1 duplicate reads (1.6 MB)**. A read cache keyed by path would save ~0.3 % of those reads: **not built.**
191 referenced blend files do not exist (each one is an engine "Blendshape file ... not found" log line).
The 365 base meshes (97 MB) are read again by the mesh pass. That pass is parallel, so it is off the critical path.

**Built: `blendshape_cache=1` (default 0) = OS file-cache prefetch.** It is safe because the engine is not touched.
- Every run records the blend-step reads in engine order (init thread, between blendshape pre/post).
  Base reads take the root of the next getRealDir check of the same call; DirHandle dirName = `[ret+8]`.
  The list is written to `binaries/v3x/par_load_prefetch.txt` at the first load transition.
- The next launch starts a below-normal thread at game init. It reads those native files sequentially
  (FILE_SHARE_* all, bytes discarded), at most `prefetch_window_mb` (64) ahead of the engine. The engine's
  position is the asset_read count. The thread stops when the first asset pass ends.
- The engine still opens and reads every file itself. The output is identical by construction; the offline test checks that the
  prefetcher reads exactly the file bytes in order (hash), that the files are unchanged, and that it stays within the window.
- [I] Gain: only if those reads are cold (page cache evicted under RAM pressure, or the first-open cost).
  Up to the read latency of 440 MB on one thread. Measure: the report shows hit / in flight / miss / path mismatch, plus the
  blendshape and asset_read ms. Risk: 440 MB of extra standby-cache churn in the worst case (window-limited).
  If the in-game report shows the blendshape step is not I/O-bound after mod_loader=on, keep it off.
- **Deferral to first use** (like lazy_mesh): not built. The data feeds CMeshType::Load and entity blend
  attributes (`0x37a9520`, hooked by lazy_mesh). Skipping `0x3ab67c0` would need replaying it with copied
  arguments before lazy_mesh loads the type, and before the `0x3ab6c10` compaction. That needs the lazy_mesh owner.

**Persistent parse cache (lead's question):**
- (a) [V] No self-contained parser output exists. `0x3e250f0` streams tokens into visitors (`vtbl+0x20` per
  entry) that build engine objects at once (asset factory, GUI templates, DB objects). There is no intermediate tree to cache.
- (b) Raw bytes + pre-tokenized lexer cache:
  - [M] The zlib inflate `0x4228920` is **libpng** (caller `0x438f950`: "Not enough image data"), i.e. image decoding, not script.
  - Lexer-from-file `0x3e1e490` (3.4k incl., callers asset_load 1.8k, `0x6c0800` 1.5k, gui 0.08k) is mostly file
    open/read (`0x3e13440` 2.3k). mod_loader=on removes its misses, and the OS cache serves the bytes.
  - Tokenizing (`0x3e1e9e0` 1.37k self, 4.3k incl. on all threads) is about 3-4 % of game-init samples. [I] A few seconds.
  - Replaying a cached token stream needs the reader ABI: the source object `[reader+0x28]` with refill
    `vtbl+8`, flags +0x98/+0x99, and `0x3e227d0` next-token. That is not RE'd. Gain/risk is low; not built. The cheapest variant would be
    reusing pass 0's tokens for pass 1 of asset_load.
- (c) Combined index: mod_loader (owner modload) already predicts 99.9 % of misses per root. A path -> winning-root
  table would only save the remaining hit probe order. That belongs to mod_loader (question posted in chat).

## Not verified
- No in-game run. Hook overhead (VFSOpen is hooked on all threads) is not measured.
- The ~s column (it assumes uniform sampling). The VFS share of each step. The duplicate-read ratio.
- That `0x6b2de0` runs synchronously inside `0x6b21a0` (indirect call; the profile edge suggests it does).
