#ifndef WIDGETS_H
#define WIDGETS_H

#include <stdint.h>
#include "vga.h"

// Tiny shared "clickable rectangle" primitives, pulled out after three
// independent reimplementations of the same idea turned up: wm.c's
// title-bar buttons, Calculator's button grid, and Notepad's toolbar.
// One shared implementation now backs all three -- see
// apps/wm/wm_render.c, apps/calculator.c, and apps/notepad.c.
//
// Deliberately minimal: a filled rect with an optional centered text
// label, plus a hit test. That's everything all three current callers
// actually needed; it is NOT a general widget-toolkit start (no focus
// management, no layout engine, no scrollbars/text fields yet) -- add
// the next primitive here only once a second real caller needs it, the
// same "don't build it until something needs it" approach the rest of
// this codebase uses.
//
// Icon-only buttons (wm.c's minimize/maximize/close, which draw
// hand-drawn vector icons, not text): pass label=NULL to widget_button()
// to get just the background fill, then draw the icon yourself with your
// own gfx_* calls afterward -- see wm_render.c's draw_window_chrome().

// 1 if (px, py) falls inside the x/y/w/h rect, 0 otherwise. Whatever
// coordinate space the caller's other numbers are already in (screen
// pixels for wm.c, window-content-relative for Calculator/Notepad) --
// this doesn't care, it's just a bounds check.
int widget_hit(int x, int y, int w, int h, int px, int py);

// Fills the rect with `bg`, then -- if `label` is non-NULL -- centers it
// in `fg` on `bg` using the current font (gfx_char_w()/gfx_char_h(), so
// it stays correct across gfx_set_font_size() calls same as everything
// else that draws text). Pass label=NULL for an icon-only button.
void widget_button(int x, int y, int w, int h, const char *label, uint32_t bg, uint32_t fg);

// ---- scrollback text widget ----
//
// A reusable "console inside a window" primitive: a bounded stream of
// characters (each remembering the vga_color it was written in), with
// wrap-and-scroll rendering into any content rect. First (and, at the
// time this was added, only) caller is apps/terminal.c, which pairs it
// with a `struct vga_sink` (vga.h) so shell_dispatch() (shell.h) can
// write straight into one of these instead of the physical console --
// but nothing here actually depends on vga_sink or the shell; this is
// just a text buffer + renderer, usable anywhere a scrolling text area
// is useful. Two APIs sit on top of the same storage: the original
// append-only putc()/backspace() pair (apps/terminal.c's only need --
// it never sees a cursor anywhere but the end), and a cursor-aware
// insert/delete/move API added later for real in-place editing
// (apps/notepad.c, apps/editor.c -- see the `cursor` field's own
// comment below). The two coexist because putc()/backspace()/clear()
// keep `cursor` pinned to the append point, so a caller that never
// touches the cursor API sees the exact same append-only behavior
// as before it existed.
//
// Storage is a fixed-size ring buffer (SCROLLBACK_CAP cells) -- once
// full, the oldest character is silently dropped for every new one
// appended, exactly like the physical console's own scrolloff behavior
// (see vga.c's legacy_scroll_if_needed()/fb_scroll_if_needed()). No
// heap, so this is meant to be embedded directly in a caller's static
// state struct (`struct text_scrollback tb;`), not allocated.
//
// Line wrapping is recomputed from the raw character stream on every
// draw (two passes over the buffer: one to count wrapped lines given
// the current content width, one to render the visible window) rather
// than cached, so it stays correct across window resizes and font-size
// changes with no invalidation bookkeeping -- the same tradeoff
// apps/notepad.c's notepad_draw() already makes, just with scrolling
// added on top. SCROLLBACK_CAP is small enough (see its comment) that
// two full passes per draw is cheap.

#define SCROLLBACK_CAP 8192

struct scrollback_cell {
    char ch;
    uint8_t fg; // an enum vga_color, stored narrow since cells add up
};

struct text_scrollback {
    struct scrollback_cell buf[SCROLLBACK_CAP];
    int start; // ring buffer head -- logical index i lives at buf[(start+i) % SCROLLBACK_CAP]
    int count; // valid cells, 0..SCROLLBACK_CAP
    enum vga_color cur_fg; // color new characters are appended with
    // 0 = pinned to the newest output (auto-follows new writes, like a
    // normal terminal); >0 = scrolled up that many wrapped lines from
    // the bottom, and NEW output no longer yanks the view back down
    // until the caller re-pins it (see widget_scrollback_scroll()).
    // Clamped to the valid range every widget_scrollback_draw() call,
    // since "valid range" depends on the current content width.
    int scroll_offset;
    // Logical index into the buffer, [0, count] -- "the cursor sits
    // just before buf[cursor]", same convention putc()'s append point
    // already used implicitly. Kept in sync at `count` (the append
    // point) automatically by every append-only call below (putc,
    // backspace, clear, init) -- so apps/terminal.c, the only caller
    // that never touches the cursor API, sees no behavior change at
    // all, exactly as before this field existed. A real editor
    // (apps/notepad.c, apps/editor.c) moves this explicitly via the
    // cursor_*() calls and edits at it via the *_at_cursor() calls,
    // both added alongside this field -- see their own comments.
    int cursor;
};

