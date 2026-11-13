/* Test plugin for test/api_v4_test.c (manifest handling in the loader). Built three times:
 *   (default)          manifest "v4_ok" 2.3.4, any build      -> loads, sets env V3X_V4_OK=1
 *   -DV4_REQUIRE       requires game build "no-such-build"     -> refused, never loaded
 *   -DV4_MINAPI=99     needs loader API v99                    -> refused, never loaded
 *   -DV4_WARN          lists "no-such-build" without REQUIRE   -> WARNING, but loads */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "v3x.h"

#if defined(V4_REQUIRE)
V3X_PLUGIN_MANIFEST_EX("v4_require", "1.0.0", "test", "must be refused (build)", "no-such-build;release/0.0*",
                       V3X_MANIFEST_REQUIRE_BUILD, 0);
#define ENV "V3X_V4_REQUIRE"
#elif defined(V4_MINAPI)
V3X_PLUGIN_MANIFEST_EX("v4_minapi", "1.0.0", "test", "must be refused (api)", NULL, 0, V4_MINAPI);
#define ENV "V3X_V4_MINAPI"
#elif defined(V4_WARN)
V3X_PLUGIN_MANIFEST("v4_warn", "0.9", "test", "loads with a warning", "no-such-build");
#define ENV "V3X_V4_WARN"
#else
V3X_PLUGIN_MANIFEST("v4_ok", "2.3.4", "Test Author", "manifest test plugin", "*");
#define ENV "V3X_V4_OK"
#endif

V3X_EXPORT int v3x_plugin_load(const V3X_API *api)
{
    (void)api;
    SetEnvironmentVariableA(ENV, "1");
    return 0;
}
