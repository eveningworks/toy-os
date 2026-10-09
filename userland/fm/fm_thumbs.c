// Thumbnails for the icons view: the File Manager's side of lib/uthumb.h,
// which owns the cache, the worker and the copy on disk -- shared with the
// Image Viewer's filmstrip.
//
// One of the File Manager's units -- see fm_internal.h for what is
// where and why these share their state directly.
#include "fm_internal.h"
#include "kpath.h"
#include "lib/uthumb.h"

#include <string.h>

static int wake(void *ctx) {
    (void)ctx;
    return uapp_post(g_app, POST_THUMB, 0);
}

static void init_once(void) {
    static int done;
    if (done) return;
    done = 1;
    // "files" is what tools/thumbcache_test.py waits on.
    static const struct uthumb_config cfg = {
        .wake = wake, .log_prefix = "files",
    };
    uthumb_init(&cfg);
}

// The fileview's callback: a lookup that may ENQUEUE, never a decode.
const struct uimg *pane_thumb(void *ctx, const char *dir,
                              const struct sys_dirent *e, int px) {
    (void)ctx;
    char path[PATH_MAX_LEN];
    if (!k_path_join(dir, e->name, path, sizeof path)) return 0;
    init_once();
    return uthumb_get(path, &e->modified, e->size, px);
}

int pane_badge(void *ctx, const char *dir, const struct sys_dirent *e, int px, char *out, int cap) {
    (void)ctx;
    char path[PATH_MAX_LEN];
    if (!k_path_join(dir, e->name, path, sizeof path)) return 0;
    const char *n = uthumb_note(path, px);
    if (!n) return 0;
    strlcpy(out, n, (size_t)cap);
    return 1;
}

int thumb_posted(void) {
    init_once();
    return uthumb_posted();
}

int thumb_tick(void) {
    return uthumb_tick();
}
