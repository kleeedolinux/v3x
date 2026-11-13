# RE: game loading and asset streaming (Victoria 3 1.15.1)

Binary: `binaries/victoria3.exe`, SCMCommit 7a630cdf. RVAs below are relative to the image base, found
with `tools/re_mem_lib.py` (`str`, `callers`, `dis`). "Verified" = string xref found in the binary.
"Inferred" = deduced from log timing, not from code.

## 1. Measured timeline (lead's machine, modded, from `tools/re_load_timeline.py`)

| milestone | run 15:55 | run 16:00 | source |
|---|---|---|---|
| checksum calculation start | +4s | +3s | log |
| localization parse | +5..11s | +3..8s | log span (inferred) |
| ThreadedInitTextureLookup (5483 files) | +14..15s | +10..11s | log |
| first mesh "Total time" line | +136s | +114s | log |
| script database parse | +144..197s | +120..164s | log span (inferred) |
| Transition Empty->Game | 204.9s | 171.7s | log, exact |
| meshes timed / summed ms | 2118 / 113.1s | 2118 / 92.2s | log |

Findings:
- The biggest block is +21s .. +114/136s (~95-115 s) before the first mesh summary line. Mesh type loading
  (`IterateTypesThreaded`) is the best candidate for it (inferred: the per-mesh times are summed over
  worker threads, so 92-113 s of CPU is not the same as wall time).
- Script database parse is ~45-53 s wall.
- Checksum, localization and the texture lookup together are under ~10 s.
- Hardware matters as much as code: crash `meta.yml` files show `SystemMemory 16108`, `SystemMemoryAvailable`
  3888 / 1765 / 624 MB, and `GPUDedicatedMemory 128` (integrated GPU). The two runs differ by 33 s with the same
  mods, which points to paging / RAM pressure. 3 crashes ("Abnormal Program Termination") happened while free RAM was low.
- Asset size (`tools/re_load_assets.py game/gfx`): 9.9 GB / 19792 files; models 3.9 GB, .dds 7.2 GB, .mesh 963 MB / 3511 files.

## 2. Anchors (verified string xrefs)

| RVA (fn) | anchor string | role |
|---|---|---|
| 0377b120 | `Starting checksum calculation` (04a953f8) | starts the checksum; callers 036b90c0, 0377bc30 |
| 0377b5f0 | `Feeding into checksum: {}` (04a95590) | hashes each file; caller 0377ba80 |
| 006b7a00 | `checksum_manifest.txt`, `[Checksum Builder]` | builds the manifest / prints the HEX checksum |
| 03ae4d80 | `IterateTypesThreaded`, `Total time "{}" in {} ms` (04ad15f0) | loads mesh types; caller 03ab3f10 |
| 03acc870 | `ThreadedInitTextureLookup found {} files` (04ad1120) | texture path lookup |
| 03dc72e0 | `LoadNodesParallelFor`, `LoadNodesCreateTask` | parallel load of the database dependency graph |
| 03dc7640 | `Database Node Init Time: {} - {} ms - {} ms including dependencies` | per-database timing; callers 02b38be0, 03063090, 030c2ae0 |
| 03dc7150 | `LoadNodeNotifyDependentsThreaded` | database graph scheduling |
| 03deb0b0 | `Transition {} took: {} seconds` (04b03cb0) | timer for the state transition |
| 03e12240 | `Done enumerating '{}'(...) in {:.3f} seconds` | VFS enumeration timer |

## 3. fast_load plugin plan (hooks in priority order)

1. **Measurement first: 03dc7640 + 03deb0b0.** Hook both entries/exits and log QPC timestamps to `v3x.log`.
   This turns the inferred spans above into exact per-database and per-transition times. No behaviour change.
   Gain: 0 s, but it is a prerequisite for every other hook.
2. **Mesh type load, 03ae4d80 (caller 03ab3f10).** Option A: log per-mesh times and widen the thread count if the
   task scheduler is under-subscribed. Option B: lazy loading (register the type, load the `.mesh` on first use).
   Upper-bound gain: most of the ~95-115 s block. Risk: high for B, because portraits and the map expect the meshes to be resident.
   Start with A.
