/* FROZEN copy of include/v3x.h as of API v4 (git 317e574, verbatim below this comment). Do not edit: test/compat_test.c builds
 * test/compat_plugin.c against it to prove plugins built for API v4 still load and work in the
 * current loader. */
/*
 * V3X - native code loader for Victoria 3.
 *
 * A plugin is a normal Windows x64 DLL placed in <game>/binaries/v3x/plugins/.
 * It must export:
 *
 *     V3X_EXPORT int v3x_plugin_load(const V3X_API *api);   // return 0 on success
 *
 * and may export:
 *
 *     V3X_EXPORT const char *v3x_plugin_name = "my_plugin";
 *     V3X_PLUGIN_MANIFEST("my_plugin", "1.0.0", "me", "what it does", "release/1.15.1");  // API v4
 *
 * Full reference with thread rules: docs/api-reference.md. Safety rules: docs/safety.md.
 *
 * v3x_plugin_load runs on the game's main thread, right before the game's own
 * entry point (CRT startup, static constructors and WinMain have NOT run yet).
 * Code is mapped and can be scanned and hooked; game globals are not set up yet.
 * Use api->on_frame for work that needs a running game.
 *
 * Keep the API C-only so plugins can be built with any compiler (MSVC, clang, zig, gcc).
 */
#ifndef V3X_H
#define V3X_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define V3X_API_VERSION 4

#ifdef __cplusplus
#define V3X_EXPORT extern "C" __declspec(dllexport)
#else
#define V3X_EXPORT __declspec(dllexport)
#endif

typedef void (*v3x_frame_fn)(void *user);
typedef void (*v3x_command_fn)(const char *args, void *user);

/* ---- API v4: hook_safe ----------------------------------------------------------------------
 * One call of a function hooked with api->hook_safe. The core's thunk fills this on the hooked
 * thread's stack, calls pre(call), calls the original with the (possibly modified) arguments
 * unless pre set skip_original, then calls post(call) and returns `ret` / `xmm_ret` to the caller.
 *
 * Win64 ABI: argument i (0-based) is in args[i] if it is an integer/pointer (rcx, rdx, r8, r9,
 * then the caller's stack slots), in xmm[i] (i < 4) if it is a float/double. A struct > 8 bytes
 * is passed as a pointer; a returned struct > 8 bytes uses a hidden pointer in args[0] (and
 * every other argument shifts by one). args[4..15] are always copied from the caller's stack, even
 * if the function takes fewer arguments (the extra slots are harmless garbage). Functions with more
 * than 16 arguments are NOT forwarded correctly (do not hook_safe them). __vectorcall / HVA
 * arguments are not supported. */
#define V3X_HOOK_MAX_ARGS 16
#define V3X_HOOK_MAX_SUBSCRIBERS 8   /* hook_safe subscribers per target (all plugins together) */
#define V3X_EHOOKBUSY 100            /* hook / hook_safe / unhook: the target is held by another plugin
                                        (see v3x.log for the owner); outside the MH_STATUS range */
typedef union V3X_Xmm {
    double d;          /* a double argument / return value */
    float f;           /* a float argument / return value */
    uint64_t u64;
    uint32_t u32[4];
    uint8_t b[16];     /* the full 128-bit register */
} V3X_Xmm;

typedef struct V3X_HookCall {
    uint64_t args[V3X_HOOK_MAX_ARGS];  /* +0x00 integer args: rcx, rdx, r8, r9, stack args 5..16; pre may change them */
    V3X_Xmm xmm[4];                    /* +0x80 xmm0..xmm3 at entry (float args 1..4); pre may change them */
    uint64_t ret;                      /* +0xc0 rax: the original's integer/pointer return value (0 before it runs) */
    int32_t skip_original;             /* +0xc8 pre sets 1: the original is not called; ret/xmm_ret are returned */
    int32_t reserved;                  /* +0xcc */
    V3X_Xmm xmm_ret;                   /* +0xd0 xmm0 after the original (float/double return value) */
    void *user;                        /* +0xe0 the `user` given to hook_safe */
    void *target;                      /* +0xe8 the hooked function */
    void *original;                    /* +0xf0 trampoline to the original (read-only; call it only with
                                          exactly the real prototype) */
    void *return_address;              /* +0xf8 the caller's return address (who called the function) */
    uint64_t scratch[2];               /* +0x100 zeroed per call; free for pre -> post state (e.g. a start time) */
} V3X_HookCall;

/* pre/post callback of hook_safe. Runs on the thread that called the hooked function. */
typedef void (*v3x_hook_fn)(V3X_HookCall *call);

