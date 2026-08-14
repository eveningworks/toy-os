#ifndef UI_LISTBOX_H
#define UI_LISTBOX_H

#include <stdint.h>
#include "ui_primitives.h"
#include "ui_scrollbar.h"

// A scrollable single-select list of text items -- rows, selection,
// hover, keyboard navigation, and a scrollbar that appears only when the
// items overflow, as ONE control.
//
// **Why a widget and not a per-app loop.** ui_radio_list already covers
// "pick one of N" for a handful of fixed options, and deliberately stops
// there: it has no scrolling, no hover, no keyboard, and no notion of a
// viewport, because the Control Panel's four timezone choices needed
// none of that. The moment a list is longer than its box, all four
// arrive at once and they are exactly the things that get half-built --
// docs/gui-guidelines.md's standing example is a scrollbar that drew
// perfectly and did nothing at all, shipped by the third app to
// copy-paste a scrolling loop. So the scrolling, the hit-testing and the
// selection all live here, and an app configures and forwards events.
//
// The behaviour model is Windows' listbox, because it is the one people
// already have in their fingers:
//
//   - Clicking a row selects it. Selection commits on RELEASE over the
//     armed row, so a press dragged off and released changes nothing --
//     the press-then-commit rule every control in this GUI follows
//     (docs/gui-guidelines.md).
//   - The row under the cursor highlights independently of the selected
//     row, and follows the mouse.
//   - The wheel scrolls the VIEW and leaves the selection alone, so the
//     selected row can scroll out of sight. That is deliberate and is
//     what Windows does; making the wheel move the selection instead
//     would mean a user cannot look ahead in a list without changing
//     the value they already chose.
//   - Arrows/Home/End/PageUp/PageDown move the selection and scroll
//     just far enough to keep it visible.
//
// **Scroll offset here is 0 = TOP**, the natural direction for a list.
// Note that ui_scrollbar.h's convention is the opposite (0 = pinned to
// the bottom/newest), because it grew up alongside a terminal
// scrollback where that is the useful anchor. This file converts at the
// boundary rather than changing ui_scrollbar, which has three existing
// callers that all depend on its convention; see listbox_bar_offset()
// in the .c file for the single place that conversion happens.
//
// **Added ahead of a second caller, by explicit user request** -- the
// same deliberate exception ui_checkbox and ui_radio_list were (see
// docs/decisions.md). Its callers today are UI Demo and ui_dropdown,
// which composes it for its popup; this project's standing rule is
// otherwise a second REAL caller before a widget is shared.
//
// ---------------------------------------------------------------------
// Using it
// ---------------------------------------------------------------------
//
//   ui_listbox_init(&lb, x, y, w, h, items, count, bg, fg, sel, track, thumb);
//
//   on_draw:    ui_listbox_set_geometry(&lb, ...); ui_listbox_draw(&lb, ox, oy);
//   on_hover:   if (ui_listbox_hover(&lb, cx, cy)) redraw = 1;
//   on_press:   ui_listbox_press(&lb, cx, cy);
//   on_release: if (ui_listbox_release(&lb) >= 0) acted();
//   on_wheel:   ui_listbox_wheel(&lb, delta);
//   on_key:     ui_listbox_key(&lb, key);
//
// Each input function returns non-zero when it consumed the event, so an
// app with other widgets can fall through to them.

// Rows are caller-owned `const char *`s, like ui_radio_list's options
// and ui_button's label -- the strings must outlive the listbox. Nothing
// here copies them, so a list backed by a filesystem listing wants its
// own storage, exactly as the file picker already keeps its own.
struct ui_listbox {
    // Content-relative geometry of the WHOLE control, rows plus
    // scrollbar -- same convention as ui_button/ui_textbox/ui_textview.
    int x, y, w, h;

    const char *const *items; // caller-owned array of `count` labels
    int count;

    int selected;  // index of the selected row, or -1 for none
    int hovered;   // index of the row under the cursor, or -1. OWNED -- read-only from app code
    int top;       // first visible row (0 = top of the list). OWNED

    uint8_t policy;  // enum ui_scrollbar_policy, from ui_textview.h's vocabulary
    int bar_w;       // scrollbar strip width, px
    int row_h;       // full height of one row; 0 = derive from the font
    int pad_x;       // left inset for a row's label, px
    int wheel_rows;  // rows per wheel notch
    int disabled;    // 1 = drawn dimmed and ignores every input

    // Caller-supplied, so this file stays theme-agnostic -- the standing
    // rule for everything under apps/ui/ (see ui_primitives.c).
    uint32_t bg, fg, sel_bg, sel_fg, track_bg, thumb_bg;

