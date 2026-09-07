#ifndef UUI_TEXTVIEW_H
#define UUI_TEXTVIEW_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/utext.h"
#include "ui/uui_scrollbar.h"
#include "ui/uui_primitives.h"

// uui_textview -- the ring-3 port of apps/ui/ui_textview.c: a utext, its
// scrollbar, the geometry that splits the two, and all of the input
// handling that makes scrolling work, as ONE control.
//
// The reason the kernel-side one exists carries over unchanged, and is
// the reason this is a port rather than three apps each growing their
// own copy again: scrolling used to be the app's job, three apps each
// carried the same ~20 lines, and the newest copy shipped a scrollbar
// that drew and did nothing. The CONTROL owns scrolling; the app
// configures it (colours, policy, how far a notch goes) and forwards
// events.
//
// TWO DIFFERENCES FROM THE KERNEL VERSION, both forced rather than
// chosen:
//
//   1. It draws into a `struct ugfx_surface *`, so every entry point
//      that paints takes one. A client has no framebuffer.
//   2. The buffer underneath is `utext`, not `text_scrollback` -- the
//      same design with no per-character colour (see utext.h). Nothing
//      in this control ever looked at a cell's colour.
//
// Using it, exactly as kernel-side:
//
//   uui_textview_init(&tv, x, y, w, h, fg, bg, track, thumb, sel);
//   on_draw:    uui_textview_set_geometry(&tv, ...); uui_textview_draw(s, &tv);
//   on_wheel:   uui_textview_wheel(&tv, notches);
//   on_press:   if (uui_textview_drag_start(&tv, x, y)) ...
//               else uui_textview_click(&tv, x, y);
//   on_motion:  uui_textview_drag(&tv, x, y);
//   on_release: uui_textview_drag_end(&tv);
//
// Each input function returns 1 if it consumed the event.

// Scrollbar policy. Same three values as the kernel's
// enum ui_scrollbar_policy; declared here because ring 3's scrollbar is
// stateless and carries no policy of its own.
enum uui_textview_policy {
    UUI_TEXTVIEW_AUTO = 0,  // bar appears only when the content overflows
    UUI_TEXTVIEW_ALWAYS,
    UUI_TEXTVIEW_NEVER,
};

// Who owns a press that lands in the text BODY (not on the scrollbar).
enum uui_textview_body {
    // The app's -- an editor places its cursor and starts a selection
    // there, and the view must not swallow that. The default.
    UUI_TEXTVIEW_BODY_APP = 0,
    // The view's: dragging the body pans it, touch-style. For a view
    // with no cursor or selection of its own.
    UUI_TEXTVIEW_BODY_PAN,
};

struct uui_textview {
    // Content-relative geometry of the WHOLE control, text strip plus
    // scrollbar -- same convention as every other uui_ widget.
    int x, y, w, h;

    struct utext tb;   // the text itself; reach in for content APIs

    uint8_t policy;    // enum uui_textview_policy
    uint8_t body;      // enum uui_textview_body
    int bar_w;
    int min_text_w;    // AUTO hides the bar below this much text width
    int wheel_lines;
    int page_overlap;  // rows kept when paging (1 = classic)
    int show_caret;
    unsigned bar_flags; // UUI_SCROLLBAR_* passed through to the bar

    uint32_t fg, bg, track_bg, thumb_bg, sel_bg;

    // Live drag state. -1 when no thumb drag is in progress; otherwise
    // the offset WITHIN the thumb that was grabbed, so the thumb tracks
    // the cursor rather than snapping its top to it (the ring-3
    // Notepad shipped that bug once by passing 0 here).
    int thumb_grab;
    int pan_last_y;
    int pan_remainder;
    int panning;
};

// `buf`/`cap` are the text's storage, and they are the CALLER's for the
// same reason utext.h gives: a view of a help page and a view of a
// 1.6 MB file want very different numbers, and neither is this
// widget's to choose. Storage last, as uui_fileview's entry array is.
void uui_textview_init(struct uui_textview *tv, int x, int y, int w, int h,
                        uint32_t fg, uint32_t bg, uint32_t track_bg,
                        uint32_t thumb_bg, uint32_t sel_bg,
                        char *buf, int cap);

void uui_textview_set_geometry(struct uui_textview *tv, int x, int y, int w, int h);

// **Both 0 -- no preference in either direction** (see
// uui_primitives.h). A text view wraps to whatever width it gets and
// scrolls at whatever height, so any number here would be invented.
void uui_textview_natural_size(const struct uui_textview *tv, int *out_w, int *out_h);

int uui_textview_scrollbar_visible(const struct uui_textview *tv);

// Width available to TEXT -- the control's width minus the bar when one
// is showing. The number an app needs for anything measured against the
// text area; computing it any other way is how a view ends up scrolling
// by the wrong page size.
int uui_textview_text_w(const struct uui_textview *tv);

void uui_textview_draw(struct ugfx_surface *s, struct uui_textview *tv);

// --- input; each returns 1 if it consumed the event ------------------

int uui_textview_wheel(struct uui_textview *tv, int delta);
int uui_textview_click(struct uui_textview *tv, int cx, int cy);
int uui_textview_drag_start(struct uui_textview *tv, int cx, int cy);
void uui_textview_drag(struct uui_textview *tv, int cx, int cy);
void uui_textview_drag_end(struct uui_textview *tv);
int uui_textview_hit(const struct uui_textview *tv, int cx, int cy);

// Full table with ROUTED POINTER INPUT (ui/uui_route.h). An app that
// declares a text view with this writes no scrolling code at all --
// wheel, track paging, thumb dragging and (in PAN mode) body panning
// are the widget's.
struct uui_widget_ops;
extern const struct uui_widget_ops uui_textview_ops;

#endif
