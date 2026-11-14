# Reverse-engineering guide

Victoria 3 ships without symbols. Everything in `docs/re/` was found with the tools below, from
strings, RTTI, `.pdata` and live profiling. This guide shows the workflow and every tool.

Setup: `pip install pefile capstone numpy` (pefile for all tools; capstone for the
disassembling ones; numpy for a few `re_*` scanners). Run the tools without `python -I`
(isolated mode hides user site-packages). Paths below are from `v3x/`; `EXE` is
`../binaries/victoria3.exe`.

## The workflow

1. **Find a string** near what you want (a log message, an assert, a file name, a console
   command): `pestrings.py`.
2. **Find the code that uses it**: `pexref.py --str` (or `re_sim_lib.py str`), then the
   enclosing function from `.pdata`.
3. **Read the function**: `pexref.py --dis`, `re_sim_lib.py dis/fn`, or live with the devconsole
   commands `fn`, `dis`, `xref` (also through the MCP bridge).
4. **Find out what is hot**: devtools sampling profiler -> `profile_latest.csv` ->
   `symbolize.py` names the functions by the strings they reference.
5. **Find classes and virtual calls**: `rtti_dump.py` (vtables by class name, slot targets, ABI
   hints).
6. **Count arguments before hooking**: `re_simopt_argcheck.py`.
7. **Write the anchor** (string + shape) in your plugin and an **offline check** that runs it
   against the exe (`sdk/v3x_testkit.h`, or a `tools/re_<topic>_check.py`).
8. **Write it down** in `docs/re/<topic>.md`: RVAs *with* the anchor used to find each one, and
   tags **[V]** verified / **[I]** inferred / **[D]** from dumps / **[R]** measured at runtime.

RVAs are relative to the image base (0x140000000 in the file). Every RVA in `docs/re/` is for
**1.15.1** (PE TimeDateStamp `0x6af3094f`) and is for orientation only: a plugin never computes
`api->image_base() + rva`, it finds the address at runtime with an anchor (next section) and
refuses to run when the anchor does not match.

## From a finding to plugin code

What you found with the tools becomes an anchor table (`sdk/v3x_anchor.h`): the string the
function references, the prologue bytes it starts with, and a `check` callback that proves the
detail your plugin relies on (here: the function reads the field at `+0x120` within its first
0x100 bytes). If any of it fails, `v3x_anchor_resolve_all` logs why and the plugin installs
nothing:

<!-- sample: file -->
```c
/* the detail we rely on: mov rax, [rcx+0x120] near the function start */
static int check_reads_field(const V3X_API *api, void *fn, void *user)
{
    (void)user;
    V3xSection text;
    if (!v3x_section(api->image_base(), ".text", &text)) return 1;
    return v3x_scan_near_in(&text, fn, 0x100, "48 8B 81 20 01 00 00") ? 0 : 2;
}

static V3xAnchor g_anchors[] = {
    {.name = "game_tick", .string = "Processing Tick: ",                  /* step 1-2: the string */
     .pattern = "48 89 5C 24 ?? 48 89 74 24 ?? 48 89 7C 24",              /* step 3: its prologue */
     .xref = V3X_ANCHOR_ALL_SAME,                                         /* every xref, one function */
     .check = check_reads_field},                                         /* the shape we use */
};

int my_resolve(const V3X_API *api)   /* shared by v3x_plugin_load and the offline test */
{
    return v3x_anchor_resolve_all(api, "my_plugin", g_anchors, 1);
}
```

The offline check runs the same code against the exe mapped as an image (no game process, nothing
executes), so a game patch that moves or changes the function fails here, not in the game:

<!-- sample: complete -->
```c
#include "v3x_testkit.h"
#include "v3x_sdk.h"

int my_resolve(const V3X_API *api);   /* from the plugin source, compiled into the test */

int main(int argc, char **argv)
{
    V3X_API api;
    if (argc > 1 && v3x_testkit_map_exe(argv[1], &api) == 0)
        V3X_TEST_CHECK(my_resolve(&api) == 0, "anchors resolve on %s", argv[1]);
    return v3x_testkit_done();   /* "N checks, M failed", exit code 1 on any failure */
}
```

