/* script_ext: script SDK for plugins (docs/scripting.md).
 *
 * Exports `const V3X_ScriptExt *v3x_script_ext(void)` (sdk/v3x_script.h). Three layers:
 *   (b) begin_pack / write_file / end_pack: write script, localization and GUI files into
 *       binaries/v3x/content_packs/<pack>/ from v3x_plugin_load; content_packs mounts them when the VFS
 *       initializes. No engine code is touched.
 *   (A) run_effect_text / eval_trigger_text: run script text once at runtime, root = the player's country,
 *       by calling the engine's own Script Runner functions (sx_engine.h). Main thread only. Console:
 *       `sxrun <effect text>`, `sxtrigger <trigger text>`.
 *   (C) register_effect / register_trigger: new `v3x_*` script keywords whose behaviour is a C callback
 *       (sx_reg.h). Registered from a post-hook of one vanilla keyword registrar, in the engine's static
 *       initialization, like a vanilla keyword.
 * (A) and (C) resolve every engine address from anchors and fail closed (feature off + a log line).
 * Console: `scriptext` lists the packs, the feature status and the registered keywords.
 * Containment (sx_guard.h): packs and keywords have an owner module; keyword callbacks run under a fault
 * guard; a plugin the core disables (V3X_EVENT_PLUGIN_DISABLED) loses its keywords and its packs.
 * Engine RE: docs/re/script.md. NOT verified in a running game (docs/scripting.md). */
#include <stdarg.h>
#include "v3x.h"
#include "sx_pack.h"
#include "sx_engine.h"
#include "sx_reg.h"

V3X_EXPORT const char *v3x_plugin_name = "script_ext";
V3X_PLUGIN_MANIFEST("script_ext", "0.2.0", "V3X", "ship script/loc/gui files, run script text, add native script keywords", "*");

#define TAG "script_ext"
static const V3X_API *api;
static HMODULE g_self;

static SxEngine g_eng;            /* layer A: Script Runner functions */
static SxRegEng g_reg;            /* layer C: keyword registration */
static int g_loaded;              /* v3x_plugin_load ran */
static int g_reg_tried, g_hooked; /* registration anchors resolved / the registrar hook installed */
static int g_registered_n;        /* keywords registered by the engine static init */
static void (*g_registrar_orig)(void);

static V3X_PRINTF(1, 2) void log_fwd(const char *fmt, ...)
{
    if (!api) return;
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    api->log(TAG, "%s", b);
}

/* ------------------------------------------------------------------ containment: disabled plugins */

/* V3X_EVENT_PLUGIN_DISABLED (core, the disabling thread): never call that module's keywords again, refuse
 * its pack calls. Anyone can emit an event, so the module must be a loaded module other than ours. */
static void on_plugin_disabled(const char *name, void *data, void *user)
{
    (void)name; (void)user;
    const V3X_PluginDisabled *d = (const V3X_PluginDisabled *)data;
    if (!d || d->struct_size < offsetof(V3X_PluginDisabled, module) + sizeof d->module) return;
    HMODULE h = (HMODULE)d->module;
    if (!h || h == g_self || sx_module_of((const void *)h) != h) return;
    int added = sx_module_disable(h);
    int kw = sx_kw_disable_module(h);
    if (added || kw) {
        char nm[128];
        sx_module_name(h, nm, sizeof nm);
        api->log(TAG, "plugin %s disabled by the core (%s): %d native keyword(s) switched off, its packs are read-only",
                 nm[0] ? nm : "?", d->reason ? d->reason : "no reason given", kw);
    }
}

/* ------------------------------------------------------------------ layer C: registration */

/* Post-hook of the debug_log effect registrar: runs in the engine's static initialization (main thread). */
static void hk_registrar(void)
{
    g_registrar_orig();
    if (InterlockedExchange(&sx_reg_closed, 1)) return;
    g_registered_n = sx_register_all(&g_reg, log_fwd);
    log_fwd("static init: %d script keyword(s) registered (%d effect(s) + %d trigger(s) queued)", g_registered_n, sx_neff, sx_ntrg);
}

