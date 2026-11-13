# Mod loading (Victoria 3 1.15.1): how mods are mounted, what they cost, what breaks

Scope: the VFS search path that mods and DLC add, its per-file cost, and playset conflicts.
Tools: `tools/re_modload_vfs.py` (cost model, archiver anchor, bench, profile),
`tools/re_modload_conflicts.py` (conflict report). Plugin: `plugins/mod_loader`. Offline test:
`test/mod_loader_test.c`. Related: docs/re/vfs.md (vfs_init / MountData / mount / refresh).
"Verified" = disassembly or a measurement on this machine; "inferred" = not checked in code.

## 1. How mods are mounted

- Playset: `Documents/Paradox Interactive/Victoria 3/content_load.json` (`enabledMods[].path`, in
  launcher order). The engine prints the list it really uses in `logs/debug.log` (`Mod:` block).
- dlc.cpp 0x3b6d500 mounts one DLC/mod: folder -> 0x3e18400 -> MountData 0x3e17820; archive ->
  `mount` 0x3dff0b0 (`xor r8d,r8d` = mount point "/"), then `refresh` 0x3e16bd0 and 0x3b6d020
  (replace_path handling: string "replace_path not implemented for MSGR_DLC"). Verified (disasm).
- Every mount PREPENDS (docs/re/vfs.md), so the search order is: last mod ... first mod, DLC, game,
  jomini, clausewitz. The last mod in the playset wins a same-path file conflict. Order between DLC
  and mods: inferred from the log order (DLC list is printed and mounted first).
- A mod whose `.metadata/metadata.json` fails to parse is **silently dropped**: "cheat decrees"
  (2980847618, no `game_custom_data` member) is in content_load.json but not in the `Mod:` list and
  not mounted (debug.log `pdx_mod_metadata.cpp:168`). A version mismatch (`dlc.cpp:753`, Cheat Menu Pro
  3194757702 for 1.12) does NOT stop the mount.
- Current playset: 23 enabled, 22 mounted, 12 DLC folders -> **37 roots** (34 mod/DLC roots above game/).

## 2. File lookup cost (PhysFS 3, verified)

- VFSOpen 0x3e1c4f0 calls `PHYSFS_getRealDir` 0x3dffd50 and then `PHYSFS_openRead` 0x3e00ac0. Both walk
  the search path (`[dirhandle+0x30]` = next) and call the archiver per root: getRealDir ->
  `funcs->stat` (+0x68), openRead -> `funcs->openRead` (+0x40). No per-path cache.
