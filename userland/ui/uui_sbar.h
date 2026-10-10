#ifndef UUI_SBAR_H
#define UUI_SBAR_H

#include <stdint.h>
#include "ui/ugfx.h"

// uui_sbar -- one vertical scrollbar's STATE, for a widget to embed: the
// overlay bar in a reserved strip, its hover, its thumb grab and its
// track paging, so every scrolling view looks and behaves the same
// (docs/gui-guidelines.md, "Scrollbars"). uui_scrollbar.h is the
// stateless painter and geometry underneath.
//
// **THE WIDGET OWNS THE POSITION; THIS OWNS THE BAR.** A widget says
// how much content there is, how much shows and where the view starts
// (uui_sbar_set()), routes its pointer events through here first, and
// moves its view when told to. Positions count from the START here --
// rows, or pixels, whatever unit the widget scrolls in -- and the
// painter's from-the-bottom scrollback offset is converted inside, once.
//
// **ONE COPY OF THE INPUT RULES.** Every widget used to carry its own
// press/motion/release, and the copies drifted: Places drew a bar that
// would not drag. A new scrolling view embeds this and gets all of it.

struct uui_sbar {
    int x, y, w, h;    // the strip; `w` is uui_sbar_width() when it was placed
    int total;         // content, in the widget's units
    int visible;       // how much of it shows
    int top;           // the first unit shown, from the start
    int step;          // one line, in those units: a page is visible - step
    int grab;          // OWNED: where in the thumb a drag holds it, or -1
    int hover;         // OWNED: the pointer is on the strip
    // The widening, eased: out over a few frames, back only after the
    // pointer has been gone a moment, so a hand drifting off does not
    // snatch the bar away -- the Start menu's numbers. A function of
    // time from the last change, so a draw from a copy reads it right.
    int wide_from;                  // OWNED: 0..255 at the last change
    unsigned long long changed_ns;  // OWNED
};

// What a pointer event did, for the widget to act on.
#define UUI_SBAR_TOOK   0x1   // it was the bar's: the widget does nothing else with it
#define UUI_SBAR_MOVED  0x2   // the view moved: read the new start from `top`
#define UUI_SBAR_REDRAW 0x4   // the bar looks different (hover, a grab)

void uui_sbar_init(struct uui_sbar *b);
// The reserved strip's width: a row less than a line, in the interface face.
int  uui_sbar_width(void);
// Where the strip is. A widget reserves it only while uui_sbar_shown().
// The width is taken HERE, in whatever face is selected -- so place it
// in the interface face, as every measurement of chrome is.
void uui_sbar_place(struct uui_sbar *b, int x, int y, int h);
// The content, before drawing or routing. `top` is clamped here.
void uui_sbar_set(struct uui_sbar *b, int total, int visible, int top);
// Is there anything to scroll? A bar with nothing to do is not drawn.
int  uui_sbar_shown(const struct uui_sbar *b);
// The strip's rect, for a widget's own hit test; 0 when not shown.
int  uui_sbar_hit(const struct uui_sbar *b, int cx, int cy);
// The bar over `ground`, in `ink`'s family: thin at rest, the groove and
// the full thumb while hovered or held. Asks for frames while it eases.
void uui_sbar_draw(const struct uui_sbar *b, struct ugfx_surface *s,
                   uint32_t ground, uint32_t ink);
// Pointer events, routed here FIRST: a press on the strip grabs the
// thumb or pages; motion drags (and tracks hover with no button); a
// release lets go. Each returns UUI_SBAR_* bits.
int  uui_sbar_press(struct uui_sbar *b, int cx, int cy);
int  uui_sbar_motion(struct uui_sbar *b, int cx, int cy, unsigned buttons);
int  uui_sbar_release(struct uui_sbar *b);
// The pointer left the widget: the bar goes thin unless a drag holds it.
int  uui_sbar_leave(struct uui_sbar *b);

#endif
