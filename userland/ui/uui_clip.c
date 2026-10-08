// One child, clipped to a viewport -- see uui_clip.h.
#include "ui/uui_clip.h"
#include "ui/ugfx.h"

void uui_clip_init(struct uui_clip *c, struct uui_item child) {
    *c = (struct uui_clip){ .child = child };
}

void uui_clip_set_viewport(struct uui_clip *c, int x, int y, int w, int h) {
    c->vx = x; c->vy = y; c->vw = w; c->vh = h;
}

int uui_clip_visible(const struct uui_clip *c) {
    return c->w > 0 && c->h > 0 && c->vw > 0 && c->vh > 0 &&
           c->x < c->vx + c->vw && c->x + c->w > c->vx &&
           c->y < c->vy + c->vh && c->y + c->h > c->vy;
}

static void clip_natural_size(const void *w, int *ow, int *oh) {
    const struct uui_clip *c = w;
    *ow = *oh = 0;
    if (c->child.ops && c->child.ops->natural_size) c->child.ops->natural_size(c->child.widget, ow, oh);
}

static void clip_set_geometry(void *w, int x, int y, int width, int height) {
    struct uui_clip *c = w;
    c->x = x; c->y = y; c->w = width; c->h = height;
    if (c->child.ops && c->child.ops->set_geometry) c->child.ops->set_geometry(c->child.widget, x, y, width, height);
}

// What shows: the child's rect cut to the viewport.
static void clip_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_clip *c = w;
    int x0 = c->x > c->vx ? c->x : c->vx, y0 = c->y > c->vy ? c->y : c->vy;
    int x1 = c->x + c->w < c->vx + c->vw ? c->x + c->w : c->vx + c->vw;
    int y1 = c->y + c->h < c->vy + c->vh ? c->y + c->h : c->vy + c->vh;
    *x = x0; *y = y0;
    *ow = x1 > x0 ? x1 - x0 : 0;
    *oh = y1 > y0 ? y1 - y0 : 0;
}

static struct uui_item *clip_children(void *w, int *n) {
    struct uui_clip *c = w;
    *n = 1;
    return &c->child;
}

// THE HIT IS THE VIEWPORT, which is what makes the router clip input:
// a container with `hit` admits the pointer to its children only where
// this says yes (uui_widget.h, `children`).
static int clip_hit(const void *w, int cx, int cy) {
    const struct uui_clip *c = w;
    return cx >= c->vx && cx < c->vx + c->vw && cy >= c->vy && cy < c->vy + c->vh;
}

// The router paints the child between these two (uui_scrollview.c says
// why a container cannot clip around a draw call of its own). A
// non-positive size is an EMPTY clip, right for an empty viewport.
static void clip_begin(struct ugfx_surface *s, void *w) {
    struct uui_clip *c = w;
    ugfx_set_clip_rect(s, c->vx, c->vy, c->vw, c->vh);
}

static void clip_end(struct ugfx_surface *s, void *w) {
    (void)w;
    ugfx_clear_clip_rect(s);
}

const struct uui_widget_ops uui_clip_ops = {
    .natural_size   = clip_natural_size,
    .set_geometry   = clip_set_geometry,
    .bounds         = clip_bounds,
    .children       = clip_children,
    .children_begin = clip_begin,
    .children_end   = clip_end,
    .hit            = clip_hit,
};
