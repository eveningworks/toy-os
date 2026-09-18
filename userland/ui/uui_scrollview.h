#ifndef UUI_SCROLLVIEW_H
#define UUI_SCROLLVIEW_H

#include "ui/uui_widget.h"
#include "ui/uui_layout.h"
#include "ui/uui_scrollanim.h"
#include "ui/uui_route.h"

// uui_scrollview -- a viewport onto a layout that is taller than it.
//
// WHY THIS EXISTS
// ---------------
// uui_layout deliberately OVERFLOWS when it is given less room than its
// children want: it hands each child its natural size and places the
// rest past the bottom edge, rather than shrinking anything below its
// stated minimum. That is the right call for a layout -- squashing a
// control into a size it said it could not use is worse -- but it means
// a window shrunk below its content silently HIDES part of it, with no
// scrollbar and no way to reach what is gone.
//
// Control Panel showed it plainly: shrink the window and six of seven
// timezones become unreachable and the status bar disappears entirely.
// Nothing in Toykit scrolled a PAGE -- uui_listbox and uui_table scroll
// their own rows and that is all.
//
// WHAT AN APP DOES
// ----------------
// Declare the page as an ordinary layout, wrap it, and put the wrapper
// in the window's layout. Nothing else changes:
//
//     static struct uui_item      PAGE[3];      // the content, as usual
//     static struct uui_layout    CONTENT = { .dir = UUI_COLUMN,
//                                             .items = PAGE, .count = 3 };
//     static struct uui_scrollview SCROLL;
//
//     uui_scrollview_init(&SCROLL, &CONTENT);
//     ITEMS[0] = (struct uui_item){ .ops = &uui_scrollview_ops,
//                                    .widget = &SCROLL, .id = ID_PAGE,
//                                    .flags = UUI_FILL_W | UUI_FILL_H };
//
// The app writes no scrolling code at all: the wheel, the scrollbar,
// dragging its thumb, paging in its trough, clipping, clamping and
// re-placing the children are the widget's, per
// docs/gui-guidelines.md's "Behaviour belongs to the component". What
// the app gets to decide is below, as accessors.
//
// HOW IT SCROLLS, because it decides what everything else has to do:
// it re-runs the content layout at a SHIFTED ORIGIN rather than
// translating coordinates at draw and input time. So a child's rect is
// always its real on-screen rect, hit-testing needs no translation, and
// a child that scrolls out of view is simply somewhere else -- which is
// also why input is clipped to the viewport, or a child scrolled above
// the top would still be clickable.
//
// WHAT IT DELIBERATELY IS NOT: horizontal. Nothing here needs it, a
// second axis doubles every rule below, and `docs/roadmap.md` is where
// it goes when something does.

struct uui_scrollview {
    // --- geometry, filled in by the layout -------------------------
    int x, y, w, h;

    // The content. CALLER-OWNED, like everything else a layout holds:
    // its storage must outlive the scrollview.
    struct uui_layout *content;

    // Routes input to the content's items. The scrollview's own, rather
    // than the app's, so a drag inside it works with no app code -- the
    // outer router grabs the scrollview, this one grabs the child.
    struct uui_router router;

    // --- state the widget owns --------------------------------------
    int offset;      // pixels scrolled down; 0 = top. Clamped, always.
    // The glide (ui/uui_scrollanim.h): `offset` jumps, the content is
    // drawn `anim_disp` px from where it says for a few frames.
    struct uui_scrollanim anim;
    int anim_disp;
    int content_h;   // the content's natural height, measured at layout
    int thumb_grab;
    // What the content looked like when it was last positioned. Compared
    // on every draw so a changed item list re-lays itself out WITHOUT
    // the app having to say so -- see uui_scrollview_content_changed().
    struct uui_item *seen_items;
    int seen_count;  // grab offset within the thumb, or -1 when not dragging

    // --- what the app may configure ---------------------------------
    int pref_rows;   // natural height, in text rows (0 = a default)
    int step;        // wheel/arrow step in pixels (0 = one text row)
    int bar_w;       // scrollbar width (0 = the scrollbar's own default)

    // Defaulted from the theme at init, like every other widget's
    // colours -- an app that wants a different look assigns them
    // rather than the widget guessing.
    uint32_t bg, track_bg, thumb_bg;
};

// `content` must outlive the scrollview. Colours come from the theme,
// and the two knobs below get font-derived defaults.
void uui_scrollview_init(struct uui_scrollview *sv, struct uui_layout *content);

// --- what the app may change ------------------------------------------

// How tall the scrollview ASKS to be, in text rows. This is what the
// window is sized from at open, so it is the difference between a window
// that opens showing everything and one that opens scrolled. Capped at
// the content's own height -- asking for twenty rows of a five-row page
// reserves empty space, which is never what the caller meant.
void uui_scrollview_set_preferred_rows(struct uui_scrollview *sv, int rows);

// TELL IT THE CONTENT'S ITEM LIST CHANGED, and it re-measures and
// re-positions. Required after adding, removing or replacing items in
// the layout a scroll view was handed -- the view otherwise lays its
// content out only when its own rect or its offset moves, so brand-new
// widgets are never positioned at all and keep a zero rect.
//
// This is not a repaint request (that is uapp_redraw): it is the third
// thing place_content() depends on, and the one an app can change
// without touching the view.
void uui_scrollview_content_changed(struct uui_scrollview *sv);

// Pixels per wheel notch and per trough-arrow click. 0 restores the
// default of one text row.
void uui_scrollview_set_step(struct uui_scrollview *sv, int px);

// --- state the app may read, and set ----------------------------------

int uui_scrollview_offset(const struct uui_scrollview *sv);
int uui_scrollview_max_offset(const struct uui_scrollview *sv);

// Is the content taller than the viewport -- i.e. is the scrollbar
// showing? Worth asking before drawing anything of your own beside it.
int uui_scrollview_scrollable(const struct uui_scrollview *sv);

// Scroll to an absolute offset. CLAMPED rather than refused, because
// every caller of this wants "as far as it goes" when it overshoots.
// Returns 1 if the offset actually moved.
int uui_scrollview_set_offset(struct uui_scrollview *sv, int px);

// Bring a band of the CONTENT into view -- `y` and `h` in the same
// coordinates the content's children are placed in (i.e. real screen
// coordinates, since that is what a scrolled layout produces). Scrolls
// the least amount that works, and does nothing if it is already
// visible. This is what a keyboard move through a long page needs.
int uui_scrollview_reveal(struct uui_scrollview *sv, int y, int h);

extern const struct uui_widget_ops uui_scrollview_ops;

#endif
