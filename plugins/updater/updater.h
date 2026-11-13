/* updater.h - the event the updater plugin emits (for mod_menu or any plugin that wants to show it).
 *
 *   static void on_update(const char *ev, void *data, void *u) {
 *       const V3X_UpdateInfo *i = data;
 *       if (i && i->struct_size >= sizeof *i) show(i->message);
 *   }
 *   api->on_event(V3X_EVENT_UPDATE_AVAILABLE, on_update, NULL);
 *
 * Emitted on the MAIN thread (next frame tick after a check found it), once per offered version and
 * session, and again when the download is verified (ready = 1). data is valid only during the callback. */
#ifndef V3X_UPDATER_H
#define V3X_UPDATER_H

#include <stdint.h>

#define V3X_EVENT_UPDATE_AVAILABLE "update_available"

#define V3X_UPDATE_AVAILABLE 3   /* a newer V3X supports this game build */
#define V3X_UPDATE_URGENT 5      /* this game build is unsupported by the installed V3X (plugins gated off)
                                    and the offered release supports it */

typedef struct V3X_UpdateInfo {
    uint32_t struct_size;     /* sizeof(V3X_UpdateInfo); fields are only appended */
    int32_t state;            /* V3X_UPDATE_AVAILABLE or V3X_UPDATE_URGENT */
    const char *installed;    /* running loader version, e.g. "0.5.0" */
    const char *available;    /* offered version, e.g. "0.6.0" */
    const char *message;      /* one line for the player (English) */
    int32_t auto_install;     /* 1 = it is installed automatically when the game exits */
    int32_t ready;            /* 1 = downloaded and SHA-256 verified */
} V3X_UpdateInfo;

#endif
