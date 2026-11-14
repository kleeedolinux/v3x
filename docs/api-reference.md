# API reference (`include/v3x.h`, API v6, loader 0.6.0)

A plugin is a Windows x64 DLL in `binaries/v3x/plugins/`. The loader (`binaries/version.dll`,
source `src/core.c`) calls its `v3x_plugin_load(const V3X_API *api)` once and passes a table of
function pointers. The API is plain C, so any compiler works.

**Read [safe-coding.md](safe-coding.md) ("Writing safe plugins") first.** This page shows the safe way
to use every function first, with every result checked. Functions that can crash the game when used
wrongly (`hook`, `patch`, raw `pattern_scan` / `find_xref` results, raw memory reads) are marked with
an **Unsafe** box that says what can go wrong and how to contain it. Every C sample on this page
compiles with `-Wall -Wextra -Werror` (`python tools/check_doc_samples.py docs/api-reference.md`).
Samples use `api` (the table passed to `v3x_plugin_load`, kept in a `static const V3X_API *api;`) and
`TAG` (your plugin name).

What is guaranteed to stay the same across loader updates (layout, return codes, thread rules,
deprecation, game patches) is in [api-stability.md](api-stability.md). Why the loader checks what it
checks: [ADR 0002](adr/0002-trust-model.md) (which plugins may load) and
[ADR 0005](adr/0005-api-v6-containment.md) (what a loaded plugin may do).

## Exports a plugin provides

