/* v3x_util.h - the small things every V3X plugin re-implements (header-only, C).
 *
 * Found in the bundled plugins (docs/api-stability.md "SDK audit"): 25 of 28 build
 * "<v3x_dir><name>.ini" / report paths by hand, 15 keep their own QueryPerformanceCounter clock,
 * 8 write reports through a hand-rolled ".tmp + MoveFileEx" dance, none has log levels.
 *
 *   char ini[MAX_PATH];
 *   v3x_plugin_file(api, "my_plugin", ".ini", ini, sizeof ini);       // <v3x>/my_plugin.ini
 *   v3x_path(api, out, sizeof out, "my_plugin_%d.csv", run);          // <v3x>/my_plugin_3.csv
 *   v3x_file_write_atomic(out, text, len);                            // readers never see half a file
 *
 *   double t0 = v3x_now_ms(); ...; double ms = v3x_now_ms() - t0;     // QPC, any thread
 *
 *   V3X_LOGW(api, "my_plugin", "anchor %s failed", name);             // "WARNING: ..." (log_at on v5,
 *   V3X_LOGD(api, "my_plugin", "x=%d", x);                            //  api->log fallback before v5;
 *   if (v3x_log_enabled(api, "my_plugin", V3X_LOG_DEBUG)) dump();     //  debug/trace dropped there)
 *
 *   static void on_reload(const char *ev, void *data, void *u) {     // `reload [plugin]` console command
 *       if (v3x_reload_is_for(data, "my_plugin")) read_config();      // (API v5 event config_reload,
 *   }                                                                 //  main thread)
 *   if (V3X_HAS(api, log_at)) api->on_event(V3X_EVENT_CONFIG_RELOAD, on_reload, NULL);
 *
 * All functions are static; unused ones cost nothing. Nothing here needs a newer loader than v1
 * except where it says so, and nothing calls a V3X_API field without V3X_HAS. */
#ifndef V3X_UTIL_H
#define V3X_UTIL_H

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "v3x.h"

/* ---- paths under <game>/binaries/v3x/ */

/* out = v3x_dir() + printf(fmt, ...). Returns out, or NULL (out = "") if it does not fit. */
static char *v3x_path(const V3X_API *api, char *out, size_t cap, const char *fmt, ...)
{
    if (!out || !cap) return NULL;
    const char *dir = api && api->v3x_dir ? api->v3x_dir() : "";
    int n = snprintf(out, cap, "%s", dir ? dir : "");
    if (n < 0 || (size_t)n >= cap) { out[0] = 0; return NULL; }
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(out + n, cap - (size_t)n, fmt, ap);
    va_end(ap);
    if (m < 0 || (size_t)(n + m) >= cap) { out[0] = 0; return NULL; }
    return out;
}

/* <v3x_dir><plugin><suffix>, e.g. ("my_plugin", ".ini") or ("my_plugin", "_report.txt"). */
static char *v3x_plugin_file(const V3X_API *api, const char *plugin, const char *suffix, char *out, size_t cap)
{
    return v3x_path(api, out, cap, "%s%s", plugin ? plugin : "", suffix ? suffix : "");
}

/* Writes n bytes to path through "<path>.<tid>.tmp" + MoveFileEx(REPLACE_EXISTING): readers (the
 * player, another tool, the next game start) see the old file or the whole new one, never a
 * partial one. Returns 0, or -1 (GetLastError() set; the temp file is removed). Any thread. */
