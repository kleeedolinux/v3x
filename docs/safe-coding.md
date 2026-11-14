# Writing safe plugins

A V3X plugin is C running inside `victoria3.exe` with no sandbox. C cannot be made memory-safe, but
the bugs that crashed or corrupted the game while the bundled plugins were written fall into a few
classes. Each one has a checked replacement in the SDK. This page gives the rules ("Rust-like":
every buffer has an owner and a size, every read is checked, every result is looked at) and the
safe API for each rule. The unsafe way is shown only in boxes marked **Unsafe**, with what can go
wrong and how to contain it.

[safety.md](safety.md) has the engine-specific rules (script and RNG scope, DllMain, patching
while workers run), each with the crash it came from. [api-reference.md](api-reference.md) has
every API function. Every C sample on this page is compiled by `tools/check_doc_samples.py`
(`t doc_samples` in `test/run_all.sh`), so the code you copy builds.

## The rules in one screen

1. **Every buffer has an owner and a size.** You never keep a pointer the loader lent you for one
   callback. You never write to a buffer without its size ([Ownership](#ownership-and-lifetimes)).
2. **Strings are built with `V3xStr`**, never with `strcpy`, `strcat`, `sprintf` or
   `n += snprintf(...)` ([Bounded strings](#bounded-strings)).
3. **Engine memory is read with `sdk/v3x_mem.h`**, never by dereferencing a pointer you worked out
   yourself ([Engine memory](#reading-engine-memory)).
4. **Bytes from files are parsed with `V3xSpan`** and sizes are multiplied with `v3x_mul_ok`
   ([Parsing](#parsing-files-and-buffers), [Integers](#integer-overflow)).
5. **Shared state is atomic, game state is touched on the main thread** (`v3x_atomic_*`,
   `V3X_ASSERT_MAIN`, `run_on_main`) ([Threads](#threads)).
6. **Hooks are `hook_safe`** on an address an anchor proved, and fail closed
   ([Hooks](#hooks)).
7. **Paths from data go through `v3x_path_join_checked`; files are written atomically**
   ([Files](#file-io)).
8. **Every result is checked.** `V3X_MUST_USE` makes the compiler say so
   ([Results](#check-every-result)).
9. **Know what fault isolation does not catch** ([Fault isolation](#fault-isolation-and-what-it-cannot-catch)).
10. **Test offline, fuzz parsers, build with `-Werror` and UBSan** ([Testing](#testing)).

Include everything with `#include "v3x_sdk.h"` and compile with `-Iinclude -Isdk`. The
template ([templates/plugin_template/](../templates/plugin_template/)) follows every rule and builds
with `-Werror`; start from it.

## Ownership and lifetimes

Who owns each buffer the API hands you, and how long you may use it:

| what | owner | valid | you may |
|---|---|---|---|
| `const V3X_API *api` passed to `v3x_plugin_load` | loader | until process exit | store the pointer, call it from any thread |
| strings the loader returns: `game_version`, `game_build_id`, `game_dir`, `v3x_dir`, `loader_version`, `game_dir_w` / `v3x_dir_w` / `*_utf8` (v6) | loader | until process exit | read, keep the pointer; never write or free |
| `args` of a console command, `name` and `data` of an event, `text` of an `exec_capture` sink, `name`/`help` in `enum_commands` | loader (or the emitter) | **only during that callback** | read; copy what you keep into your own buffer |
| `V3X_HookCall *call` in `hook_safe` pre/post | the hooked thread's stack | only during that pre/post call | read/modify `args`, `ret`, `scratch`; never store the pointer; pass state pre to post in `call->scratch[0..1]` |
| `call->original` / the `original` of a raw hook | loader | until process exit (never freed, [ADR 0003](adr/0003-hook-lifetime.md)) | call it with the exact prototype |
| names, help texts, patch bytes you pass in | you | the loader copies (or reads) them before it returns | pass stack buffers |
| the `user` pointer you register with `on_frame`, `on_event`, `register_command`, `run_on_main`, `hook_safe` | you | must stay valid while registered, and until a call that started before the removal finished | use static storage (your DLL is never unloaded) |
| manifest strings | you | forever | string literals only |
| `V3xIni`: `v3x_ini_get` results | the `V3xIni` | until `v3x_ini_free` | copy with `v3x_ini_str` |
| `V3xStr`, `V3xSpan` | they **borrow** your buffer | while the buffer lives | keep the buffer alive longer than the builder or reader |
| `V3xAnchor.result`, `.err` | your anchor table | forever (static) | read after resolving |
| data you `emit_event` | you | during the emit (subscribers run synchronously) | a stack struct is fine |

SDK naming follows the same idea: `*_new` / `*_free` return memory you own and must free with the
matching function; `*_view` and `const` pointers into a caller buffer only borrow it.

Copying a callback argument you want to keep:

<!-- sample: file -->
```c
static char g_last_save[128];   /* ours: static, sized */

static void on_save(const char *event, void *data, void *user)
{
    (void)event; (void)user;
    const V3X_SaveBegin *sb = (const V3X_SaveBegin *)data;   /* valid only during this call */
    V3xStr s;
    v3x_str_init(&s, g_last_save, sizeof g_last_save);
    if (sb && sb->name) (void)v3x_str_append(&s, sb->name);   /* copied, cut to fit */
}
```

## Bounded strings

`V3xStr` (`sdk/v3x_safe.h`) builds a string in a buffer you own. It never writes past `cap`, keeps
the buffer NUL-terminated, cuts at a UTF-8 boundary, and after the first append that does not fit
it sets `truncated` and turns every later append into a no-op (the content is always a clean
prefix). Check once at the end:

<!-- sample: body | const char *key = "speed"; int val = 5; -->
```c
char line[256];
V3xStr s;
v3x_str_init(&s, line, sizeof line);
(void)v3x_str_append(&s, "settings: ");
(void)v3x_str_appendf(&s, "%s=%d ", key, val);
(void)v3x_str_appendc(&s, '\n');
if (v3x_str_truncated(&s)) V3X_LOGW(api, TAG, "settings line cut at %zu bytes", v3x_str_len(&s));
api->print("%s", v3x_str_cstr(&s));
```

* Each append returns `V3X_R_OK` or `V3X_R_ETRUNC`. Define `V3X_SAFE_STRICT` before including the
  SDK to make ignoring them a warning, if you prefer per-call checks.
* **Never pass text you did not write as the format string.** `api->log(TAG, "%s", text)`, not
  `api->log(TAG, text)`. `log`, `print`, `log_at`, `v3x_logf` and `v3x_str_appendf` carry
  `V3X_PRINTF`, so `-Wformat=2` catches mismatched arguments; put `V3X_PRINTF(fmt, args)` on your
  own variadic helpers too.
* **Sanitize text from mods or files before logging it.** Tools parse `v3x.log`; a mod name with a
  newline or a bidi override could forge or hide a line:

<!-- sample: body | const char *mod_name = "x"; -->
```c
char clean[128];
v3x_sanitize_line(clean, sizeof clean, mod_name);   /* control, C1 and bidi characters -> '?' */
V3X_LOGW(api, TAG, "mod \"%s\" has no descriptor", clean);
```

* `snprintf(dst, sizeof dst, "%s", src)` is fine for a single copy; the banned form is the
  accumulating `n += snprintf(buf + n, sizeof buf - n, ...)`: once `n` passes the size, the size
  argument wraps around and the next call writes past the buffer. `tools/check_unsafe.sh` fails
  the build on it and on `strcpy`, `strcat`, `sprintf`, `vsprintf`, `gets`, `wcscpy`, `wcscat`,
  `lstrcpy*`, `lstrcat*`, `wsprintf*`.

## Reading engine memory

Game memory changes under you: other threads free objects, mimalloc decommits pages, and a
pointer chain you reverse-engineered on one build is garbage on the next. A plain dereference that
faults on the console thread or an engine worker kills the game. `sdk/v3x_mem.h` never
dereferences: it checks the range with `VirtualQuery` and copies it with `ReadProcessMemory`,
which fails instead of faulting even if the page disappears between check and copy.

<!-- sample: file | static const V3X_API *api; static uintptr_t g_manager_global; -->
```c
/* console command: print the name of the object [[global]+0x120]+0x1c00 points to */
static void cmd_mgr(const char *args, void *user)
{
    (void)args; (void)user;
    static const uint32_t chain[] = {0x120, 0x1c00};
    uintptr_t mgr, name_ptr;
    char name[64];
    if (!v3x_mem_chain(g_manager_global, chain, 2, &mgr)) { api->print("manager not readable"); return; }
    if (!v3x_mem_read_ptr(mgr + 0x10, &name_ptr) || v3x_mem_read_cstr(name_ptr, name, sizeof name) < 0) {
        api->print("manager name not readable");
        return;
    }
    char clean[64];
    v3x_sanitize_line(clean, sizeof clean, name);
    api->print("manager %p: %s", (void *)mgr, clean);
}
```

| helper | does |
|---|---|
| `v3x_mem_read(addr, out, n)`, `_u8/_u32/_u64/_ptr` | copies exactly n bytes; 1 = ok, 0 = some byte unreadable (never faults) |
| `v3x_mem_chain(base, offsets, n, &out)` | `p = *(p + off)` for each offset; 0 if any step is unreadable or NULL |
| `v3x_mem_read_cstr(addr, buf, cap)` | NUL-terminated copy, cut at `cap` or at the end of readable memory; -1 if nothing is readable |
| `v3x_mem_readable(addr, n)`, `v3x_mem_is_code(addr)` | committed + readable / executable right now |
| `v3x_mem_in_module(base, size, addr, n)` | `[addr, addr+n)` lies inside a module (overflow-safe) |

A checked read costs about 1 us (a `VirtualQuery` and a syscall). Resolve and validate pointers
once (at load or on the first frame), then keep the validated values; do not do it millions of
times per second in a hot hook.

> **Unsafe — when you really need it:** a raw dereference (`*(uint64_t *)(p + 0x120)`) or a
> `memcpy` from an engine pointer.
> * When: in a hot hook, on a pointer the engine just passed you as an argument (`call->args[i]`),
>   with a layout you verified on this build, or on a `.data` global of the exe (it never moves).
> * What can go wrong: the object was freed, the offset moved with a patch, the page was
>   decommitted: an access violation inside an engine call, which fault isolation does NOT catch
>   (the game crashes), or a silent read of garbage that you then act on.
> * How to contain it: prove the offset with an anchor `check` callback (the instruction that
>   uses it, see [Hooks](#hooks)); validate the pointer once with `v3x_mem_readable` and
>   `v3x_mem_in_module`; read raw only inside the call that handed you the pointer; mark the line
>   `/* v3x-unsafe-ok: <why> */`.

## Parsing files and buffers

Every file is untrusted input: mod content, caches you wrote yourself last run, `v3x.ini`, files
under `binaries/v3x/` (Steam makes the game folder writable for every local user;
[ADR 0002](adr/0002-trust-model.md)). Parse bytes with `V3xSpan`: every read checks the remaining
length, the first failure sets a sticky error, later reads fail too and outputs are zeroed, so one
check at the end is enough:

<!-- sample: file -->
```c
typedef struct Entry { uint32_t rva, len; } Entry;

/* cache file: "MYC1", u32 version, u32 count, count x {u32 rva, u32 len}. NULL = rebuild the cache. */
static Entry *parse_cache(const uint8_t *data, size_t size, uint32_t *count_out)
{
    V3xSpan r;
    uint32_t version = 0, count = 0;
    size_t bytes;
    v3x_span_init(&r, data, size);
    const uint8_t *magic = v3x_span_take(&r, 4);
    (void)v3x_span_u32le(&r, &version);
    (void)v3x_span_u32le(&r, &count);
    if (v3x_span_failed(&r) || memcmp(magic, "MYC1", 4) != 0 || version != 1) return NULL;
    /* the count comes from the file: prove it fits BEFORE allocating */
    if (count == 0 || !v3x_mul_ok(count, 8, &bytes) || bytes > v3x_span_remaining(&r)) return NULL;
    Entry *e = (Entry *)v3x_alloc_array(count, sizeof *e);   /* calloc with an overflow check */
    if (!e) return NULL;
    for (uint32_t i = 0; i < count; i++) {
        (void)v3x_span_u32le(&r, &e[i].rva);
        (void)v3x_span_u32le(&r, &e[i].len);
    }
    if (v3x_span_failed(&r)) { free(e); return NULL; }
    *count_out = count;
    return e;
}
```

* `v3x_span_take(&r, n)` returns a pointer to the next n bytes or NULL; `v3x_span_sub(&r, n)` a
  sub-reader limited to them (for a length-prefixed record); `v3x_span_skip` skips.
* A cache starts with magic, version and length, and is rebuilt on any mismatch. A path or offset
  read from a cache is checked like any other input.
* Every new parser gets a fuzz harness ([Testing](#testing)).

## Integer overflow

Sizes and offsets from files and from engine memory are attacker- or bug-controlled. `count * size`
wraps silently in C, and the allocation that follows is too small.

| instead of | use |
|---|---|
| `malloc(n * sizeof(T))` | `v3x_alloc_array(n, sizeof(T))` (NULL on overflow, zeroed) |
| `realloc(p, n * sizeof(T))` | `v3x_realloc_array(p, n, sizeof(T))` |
| `a + b`, `a * b` on sizes | `v3x_add_ok(a, b, &r)`, `v3x_mul_ok(a, b, &r)` (1 = ok) |
| 32-bit offsets / 64-bit signed | `v3x_add_ok_u32`, `v3x_add_ok_i64` |
| `if (off + len <= size)` | `if (len <= size && off <= size - len)` (cannot wrap) |
| `(int)strlen(s)`, `(DWORD)size` | keep `size_t`; clamp before narrowing |

Clamp values from config before use (`if (n < 0) n = 0; if (n > MAX) n = MAX;`), as the template
does for `log_every`.

## Threads

Which thread runs your code (the full table is in
[api-reference.md](api-reference.md#thread-rules-at-a-glance)):

| code | thread | may touch game state? |
|---|---|---|
| `v3x_plugin_load` | main thread, before the game's CRT and WinMain | no (it does not exist yet) |
| `on_frame`, `run_on_main` callbacks, `config_reload` handlers | main thread | UI-side state; not a tick task (no script) |
| console command handlers | the thread that ran the command: console input thread, devbridge pipe thread, or the caller of `exec_command` | no: post the work with `run_on_main` |
| `hook_safe` pre/post, raw detours | whatever thread calls the hooked function (often an engine worker) | only what that engine function itself may touch |
| event subscribers | the emitting thread, synchronously | depends on the event |
| threads you create | yours | no |

Rules:

* **Shared variables are atomic.** A flag or counter used by two threads is `volatile int` read and
  written with `v3x_atomic_load_int` / `_store_int` / `_cas_int` / `_add_int` (pointers:
  `v3x_atomic_*_ptr`; 64-bit counters: `InterlockedIncrement64` / `InterlockedAdd64`). Never a
  plain read-modify-write.
* **Assert the main thread** in code that touches game state: `if (!V3X_ASSERT_MAIN(api)) return;`
  logs an ERROR once and returns 0 on any other thread.
* **No locks in hooks** that your other code holds while calling into the engine (deadlock), no
  waiting without a timeout, no allocation while another thread is suspended, no `log` per call in
  hot functions (count, report from a command).
* **Process exit:** do the final work in the `process_exit` event, short and without waiting for
  other threads; nothing in `DllMain`.

A console command that changes game state hands the work to the main thread. The job slot is
static (its lifetime outlives the queue) and claimed with a compare-and-swap, so two commands
cannot overwrite each other's argument:

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
    v3x_atomic_store_int(&g_job_busy, 0);   /* release the slot */
}

static void cmd_set(const char *args, void *user)   /* console input thread */
{
    (void)user;
    if (!v3x_atomic_cas_int(&g_job_busy, 0, 1)) { api->print("busy, try again"); return; }
    V3xStr s;
    v3x_str_init(&s, g_job_arg, sizeof g_job_arg);
    if (v3x_str_append(&s, args) != V3X_R_OK) {   /* refuse instead of acting on a cut argument */
        api->print("argument too long (max %zu bytes)", sizeof g_job_arg - 1);
        v3x_atomic_store_int(&g_job_busy, 0);
        return;
    }
    if (api->run_on_main(job_on_main, NULL) != V3X_OK) {
        api->print("queue full");
        v3x_atomic_store_int(&g_job_busy, 0);
    }
}
```

## Hooks

**Use `api->hook_safe(target, pre, post, user)`** (API v4). The core generates a pass-through thunk:
it saves all 16 integer argument slots and xmm0-3, calls `pre`, forwards everything to the
original (unless `pre` set `skip_original`), keeps rax/xmm0, calls `post`, and returns. You never
write a detour with a guessed prototype, so you cannot forward too few arguments (that crashed the
game: [safety.md](safety.md#argument-forwarding)). The thunk is register-safe: the caller gets
every volatile register back exactly as the original left it (the game is built with
whole-program optimisation and its callers keep values in volatile registers across calls), and
it has unwind info, so exceptions and stack walks pass through. Several plugins can `hook_safe`
the same function.

<!-- sample: file | static const V3X_API *api; static void *g_target; -->
```c
static volatile LONG64 g_calls;

static void count_pre(V3X_HookCall *call)
{
    (void)call;
    InterlockedIncrement64(&g_calls);   /* worker thread: atomic, no lock, no log */
}

static int install(void)
{
    if (!V3X_HAS(api, hook_safe)) return 1;               /* loader too old: fail closed */
    int rc = api->hook_safe(g_target, count_pre, NULL, NULL);
    if (rc != V3X_OK) {                                    /* V3X_EHOOKBUSY, V3X_EPERM, MH_* */
        V3X_LOGE(api, TAG, "hook_safe FAILED (%d): feature off", rc);
        return 1;
    }
    return 0;
}
```

* **Only hook an address an anchor proved** (`sdk/v3x_anchor.h`: a string every xref agrees on,
  prologue bytes, a `check` callback), and install nothing that depends on a failed anchor
  ([safety.md](safety.md#fail-closed-anchors)).
* **Check the result.** `V3X_EHOOKBUSY` (100): another plugin holds a raw hook there (v3x.log names
  it). `V3X_EPERM` (v6): your manifest declares capabilities without `hook`, or the call came from
  code outside any module. 9: thunk pool full or 8 subscribers already.
* **pre to post state** goes in `call->scratch[0..1]` (zeroed per call), never in a global (other
  threads run the same function at the same time).
* **Never free a trampoline, never unload yourself.** `unhook` only disables the hook; the
  trampoline stays valid because another thread may be about to call it
  ([ADR 0003](adr/0003-hook-lifetime.md)). Keep every callback's code and `user` data alive
  forever: never `FreeLibrary` your own DLL.
* **Declare capabilities** (API v6): `V3X_PLUGIN_MANIFEST_CAPS(..., "hook")` makes the loader refuse
  `patch` and `exec_command` for your plugin, so a bug cannot use what you never needed.
* Callbacks run inside engine calls: a fault in them is the game's crash
  ([Fault isolation](#fault-isolation-and-what-it-cannot-catch)). Keep them tiny.

> **Unsafe — when you really need it:** a raw detour with `api->hook(target, detour, &original)`.
> * When: you *replace* a function whose exact prototype you have proven, or the thunk's ~40 extra
>   instructions matter in a function called millions of times per second.
> * What can go wrong: a detour with fewer arguments than the real function corrupts stack
>   arguments (the `sim_opt` crash); float arguments are lost if your detour does floating-point
>   work before calling the original; a raw hook is exclusive (`V3X_EHOOKBUSY` for everyone else),
>   so it blocks other plugins; a fault in the detour crashes the game.
> * How to contain it: count arguments with `tools/re_simopt_argcheck.py`; when unsure forward 8
>   slots with `V3X_PASS8_ARGS` / `V3X_PASS8` (`sdk/v3x_hook.h`); call `original` with exactly the
>   real prototype; check the return value and fail closed; declare `"hook"` in the manifest.

> **Unsafe — when you really need it:** `api->patch(addr, bytes, n)` (overwrite code or read-only
> data).
> * When: a one-byte branch flip or a constant that has no function boundary to hook.
> * What can go wrong: a thread executing those bytes at that moment resumes in the middle of the
>   new ones; a wrong address silently corrupts code that runs minutes later; the next game patch
>   moves everything.
> * How to contain it: find the site with a unique pattern (`V3xAnchor` without a string), verify
>   the exact old bytes with `v3x_pattern_match_at` before writing, patch in `v3x_plugin_load`
>   (before the game's code runs) or between ticks on the main thread, keep `n` minimal (max
>   `V3X_PATCH_MAX` per call), declare `"patch"` in the manifest. Rollback restores the bytes if
>   your load fails.

> **Unsafe — when you really need it:** your own MinHook or a hand-written trampoline.
> * When: never in a plugin. The loader owns the hook registry; a hook installed behind its back
>   cannot be shared, is not rolled back, and `unhook` can only disable it.
> * What can go wrong: two hook engines patching the same prologue, a freed trampoline that
>   another thread is still about to call (use-after-free of code), no register-safe thunk.
> * How to contain it: use `hook_safe`. If a function cannot be hooked that way, ask in an issue.

## File I/O

* **Your own files** live in `api->v3x_dir()`: build their paths with `v3x_plugin_file` /
  `v3x_path` (`sdk/v3x_util.h`, NULL when the path does not fit). On API v6 the wide
  `api->v3x_dir_w()` is correct for any folder name (the ANSI one is lossy outside the code page).
* **Names from data** (a mod file, a cache, an ini value) are joined with
  `v3x_path_join_checked(out, cap, base, rel, flags)`: it rejects `..`, `.`, `:`, absolute and UNC
  paths, leading/trailing/double separators, `<>"|?*` and control characters, trailing dots and
  spaces, device names (`CON`, `NUL`, `COM1`, ...), components over 255 bytes and paths over 512;
  `V3X_PATH_SINGLE` allows only a file name; `V3X_PATH_CHECK_REPARSE` refuses symlinks and
  junctions on the way (use it for every file you write or delete).
* **Write atomically**: `v3x_file_write_atomic(path, data, n)` writes a temp file and renames it,
  so readers never see half a file.
* **`v3x.ini` is shared** with mod_menu, doctor and the installer: hold `v3x_config_lock` around
  read-modify-write.
* Write rarely and small: the game's own saves fail when the disk is full.

<!-- sample: file -->
```c
/* name_from_file comes from a mod: untrusted */
static int write_entry(const V3X_API *api, const char *name_from_file, const char *text, size_t n)
{
    char dir[MAX_PATH], path[MAX_PATH];
    if (!v3x_path(api, dir, sizeof dir, "my_plugin_cache")) return -1;
    V3xResult r = v3x_path_join_checked(path, sizeof path, dir, name_from_file,
                                        V3X_PATH_SINGLE | V3X_PATH_CHECK_REPARSE);
    if (r != V3X_R_OK) {
        char clean[96];
        v3x_sanitize_line(clean, sizeof clean, name_from_file);
        V3X_LOGW(api, "my_plugin", "refused file name \"%s\" (%d)", clean, (int)r);
        return -1;
    }
    CreateDirectoryA(dir, NULL);   /* ok if it exists */
    return v3x_file_write_atomic(path, text, n);
}

/* edit v3x.ini: the lock spans read, edit and write */
static int set_ini_line(const V3X_API *api, const char *new_text, size_t n)
{
    char ini[MAX_PATH];
    if (!v3x_plugin_file(api, "v3x", ".ini", ini, sizeof ini)) return -1;
    HANDLE lk = v3x_config_lock(5000);
    if (!lk) { V3X_LOGW(api, "my_plugin", "v3x.ini is locked by another tool, not written"); return -1; }
    /* ... read ini, build the new content in a V3xStr ... */
    int rc = v3x_file_write_atomic(ini, new_text, n);
    v3x_config_unlock(lk);
    return rc;
}
```

## Check every result

Every API call and SDK helper that can fail says so in its return value; a plugin that ignores one
runs a feature on a half-installed state.

* SDK functions that must not be ignored are marked `V3X_MUST_USE` (checked arithmetic, path
  checks, allocation, span reads): ignoring one is a warning, an error with `-Werror`. Cast to
  `(void)` only where a later check covers it (span reads followed by `v3x_span_failed`).
* Loader calls: `hook_safe`, `hook`, `patch`, `register_command`, `on_event`, `on_frame`,
  `run_on_main` return 0 on success; anything else means "not installed". Log it and fail closed
  (that feature off), never assume.
* Return non-zero from `v3x_plugin_load` when a required part failed: the loader rolls the plugin
  back (removes its hooks, restores its patches, skips its callbacks).
* Optional parts (a console command, a reload handler) log a warning and keep the plugin running.
* Newer API fields only after `V3X_HAS(api, field)`.

## Fault isolation and what it cannot catch

Since loader 0.5.0 every callback the loader calls itself runs inside an SEH guard:
`v3x_plugin_load`, `on_frame`, `run_on_main`, console commands, event subscribers and
`enum_commands`. A fault there (access violation, illegal instruction, divide by zero, in-page
error, an escaping C++ exception) is caught, logged with `module+offset`, and the plugin's
callbacks are switched off; the game keeps running. On API v6 the core emits `plugin_disabled`.

It does **not** catch:

* faults in `hook_safe` pre/post and raw detours (they run inside engine calls),
* faults in engine code you called or patched,
* faults in threads you started and in `DllMain`,
* corruption: a wrong write that does not fault (a bad patch, a buffer overrun into your own
  globals, a wrong value written into game state) is invisible until something else breaks.

And a caught fault may have left your own locks held or data half-updated, which is why the whole
plugin is switched off. Fault isolation is for stability, not a security boundary: it is the
reason to follow the rules above, not a replacement for them.

## Testing

| tool | what it catches | how |
|---|---|---|
| offline anchors (`sdk/v3x_testkit.h`) | anchors that no longer match after a game patch, wrong install logic | `python sdk/build_plugin.py <dir> --test ../binaries/victoria3.exe` maps the exe read-only and runs your real resolve and load code against a fake API |
| `-Werror` | format mismatches, ignored `V3X_MUST_USE` results, shadowing, missing initialisers | `build_plugin.py --werror` (or `V3X_WERROR=1`) |
| UBSan | overflow of signed ints, out-of-bounds array index, misaligned or NULL access | `build_plugin.py --test --sanitize`; `SANITIZE=1 bash test/run_all.sh` (trap = crash = failed test) |
| fuzzing (`test/fuzz/`) | parser bugs on malformed input: reads past the end, hangs, crashes | a `test/fuzz/fuzz_<name>.c` harness, `ONLY=fuzz bash test/run_all.sh` |
| `tools/check_unsafe.sh` | banned idioms (`strcpy`, `sprintf`, `n += snprintf`, ...) | `bash tools/check_unsafe.sh plugins/<name>` |
| `test/static_analysis.sh` | clang-tidy / cppcheck findings (report only) | `bash test/static_analysis.sh` |
| `tools/check_doc_samples.py` | doc samples that do not compile | `python tools/check_doc_samples.py` |

A fuzz harness: one file, the parser under test included, invariants asserted. The driver feeds
seeds and mutations; the input ends at a no-access guard page, so a read past `len` crashes at
once. A crash writes the input to `$V3X_TMP/t/fuzz/crash-<target>-<iteration>.bin`; reproduce
with `FUZZ_REPRO=<file> $V3X_TMP/t/fuzz/fuzz_<name>.exe <target>`.

<!-- sample: complete -->
```c
/* test/fuzz/fuzz_my_cache.c */
#include "v3x_fuzz.h"
#include "v3x_safe.h"

static int parse_header(const uint8_t *d, size_t n, uint32_t *count)
{
    V3xSpan r;
    uint32_t magic = 0;
    v3x_span_init(&r, d, n);
    (void)v3x_span_u32le(&r, &magic);
    (void)v3x_span_u32le(&r, count);
    return !v3x_span_failed(&r) && magic == 0x3143594du;   /* "MYC1" */
}

static void fuzz_my_cache(const uint8_t *data, size_t len)
{
    uint32_t count = 0;
    if (parse_header(data, len, &count)) V3X_FUZZ_ASSERT(len >= 8);   /* invariant */
}
V3X_FUZZ_TARGET(my_cache, fuzz_my_cache)
V3X_FUZZ_SEED(my_cache, "MYC1\x02\x00\x00\x00")
```

Rules for harnesses: deterministic (no time, no threads, no state carried between inputs), no
files outside a temp dir, no network. Use `v3x_fuzz_cstr(data, len)` when the parser wants a
NUL-terminated string.

## Safe vs unsafe

| unsafe idiom | safe replacement |
|---|---|
| `strcpy`, `strcat`, `sprintf`, `n += snprintf(buf + n, ...)` | `V3xStr`: `v3x_str_append`, `v3x_str_appendf`; one copy: `snprintf(dst, sizeof dst, "%s", s)` |
| `api->log(TAG, text)` with foreign text | `api->log(TAG, "%s", clean)` after `v3x_sanitize_line` |
| `*(uint64_t *)(p + 0x120)` on a pointer you computed | `v3x_mem_read_u64(p + 0x120, &v)`, `v3x_mem_chain` |
| `memcpy(dst, engine_ptr, n)` | `v3x_mem_read(engine_ptr, dst, n)` |
| `strlen` / `strcpy` on an engine string | `v3x_mem_read_cstr(addr, buf, sizeof buf)` |
| `*(uint32_t *)(file + off)` | `V3xSpan`: `v3x_span_u32le`, `v3x_span_take`, `v3x_span_sub` |
| `malloc(n * size)`, `off + len <= size` | `v3x_alloc_array(n, size)`, `v3x_mul_ok`, `len <= size && off <= size - len` |
| `if (!g_flag) g_flag = 1;` across threads | `v3x_atomic_cas_int(&g_flag, 0, 1)` |
| touching game state from a command or your thread | `run_on_main` + `V3X_ASSERT_MAIN(api)` |
| `api->hook` with a hand-written detour | `api->hook_safe(target, pre, post, user)` |
| `api->pattern_scan(...)` result used directly, `find_xref(s, 0)` | `V3xAnchor` + `v3x_anchor_resolve_all` (unique match, all xrefs agree, prologue check, `check` callback) |
| a hardcoded RVA (`image_base() + 0x1465690`) | an anchor; RVAs in docs are for orientation only |
| `api->patch` without checking the old bytes | anchor on the old bytes (`v3x_pattern_match_at`), patch at load time, `"patch"` capability |
| `MH_CreateHook` / own trampolines, `FreeLibrary(self)` | `hook_safe`; never unload |
| `snprintf(path, "%s%s", dir, name_from_file)` | `v3x_path_join_checked(out, cap, dir, name, V3X_PATH_SINGLE \| V3X_PATH_CHECK_REPARSE)` |
| `fopen(path, "wb")` + write in place | `v3x_file_write_atomic(path, data, n)` |
| rewriting `v3x.ini` without a lock | `v3x_config_lock(5000)` ... `v3x_config_unlock` |
| ignoring a return value | check it; `V3X_MUST_USE` + `-Werror` |
| work in `DllMain` | `v3x_plugin_load`, `process_exit` event |

## The unsafe escape hatches

Some bundled plugins need the unsafe tools: `alloc_tracker` holds raw hooks on mimalloc's hot
allocation functions, `log_filter` replaces spdlog's `sink_it_`, `sim_opt` patches a vtable slot and
`tick_par` redirects a call site ([plugins.md](plugins.md#hooks-that-cannot-be-shared)). When you need one:

1. Write down why the safe way does not work (in the code, next to it).
2. Contain it with the steps in its box above: prove the address and shape, check the result,
   fail closed, keep it in one small function.
3. Mark the line `/* v3x-unsafe-ok: <reason> */` if `tools/check_unsafe.sh` flags it, so reviews can
   find every exception with one grep (the allow file `tools/check_unsafe.allow` is for whole
   generated or third-party files and needs a reason comment).
4. Add an offline test for it (the anchor, the bytes you expect before patching) and run the
   long in-game session from [safety.md](safety.md#test-offline-first-then-smoke-test-with-logs-backed-up).