// Resets to empty, cursor color VGA_LIGHT_GREY, pinned to bottom.
void widget_scrollback_init(struct text_scrollback *tb);

// Appends one character in `tb->cur_fg`. '\n' is a line break (like
// vga_putc()); there is no special-casing of '\r' or other control
// codes here -- callers that care (e.g. a sink's putc callback) should
// filter before calling this, same as vga_putc() already does for '\b'
// (see vga.h's struct vga_sink comment) by routing it to `backspace`
// instead of `putc` at the call-site level.
void widget_scrollback_putc(struct text_scrollback *tb, char c);

// Removes the single most recently appended character, if any.
void widget_scrollback_backspace(struct text_scrollback *tb);

// Empties the buffer and re-pins to the bottom.
void widget_scrollback_clear(struct text_scrollback *tb);

// Sets the color newly appended characters will be stored with.
// Doesn't affect anything already in the buffer.
void widget_scrollback_set_color(struct text_scrollback *tb, enum vga_color fg);

// Scrolls the view by `delta` wrapped lines -- positive moves toward
// older output, negative toward newer. Clamped to the valid range on
// the next draw (there's no need to know the current content
// width/height just to request a scroll). Scrolling back to exactly 0
// re-pins to the bottom (new output will auto-follow again); any
// positive offset un-pins it.
void widget_scrollback_scroll(struct text_scrollback *tb, int delta_lines);

// Computes the total wrapped-line count and visible-row count for
// `cw`/`ch` without drawing anything -- the same pass-1 walk
// widget_scrollback_draw() does internally, exposed separately so a
// caller that needs to know the scroll range (a visual scrollbar
// widget, for instance) doesn't also have to render the text just to
// find it out. Also clamps `tb->scroll_offset` to
// [0, total_lines - visible_rows], same as draw() does -- so after
// calling this, `tb->scroll_offset` (a public field) is the third
// number such a caller needs, already normalized.
void widget_scrollback_metrics(struct text_scrollback *tb, int cw, int ch,
                                int *out_total_lines, int *out_visible_rows);

// Fills (cx, cy, cw, ch) with `bg`, then draws whatever's currently in
// view given `tb->scroll_offset` and the current font size
// (gfx_char_w()/gfx_char_h()), wrapping at `cw`'s column count. If
// `show_cursor` is non-zero and `tb->cursor`'s current position is
// within the visible window, draws a solid cursor block there in
// `tb->cur_fg` -- there's nothing to draw when it's scrolled out of
// view. For an append-only caller (apps/terminal.c) `tb->cursor`
// always trails the write position, so this is exactly the old
// "end-of-text cursor" behavior; a cursor-aware caller sees it drawn
// wherever tb->cursor actually is.
void widget_scrollback_draw(struct text_scrollback *tb, int cx, int cy, int cw, int ch,
                             uint32_t bg, int show_cursor);

// ---- cursor-aware editing (see the `cursor` field's own comment) ----
//
// Everything below operates on `tb->cursor`, moving it and/or
// inserting/deleting at its current position -- unlike putc()/
// backspace() above, none of these touch the append point directly.
// '\n' is an ordinary buffer character here, same as putc() treats it:
// there's no separate concept of "lines" in storage, cursor_up()/
// cursor_down()/cursor_home()/cursor_end() just scan for the nearest
// '\n' boundaries on the fly (see widgets.c) -- cheap enough for
// SCROLLBACK_CAP's size, same "recompute from source, don't cache"
// tradeoff as the rest of this widget.

// Moves the cursor one character left/right, clamped to [0, count].
void widget_scrollback_cursor_left(struct text_scrollback *tb);
void widget_scrollback_cursor_right(struct text_scrollback *tb);

// Moves the cursor up/down one line, preserving its column within the
// line where possible (clamped to the target line's length if it's
// shorter) -- ordinary text-editor up/down behavior. No-op at the
// first/last line respectively.
void widget_scrollback_cursor_up(struct text_scrollback *tb);
void widget_scrollback_cursor_down(struct text_scrollback *tb);

