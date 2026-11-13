/* v3x_sdk.h - everything a V3X plugin usually needs, in one include.
 *
 *   include/v3x.h       the loader API (V3X_API, API v1-v4)            docs/api-reference.md
 *   include/v3x_rtti.h  MSVC RTTI: vtable by class name, enumerate classes
 *   sdk/v3x_mem.h       fault-proof memory reads (VirtualQuery + ReadProcessMemory)
 *   sdk/v3x_scan.h      sections, unique pattern scans, xrefs, call/rip targets
 *   sdk/v3x_anchor.h    fail-closed anchor tables (string/pattern/shape -> function)
 *   sdk/v3x_ini.h       <plugin>.ini reader
 *   sdk/v3x_hook.h      hook_safe helpers, V3X_PASS8 for raw detours
 *   sdk/v3x_game.h      game context: script RNG scope owned by this thread? main thread?
 *   sdk/v3x_testkit.h   offline tests against victoria3.exe mapped as an image (tests only)
 *
 * All helpers are header-only `static` functions (unused ones cost nothing; build with
 * -Wno-unused-function). Build a plugin: python sdk/build_plugin.py <plugin_dir> (see
 * docs/getting-started.md). Compile with -Iinclude -Isdk. */
#ifndef V3X_SDK_H
#define V3X_SDK_H

#define V3X_SDK_VERSION 1

#include "v3x.h"
#include "v3x_rtti.h"
#include "v3x_mem.h"
#include "v3x_scan.h"
#include "v3x_anchor.h"
#include "v3x_ini.h"
#include "v3x_hook.h"
#include "v3x_game.h"

#endif
