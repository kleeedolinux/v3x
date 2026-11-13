/* Builds the vendored Zydis amalgamation (third_party/zydis, MIT) into the devconsole plugin.
 * build.sh compiles every .c file in plugins/<name>/, so this file is what pulls Zydis in.
 * Static build: no dllimport/dllexport on Zydis symbols. Zydis' own warnings are not ours. */
#define ZYDIS_STATIC_BUILD
#define ZYCORE_STATIC_BUILD
#pragma clang diagnostic ignored "-Weverything"
#include "../../third_party/zydis/Zydis.c"