// Moves the cursor to the start/end of its current line (the nearest
// '\n' boundary, or the buffer's start/end).
void widget_scrollback_cursor_home(struct text_scrollback *tb);
void widget_scrollback_cursor_end(struct text_scrollback *tb);

// Inserts `c` at the cursor, shifting everything after it right by one,
// and advances the cursor past the newly-inserted character (so typing
// a run of characters reads naturally left to right). No-op if the
// buffer is already at SCROLLBACK_CAP -- refuses rather than evicting
// from the head the way putc() does, since shifting the ring's start
// out from under an in-progress edit would desync `cursor` from the
// content it's supposed to point into.
void widget_scrollback_insert_at_cursor(struct text_scrollback *tb, char c);

// Deletes the character the cursor is just before (forward delete, like
// a real editor's Delete key) -- cursor position doesn't change. No-op
// if the cursor is already at the end.
void widget_scrollback_delete_at_cursor(struct text_scrollback *tb);

// Deletes the character just before the cursor (backward delete, like
// Backspace) and moves the cursor back by one. No-op if the cursor is
// already at the start.
void widget_scrollback_backspace_at_cursor(struct text_scrollback *tb);

// ---- vertical scrollbar widget ----
//
// A companion to text_scrollback (or anything else that can report a
// total-lines/visible-rows/scroll_offset triple -- nothing here
// actually depends on text_scrollback's struct): draws a track +
// proportional thumb, classifies a click as landing on the thumb
// (start a drag) or the empty track above/below it (page up/down), and
// converts an in-progress drag's mouse position back into a
// scroll_offset. Geometry (thumb size/position) is recomputed from the
// three inputs on every call rather than cached -- same "recompute from
// source" tradeoff every other widget here makes -- so `x/y/w/h` and
// `total_lines`/`visible_rows`/`scroll_offset` must be the SAME values
// across a draw()/hit()/thumb_rect() call for them to agree with each
// other (get total_lines/visible_rows from widget_scrollback_metrics(),
// same call, right before using any of these).
//
// `scroll_offset` follows text_scrollback's convention: 0 = pinned to
// the bottom/newest (thumb at the bottom of the track), increasing
// toward `total_lines - visible_rows` = fully scrolled to the
// oldest/top (thumb at the top of the track).
#define SCROLLBAR_MIN_THUMB_H 16

enum scrollbar_zone {
    SCROLLBAR_ZONE_NONE,  // (px, py) isn't inside the track rect at all
    SCROLLBAR_ZONE_THUMB, // landed on the thumb -- caller should start a drag
    SCROLLBAR_ZONE_ABOVE, // landed on the empty track above the thumb -- page toward older content
    SCROLLBAR_ZONE_BELOW, // landed on the empty track below the thumb -- page toward newer content
};

// Fills (x, y, w, h) with `track_bg`, then draws the proportionally-sized
// thumb in `thumb_bg`. If `total_lines <= visible_rows` (nothing to
// scroll), only the track is drawn -- there's no thumb to show since
// the whole content already fits.
void widget_scrollbar_draw(int x, int y, int w, int h, int total_lines, int visible_rows,
                            int scroll_offset, uint32_t track_bg, uint32_t thumb_bg);

// Classifies (px, py) against the same geometry widget_scrollbar_draw()
// would compute for these arguments.
enum scrollbar_zone widget_scrollbar_hit(int x, int y, int w, int h, int total_lines,
                                          int visible_rows, int scroll_offset, int px, int py);

// Outputs the thumb's current y position and height (in the same
// coordinate space as `y`/`h`) for this geometry -- used when a click
// lands on SCROLLBAR_ZONE_THUMB, to record where within the thumb the
// grab happened (`py - *out_thumb_y`) so widget_scrollbar_offset_for_drag()
// below can keep that same relative grab point under the cursor for the
// rest of the drag, instead of the thumb jumping to re-center under it.
void widget_scrollbar_thumb_rect(int y, int h, int total_lines, int visible_rows,
                                  int scroll_offset, int *out_thumb_y, int *out_thumb_h);

// Converts an in-progress drag's current mouse `py` back into a
// scroll_offset, given `grab_offset_in_thumb` (the `py - thumb_y` value
// captured via widget_scrollbar_thumb_rect() when the drag started).
// The result is already clamped to [0, total_lines - visible_rows] --
// safe to assign straight to a text_scrollback's `scroll_offset` field.
int widget_scrollbar_offset_for_drag(int y, int h, int total_lines, int visible_rows,
                                      int py, int grab_offset_in_thumb);

