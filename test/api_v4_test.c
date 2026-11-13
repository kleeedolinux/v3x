/* End-to-end test of loader API v4 (no game): links build/version.dll like victoria3.exe, so V3X
 * boots and loads every DLL in <dir>/v3x/plugins before main. Checks hook_safe through the loader,
 * main_thread_id, game_build_id, enum_commands, the plugin manifest (shown by `plugins`, min_api
 * and REQUIRE_BUILD refusals, mismatch warning) and the v3x.ini [plugins] skip list.
 * Setup (from v3x/, $CC and $T as in README "Tests"):
 *   D=$T/api_v4_test; mkdir -p $D/v3x/plugins && cp build/version.dll $D/
 *   $CC -shared -o $D/v3x/plugins/v4_ok.dll test/v4_plugin.c
 *   $CC -shared -DV4_REQUIRE -o $D/v3x/plugins/v4_require.dll test/v4_plugin.c
 *   $CC -shared -DV4_MINAPI=99 -o $D/v3x/plugins/v4_minapi.dll test/v4_plugin.c
 *   $CC -shared -DV4_WARN -o $D/v3x/plugins/v4_warn.dll test/v4_plugin.c
 *   cp build/plugins/hello.dll $D/v3x/plugins/ && printf '[plugins]\nHello = 0\nv4_ok=1\n' > $D/v3x/v3x.ini
 *   $CC -o $D/api_v4_test.exe test/api_v4_test.c -Lbuild -l:version.dll
 *   (cd $D && V3X_NO_CONSOLE=1 ./api_v4_test.exe)
 * Exit code 0 = ALL OK. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/v3x.h"

__declspec(dllimport) const V3X_API *v3x_api(void);

static int fails, checks;
#define CHECK(c, ...) do { int ok_ = !!(c); checks++; printf("%s: ", ok_ ? "OK  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); fails += !ok_; } while (0)

static volatile long long sink_;
__attribute__((noinline)) long long six(long long a, long long b, long long c, long long d, long long e, long long f)
{
    sink_++;
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f;
}
static long long (*volatile p_six)(long long, long long, long long, long long, long long, long long) = six;
static int g_pre, g_post;
static void pre(V3X_HookCall *c) { g_pre += c->args[5] == 6 && c->user == (void *)&g_pre; c->args[4] = 50; }
static void post(V3X_HookCall *c) { g_post += c->ret == 1 + 4 + 9 + 16 + 250 + 36; c->ret += 1; }

static int g_ncmd, g_has_plugins;
static void on_cmd(const char *name, const char *help, void *user)
{
    (void)help; (void)user;
    g_ncmd++;
    g_has_plugins |= !strcmp(name, "plugins");
}

static char g_out[16384];
static void sink(const char *text, void *user)
{
    (void)user;
    strncat(g_out, text, sizeof g_out - strlen(g_out) - 1);
}

static int line_has(const char *hay, const char *a, const char *b)   /* a and b on the same line */
{
    for (const char *p = hay; (p = strstr(p, a));) {
        const char *ls = p, *le = strchr(p, '\n');
        while (ls > hay && ls[-1] != '\n') ls--;
        if (!le) le = p + strlen(p);
        const char *q = strstr(ls, b);
        if (q && q < le) return 1;
        p += strlen(a);
    }
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const V3X_API *api = v3x_api();
    CHECK(api && api->api_version >= 4 && api->struct_size == sizeof(V3X_API), "api v%u (>= 4), struct_size %u",
          api ? api->api_version : 0, api ? api->struct_size : 0);
    CHECK(V3X_HAS(api, hook_safe) && V3X_HAS(api, main_thread_id) && V3X_HAS(api, game_build_id) && V3X_HAS(api, enum_commands),
          "V3X_HAS: all four v4 fields");
    CHECK(api->main_thread_id() == GetCurrentThreadId(), "main_thread_id = the thread that ran the entry point");

    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    char want[40];
    snprintf(want, sizeof want, "%08lx-%lx", (unsigned long)nt->FileHeader.TimeDateStamp, (unsigned long)nt->OptionalHeader.SizeOfImage);
    CHECK(!strcmp(api->game_build_id(), want), "game_build_id %s (want %s)", api->game_build_id(), want);

    CHECK(api->hook_safe(NULL, pre, post, NULL) == -1 && api->hook_safe((void *)six, NULL, NULL, NULL) == -1, "hook_safe bad args -> -1");
    CHECK(api->hook_safe((void *)six, pre, post, &g_pre) == 0, "hook_safe through the loader");
    long long r = p_six(1, 2, 3, 4, 5, 6);
    CHECK(r == 1 + 4 + 9 + 16 + 250 + 36 + 1 && g_pre == 1 && g_post == 1, "6-arg call: pre changed arg 5, post ret+1 -> %lld", r);
    CHECK(api->unhook((void *)six) == 0 && p_six(1, 2, 3, 4, 5, 6) == 91 && g_pre == 1, "unhook restores the original");

    int n = api->enum_commands(on_cmd, NULL);
    CHECK(n >= 6 && n == g_ncmd && g_has_plugins, "enum_commands: %d commands, 'plugins' among them", n);
    CHECK(api->enum_commands(NULL, NULL) == -1, "enum_commands(NULL) -> -1");

    g_out[0] = 0;
    CHECK(api->exec_capture("plugins", sink, NULL) == 0, "exec 'plugins'");
    printf("---- plugins output\n%s----\n", g_out);
    CHECK(line_has(g_out, "v4_ok", "2.3.4") && line_has(g_out, "v4_ok", "loaded at"), "manifest name + version + loaded");
    CHECK(line_has(g_out, "manifest test plugin", "Test Author"), "manifest description + author line");
    CHECK(line_has(g_out, "v4_require", "refused: game"), "REQUIRE_BUILD mismatch refused");
    CHECK(line_has(g_out, "v4_minapi", "refused: needs loader API v99"), "min_api 99 refused");
    CHECK(line_has(g_out, "v4_warn", "loaded at"), "build mismatch without REQUIRE_BUILD still loads");
    CHECK(line_has(g_out, "hello", "disabled (v3x.ini"), "hello disabled by v3x.ini [plugins] (case-insensitive key)");
    char e[8];
    CHECK(GetEnvironmentVariableA("V3X_V4_OK", e, sizeof e) && GetEnvironmentVariableA("V3X_V4_WARN", e, sizeof e),
          "v4_ok and v4_warn ran v3x_plugin_load");
    CHECK(!GetEnvironmentVariableA("V3X_V4_REQUIRE", e, sizeof e) && !GetEnvironmentVariableA("V3X_V4_MINAPI", e, sizeof e),
          "refused plugins never ran v3x_plugin_load");

    FILE *f = fopen("v3x/v3x.log", "rb");
    static char log[65536];
    size_t ln = f ? fread(log, 1, sizeof log - 1, f) : 0;
    if (f) fclose(f);
    log[ln] = 0;
    CHECK(strstr(log, "WARNING: plugin 'v4_warn' lists game builds") != NULL, "mismatch WARNING in v3x.log");
    CHECK(strstr(log, "plugin hello disabled (v3x.ini)") != NULL && strstr(log, "1 disabled in v3x.ini"), "skip logged in v3x.log");

    printf("%d checks, %d failed\n%s\n", checks, fails, fails ? "FAILED" : "ALL OK");
    return fails ? 1 : 0;
}
