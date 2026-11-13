/* Offline test for my_plugin: no game process, nothing executes in the exe.
 *     python sdk/build_plugin.py templates/plugin_template --test [path/to/victoria3.exe]
 * Without an exe it checks the callbacks; with one it maps the exe read-only (sdk/v3x_testkit.h)
 * and runs the plugin's real anchor code against it, then loads the plugin against the fake API
 * (hooks are recorded, not installed). Exit code 0 = ALL OK. */
#include "v3x_testkit.h"
#include "my_plugin.c"

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    /* 1. callbacks: pre stores a timestamp, post counts */
    V3X_API fake;
    v3x_testkit_api(&fake);
    api = &fake;
    QueryPerformanceFrequency(&g_freq);
    V3X_HookCall c; memset(&c, 0, sizeof c);
    tick_pre(&c);
    V3X_TEST_CHECK(c.scratch[0] != 0, "pre stores the start time in scratch[0]");
    tick_post(&c);
    V3X_TEST_CHECK(g_ticks == 1, "post counts one tick");

    /* 2. anchors against the real exe */
    if (argc > 1) {
        V3X_API real;
        if (v3x_testkit_map_exe(argv[1], &real) != 0) V3X_TEST_CHECK(0, "map %s", argv[1]);
        else {
            V3X_TEST_CHECK(my_plugin_resolve(&real) == 0, "anchors resolve on %s", argv[1]);
            printf("  game_tick at exe+0x%llx (1.15.1: 0x1465690)\n", v3x_testkit_rva(my_plugin_anchors[0].result));
            V3X_TEST_CHECK(v3x_plugin_load(&real) == 0 && v3x_tk.hooks == 1, "plugin loads and installs exactly 1 hook");
        }
    } else
        printf("(no exe given: anchor check skipped)\n");
    return v3x_testkit_done();
}
