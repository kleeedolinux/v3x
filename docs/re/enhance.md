# Engine enhancements: saves, game speed, debug_mode, ideas (worker enhance)

Binary: `binaries/victoria3.exe` 1.15.1 (release/1.15.1, 7a630cdf29e). All addresses are **RVAs**.
Plugin: `plugins/enhance/enhance.c`, config `v3x/enhance.ini` (`plugins/enhance/enhance.ini.example`).
Offline anchor check: `test/enhance_test.c` (36 checks, most against the real exe; build/run lines in its header).
Debug-flag reader list: `tools/re_enh_debug.py`.
Tool: `tools/re_enh_lib.py <exe> "cmd args" ...` (annotated `dis`, `str`, `rva`, `callers`, `strs`, `fn`,
`q8`, `ptr` = absolute pointers in .rdata/.data, `anyref` = any rip-relative operand, e.g. byte flags).
It reuses `re_sim_lib.Img` with its own index cache in the temp directory (`RE_SIM_CACHE`).

## 1. Autosave freeze

### Measured (lead's session, 2026-11-13, binaries/v3x + Documents/Paradox Interactive/Victoria 3)

* Every autosave is **156 MB of uncompressed text** (`save games/autosave*.v3`, header `SAV0100`).
* Each save matches a **~3.2 s main-thread frame** in `v3x.log` (frame_pacer `max` per 10 s window)
  and a **+430 MB** transient private-memory jump (crash_logger), for example:

  | save file mtime | frame_pacer window end | max frame |
  |---|---|---|
  | autosave_4 03:27:44 | 03:27:47 | 3204 ms |
  | autosave_3 03:28:30 | 03:28:30 | 3212 ms |
  | autosave_2 03:30:32 | 03:30:32 | 3319 ms |
  | autosave_1 03:31:56 | 03:32:02 | 3123 ms |
  | autosave 03:32:58 | 03:33:02 | 3220 ms |
  | autosave_exit 03:39:04 | 03:39:03 | 3483 ms |

  The whole session has 129 windows with a frame over 2 s (median 2.96 s).
* devtools report #1020 (the 10.8 s window around the 03:27:44 save): `ZwWriteFile` is only 156
  samples (~0.16 s). The freeze is serialization, not disk I/O.
* **Measured after the fix** (session 13:25, crash3 folder): binary saves are **26 MB** instead of 156 MB,
  but `save_timing` logged **3197 ms and 3607 ms**. So binary_saves_in_debug shrinks the files ~6x but does
  **not** shorten the freeze. Serialization dominates in both formats.
  The second save (13:36:40) failed with `Failed to write to disk` (CPdxFileException, virtualfilesystem.cpp:798):
  `autosave.v3.tmp` stayed at 0 bytes. C: had about 2.7 GB free afterwards, and the commit limit grew from 36955 to
  41091 MB during that session (pagefile growth), so the disk was probably full. The engine's tmp + rename kept
  the earlier autosaves intact.

### Why the saves are text: debug_mode

`console_history.txt` starts with `debug_mode`. The debug flag (byte **05c0efe7**) is set by the
`debug_mode` launch argument (03dc04ec, in 03dc03c0 after the `"debug_mode"` xref 03dc04dc). The console
command toggles it (`sete` at 036b9271 in 036b90c0, `"Turning debug ON"`). Every save path picks the format like this:

| site | function | code | without debug | with debug |
|---|---|---|---|---|
| 00ca96af | 00ca9580 CAutoSaveCommand (calls 03663630, rotation `{}_{}{}`) | `mov r9d,3; cmp [D],al; cmovne r9d,eax` (cmovne at **00ca96bb**) | 3 | 0 |
| 036628a3 | 03662820 save by name (callers: console `save` 008b7be0 `Saved game as "%s"`, 00f8daf0 `ingameidler.cpp:782`, probably the exit save, not confirmed; 032ef430) | same, r8d (cmovne at **036628af**) | 3 | 0 |
| 02711189 | 02711100 save dialog command | `movzx eax,[D]; test al,al; je` (je at **02711192**) | setting `save_file_format` (or 5) | 0 |

All three call **03662910 SaveGameState** (`"SaveGameState"` 04a87b20, savegamemanagerbase.cpp) with
(this, name, format, flag). Format = header kind byte (header writer 0365fcc0, `"%.3s%.2x%.2x%.16d"`):
0 = uncompressed text (`SAV0100`), 3 = zipped binary (`SAV0103`, release default), 4/5 = the setting
options `zip_text_all` / `zip_binary_all` (setting table 00468630, registration 00468780 `save_file_format`
in category `game`). In 0365fcc0: format <2 goes to 03660628 (uncompressed), 2-3 to 036601b2, 4-5 to 0365fe29.

