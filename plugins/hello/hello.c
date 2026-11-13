/* Minimal V3X plugin template: logs on load and once the game's message loop is running. */
#include "v3x.h"

V3X_EXPORT const char *v3x_plugin_name = "hello";
V3X_PLUGIN_MANIFEST("hello", "0.5.0", "v3x", "minimal example plugin", "*");

static const V3X_API *api;
static unsigned long long frames;

static void on_frame(void *user)
{
    (void)user;
    frames++;
    if (frames == 1 || frames % 3000 == 0)
        api->log("hello", "frame %llu", frames);
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    api->log("hello", "hello from inside %s (game %s, base %p)", api->game_dir(), api->game_version(),
             (void *)api->image_base());
    return api->on_frame(on_frame, 0);
}