// ---- single-line text input widget ----
//
// A minimal editable text box: fixed-size buffer, a cursor position,
// and an explicit active/inactive flag the widget itself never changes
// on its own (see widget_textfield_key()'s own comment for why) -- the
// owning app decides when to activate/deactivate it (typically: a
// click on the field's rect activates it, via the app's own on_click
// and widget_hit(); Enter, or a click elsewhere, deactivates it) and
// reads `buf` whenever it wants the current text, which is always up
// to date -- there's no separate "commit" step, every accepted
// keystroke updates `buf` immediately. First real caller: apps/
// notepad.c's filename field (see CHANGELOG.md's build 490),
// previously a fixed `"notepad.txt"`.
//
// Deliberately minimal, same philosophy as the rest of this file: no
// text selection, no copy/paste, no undo, no revert-on-Escape (Escape
// isn't handled specially at all -- see widget_textfield_key()), no
// horizontal scroll-within-the-field (see widget_textfield_draw()).
// Add the next capability only once a real caller needs it.

#define TEXTFIELD_MAX 48

struct text_field {
    char buf[TEXTFIELD_MAX]; // NUL-terminated, always <= TEXTFIELD_MAX - 1 chars
    int len;    // k_strlen(buf), kept in sync so callers don't have to recompute it
    int cursor; // [0, len] -- caret position; insert/backspace/delete operate here
    int active; // 1 = has focus (draws a caret, accepts keys); 0 = inert, plain text
};

// Copies `initial` into buf (truncated to fit if longer than
// TEXTFIELD_MAX - 1), cursor at the end, active = 0. `initial` may be
// NULL for an empty field.
void widget_textfield_init(struct text_field *tf, const char *initial);

// Sets `active` directly -- see this section's top comment for who's
// responsible for calling this and when.
void widget_textfield_set_active(struct text_field *tf, int active);

// Handles one key while `tf->active`: printable ASCII (32-126) inserts
// at the cursor, backspace/delete edit around it, left/right/home/end
// move it. Returns 1 if the key was handled (caller should redraw), 0
// if it wasn't recognized -- notably, '\r'/'\n' and anything else
// falls through unhandled, since committing/deactivating on Enter is
// the caller's decision, not this widget's (some callers might want
// different behavior). Always returns 0, doing nothing, when
// `tf->active` is 0 -- callers don't need their own "if active" guard
// before calling this.
int widget_textfield_key(struct text_field *tf, int key);

// Fills (x, y, w, h) with `bg`, draws a 1px border in `border`, and
// draws `tf->buf` in `fg` -- plus a solid caret at `tf->cursor` if
// `tf->active`. Text longer than fits `w` is simply clipped the same
// way every other text-drawing call in this codebase already is (see
// gfx_draw_string()) -- there's no horizontal scroll-within-the-field
// yet, matching TEXTFIELD_MAX being small enough that this hasn't come
// up for a real caller.
void widget_textfield_draw(int x, int y, int w, int h, const struct text_field *tf,
                            uint32_t bg, uint32_t fg, uint32_t border);

// ---- checkbox widget ----
//
// A small square box, checked/unchecked, with an optional label to its
// right -- e.g. `widget_checkbox_draw(x, y, 14, checked, "Word wrap",
// bg, fg)`. Unlike every other widget in this file, there was no
// second hand-rolled implementation to consolidate first when this was
// added (see CHANGELOG.md's build 490) -- built ahead of an actual
// caller, by explicit choice, for whenever a future settings-style app
// needs one. Kept exactly as minimal as everything else here so it's
// cheap to have sitting unused: draw + hit-test only, no group/
// mutual-exclusivity logic (that's a radio-button concept, not this).

// Total clickable width for a checkbox at `size` with `label` (or just
// `size` if `label` is NULL) -- shared by draw()/hit() so they always
// agree on the same geometry, same pattern as the scrollbar widgets
// above.
int widget_checkbox_width(int size, const char *label);

// Draws the box (outlined in `fg`; filled with `fg` too, inset, when
// `checked`) at (x, y), sized `size` x `size`, then `label` (if any)
// in `fg` on `bg` to its right, vertically centered against the box.
void widget_checkbox_draw(int x, int y, int size, int checked, const char *label,
                           uint32_t bg, uint32_t fg);

// 1 if (px, py) falls inside the box+label's combined clickable area
// (widget_checkbox_width()'s width, by max(size, a text row's height)
// tall), 0 otherwise.
int widget_checkbox_hit(int x, int y, int size, const char *label, int px, int py);

#endif