static int v3x_file_write_atomic(const char *path, const void *data, size_t n)
{
    char tmp[MAX_PATH + 24];
    if (!path || (!data && n)) { SetLastError(ERROR_INVALID_PARAMETER); return -1; }
    int k = snprintf(tmp, sizeof tmp, "%s.%lu.tmp", path, (unsigned long)GetCurrentThreadId());
    if (k < 0 || (size_t)k >= sizeof tmp) { SetLastError(ERROR_FILENAME_EXCED_RANGE); return -1; }
    HANDLE h = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    const char *p = (const char *)data;
    size_t left = n;
    BOOL ok = TRUE;
    while (ok && left) {
        DWORD w = 0, chunk = left > (1u << 30) ? (1u << 30) : (DWORD)left;
        ok = WriteFile(h, p, chunk, &w, NULL) && w == chunk;
        p += w; left -= w;
    }
    DWORD err = ok ? 0 : GetLastError();
    CloseHandle(h);
    if (ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 0;
    if (ok) err = GetLastError();
    DeleteFileA(tmp);
    SetLastError(err ? err : ERROR_WRITE_FAULT);
    return -1;
}

/* ---- time (QueryPerformanceCounter; monotonic, any thread) */

static int64_t v3x_qpc(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
static double v3x_qpc_freq(void)
{
    static volatile LONG64 f;   /* constant for the life of the system; racing writers store the same value */
    if (!f) { LARGE_INTEGER q; QueryPerformanceFrequency(&q); f = q.QuadPart; }
    return (double)f;
}
static double v3x_qpc_ms(int64_t ticks) { return (double)ticks * 1000.0 / v3x_qpc_freq(); }
static double v3x_now_ms(void) { return v3x_qpc_ms(v3x_qpc()); }
static double v3x_now_s(void) { return (double)v3x_qpc() / v3x_qpc_freq(); }

/* ---- leveled logging with a fallback for loaders before API v5 */

/* Threshold for tag: api->log_level (v5), else V3X_LOG_INFO (old loaders log everything up to info). */
static int v3x_log_threshold(const V3X_API *api, const char *tag)
{
    return V3X_HAS(api, log_level) ? api->log_level(tag) : V3X_LOG_INFO;
}
static int v3x_log_enabled(const V3X_API *api, const char *tag, int level)
{
    return api && level <= v3x_log_threshold(api, tag);
}

/* api->log_at on v5; on older loaders api->log with the same "ERROR: " / "WARNING: " prefix, and
 * debug/trace lines dropped. The message is formatted here (max ~1.9 KB, like the core's line). */
static void v3x_logf(const V3X_API *api, int level, const char *tag, const char *fmt, ...)
{
    static const char *const prefix[] = {"", "ERROR: ", "WARNING: ", "", "debug: ", "trace: "};
    char buf[1900];
    if (!api || !fmt) return;
    if (level < V3X_LOG_ERROR) level = V3X_LOG_ERROR;
    if (level > V3X_LOG_TRACE) level = V3X_LOG_TRACE;
    if (!v3x_log_enabled(api, tag, level)) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (V3X_HAS(api, log_at)) api->log_at(level, tag, "%s", buf);
    else api->log(tag, "%s%s", prefix[level], buf);
}
#define V3X_LOGE(api, tag, ...) v3x_logf((api), V3X_LOG_ERROR, (tag), __VA_ARGS__)
#define V3X_LOGW(api, tag, ...) v3x_logf((api), V3X_LOG_WARN, (tag), __VA_ARGS__)
#define V3X_LOGI(api, tag, ...) v3x_logf((api), V3X_LOG_INFO, (tag), __VA_ARGS__)
#define V3X_LOGD(api, tag, ...) v3x_logf((api), V3X_LOG_DEBUG, (tag), __VA_ARGS__)
#define V3X_LOGT(api, tag, ...) v3x_logf((api), V3X_LOG_TRACE, (tag), __VA_ARGS__)

/* ---- config reload (API v5 event V3X_EVENT_CONFIG_RELOAD) */

/* 1 if a config_reload event's data addresses `plugin`: NULL (= all plugins) or the same name
 * (case-insensitive). */
static int v3x_reload_is_for(const void *data, const char *plugin)
{
    return !data || (plugin && !_stricmp((const char *)data, plugin));
}

#endif