/* Optional plugin manifest (API v4), exported as data named `v3x_plugin_manifest`. The loader
 * reads it before calling v3x_plugin_load and shows it in the `plugins` console command.
 * Use V3X_PLUGIN_MANIFEST(...) below instead of filling it by hand. All strings may be NULL. */
#define V3X_MANIFEST_REQUIRE_BUILD 1u   /* flags: refuse to load (v3x_plugin_load is not called)
                                           when the running game matches none of game_builds */
typedef struct V3X_PluginManifest {
    uint32_t struct_size;      /* sizeof(V3X_PluginManifest) the plugin was built with */
    uint32_t flags;            /* V3X_MANIFEST_* */
    uint32_t min_api;          /* lowest loader api_version this plugin needs (0 = any); an older
                                  loader refuses to load the plugin */
    uint32_t reserved;
    const char *name;          /* e.g. "my_plugin" (wins over v3x_plugin_name) */
    const char *version;       /* e.g. "1.2.0" */
    const char *author;
    const char *description;   /* one line */
    const char *game_builds;   /* ';'-separated list of supported games, each either a
                                  game_version() string ("release/1.15.1"), a game_build_id()
                                  ("6af3094f-60fb000") or a prefix ending in '*' ("release/1.15*").
                                  NULL, "" or "*" = any build. A mismatch only logs a WARNING
                                  unless flags has V3X_MANIFEST_REQUIRE_BUILD. */
} V3X_PluginManifest;

#ifdef __cplusplus
#define V3X_MANIFEST_EXPORT_ extern "C" __declspec(dllexport)
#else
#define V3X_MANIFEST_EXPORT_ __declspec(dllexport)
#endif
/* V3X_PLUGIN_MANIFEST("my_plugin", "1.0.0", "me", "does things", "release/1.15.1") at file scope.
 * V3X_PLUGIN_MANIFEST_EX adds flags and min_api. */
#define V3X_PLUGIN_MANIFEST_EX(name_, version_, author_, desc_, builds_, flags_, min_api_)              \
    V3X_MANIFEST_EXPORT_ const V3X_PluginManifest v3x_plugin_manifest = {                               \
        (uint32_t)sizeof(V3X_PluginManifest), (flags_), (min_api_), 0u, (name_), (version_), (author_), \
        (desc_), (builds_)}
#define V3X_PLUGIN_MANIFEST(name_, version_, author_, desc_, builds_) \
    V3X_PLUGIN_MANIFEST_EX(name_, version_, author_, desc_, builds_, 0u, 0u)

