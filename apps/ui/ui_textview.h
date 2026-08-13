#ifndef UI_TEXTVIEW_H
#define UI_TEXTVIEW_H

#include <stdint.h>
#include "ui_scrollback.h"
#include "ui_scrollbar.h"
#include "ui_primitives.h"

// A scrollable text view: a text_scrollback, its scrollbar, the
// geometry that splits the two, and all of the input handling that
// makes scrolling work -- as ONE control.
//
// **Why this exists.** Scrolling used to be the app's job. Notepad,
// Terminal and UI Demo each carried the same ~20 lines: work out
// whether the content overflows, reserve a strip on the right if it
// does, page on a track click, remember a grab offset and follow the
// thumb, multiply wheel notches by 3. Three copies of one behaviour,
// and the newest of them (UI Demo) shipped with the scrollbar drawn but
// completely inert -- it had the layout half and none of the input
// half, which is exactly the failure a copy-paste pattern invites. A
// scrollbar that draws and does not scroll is the bug this file exists
// to make unwritable.
//
// The model is a real OS's: the CONTROL owns scrolling, and the app
// configures it (whether the bar shows, what colour it is, how far a
// notch goes) rather than reimplementing it. An app forwards its WM
// callbacks in and is done.
//
// Layering matches the rest of apps/ui/: `text_scrollback` stays the
// data structure, `ui_scrollbar` stays the stateless drawing/hit
// primitive, and this composes them -- the same way ui_button_group
// composes ui_button rather than replacing it.
//
// ---------------------------------------------------------------------
// Using it
// ---------------------------------------------------------------------
//
//   ui_textview_init(&tv, x, y, w, h, bg, track, thumb, sel);
//   tv.policy = UI_SCROLLBAR_AUTO;        // optional, this is the default
//
//   on_draw:       ui_textview_set_geometry(&tv, ...); ui_textview_draw(&tv, ox, oy);
//   on_wheel:      ui_textview_wheel(&tv, delta);
//   on_click:      ui_textview_click(&tv, cx, cy);
//   on_drag_start: if (ui_textview_drag_start(&tv, cx, cy)) return 1;
//   on_drag:       ui_textview_drag(&tv, cx, cy);
//
// Each input function returns 1 if it consumed the event, so an app
// with other widgets can fall through to them.

enum ui_scrollbar_policy {
    // Show the bar only when the content actually overflows AND the
    // view is wide enough to spare the strip. The second half is not
    // decoration: Terminal has always hidden its bar below a minimum
    // width, because a scrollbar eating a third of a narrow window is
    // worse than no scrollbar.
    UI_SCROLLBAR_AUTO = 0,
    UI_SCROLLBAR_ALWAYS, // reserve the strip even when it can't scroll
    UI_SCROLLBAR_NEVER,  // no bar; the wheel still scrolls
};

// Who owns a press that lands in the text BODY (not on the scrollbar).
enum ui_textview_body {
    // The app's. Notepad places its cursor and starts a selection
    // there, and the view must not swallow that -- drag_start() returns
    // 0 for body presses so the app's own handler runs. The default,
    // because it's what every existing caller needs.
    UI_TEXTVIEW_BODY_APP = 0,
    // The view's: dragging the body pans it, touch-style. For a view
    // with no selection or cursor of its own.
    UI_TEXTVIEW_BODY_PAN,
};

struct ui_textview {
    // Content-relative geometry of the WHOLE control, text strip plus
    // scrollbar -- same convention as ui_button/ui_textbox.
    int x, y, w, h;

    struct text_scrollback tb; // the text itself; reach in for content APIs

    uint8_t policy;    // enum ui_scrollbar_policy
    uint8_t body;      // enum ui_textview_body
    int bar_w;         // scrollbar strip width, px
    int min_text_w;    // AUTO hides the bar below this much text width
    int wheel_lines;   // lines per wheel notch
    int page_overlap;  // rows of overlap kept when paging (1 = classic)
    // Draw text_scrollback's caret? An editor wants it (Notepad); a
    // read-only log or a terminal's own scrollback does not. Config
    // rather than a separate draw call, so the app still makes exactly
    // one call and the control decides what that means.
    int show_caret;

    // Caller-supplied, so this file stays theme-agnostic -- same rule
    // ui_primitives.c states for all of apps/ui/.
    uint32_t bg, track_bg, thumb_bg, sel_bg;

    // Live thumb-drag state. -1 when no thumb drag is in progress;
    // otherwise the offset within the thumb that was grabbed, so the
    // thumb tracks the cursor instead of snapping its top to it.
    int thumb_grab;
    // Live body-pan state (UI_TEXTVIEW_BODY_PAN only): the cursor y the
    // last drag tick saw, and the leftover sub-line remainder so slow
    // drags still accumulate into whole lines instead of rounding to
    // zero every tick and never moving.
    int pan_last_y;
    int pan_remainder;
    int panning;
};

// Geometry + colours + the defaults every current caller wants:
// AUTO policy, body presses to the app, 3 lines per notch.
void ui_textview_init(struct ui_textview *tv, int x, int y, int w, int h,
                       uint32_t bg, uint32_t track_bg, uint32_t thumb_bg,
                       uint32_t sel_bg);

// Updates x/y/w/h only -- everything else, including scroll position and
// an in-progress drag, is left alone. What a per-frame relayout calls.
void ui_textview_set_geometry(struct ui_textview *tv, int x, int y, int w, int h);

// 1 if the scrollbar is showing right now, given the policy and the
// current content. Apps rarely need this; ui_textview_text_w() is
// usually the question they actually have.
int ui_textview_scrollbar_visible(const struct ui_textview *tv);

// Width available to TEXT -- the control's width minus the scrollbar
// strip when one is showing. This is the number an app needs for
// anything that measures against the text area (wrapping, hit-testing a
// character position), and computing it any other way is how a view
// ends up scrolling by the wrong page size.
int ui_textview_text_w(const struct ui_textview *tv);

// Draws background, text and (if visible) the scrollbar.
// `origin_x`/`origin_y` are the window's content origin, same as
// ui_button_draw().
void ui_textview_draw(struct ui_textview *tv, int origin_x, int origin_y);

// --- input; each returns 1 if it consumed the event ------------------

int ui_textview_wheel(struct ui_textview *tv, int delta);

// A click on the scrollbar TRACK pages toward it. A click on the thumb
// or in the body is not consumed here -- the thumb belongs to
// drag_start() below, and the body to the app.
int ui_textview_click(struct ui_textview *tv, int cx, int cy);

// Claims the drag if the press is on the thumb (always) or in the body
// (only when policy is UI_TEXTVIEW_BODY_PAN). Returns 0 otherwise, so
// an app's own on_drag_start logic runs for body presses.
int ui_textview_drag_start(struct ui_textview *tv, int cx, int cy);

// Only meaningful while drag_start() returned 1.
void ui_textview_drag(struct ui_textview *tv, int cx, int cy);

// Clears drag state. Safe to call unconditionally from on_release.
void ui_textview_drag_end(struct ui_textview *tv);

// 1 if (cx, cy) is inside the control at all.
int ui_textview_hit(const struct ui_textview *tv, int cx, int cy);

#endif