/* Resolves the registration anchors and installs the hook, once. 0 = armed. Called at load when keywords are
 * already queued, else by the first register_* after load. */
static int arm_registration(void)
{
    if (g_hooked) return 0;
    if (sx_reg_off || g_reg_tried) return -1;
    g_reg_tried = 1;
    int st = 0;
    if (sx_reg_resolve(api, &g_reg) || sx_reg_build(&g_reg)) {
        snprintf(sx_reg_off_why, sizeof sx_reg_off_why, "%s", g_reg.why[0] ? g_reg.why : "registration anchors failed");
    } else if (!V3X_HAS(api, hook)) {
        snprintf(sx_reg_off_why, sizeof sx_reg_off_why, "loader has no hook");
    } else if ((st = api->hook(g_reg.registrar, (void *)hk_registrar, (void **)&g_registrar_orig)) != 0) {
        snprintf(sx_reg_off_why, sizeof sx_reg_off_why, "hook of the keyword registrar failed (%d)", st);
    } else {
        g_hooked = 1;
        api->log(TAG, "keyword registration armed: registrar exe+0x%llx hooked, register fns effect exe+0x%llx trigger exe+0x%llx",
                 (unsigned long long)((uintptr_t)g_reg.registrar - api->image_base()),
                 (unsigned long long)((uintptr_t)g_reg.eff_register - api->image_base()),
                 (unsigned long long)((uintptr_t)g_reg.trg_register - api->image_base()));
        return 0;
    }
    sx_reg_off = 1;
    api->log(TAG, "keyword registration OFF: %s (queued keywords dropped: %d effect, %d trigger)", sx_reg_off_why, sx_neff, sx_ntrg);
    sx_neff = sx_ntrg = 0;
    return -1;
}

static int on_main_or_unknown(void)
{
    return !api || v3x_on_main_thread(api) != 0;   /* before our own load there is no api: cannot tell, accept */
}

static int sx_register_effect(const char *name, v3x_script_effect_fn cb, void *user)
{
    if (!on_main_or_unknown()) return V3X_SCRIPT_ETHREAD;
    int rc = sx_kw_add(sx_eff, &sx_neff, name, (void *)cb, user, "effect");
    if (rc || !g_loaded) return rc;
    if (arm_registration()) return V3X_SCRIPT_EOFF;
    api->log(TAG, "queued effect %s", name);
    return 0;
}

static int sx_register_trigger(const char *name, v3x_script_trigger_fn cb, void *user)
{
    if (!on_main_or_unknown()) return V3X_SCRIPT_ETHREAD;
    int rc = sx_kw_add(sx_trg, &sx_ntrg, name, (void *)cb, user, "trigger");
    if (rc || !g_loaded) return rc;
    if (arm_registration()) return V3X_SCRIPT_EOFF;
    api->log(TAG, "queued trigger %s", name);
    return 0;
}

/* ------------------------------------------------------------------ layer A: run text */

static volatile LONG g_eng_state;   /* 0 not tried, 1 resolving, 2 done (g_eng.ok tells) */

/* Resolves the Script Runner anchors once (any thread, read-only memory scans), logging every anchor. */
static void ensure_engine(void)
{
    if (g_eng_state == 2 || !api) return;
    if (InterlockedCompareExchange(&g_eng_state, 1, 0) != 0) {
        for (int i = 0; i < 20000 && g_eng_state != 2; i++) Sleep(1);
        return;
    }
    if (api->image_base() && V3X_HAS(api, main_thread_id)) {
        if (sx_engine_resolve(api, TAG, &g_eng) == 0) {
            sx_veh_install();
            api->log(TAG, "script runner ready: parse effect exe+0x%llx trigger exe+0x%llx, exec exe+0x%llx, eval exe+0x%llx, random scope exe+0x%llx, player id [[exe+0x%llx]+0x608]+0x%x",
                     (unsigned long long)((uintptr_t)g_eng.eff_parse - api->image_base()), (unsigned long long)((uintptr_t)g_eng.trg_parse - api->image_base()),
                     (unsigned long long)((uintptr_t)g_eng.eff_exec - api->image_base()), (unsigned long long)((uintptr_t)g_eng.trg_eval - api->image_base()),
                     (unsigned long long)((uintptr_t)g_eng.rng_global - api->image_base()), (unsigned long long)((uintptr_t)g_eng.player_root - api->image_base()),
                     g_eng.player_off);
        } else {
            api->log(TAG, "script runner OFF: %s", g_eng.why);
        }
    } else {
        snprintf(g_eng.why, sizeof g_eng.why, "no game image / loader without main_thread_id");
    }
    g_eng_state = 2;
}


