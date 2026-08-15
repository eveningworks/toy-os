#ifndef UUI_CANVAS_H
#define UUI_CANVAS_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"


// Split out of the single uwidgets.c/.h this used to be, one file per
// widget -- the same shape as apps/ui/, so a widget's kernel-side and
// ring-3 versions live at matching paths. See ui/uui.h.

// --- canvas -----------------------------------------------------------
//
// A rectangle an app draws shapes into, with its own local coordinate
// system. It exists rather than apps calling the geometry directly
// because every drawing app otherwise re-derives the same three things:
// where its drawing area sits inside the window, how to turn a local
// coordinate into a surface one, and how to stop a shape escaping its
// box. The third is invisible until a shape grows, and then it paints
// over the app's own chrome.

struct uui_canvas {
    int x, y, w, h;   // content-relative, like every other widget here
    uint32_t bg;
    uint32_t border;  // 0 for none
};

void uui_canvas_init(struct uui_canvas *c, int x, int y, int w, int h,
                      uint32_t bg, uint32_t border);
void uui_canvas_set_geometry(struct uui_canvas *c, int x, int y, int w, int h);

// Fills the background and draws the border. The shape calls below do
// not clear, so call this first.
void uui_canvas_begin(struct ugfx_surface *s, const struct uui_canvas *c);

// Centre of the canvas in LOCAL coordinates -- what a rotating shape
// almost always wants to spin about.
int uui_canvas_cx(const struct uui_canvas *c);
int uui_canvas_cy(const struct uui_canvas *c);

// Shapes, in canvas-LOCAL coordinates, clipped to the canvas so one
// larger than its box is cut at the edge rather than escaping it.
void uui_canvas_line(struct ugfx_surface *s, const struct uui_canvas *c,
                      int x0, int y0, int x1, int y1, uint32_t color, enum geom_aa aa);
void uui_canvas_polyline(struct ugfx_surface *s, const struct uui_canvas *c,
                          const int *xs, const int *ys, int count, int closed,
                          uint32_t color, enum geom_aa aa);
void uui_canvas_ellipse(struct ugfx_surface *s, const struct uui_canvas *c,
                         int cx, int cy, int rx, int ry, uint32_t color, enum geom_aa aa);
void uui_canvas_circle(struct ugfx_surface *s, const struct uui_canvas *c,
                        int cx, int cy, int r, uint32_t color, enum geom_aa aa);
void uui_canvas_fill_ellipse(struct ugfx_surface *s, const struct uui_canvas *c,
                              int cx, int cy, int rx, int ry, uint32_t color);

int uui_canvas_hit(const struct uui_canvas *c, int cx, int cy);

#endif
