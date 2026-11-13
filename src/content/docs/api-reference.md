---
title: "API reference (`include/v3x.h`, API v5, loader 0.5.0)"
summary: "Every field of V3X_API, thread rules, the manifest, events, return codes."
group: modders
order: 12
source: "docs/api-reference.md"
sourceUrl: "https://github.com/kleeedolinux/v3x/blob/main/docs/api-reference.md"
updated: "2026-11-13"
---
A plugin is a Windows x64 DLL in `binaries/v3x/plugins/`. The loader (`binaries/version.dll`,
source `src/core.c`) calls its `v3x_plugin_load(const V3X_API *api)` once and passes a table of
function pointers. The API is plain C, so any compiler works.

What is guaranteed to stay the same across loader updates (layout, return codes, thread rules,
deprecation, game patches) is in [api-stability.md](/docs/api-stability/).

## Exports a plugin provides

| export | required | meaning |
|---|---|---|
| `V3X_EXPORT int v3x_plugin_load(const V3X_API *api)` | yes | return 0 on success; anything else is logged as `FAILED: v3x_plugin_load returned N` |
| `V3X_EXPORT const char *v3x_plugin_name = "x";` | no | display name (default: the file name) |
| `V3X_PLUGIN_MANIFEST(name, version, author, description, game_builds)` | no (v4) | data export `v3x_plugin_manifest`, see [Manifest](#manifest) |

### When `v3x_plugin_load` runs

On the game's **main thread**, from a hook on the exe's entry point, **before** the CRT starts,
before static constructors and before WinMain. So:

* code is mapped: scanning, `hook`, `hook_safe` and `patch` work;
* game globals do not exist yet (most are zero); the engine has not read its settings;
* the CRT of the *game* is not initialised (your own DLL's CRT is);
* do not block, do not wait for game threads (there are none yet).

Do the work that needs a running game in your hooks, or in `on_frame`.

### Loading order and the skip list

The loader enumerates `binaries/v3x/plugins/*.dll` (`FindFirstFile` order, usually
alphabetical). For each file:

1. If `binaries/v3x/v3x.ini` has `[plugins]` `<file name without .dll> = 0` (`off`, `no`,
   `false`; case-insensitive), the DLL is not loaded at all: `disabled (v3x.ini [plugins])`.
2. Crash-loop guard (0.5.0): if the game died while loading this plugin on the previous start
   (`v3x/loading.marker` still names it), it is skipped once: `disabled: crashed during load
   last start`. The marker is written before step 3 and removed after step 5.
3. `LoadLibrary`. Your `DllMain` runs under the loader lock: do nothing there (see
   [safety.md](/docs/safety/#dllmain)).
4. The manifest is read (if exported). `min_api` above the loader's version, or a game-build
   mismatch with `V3X_MANIFEST_REQUIRE_BUILD`, refuses the plugin (`v3x_plugin_load` is not
   called; the DLL is unloaded). On a game build the loader does not know (a patch), the
   [unknown-build gate](/docs/api-stability/#game-patches) also skips plugins whose manifest does not
   declare that build.
5. `v3x_plugin_load(&api)`, inside a fault guard. If it returns non-zero or faults, the loader
   **rolls the plugin back**: its hooks are removed, its patches restored (newest first), and its
   `on_frame` / command / event / `run_on_main` callbacks are never called. Status:
   `FAILED: v3x_plugin_load returned N, rolled back` or `FAULTED in v3x_plugin_load, rolled back`.

### Fault isolation (0.5.0)

Every callback the loader itself calls (`v3x_plugin_load`, `on_frame`, `run_on_main`, console
commands, event subscribers) runs inside an SEH guard. An access violation, illegal
instruction, divide by zero, in-page error or an escaping C++ exception there is caught: v3x.log
gets `FAULT in plugin x.dll during <what>: exception 0x... (write of 0x...) at module+offset`,
the plugin's callbacks are skipped from then on (`plugins` shows `FAULTED in ...`), its hooks
stay, and the game keeps running. **Not isolated**: faults in your hook detours / `hook_safe`
pre/post (they run inside engine calls), in engine code you call or patched, and in threads you
start. A fault isolated this way may have left your own locks or data half-updated; that is why
the whole plugin is switched off.

## Versioning

* `api->api_version`: the loader's `V3X_API_VERSION` (1..5).
* `api->struct_size`: `sizeof(V3X_API)` of the loader.
* The table is **append-only**: fields are never moved or removed. A plugin built with a newer
  header runs on an older loader as long as it checks before using newer fields:

```c
if (V3X_HAS(api, hook_safe)) api->hook_safe(fn, pre, post, NULL);   /* v4 */
else return 1;                                                        /* fail closed */
```

`V3X_HAS(api, field)` checks `struct_size` covers the field and the pointer is set. For v2/v3
fields, `api->api_version >= N` also works.

| version | added |
|---|---|
| v1 | `log`, `image_base`, `image_size`, `game_version`, `pattern_scan`, `find_string`, `find_xref`, `function_start`, `hook`, `unhook`, `patch`, `on_frame`, `game_dir`, `v3x_dir` |
| v2 | `register_command`, `print`, `run_on_main`, `exec_command` |
| v3 | `exec_capture`, `on_event`, `emit_event` |
| v4 | `hook_safe`, `main_thread_id`, `game_build_id`, `enum_commands`; the plugin manifest; the `[plugins]` skip list |
| v5 | `log_at`, `log_level`, `loader_version`, `build_matches`; the `config_reload` event; named return codes and log levels; loader 0.5.0 hardening (fault isolation, rollback, crash-loop guard, unknown-build gate) |

## Thread rules at a glance

| function | callable from | callbacks run on |
|---|---|---|
| `log`, `print` | any thread (thread-safe, serialized by one lock) | - |
| `image_base`, `image_size`, `game_version`, `game_build_id`, `game_dir`, `v3x_dir`, `main_thread_id` | any thread (constant after boot) | - |
| `pattern_scan`, `find_string`, `find_xref`, `function_start` | any thread (read-only) | - |
| `hook`, `unhook`, `hook_safe` | any thread; best at load time. MinHook briefly suspends all other threads (not for a 2nd+ `hook_safe` subscriber on an already hooked target) | your detour / pre / post: the thread that calls the hooked function |
| `patch` | any thread, but patching code another thread is executing is a race: patch at load time or while the code cannot run | - |
| `on_frame` | **load time** (`v3x_plugin_load`) only; the table is not locked | main thread, once per message-pump iteration |
| `run_on_main` | any thread | main thread, next frame tick |
| `register_command` | any thread | console input thread (`v3x_console_input`), or the caller of `exec_command`/`exec_capture` |
| `exec_command`, `exec_capture` | any thread; the command runs synchronously on the caller | caller |
| `on_event` | any thread | the emitting thread (synchronously) |
| `emit_event` | any thread | - |
| `enum_commands` | any thread | caller |
| `log_at`, `log_level`, `loader_version`, `build_matches` (v5) | any thread (`log_level` lock-free) | - |
| `config_reload` event (v5) | emitted by the core on the main thread | main thread |

Every function returns immediately and does nothing once the process is exiting
(`ExitProcess` has started killing threads, or `DLL_PROCESS_DETACH` ran): logging, printing,
hooking, registering and emitting are refused then.

## V3X_API fields

### `uint32_t api_version`, `uint32_t struct_size`
See [Versioning](#versioning).

### `void log(const char *tag, const char *fmt, ...)`
printf-style line to `binaries/v3x/v3x.log` and the V3X console, prefixed
`[hh:mm:ss.mmm][tag] `. The file is flushed after every line (it survives a crash). Lines
containing `FAIL`, `ERROR`, `EXCEPTION` or `WARNING` are shown red. Max line ~2 KB. Any thread.
Do not log from very hot paths (every call takes a global lock and writes the file).

### `uintptr_t image_base(void)`, `size_t image_size(void)`
Base address and `SizeOfImage` of `victoria3.exe` in memory. RVA = address - `image_base()`.

### `const char *game_version(void)`
Contents of `<game>/caligula_branch.txt`, e.g. `"release/1.15.1"`; `"unknown"` if missing.

### `void *pattern_scan(const char *ida_pattern)`
First match of an IDA-style pattern (`"48 8B 05 ?? ?? ?? ?? 48 85 C0"`, `?` or `??` = any
byte) in the exe's `.text`, or NULL. It does **not** tell you whether the match is unique:
use `v3x_scan_count` (`sdk/v3x_scan.h`) or `V3xAnchor` (`sdk/v3x_anchor.h`) for anchors.

### `void *find_string(const char *s)`
Address of the exact NUL-terminated ASCII string `s` in `.rdata`/`.data` (a match must start
after a NUL byte, so substrings of longer strings are not returned). NULL if absent.

### `void *find_xref(const void *target, int index)`
The `index`-th instruction (0 = first, in address order) in `.text` that references `target`
with a RIP-relative `lea`/`mov` (`48|4C 8D|8B modrm(rm=101) disp32`). NULL when there is no
such instruction. Other encodings (`cmp`, `vmov*`, 32-bit `lea`) are not found; see
`v3x_xrefs` in `sdk/v3x_scan.h` for all of them at once.

### `void *function_start(const void *addr)`
Start of the function containing `addr`, from the exe's `.pdata` unwind table, following
chained unwind entries (MSVC splits hot/cold parts) back to the primary function. NULL for leaf
functions without unwind info.

### `int hook(void *target, void *detour, void **original)`
MinHook inline hook: `target` now jumps to `detour`; `*original` receives a trampoline that runs
the original code. Returns 0 or a MinHook `MH_STATUS`. A raw hook is **exclusive**: if the
target already has a raw hook or `hook_safe` subscribers (any plugin), `hook` returns
`V3X_EHOOKBUSY` (100) and v3x.log names the holder, e.g.
`hook 00007FF6... by my_plugin.dll: V3X_EHOOKBUSY, target held by par_load.dll (hook_safe)`.
Raw detours are not chained on purpose (each would have to call the previous one's trampoline
with the exact prototype; one mistake breaks every plugin behind it). For engine functions other
plugins may want too, use `hook_safe`: it can be shared.
Your detour must have **exactly** the target's calling convention and **at least** its
argument count: see [safety.md](/docs/safety/#argument-forwarding). Prefer `hook_safe`.

### `int unhook(void *target)`
Removes what the **calling plugin** holds on `target` (the core finds the calling DLL from the
return address): its raw hook, or all of its `hook_safe` subscriptions there. Other plugins'
subscriptions on the same target stay; when the last subscriber leaves, the thunk is disabled
and the original bytes are restored. Returns 0, `V3X_EHOOKBUSY` if only other plugins hold the
target (nothing is removed), or 4 `MH_ERROR_NOT_CREATED` if nothing is hooked there. A thread
may still be running inside your detour or callback: keep its code and data alive (never
`FreeLibrary` yourself).

### `int patch(void *addr, const void *bytes, size_t n)`
Writes `n` bytes over code or read-only data (handles page protection and the instruction
cache). 0 on success, otherwise a Win32 error code. Verify the bytes you replace first.

### `int on_frame(v3x_frame_fn fn, void *user)`
Registers `fn(user)`, called on the main thread once per message-pump iteration (from a
`PeekMessageW` hook; about once per frame whenever the main thread pumps messages). Max 64 callbacks
for all plugins. Register only from `v3x_plugin_load`. Returns 0 or -1 (table full, or `fn` NULL).
Keep it cheap: it runs every frame on the thread that renders.

### `const char *game_dir(void)`, `const char *v3x_dir(void)`
Absolute paths with a trailing backslash: `.../Victoria 3/binaries/` and
`.../Victoria 3/binaries/v3x/`. Your config is `v3x_dir() + "<plugin>.ini"`; write output
files there too (`v3x_plugin_file` / `v3x_path` / `v3x_file_write_atomic` in `sdk/v3x_util.h`).

### `int register_command(const char *name, const char *help, v3x_command_fn fn, void *user)` (v2)
Adds a console command. `name`: up to 31 chars, no spaces, case-insensitive. `fn(args, user)`
receives the rest of the line (leading spaces stripped, trailing newline removed, never NULL).
Returns 0, -1 (table of 128 full / bad args), -2 (name taken). Without a console window
registration still succeeds; commands then run only through `exec_command` (e.g. devbridge).
**`fn` runs on the console input thread**, not the main thread: read your own state only, and
use `run_on_main` for anything that touches the game.

### `int print(const char *fmt, ...)` (v2)
printf to the console (no tag) and `v3x.log`; a newline is added if missing; output over 8 KB
is truncated. Returns chars written or -1. Thread-safe. Use it for command output.

### `int run_on_main(v3x_frame_fn fn, void *user)` (v2)
Queues `fn(user)` to run once on the main thread in the next frame tick (after the `on_frame`
callbacks). Up to 256 queued items. Returns 0 or -1 (queue full). Note: the main thread is the
render/UI thread; it is **not** inside a simulation tick task, so it is not a place to run game
script (see [safety.md](/docs/safety/#script-and-rng-scope)).

### `int exec_command(const char *line)` (v2)
Runs one console line synchronously on the calling thread. 0 if a command ran (also if it
faulted: the fault is isolated, see [Fault isolation](#fault-isolation-050)), -1 if unknown or if
its plugin was disabled after a fault.

### `int exec_capture(const char *line, void (*sink)(const char *text, void *user), void *user)` (v3)
Like `exec_command`, and every `print`/`log` made **on the calling thread** while the command
runs is also passed to `sink` (one call per print/log; print text with its `\n`, log text as the
full line). Output from other threads is not captured. Nested captures use the inner sink.
`sink` may be NULL.

### `int on_event(const char *name, void (*fn)(const char *name, void *data, void *user), void *user)` (v3)
Subscribes to a named event (1..63 chars, case-sensitive). 64 subscriptions for all plugins
together; the same (name, fn, user) only once. Returns 0, -1 (full / bad args), -2 (already).

### `int emit_event(const char *name, void *data)` (v3)
Calls every subscriber synchronously on the calling thread, in subscription order, with `data`
untouched. Returns the number of subscribers called, -1 on bad args.

Known events:

| event | emitter, thread | data |
|---|---|---|
| `load_transition` | fast_load, main, when a load-state transition ends | `const V3X_LoadTransition *` {`from`, `to`, `seconds`}, may be NULL |
| `load_transition_begin` | fast_load, main, when one begins | `const V3X_LoadTransition *` (`seconds` = 0), may be NULL |
| `save_begin` | enhance, the saving thread, right before `SaveGameState` | `const V3X_SaveBegin *` {`name`} |
| `save_end` | enhance, same thread, after the save returned | same pointer |
| `process_exit` | core, the thread calling `ExitProcess`, while every thread still runs | `const LONG *` exit code |
| `config_reload` (v5) | core, **main thread**, next frame tick after the console command `reload [plugin]` | `const char *` plugin name, or NULL = all plugins |

The names are also macros: `V3X_EVENT_PROCESS_EXIT`, `V3X_EVENT_CONFIG_RELOAD`,
`V3X_EVENT_LOAD_TRANSITION`, `V3X_EVENT_LOAD_TRANSITION_BEGIN`, `V3X_EVENT_SAVE_BEGIN`,
`V3X_EVENT_SAVE_END`. A subscriber of a plugin that faulted is skipped; `emit_event` counts only
the subscribers it called.

`process_exit` subscribers must be short and must not wait for other threads without a timeout.
`save_begin` runs **outside** any tick task: never run game script from it.

### `int hook_safe(void *target, v3x_hook_fn pre, v3x_hook_fn post, void *user)` (v4)
Hooks `target` with a core-generated pass-through thunk. On every call:

1. the thunk saves `rcx, rdx, r8, r9`, the caller's first 12 stack argument slots and
   `xmm0..xmm3` into a `V3X_HookCall` on its own stack frame;
2. `pre(call)` runs (if set). It may change `args[]` / `xmm[]`, or set `skip_original = 1`
   and a replacement `ret` / `xmm_ret`;
3. unless skipped, the original runs with all 16 integer slots and xmm0-3 reloaded;
   `rax` -> `call->ret`, `xmm0` -> `call->xmm_ret`;
4. `post(call)` runs (if set) and may change `ret` / `xmm_ret`;
5. the thunk returns `ret` in rax and `xmm_ret` in xmm0.

`pre` or `post` may be NULL, not both. Returns 0, -1 (bad args), `V3X_EHOOKBUSY` (the target
has a raw `hook`; v3x.log names the owner), 3 `MH_ERROR_ALREADY_CREATED` (this plugin already
subscribed the same `pre`/`post`/`user` there), or another `MH_STATUS` (9 =
`MH_ERROR_MEMORY_ALLOC` also when the pool of 4096 hooked targets is full or the target already
has `V3X_HOOK_MAX_SUBSCRIBERS` = 8 subscribers). Remove with `unhook`. The
thunk has unwind info (`.pdata`), so C++ exceptions thrown through the hooked function, crash
handlers and profilers' stack walks pass through it. Overhead: about 40 instructions plus your
callbacks per call.

**Several subscribers on one target.** Any number of plugins (up to 8 subscriptions per
target, a plugin may subscribe several different `pre`/`post`/`user` triples) can `hook_safe` the
same function, e.g. par_load and lazy_gfx both on `VFSOpen`. They share one thunk and run like
nested wrappers, in subscription (= plugin load) order:

```
pre(A)  pre(B)  pre(C)  original  post(C)  post(B)  post(A)
```

* each subscriber gets its own `call->user` and its own `call->scratch` (zeroed per call);
* `args[]` / `xmm[]` changed by a `pre` are what the next `pre` and the original see; `ret` /
  `xmm_ret` changed by a `post` are what the previous `post` and the caller see;
* `skip_original`: when a `pre` sets it, the chain stops there. The later subscribers see nothing
  of that call (neither `pre` nor `post`), the original is not called, and the `post` callbacks of
  the skipping subscriber and of the earlier ones run (with `skip_original = 1`). So the first
  subscriber that skips wins, and its `ret` is the one returned (unless an earlier `post`
  changes it);
* subscribing / unsubscribing is safe while other threads call the function: the subscriber list
  is an immutable snapshot swapped atomically (callers take no lock); a call that already started
  finishes with the list it started with (its `pre` and `post` always pair up);
* `unhook` removes only the calling plugin's subscriptions (see `unhook`).

Do not rely on the order between plugins beyond "load order"; if two plugins must agree on who
skips or rewrites arguments, coordinate through an event.

Limits: functions with more than 16 arguments, `__vectorcall` HVA arguments and functions that
inspect their own return address (`_ReturnAddress`) are not forwarded exactly. Do not
`hook_safe` such functions.

```c
typedef struct V3X_HookCall {
    uint64_t args[16];        /* integer args: rcx, rdx, r8, r9, stack args 5..16 */
    V3X_Xmm  xmm[4];          /* xmm0..3 at entry: float/double args 1..4 (.d / .f) */
    uint64_t ret;             /* rax after the original */
    int32_t  skip_original;   /* pre sets 1: do not call the original */
    int32_t  reserved;
    V3X_Xmm  xmm_ret;         /* xmm0 after the original */
    void    *user;            /* hook_safe's user */
    void    *target;          /* the hooked function */
    void    *original;        /* trampoline (call only with the exact prototype) */
    void    *return_address;  /* who called the function */
    uint64_t scratch[2];      /* zeroed per call; pre -> post state */
} V3X_HookCall;
```

Argument `i` (0-based) of a Win64 function is in `args[i]` if it is an integer, pointer or a
struct of 1/2/4/8 bytes; larger structs are passed as a pointer in `args[i]`. A float/double
argument `i < 4` is in `xmm[i]`; from the 5th on, floats are in `args[i]` as raw bits (use
`memcpy`). A function returning a struct > 8 bytes has a hidden result pointer in `args[0]` and
every argument shifts by one. Helpers: `V3X_ARG(call, i, type)`, `V3X_ARGF(call, i)` (double),
`V3X_ARGF32(call, i)` (float) in `sdk/v3x_hook.h`.

The callbacks run on the thread that calls the hooked function, often an engine worker:
keep them short, do not take locks the engine might also need, do not call `log` thousands of
times per second, and do not run game script unless `v3x_script_random_allowed()` says so
(`sdk/v3x_game.h`).

### `uint32_t main_thread_id(void)` (v4)
Win32 thread id of the game's main thread (the entry-point thread; `on_frame` and
`run_on_main` run there). `v3x_on_main_thread(api)` in `sdk/v3x_game.h` wraps it.

### `const char *game_build_id(void)` (v4)
Identity of the running exe from its PE header: `"<TimeDateStamp>-<SizeOfImage>"` in hex, e.g.
`"6af3094f-60fb000"` for 1.15.1. It changes with every game patch, also hotfixes that keep
`caligula_branch.txt`. Use it in your manifest's `game_builds` to pin a plugin to a build.

### `int enum_commands(void (*fn)(const char *name, const char *help, void *user), void *user)` (v4)
Calls `fn` for every registered console command in registration order. Returns the count, -1
on bad args. For completion UIs and tools.

### `void log_at(int level, const char *tag, const char *fmt, ...)` (v5)
`log` with a level: `V3X_LOG_ERROR` (1), `V3X_LOG_WARN` (2), `V3X_LOG_INFO` (3),
`V3X_LOG_DEBUG` (4), `V3X_LOG_TRACE` (5); out-of-range levels are clamped. Written only if
`level <= log_level(tag)`. ERROR/WARN/DEBUG/TRACE lines get `ERROR: ` / `WARNING: ` / `debug: ` /
`trace: ` after the tag (the first two are shown red in the console). Any thread.
`V3X_LOGE/W/I/D/T(api, tag, ...)` in `sdk/v3x_util.h` use it and fall back to `log` on older
loaders (dropping debug/trace there).

### `int log_level(const char *tag)` (v5)
Current threshold for `tag` (NULL = global): `v3x.ini` `[log]` `<tag> = <level>`, else `[log]`
`level = <level>`, else env `V3X_LOG_LEVEL`, else info. Levels: `off` (0), `error`, `warn`,
`info`, `debug`, `trace` or `0`-`5`. Tags are case-insensitive, up to 31 chars, 32 tags. The
console command `loglevel [tag] <level>` changes it at runtime. Lock-free: use it to skip
expensive formatting (`if (api->log_level("me") >= V3X_LOG_DEBUG) dump();`).

### `const char *loader_version(void)` (v5)
Semantic version of the loader build, e.g. `"0.5.0"`. Independent of `api_version` (the table
layout) and of `game_version` / `game_build_id` (the game). See
[api-stability.md](/docs/api-stability/#three-version-numbers).

### `int build_matches(const char *game_builds)` (v5)
1 if the running game matches a `;`-separated list with exactly the manifest's `game_builds`
rules (see [Manifest](#manifest)), else 0. For plugins that switch single features per build:
`if (api->build_matches("release/1.15*")) install_feature_x();`.

## Return codes

v1-v4 return values are unchanged; v5 only gives them names (`include/v3x.h`):

| name | value | returned by |
|---|---|---|
| `V3X_OK` | 0 | success everywhere |
| `V3X_EFAIL` | -1 | bad arguments, table/queue full, unknown or disabled command, process exiting (`register_command`, `run_on_main`, `exec_*`, `on_event`, `emit_event`, `enum_commands`, `on_frame`, `hook_safe` bad args) |
| `V3X_EEXIST` | -2 | `register_command`: name taken; `on_event`: same (name, fn, user) |
| `V3X_EMH_NOT_INITIALIZED` | 2 | hooks: MinHook unavailable or process exiting |
| `V3X_EMH_ALREADY_CREATED` | 3 | `hook_safe`: this plugin already subscribed the same pre/post/user there |
| `V3X_EMH_NOT_CREATED` | 4 | `unhook`: nothing hooked there |
| `V3X_EMH_NOT_EXECUTABLE` | 7 | hooks: target is not executable memory |
| `V3X_EMH_UNSUPPORTED_FUNCTION` | 8 | hooks: the first instructions cannot be relocated |
| `V3X_EMH_MEMORY_ALLOC` | 9 | hooks: no trampoline memory, thunk pool full, 8 subscribers already |
| `V3X_EMH_MEMORY_PROTECT` | 10 | hooks: VirtualProtect failed |
| `V3X_EHOOKBUSY` | 100 | `hook` / `hook_safe` / `unhook`: another plugin holds the target |
| Win32 error | >0 | `patch`: `GetLastError()` of VirtualProtect (`ERROR_INVALID_PARAMETER` for NULL / 0 bytes since 0.5.0) |

Counts: `print` (chars), `emit_event` (subscribers called), `enum_commands` (commands).

## Manifest

```c
V3X_PLUGIN_MANIFEST("my_plugin", "1.2.0", "you", "one-line description", "release/1.15*;6af3094f-60fb000");
V3X_PLUGIN_MANIFEST_EX(name, version, author, desc, builds, V3X_MANIFEST_REQUIRE_BUILD, 4 /* min_api */);
```

| field | meaning |
|---|---|
| `struct_size` | set by the macro; lets the struct grow later |
| `flags` | `V3X_MANIFEST_REQUIRE_BUILD`: refuse to load on a build not listed |
| `min_api` | refuse to load on a loader older than this API version |
| `name`, `version`, `author`, `description` | shown by the `plugins` command (`name` replaces `v3x_plugin_name`) |
| `game_builds` | `;`-separated: `game_version()` strings (`release/1.15.1`), `game_build_id()` strings, or prefixes ending in `*` (`release/1.15*`). NULL, `""` or `"*"` = any. Case-insensitive |

A mismatch without `REQUIRE_BUILD` logs `WARNING: plugin 'x' lists game builds ...` and loads
anyway. Anchors remain the real safety check: a manifest says what you tested, anchors prove
the code is still what you expect.

## Built-in console commands

`help [command]`, `plugins` (name, version, file, status, then description and author),
`log <text>`, `version` (loader version, API version, game version, build id, exe base),
`loglevel [tag] [level]` (v5: show or set `log_at` thresholds), `reload [plugin]` (v5: emit
`config_reload` on the main thread), `quit`/`exit` (only explain how to close the game; the
console's close button is removed because closing a console window kills its process).

## Environment variables and `v3x.ini`

| setting | effect |
|---|---|
| `V3X_DISABLE=1` | loader does nothing (no hooks, no plugins) |
| `V3X_NO_CONSOLE=1` or `console=0` in `v3x.ini` | no console window |
| `[plugins]` `<dll stem> = 0` in `v3x.ini` | skip that plugin |
| `[log]` `level = <level>`, `<tag> = <level>` in `v3x.ini`; env `V3X_LOG_LEVEL` | `log_at` thresholds (v5) |
| `unknown_build = load` at the top of `v3x.ini` (before any `[section]`), or env `V3X_UNKNOWN_BUILD=load` | on a game build the loader does not know, load every plugin (default `safe`: only plugins whose manifest declares the build) |
| env `V3X_BUILD_GATE=1` | apply the unknown-build gate even when the host is not `victoria3.exe` (tests) |
| `v3x/loading.marker` (written by the loader) | names the plugin being loaded; if it survives a crash, that plugin is skipped once |

Plugins have their own (`V3X_PROFILE`, `V3X_DEVBRIDGE_PIPE`, `V3X_NO_TICKPROF`, ...), see
[plugins.md](/docs/plugins/).

## Exports of version.dll

Besides the 17 forwarded `version.dll` functions, the loader exports
`const V3X_API *v3x_api(void)` for tests and tools that are not plugins:
`GetProcAddress(GetModuleHandleA("version.dll"), "v3x_api")`. Since 0.5.0 also three data
exports that tools read from the file without loading it (`sdk/build_plugin.py check`):
`uint32_t v3x_loader_api` (API version), `char v3x_loader_semver[]` (loader version) and
`char v3x_loader_known_builds[]` (`;`-separated build ids the loader was tested on).