    // Live press state, all OWNED -- read-only from app code.
    //
    // `armed` is the row that would be selected if the button were
    // released right now, or -1 for none. It is RECOMPUTED every tick
    // while held, so dragging down the list moves it and dragging off
    // the control clears it -- which is both what Windows' listbox does
    // and what ui_button_group already does with its `pressed` flag.
    // Nothing commits until release (docs/gui-guidelines.md), so a press
    // dragged off and released still changes nothing.
    int armed;
    // 1 while a press is in progress at all, so the first tick can
    // classify it once (row vs. scrollbar) and later ticks don't
    // re-classify -- otherwise dragging a row press sideways onto the
    // scrollbar would start paging mid-gesture.
    int pressing;
    // 1 when the in-progress press began on the scrollbar. Such a press
    // selects nothing on release, and is held separately from `armed`
    // precisely because `armed` is recomputed each tick.
    int bar_press;
    // Live thumb-drag state, same meaning as ui_textview's: -1 when no
    // drag is in progress, otherwise the grab offset within the thumb.
    int thumb_grab;
};

// Geometry, items and colours, plus the defaults every current caller
// wants: AUTO scrollbar policy, font-derived row height, 3 rows per
// wheel notch, nothing selected, nothing hovered, scrolled to the top.
void ui_listbox_init(struct ui_listbox *lb, int x, int y, int w, int h,
                      const char *const *items, int count,
                      uint32_t bg, uint32_t fg, uint32_t sel_bg, uint32_t sel_fg,
                      uint32_t track_bg, uint32_t thumb_bg);

// Updates x/y/w/h only -- selection, scroll position and any in-progress
// drag are left alone. What a per-frame relayout calls, same contract as
// ui_textview_set_geometry().
void ui_listbox_set_geometry(struct ui_listbox *lb, int x, int y, int w, int h);

// Replaces the item array. Clamps `selected` and `top` to the new count
// (selection becomes -1 if the list is now empty) rather than leaving an
// index pointing past the end -- a stale index is how a list that was
// just refiltered reads one item beyond its array.
void ui_listbox_set_items(struct ui_listbox *lb, const char *const *items, int count);

// Sets the selection and scrolls it into view. Out-of-range clamps to
// -1 (nothing selected) rather than being silently corrected to a real
// row, matching ui_radio_list's "mark nothing rather than mark the
// wrong thing" rule.
void ui_listbox_set_selected(struct ui_listbox *lb, int index);

// --- geometry questions an app may legitimately have -----------------

// 1 if the scrollbar is showing right now, given the policy and item
// count.
int ui_listbox_scrollbar_visible(const struct ui_listbox *lb);

// Height of one row in pixels -- `row_h` if the caller set one,
// otherwise derived from the current font. Use this rather than
// gfx_char_h() arithmetic when sizing a box around a listbox, or the two
// disagree the moment `row_h` is set.
int ui_listbox_row_h(const struct ui_listbox *lb);

// How many rows are fully visible in the current height. What
// PageUp/PageDown moves by, and what a caller sizing a window to "show
// N items" is really asking about.
int ui_listbox_visible_rows(const struct ui_listbox *lb);

// Total height needed to show `rows` items with no scrolling -- for a
// caller laying out around the control instead of fitting into it.
int ui_listbox_height_for_rows(const struct ui_listbox *lb, int rows);

// 1 if (cx, cy) is inside the control at all (rows or scrollbar).
int ui_listbox_hit(const struct ui_listbox *lb, int cx, int cy);

// Index of the row at (cx, cy), or -1 if the point isn't on one --
// including when it is on the scrollbar strip rather than a row.
int ui_listbox_row_at(const struct ui_listbox *lb, int cx, int cy);

void ui_listbox_draw(struct ui_listbox *lb, int origin_x, int origin_y);

// --- input; each returns non-zero when it consumed the event ---------

// Tracks the hovered row. Returns 1 only when the hovered row CHANGED,
// which is the on_hover contract exactly (returning 1 every tick
// repaints the window continuously -- see docs/gui-guidelines.md).
// Pass (-1, -1) when the cursor leaves the window, same as the WM does.
int ui_listbox_hover(struct ui_listbox *lb, int cx, int cy);

// Arms a row, or starts a scrollbar interaction (a track click pages
// immediately; a thumb press begins a drag). Called every tick while
// held, with live coordinates, so it also tracks whether the cursor is
// still over the armed row -- the same shape gui_apps.h's on_press has.
int ui_listbox_press(struct ui_listbox *lb, int cx, int cy);

// Commits: returns the newly selected row index if a press committed
// over the row it armed, or -1 if there was nothing armed or the press
// had been dragged off it. Safe to call unconditionally from
// on_release. This return value IS the "did the user pick something"
// answer -- the same shape ui_button_group_release() has, and for the
// same reason (both Calculator and Notepad were uncancellable until it
// existed; see docs/decisions.md).
int ui_listbox_release(struct ui_listbox *lb);

// Scrolls the VIEW only -- the selection does not move. Positive delta
// scrolls toward the top of the list, matching mouse.h's wheel sign
// convention as used everywhere else in this GUI.
int ui_listbox_wheel(struct ui_listbox *lb, int delta);

// Arrows/Home/End/PageUp/PageDown move the selection and scroll it into
// view. Returns 1 if the key was consumed. Everything else -- including
// Enter and Esc -- falls through unhandled, because what those mean is
// the caller's decision (ui_dropdown treats them as commit and cancel;
// a plain listbox in a dialog may want them for the dialog's buttons).
int ui_listbox_key(struct ui_listbox *lb, int key);

#endif
