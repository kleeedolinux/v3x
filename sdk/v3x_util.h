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
#include "v3x_safe.h"   /* v3x_widen, v3x_path_join_checkedW, reparse checks */
V3X_NN_BEGIN

/* ---- paths under <game>/binaries/v3x/ */

/* out = v3x_dir() + printf(fmt, ...). Returns out, or NULL (out = "") if it does not fit. */
static char *v3x_path(const V3X_API *api, char *V3X_NN out, size_t cap, const char *V3X_NN fmt, ...) V3X_PRINTF(4, 5);
static char *v3x_path(const V3X_API *api, char *out, size_t cap, const char *fmt, ...)
{
    if (!out || !cap) return NULL;
    if (!fmt) { out[0] = 0; return NULL; }
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
static char *v3x_plugin_file(const V3X_API *api, const char *plugin, const char *suffix, char *V3X_NN out, size_t cap)
{
    return v3x_path(api, out, cap, "%s%s", plugin ? plugin : "", suffix ? suffix : "");
}

/* ---- atomic file writes (audit N6: no reparse point in the chain, temp file created exclusively)
 *
 * v3x_file_write_atomicW(path, data, n) writes n bytes so that readers (the player, another tool, the
 * next game start) see the old file or the whole new one, never a partial one:
 *   1. the parent directory is opened with FILE_FLAG_OPEN_REPARSE_POINT, must not be a reparse point,
 *      and is HELD (no FILE_SHARE_DELETE) until the end: neither it nor any directory above it can be
 *      renamed, deleted or swapped for a junction while the write is in progress;
 *   2. the temp file "<name>.<pid>.<tid>.<n>.v3xtmp" is created with CREATE_NEW +
 *      FILE_FLAG_OPEN_REPARSE_POINT (never an existing file, never through a planted link) and its
 *      final path is verified to be inside the held directory;
 *   3. data is written and flushed, then the temp file is renamed over path THROUGH ITS HANDLE
 *      (SetFileInformationByHandle FileRenameInfo, replace existing): no name lookup of the temp file
 *      after it was created. On any failure the temp file is deleted through its handle.
 * v3x_file_write_atomic_inW(base, rel, ...) additionally validates rel (v3x_path_rel_ok_exW) and
 * checks that no component of rel below base is a reparse point - after step 1, so the check cannot
 * race. The char versions take CP_ACP paths (like api->v3x_dir()). All return 0, or -1 with
 * GetLastError() set (ERROR_REPARSE_POINT_ENCOUNTERED = a reparse point refused). Any thread. */
#ifndef ERROR_REPARSE_POINT_ENCOUNTERED
#define ERROR_REPARSE_POINT_ENCOUNTERED 4395L
#endif

/* internal: dir handle held, path = full path, name = its last component (inside path) */
static int v3x_write_atomic_held_(HANDLE dir, const wchar_t *path, const wchar_t *name, const void *data, size_t n)
{
    static volatile LONG seq;
    wchar_t fdir[V3X_PATH_WMAX], ftmp[V3X_PATH_WMAX], tmp[V3X_PATH_WMAX];
    if (v3x_final_path_w_(dir, fdir, V3X_PATH_WMAX)) return -1;
    size_t dl = (size_t)(name - path);   /* path[0..dl) = directory incl. the separator */
    HANDLE h = INVALID_HANDLE_VALUE;
    DWORD err = 0;
    for (int attempt = 0; attempt < 16 && h == INVALID_HANDLE_VALUE; attempt++) {
        int k = _snwprintf(tmp, V3X_PATH_WMAX, L"%ls.%lu.%lu.%ld.v3xtmp", path, (unsigned long)GetCurrentProcessId(),
                           (unsigned long)GetCurrentThreadId(), (long)InterlockedIncrement(&seq));
        if (k < 0 || k >= V3X_PATH_WMAX) { SetLastError(ERROR_FILENAME_EXCED_RANGE); return -1; }
        h = CreateFileW(tmp, GENERIC_WRITE | DELETE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        err = GetLastError();
        if (h == INVALID_HANDLE_VALUE && err != ERROR_FILE_EXISTS && err != ERROR_ALREADY_EXISTS) return -1;
    }
    if (h == INVALID_HANDLE_VALUE) { SetLastError(err); return -1; }
    /* the temp file must be <final dir>\<temp name> */
    int ok = v3x_final_path_w_(h, ftmp, V3X_PATH_WMAX) == 0;
    if (ok) {
        size_t fl = wcslen(fdir), tl = wcslen(tmp + dl);
        ok = fl + 1 + tl < V3X_PATH_WMAX;
        if (ok) {
            fdir[fl] = L'\\';
            memcpy(fdir + fl + 1, tmp + dl, (tl + 1) * sizeof(wchar_t));
            ok = v3x_path_ieq_w_(ftmp, fdir);
        }
        if (!ok) err = ERROR_REPARSE_POINT_ENCOUNTERED;
    } else err = GetLastError();
    const char *p = (const char *)data;
    size_t left = n;
    while (ok && left) {
        DWORD w = 0, chunk = left > (1u << 30) ? (1u << 30) : (DWORD)left;
        ok = WriteFile(h, p, chunk, &w, NULL) && w == chunk;
        if (!ok) err = GetLastError();
        p += w; left -= w;
    }
    if (ok && !FlushFileBuffers(h)) { ok = 0; err = GetLastError(); }
    if (ok) {   /* rename through the handle */
        size_t pl = wcslen(path);
        size_t bytes = offsetof(FILE_RENAME_INFO, FileName) + (pl + 1) * sizeof(wchar_t);
        FILE_RENAME_INFO *ri = (FILE_RENAME_INFO *)calloc(1, bytes);
        if (!ri) { ok = 0; err = ERROR_NOT_ENOUGH_MEMORY; }
        else {
            ri->ReplaceIfExists = TRUE;
            ri->RootDirectory = NULL;
            ri->FileNameLength = (DWORD)(pl * sizeof(wchar_t));
            memcpy(ri->FileName, path, (pl + 1) * sizeof(wchar_t));
            ok = SetFileInformationByHandle(h, FileRenameInfo, ri, (DWORD)bytes) != 0;
            if (!ok) err = GetLastError();
            free(ri);
        }
    }
    if (!ok) v3x_delete_by_handle_(h);
    CloseHandle(h);
    if (ok) return 0;
    SetLastError(err ? err : ERROR_WRITE_FAULT);
    return -1;
}

/* internal: full = absolute path of the target. from = length of the trusted base incl. its separator
 * (SIZE_MAX: none): the chain full[from..] is checked, and the parent may be a reparse point only if
 * it IS the trusted base. The parent is opened with FILE_LIST_DIRECTORY so that the missing
 * FILE_SHARE_DELETE really blocks renames (an attributes-only handle takes no part in sharing). */
static int v3x_write_atomic_full_(const wchar_t *full, size_t from, const void *data, size_t n)
{
    const wchar_t *name = NULL;
    for (const wchar_t *q = full; *q; q++)
        if (*q == L'\\' || *q == L'/') name = q + 1;
    if (!name || !*name) { SetLastError(ERROR_INVALID_NAME); return -1; }
    wchar_t dir[V3X_PATH_WMAX];
    size_t dl = (size_t)(name - full);
    memcpy(dir, full, dl * sizeof(wchar_t));
    dir[dl] = 0;
    if (dl > 1 && dir[dl - 2] != L':') dir[dl - 1] = 0;   /* "C:\a\" -> "C:\a"; keep "C:\" */
    int parent_trusted = from != SIZE_MAX && dl <= from;
    const DWORD acc = FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, sh = FILE_SHARE_READ | FILE_SHARE_WRITE;
    HANDLE hd = CreateFileW(dir, acc, sh, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (hd == INVALID_HANDLE_VALUE) return -1;
    BY_HANDLE_FILE_INFORMATION fi;
    int rc = -1;
    if (!GetFileInformationByHandle(hd, &fi)) { /* GetLastError set */ }
    else if ((fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && !parent_trusted) SetLastError(ERROR_REPARSE_POINT_ENCOUNTERED);
    else {
        if (fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {   /* the trusted base is a link: hold its target */
            CloseHandle(hd);
            hd = CreateFileW(dir, acc, sh, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
            if (hd == INVALID_HANDLE_VALUE) return -1;
        }
        if (from != SIZE_MAX && v3x_path_has_reparseW(full, from)) SetLastError(ERROR_REPARSE_POINT_ENCOUNTERED);
        else rc = v3x_write_atomic_held_(hd, full, name, data, n);
    }
    DWORD e = GetLastError();
    CloseHandle(hd);
    SetLastError(e);
    return rc;
}

static int v3x_file_write_atomicW(const wchar_t *V3X_NN path, const void *data, size_t n)
{
    wchar_t full[V3X_PATH_WMAX];
    if (!path || !path[0] || (!data && n)) { SetLastError(ERROR_INVALID_PARAMETER); return -1; }
    DWORD k = GetFullPathNameW(path, V3X_PATH_WMAX, full, NULL);
    if (k == 0 || k >= V3X_PATH_WMAX) { SetLastError(k ? ERROR_FILENAME_EXCED_RANGE : GetLastError()); return -1; }
    return v3x_write_atomic_full_(full, SIZE_MAX, data, n);
}

/* path in the ANSI code page (CP_ACP), e.g. built from api->v3x_dir() */
static int v3x_file_write_atomic(const char *V3X_NN path, const void *data, size_t n)
{
    wchar_t w[V3X_PATH_WMAX];
    if (!path || (!data && n)) { SetLastError(ERROR_INVALID_PARAMETER); return -1; }
    if (v3x_widen(CP_ACP, path, SIZE_MAX, w, V3X_PATH_WMAX) != V3X_R_OK) { SetLastError(ERROR_NO_UNICODE_TRANSLATION); return -1; }
    return v3x_file_write_atomicW(w, data, n);
}

/* base\rel: rel validated, no reparse point in base\rel's chain below base (checked while the
 * parent directory is held), parent directories must exist. */
static int v3x_file_write_atomic_inW(const wchar_t *V3X_NN base, const wchar_t *V3X_NN rel, const void *data, size_t n)
{
    wchar_t joined[V3X_PATH_WMAX], full[V3X_PATH_WMAX], fbase[V3X_PATH_WMAX];
    if (!base || !base[0] || !rel || (!data && n)) { SetLastError(ERROR_INVALID_PARAMETER); return -1; }
    V3xResult r = v3x_path_join_checkedW(joined, V3X_PATH_WMAX, base, rel, 0);
    if (r != V3X_R_OK) { SetLastError(r == V3X_R_ETRUNC ? ERROR_FILENAME_EXCED_RANGE : ERROR_INVALID_NAME); return -1; }
    DWORD k = GetFullPathNameW(joined, V3X_PATH_WMAX, full, NULL);
    DWORD kb = GetFullPathNameW(base, V3X_PATH_WMAX, fbase, NULL);
    if (k == 0 || k >= V3X_PATH_WMAX || kb == 0 || kb >= V3X_PATH_WMAX) { SetLastError(ERROR_FILENAME_EXCED_RANGE); return -1; }
    while (kb > 0 && (fbase[kb - 1] == L'\\' || fbase[kb - 1] == L'/')) fbase[--kb] = 0;
    /* full must still start with base\ (GetFullPathName cannot move it: rel has no "..") */
    if (wcsncmp(full, fbase, kb) != 0 || full[kb] != L'\\') { SetLastError(ERROR_INVALID_NAME); return -1; }
    return v3x_write_atomic_full_(full, (size_t)kb + 1, data, n);
}

static int v3x_file_write_atomic_in(const char *V3X_NN base, const char *V3X_NN rel, const void *data, size_t n)
{
    wchar_t wb[V3X_PATH_WMAX], wr[V3X_PATH_REL_MAX + 1];
    if (!base || !rel || (!data && n)) { SetLastError(ERROR_INVALID_PARAMETER); return -1; }
    if (v3x_widen(CP_ACP, base, SIZE_MAX, wb, V3X_PATH_WMAX) != V3X_R_OK ||
        strnlen(rel, V3X_PATH_REL_MAX + 1) > V3X_PATH_REL_MAX || v3x_widen(CP_ACP, rel, SIZE_MAX, wr, V3X_PATH_REL_MAX + 1) != V3X_R_OK) {
        SetLastError(ERROR_INVALID_NAME);
        return -1;
    }
    return v3x_file_write_atomic_inW(wb, wr, data, n);
}

/* Cross-process lock for read-modify-write of the shared v3x.ini (audit L11: mod_menu, doctor and the
 * installer all rewrite it). Named mutex V3X_CONFIG_MUTEX; waits up to timeout_ms (an abandoned mutex
 * counts as acquired). Returns the handle to pass to v3x_config_unlock, or NULL on timeout/error (the
 * caller decides: the bundled plugins log and write anyway). Pair with v3x_file_write_atomic.
 *   HANDLE lk = v3x_config_lock(5000); ...read, edit, v3x_file_write_atomic...; v3x_config_unlock(lk); */
#define V3X_CONFIG_MUTEX L"Local\\v3x_ini"
static HANDLE v3x_config_lock(DWORD timeout_ms)
{
    HANDLE m = CreateMutexW(NULL, FALSE, V3X_CONFIG_MUTEX);
    if (!m) return NULL;
    DWORD w = WaitForSingleObject(m, timeout_ms);
    if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) { CloseHandle(m); return NULL; }
    return m;
}
static void v3x_config_unlock(HANDLE m)
{
    if (m) { ReleaseMutex(m); CloseHandle(m); }
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
static void v3x_logf(const V3X_API *api, int level, const char *tag, const char *fmt, ...) V3X_PRINTF(4, 5);
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

V3X_NN_END
#endif