static int sx_run_effect_text(const char *text, char *err, size_t errlen)
{
    if (!api || !g_loaded) return sx_fail(err, errlen, V3X_SCRIPT_EOFF, "script_ext has not been loaded yet");
    ensure_engine();
    return sx_run_effect(api, &g_eng, text, err, errlen);
}

static int sx_eval_trigger_text(const char *text, int *result, char *err, size_t errlen)
{
    if (!api || !g_loaded) return sx_fail(err, errlen, V3X_SCRIPT_EOFF, "script_ext has not been loaded yet");
    ensure_engine();
    return sx_eval_trigger(api, &g_eng, text, result, err, errlen);
}

static uint32_t sx_features(void)
{
    uint32_t f = 0;
    ensure_engine();
    if (g_eng.ok && !sx_run_disabled) f |= V3X_SCRIPT_F_RUN;
    if (g_hooked && !sx_reg_closed) f |= V3X_SCRIPT_F_REGISTER;
    if (sx_reg_closed && g_registered_n > 0) f |= V3X_SCRIPT_F_REGISTERED;
    return f;
}

static V3X_ScriptExt g_table = {
    (uint32_t)sizeof(V3X_ScriptExt), V3X_SCRIPT_VERSION, sx_begin_pack, sx_write_file, sx_end_pack, sx_packs_dir,
    sx_features, sx_run_effect_text, sx_eval_trigger_text, sx_register_effect, sx_register_trigger,
};

V3X_EXPORT const V3X_ScriptExt *v3x_script_ext(void)
{
    if (!sx_root[0]) sx_root_from_module(g_self);
    return &g_table;
}

/* `sxrun` / `sxtrigger` run on the console input thread: hand the text to the main thread. */
typedef struct SxJob { int trigger; char text[1]; } SxJob;

static void job_main(void *u)
{
    SxJob *j = (SxJob *)u;
    char err[512];
    int rc;
    if (j->trigger) {
        int res = 0;
        rc = sx_eval_trigger_text(j->text, &res, err, sizeof err);
        if (rc == 0) api->print("sxtrigger: %s", res ? "true" : "false");
    } else {
        rc = sx_run_effect_text(j->text, err, sizeof err);
        if (rc == 0) api->print("sxrun: done (engine errors, if any, are in game.log)");
    }
    if (rc) api->print("%s: failed (%d): %s", j->trigger ? "sxtrigger" : "sxrun", rc, err);
    api->log(TAG, "%s '%s' -> %d%s%s", j->trigger ? "sxtrigger" : "sxrun", j->text, rc, rc ? " " : "", rc ? err : "");
    free(j);
}

static void queue_job(const char *args, int trigger)
{
    while (*args == ' ') args++;
    if (!*args) {
        api->print("usage: %s <%s text>   e.g. %s", trigger ? "sxtrigger" : "sxrun", trigger ? "trigger" : "effect",
                   trigger ? "sxtrigger is_ai = no" : "sxrun add_treasury = 1000");
        return;
    }
    size_t n = strlen(args);
    SxJob *j = (SxJob *)malloc(sizeof(SxJob) + n);
    if (!j) { api->print("out of memory"); return; }
    j->trigger = trigger;
    memcpy(j->text, args, n + 1);
    if (api->run_on_main(job_main, j)) { api->print("main-thread queue full, try again"); free(j); }
}

