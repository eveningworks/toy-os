#ifndef UUI_SCROLLANIM_H
#define UUI_SCROLLANIM_H

// SMOOTH SCROLLING AS A DISPLACEMENT OF THE DRAWN CONTENT, easing to
// zero -- the widget's own position (`top`, `offset`, `scroll_offset`)
// still jumps exactly as it always did, so every reader of it, every
// hit-test and every test tool sees the same numbers as before. Only
// the DRAW adds `disp` to its content y for a few frames. Qt's item
// views animate a per-item scroll the same way.
//
// The widget's three calls:
//
//   uui_scrollanim_arm(&sa)         in its wheel, trough-page and key
//                                   paths -- "the next position change
//                                   is the user scrolling: glide it"
//   uui_scrollanim_cancel(&sa)      in a thumb drag and on a data
//                                   reload -- "draw where the position
//                                   says, now" (gui-guidelines: the
//                                   thumb follows the cursor 1:1)
//   disp = uui_scrollanim_sync(&sa, pos_px)   at the top of draw, with
//                                   the position in PIXELS; add `disp`
//                                   to every content y, and fold it into
//                                   the scrollbar's offset
//
// Arming is explicit rather than inferred from "the position changed",
// because a change that is not the user's (a reload that shortens the
// list, a resize that re-clamps) must not glide -- it would read as
// the content lurching on its own. A change that arrives unarmed just
// resets the reference point.
//
// The sign: `pos_px` grew by D (the view scrolled DOWN, the content
// moved up by D) -> the content is drawn D lower than its new place,
// where it was, and eases to 0. A widget draws the rows that displaced
// content uncovers: with disp > 0 the rows ABOVE its first, with
// disp < 0 the rows BELOW its last, and clips to its own rect.

#include "lib/utween.h"

struct uui_scrollanim {
    int inited;      // has sync() seen a position yet
    int last_px;     // the position at the last sync
    int armed;       // the next change glides
    int enabled;     // the setting, read when armed
    struct utween tw;
    int disp;        // what the last sync returned, for a hit-test mid-glide
};

void uui_scrollanim_init(struct uui_scrollanim *a);
void uui_scrollanim_arm(struct uui_scrollanim *a);
void uui_scrollanim_cancel(struct uui_scrollanim *a);
int  uui_scrollanim_sync(struct uui_scrollanim *a, int pos_px);
static inline int uui_scrollanim_active(const struct uui_scrollanim *a) { return a->tw.active; }

// How many extra rows of `row_h` a displacement uncovers at one end.
static inline int uui_scrollanim_extra_rows(int disp, int row_h) {
    int d = disp < 0 ? -disp : disp;
    return row_h > 0 ? (d + row_h - 1) / row_h : 0;
}

#endif // UUI_SCROLLANIM_H