typedef struct V3X_API {
    uint32_t api_version;   /* V3X_API_VERSION the loader was built with */
    uint32_t struct_size;   /* sizeof(V3X_API) of the loader; check before using newer fields */

    /* Logging to <game>/binaries/v3x/v3x.log. `tag` is usually your plugin name. */
    void (*log)(const char *tag, const char *fmt, ...);

    /* Main module (victoria3.exe) */
    uintptr_t (*image_base)(void);
    size_t (*image_size)(void);
    const char *(*game_version)(void);   /* contents of caligula_branch.txt, e.g. "release/1.15.1" */

    /* Finding engine code without hardcoded addresses.
     * pattern_scan: IDA-style signature, e.g. "48 8B 05 ?? ?? ?? ?? 48 85 C0", searched in .text.
     * find_string:  address of the exact NUL-terminated ASCII string in .rdata/.data.
     * find_xref:    the index-th instruction in .text that references `target` with a
     *               RIP-relative lea/mov (index 0 = first).
     * function_start: start of the function containing `addr` (from the .pdata unwind table).
     * All return NULL when nothing is found. */
    void *(*pattern_scan)(const char *ida_pattern);
    void *(*find_string)(const char *s);
    void *(*find_xref)(const void *target, int index);
    void *(*function_start)(const void *addr);

    /* Inline hooking (MinHook). `original` receives a callable trampoline.
     * Return 0 on success, otherwise a MinHook MH_STATUS code.
     * A raw hook is EXCLUSIVE: on a target that already has a raw hook or hook_safe subscribers
     * (from any plugin) hook returns V3X_EHOOKBUSY and v3x.log names the owner. Prefer hook_safe
     * for engine functions other plugins may want too: it can be shared.
     * unhook removes only what the CALLING module (plugin DLL, found from the return address)
     * holds on `target`: its raw hook, or all of its hook_safe subscriptions there (other plugins'
     * subscriptions stay; the thunk is disabled and the original bytes restored when the last
     * subscriber leaves). Returns V3X_EHOOKBUSY if only other plugins hold it,
     * MH_ERROR_NOT_CREATED (4) if nothing is hooked. */
    int (*hook)(void *target, void *detour, void **original);
    int (*unhook)(void *target);

    /* Write bytes over code or read-only data (handles page protection + icache). 0 on success. */
    int (*patch)(void *addr, const void *bytes, size_t n);

    /* Called once per message-pump iteration on the game's main thread (about once per frame). */
    int (*on_frame)(v3x_frame_fn fn, void *user);

    /* Absolute paths with trailing backslash. */
    const char *(*game_dir)(void);     /* .../Victoria 3/binaries/ */
    const char *(*v3x_dir)(void);      /* .../Victoria 3/binaries/v3x/ */

    /* ---- API v2 (check api_version >= 2 or struct_size before using) ----
     * Interactive console. The V3X console window reads lines on a background input thread
     * and dispatches "name args..." to registered commands. Without a console window
     * (console=0 / V3X_NO_CONSOLE=1) registration still succeeds, commands just never run.
     *
     * Built-in commands: help, plugins, log <text>, version, quit (only prints a note: V3X never
     * exits the game). Ctrl+C in the console is swallowed so it cannot kill the game.
     *
     * register_command: `fn` runs on the console input thread (NOT the game's main thread) with
     *   the rest of the line after the name (leading spaces stripped, never NULL). Names are
     *   case-insensitive, up to 31 chars. Returns 0, -1 table full / bad args, -2 name taken.
     * print: printf to the console (plain, no tag) and v3x.log; a trailing newline is added if
     *   missing. Output over 8 KB is truncated. Returns chars written or -1. Thread-safe.
     * run_on_main: queue fn(user) to run once in the next on_frame tick on the main thread
     *   (use it from commands that touch game state). Returns 0, or -1 if the queue is full.
     * exec_command: run one console line synchronously on the calling thread, as if typed.
     *   Returns 0 if a command ran, -1 if the name is unknown/empty. */
    int (*register_command)(const char *name, const char *help, v3x_command_fn fn, void *user);
    int (*print)(const char *fmt, ...);
    int (*run_on_main)(v3x_frame_fn fn, void *user);
    int (*exec_command)(const char *line);

    /* ---- API v3 (check api_version >= 3 or struct_size before using) ----
     * exec_capture: like exec_command (synchronous, on the calling thread, same return value), and
     *   while the command runs, every api->print and api->log made ON THE CALLING THREAD is also
     *   passed to sink(text, user). Output still goes to the console and v3x.log as usual; output
     *   from other threads (e.g. threads the command starts) is not captured.
     *   text: for print, the printed text with its trailing '\n'; for log, the full log line
     *   ("[hh:mm:ss.mmm][tag] message\n"). One sink call per print/log call.
     *   The sink runs on the calling thread; print/log made from inside the sink are not fed back
     *   to it. Nested exec_capture calls use the inner sink and restore the outer one afterwards.
     *   sink may be NULL (then it is exactly exec_command). */
    int (*exec_capture)(const char *line, void (*sink)(const char *text, void *user), void *user);

    /* Named events between plugins (one plugin owns a hook and emits, others subscribe).
     * on_event: subscribe fn(name, data, user) to `name` (case-sensitive, 1..63 chars). The table is
     *   fixed at 64 subscriptions for all plugins together; the same (name, fn, user) may be
     *   subscribed only once. Returns 0, -1 table full / bad args, -2 already subscribed.
     * emit_event: calls every subscriber of `name` synchronously on the emitting thread, in
     *   subscription order, with `data` passed through untouched (meaning defined per event).
     *   Subscribers may subscribe or emit from inside the callback (a subscriber added during an
     *   emit is called from the next emit on). Returns the number of subscribers called, -1 on bad args.
     * Known events (emitter, thread, data):
     *   "load_transition"        fast_load, main thread, when a load-state transition ENDS;
     *                            data = const V3X_LoadTransition * (may be NULL).
     *   "load_transition_begin"  fast_load, main thread, when a transition BEGINS;
     *                            data = const V3X_LoadTransition * (seconds = 0; may be NULL).
     *   "save_begin"             enhance, the saving thread (main), right BEFORE the engine serializes
     *                            a save (SaveGameState: autosave, exit save, console, save dialog);
     *                            data = const V3X_SaveBegin *.
     *   "save_end"               enhance, same thread, right after that save call returned;
     *                            data = the same const V3X_SaveBegin *.
     *   "process_exit"           core, the thread calling ExitProcess, once, BEFORE the OS kills the other
     *                            threads (they all still run); data = const long * (exit code). The place
     *                            for final reports and for stopping anything that suspends threads. Keep
     *                            it short; never wait for another thread without a timeout.
     * Process exit rule: in DllMain(DLL_PROCESS_DETACH) with reserved != NULL every other thread is
     * already dead (maybe holding a lock): do nothing there. From then on log/print/hook/run_on_main/
     * emit_event/exec_command/register_command/on_event return at once without effect. */
    int (*on_event)(const char *name, void (*fn)(const char *name, void *data, void *user), void *user);
    int (*emit_event)(const char *name, void *data);

    /* ---- API v4 (check with V3X_HAS(api, field) before using) ----
     * hook_safe: hook `target` without writing a detour. The core generates a pass-through thunk
     *   that saves all 16 integer argument slots (rcx, rdx, r8, r9 + 12 stack slots) and xmm0-xmm3,
     *   calls pre(call), forwards everything to the original (unless call->skip_original), keeps
     *   rax and xmm0, calls post(call) and returns them. pre or post may be NULL (not both). The thunk
     *   has unwind info, so exceptions and stack walks pass through it. Callbacks run on whatever
     *   thread calls the hooked function (often an engine worker): keep them short, and never run
     *   game script from them unless you know you are inside a tick task (sdk/v3x_game.h
     *   v3x_script_random_allowed). Returns 0 or a MinHook MH_STATUS like hook (9 =
     *   MH_ERROR_MEMORY_ALLOC also when the thunk pool is full or the target already has
     *   V3X_HOOK_MAX_SUBSCRIBERS subscribers, 3 = MH_ERROR_ALREADY_CREATED when this plugin already
     *   subscribed the same pre/post/user there, V3X_EHOOKBUSY when the target has a raw hook,
     *   -1 bad args). Remove with unhook; the thunk itself is never freed (a thread may still be
     *   inside it). Any thread.
     *   Several plugins (or one plugin several times, with different pre/post/user) may hook_safe
     *   the SAME target: they share one thunk and run nested, like wrappers. pre callbacks run in
     *   subscription order, then the original, then post callbacks in REVERSE order. Each one gets
     *   its own call->user and its own call->scratch; args / ret changes are seen by the next one.
     *   A pre that sets skip_original ends the chain for that call: the later subscribers' pre and
     *   post do not run, the original is not called, the post callbacks of the earlier subscribers
     *   and of the skipping one still run (so the first subscriber to skip wins). Subscribing and
     *   unsubscribing are safe while other threads call the target; a call that already started
     *   finishes with the subscriber list it started with.
     * main_thread_id: Win32 thread id of the game's main thread (the one on_frame/run_on_main use).
     * game_build_id: identity of the running victoria3.exe from its PE header,
     *   "<TimeDateStamp hex>-<SizeOfImage hex>", e.g. "6af3094f-60fb000". It changes with every
     *   game patch, even when caligula_branch.txt (game_version) does not.
     * enum_commands: calls fn(name, help, user) for every registered console command in
     *   registration order (outside the command lock; fn may register commands). Returns the
     *   number of commands, -1 on bad args. Any thread. */
    int (*hook_safe)(void *target, v3x_hook_fn pre, v3x_hook_fn post, void *user);
    uint32_t (*main_thread_id)(void);
    const char *(*game_build_id)(void);
    int (*enum_commands)(void (*fn)(const char *name, const char *help, void *user), void *user);
} V3X_API;

/* True if the loader's API table has `field` and it is set (the loader was built with a header
 * that has it). Example: if (V3X_HAS(api, hook_safe)) api->hook_safe(...); */
#define V3X_HAS(api, field) \
    ((api) && (api)->struct_size >= offsetof(V3X_API, field) + sizeof((api)->field) && (api)->field)

/* data of the "load_transition" event */
typedef struct V3X_LoadTransition {
    const char *from, *to;   /* state names, may be NULL */
    double seconds;          /* time spent in `from` */
} V3X_LoadTransition;

/* data of the "save_begin" / "save_end" events */
typedef struct V3X_SaveBegin {
    const char *name;        /* save name as the engine passes it (NUL-terminated copy, valid only during the
                                callback; exact form, e.g. with or without ".v3", not verified in game); may be NULL */
} V3X_SaveBegin;

typedef int (*v3x_plugin_load_fn)(const V3X_API *api);

/* version.dll also exports `const V3X_API *v3x_api(void)` (for tests and tools that are not
 * plugins, e.g. GetProcAddress(GetModuleHandleA("version.dll"), "v3x_api")). */
typedef const V3X_API *(*v3x_api_fn)(void);

#ifdef __cplusplus
}
#endif
#endif
