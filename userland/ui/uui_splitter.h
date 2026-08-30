#ifndef UUI_SPLITTER_H
#define UUI_SPLITTER_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"
#include "ui/uui_widget.h"

// A DRAGGABLE DIVIDER between two things that share a run of space --
// Qt's QSplitter, GTK's GtkPaned, Explorer's navigation-pane divider.
//
// **IT OWNS A FRACTION, NOT A PIXEL COLUMN.** The value is per mille of
// the travel, so a window that gets wider keeps the proportion the user
// chose instead of stranding one side at the size it had when the
// window was small -- and the saved value stays meaningful across a
// font-size change and a different screen. The APP supplies the track
// (`uui_splitter_set_track`) on every layout pass, because only the app
// knows what the two children are and what is left after the chrome.
//
// **A DRAG IS A DELTA FROM THE PRESS, not the pointer's absolute
// position.** The handle then follows the pointer exactly whatever
// offsets, gaps or margins sit between the track's origin and the band
// -- which is what lets the same widget serve File Manager's
// hand-computed rects and System Settings' `uui_layout` row, where the
// layout inserts a gap on each side of the band.
//
// The band is the HIT ZONE and it is wider than the line drawn in it,
// as every real splitter's is: a one-pixel target is not a target.

// The value's range. Per mille rather than percent so a drag on a wide
// window moves the number on every pixel of travel.
#define UUI_SPLIT_SCALE 1000

struct uui_splitter {
    int x, y, w, h;        // the band, content-relative
    int horizontal;        // 1: children side by side, band drags in x

    int frac;              // OWNED -- 0..UUI_SPLIT_SCALE along the track
    int def_frac;          // where a double click puts it back

    // The track, in the app's coordinates, set every layout pass. `lo`
    // and `hi` bracket both children AND the band; the two minima are
    // the pixels each child must keep, which is what stops a pane being
    // dragged to nothing with no handle left to drag back.
    int lo, hi, min_before, min_after;

    int hovered, dragging, focused, disabled;

    // The press anchor -- see the delta note above.
    int anchor_c, anchor_frac;
    unsigned long last_click_tick;
};

// The band's thickness, font-derived like every other size here.
int uui_splitter_thickness(void);

void uui_splitter_init(struct uui_splitter *sp, int horizontal, int frac);

// The two children's span. Call it before reading a position, on every
// layout pass: a resize changes it and nothing else tells the widget.
void uui_splitter_set_track(struct uui_splitter *sp, int lo, int hi,
                             int min_before, int min_after);

void uui_splitter_set_frac(struct uui_splitter *sp, int frac);
int  uui_splitter_frac(const struct uui_splitter *sp);

// Where the split falls, given the current track: `pos` is the band's
// leading edge, `before`/`after` the pixels each child gets. All three
// are derived from the same arithmetic, so a caller that uses two of
// them cannot disagree with itself.
int uui_splitter_pos(const struct uui_splitter *sp);
int uui_splitter_before(const struct uui_splitter *sp);
int uui_splitter_after(const struct uui_splitter *sp);

void uui_splitter_natural_size(const struct uui_splitter *sp, int *out_w, int *out_h);
void uui_splitter_set_geometry(struct uui_splitter *sp, int x, int y, int w, int h);
void uui_splitter_draw(struct ugfx_surface *s, const struct uui_splitter *sp);

int  uui_splitter_hit(const struct uui_splitter *sp, int cx, int cy);
int  uui_splitter_press(struct uui_splitter *sp, int cx, int cy);
int  uui_splitter_drag(struct uui_splitter *sp, int cx, int cy);
void uui_splitter_drag_end(struct uui_splitter *sp);
int  uui_splitter_hover(struct uui_splitter *sp, int cx, int cy);

// Arrow keys nudge, Home/End go to the ends -- GtkPaned's keymap. An
// app with no focus ring (File Manager's Tab is the commander's pane
// swap) can call this from its own on_key instead.
int  uui_splitter_key(struct uui_splitter *sp, int key);

extern const struct uui_widget_ops uui_splitter_ops;

#endif