static void cmd_sxrun(const char *args, void *user) { (void)user; queue_job(args, 0); }
static void cmd_sxtrigger(const char *args, void *user) { (void)user; queue_job(args, 1); }

static void cmd_scriptext(const char *args, void *user)
{
    (void)args; (void)user;
    api->print("script_ext: content_packs dir %s", sx_root[0] ? sx_root : "(unknown)");
    int any = 0;
    for (int i = 0; i < SX_MAX_PACKS; i++) {
        if (!sx_packs[i].name[0]) continue;
        any = 1;
        api->print("  %s: %d file(s), owner %s%s", sx_packs[i].name, sx_packs[i].n, sx_packs[i].owner_name[0] ? sx_packs[i].owner_name : "?",
                   sx_packs[i].active ? " (end_pack not called)" : "");
    }
    if (!any) api->print("  no pack written this session");
    ensure_engine();
    if (g_eng.ok) api->print("  run script text (sxrun / sxtrigger): %s", sx_run_disabled ? "DISABLED after a fault" : "available");
    else api->print("  run script text (sxrun / sxtrigger): OFF: %s", g_eng.why);
    if (g_hooked)
        api->print("  native keywords: %s, %d registered, %d effect(s) + %d trigger(s) queued", sx_reg_closed ? "engine start done" : "armed",
                   g_registered_n, sx_neff, sx_ntrg);
    else
        api->print("  native keywords: %s", sx_reg_off ? sx_reg_off_why : "no plugin registered any");
    for (int pass = 0; pass < 2; pass++) {
        const SxKw *tab = pass ? sx_trg : sx_eff;
        int n = pass ? sx_ntrg : sx_neff;
        for (int i = 0; i < n; i++) {
            char own[128];
            sx_module_name(tab[i].owner, own, sizeof own);
            api->print("    %-7s %-32s token %u owner %s%s%s", pass ? "trigger" : "effect", tab[i].name, (unsigned)tab[i].token, own[0] ? own : "?",
                       tab[i].registered ? "" : " (not registered yet)", tab[i].off ? (tab[i].faults ? " OFF (faulted)" : " OFF (plugin disabled)") : "");
        }
    }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) g_self = h;   /* only remembers the handle */
    return TRUE;
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    sx_logf = log_fwd;
    sx_run_log = log_fwd;
    if (!sx_root[0]) sx_root_from_module(g_self);
    if (!sx_root[0]) {   /* DLL not in binaries/v3x/plugins: use the loader's folder */
        snprintf(sx_root, sizeof sx_root, "%scontent_packs\\", api->v3x_dir());
        CreateDirectoryA(sx_root, NULL);
    }
    if (V3X_HAS(api, register_command)) {
        api->register_command("scriptext", "script_ext: content packs, script runner and native keyword status", cmd_scriptext, NULL);
        api->register_command("sxrun", "sxrun <effect text>: run script effects once (root = player country), e.g. sxrun add_treasury = 1000", cmd_sxrun, NULL);
        api->register_command("sxtrigger", "sxtrigger <trigger text>: evaluate a script trigger once (root = player country)", cmd_sxtrigger, NULL);
    }
    sx_guard_install();
    if (!sx_guard_veh) api->log(TAG, "WARNING: AddVectoredExceptionHandler failed (error %lu): a faulting keyword callback is not contained",
                                (unsigned long)GetLastError());
    int erc = api->on_event(V3X_EVENT_PLUGIN_DISABLED, on_plugin_disabled, NULL);
    if (erc != 0 && erc != V3X_EEXIST)
        api->log(TAG, "WARNING: on_event(%s) failed (%d): keywords of a plugin the core disables are not switched off", V3X_EVENT_PLUGIN_DISABLED, erc);
    g_loaded = 1;   /* layer A resolves its anchors on first use (ensure_engine): ~0.5 s of scanning not paid by plugins that never run script */
    if (sx_neff + sx_ntrg > 0 && api->image_base()) arm_registration();
    api->log(TAG, "ready, content packs in %s", sx_root);
    return 0;
}
