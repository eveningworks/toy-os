// The popup-surface seam -- see ui/uui_popup.h. One provider per
// process, installed by uapp; no widget owns it.
#include "ui/uui_popup.h"

static const struct uui_popup_ops *g_ops;
static void *g_ctx;

void uui_popup_set_provider(const struct uui_popup_ops *ops, void *ctx) {
    g_ops = ops;
    g_ctx = ctx;
}

int uui_popup_open(int ax, int ay, int aw, int ah, int w, int h, int gravity,
                   unsigned flags, void (*done)(void *owner), void *owner,
                   int *out_x, int *out_y) {
    if (!g_ops || !g_ops->open) return 0;
    return g_ops->open(g_ctx, ax, ay, aw, ah, w, h, gravity, flags, done, owner,
                       out_x, out_y);
}

void uui_popup_close(int id) {
    if (id > 0 && g_ops && g_ops->close) g_ops->close(g_ctx, id);
}

struct ugfx_surface *uui_popup_surface(int id) {
    if (id <= 0 || !g_ops || !g_ops->surface) return 0;
    return g_ops->surface(g_ctx, id);
}
