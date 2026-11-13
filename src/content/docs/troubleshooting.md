---
title: "Troubleshooting"
summary: "Save the evidence, read v3x.log and crash.log, hangs, build problems."
group: players
order: 3
source: "docs/troubleshooting.md"
sourceUrl: "https://github.com/kleeedolinux/v3x/blob/main/docs/troubleshooting.md"
updated: "2026-11-13"
---
## First: save the evidence

Before you start the game again, copy these somewhere (the next start overwrites them):

* `binaries/v3x/v3x.log` (or `v3x.<pid>.log` if v3x.log was locked), `crash.log`,
  `crash.prev.log`, and every plugin output you care about (`devtools.log`, `*.csv`, `*_report.txt`)
* `Documents/Paradox Interactive/Victoria 3/logs/` (`error.log`, `debug.log`, `system.log`)
* `Documents/Paradox Interactive/Victoria 3/crashes/<newest>/` (minidump, `meta.yml`) if the
  game's crash reporter ran

Then bisect: `V3X_DISABLE=1` (no V3X at all), then `[plugins]` `<name> = 0` in
`binaries/v3x/v3x.ini` for half of the plugins, and so on.

## Reading v3x.log

```
[14:02:11.031][v3x] V3X api v4, exe=...\victoria3.exe base=00007FF6... size=0x60fb000 game=release/1.15.1
[14:02:11.032][v3x] entry hook at 00007FF6...: ok
[14:02:11.090][v3x] entry point reached on main thread 4242
[14:02:11.091][v3x] frame hook (PeekMessageW): ok
[14:02:11.120][my_plugin] anchor game_tick at exe+0x1465690 ok
[14:02:11.121][v3x] loaded plugin 'my_plugin' v0.1.0 (my_plugin.dll)
[14:02:11.400][v3x] 21/23 plugins loaded, 2 disabled in v3x.ini
```

* `entry hook ... FAILED` or no log at all: version.dll is not loaded (wrong folder: it must be
  `binaries/version.dll`), or `V3X_DISABLE` is set.
* `FAILED to load x.dll (error 126)`: a DLL the plugin imports is missing (built against a
  debug CRT or a C++ runtime that is not installed); `193`: not an x64 DLL.
* `x.dll: no v3x_plugin_load export, skipped`: missing `V3X_EXPORT` (or C++ name mangling:
  `V3X_EXPORT` adds `extern "C"` only when compiled as C++ with the header).
* `plugin 'x' returned N from v3x_plugin_load`: the plugin refused, usually an anchor. The lines
  just above say which one (`anchor ... FAILED: ...`).
* `refused: needs loader API vN`: update version.dll (`./build.sh install`).
* `refused: game ... not in its builds` / `WARNING: plugin 'x' lists game builds`: the plugin's
  manifest does not list this game build. With the WARNING it still loaded; check its anchors.
* Red lines in the console are any line with `FAIL`, `ERROR`, `EXCEPTION` or `WARNING`.

## Reading crash.log

`crash_logger` writes `binaries/v3x/crash.log` with unbuffered writes, so it survives the crash.
Two kinds of entries:

**C++ exceptions** (thrown, often caught by the engine; not necessarily a crash):
```
C++ EXCEPTION #12 thrown: <decoded type names, what()> (magic 19930520, object 000001F2...)
  #00 KERNELBASE.dll+0x...
  #01 victoria3.exe+0x...
  ...
```
(The exact layout of the type part is in `handle_cxx` in `plugins/crash_logger/crash_logger.c`.)
A burst of `bad_alloc` with low "commit avail" is out of memory: see docs/re/memory.md and
mem_tuner. Throws are rate-limited (200 total, 3 per type and site).