3. **Checksum, 0377b120 / 0377b5f0.** Cache the per-file hash keyed by (path, size, mtime) and skip unchanged files.
   Gain: a few seconds at most (the exact duration is not measured yet). Lead-level call: the checksum drives
   multiplayer/ironman compatibility, so the cached result must be byte-identical.
4. **Database graph, 03dc72e0.** Only after hook 1 shows which databases dominate the ~45-53 s parse. Possible fix:
   reorder or split the long ones across more tasks.
5. **Texture streaming / RAM (03acc870 is not a bottleneck: about 1 s).** With 128 MB VRAM and <4 GB free RAM, the
   cheapest win is not a hook. Measure again with RAM freed and with lower texture quality. Only add a hook if that confirms paging.

Measurement protocol: same modset, same machine, close other apps, run 3 cold + 3 warm starts. Run
`python tools/re_load_timeline.py "<Documents>/Paradox Interactive/Victoria 3/logs"` and compare
`Transition Empty->Game` and the mesh/database offsets before and after each hook.

## 4. fast_load plugin (built; offline-verified, no in-game run yet)

`plugins/fast_load/fast_load.c`. Each anchor is found by its string and byte-checked (`ANCHORS[]`, mirrored
in `tools/re_load_anchors.py`). The offline check against the real exe:
`python tools/re_load_anchors.py ../binaries/victoria3.exe` or `test/fast_load_test.c <exe>`.
Output goes to `binaries/v3x/load_timeline.txt` plus one summary line per transition in v3x.log. The plugin
also emits the `load_transition` event (API v3) with the engine's own seconds value, read from `[[task+0x150]+0x128]`.

**Mesh thread setting (verified in disasm, 03ae4d80):**
- The loader reads `r12d = [[05c726e8]+0x648]` at fn+0x41 and clamps it to 2..15.
- The parallel-for `IterateTypesThreaded` caps its worker count at `min(r12d, setting+1)` (fn+0x3b9).
- Each item runs through the vcall thunk 02322020 (`mov rax,[rcx]; jmp [rax+0x30]`). fast_load hooks that thunk
  entry-only to record which threads run items (`est_parallelism` in the timeline).
- `mesh_threads=N` (2..15, default 0 = off) in `fast_load.ini` writes N into the setting for the duration of
  the mesh loader call and then restores the old value. The same setting may be used elsewhere.

**Checksum (verified in disasm), cache NOT built:**
- 0377b5f0 feeds a fixed blob, the game version and then the manifest (`checksum_manifest.txt`) through
  03dbe9d0 (pdx_core/checksum.cpp).
- 03dbe9d0 hashes each manifest entry in tasks (03df8ee0 / 03e45e00) into separate "Name" and "Contents" MD5s.
  The contents MD5 is one running stream over the entry's files. The hex digests are then fed into the master hash (03dce0b0).
- A per-file (path, size, mtime) cache therefore cannot reproduce the digest. A correct cache would have to be
  per manifest entry (key = the entry's whole file list) and needs more RE of the per-entry task.
- Decision (CTO): only pursue it if the in-game `cs_feed` timing exceeds 2 s. `checksum_cache=1` is accepted but fails closed.

**Database nodes (03dc7640, from disasm):**
- graph = `[ctx+8]`, nodes `[graph+0x70]`, count `[graph+0x7c]`.
- Each node has its name `{ptr,len}` at +0x08 and its own time in ms at +0xa0. Its dependency list has the sentinel at +0xb0 and the first element at +0xc0; each element has `next` at +0x10 and the dependency at +0x28.
- The engine measures every node itself but logs it with a filtered category, so the times never reach debug.log.
  fast_load copies them into the timeline.

## Not verified
- That the +21..114 s block is mesh loading (no wall-clock instrumentation yet).
- How long the checksum takes, and whether it runs synchronously.
- The meaning of function 03ab3f10 beyond "calls the mesh loader".
