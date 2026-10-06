// The directory walk -- see uwalk.h.
#include "lib/uwalk.h"
#include <string.h>
#include "kpath.h"

static int push(struct uwalk *w, const char *path) {
    if (w->depth >= UWALK_DEPTH) { w->too_deep++; return 0; }
    if (strlcpy(w->frame[w->depth].path, path, UWALK_PATH) >= UWALK_PATH) return 0;
    w->frame[w->depth].offset = 0;
    w->depth++;
    w->dirs++;
    return 1;
}

int uwalk_begin(struct uwalk *w, const char *root) {
    struct sys_stat st;
    memset(w, 0, sizeof *w);
    if (sys_stat(root, &st) != 0 || !st.is_dir) return 0;
    return push(w, root);
}

int uwalk_step(struct uwalk *w, uwalk_fn fn, void *ctx) {
    if (w->stopped || w->depth == 0) return 0;
    int top = w->depth - 1;
    int n = sys_listdir_at(w->frame[top].path, w->page, UWALK_PAGE, w->frame[top].offset);
    if (n < 0) w->unreadable++;
    if (n <= 0) { w->depth--; return w->depth > 0; }

    for (int i = 0; i < n; i++) {
        char path[UWALK_PATH];
        w->entries++;
        if (!k_path_join(w->frame[top].path, w->page[i].name, path, sizeof path)) continue;
        int r = fn(ctx, path, &w->page[i]);
        if (r < 0) { w->stopped = 1; return 0; }
        if (w->page[i].is_dir && r > 0) {
            // DESCEND NOW, and come back to the rest of this page: the
            // parent's frame remembers how far it got.
            w->frame[top].offset += i + 1;
            push(w, path);
            return 1;
        }
    }
    w->frame[top].offset += n;
    return 1;
}