**Fatal exceptions** (the crash itself):
```
EXCEPTION ACCESS_VIOLATION (0xc0000005) read of address 0x0 at victoria3.exe+0x3787360
  rax=0000000000000000 rbx=... rcx=... rdx=...
  rsi=... rdi=... rbp=... rsp=...
  r8 =... r9 =... r10=... r11=...
  r12=... r13=... r14=... r15=...
  rip=00007ff6... eflags=00010246
  #00 victoria3.exe+0x3787360
  #01 victoria3.exe+0x101c770
  #02 my_plugin.dll+0x1234
  ...
UNHANDLED EXCEPTION 0xc0000005 at victoria3.exe+0x3787360 - process is going down
```
Addresses outside any module print as plain `0x...`. The access kind is `read`, `write` or
`execute (DEP)`.

How to read it:

1. **Which module?** If the fault (or the first frames) is in your `plugin.dll+0x...`, it is your
   code: `addr2line`/`llvm-symbolizer` on a build with `-g` (`python sdk/build_plugin.py <dir>
   --debug`) gives the line.
2. **In victoria3.exe:** name the function: `python tools/symbolize.py --exe <exe> 0x3787360`, or
   `fn +0x3787360` in the devconsole (next run). Look the RVA up in docs/re/*.md (search the hex
   without `0x` and with leading zeros, e.g. `03787360`).
3. **Read address 0x0 / small** = a NULL pointer the engine did not expect. Typical causes from
   our history:
   * `0x3787360` (the script RNG): script ran without an `ALLOW_RANDOM_IN_SCOPE` owner, i.e.
     outside a tick task (docs/safety.md "Script and RNG scope").
   * A garbage value in a stack argument: a detour forwarding fewer arguments than the function
     takes (docs/safety.md "Argument forwarding"). Check every raw `hook` in the stack.
4. **Is a hooked function on the stack?** Compare the stack's RVAs with the anchors your plugins
   log at load (`anchor X at exe+0x...`). A call through `hook_safe` shows a `version.dll+0x...`
   frame (the thunk) between the caller and the original; frames at plain `0x...` addresses
   outside any module are usually MinHook trampolines (the first bytes of a hooked function).
5. **`crash.prev.log`** is the run before; compare whether the same site repeats.

Minidumps from the game's reporter: `python tools/dmpinfo.py <dump>` prints the exception, the
faulting module+offset, the C++ type and exe return addresses on the faulting thread's stack.

## The game hangs

* **At exit**: a plugin doing work in `DLL_PROCESS_DETACH` (locks held by killed threads) or a
  sampler that suspended the main thread. Since API v4 the core refuses all API calls at exit;
  move final writes to the `process_exit` event. `test/shutdown_test.c` reproduces exit hangs.
* **Everything freezes while the V3X console is selected**: Windows QuickEdit selection blocks
  every console write. V3X turns QuickEdit off; if you changed console properties, press Esc or
  set `console=0`.
* **Long freezes on autosave**: serialization (~3 s, docs/re/enhance.md), not V3X. In
  `debug_mode` saves are text; `enhance binary_saves_in_debug=1` (default) keeps them binary.

## A plugin does nothing

* `plugins` console command: is it loaded? disabled in `v3x.ini`? refused?
* Its anchors: `anchor ... FAILED` lines. After a game patch this is expected until the anchors
  are updated; run the plugin's offline test against the new exe
  (`python sdk/build_plugin.py <dir> --test`) to see which check moved.
* `MH_STATUS 3` (`MH_ERROR_ALREADY_CREATED`) from `hook`/`hook_safe`: another plugin already
  hooks that function ([plugins.md](/docs/plugins/#hooks-that-cannot-be-shared)).
* Console commands do nothing: commands only run in the V3X console window (or through
  devbridge), not in the in-game `~` console.

## Build problems

* `No module named ziglang`: `pip install ziglang` for the Python you run.
* `unknown type name 'V3X_API'`: include path; build with `-Iinclude -Isdk` (the build script does).
* Unused-function warnings from the SDK headers: they are header-only `static` helpers; build
  with `-Wno-unused-function`.
* `./build.sh` stops at one plugin: `set -e` stops at the first compile error; the plugins after
  it in alphabetical order are not rebuilt. Fix that plugin or build yours with
  `sdk/build_plugin.py`.
* C: full: point `TEMP`, `TMP`, `ZIG_GLOBAL_CACHE_DIR` and `ZIG_LOCAL_CACHE_DIR` at another drive.
