/* Engine patch: allow the developer console in ironman games.
 *
 * How it finds its target without hardcoded addresses:
 *   "No Console commands in ironman!" (.rdata) -> the one lea that loads it (.text)
 *   -> start of the enclosing function via .pdata = the "is console blocked?" predicate.
 * The predicate is `bool f(Ctx *ctx)`: it returns true and writes the reason into an
 * MSVC std::string at (*ctx)+8. We call the original and, only when the reason is the
 * ironman one, report "not blocked". Multiplayer stays blocked (desyncs, fairness). */
#include <string.h>
#include "v3x.h"

V3X_EXPORT const char *v3x_plugin_name = "ironman_console";

static const V3X_API *api;
static const char IRONMAN_MSG[] = "No Console commands in ironman!";

typedef struct { union { char buf[16]; char *ptr; } u; size_t size; size_t cap; } MsvcString;
typedef _Bool (*blocked_fn)(void **ctx);
static blocked_fn orig_blocked;

static _Bool hk_blocked(void **ctx)
{
    _Bool blocked = orig_blocked(ctx);
    if (!blocked || !ctx || !*ctx) return blocked;
    MsvcString *reason = (MsvcString *)((char *)*ctx + 8);
    const char *s = reason->cap >= 16 ? reason->u.ptr : reason->u.buf;
    if (reason->size == sizeof IRONMAN_MSG - 1 && !memcmp(s, IRONMAN_MSG, reason->size)) {
        reason->size = 0;
        ((char *)s)[0] = 0;
        api->log("ironman_console", "console allowed in ironman");
        return 0;
    }
    return blocked;
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    void *str = api->find_string(IRONMAN_MSG);
    if (!str) { api->log("ironman_console", "string not found, game version unsupported"); return 1; }
    void *xref = api->find_xref(str, 0);
    void *fn = xref ? api->function_start(xref) : 0;
    if (!fn) { api->log("ironman_console", "predicate not found (xref=%p)", xref); return 2; }
    int rc = api->hook(fn, (void *)hk_blocked, (void **)&orig_blocked);
    api->log("ironman_console", "predicate at exe+0x%llx, hook %s",
             (unsigned long long)((char *)fn - (char *)api->image_base()), rc == 0 ? "installed" : "FAILED");
    return rc;
}