`python sdk/build_plugin.py <dir> --test ../binaries/victoria3.exe` builds and runs it (any
`test_*.c` in the plugin folder). Then read the matched address back with the devconsole (`fn
<addr>`) and compare with your notes.

> **Unsafe — when you really need it:** `api->pattern_scan(...)` or `api->find_xref(s, 0)` used
> directly as the hook target.
> * What can go wrong: `pattern_scan` returns the first match, so a pattern with two matches is a
>   coin flip after the next patch; xref index 0 is whichever reference the linker put first.
>   Either way you hook or patch the wrong code and the game crashes later, somewhere else.
> * How to contain it: use them only to explore (devconsole `scan`, `xref`). In a plugin use
>   `V3xAnchor`: it insists on exactly one pattern match, on all xrefs agreeing on one function,
>   on the prologue, and on your `check`.

## Static tools

### `tools/pestrings.py` - strings with RVAs
```sh
python tools/pestrings.py $EXE 6 > strings.txt          # minimum length 6
grep -n "Processing Tick" strings.txt
```

### `tools/pexref.py` - code references to a string or an RVA
```sh
python tools/pexref.py $EXE --str "Processing Tick: "
python tools/pexref.py $EXE --rva 0x475efc0 --dis 8 20  # disassemble 8 before / 20 after each xref
```
Finds RIP-relative `lea`/`mov` like `api->find_xref`. `tools/re_sim_anyref.py` and
`tools/re_hist_anyref.py` find any RIP-relative operand (cmp, vmov, byte flags).

### `tools/peptr.py` - absolute pointers to an RVA or string
```sh
python tools/peptr.py $EXE --str "CTickTask_UpdateAI"   # vtables, tables, registries in .rdata/.data
```

### `tools/re_sim_lib.py` - indexed disassembly (and its per-topic wrappers)
Builds a one-time xref index (cached as `tools/re_sim_index.pkl`; set `RE_SIM_CACHE` to put it
elsewhere).
```sh
python tools/re_sim_lib.py $EXE str "Calling global random function without having called ALLOW_RANDOM_IN_SCOPE in this thread"
python tools/re_sim_lib.py $EXE dis 3787300 40           # 40 instructions from an RVA
python tools/re_sim_lib.py $EXE fn 3787300               # start/end of the .pdata function
python tools/re_sim_lib.py $EXE calls 3787300            # direct call targets inside it
python tools/re_sim_lib.py $EXE callers 3787300          # direct callers
```
`re_enh_lib.py`, `re_hist_lib.py`, `re_mem_lib.py`, `re_stream_lib.py`, `re_vfs_lib.py`,
`re_load_*.py`, `re_console_*.py`, `re_threads_*.py`, `re_tickpar_*.py`, `re_simopt_*.py` are
topic helpers written by the workers of each area; each has a usage docstring at the top.

### `tools/rtti_dump.py` - MSVC RTTI: classes, vtables, slots
```sh
python tools/rtti_dump.py $EXE --prefix CTickTask_ --slots 8 --slot 6
```
Same algorithm as `include/v3x_rtti.h` (which your plugin uses at runtime). The slot analysis
prints target size, sharing between classes, first instructions and ABI hints (xmm reads, stack
arg reads, float returns).

### `tools/re_simopt_argcheck.py` - how many arguments does it take?
```sh
python tools/re_simopt_argcheck.py $EXE e02c90 146d010
```
Two independent signals: what callers write to `[rsp+0x20..]` before the call, and what the
callee reads above its home slots. If either shows stack arguments, a 4-argument detour is wrong.
(`hook_safe` makes this moot for up to 16 arguments.)

### `tools/re_mem_catch.py` - who catches which C++ exception
```sh
python tools/re_mem_catch.py $EXE ".?AVbad_alloc@std@@"
```

### `tools/dmpinfo.py`, `tools/re_mem_dmp.py` - minidumps
```sh
python tools/dmpinfo.py <crash.dmp>      # exception, faulting module+offset, C++ type, exe return addresses
python tools/re_mem_dmp.py <crash.dmp>   # memory summary
```
The game's crash reporter writes dumps under `Documents/Paradox Interactive/Victoria 3/crashes/`.

## Runtime tools (plugins)

