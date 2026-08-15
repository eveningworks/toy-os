#ifndef UWIDGETS_H
#define UWIDGETS_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui.h"

// uwidgets -- the rest of apps/ui/, ported to ring 3.
//
// `uui.h` carries the primitives Calculator needed (states, buttons,
// button groups); this file carries everything else a client might want:
// a scrollbar, a single-line text field, a checkbox, a radio list, a
// listbox, a dropdown, and the keyboard-focus ring that ties them
// together.
//
// Same two rules the kernel originals follow, both of which survive the
// port intact because they are the interesting part:
//
//   * ONE geometry calculation per widget, shared by draw / hit-test /
//     drag. The scrollbar is the clearest case -- thumb position is
//     computed in a single helper so draw(), hit() and the drag maths
//     cannot disagree about where the thumb is. Every widget here that
//     has an interactive sub-part does the same.
//
//   * A press ARMS and a release COMMITS, and a press dragged off its
//     target commits nothing. See uui.h.
//
// The one structural difference from `apps/ui/`: every draw takes a
// `struct ugfx_surface *` and content-relative coordinates, because a
// client draws into its own buffer and has no screen origin to add.

// --- scrollbar --------------------------------------------------------

#define UUI_SCROLLBAR_MIN_THUMB_H 16

enum uui_scrollbar_zone {
    UUI_SB_NONE = 0,
    UUI_SB_ABOVE,  // the track above the thumb -- page up
    UUI_SB_THUMB,  // the thumb itself -- start a drag
    UUI_SB_BELOW,  // the track below the thumb -- page down
};

void uui_scrollbar_draw(struct ugfx_surface *s, int x, int y, int w, int h,
                         int total_lines, int visible_rows, int scroll_offset,
                         uint32_t track_bg, uint32_t thumb_bg);

enum uui_scrollbar_zone uui_scrollbar_hit(int x, int y, int w, int h,
                                           int total_lines, int visible_rows,
                                           int scroll_offset, int px, int py);

void uui_scrollbar_thumb_rect(int y, int h, int total_lines, int visible_rows,
                               int scroll_offset, int *out_thumb_y, int *out_thumb_h);

// The scroll offset a thumb drag to `py` implies. `grab_offset_in_thumb`
// is how far down the thumb the drag started, so the thumb doesn't jump
// under the cursor on the first pixel of movement.
int uui_scrollbar_offset_for_drag(int y, int h, int total_lines, int visible_rows,
                                   int py, int grab_offset_in_thumb);

// --- single-line text field -------------------------------------------

#define UUI_FIELD_MAX 48

struct uui_field {
    char buf[UUI_FIELD_MAX]; // NUL-terminated
    int len;
    int cursor; // [0, len]
    int active; // 1 = focused: draws a caret and accepts keys
};

void uui_field_init(struct uui_field *f, const char *initial);
void uui_field_set_active(struct uui_field *f, int active);

// Returns 1 if the key was consumed. Deliberately does NOT consume
// Enter: "commit this field" is the caller's decision, not the widget's.
int uui_field_key(struct uui_field *f, int key);

// Draws the field, scrolling its content horizontally so the caret
// stays visible -- typing past the right edge behaves like a real text
// input rather than drawing through the border.
void uui_field_draw(struct ugfx_surface *s, int x, int y, int w, int h,
                     const struct uui_field *f,
                     uint32_t bg, uint32_t fg, uint32_t border);

// --- checkbox ---------------------------------------------------------

// Width of the whole clickable area: box + gap + label.
int uui_checkbox_width(int size, const char *label);

void uui_checkbox_draw(struct ugfx_surface *s, int x, int y, int size,
                        int checked, int hovered, const char *label,
                        uint32_t bg, uint32_t fg);

// Hit-tests box AND label -- a highlight larger than its target is a
// lie about where to click, and so is the reverse.
int uui_checkbox_hit(int x, int y, int size, const char *label, int px, int py);

// --- radio list -------------------------------------------------------

struct uui_radio_list {
    const char *const *options; // caller-owned
    int count;
    int cols;        // 1 = a plain vertical list
    int row_h;       // full row height including its gap
    int col_w;
    int marker_size;
};