**Implemented: `binary_saves_in_debug=1`** NOPs the two cmovne and turns the je into jmp. Saves are then what a
non-debug game writes. The write path itself (temp file, rename, cloud, continue_game.json) is untouched.
Fail-closed: D must be found through both writers, exactly 2 + 1 sites must reference D, and the register pairs
must match.

### Compression

Zip writing: 03b82910 → 03b81590 `WriteDeflateCompressed` (pdx_zip_impl.h). It already uses 1 MB chunks over
min(hardware threads, size in MB) jobs. Each job runs deflateInit2_ in **03b812c0** (level = ecx; method 8,
windowBits -15, memLevel 9, `"1.2.11"`). The level comes from a per-format table in the save manager (pointer
passed to 036630e0: `rbx + (format+0x12)*8`). **Implemented: `deflate_level=1..9`** hooks 03b812c0 and replaces
the level (0 = untouched). The engine level is logged on the first save.

### Background-thread write: not done

* There is no self-contained buffer handed to a writer. 0365fcc0 writes the header, `meta` and the zip
  entries through the output stream object (vtable +0x50 = Write; further virtual calls at +0x60/+0x68/+0x88/+0x90,
  not identified), interleaved with serialization (0369e0d0 → task system 03e25080, `WriteArrayThreaded` 032ded40).
  Making it asynchronous means replacing the stream object and guaranteeing completion before the next save,
  load-game and exit, plus keeping the `continue_game.temp` → `.json` rename (03660830) consistent.
* The measured write share is ~0.16 s of 3.2 s. Verdict: skipped.

## 2. Game speed

* **SetGameSpeed 01465270** (`"Changing game speed to: {}"`): clamps to 0..4 and stores at gamestate+0x70.
  Gamestate getter: 01466800. Callers: CIngameIdlerLogic message handler 00f8b320 (+/-, set), save loader 01462d40,
  console `SET_GAME_SPEED` 00ca91a0.
* **Tick issuer 00f8d340** runs once per main-thread frame, at the end of CIngameIdlerLogic::Update 00f8bfc0
  (00f8c2fe). It accumulates frame time and compares it with a stack table built from **04b08610**
  (2.0 1.0 0.5 0.25) + 04b07338 (0.0001, shared constant). If no tick is in flight (idler+0xdf), it posts one
  **CPartialDailyTickCommand** (vtable 046f4dd8, Execute 00ca8690 → GameState tick 01465690). The command runs
  on the Network/Session thread. Its last statement sends message 0x5b03 (00ca8a42), whose handler clears
  idler+0xdf (00f8bebf).
* tick_profile.csv: 12816 TICK-rate calls and 3204 DAY-rate calls, so **4 ticks = 1 day**. Speed 5 =
  0.0001 s per tick = already unthrottled. **Speed 6/7 cannot be faster through the interval.**

**Implemented: `speed_table=1`** with `speed1..speed4` patches the 16-byte .rdata table 04b08610. It is read by
exactly one instruction (00f8d41a), which a full .text scan verifies, and its vanilla values are checked. Console
`speed` / `speed <1-4> <s>` (written on the main thread).

## 4. What debug_mode turns on: every reader of the flag (05c0efe7)

`tools/re_enh_debug.py <exe> 5c0efe7 <profile.csv>` lists all 37 rip-relative references (35 reads, 2 writes)
with functions, strings and session-profile samples (`profile_Game_to_Empty_2.csv`, devtools 1 kHz, debug_mode
was on). The console `debug_mode` command toggles only this byte. The `-debug_mode` launch argument also sets
05c0efbf (checksum, 1 reader) and 05c0efe6 (= filewatcher, also set by `-filewatcher`; read every frame in the
main loop 0377cace). It is not known whether the lead uses the launch argument.

