#ifndef UTEXT_H
#define UTEXT_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_edit.h"

// utext -- the ring-3 port of apps/ui/ui_scrollback.c: a wrapped,
// scrollable, editable text buffer with a cursor and a selection.
//
// This is the widget that makes an editor possible in ring 3, and it is
// a faithful port of the kernel one's DESIGN rather than a fresh
// invention. Three properties are carried over deliberately, because
// each is load-bearing and each is easy to lose in a rewrite:
//
//   1. Wrapping is recomputed from the raw character stream on every
//      draw, never cached. That is what keeps it correct across window
//      resizes and font changes with no invalidation bookkeeping to get
//      wrong. The buffer is small enough that two passes cost nothing.
//
//   2. measure / draw / index_at_point all share ONE wrap accounting
//      (utext_measure below). They cannot disagree about where a line
//      breaks -- which matters because index_at_point() is the exact
//      inverse of draw()'s placement, and a click landing one character
//      off is the classic symptom of two copies of that arithmetic
//      drifting.
//
//   3. There is no "lines" concept in storage. '\n' is an ordinary
//      character and the cursor_up/down/home/end calls scan for the
//      nearest newline on the fly. A line index would be a second
//      representation to keep in sync with the text.
//
// ONE DELIBERATE REDUCTION from the kernel version: cells store no
// per-character colour. The kernel widget carries an `enum vga_color`
// per cell because apps/terminal.c renders coloured output through it;
// the only ring-3 client is an editor whose text is all one colour, so
// storing 8KB of identical colour bytes would be waste. A future
// userland terminal would add it back -- and should, rather than
// working around its absence.

#define UTEXT_CAP 8192 // characters; the ring drops the OLDEST when full

struct utext {
    char buf[UTEXT_CAP];
    int start; // ring head -- logical index i lives at buf[(start+i) % UTEXT_CAP]
    int count; // valid characters, 0..UTEXT_CAP

    // 0 = pinned to the newest text; >0 = scrolled up that many wrapped
    // lines. Clamped to the valid range on every measure, since "valid"
    // depends on the current width.
    int scroll_offset;

    // Caret and selection, plus the keymap that goes with them, come
    // from the shared edit core (ui/uui_edit.h) -- the same one the
    // single-line uui_textbox uses, so a field and a document cannot
    // disagree about what Ctrl+A or Shift+Left does.
    //
    // `ed.cursor` is what `cursor` used to be, and `ed.sel_anchor` /
    // `ed.sel_active` what those were: same meanings, one owner.
    struct uui_edit ed;
};

void utext_init(struct utext *t);
void utext_clear(struct utext *t);

// Appends at the END (not the cursor), evicting the oldest character if
// full -- for loading a file, where there is no cursor yet.
void utext_putc(struct utext *t, char c);

// Reads logical index `i`. Out of range returns 0.
char utext_at(const struct utext *t, int i);

// Writes logical index `i`, in place. Out of range does nothing.
//
// **THIS IS WHAT A TERMINAL DOES AND AN EDITOR DOES NOT.** A carriage
// return moves the caret to column 0 and what follows OVERWRITES what
// was there; that is how /bin/tosh repaints an edited line through fd 1
// with nothing but '\r'. Insert-at-cursor cannot express it -- it would
// push the old text along rather than replacing it -- which is why this
// exists beside utext_insert() rather than instead of it.
void utext_set(struct utext *t, int i, char c);

// Scrolls by whole wrapped lines: positive is toward older text.
// Scrolling back to 0 re-pins to the bottom.
void utext_scroll(struct utext *t, int delta_lines);

// Total wrapped lines and visible rows for a `w` x `h` box, without
// drawing. Also clamps t->scroll_offset, so after this the caller has
// everything a scrollbar needs.
void utext_metrics(struct utext *t, int w, int h,
                    int *out_total_lines, int *out_visible_rows);

// Fills the box with `bg`, draws the visible window, highlights any
// selection with `sel_bg`, and (if `show_cursor`) draws a caret bar.
void utext_draw(struct utext *t, struct ugfx_surface *s,
                 int x, int y, int w, int h,
                 uint32_t fg, uint32_t bg, uint32_t sel_bg, int show_cursor);

// The inverse of draw()'s placement: a point in the same coordinate
// space becomes a logical index. Clicking past a line's end lands at
// that end; below the last line lands at the buffer's end.
int utext_index_at_point(struct utext *t, int x, int y, int w, int h, int px, int py);

// --- cursor movement and editing -------------------------------------

void utext_cursor_left(struct utext *t);
void utext_cursor_right(struct utext *t);
void utext_cursor_up(struct utext *t);
void utext_cursor_down(struct utext *t);
void utext_cursor_home(struct utext *t);
void utext_cursor_end(struct utext *t);

// Inserts at the cursor and advances past it. REFUSES at UTEXT_CAP
// rather than evicting from the head the way utext_putc() does --
// shifting the ring's start out from under an in-progress edit would
// desync `cursor` from the text it points into.
void utext_insert(struct utext *t, char c);

void utext_delete(struct utext *t);    // forward, cursor stays put
void utext_backspace(struct utext *t); // backward, cursor moves back

// --- selection --------------------------------------------------------

void utext_sel_start(struct utext *t);  // anchor at the current cursor
void utext_sel_clear(struct utext *t);
int  utext_sel_present(const struct utext *t);
void utext_sel_range(const struct utext *t, int *out_start, int *out_end);
void utext_sel_delete(struct utext *t);

// One keypress through the SHARED keymap: Ctrl+A, Shift+arrows, typing
// replaces the selection, Backspace and Delete remove it. Returns 1 if
// consumed. Enter is deliberately NOT consumed -- a document inserts a
// newline and a field commits, and that is the caller's call.
//
// Notepad hand-wrote roughly sixty lines of this before the core
// existed; a second editor would have written them again, differently.
int utext_key(struct utext *t, int key, unsigned mods); // removes it and clears

#endif
