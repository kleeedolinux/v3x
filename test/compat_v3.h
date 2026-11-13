/* FROZEN copy of include/v3x.h as of API v3 (reconstructed from 317e574 by removing the API v4 section; layout per CHANGELOG). Do not edit: test/compat_test.c builds
 * test/compat_plugin.c against it to prove plugins built for API v3 still load and work in the
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

#define V3X_API_VERSION 3

#ifdef __cplusplus
#define V3X_EXPORT extern "C" __declspec(dllexport)
#else
#define V3X_EXPORT __declspec(dllexport)
#endif

typedef void (*v3x_frame_fn)(void *user);
typedef void (*v3x_command_fn)(const char *args, void *user);

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

} V3X_API;

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