| read at | function | what it gates (from strings/code) | profile incl. samples | lean_debug |
|---|---|---|---|---|
| 0087e684 | 0087e670 | console-blocked predicate: debug = console allowed | 0 | keep (needed) |
| 036bd715 | 036bd700 | console window may open (`console_edit`) | 0 | keep (needed) |
| 036b8aae | 036b8a20 | `reload` command allowed | 0 | keep |
| 036b90e3, 036b9271 (write) | 036b90c0 | the `debug_mode` command itself | 0 | keep |
| 03dc04ec (write) | 03dc03c0 | `-debug_mode` launch argument | 0 | keep |
| 00ca96b5, 036628a9, 02711189 | save paths | text save format | (see 1.) | binary_saves_in_debug |
| 036bc320 | (leaf, via `_Func_impl` 04a91db0, CApplicationUtilsContext lambda) | GUI function **InDebugMode**: 25 `InDebugMode` uses in game/gui `visible =` conditions (error_deer.gui = error lion + FPS line, debug buttons in front/market/states/military/ship panels, tooltip.gui, custom_tooltip.gui, eventwindow.gui) | not attributable (GUI) | **lean_gui** |
| 02503115 | 02502eb0 | **bypasses a cache**: with an unchanged key, vanilla reuses the text built by 02373fb0 (MILITARY_THEATER_DEBUG_TOOLTIP...); debug rebuilds it every call | 02502eb0 71046, 02373fb0 14122 (all builder calls: at most ~14 s in 3 h is rebuild cost) | **lean_theater_cache** |
| 01a3f9fa | 01a3f9c0 | DEBUG_JOURNAL_ENTRY_ID | 0 | **lean_tooltips** |
| 01b1fb91 | 01b1fb60 | script value formatting (NEUTRAL_SCRIPT_VALUE_FORMAT) | 0 | lean_tooltips |
| 01d54fc0 | 01d54f70 | DEBUG_BUILDING_TOOLTIP | 0 | lean_tooltips |
| 01dd5435 | 01dd5220 | PLAYED_BY_PLAYER/AI_DESC | 0 | lean_tooltips |
| 0201c1b2 | 0201c160 | EVENT_DEBUG_TOOLTIP | 0 | lean_tooltips |
| 02059e15 | 02059de0 | POP_DEBUG_INFO | 0 | lean_tooltips |
| 023740e5 | 02373fb0 | MILITARY_THEATER_DEBUG_(ENTRY_)TOOLTIP | (see cache) | lean_tooltips |
| 0355d2f6, 0355d618 | 0355d0e0 | localization: JOMINI_DEBUG_FORMAT | 0 | lean_tooltips |
| 035a644d, 035a674b | 035a61e0 | localization: JOMINI_DEBUG_FORMAT | 0 | lean_tooltips |
| 035a699b | 035a6900 | localization: JOMINI_DEBUG_FORMAT | 0 | lean_tooltips |
| 00694daa, 0069da9a, 0069f8fa, 006a3e3a | game_telemetry.cpp | telemetry (debug changes what is sent) | 74 / 0 / 0 / 0 | keep |
| 006b19d1 | 006aff80 | startup / DLC setup | 0 | keep |
| 0102763a | 01027610 | map load checks (province pixel counts), load only | 0 | keep |
| 028115e3 | 02811550 | MATERIAL_DEBUG view (console command only) | 0 | keep |
| 0377b633 | 0377b5f0 | checksum (`Feeding into checksum`) | 0 | keep (do not touch) |
| 0281cef5 | 0281cd20 | per-frame loop over objects: items with flag bit 0x20 are also taken in debug | self 937, incl 2645 | keep (not identified) |
| 0237b232 | 0237b210 | early-out `je` when debug off (caller 0237a690) | 0 | keep (not identified) |
| 00a4a5ae | 00a4a5a0 | debug-only branch, callers 00adb210 / 00adce20 (Logic access) | 0 | keep (not identified) |
| 032ef85e | 032ef770 | clears a setting bit (+0x111) while debug | 6 | keep (not identified) |
| 027693a0 | (leaf, 6 GUI trigger vtables: CHasContainerTags/Name, CHasAnyTags) | debug-only trigger result | 0 | keep |

**Map modes and decrees:** no reader of the flag is in map-mode or decree code, so debug_mode does not explain
the map-mode lag by itself (shared with worker mapmode).

**Implemented: `lean_debug=1`** (`apply_lean`). The readers of the three groups get their disp32 rewritten to a
shadow byte S = exe+0x5d14ff0. That is the last 16 bytes of the page holding the end of .data
(VirtualSize ends at 0x5d14c44: zero, RW). S reads 0 while lean is active. `lean off` mirrors the real flag into
S every frame. The console and cheats keep reading the real flag. Exact counts per group (gui 1, tooltips 12 via
the 8 anchor strings, cache 1 with shape `40 38 35 D 74`); a mismatch skips only that group.

## 5. fast_speed5: withdrawn (caused a crash)

**Crash 13:37:44** (session folder crash3-20261113-133830, not in the repo):
- The lead typed `add_relations RUS 1000` at speed 5.
- debug.log: `Random is blocked by thread 5052`. devtools' thread table: 5052 = **Network/Session thread**.
- Then AV, read of 0x0 at 03787360. The global random pointer `[0540ae80]` was NULL: `test rdi,rdi; je` takes
  the assert path, then `mov r9,[rdi]`.