| export | required | meaning |
|---|---|---|
| `V3X_EXPORT int v3x_plugin_load(const V3X_API *api)` | yes | return 0 on success; anything else is logged as `FAILED: v3x_plugin_load returned N` and rolls the plugin back |
| `V3X_PLUGIN_MANIFEST_CAPS(name, version, author, description, game_builds, flags, min_api, capabilities)` | no (v6; recommended) | data export `v3x_plugin_manifest`, see [Manifest](#manifest) |
| `V3X_PLUGIN_MANIFEST(...)` / `V3X_PLUGIN_MANIFEST_EX(...)` | no (v4) | the same without capabilities (= everything allowed) |
| `V3X_EXPORT const char *v3x_plugin_name = "x";` | no | display name (default: the file name; the manifest's `name` wins) |

A minimal plugin that does everything the safe way:

<!-- sample: complete -->
```c
#include "v3x_sdk.h"
#define TAG "my_plugin"

/* Declare what you use: the loader refuses patch / exec_command for this plugin (API v6). */
V3X_PLUGIN_MANIFEST_CAPS("my_plugin", "1.0.0", "you", "counts frames", "*", 0u, 0u, "hook");

static const V3X_API *api;      /* set once in v3x_plugin_load, read-only afterwards */
static volatile int g_frames;   /* written on the main thread, read by the command: atomic */

static void on_frame(void *user)
{
    (void)user;
    v3x_atomic_add_int(&g_frames, 1);
}

static void cmd_frames(const char *args, void *user)   /* console input thread */
{
    (void)args; (void)user;
    api->print("%s: %d frames", TAG, v3x_atomic_load_int(&g_frames));
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    if (!V3X_HAS(api, log_at)) return 1;                               /* needs API v5: fail closed */
    if (api->on_frame(on_frame, NULL) != V3X_OK) return 1;             /* half-installed = not installed */
    if (api->register_command("frames", "frames counted so far", cmd_frames, NULL) != V3X_OK) return 1;
    return 0;                                                          /* a non-zero return rolls back */
}
```

### When `v3x_plugin_load` runs

On the game's **main thread**, from a hook on the exe's entry point, **before** the CRT starts,
before static constructors and before WinMain. So:

* code is mapped: scanning, `hook_safe` (and `hook` / `patch`) work;
* game globals do not exist yet (most are zero); the engine has not read its settings;
* the CRT of the *game* is not initialised (your own DLL's CRT is);
* do not block, do not wait for game threads (there are none yet).

Do the work that needs a running game in your hooks, or in `on_frame`. Work that needs **other
plugins** to be loaded (for example `v3x_script_get` from `sdk/v3x_script.h`) goes into a
`plugins_loaded` event handler (v6).

### Loading order and the checks before loading

The loader enumerates `binaries/v3x/plugins/*.dll` (`FindFirstFile` order, usually alphabetical).
For each file, **everything up to step 6 works on the file, before `LoadLibrary`**: a plugin that is
refused there is never mapped, so neither its `DllMain` nor its static constructors run (loader 0.6.0,
`src/v3x_pe.h`).

1. `[plugins]` `<file name without .dll> = 0` in `binaries/v3x/v3x.ini` (`off`, `no`, `false`;
   case-insensitive): `disabled (v3x.ini [plugins])`.
2. Crash-loop guard: if the game died while loading this plugin on the previous start
   (`v3x/loading.marker` still names it), it is skipped once: `disabled: crashed during load last
   start`.
3. The file is opened without write/delete sharing and read once; the handle stays open until
   `LoadLibrary` returned, so the bytes that are checked are the bytes that are mapped. SHA-256 of
   those bytes.
4. **Trust** ([below](#trust-which-plugin-dlls-load)): official (signed `plugins.json`), in your
   allowlist, or `V3X_TRUST=dev`. Otherwise `NOT LOADED: untrusted (type 'trust <plugin>' to allow
   it)`.
5. Bounded PE reader: a malformed file, or one without a `v3x_plugin_load` export, is refused.
6. The manifest (read from the file, so it must be a compile-time constant): `min_api` above the
   loader's API version, or a game-build mismatch with `V3X_MANIFEST_REQUIRE_BUILD`, refuses the plugin.
   On a game build the loader does not know (a patch), the
   [unknown-build gate](api-stability.md#game-patches) also skips plugins whose manifest does not
   declare that build. Capabilities are parsed (unknown words are logged and ignored).
7. `loading.marker` is written, then `LoadLibrary`. Your `DllMain` runs under the loader lock: do
   nothing there (see [safety.md](safety.md#dllmain)).
8. `v3x_plugin_load(&api)`, inside a fault guard. If it returns non-zero or faults, the loader
   **rolls the plugin back**: its hooks are disabled, its patches restored (newest first), its
   callbacks are never called, and `plugin_disabled` is emitted. Status:
   `FAILED: v3x_plugin_load returned N, rolled back` or `FAULTED in v3x_plugin_load, rolled back`.

After the last plugin the core emits `plugins_loaded` on the main thread.

### Trust: which plugin DLLs load

Since loader 0.6.0 a plugin DLL loads only if one of these holds
([ADR 0002](adr/0002-trust-model.md)):

| `plugins` shows | rule |
|---|---|
| `trust: official` | `binaries/v3x/plugins.json` lists the file name with this SHA-256 and `plugins.json.sig` is a valid release signature over it. `V3X-Setup.exe` installs both for the bundled plugins |
| `trust: trusted (allowlist)` | the SHA-256 is in `%LOCALAPPDATA%\V3X\trusted_plugins.txt` (one `<sha256> <file name>` line per plugin, `#` comments; only the hash counts). The console command `trust <plugin>` adds the hash that was refused this start; it applies at the next start |
| `trust: dev (unchecked)` | env `V3X_TRUST=dev` is set for the game process |

`trust` without an argument lists the plugins refused this start with their hashes. Rebuilding a
plugin changes its hash: allow it again, or develop with `V3X_TRUST=dev`
([getting-started.md](getting-started.md#4-run-it-2-minutes)). There is no trust key in `v3x.ini`.

> **Unsafe — when you really need it:** `V3X_TRUST=dev`.
> * When: you build and reload your own plugin many times a day.
> * What can go wrong: the check is off for **every** DLL in `binaries/v3x/plugins/` in that game
>   process, including one another program dropped there.
> * How to contain it: set it only for the game process you start for testing (not as a permanent
>   user variable on a machine you also play on), and keep the plugins folder free of DLLs you did
>   not build. `v3x.log` logs a WARNING at every start while it is on.

### Fault isolation

Every callback the loader itself calls (`v3x_plugin_load`, `on_frame`, `run_on_main`, console
commands, event subscribers, `enum_commands`) runs inside an SEH guard. An access violation, illegal
instruction, divide by zero, in-page error or an escaping C++ exception there is caught: v3x.log
gets `FAULT in plugin x.dll during <what>: exception 0x... (write of 0x...) at module+offset`,
the plugin's callbacks are skipped from then on (`plugins` shows `FAULTED in ...`), its hooks
stay, `plugin_disabled` is emitted, and the game keeps running. **Not isolated**: faults in your hook
detours / `hook_safe` pre/post (they run inside engine calls), in engine code you call or patched,
in `DllMain` and in threads you start. A fault isolated this way may have left your own locks or
data half-updated; that is why the whole plugin is switched off. Fault isolation is for stability, not
a security boundary.

## Contracts that hold for every function (API v6)

`include/v3x.h` writes these down above `V3X_API`; they apply to every entry below.

* **Ownership.** Every hook, patch and registration belongs to the **module** (DLL) that called the
  API, found from the return address. A tail call such as `return api->hook_safe(...)` from a callback
  the loader started counts for the plugin whose callback runs. Code that lives in no module (heap,
  generated code) on a thread where no plugin callback runs has the **unknown** owner, which owns
  nothing: `hook` / `hook_safe` / `unhook` return `V3X_EPERM`, `patch` `ERROR_ACCESS_DENIED`. The
  loader's own hooks (exe entry point, `PeekMessageW` frame hook, `RtlExitUserProcess`) belong to the
  core and cannot be removed (`V3X_EHOOKBUSY`). Call the API from your DLL's own code.
* **Pointers you pass** (names, help texts, patch bytes, `original` out-pointers) are read or written
  before the loader takes any lock: a bad pointer faults in your context and cannot leave a loader lock
  held.
* **Lifetimes.** Strings passed *to* your callbacks (command args, event names and core event data,
  `exec_capture` sink text, `enum_commands` name/help) are valid only during the callback: copy what you
  keep. Strings the loader returns (`game_version`, `game_dir`, `loader_version`, ...) live until
  process exit. Registrations stay until you remove them (`off_frame`, `off_event`,
  `unregister_command`) or your plugin is disabled. A call already running when a removal returns may
  still finish; no new call starts afterwards.
* **Limits** (truncation is silent unless stated): log line 2047 bytes incl. the prefix; `print` 8191
  bytes; `exec_*` command name 63 chars, arguments 2047 bytes; `register_command` name 31 chars (longer
  = -1), help 159 bytes (cut); event names 1..63 chars (longer = -1); `pattern_scan` 256 bytes (longer
  = NULL); `patch` `V3X_PATCH_MAX` = 65536 bytes per call; `find_xref` index < 0 = NULL.
* **Quotas** per plugin module: `V3X_QUOTA_FRAME` (16) `on_frame` callbacks, `V3X_QUOTA_EVENTS` (24)
  event subscriptions, `V3X_QUOTA_COMMANDS` (24) commands. Above that the call returns -1 like a full
  table and v3x.log names the quota. Global tables: 64 frame callbacks, 64 subscriptions, 128 commands,
  512 live patches.
* **Capabilities.** If your manifest declares capabilities, `hook` / `hook_safe` / `unhook` need
  `hook`, `patch` needs `patch`, `exec_command` / `exec_capture` need `exec`; otherwise `V3X_EPERM`
  (`patch`: `ERROR_ACCESS_DENIED`), logged once. `fs`, `net`, `script` are advisory.
* **Process exit.** Once `ExitProcess` has started, every function returns at once without effect.

## Versioning

* `api->api_version`: the loader's `V3X_API_VERSION` (1..6).
* `api->struct_size`: `sizeof(V3X_API)` of the loader.
* The table is **append-only**: fields are never moved or removed. A plugin built with a newer
  header runs on an older loader as long as it checks before using newer fields, and fails closed
  when a field it needs is missing:

<!-- sample: body -->
```c
if (!V3X_HAS(api, hook_safe)) {                        /* v4 field: check before use */
    api->log(TAG, "loader too old (API v%u), need v4: not installed", api->api_version);
    return 1;                                          /* fail closed: install nothing */
}
```

`V3X_HAS(api, field)` checks that `struct_size` covers the field and the pointer is set. For v2/v3
fields, `api->api_version >= N` also works. Or declare `min_api` in the manifest and an older loader
refuses the plugin before loading it.

| version | added |
|---|---|
| v1 | `log`, `image_base`, `image_size`, `game_version`, `pattern_scan`, `find_string`, `find_xref`, `function_start`, `hook`, `unhook`, `patch`, `on_frame`, `game_dir`, `v3x_dir` |
| v2 | `register_command`, `print`, `run_on_main`, `exec_command` |
| v3 | `exec_capture`, `on_event`, `emit_event` |
| v4 | `hook_safe`, `main_thread_id`, `game_build_id`, `enum_commands`; the plugin manifest; the `[plugins]` skip list |
| v5 | `log_at`, `log_level`, `loader_version`, `build_matches`; the `config_reload` event; named return codes and log levels; loader 0.5.0 hardening (fault isolation, rollback, crash-loop guard, unknown-build gate) |
| v6 | `off_frame`, `off_event`, `unregister_command`, `game_dir_w`, `v3x_dir_w`, `game_dir_utf8`, `v3x_dir_utf8`; manifest `capabilities`; `plugin_disabled` and `plugins_loaded` events; `V3X_EARG`, `V3X_ENOTFOUND`, `V3X_EPERM`; quotas; loader 0.6.0 trust check, pre-load checks, ownership and patch rules, reserved events, strict `pattern_scan` |

## Thread rules at a glance

| function | callable from | callbacks run on |
|---|---|---|
| `log`, `log_at`, `print` | any thread (serialized by one lock) | - |
| `image_base`, `image_size`, `game_version`, `game_build_id`, `game_dir`, `v3x_dir`, `*_dir_w`, `*_dir_utf8`, `main_thread_id`, `loader_version`, `build_matches` | any thread (constant after boot) | - |
| `log_level` | any thread, lock-free | - |
| `pattern_scan`, `find_string`, `find_xref`, `function_start` | any thread (read-only) | - |
| `hook_safe`, `hook`, `unhook` | any thread; best at load time. Other threads are briefly suspended (not for a 2nd+ `hook_safe` subscriber on an already hooked target) | pre / post / detour: the thread that calls the hooked function |
| `patch` | any thread; other threads are suspended while it writes, but a thread executing the patched bytes resumes in the middle of the new ones: patch at load time or code no thread runs | - |
| `on_frame`, `off_frame` | any thread (atomic) | main thread, once per message-pump iteration |
| `run_on_main` | any thread | main thread, next frame tick |
| `register_command`, `unregister_command` | any thread | console input thread, or the caller of `exec_command` / `exec_capture` |
| `exec_command`, `exec_capture` | any thread; the command runs synchronously on the caller. **Command handlers never run concurrently**: one recursive lock (nested `exec_*` from a handler is fine); another thread waits up to 5 s, then gets -1 | caller |
| `on_event`, `off_event` | any thread | the emitting thread (synchronously) |
| `emit_event` | any thread | - |
| `enum_commands` | any thread | caller |
| core events `config_reload`, `plugins_loaded` | - | main thread |
| core event `plugin_disabled` | - | the thread that disabled the plugin |
| core event `process_exit` | - | the thread calling `ExitProcess` |

## V3X_API fields

### `uint32_t api_version`, `uint32_t struct_size`
See [Versioning](#versioning).

### `void log(const char *tag, const char *fmt, ...)`
printf-style line to `binaries/v3x/v3x.log` and the V3X console, prefixed
`[hh:mm:ss.mmm][tag] `. The file is flushed after every line (it survives a crash). Lines
containing `FAIL`, `ERROR`, `EXCEPTION` or `WARNING` are shown red. Max line 2047 bytes. Any thread.
`fmt` is format-checked by the compiler (`V3X_PRINTF`). Text that is not yours (mod names, file
contents, engine strings) goes in **only as a `%s` argument**, cleaned with `v3x_sanitize_line`
(control characters and newlines would forge log lines that tools parse):

<!-- sample: file | static const V3X_API *api; -->
```c
static void report_mod(const char *mod_name)            /* from a mod file: untrusted */
{
    char clean[128];
    v3x_sanitize_line(clean, sizeof clean, mod_name);    /* control chars -> '?', cut on a UTF-8 boundary */
    api->log(TAG, "scanned mod %s", clean);              /* never api->log(TAG, mod_name) */
}
```

Do not log from very hot paths (every call takes a global lock and writes the file): count, and
report from a command.

### `uintptr_t image_base(void)`, `size_t image_size(void)`
Base address and `SizeOfImage` of `victoria3.exe` in memory. RVA = address - `image_base()`.
Use them to check that an address is inside the exe, not to compute engine addresses from RVAs.

### `const char *game_version(void)`
Contents of `<game>/caligula_branch.txt`, e.g. `"release/1.15.1"`; `"unknown"` if missing.

### `void *find_string(const char *s)`
Address of the exact NUL-terminated ASCII string `s` in `.rdata`/`.data` (a match must start
after a NUL byte, so substrings of longer strings are not returned). NULL if absent. Safe to call;
the result is data, not code.

### Finding engine code: use `V3xAnchor`

Never hook or patch an address that a single scan returned. Describe the target as an anchor
(`sdk/v3x_anchor.h`): a string every reference agrees on, the prologue bytes it must start with, and an
optional `check` callback. `v3x_anchor_resolve_all` resolves all anchors, verifies each one (unique,
same function, prologue) and logs why one failed; if any fails, install nothing:

<!-- sample: file | static const V3X_API *api; -->
```c
static V3xAnchor g_anchors[] = {
    {.name = "game_tick",
     .string = "Processing Tick: ",                           /* the function that logs this */
     .pattern = "48 89 5C 24 ?? 48 89 74 24 ?? 48 89 7C 24",  /* must match its first bytes */
     .xref = V3X_ANCHOR_ALL_SAME},                            /* every xref in ONE function */
};

static int find_targets(void)
{
    if (v3x_anchor_resolve_all(api, TAG, g_anchors, 1) != 0) return 1;   /* logged; fail closed */
    return 0;                                                            /* g_anchors[0].result is proven */
}
```

The raw scanners below are what anchors are built on.

### `void *pattern_scan(const char *ida_pattern)`
First match of an IDA-style pattern (`"48 8B 05 ?? ?? ?? ?? 48 85 C0"`) in the exe's `.text`, or NULL.
Strict since loader 0.6.0: tokens are separated by spaces/tabs and are exactly two hex digits or `?` /
`??`; any other token, or more than 256 bytes, returns NULL and logs a WARNING (a bad token used to be
read as `00`). Count matches with `v3x_scan_count` (`sdk/v3x_scan.h`) when you need to know whether a
pattern is unique.

### `void *find_xref(const void *target, int index)`
The `index`-th instruction (0 = first, in address order) in `.text` that references `target`
with a RIP-relative `lea`/`mov` (`48|4C 8D|8B modrm(rm=101) disp32`). NULL when there is no
such instruction or `index < 0`. Other encodings (`cmp`, `vmov*`, 32-bit `lea`) are not found; see
`v3x_xrefs` in `sdk/v3x_scan.h` for all of them at once.

### `void *function_start(const void *addr)`
Start of the function containing `addr`, from the exe's `.pdata` unwind table, following
chained unwind entries (MSVC splits hot/cold parts) back to the primary function. NULL for leaf
functions without unwind info.

> **Unsafe — when you really need it:** using a `pattern_scan` or `find_xref(s, 0)` result directly
> as a hook or patch target.
> * When: never for a target; at most for exploring in the devconsole.
> * What can go wrong: `pattern_scan` returns the **first** of possibly several matches; after a game
>   patch (or another plugin's patch) the first match is a different function, and the hook or patch
>   lands in the middle of unrelated code. `find_xref(s, 0)` picks one of several references to a
>   string. The crash happens later, somewhere else.
> * How to contain it: use `V3xAnchor` (above): a unique match (`v3x_scan_count == 1`) or
>   `V3X_ANCHOR_ALL_SAME`, a prologue `pattern`, a `check` callback, and fail closed when anything
>   does not match.

> **Unsafe — when you really need it:** reading engine memory through a pointer you computed
> (`*(uint64_t *)(obj + 0x120)`).
> * When: never directly; engine layouts change with patches.
> * What can go wrong: an access violation in a hook or a worker thread is the game's crash (not
>   isolated); a stale pointer reads freed memory.
> * How to contain it: read with `sdk/v3x_mem.h` (`v3x_mem_read_u64`, `v3x_mem_chain`,
>   `v3x_mem_read_cstr`), which checks the page and copies under a fault guard, and treat a failed
>   read as "feature off" ([safe-coding.md](safe-coding.md#reading-engine-memory)).

### `int hook_safe(void *target, v3x_hook_fn pre, v3x_hook_fn post, void *user)` (v4)
**The way to hook.** Hooks `target` with a core-generated pass-through thunk; you write no detour and
guess no prototype. Hook only an address an anchor proved, check the result, and keep pre-to-post
state in `call->scratch`:

<!-- sample: file | static const V3X_API *api; -->
```c
static volatile LONG64 g_calls;

static void count_pre(V3X_HookCall *call)
{
    call->scratch[0] = (uint64_t)GetTickCount64();   /* pre -> post state: per call, never a global */
    InterlockedIncrement64(&g_calls);                /* engine worker thread: atomic, no lock, no log */
}

static int install(void *proven_target)              /* from a resolved V3xAnchor */
{
    if (!V3X_HAS(api, hook_safe)) return 1;           /* loader too old: fail closed */
    int rc = api->hook_safe(proven_target, count_pre, NULL, NULL);
    if (rc != V3X_OK) {                               /* V3X_EHOOKBUSY, V3X_EPERM, 3, 9, ... */
        V3X_LOGE(api, TAG, "hook_safe FAILED (%d): feature off", rc);
        return 1;
    }
    return 0;
}
```

On every call:

1. the thunk saves `rcx, rdx, r8, r9`, the caller's first 12 stack argument slots and
   `xmm0..xmm3` into a `V3X_HookCall` on its own stack frame;
2. `pre(call)` runs (if set). It may change `args[]` / `xmm[]`, or set `skip_original = 1`
   and a replacement `ret` / `xmm_ret`;
3. unless skipped, the original runs with all 16 integer slots and xmm0-3 reloaded;
   `rax` -> `call->ret`, `xmm0` -> `call->xmm_ret`;
4. `post(call)` runs (if set) and may change `ret` / `xmm_ret`;
5. the thunk returns `ret` in rax and `xmm_ret` in xmm0. Every other volatile register is returned as
   the original left it.

`pre` or `post` may be NULL, not both. Returns 0, -1 `V3X_EARG` (bad args), `V3X_EPERM` (no `hook`
capability, or unknown owner), `V3X_EHOOKBUSY` (the target has a raw `hook`; v3x.log names the owner),
3 `MH_ERROR_ALREADY_CREATED` (this plugin already subscribed the same `pre`/`post`/`user` there), or
another `MH_STATUS` (9 = `MH_ERROR_MEMORY_ALLOC` also when the pool of 4096 hooked targets is full or
the target already has `V3X_HOOK_MAX_SUBSCRIBERS` = 8 subscribers). Remove with `unhook`. The thunk
has unwind info (`.pdata`), so C++ exceptions thrown through the hooked function, crash handlers and
profilers' stack walks pass through it. Overhead: about 40 instructions plus your callbacks per call.

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

<!-- sample: skip | header excerpt (the real definition is in include/v3x.h) -->
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
argument `i < 4` is in `xmm[i]`. **From the 5th argument on, floats are raw bits in `args[i]`**: copy
them into a local with `memcpy`, never cast the pointer. A function returning a struct > 8 bytes has a
hidden result pointer in `args[0]` and every argument shifts by one. Helpers: `V3X_ARG(call, i, type)`,
`V3X_ARGF(call, i)` (double), `V3X_ARGF32(call, i)` (float) in `sdk/v3x_hook.h`.

<!-- sample: file -->
```c
static void read_args_pre(V3X_HookCall *call)
{
    double scale = call->xmm[1].d;                    /* 2nd argument, a double: in xmm1 */
    double weight;
    memcpy(&weight, &call->args[5], sizeof weight);   /* 6th argument, a double: raw bits in args[5] */
    call->scratch[0] = scale > weight;                /* not *(double *)&call->args[5] */
}
```

The callbacks run on the thread that calls the hooked function, often an engine worker:
keep them short, do not take locks the engine might also need, do not call `log` thousands of
times per second, and do not run game script unless `v3x_script_random_allowed()` says so
(`sdk/v3x_game.h`). A fault in them is the game's crash (not isolated).

### `int hook(void *target, void *detour, void **original)`
Use [`hook_safe`](#int-hook_safevoid-target-v3x_hook_fn-pre-v3x_hook_fn-post-void-user-v4) instead.
MinHook inline hook: `target` now jumps to `detour`; `*original` receives a trampoline that runs
the original code (written before the hook is enabled). Returns 0, `V3X_EARG`, `V3X_EPERM` (no `hook`
capability, unknown owner) or a MinHook `MH_STATUS`. A raw hook is **exclusive**: if the target already
has a raw hook or `hook_safe` subscribers (any plugin), `hook` returns `V3X_EHOOKBUSY` (100) and
v3x.log names the holder, e.g.
`hook 00007FF6... by my_plugin.dll: V3X_EHOOKBUSY, target held by par_load.dll (hook_safe)`.
Raw detours are not chained on purpose (each would have to call the previous one's trampoline
with the exact prototype; one mistake breaks every plugin behind it).

> **Unsafe — when you really need it:** a raw detour with `api->hook(target, detour, &original)`.
> * When: you *replace* a function whose exact prototype you have proven, or the thunk's ~40 extra
>   instructions matter in a function called millions of times per second.
> * What can go wrong: a detour with fewer arguments than the real function corrupts stack
>   arguments ([safety.md](safety.md#argument-forwarding)); float arguments are lost if the detour
>   does floating-point work before calling the original; the hook is exclusive (`V3X_EHOOKBUSY` for
>   every other plugin); a fault in the detour crashes the game.
> * How to contain it: an anchor-proven target; count the arguments with
>   `tools/re_simopt_argcheck.py`; when unsure forward 8 slots with `V3X_PASS8_ARGS` / `V3X_PASS8`
>   (`sdk/v3x_hook.h`); call `original` only with the exact prototype; check the result and fail
>   closed; declare `"hook"` in the manifest.

Contained use (the steps above in code):

<!-- sample: file | static const V3X_API *api; -->
```c
static v3x_pass8_fn g_orig;           /* trampoline: valid forever once set (never freed) */
static volatile LONG64 g_hits;

static uint64_t my_detour(V3X_PASS8_ARGS)            /* forwards 8 slots: never fewer than the target */
{
    InterlockedIncrement64(&g_hits);                 /* no floating point before the call */
    return g_orig(V3X_PASS8);
}

static int install_raw(void *proven_target)          /* from a resolved V3xAnchor */
{
    int rc = api->hook(proven_target, (void *)my_detour, (void **)&g_orig);
    if (rc != V3X_OK) {                              /* V3X_EHOOKBUSY: someone else holds it */
        V3X_LOGE(api, TAG, "hook FAILED (%d): feature off", rc);
        return 1;
    }
    return 0;
}
```

### `int unhook(void *target)`
Removes what the **calling plugin** holds on `target`: its raw hook, or all of its `hook_safe`
subscriptions there. Other plugins' subscriptions on the same target stay; when the last subscriber
leaves, the thunk is disabled and the original bytes are restored. Returns 0, `V3X_EARG` for NULL
(since 0.6.0; it used to disable **every** hook in the process), `V3X_EHOOKBUSY` if only other plugins
or the core hold the target (nothing is removed), `V3X_EPERM` for the unknown owner, or 4
`MH_ERROR_NOT_CREATED` if nothing is hooked there (also for a target the loader never hooked; nothing
is touched). Unhook only **disables**: the trampoline (`original`) is never freed, because another
thread may be about to call it, and a later hook of the same target reuses it
([ADR 0003](adr/0003-hook-lifetime.md)). A thread may still be running inside your detour or callback:
keep its code and data alive (never `FreeLibrary` yourself). Rollback of a failed plugin disables the
same way.

<!-- sample: file | static const V3X_API *api; static void *g_target; -->
```c
static void feature_off(void)
{
    if (!g_target) return;                            /* never unhook(NULL) */
    int rc = api->unhook(g_target);
    if (rc != V3X_OK && rc != V3X_EMH_NOT_CREATED)
        V3X_LOGW(api, TAG, "unhook returned %d", rc);
    g_target = NULL;                                  /* callbacks and their data stay alive anyway */
}
```

### `int patch(void *addr, const void *bytes, size_t n)`
Writes `n` bytes over code or read-only data. **Prefer a hook** (`hook_safe` with `skip_original`
replaces a function's behaviour without writing code). 0 on success, otherwise a Win32 error code:

| error | when (loader 0.6.0) |
|---|---|
| `ERROR_INVALID_PARAMETER` | NULL / 0 bytes, or `n > V3X_PATCH_MAX` (65536) |
| `ERROR_INVALID_ADDRESS` | some byte of `[addr, addr+n)` is not committed and accessible; nothing written |
| `ERROR_ACCESS_DENIED` | the loader's memory (version.dll, its API table, its hook stub pool), another plugin's module, the first 16 bytes of a function hooked by another module or the core, the unknown owner, or a manifest that declares capabilities without `patch` |
| `ERROR_NOT_ENOUGH_MEMORY` | the record table (512 live patches) is full; nothing written (a patch is never applied untracked) |
| other | `VirtualProtect` failed (`GetLastError()`) |

Every page gets the least rights needed (read-write for data; read-write-execute only for code pages,
only while the bytes are copied) and its original protection back; other threads are suspended while
it writes (the same scheme and lock as MinHook). The replaced bytes are recorded, so a failed load
restores them.

> **Unsafe — when you really need it:** `api->patch(addr, bytes, n)`.
> * When: a one-byte branch flip or a constant that has no function boundary to hook.
> * What can go wrong: a thread executing those bytes at that moment resumes in the middle of the new
>   ones (suspension does not help a thread that is already inside them); a wrong address silently
>   corrupts code that runs minutes later; the next game patch moves everything.
> * How to contain it: find the site with a unique anchor; verify the **exact old bytes** with
>   `v3x_pattern_match_at` right before writing; patch in `v3x_plugin_load` (before the game's code
>   runs) or on the main thread between ticks; keep `n` minimal; check the result and fail closed;
>   declare `"patch"` in the manifest.

Contained use:

<!-- sample: file | static const V3X_API *api; -->
```c
static int flip_branch(uint8_t *site)                 /* from a unique anchor */
{
    V3xPattern old;
    if (!v3x_pattern_compile("74 0A 48 8B CB", &old)) return 1;    /* the bytes we expect: je +10 ... */
    if (!v3x_pattern_match_at(&old, site)) {                       /* not what we proved: refuse */
        V3X_LOGE(api, TAG, "patch site changed: not patched");
        return 1;
    }
    static const uint8_t jmp = 0xEB;                               /* je -> jmp, one byte */
    int err = api->patch(site, &jmp, 1);
    if (err != 0) {
        V3X_LOGE(api, TAG, "patch FAILED (Win32 error %d): feature off", err);
        return 1;
    }
    return 0;                                                      /* restored by rollback on failure */
}
```

### `int on_frame(v3x_frame_fn fn, void *user)`
Registers `fn(user)`, called on the main thread once per message-pump iteration (from a
`PeekMessageW` hook; about once per frame whenever the main thread pumps messages). Max 64 callbacks
for all plugins, `V3X_QUOTA_FRAME` (16) per plugin. Any thread may register (atomic);
`v3x_plugin_load` is the usual place. Returns 0 or -1 (table or quota full, `fn` NULL, process
exiting). Keep it cheap: it runs every frame on the thread that renders. Pair it with `off_frame` when
the work is done:

<!-- sample: file | static const V3X_API *api; -->
```c
static volatile int g_wait;   /* frames left */

static void wait_then_stop(void *user)
{
    (void)user;
    if (v3x_atomic_add_int(&g_wait, -1) > 0) return;           /* not yet (returns the new value) */
    api->log(TAG, "done after 300 frames");
    if (V3X_HAS(api, off_frame)) (void)api->off_frame(wait_then_stop, NULL);   /* safe from inside fn */
}

static int start_wait(void)
{
    v3x_atomic_store_int(&g_wait, 300);
    return api->on_frame(wait_then_stop, NULL) == V3X_OK ? 0 : 1;
}
```

### `int off_frame(v3x_frame_fn fn, void *user)` (v6)
Removes the `on_frame` registration `(fn, user)` that the calling module made (or whose `fn` lives in
it). Returns 0, `V3X_ENOTFOUND` (no such registration), `V3X_EPERM` (it belongs to another module),
`V3X_EARG`. Any thread; safe from inside `fn` itself (it is not called again). A call already running
on the main thread may finish. Check `V3X_HAS(api, off_frame)` first (see `on_frame` above).

### `const char *game_dir(void)`, `const char *v3x_dir(void)`
Absolute paths with a trailing backslash: `.../Victoria 3/binaries/` and
`.../Victoria 3/binaries/v3x/`, in the **ANSI** code page (frozen `const char *`). Lossy when the
folder name has characters outside that code page (boot logs a WARNING then); use the v6 getters
below when you open files. Never build paths with `snprintf(... "%s%s", dir, name)`: use
`v3x_plugin_file` / `v3x_path` (`sdk/v3x_util.h`) for your own files, `v3x_path_join_checked` for a name
that comes from data, and `v3x_file_write_atomic` to write:

<!-- sample: body -->
```c
char ini[512];
if (!v3x_plugin_file(api, TAG, ".ini", ini, sizeof ini)) return 1;   /* <v3x>/my_plugin.ini; NULL = too long */
```

### `const wchar_t *game_dir_w(void)`, `const wchar_t *v3x_dir_w(void)` (v6)
The same folders as UTF-16 with a trailing backslash, correct for any folder name. Use them with the
`...W` Win32 functions and `v3x_file_write_atomic_inW` (`sdk/v3x_util.h`). Valid until process exit; any thread.

### `const char *game_dir_utf8(void)`, `const char *v3x_dir_utf8(void)` (v6)
The same folders in UTF-8 (for logs, and for `MultiByteToWideChar(CP_UTF8, ...)`); `""` if the path
could not be read. Valid until process exit; any thread.

<!-- sample: body -->
```c
if (!V3X_HAS(api, v3x_dir_w)) return 1;                 /* older loader: fall back or fail closed */
static const char text[] = "ok\n";
/* <v3x>\my_plugin_report.txt: name checked, no reparse points, temp file + rename */
if (v3x_file_write_atomic_inW(api->v3x_dir_w(), L"my_plugin_report.txt", text, sizeof text - 1) != 0) return 1;
api->log(TAG, "report written to %smy_plugin_report.txt", api->v3x_dir_utf8());
```

### `int register_command(const char *name, const char *help, v3x_command_fn fn, void *user)` (v2)
Adds a console command. `name`: up to 31 chars, no spaces, case-insensitive; `help` up to 159 bytes.
`fn(args, user)` receives the rest of the line (leading spaces stripped, trailing newline removed,
never NULL, valid only during the call). Returns 0, -1 (table of 128 or quota of 24 full, bad args),
-2 `V3X_EEXIST` (name taken). Without a console window registration still succeeds; commands then run
only through `exec_command` (e.g. devbridge).

**`fn` runs on the console input thread** (or whoever called `exec_command`), not the main thread:
copy `args` into a slot you own, refuse what does not fit, and hand game-state work to `run_on_main`:

<!-- sample: file | static const V3X_API *api; -->
```c
static volatile int g_job_busy;    /* 0 = free, 1 = queued: claimed with cas */
static char g_job_arg[64];         /* written only by the thread that won the cas */

static void job_on_main(void *user)
{
    (void)user;
    if (V3X_ASSERT_MAIN(api)) {
        /* touch game state with g_job_arg here */
    }
    v3x_atomic_store_int(&g_job_busy, 0);                  /* release the slot */
}

static void cmd_set(const char *args, void *user)         /* console input thread */
{
    (void)user;
    if (!v3x_atomic_cas_int(&g_job_busy, 0, 1)) { api->print("busy, try again"); return; }
    V3xStr s;
    v3x_str_init(&s, g_job_arg, sizeof g_job_arg);
    if (v3x_str_append(&s, args) != V3X_R_OK) {            /* args are borrowed: copy, refuse if cut */
        api->print("argument too long (max %zu bytes)", sizeof g_job_arg - 1);
        v3x_atomic_store_int(&g_job_busy, 0);
        return;
    }
    if (api->run_on_main(job_on_main, NULL) != V3X_OK) {
        api->print("queue full");
        v3x_atomic_store_int(&g_job_busy, 0);
    }
}

static int add_commands(void)
{
    int rc = api->register_command("myset", "<value>  set the value (applied on the main thread)", cmd_set, NULL);
    if (rc != V3X_OK) { V3X_LOGE(api, TAG, "register_command myset FAILED (%d)", rc); return 1; }
    return 0;
}
```

### `int unregister_command(const char *name)` (v6)
Removes the command `name` (case-insensitive) that the calling module registered; the name is free
again afterwards. Returns 0, `V3X_ENOTFOUND`, `V3X_EPERM` (another module's command, or a built-in
command of the core), `V3X_EARG`. A handler already running may finish.

<!-- sample: body -->
```c
if (V3X_HAS(api, unregister_command)) {
    int rc = api->unregister_command("myset");
    if (rc != V3X_OK && rc != V3X_ENOTFOUND) api->log(TAG, "unregister_command myset: %d", rc);
}
```

### `int print(const char *fmt, ...)` (v2)
printf to the console (no tag) and `v3x.log`; a newline is added if missing; output over 8 KB
is truncated. Returns chars written or -1. Thread-safe. Use it for command output. The same rule as
`log`: foreign text only as a `%s` argument after `v3x_sanitize_line`.

### `int run_on_main(v3x_frame_fn fn, void *user)` (v2)
Queues `fn(user)` to run once on the main thread in the next frame tick (after the `on_frame`
callbacks). Up to 256 queued items. Returns 0 or -1 (queue full): check it, and keep `user` alive
until `fn` ran (a static slot, as in the `register_command` example, not a stack variable). The main
thread is the render/UI thread; it is **not** inside a simulation tick task, so it is not a place to run
game script (see [safety.md](safety.md#script-and-rng-scope)).

### `int exec_command(const char *line)` (v2)
Runs one console line synchronously on the calling thread. 0 if a command ran (also if it
faulted: the fault is isolated, see [Fault isolation](#fault-isolation)), -1 if unknown, if
its plugin was disabled after a fault, if another thread's command was still running after 5 s
(command handlers are serialised), or if your manifest declares capabilities without `exec` (v6).

<!-- sample: body -->
```c
if (api->exec_command("version") != V3X_OK) {          /* needs "exec" when capabilities are declared */
    api->log(TAG, "version command did not run");
}
```

### `int exec_capture(const char *line, void (*sink)(const char *text, void *user), void *user)` (v3)
Like `exec_command`, and every `print`/`log` made **on the calling thread** while the command
runs is also passed to `sink` (one call per print/log; print text with its `\n`, log text as the
full line; valid only during the sink call: copy with `V3xStr`). Output from other threads is not
captured. Nested captures use the inner sink. `sink` may be NULL. Needs `exec` like `exec_command`.

<!-- sample: file | static const V3X_API *api; -->
```c
static void collect(const char *text, void *user)
{
    (void)v3x_str_append((V3xStr *)user, text);   /* bounded; the truncated flag is checked once below */
}

static int capture_version(char *out, size_t cap)
{
    V3xStr s;
    v3x_str_init(&s, out, cap);
    if (api->exec_capture("version", collect, &s) != V3X_OK) return 1;
    return v3x_str_truncated(&s) ? 1 : 0;
}
```

### `int on_event(const char *name, void (*fn)(const char *name, void *data, void *user), void *user)` (v3)
Subscribes to a named event (1..63 chars, case-sensitive). 64 subscriptions for all plugins
together, `V3X_QUOTA_EVENTS` (24) per plugin; the same (name, fn, user) only once. Returns 0, -1 (full
/ quota / bad args), -2 `V3X_EEXIST` (already). Remove with `off_event`.

Events carry no emitter identity: any plugin may emit any non-reserved name. Treat `data` as
untrusted input: check for NULL, check `struct_size` for v6 payloads, and copy what you keep (it is
valid only during the call):

<!-- sample: file | static const V3X_API *api; -->
```c
static void on_disabled(const char *name, void *data, void *user)
{
    (void)name; (void)user;
    const V3X_PluginDisabled *d = (const V3X_PluginDisabled *)data;
    if (!d || d->struct_size < offsetof(V3X_PluginDisabled, reason) + sizeof d->reason) return;
    char who[64];
    v3x_sanitize_line(who, sizeof who, d->name ? d->name : "?");
    api->log(TAG, "plugin %s was disabled: stop calling into module %p", who, d->module);
}

static int subscribe(void)
{
    int rc = api->on_event(V3X_EVENT_PLUGIN_DISABLED, on_disabled, NULL);
    return rc == V3X_OK ? 0 : 1;
}
```

### `int off_event(const char *name, void (*fn)(const char *name, void *data, void *user), void *user)` (v6)
Removes the subscription `(name, fn, user)` the calling module made. Returns 0, `V3X_ENOTFOUND`,
`V3X_EPERM` (another module's), `V3X_EARG`. A subscriber removed during an emit is not called by that
emit if it had not been reached yet.

### `int emit_event(const char *name, void *data)` (v3)
Calls every subscriber synchronously on the calling thread, in subscription order, with `data`
untouched. Returns the number of subscribers called, -1 on bad args. **Reserved** (v6): the core's own
events `process_exit`, `plugin_disabled` and `plugins_loaded` cannot be emitted through the API (-1,
nobody is called). A payload you define for a new event should start with `uint32_t struct_size`, so
fields can be appended later.

<!-- sample: file | static const V3X_API *api; -->
```c
typedef struct MyEvent {
    uint32_t struct_size;     /* first field: subscribers check it before reading */
    uint32_t count;
} MyEvent;

static void announce(uint32_t count)
{
    MyEvent e = {(uint32_t)sizeof e, count};
    int n = api->emit_event("my_plugin_done", &e);   /* e lives until emit_event returns */
    if (n < 0) api->log(TAG, "emit_event failed");
}
```

Known events:

| event | emitter, thread | data |
|---|---|---|
| `plugins_loaded` (v6, reserved) | core, **main thread**, once after the last `v3x_plugin_load` returned | NULL |
| `plugin_disabled` (v6, reserved) | core, the thread that disabled the plugin (fault guard, rollback), before any of its callbacks can run again | `const V3X_PluginDisabled *` {`struct_size`, `reserved`, `name` (file stem), `module` (HMODULE), `reason`} |
| `process_exit` (reserved) | core, the thread calling `ExitProcess`, while every thread still runs | `const LONG *` exit code |
| `config_reload` (v5) | core, **main thread**, next frame tick after the console command `reload [plugin]` | `const char *` plugin name, or NULL = all plugins (`v3x_reload_is_for`) |
| `load_transition` | fast_load, main, when a load-state transition ends | `const V3X_LoadTransition *` {`from`, `to`, `seconds`}, may be NULL |
| `load_transition_begin` | fast_load, main, when one begins | `const V3X_LoadTransition *` (`seconds` = 0), may be NULL |
| `save_begin` | enhance, the saving thread, right before `SaveGameState` | `const V3X_SaveBegin *` {`name`} |
| `save_end` | enhance, same thread, after the save returned | same pointer |

The names are also macros: `V3X_EVENT_PLUGINS_LOADED`, `V3X_EVENT_PLUGIN_DISABLED`,
`V3X_EVENT_PROCESS_EXIT`, `V3X_EVENT_CONFIG_RELOAD`, `V3X_EVENT_LOAD_TRANSITION`,
`V3X_EVENT_LOAD_TRANSITION_BEGIN`, `V3X_EVENT_SAVE_BEGIN`, `V3X_EVENT_SAVE_END`. A subscriber of a
plugin that was disabled is skipped (the disabled plugin's own `plugin_disabled` subscribers are not
called); `emit_event` counts only the subscribers it called.

* `plugin_disabled`: a registry that calls other plugins' code (script_ext keywords, your own
  callback lists) must stop calling into `data->module`.
* `plugins_loaded`: the place for work that needs other plugins, e.g. `v3x_script_get`.
* `process_exit` subscribers must be short and must not wait for other threads without a timeout.
* `save_begin` runs **outside** any tick task: never run game script from it.

### `uint32_t main_thread_id(void)` (v4)
Win32 thread id of the game's main thread (the entry-point thread; `on_frame` and
`run_on_main` run there). `V3X_ASSERT_MAIN(api)` (`sdk/v3x_safe.h`) and `v3x_on_main_thread(api)`
(`sdk/v3x_game.h`) wrap it.

### `const char *game_build_id(void)` (v4)
Identity of the running exe from its PE header: `"<TimeDateStamp>-<SizeOfImage>"` in hex, e.g.
`"6af3094f-60fb000"` for 1.15.1. It changes with every game patch, also hotfixes that keep
`caligula_branch.txt`. Use it in your manifest's `game_builds` to pin a plugin to a build.

### `int enum_commands(void (*fn)(const char *name, const char *help, void *user), void *user)` (v4)
Calls `fn` for every registered console command in registration order (strings valid only during
the call). Returns the count, -1 on bad args. For completion UIs and tools.

### `void log_at(int level, const char *tag, const char *fmt, ...)` (v5)
`log` with a level: `V3X_LOG_ERROR` (1), `V3X_LOG_WARN` (2), `V3X_LOG_INFO` (3),
`V3X_LOG_DEBUG` (4), `V3X_LOG_TRACE` (5); out-of-range levels are clamped. Written only if
`level <= log_level(tag)`. ERROR/WARN/DEBUG/TRACE lines get `ERROR: ` / `WARNING: ` / `debug: ` /
`trace: ` after the tag (the first two are shown red in the console). Any thread.
`V3X_LOGE/W/I/D/T(api, tag, ...)` in `sdk/v3x_util.h` use it and fall back to `log` on older
loaders (dropping debug/trace there).

<!-- sample: body -->
```c
V3X_LOGW(api, TAG, "anchor %s not found: feature off", "game_tick");
```

### `int log_level(const char *tag)` (v5)
Current threshold for `tag` (NULL = global): `v3x.ini` `[log]` `<tag> = <level>`, else `[log]`
`level = <level>`, else env `V3X_LOG_LEVEL`, else info. Levels: `off` (0), `error`, `warn`,
`info`, `debug`, `trace` or `0`-`5`. Tags are case-insensitive, up to 31 chars, 32 tags. The
console command `loglevel [tag] <level>` changes it at runtime. Lock-free: use it to skip
expensive formatting.

<!-- sample: body -->
```c
if (V3X_HAS(api, log_level) && api->log_level(TAG) >= V3X_LOG_DEBUG)
    V3X_LOGD(api, TAG, "expensive dump: %d entries", 42);
```

### `const char *loader_version(void)` (v5)
Semantic version of the loader build, e.g. `"0.6.0"`. Independent of `api_version` (the table
layout) and of `game_version` / `game_build_id` (the game). Report it in bug reports; never gate
features on it (use `V3X_HAS`). See [api-stability.md](api-stability.md#three-version-numbers).

### `int build_matches(const char *game_builds)` (v5)
1 if the running game matches a `;`-separated list with exactly the manifest's `game_builds`
rules (see [Manifest](#manifest)), else 0. For plugins that switch single features per build; the
feature's anchors stay the real check:

<!-- sample: body -->
```c
if (V3X_HAS(api, build_matches) && !api->build_matches("release/1.15*")) {
    V3X_LOGI(api, TAG, "feature x is only tested on 1.15: off");
    return 0;                                            /* the rest of the plugin still loads */
}
```

## Return codes

v1-v5 return values are unchanged; v5 gives them names, v6 adds three (`include/v3x.h`):

| name | value | returned by |
|---|---|---|
| `V3X_OK` | 0 | success everywhere |
| `V3X_EFAIL` / `V3X_EARG` | -1 | bad arguments (`V3X_EARG`, v6 name), table/queue/quota full, unknown or disabled command, process exiting, reserved event (`register_command`, `run_on_main`, `exec_*`, `on_event`, `emit_event`, `enum_commands`, `on_frame`, `hook_safe` / `hook` / `unhook` bad args, `off_*`) |
| `V3X_EEXIST` | -2 | `register_command`: name taken; `on_event`: same (name, fn, user) |
| `V3X_ENOTFOUND` | -3 | `off_frame`, `off_event`, `unregister_command`: no such registration (v6) |
| `V3X_EPERM` | -4 | the caller may not: unknown owner, another module's or the core's registration / hook, or the capability is not declared (v6) |
| `V3X_EMH_NOT_INITIALIZED` | 2 | hooks: MinHook unavailable or process exiting |
| `V3X_EMH_ALREADY_CREATED` | 3 | `hook_safe`: this plugin already subscribed the same pre/post/user there |
| `V3X_EMH_NOT_CREATED` | 4 | `unhook`: nothing hooked there |
| `V3X_EMH_NOT_EXECUTABLE` | 7 | hooks: target is not executable memory |
| `V3X_EMH_UNSUPPORTED_FUNCTION` | 8 | hooks: the first instructions cannot be relocated |
| `V3X_EMH_MEMORY_ALLOC` | 9 | hooks: no trampoline memory, thunk pool full, 8 subscribers already |
| `V3X_EMH_MEMORY_PROTECT` | 10 | hooks: VirtualProtect failed |
| `V3X_EHOOKBUSY` | 100 | `hook` / `hook_safe` / `unhook`: another plugin or the core holds the target |
| Win32 error | >0 | `patch` (see its table) |

Counts: `print` (chars), `emit_event` (subscribers called), `enum_commands` (commands).

## Manifest

<!-- sample: file -->
```c
V3X_PLUGIN_MANIFEST_CAPS("my_plugin", "1.2.0", "you", "one-line description",
                         "release/1.15*;6af3094f-60fb000",   /* builds you tested */
                         0u,                                 /* flags: V3X_MANIFEST_REQUIRE_BUILD to refuse others */
                         5u,                                 /* min_api: refuse loaders older than API v5 */
                         "hook fs");                         /* capabilities (v6) */
```

`V3X_PLUGIN_MANIFEST(name, version, author, desc, builds)` and
`V3X_PLUGIN_MANIFEST_EX(..., flags, min_api)` are the same without capabilities. The manifest is read
from the DLL **file** before `LoadLibrary`, so it must be a compile-time constant (the macros make it
one); string fields are read up to 4095 bytes and must point into the DLL's own image.

| field | meaning |
|---|---|
| `struct_size` | set by the macro; lets the struct grow later (64 bytes in v6) |
| `flags` | `V3X_MANIFEST_REQUIRE_BUILD`: refuse to load on a build not listed |
| `min_api` | refuse to load on a loader older than this API version |
| `name`, `version`, `author`, `description` | shown by the `plugins` command (`name` replaces `v3x_plugin_name`) |
| `game_builds` | `;`-separated: `game_version()` strings (`release/1.15.1`), `game_build_id()` strings, or prefixes ending in `*` (`release/1.15*`). NULL, `""` or `"*"` = any. Case-insensitive |
| `capabilities` (v6) | comma/space separated subset of `hook patch fs net script exec`. NULL = undeclared = everything allowed. Declared: `hook`, `patch`, `exec` are **enforced** (calls without them return `V3X_EPERM` / `ERROR_ACCESS_DENIED`, logged once); `fs`, `net`, `script` are advisory (shown by `plugins`; a native DLL cannot be stopped from calling the OS). Unknown words are logged and ignored. Older loaders ignore the field |

Declare the smallest set you use: a bug or a hijacked code path in your plugin then cannot patch code
or run console commands through the API. A mismatch of `game_builds` without `REQUIRE_BUILD` logs
`WARNING: plugin 'x' lists game builds ...` and loads anyway. Anchors remain the real safety check: a
manifest says what you tested, anchors prove the code is still what you expect.

## Built-in console commands

`help [command]`, `plugins` (name, version, file, status, then description and author, then trust and
capabilities), `trust [plugin]` (v6: list the plugins refused as untrusted / allow one at the next
start), `log <text>`, `version` (loader version, API version, game version, build id, exe base),
`loglevel [tag] [level]` (v5: show or set `log_at` thresholds), `reload [plugin]` (v5: emit
`config_reload` on the main thread), `quit`/`exit` (only explain how to close the game; the
console's close button is removed because closing a console window kills its process). Built-in
commands belong to the core: `unregister_command` refuses them.

## Environment variables and `v3x.ini`

| setting | effect |
|---|---|
| `V3X_DISABLE=1` | loader does nothing (no hooks, no plugins) |
| `V3X_NO_CONSOLE=1` or `console=0` in `v3x.ini` | no console window |
| `V3X_TRUST=dev` | load plugin DLLs without the trust check (development only, see [Trust](#trust-which-plugin-dlls-load)) |
| `%LOCALAPPDATA%\V3X\trusted_plugins.txt` | your allowlist of plugin hashes (written by `trust <plugin>`) |
| `[plugins]` `<dll stem> = 0` in `v3x.ini` | skip that plugin |
| `[log]` `level = <level>`, `<tag> = <level>` in `v3x.ini`; env `V3X_LOG_LEVEL` | `log_at` thresholds (v5) |
| `unknown_build = load` at the top of `v3x.ini` (before any `[section]`), or env `V3X_UNKNOWN_BUILD=load` | on a game build the loader does not know, load every plugin (default `safe`: only plugins whose manifest declares the build) |
| env `V3X_BUILD_GATE=1` | apply the unknown-build gate even when the host is not `victoria3.exe` (tests) |
| `v3x/loading.marker` (written by the loader) | names the plugin being loaded; if it survives a crash, that plugin is skipped once |

Plugins have their own (`V3X_PROFILE`, `V3X_DEVBRIDGE_PIPE`, `V3X_NO_TICKPROF`, ...), see
[plugins.md](plugins.md) and [configuration.md](configuration.md).

## Exports of version.dll

Besides the 17 forwarded `version.dll` functions, the loader exports
`const V3X_API *v3x_api(void)` for tests and tools that are not plugins:
`GetProcAddress(GetModuleHandleA("version.dll"), "v3x_api")`. Calls through it come from a module that
is not a plugin (no capability restrictions, no plugin quotas); from heap code they have the unknown
owner. Since 0.5.0 also three data exports that tools read from the file without loading it
(`sdk/build_plugin.py check`): `uint32_t v3x_loader_api` (API version), `char v3x_loader_semver[]`
(loader version) and `char v3x_loader_known_builds[]` (`;`-separated build ids the loader was tested on).
