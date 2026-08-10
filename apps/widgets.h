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
// just a text buffer + renderer, usable anywhere a scrolling read-only
// (from the widget's own point of view -- callers append via the
// putc/backspace API, there's no in-place editing) text area is useful.
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
// `show_cursor` is non-zero and the write position (end of the buffer)
// is within the visible window, draws a solid end-of-text cursor block
// there in `tb->cur_fg` -- same idea as notepad_draw()'s cursor, and
// with the same limitation: this widget has no concept of a cursor
// anywhere but the end (append-only), so there's nothing to draw when
// scrolled up past it.
void widget_scrollback_draw(struct text_scrollback *tb, int cx, int cy, int cw, int ch,
                             uint32_t bg, int show_cursor);

#endif