How it happens (static, 1.15.1):
- Each main frame, 03686900 calls **CIngameIdlerLogic::Update 00f8bfc0** (vtable slot 4) through `[rbx+8]`.
  It ends with the tick issuer 00f8d340. Then it calls **CIngameInterfaceIdlerGfx::Update 023bb130** through
  `[rbx+0x10]`.
- 023bb130 takes Interface access (03689380 with edx=1), updates the GUI and releases it (03689490). Only then
  does it run the console (036b8560 → ... → dispatch 03dc1180). Console commands run without game-state access.
- A console command takes access itself, in this order. add_relations OnExecute 0088b8a0:
  1. `ALLOW_RANDOM_IN_SCOPE`: 037879e0 does `lock cmpxchg` of the random owner. If another thread owns it,
     it only logs "Random is blocked by thread {}" and asserts. Then it `xchg`es its scope into the single
     global 0540ae80.
  2. Only then it waits for Logic access: 03689380 with edx=2 (consolecmdimpl.cpp:4988).
- Message 0x5b03 is sent by the last statement of CPartialDailyTickCommand::Execute 00ca8690 (00ca8a42).
  That runs on the session thread, inside the tick command, holding Logic access and the random scope.
- Vanilla posts a new tick only from the main thread in CIngameIdlerLogic::Update. So after a tick ends, the
  session has no next tick until the main thread's next logic update.
- fast_speed5 posted the next tick right there, on the session thread, so ticks ran back to back and the
  session was almost always inside a random scope. A console command took the random scope during a running
  tick and waited for Logic access. When the session's scope ended it restored the saved pointer (NULL) over
  the console's. The console then used NULL and crashed.
- Vanilla has the same order in console commands, but the window is the session's pickup delay after a
  main-thread post, not continuous ticking.

**Why there is no safe redesign with a gain:**
- Posting from any non-main thread recreates the overlap.
- On the main thread, the only other points (message pump, frame end) are right before the next logic update,
  so they gain nothing.
- A post placed before the interface update would make a fresh tick overlap that frame's console commands more
  often than vanilla.

So the feature is removed. The `fast_speed5` key is only read to log "WITHDRAWN, ignored", and no hooks are
installed (test: no hook on 00f8d340 / 00f8b320).

**lean_debug is not involved.** None of its redirected readers is on the crash path (03787300, 00c23170,
00c1fa20, 0088b8a0, 03dc1180, 036bf740, 036c0070, 036b85cd, 023bb130, 03686900, 037879e0, 03689380), and none
of those functions reads the debug flag. The console gates (0087e684, 036bd715, 036b8aae) still read the real
flag.

## 6. Save events for other plugins

The SaveGameState hook (03662910) is always installed (when its anchor matches). It emits `save_begin` before
the original and `save_end` after, both with `V3X_SaveBegin {name}` (include/v3x.h, API v3). The name is a
NUL-terminated copy of the engine's {ptr,len}, or NULL. sim_opt subscribes to save_begin. Note: format 3 saves
ran partly on worker thread Task_P_BN 1 (the 13:36:40 write exception was thrown on tid 6020). The hook itself
is called from the main-thread save paths.

## 7. Further enhancement ideas, ranked by value/risk

1. **Safe console** (new, from the crash analysis). Hook the console dispatch 03dc1180 and take Logic access
   (03689380 with edx=2) before running a command, so the command's random scope can never overlap a running
   tick. That race also exists in vanilla, with a small window. Value: removes a crash class when cheating at
   speed 5. Risk: medium (03689380 re-entrancy has to be checked, because the command takes access again inside).
2. **(partly done as lean_debug) Cheats without debug_mode**: a predicate bypass like ironman_console for the
   console gates 0087e684 / 036bd715. Then debug_mode is not needed at all. Risk: medium; multiplayer must stay
   blocked.
3. **Faster saves**: the freeze is serialization (3.2-3.6 s even in binary format). Next step is a profile of
   a save to see whether 0369e0d0's threaded writers are starved (about 1.3 s main-thread CPU per save in the
   first session). Value high, risk unknown.
4. **Runtime toggle of "Sacrifice sub-tick rendering for tick speed"** (launch option
   `disable_renderframeifneeded`, 04698700) only at speed 5. It is an engine switch; coordinate with frame_pacer
   tick_render_hz. Risk low, value medium.
5. **"No auto-pause for notifications" toggle** (launch option `no_notifications`, 04698838) for long speed-5
   runs. Risk low-medium.
6. (tried) Issue the next tick at tick end: withdrawn, see 5.