void uui_radio_list_size(const struct uui_radio_list *l, int *out_w, int *out_h);
void uui_radio_list_draw(struct ugfx_surface *s, const struct uui_radio_list *l,
                          int x, int y, int selected, int hovered,
                          uint32_t bg, uint32_t fg);
// Index under (px, py), or -1.
int uui_radio_list_hit(const struct uui_radio_list *l, int x, int y, int px, int py);

// --- listbox ----------------------------------------------------------

struct uui_listbox {
    int x, y, w, h;
    const char *const *items; // caller-owned
    int count;
    int selected;  // or -1
    int hovered;   // OWNED -- driven by uui_listbox_hover()
    int top;       // first visible row; OWNED
    int row_h;     // 0 = derive from the font
    int bar_w;
    uint32_t bg, fg, sel_bg, sel_fg, track_bg, thumb_bg;
};

void uui_listbox_init(struct uui_listbox *lb, int x, int y, int w, int h,
                       const char *const *items, int count);
void uui_listbox_set_items(struct uui_listbox *lb, const char *const *items, int count);
int  uui_listbox_row_h(const struct uui_listbox *lb);
int  uui_listbox_visible_rows(const struct uui_listbox *lb);
int  uui_listbox_scrollbar_visible(const struct uui_listbox *lb);
void uui_listbox_draw(struct ugfx_surface *s, const struct uui_listbox *lb);

// Row index at (cx, cy), or -1 if outside / on the scrollbar.
int  uui_listbox_hit(const struct uui_listbox *lb, int cx, int cy);
int  uui_listbox_hover(struct uui_listbox *lb, int cx, int cy);   // 1 if changed
int  uui_listbox_click(struct uui_listbox *lb, int cx, int cy);   // selects; 1 if changed
int  uui_listbox_wheel(struct uui_listbox *lb, int notches);      // 1 if scrolled
int  uui_listbox_key(struct uui_listbox *lb, int key);            // arrows/home/end

// --- dropdown ---------------------------------------------------------

struct uui_dropdown {
    int x, y, w, h;
    struct uui_listbox list; // the popup, which IS a listbox
    int open;
    int max_rows;
    uint32_t bg, fg, border;
};

void uui_dropdown_init(struct uui_dropdown *d, int x, int y, int w, int h,
                        const char *const *items, int count);
void uui_dropdown_draw(struct ugfx_surface *s, const struct uui_dropdown *d);

// **Call this AFTER every other widget has drawn.** Drawing is
// immediate-mode, so z-order is call order -- a popup drawn in place
// would be painted over by whatever comes next. Same rule the kernel
// dropdown documents, and the same bug if ignored.
void uui_dropdown_draw_popup(struct ugfx_surface *s, const struct uui_dropdown *d);

int uui_dropdown_hit(const struct uui_dropdown *d, int cx, int cy);
int uui_dropdown_click(struct uui_dropdown *d, int cx, int cy); // 1 if it consumed the click
int uui_dropdown_key(struct uui_dropdown *d, int key);
int uui_dropdown_selected(const struct uui_dropdown *d);

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

// --- keyboard focus ring ----------------------------------------------
//
// ROUTE KEYS THROUGH uui_focus_key(), never by trying each widget in
// turn: the first widget tried swallows every key it recognises, which
// is how a listbox next to a dropdown became unreachable from the
// keyboard in the kernel version.

struct uui_focus_ops {
    int  (*key)(void *w, int key);
    int  (*hit)(const void *w, int cx, int cy);
    void (*set_focused)(void *w, int focused);
};

struct uui_focusable {
    void *widget;
    const struct uui_focus_ops *ops;
};

struct uui_focus {
    struct uui_focusable *items; // caller-owned; tab order = array order
    int count;
    int current; // or -1
};

void uui_focus_init(struct uui_focus *f, struct uui_focusable *items, int count);
void uui_focus_set(struct uui_focus *f, int index);
int  uui_focus_next(struct uui_focus *f);
int  uui_focus_prev(struct uui_focus *f);

// Handles Tab/Shift-Tab itself, then forwards to the focused widget.
// Returns 1 if consumed.
int  uui_focus_key(struct uui_focus *f, int key, unsigned mods);

// Moves focus to whatever was clicked. Returns 1 if focus changed.
int  uui_focus_click(struct uui_focus *f, int cx, int cy);

#endif
