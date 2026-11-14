/* Backward/forward compatibility of the plugin ABI (no game). Links version.dll like
 * victoria3.exe, so the loader boots and loads <dir>/v3x/plugins/compat_v1..v6.dll, each built from
 * test/compat_plugin.c against the FROZEN header of its API generation (test/compat_v1..v5.h) or the
 * current one (v6). Proves: plugins built for every older API still load, see their fields at the
 * same offsets and get working functions; V3X_HAS is false for fields beyond a table's struct_size;
 * a v5 plugin degrades (no crash, no v5 call) on a table without v5.
 * Modes:
 *   compat_test.exe        current loader (build/version.dll): api v6, all plugins "ok", v5 not degraded,
 *                          the v6 plugin used the v6 calls
 *   compat_test.exe old    a v4 loader built from git 317e574 (run_all.sh builds it): api v4, the v1-v4
 *                          plugins "ok", the v5 and v6 plugins loaded and DEGRADED (forward compatibility)
 * Setup (from v3x/; test/run_all.sh does all of it):
 *   D=$T/compat; mkdir -p $D/v3x/plugins && cp build/version.dll $D/
 *   for v in 1 2 3 4 5 6; do $CC -shared -DCOMPAT_V=$v -o $D/v3x/plugins/compat_v$v.dll test/compat_plugin.c; done
 *   $CC -o $D/compat_test.exe test/compat_test.c -Lbuild -l:version.dll -luser32
 *   (cd $D && V3X_NO_CONSOLE=1 ./compat_test.exe)
 * Exit code 0 = ALL OK. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "../include/v3x.h"

__declspec(dllimport) const V3X_API *v3x_api(void);

static int fails, checks;
#define CHECK(c, ...) do { int ok_ = !!(c); checks++; printf("%s: ", ok_ ? "OK  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); fails += !ok_; } while (0)

static void *sym(const char *dll, const char *name)
{
    HMODULE m = GetModuleHandleA(dll);
    return m ? (void *)GetProcAddress(m, name) : NULL;
}

static void pump(int n)
{
    MSG msg;
    for (int i = 0; i < n; i++) PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE);   /* the hooked frame tick */
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int old = argc > 1 && !strcmp(argv[1], "old");
    printf("marker: %s\n", "v3x_compat_host_marker_string");   /* found by the plugins via find_string/find_xref */
    const V3X_API *api = v3x_api();
    CHECK(api != NULL, "v3x_api() export");
    if (!api) return 1;
    unsigned want = old ? 4 : V3X_API_VERSION;
    CHECK(api->api_version == want, "loader api v%u (want %u)", api->api_version, want);
    pump(3);

    char dll[32];
    for (int v = 1; v <= 6; v++) {
        snprintf(dll, sizeof dll, "compat_v%d.dll", v);
        const char *res = (const char *)sym(dll, "compat_result");
        volatile long *frames = (volatile long *)sym(dll, "compat_frames");
        CHECK(res && !strcmp(res, "ok"), "plugin built for API v%d loaded and every call it makes works: %s", v, res ? res : "(not loaded)");
        CHECK(frames && *frames >= 3, "API v%d plugin: on_frame callback ran (%ld frames)", v, frames ? *frames : -1);
        if (v >= 2) {
            volatile long *mainq = (volatile long *)sym(dll, "compat_main");
            CHECK(mainq && *mainq == 1, "API v%d plugin: run_on_main ran once on the main thread", v);
            volatile long *cmds = (volatile long *)sym(dll, "compat_cmds");
            char line[32];
            snprintf(line, sizeof line, "compat_v%d x", v);
            CHECK(api->exec_command(line) == 0 && cmds && *cmds == 1, "API v%d plugin: its console command runs", v);
        }
    }
    int called = api->emit_event("compat_ping", NULL);
    CHECK(called == 4, "event from the host reaches the v3, v4, v5 and v6 subscribers (%d)", called);
    for (int v = 3; v <= 6; v++) {
        snprintf(dll, sizeof dll, "compat_v%d.dll", v);
        volatile long *ev = (volatile long *)sym(dll, "compat_events");
        CHECK(ev && *ev == 1, "API v%d plugin got the event", v);
    }

    /* V3X_HAS against tables of each older size (what a new plugin sees on an old loader) */
    V3X_API t = *api;
    t.struct_size = (uint32_t)offsetof(V3X_API, register_command);   /* v1 table */
    CHECK(!V3X_HAS(&t, register_command) && V3X_HAS(&t, v3x_dir), "V3X_HAS on a v1-sized table: v3x_dir yes, register_command no");
    t.struct_size = (uint32_t)offsetof(V3X_API, hook_safe);           /* v3 table */
    CHECK(!V3X_HAS(&t, hook_safe) && V3X_HAS(&t, emit_event), "V3X_HAS on a v3-sized table: emit_event yes, hook_safe no");
    t.struct_size = (uint32_t)offsetof(V3X_API, log_at);              /* v4 table */
    CHECK(!V3X_HAS(&t, log_at) && !V3X_HAS(&t, build_matches) && V3X_HAS(&t, enum_commands),
          "V3X_HAS on a v4-sized table: enum_commands yes, log_at/build_matches no");
    t = *api;
    t.struct_size = sizeof(V3X_API);
    t.log_at = NULL;
    CHECK(!V3X_HAS(&t, log_at), "V3X_HAS: a NULL field is absent even inside struct_size");

    int (*probe)(const V3X_API *) = (int (*)(const V3X_API *))sym("compat_v5.dll", "compat_probe");
    volatile long *deg = (volatile long *)sym("compat_v5.dll", "compat_degraded");
    CHECK(probe != NULL, "v5 plugin exports compat_probe");
    if (probe) {
        t = *api;
        t.struct_size = (uint32_t)offsetof(V3X_API, log_at);
        CHECK(probe(&t) == 0, "v5 plugin given a v4-sized table: degrades, makes no v5 call");
    }
    volatile long *v6ok = (volatile long *)sym("compat_v6.dll", "compat_v6_ok");
    if (old) {
        CHECK(api->struct_size == offsetof(V3X_API, log_at), "old loader table ends after enum_commands (%u)", api->struct_size);
        CHECK(deg && *deg == 1, "v5 plugin on the real v4 loader: loaded and degraded");
        CHECK(v6ok && *v6ok == 0, "v6 plugin on the real v4 loader: loaded, v6 calls skipped (V3X_HAS)");
    } else {
        CHECK(v6ok && *v6ok == 1, "v6 plugin on the v6 loader: off_frame/off_event/unregister_command/dir getters/unhook(NULL) work");
        t = *api;
        t.struct_size = (uint32_t)offsetof(V3X_API, off_frame);   /* v5 table */
        CHECK(!V3X_HAS(&t, off_frame) && V3X_HAS(&t, build_matches), "V3X_HAS on a v5-sized table: build_matches yes, off_frame no");
        CHECK(api->struct_size == sizeof(V3X_API), "current loader fills the whole v%d table (%u)", V3X_API_VERSION, api->struct_size);
        CHECK(deg && *deg == 0 && probe && probe(api) == 1, "v5 plugin on the v5 loader: uses v5");
        /* every function pointer of the table is set (api_version N is reported only with all vN fields) */
        const void *const *fp = (const void *const *)((const char *)api + 8);
        int nulls = 0, n = (int)((api->struct_size - 8) / sizeof(void *));
        for (int i = 0; i < n; i++) nulls += fp[i] == NULL;
        CHECK(nulls == 0, "all %d function pointers of the v%u table are non-NULL", n, api->api_version);
        uint32_t *la = (uint32_t *)sym("version.dll", "v3x_loader_api");
        const char *sv = (const char *)sym("version.dll", "v3x_loader_semver");
        CHECK(la && *la == V3X_API_VERSION && sv && !strcmp(sv, api->loader_version()),
              "version.dll data exports v3x_loader_api=%u v3x_loader_semver=%s", la ? *la : 0, sv ? sv : "?");
    }
    printf("%d checks, %d failed\n%s\n", checks, fails, fails ? "FAILED" : "ALL OK");
    return fails ? 1 : 0;
}
