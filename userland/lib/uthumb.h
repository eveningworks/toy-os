#ifndef UTHUMB_H
#define UTHUMB_H

#include <stdint.h>
#include "lib/uimg.h"
#include "rt/sys.h"   // struct rtc_time

// uthumb -- thumbnails: a LOOKUP on the draw path, a decode on a WORKER
// THREAD, and a copy on disk (/var/cache/thumbnails) that outlives the
// process. The File Manager's icons view, the Image Viewer's filmstrip
// and the Video Player's playlist all draw through it -- a video's
// thumbnail is one of its frames (lib/uvid.h).
//
// THE LOOKUP NEVER DECODES. uthumb_get() answers from the table or
// queues the file and answers NULL; the worker decodes one at a time and
// the app is woken through its own `wake` callback (a uapp_post(), in
// practice) to call uthumb_posted() on its main thread, which installs
// the result. Nautilus's shape and Dolphin's; neither decodes on the UI
// thread and neither has a batch size.
//
// KEYED BY PATH + MTIME + SIZE ON DISK + PIXEL SIZE, so a rewritten file
// re-decodes, a renamed one misses, and the same file at two sizes (a
// grid cell and a preview) is two entries rather than one evicting the
// other every frame. LRU over a fixed table, icon_cache's arrangement.
//
// ONE PROCESS, ONE CACHE: the state is file-scope, and uthumb_init() is
// called once.

struct uthumb_config {
    // Called (possibly from the WORKER thread) when uthumb_posted() has
    // work: it must hand control back to the main thread -- a
    // uapp_post() -- and return nonzero if it could.
    int (*wake)(void *ctx);
    void *ctx;
    // Called on the MAIN thread right after a thumbnail was written to
    // the disk cache: an app watching the volume-wide SYS_FS_GENERATION
    // claims it here, or it reloads in answer to its own write. NULL is
    // fine for one that watches per folder (SYS_FS_GENERATION_OF).
    void (*stored)(void *ctx);
    // The prefix of the one line logged when a queue drains: "<prefix>:
    // N thumbnail(s) in T ms, C from the cache". Tests wait on it.
    const char *log_prefix;
};

void uthumb_init(const struct uthumb_config *cfg);

// The thumbnail of `path` no bigger than `px` on its longer side, or
// NULL -- not ready yet (it is queued, and `wake` will fire), or not a
// picture. `mtime`/`size` are the file's, from the listing the caller
// already has; they are the freshness key.
const struct uimg *uthumb_get(const char *path, const struct rtc_time *mtime,
                              uint32_t size, int px);

// The short NOTE beside a ready thumbnail -- a video's length, "1:17" --
// or NULL. A lookup like uthumb_get(), for a badge on the picture.
const char *uthumb_note(const char *path, int px);

// Whether `path` at `px` has been tried and is NOT a picture (or could
// not be decoded) -- so a caller can draw a generic icon rather than a
// placeholder that waits for ever.
int uthumb_failed(const char *path, int px);

// The app's post arrived: install what the worker finished and start the
// next. Returns 1 when a thumbnail became ready (the caller repaints).
int uthumb_posted(void);

// A backstop for a wake that could not be queued; call it from a
// periodic tick. Returns 0.
int uthumb_tick(void);

// --- the cache, from OUTSIDE the worker ----------------------------------
//
// THE DESKTOP IS DRAWN BY THE COMPOSITOR, which must never run a JPEG,
// PNG, GIF or video decoder on a file someone else wrote: one malformed
// picture would take every window down with it. So its thumbnails are
// made by a separate program (/bin/thumb, through uthumb_make()) into
// this same cache, and the compositor only reads finished entries back
// (uthumb_load_cached()) -- Windows' out-of-process thumbnail handlers
// and GNOME's sandboxed thumbnailers, the same split.

// Make, or confirm fresh, the cache entry for `path` at `px`, here and
// now. 0 when there is one; -1 for a file that is not a picture or a
// video, or one whose path the cache cannot name.
int uthumb_make(const char *path, int px);

// The cache entry for `path` at `px`, when it is newer than `mtime` (the
// file's, from a listing): decoded as QOI AND NOTHING ELSE, so a file of
// another format placed under the cache's name reaches no other decoder.
// 0 and `*out` filled (the caller frees it), or -1.
int uthumb_load_cached(const char *path, const struct rtc_time *mtime, int px, struct uimg *out);

#endif
