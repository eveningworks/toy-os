#ifndef UUI_LAYOUT_H
#define UUI_LAYOUT_H

#include "ui/uui_widget.h"

// uui_layout -- places widgets so apps stop doing coordinate
// arithmetic.
//
// WHY THIS EXISTS
// ---------------
// Removing the event loop (ui/uapp.h) was necessary and not sufficient.
// An app still had to compute every rectangle, which in practice meant
// each one re-deriving the same three things: a control's natural size
// (with the padding constants copied in), each control's position from
// its neighbour's height, and the window's size from both -- a third
// copy of the same numbers. Calculator's `button_rect()` and
// `metrics_init()` were exactly that, and the "Hello button, World
// label" example in docs/uapp-design.md still needed twelve lines of it
// for two controls.
//
// LAYOUT IS NOT RETAINED MODE
// ---------------------------
// Rects are computed once -- at open, and again on a resize -- and
// drawing stays immediate: uui_layout_draw() walks the items and calls
// each one's draw. Nothing retains damage state; the compositor one
// level up keeps doing that job. The first draft of the design rejected
// layout by conflating the two, which was wrong: computing rects once
// and drawing them immediately is what every app here already does by
// hand.
//
// Margins and gaps are FONT-DERIVED by default, so a laid-out window
// reflows with `fontsize` like the rest of the GUI rather than only
// looking right at one size.

enum uui_dir {
    UUI_COLUMN, // stack downward
    UUI_ROW,    // stack rightward
    UUI_GRID,   // uniform cells, `cols` across, filled left-to-right
};

struct uui_layout {
    enum uui_dir dir;
    int cols;     // UUI_GRID only; <= 0 behaves as 1

    // <= 0 means the font-derived default (see uui_layout.c). Set them
    // to pin a specific spacing; leave them alone to get one that
    // reflows.
    //
    // **A DEFAULT MARGIN IS THE WINDOW'S EDGE, NOT EVERY NESTING
    // LEVEL'S.** A layout placed inside another container takes NO
    // margin of its own unless it names one, which is Qt's rule for a
    // sub-layout and GTK's for a box. Without that they compound: System
    // Settings nests three deep and paid three character cells, 28 px
    // before the sidebar and another 14 inside the page.
    int margin, gap;

    // Set by the ops table when this layout is laid out as a CHILD --
    // the one signal that it is not the outermost. Not an app's to set.
    int nested;

    struct uui_item *items; // caller-owned
    int count;

    // Filled in by uui_layout_run(). Read them if you need to know
    // where the container ended up.
    int x, y, w, h;
};

// The spacing this container actually used -- what `margin`/`gap` were
// set to, or the font-derived default when they were left alone. For an
// app that has to place something in the container's coordinates (a
// splitter deriving its travel from the room its two neighbours share);
// re-deriving the defaults in the app is a second copy of them.
int uui_layout_margin(const struct uui_layout *l);
int uui_layout_gap(const struct uui_layout *l);

// The preferred minimum for the whole container, children included.
// This is what a window is sized from -- see uapp's `layout` field.
void uui_layout_natural_size(const struct uui_layout *l, int *out_w, int *out_h);

// Places every child inside the given rectangle, recursively. Call it
// at open and after a resize; it is cheap and idempotent, so calling it
// every draw is fine too (that is what makes a font-size change reflow
// with no invalidation bookkeeping).
void uui_layout_run(struct uui_layout *l, int x, int y, int w, int h);

// Immediate-mode: walks the items in order and calls each one's draw.
//
// ONE PASS, deliberately, and worth knowing before adding a dropdown to
// a layout: uui_dropdown's popup has to be drawn AFTER every other
// widget, because drawing is immediate and z-order is call order. That
// needs a second `draw_overlay` pass here. It is not built, because no
// layout contains a dropdown yet and shipping an untested slot for
// nobody is the habit this project keeps deleting things for -- but the
// change is internal to this function, so it costs no app anything when
// it lands.
void uui_layout_draw(struct ugfx_surface *s, const struct uui_layout *l);

// A layout is itself a widget, so containers nest -- which is how
// Calculator becomes "a column of [display area, button grid]".
extern const struct uui_widget_ops uui_layout_ops;

#endif