- DIR archiver (PhysFS "Non-archive, direct filesystem I/O", struct rva 0x467e140, 9 slots):
  openArchive 0x3e03870, enumerate 0x3e03960, openRead 0x3e03bc0 (`mov r8d,'r'; jmp 0x3e03a90`),
  stat 0x3e03e00 -> platformStat 0x3e034d0 (GetFileAttributesExW); openRead -> createNativeIo 0x3dfc9a0 ->
  platformOpen 0x3e031e0 (CreateFileW). Opaque = root path + `\`.
- PhysFS error code = thread-local int at `[gs:[0x58]][0] + 0x5918` (written in openArchive
  `65 48 8B 04 25 58 00 00 00 BA imm32 ...`); NOT_FOUND = 0xb. PHYSFS_enumerate 0x3e00360 reads it after a
  failed stat (`cmp [r13+rcx],0xb` at 0x3e0064c) and otherwise reads the stat buffer, so a fake miss
  MUST set 0xb.
- Static model (`re_modload_vfs.py`): 32 708 unique paths; looking each up once = 1 100 365 probes, of
  which **1 067 657 are misses (32.6 per lookup)**, 1 065 776 on mod/DLC roots. Vanilla files pay 34
  misses each.
- Bench (warm cache, this machine): stat miss 8.4 us, CreateFileW miss 16.9-21.4 us, stat hit 5.6-6.7 us,
  open hit 24-35 us (first open of a file: ~4.6-4.8 ms, AV scan, inferred). One miss pair ~27-30 us ->
  ~30 s single-thread if every path is opened once.
- Profile (devtools CSV, Empty->Game, 1 kHz x 4 threads): getRealDir incl 23-26k samples, openRead incl
  33-36k, VFSOpen incl 47-49k = **10.9-11.8 % of all samples**; CreateFileW 31-35k, GetFileAttributesExW
  24-27k. The misses dominate (34 misses x ~17 us vs 1 hit x ~24 us per open). Wall-clock share is not
  measured (lookups run on several loader threads).
- Enumeration (PHYSFS_enumerate) is small: ~0.5 % of samples. Overridden files are not parsed twice
  (only the winner is opened); duplicate work comes from duplicate KEYS in different file names (sec. 4).
- Checksum: the manifest (game/checksum_manifest.txt) hashes common/events/map_data/gui/localization
  through the VFS, so it pays the same per-file probes (fast_load: checksum 2.3-15.7 s per load).

## 3. plugins/mod_loader

- Hooks DIR stat and DIR openRead (MinHook, 8 args forwarded), found via the archiver struct: exactly
  one .rdata pointer to the description, version 0, extension "", author string, supportsSymlinks 1,
  9 slots in .text, openRead thunk bytes, stat prologue (`resolve()` in mod_loader.c). Any mismatch =
  plugin inactive. Offline: `mlt.exe ../binaries/victoria3.exe` -> stat 0x3e03e00, openRead 0x3e03bc0,
  err_off 0x5918.
- Per root (workshop + DLC by default, local mods off): file index built once on a thread-pool thread
  (FindFirstFileExW, files + parent dirs, ASCII lower-case). Any enumeration error = not indexed.
- `mode=measure` (default): no behaviour change; each probe on an indexed root records the index
  prediction and checks it against the engine's answer (mismatches listed in the report).
- `mode=on`: predicted misses return 0 / NULL with error 0xb, no syscall. Every `verify_every`-th skip
  (64) is re-checked; one mismatch = cache off for the session. Refused (falls back to measure) when the
  error slot is not found. Names Windows can alias are never predicted (`ml_norm`: `~` 8.3 names, trailing
  dot/space, non-ASCII, devices, `:` streams); the offline test shows a real `COMMON~1` alias.
- Not handled: a file created in an indexed mod/DLC folder while the game runs is invisible in
  mode=on until restart (verify sampling catches it, then turns the cache off). Workshop updates while the
  game runs would hit this.
- `skip_mods=` / `skip_fatal=1` (off): the root answers "not found" to everything (mod has no files).
  skip_fatal uses the previous run's `logs/error.log` parse errors (`pdx_persistent_reader`, `Expected '`),
  attributed to the highest-priority mod providing the file. DLC roots are never hidden.
- Report `binaries/v3x/mod_report.txt` (after each load transition, `modload` command): playset vs mounted
  roots (NOT MOUNTED detection), per-root probes / misses / ms in misses / predicted-skippable ms /
  mismatches, mod>mod overrides, vanilla and DLC files a mod replaces, localization .yml without BOM,
  previous-run errors per mod.

## 4. Breakage in the current playset (`re_modload_conflicts.py --errlog error.1.log`)

- Not mounted: 2980847618 "cheat decrees" (metadata). Duplicate mod: two "Cheat Menu Pro" (3717461054 and
  3194757702 for 1.12); the 1.12 one is later and wins 2 files.
- 53 whole-file replaces of vanilla/DLC files (Historical AI Mod 25, Community Mod Framework 15, ...).
- 162 duplicate top-level keys (140 with a mod), 147 INJECT/REPLACE patches. Example: Get Achievements
  redefines achievements already in vanilla; engine log: `Duplicated key peccavi will not be created`.
- Unbalanced braces (rest of file dropped): Cheat Menu Pro 98_sakuya_diplomatic_actions.txt,
  sakuya_force_sgui.txt; Super Assimilation / Super Migration decree files.
- error.1.log: 39 errors from Cheat Menu Pro 3194757702, 12 from Get Achievements, 5 from Auto-Diplomacy.
- Heuristics: per-folder file-name order, replace_path = exact folder, some folders skipped
  (defines, on_actions, history, scripted_lists, script_values).

## Not verified

- No in-game run of mod_loader (measure or on); the real number of probes and the wall-clock gain.
- DLC vs mod mount order and replace_path semantics in code (0x3b6d020 not disassembled in detail).
- That the engine never relies on GetLastError after a DIR miss (only the PhysFS code is set).
