#ifndef RUBBERBAND_H
#define RUBBERBAND_H

#include <stdint.h>

// Rubber-band selection: drag a rectangle over a set of items and select
// what it touches, the way every desktop and file manager does it.
//
// **This module owns the BEHAVIOUR, the caller owns the items.** It
// knows about a drag in progress, which items the band covers, what the
// selection is, and how a modifier changes the answer. It never learns
// what an item is, where the items live, or how they are drawn -- the
// caller answers "how many?" and "where is item i?" through struct
// rb_ops and does its own drawing. That split is what lets the kernel's
// desktop and a ring-3 file manager use one implementation instead of
// two that drift, which is this project's standing rule for widget
// behaviour (docs/gui-guidelines.md).
//
// **It is SHARED SOURCE compiled twice** -- once into the kernel and
// once into libuapp.a (see the Makefile's build/userland/shared/ rule),
// the same arrangement kernel/lib/geom.c already uses. So it must stay
// freestanding: <stdint.h> only, no kernel state, no allocator, no
// drawing.
//
// It deliberately does NOT do group DRAGGING (moving every selected item
// together). That is the natural next layer and this is the piece it
// would be built on: a caller that wants it has rb_is_selected() to
// iterate and a band that is already out of the way once rb_end() has
// run.

// Items are tracked in a fixed bitset, because there is no allocator on
// either side of this boundary. Sized to SYS_LISTDIR_MAX so a file
// manager pane's full listing fits (its rows can still be listing+1 --
// the synthetic "..", which cannot be selected anyway); a caller with
// more items must page them, and rb_ops::count is clamped to this so an
// over-large answer can never write past the bitset.
#define RB_MAX_ITEMS 256
#define RB_WORDS ((RB_MAX_ITEMS + 31) / 32)

// How far the pointer must travel before a press becomes a BAND rather
// than a click. Without this every click is a zero-size drag that
// selects nothing, so clicking an item would clear the selection and
// then immediately be reported as a band.
#define RB_DRAG_THRESHOLD 4

// How a drag combines with the selection that was already there.
enum rb_mode {
    RB_REPLACE, // plain drag: the band's contents become the selection
    RB_ADD,     // Ctrl/Shift: the band ADDS to what was selected
    RB_TOGGLE,  // Ctrl: the band flips each item it covers
};

// What the caller must be able to answer about its items. Geometry only.
// `index` is always in [0, count).
struct rb_ops {
    int (*count)(void *ctx);
    void (*rect)(void *ctx, int index, int *x, int *y, int *w, int *h);
};

struct rubberband {
    int armed;      // a press happened; may or may not become a band
    int active;     // the threshold was crossed -- there IS a band
    int ax, ay;     // where the press landed
    int cx, cy;     // where the pointer is now
    enum rb_mode mode;
    uint32_t sel[RB_WORDS];
    uint32_t base[RB_WORDS]; // the selection when this drag began
};

// --- selection state --------------------------------------------------

// Drop the selection and any drag in progress. Also the correct way to
// initialise one: a zeroed struct rubberband is already valid, and this
// says so at the call site.
void rb_clear(struct rubberband *rb);

int rb_is_selected(const struct rubberband *rb, int index);
void rb_select(struct rubberband *rb, int index, int on);
int rb_selected_count(const struct rubberband *rb);

// The lowest selected index, or -1. For a caller that has one "current"
// item (the desktop's double-click target, a file manager's preview).
int rb_first_selected(const struct rubberband *rb);

// --- the drag ---------------------------------------------------------

// A press at (x, y). Records the anchor and remembers the current
// selection, which is what lets RB_ADD and RB_TOGGLE stay correct as the
// band grows AND shrinks.
void rb_begin(struct rubberband *rb, int x, int y, enum rb_mode mode);

// The pointer moved to (x, y). Past the threshold this recomputes the
// selection FROM the remembered base every time, rather than
// accumulating -- so pulling the band back off an item deselects it
// again, which is what a real one does and what an accumulating
// implementation gets wrong.
//
// `ops` MAY BE NULL, and that means a band that selects nothing: the
// rectangle still tracks and rb_rect() still answers, which is all a
// caller dragging over a picture rather than over a list needs.
void rb_motion(struct rubberband *rb, int x, int y,
                const struct rb_ops *ops, void *ctx);

// The button was released. Returns 1 if this was a BAND (the threshold
// was crossed), 0 if it was just a click -- so a caller can skip its own
// click handling in the first case without tracking that itself.
//
// A plain click in RB_REPLACE mode clears the selection, which is the
// "click empty space to deselect" behaviour, and falls out of the same
// rule rather than being a special case.
int rb_end(struct rubberband *rb);

// The band's rectangle, normalised so w/h are never negative (a drag
// up-and-left is as ordinary as any other). Returns 0 and touches
// nothing when there is no band to draw.
int rb_rect(const struct rubberband *rb, int *x, int *y, int *w, int *h);

// Do two rectangles overlap at all? Exported because it is the one piece
// of geometry a caller may legitimately need itself (hit-testing a click
// against the same items), and having two copies of an inclusive/
// exclusive edge convention is exactly how they come to disagree.
int rb_overlaps(int ax, int ay, int aw, int ah,
                 int bx, int by, int bw, int bh);

#endif
