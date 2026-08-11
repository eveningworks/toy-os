#ifndef UI_SCROLLBACK_H
#define UI_SCROLLBACK_H

#include <stdint.h>
#include "vga.h"

// A reusable "console inside a window" primitive: a bounded stream of
// characters (each remembering the vga_color it was written in), with
// wrap-and-scroll rendering into any content rect. Originally lived in
// apps/widgets.c/.h; moved here (same content, no behavior/rename
// change) when the rest of that file's widgets each got their own
// apps/ui/ file -- see docs/decisions.md.
//
// First (and, at the time this was added, only) caller is
// apps/terminal.c, which pairs it with a `struct vga_sink` (vga.h) so
// shell_dispatch() (shell.h) can write straight into one of these
// instead of the physical console -- but nothing here actually depends
// on vga_sink or the shell; this is just a text buffer + renderer,
// usable anywhere a scrolling text area is useful. Two APIs sit on top
// of the same storage: the original append-only putc()/backspace() pair
// (apps/terminal.c's only need -- it never sees a cursor anywhere but
// the end), and a cursor-aware insert/delete/move API added later for
// real in-place editing (apps/notepad.c, apps/editor.c -- see the
// `cursor` field's own comment below). The two coexist because
// putc()/backspace()/clear() keep `cursor` pinned to the append point,
// so a caller that never touches the cursor API sees the exact same
// append-only behavior as before it existed.
//
// Storage is a fixed-size ring buffer (SCROLLBACK_CAP cells) -- once
// full, the oldest character is silently dropped for every new one
// appended, exactly like the physical console's own scrolloff behavior
// (see vga.c's legacy_scroll_if_needed()/fb_scroll_if_needed()). No
// heap, so this is meant to be embedded directly in a caller's static
// state struct (`struct text_scrollback tb;`), not allocated -- unlike
// ui_button/ui_textbox, this widget does NOT own its own screen
// geometry (draw() still takes cx/cy/cw/ch every call): every real
// caller recomputes its content area from the window's live size on
// every frame anyway (resize support), so there's no fixed geometry an
// owned-state wrapper would actually save here.
//
// Line wrapping is recomputed from the raw character stream on every
// draw (two passes over the buffer: one to count wrapped lines given
// the current content width, one to render the visible window) rather
// than cached, so it stays correct across window resizes and font-size
// changes with no invalidation bookkeeping. SCROLLBACK_CAP is small
// enough that two full passes per draw is cheap.

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
    // all. A real editor (apps/notepad.c, apps/editor.c) moves this
    // explicitly via the cursor_*() calls and edits at it via the
    // *_at_cursor() calls, both added alongside this field.
    int cursor;
    // Selection state, added alongside click-to-position/drag-select
    // support -- see the "selection" section below. `sel_active` is a
    // separate flag rather than encoding "no selection" as
    // `sel_anchor == cursor`, because a caller may legitimately want an
    // active-but-currently-empty selection mid-drag (anchor and cursor
    // briefly coincide the instant a drag starts, before the mouse has
    // moved) without that being indistinguishable from "no selection at
    // all". Both fields are 0 at init/clear, same as `cursor`.
    int sel_anchor;   // logical index the selection was started from
    int sel_active;   // 1 while a selection exists (even if empty), 0 otherwise
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
// the next draw. Scrolling back to exactly 0 re-pins to the bottom (new
// output will auto-follow again); any positive offset un-pins it.
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
// view given `tb->scroll_offset` and the current font size, wrapping at
// `cw`'s column count. Any selected cells (see the "selection" section
// below) are filled with `sel_bg` first -- pass any color if the caller
// never uses selection (e.g. apps/terminal.c today), it'll simply never
// be selected against. If `show_cursor` is non-zero and `tb->cursor`'s
// current position is within the visible window, draws a thin cursor
// bar there in `tb->cur_fg`. `sel_bg` is caller-supplied rather than a
// hardcoded color here, same as every other color this widget draws
// with -- see ui_primitives.c's own comment on apps/ui/ staying
// theme-agnostic.
void widget_scrollback_draw(struct text_scrollback *tb, int cx, int cy, int cw, int ch,
                             uint32_t bg, uint32_t sel_bg, int show_cursor);

// ---- cursor-aware editing (see the `cursor` field's own comment) ----
//
// Everything below operates on `tb->cursor`, moving it and/or
// inserting/deleting at its current position -- unlike putc()/
// backspace() above, none of these touch the append point directly.
// '\n' is an ordinary buffer character here: there's no separate
// concept of "lines" in storage, cursor_up()/cursor_down()/
// cursor_home()/cursor_end() just scan for the nearest '\n' boundaries
// on the fly -- cheap enough for SCROLLBACK_CAP's size.