### devconsole - read-only RE commands in the running game
Type into the V3X console (`help` lists them):
```
fn +0x1465690              whole function containing the RVA, Zydis disassembly
dis victoria3.exe+0x1465690 30
xref "Processing Tick: "   rip-relative references to an exact string (or an address)
scan 48 8B 05 ?? ?? ?? ?? 48 85 C0
mem <addr> 256             hex + ASCII; unreadable memory prints an error, never crashes
mods                       loaded modules
```
Addresses: hex, `+rva` (in the exe), or `module+offset`.

### devtools - sampling profiler
`binaries/v3x/devtools.ini`: `profile=1` (or env `V3X_PROFILE=1`, or console `profile on`).
About 1 kHz on the main thread + the 4 busiest threads; `unwind_depth=8` frames for inclusive
time. Outputs in `binaries/v3x/`: `devtools.log` (CPU per thread, memory, GPU memory every
`interval_sec`), `profile_latest.csv` (rva, self samples, inclusive, module),
`profile_edges.csv` (caller -> callee), and per load transition
`profile_<from>_to_<to>_<n>.csv`.
```sh
python tools/symbolize.py --exe $EXE --top 50 ../binaries/v3x/profile_latest.csv
python tools/re_simopt_tree.py profile.csv profile_edges.csv <rva>     # hot call tree below a function
python tools/re_map_chain.py profile_edges.csv <rva> --exe $EXE        # who calls it
python tools/re_load_attrib.py profile_latest.csv                       # attribution for load phases
```
`symbolize.py` names functions by the strings they reference (source paths first) and, for
functions without strings, by their callees.

### tick_profiler - time per tick task
Hooks `RunTask`; console `ticks`; `binaries/v3x/tick_profile.csv` every 60 s.

### alloc_tracker - allocations by call site
`binaries/v3x/alloc_tracker.ini`: `enabled=1`. Hooks mimalloc `mi_new`/`mi_malloc`/`mi_free`;
console `allocs [top N]`; `binaries/v3x/alloc_report.txt` (live and total bytes per call site).
Use it to find who holds memory; turn it off again afterwards (it costs a few ns per allocation).

### crash_logger - what happened
`binaries/v3x/crash.log`: C++ throws with type names, fatal exceptions with registers and stack
as `module+offset`. See [troubleshooting.md](troubleshooting.md).

### devbridge + MCP - drive the console from a tool or an AI assistant
`plugins/devbridge` serves the V3X console over the named pipe `\\.\pipe\v3x_devbridge`
(env `V3X_DEVBRIDGE_PIPE` changes the name). `mcp/v3x_mcp.py` is a stdio MCP server (Python
standard library only) that exposes it as tools: `v3x_exec` (any console line), `v3x_dis`,
`v3x_fn`, `v3x_xref`, `v3x_mem`, `v3x_profile_report`, `v3x_ticks`. Register it from
`.mcp.json.example` (replace the path with yours). Every call opens the pipe, runs one command,
returns its captured output (`exec_capture`) and closes it.

## Engine-specific tips

* **Assert and log strings are the best anchors.** The engine keeps source file names
  (`gamestatetick.cpp:1253`), format strings (`"Tick Tasks for {}"`) and assert texts.
* **Console commands** are registered by name; `tools/re_console_cmds.py` lists their descriptors
  and handlers (docs/re/console.md). A console command that does what you want is a short path to
  the function that does it.
* **Tick tasks** are classes `CTickTask_<Name>`: find the vtable by RTTI, slot 6 is Execute
  (docs/re/simulation.md).
* **Split functions**: MSVC moves cold code into separate chunks with chained unwind info.
  `function_start` follows the chain; your shape checks must use the primary function.
* **Leaf functions** (e.g. `mi_free`) have no `.pdata` entry: `function_start` returns NULL;
  find them by their callers (`tools/re_mem_count.py`).
* **Statically linked libraries** (mimalloc 2.0.x, spdlog, PhysFS 3, SDL, Zstd...) can be matched
  against their open-source code: compare strings and constants.

## Writing it up

Each `docs/re/<topic>.md` starts with the binary it was made on, the tools used, and the tags.
For every RVA give the anchor that finds it (string, pattern, caller) so the next person can
re-find it after a patch. Put "Not verified" in its own section.
