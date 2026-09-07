#ifndef UTEXT_H
#define UTEXT_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_edit.h"

// utext -- a wrapped, scrollable, editable text buffer with a cursor
// and a selection.
//
// Three properties are load-bearing and easy to lose in a rewrite:
//
//   1. Wrapping is derived from the raw character stream, never stored.
//      That is what keeps it correct across window resizes and font
//      changes with no invalidation bookkeeping to get wrong. What IS
//      cached is a sparse index into that derivation -- see the wrap
//      cache below, which is an accelerator and never an answer.
//
//   2. measure / draw / index_at_point all share ONE wrap accounting
//      (wrap_step below). They cannot disagree about where a line
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
// **THE CALLER OWNS THE STORAGE, AND THAT IS WHAT LETS ONE DOCUMENT BE
// 8 KB AND ANOTHER 1.6 MB.** utext allocates nothing (ttf.h's scratch
// and uui_fileview's entry array take the same shape). A small editor
// hands it a static UTEXT_CAP buffer; Notepad sizes one to the file it
// is about to open. The buffer is FLAT, not a ring: an editor inserts
// in the middle, and the modulo a ring costs on every character was
// paid on every pass over the document.
//
// ONE DELIBERATE REDUCTION: cells store no per-character colour, so all
// text in a utext is one colour. A terminal needs per-cell attributes
// and should not be built on this.

#define UTEXT_CAP 8192 // the size a SMALL editor should hand it

// Sparse line index: `idx[k]` is the character index at which wrapped
// line `k * stride` begins. It is REBUILT, not maintained -- one O(n)
// pass whenever the text or the wrap width changes -- so it cannot
// drift from the text the way a maintained line table can.
//
// Fixed size, and the stride is what absorbs a bigger document: on
// filling up, the table keeps every second entry and doubles the
// stride. So a lookup scans at most `stride` lines from a checkpoint
// rather than the whole document from index 0, which is the difference
// between a 1.6 MB file redrawing per frame and not.
#define UTEXT_CKPTS 256

struct utext_wrap {
    int      valid;
    int      cols;        // the wrap width this was built for
    unsigned rev;         // the text revision it was built from
    int      total_lines;
    int      n;           // checkpoints in use
    int      stride;      // lines between checkpoints
    int      idx[UTEXT_CKPTS];
};

struct utext {
    char *buf;   // the caller's; utext never frees it
    int   cap;   // its size in characters
    int   count; // valid characters, 0..cap

    // Bumped by every mutation. The wrap cache compares it rather than
    // being invalidated by hand at each call site, which is the version
    // of this that goes wrong.
    unsigned rev;

    // 0 = pinned to the newest text; >0 = scrolled up that many wrapped
    // lines. Clamped to the valid range on every measure, since "valid"
    // depends on the current width.
    //
    // **0 IS THE BOTTOM, so a freshly loaded document must be scrolled
    // to the TOP explicitly** (utext_scroll_top) -- leaving it at 0 is
    // how every file Notepad opened arrived scrolled to its end.
    int scroll_offset;

    // Caret and selection, plus the keymap that goes with them, come
    // from the shared edit core (ui/uui_edit.h) -- the same one the
    // single-line uui_textbox uses, so a field and a document cannot
    // disagree about what Ctrl+A or Shift+Left does.
    struct uui_edit ed;

    struct utext_wrap wrap;
};

// `buf` must stay alive for as long as `t` is used, and holds `cap`
// characters. Both are the caller's to free.
void utext_init_buf(struct utext *t, char *buf, int cap);

// Empties the text; keeps the buffer.
void utext_clear(struct utext *t);

// Appends at the END (not the cursor), for loading a file. REFUSES at
// capacity rather than evicting: a document silently missing its head
// is worse than one that says it did not fit. Returns 1 if it went in.
int utext_putc(struct utext *t, char c);

// Reads logical index `i`. Out of range returns 0.
char utext_at(const struct utext *t, int i);

// Writes logical index `i`, in place. Out of range does nothing.
void utext_set(struct utext *t, int i, char c);

// Scrolls by whole wrapped lines: positive is toward older text.
void utext_scroll(struct utext *t, int delta_lines);

// The two ends, named rather than open-coded. `_top` is not
// `utext_scroll(t, t->count)` by luck -- it is that clamp stated once.
void utext_scroll_top(struct utext *t);
void utext_scroll_bottom(struct utext *t);

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

// Scrolls the view so the caret is on screen, if it is not already --
// what typing, arrow keys and a paste all need, and nothing else.
void utext_reveal_cursor(struct utext *t, int w, int h);

// --- cursor movement and editing -------------------------------------

void utext_cursor_left(struct utext *t);
void utext_cursor_right(struct utext *t);
void utext_cursor_up(struct utext *t);
void utext_cursor_down(struct utext *t);
void utext_cursor_home(struct utext *t);
void utext_cursor_end(struct utext *t);

// Inserts at the cursor and advances past it. Refuses at capacity.
void utext_insert(struct utext *t, char c);

void utext_delete(struct utext *t);    // forward, cursor stays put
void utext_backspace(struct utext *t); // backward, cursor moves back

// --- selection --------------------------------------------------------

void utext_sel_start(struct utext *t);  // anchor at the current cursor
void utext_sel_clear(struct utext *t);
int  utext_sel_present(const struct utext *t);
void utext_sel_range(const struct utext *t, int *out_start, int *out_end);
void utext_sel_delete(struct utext *t);
void utext_sel_all(struct utext *t);

// --- text in and out, for a clipboard ---------------------------------

// The selected characters. Returns how many there ARE, which may exceed
// `cap` -- so a caller with a fixed clipboard can refuse rather than
// copy half a selection. Writes at most `cap` bytes and NUL-terminates
// when there is room. 0 when nothing is selected.
int utext_sel_text(const struct utext *t, char *out, int cap);

// Replaces any selection with `n` characters, leaving the caret after
// them. Returns how many went in -- fewer than `n` means the buffer
// filled up. '\r' is dropped and '\r\n' therefore arrives as '\n', so
// text pasted from anywhere lands as this buffer's own line endings.
int utext_insert_text(struct utext *t, const char *s, int n);

// One keypress through the SHARED keymap: Ctrl+A, Shift+arrows, typing
// replaces the selection, Backspace and Delete remove it. Returns 1 if
// consumed. Enter is deliberately NOT consumed -- a document inserts a
// newline and a field commits, and that is the caller's call.
int utext_key(struct utext *t, int key, unsigned mods);

#endif