// Moves the cursor one character left/right, clamped to [0, count].
void widget_scrollback_cursor_left(struct text_scrollback *tb);
void widget_scrollback_cursor_right(struct text_scrollback *tb);

// Moves the cursor up/down one line, preserving its column within the
// line where possible (clamped to the target line's length if it's
// shorter). No-op at the first/last line respectively.
void widget_scrollback_cursor_up(struct text_scrollback *tb);
void widget_scrollback_cursor_down(struct text_scrollback *tb);

// Moves the cursor to the start/end of its current line (the nearest
// '\n' boundary, or the buffer's start/end).
void widget_scrollback_cursor_home(struct text_scrollback *tb);
void widget_scrollback_cursor_end(struct text_scrollback *tb);

// Inserts `c` at the cursor, shifting everything after it right by one,
// and advances the cursor past the newly-inserted character. No-op if
// the buffer is already at SCROLLBACK_CAP -- refuses rather than
// evicting from the head the way putc() does, since shifting the ring's
// start out from under an in-progress edit would desync `cursor` from
// the content it's supposed to point into.
void widget_scrollback_insert_at_cursor(struct text_scrollback *tb, char c);

// Deletes the character the cursor is just before (forward delete, like
// a real editor's Delete key) -- cursor position doesn't change. No-op
// if the cursor is already at the end.
void widget_scrollback_delete_at_cursor(struct text_scrollback *tb);

// Deletes the character just before the cursor (backward delete, like
// Backspace) and moves the cursor back by one. No-op if the cursor is
// already at the start.
void widget_scrollback_backspace_at_cursor(struct text_scrollback *tb);

// ---- click-to-position + selection ----
//
// Added so a caller (apps/notepad.c to start with) can support mouse
// interaction with the text body, not just keyboard navigation. Both
// pieces are opt-in -- a caller that never calls any of these (e.g.
// apps/terminal.c today) sees no behavior change at all, same reasoning
// as the append-only vs. cursor-aware APIs above coexisting.

// Converts a point in the *same coordinate space* widget_scrollback_draw()
// takes (cx/cy/cw/ch is the widget's rect; px/py is the point to test,
// e.g. a click's content-relative coordinates) into a logical buffer
// index -- the inverse of the pixel math draw() uses to place each
// cell, using the identical wrapped-line/column accounting so a given
// index always round-trips to the same screen position draw() would
// render its cursor at. Clicking past the end of a line lands at that
// line's end; clicking below the last line lands at the end of the
// buffer; clicking in the middle of a character cell rounds down to
// that character (not the nearest edge).
int widget_scrollback_index_at_point(struct text_scrollback *tb, int cx, int cy, int cw, int ch,
                                      int px, int py);

// Starts a new selection anchored at the CURRENT cursor position (call
// this first, e.g. on mouse-down or the first shift+arrow press), then
// move `tb->cursor` however the caller likes (index_at_point() for a
// click/drag, or the cursor_*() calls above for shift+arrow) -- the
// selection always spans [min(anchor,cursor), max(anchor,cursor)) as of
// whatever `tb->cursor` currently is, computed lazily by
// widget_scrollback_selection_range() rather than tracked incrementally.
// Safe to call again to re-anchor (e.g. a fresh mouse-down replaces any
// selection from a previous drag).
void widget_scrollback_selection_start(struct text_scrollback *tb);

// Drops the current selection, if any (a plain click or arrow key with
// no shift held should call this). No-op if none is active.
void widget_scrollback_selection_clear(struct text_scrollback *tb);

// 1 if a selection is active AND currently non-empty (anchor != cursor),
// 0 otherwise -- what a caller actually wants to know before acting on
// "is there a selection to delete/highlight", since an active-but-empty
// selection (see the `sel_active` field comment) shouldn't visually
// highlight anything or be deletable.
int widget_scrollback_selection_present(const struct text_scrollback *tb);

// Fills *out_start/*out_end with the selection's buffer range, sorted
// low-to-high (a valid [start,end) half-open range even when the drag
// went right-to-left). Only meaningful when
// widget_scrollback_selection_present() is true; harmless (both set to
// tb->cursor) otherwise.
void widget_scrollback_selection_range(const struct text_scrollback *tb, int *out_start, int *out_end);

// Deletes the selected range (if any), moves the cursor to where the
// selection started, and clears the selection -- what Backspace/Delete
// or typing a replacement character should call first when a selection
// is present. No-op if widget_scrollback_selection_present() is false.
void widget_scrollback_delete_selection(struct text_scrollback *tb);

#endif
