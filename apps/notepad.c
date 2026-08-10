// A minimal text editor: type, backspace, enter, arrow-key/Home/End
// cursor movement, Delete, and a toolbar with Save/Load buttons that
// persist to the in-memory filesystem (see kernel/include/fs.h) under a
// fixed filename. No filename picker yet -- see apps/README.md if you
// want to extend it. Also doubles as the WM's keyboard-focus test: open
// it alongside About and confirm keystrokes always land in whichever
// window is on top.
//
// Cursor movement (build 377) rides on widgets.h's text_scrollback
// gaining a real cursor -- see its own top comment for why: this file
// was exactly the "second real caller" that justified adding it, next
// to the CLI/GUI-Terminal text editor (apps/editor.c) that motivated
// the work in the first place. One known gap: moving the cursor
// off-screen (e.g. pressing Up repeatedly while scrolled) doesn't
// auto-scroll the view to follow it -- the cursor just becomes
// invisible until scrolled back into view by hand. Not implemented in
// this pass; see CHANGELOG.md's build 377 entry.
//
// Phase 4/4 of the scrollbar plan (see CHANGELOG.md builds 263, 273,
// 283 for the first three): converted from a flat char[] + manual
// col/row draw loop to the same struct text_scrollback (widgets.h)
// apps/terminal.c uses. That's the whole point of sharing the widget --
// Page Up/Page Down, the visual draggable scrollbar, and the mouse
// wheel all come for free from code already written and tested for
// Terminal, instead of a second, Notepad-specific scrolling
// implementation. Save/Load now serialize the scrollback's ring buffer
// to/from a flat byte stream at the filesystem boundary (see
// notepad_serialize()/notepad_load_text() below) since fs_write()/
// fs_read() (fs.h) only know about flat buffers, not this widget.
#include "notepad.h"
#include "wm/wm.h"
#include "widgets.h"
#include "theme.h"
#include "kapi.h"

#define NOTEPAD_FILE "notepad.txt"
// Macros, not cached constants, so both track gfx_char_w()/gfx_char_h()
// live if the font size changes at runtime (see gfx_set_font_size()) --
// same reasoning as WM_TITLEBAR_H in wm.h.
#define TOOLBAR_H (gfx_char_h() + 8)
#define BTN_W (4 * gfx_char_w() + 16) // fits "Save"/"Load" (4 chars) at any font size
#define BTN_GAP 8
#define BTN_MARGIN 4
// How many text rows/cols the initial window should comfortably fit --
// text itself always reflows to whatever size the window actually is
// (see widget_scrollback_draw()'s own reflow), this just picks a
// sensible starting size for the current font.
#define NOTEPAD_COLS 44
#define NOTEPAD_ROWS 12
#define STATUS_COLS 14 // room for the longest status text, "No file yet."
// Same pattern as TERM_SCROLLBAR_W/TERM_MIN_W_FOR_SCROLLBAR in
// terminal.c: width of the scrollbar strip reserved along the text
// area's right edge, skipped entirely below a minimum width.
#define NOTEPAD_SCROLLBAR_W (gfx_char_w() + 4)
#define NOTEPAD_MIN_W_FOR_SCROLLBAR (NOTEPAD_SCROLLBAR_W * 3)

// Single static instance -- the window manager only allows one open
// Notepad window at a time (see wm.c's open_app), so this doesn't need
// to be a pool.
struct notepad_state {
    struct text_scrollback tb;
    char status[32]; // brief feedback after Save/Load, shown in the toolbar
    int scrollbar_grab_offset; // set by notepad_drag_start(), read by notepad_drag() -- see widgets.h's widget_scrollbar_thumb_rect()
};
static struct notepad_state g_notepad;

// Scratch buffer for notepad_serialize() (see notepad_click()'s Save
// handler) -- static, not a stack-local, deliberately: the WM runs on
// the kernel's own boot stack (16KB total, see boot.asm), not a process
// kstack, and putting a SCROLLBACK_CAP-sized (8KB) buffer on it would
// eat half of that in one local array on top of whatever call depth
// already got here. A second static instance is fine for the same
// reason g_notepad itself is: only one Notepad window can ever be open
// (see wm.c's open_app), so there's nothing to make reentrant.
static char g_save_buf[SCROLLBACK_CAP];

// Content-area size for the current font -- see gui_apps.h's
// default_size. Width is whichever of "toolbar + status text" or
// "NOTEPAD_COLS of text" is wider, so the toolbar never feels cramped
// even though the text area itself is fully dynamic.
void notepad_default_size(int *w, int *h) {
    int toolbar_w = 2 * BTN_MARGIN + 2 * BTN_W + BTN_GAP + STATUS_COLS * gfx_char_w();
    int text_w = NOTEPAD_COLS * gfx_char_w();
    *w = toolbar_w > text_w ? toolbar_w : text_w;
    *h = TOOLBAR_H + NOTEPAD_ROWS * gfx_char_h();
}

void notepad_open(struct window *win) {
    widget_scrollback_init(&g_notepad.tb);
    widget_scrollback_set_color(&g_notepad.tb, VGA_BLACK); // near-black-on-white, not the terminal's light-grey-on-black
    g_notepad.status[0] = '\0';
    window_set_state(win, &g_notepad);
}

// Splits the text area (below the toolbar) into the text region and (if
// there's room) the scrollbar strip -- shared by notepad_draw(),
// notepad_key()'s Page Up/Down handling, and the click/drag handlers
// below, so all four agree on exactly the same geometry
// widget_scrollback_draw() actually used to render. Mirrors terminal.c's
// term_layout(); out_text_h excludes the toolbar row the same way
// out_ch there is the whole content area (Terminal has no toolbar).
static void notepad_layout(struct window *win, int *out_text_w, int *out_text_h, int *out_show_scrollbar) {
    int cw = window_content_w(win);
    *out_text_h = window_content_h(win) - TOOLBAR_H;
    if (cw > NOTEPAD_MIN_W_FOR_SCROLLBAR) {
        *out_show_scrollbar = 1;
        *out_text_w = cw - NOTEPAD_SCROLLBAR_W;
    } else {
        *out_show_scrollbar = 0;
        *out_text_w = cw;
    }
}

static void draw_toolbar(struct window *win, struct notepad_state *st,
                          int cx, int cy, int cw, uint32_t fg) {
    uint32_t toolbar_bg = THEME_BUTTON_BG;
    uint32_t btn_bg = gfx_rgb(200, 200, 212);
    gfx_fill_rect(cx, cy, cw, TOOLBAR_H, toolbar_bg);

    int by = cy + BTN_MARGIN;
    int bh = TOOLBAR_H - 2 * BTN_MARGIN;

    int save_x = cx + BTN_MARGIN;
    widget_button(save_x, by, BTN_W, bh, "Save", btn_bg, fg);

    int load_x = save_x + BTN_W + BTN_GAP;
    widget_button(load_x, by, BTN_W, bh, "Load", btn_bg, fg);

    if (st->status[0]) {
        gfx_draw_string(load_x + BTN_W + 12, by, st->status, fg, toolbar_bg);
    }
    (void)win;
}

void notepad_draw(struct window *win) {
    struct notepad_state *st = (struct notepad_state *)window_get_state(win);
    int cx = window_content_x(win);
    int cy = window_content_y(win);
    int cw = window_content_w(win);

    uint32_t bg = THEME_WHITE;
    uint32_t fg = THEME_TEXT;

    draw_toolbar(win, st, cx, cy, cw, fg);

    int text_y = cy + TOOLBAR_H;
    int text_w, text_h, show_scrollbar;
    notepad_layout(win, &text_w, &text_h, &show_scrollbar);

    widget_scrollback_draw(&st->tb, cx, text_y, text_w, text_h, bg, 1);

    if (show_scrollbar) {
        int total_lines, visible_rows;
        widget_scrollback_metrics(&st->tb, text_w, text_h, &total_lines, &visible_rows);
        widget_scrollbar_draw(cx + text_w, text_y, NOTEPAD_SCROLLBAR_W, text_h, total_lines, visible_rows,
                               st->tb.scroll_offset, gfx_rgb(225, 225, 230), gfx_rgb(150, 150, 160));
    }
}

void notepad_key(struct window *win, int key) {
    struct notepad_state *st = (struct notepad_state *)window_get_state(win);

    if (key == '\b') {
        widget_scrollback_backspace_at_cursor(&st->tb);
    } else if (key == KEY_DELETE) {
        widget_scrollback_delete_at_cursor(&st->tb);
    } else if (key == '\r' || key == '\n') {
        widget_scrollback_insert_at_cursor(&st->tb, '\n');
    } else if (key == KEY_ARROW_LEFT) {
        widget_scrollback_cursor_left(&st->tb);
    } else if (key == KEY_ARROW_RIGHT) {
        widget_scrollback_cursor_right(&st->tb);
    } else if (key == KEY_ARROW_UP) {
        widget_scrollback_cursor_up(&st->tb);
    } else if (key == KEY_ARROW_DOWN) {
        widget_scrollback_cursor_down(&st->tb);
    } else if (key == KEY_HOME) {
        widget_scrollback_cursor_home(&st->tb);
    } else if (key == KEY_END) {
        widget_scrollback_cursor_end(&st->tb);
    } else if (key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) {
        // Same "one screenful minus a line of overlap" convention as
        // terminal.c's KEY_PAGE_UP/KEY_PAGE_DOWN handling.
        int text_w, text_h, show_scrollbar;
        notepad_layout(win, &text_w, &text_h, &show_scrollbar);
        int total_lines, visible_rows;
        widget_scrollback_metrics(&st->tb, text_w, text_h, &total_lines, &visible_rows);
        int page = visible_rows > 1 ? visible_rows - 1 : 1;
        widget_scrollback_scroll(&st->tb, key == KEY_PAGE_UP ? page : -page);
        window_invalidate(win);
        return; // paging doesn't touch st->status/the text itself
    } else if (key >= 32 && key < 127) {
        widget_scrollback_insert_at_cursor(&st->tb, (char)key);
    } else {
        return; // ignore other control codes for this simple version
    }

    st->status[0] = '\0'; // typing invalidates any stale "Saved."/"Loaded."
    window_invalidate(win);
}

// Flattens the scrollback's ring buffer into a plain null-terminated
// byte string -- fs_write() (fs.h) only knows how to persist flat
// buffers, not this widget, and reaches into tb's buf/start/count
// fields directly (same as terminal.c's scrollbar code already reads
// tb.scroll_offset directly -- struct text_scrollback is a plain public
// struct, not an opaque handle). `max` should leave room for the
// trailing '\0'; returns the number of characters written (excluding
// it).
static int notepad_serialize(struct text_scrollback *tb, char *out, int max) {
    int n = 0;
    for (int i = 0; i < tb->count && n < max - 1; i++) {
        out[n++] = tb->buf[(tb->start + i) % SCROLLBACK_CAP].ch;
    }
    out[n] = '\0';
    return n;
}

// Inverse of notepad_serialize(): clears the scrollback and re-appends
// `data` one character at a time via the normal putc path, so loaded
// text goes through exactly the same code a typed character would
// (same ring-buffer-full behavior if a saved file somehow exceeds
// SCROLLBACK_CAP, same coloring).
static void notepad_load_text(struct text_scrollback *tb, const char *data, uint32_t n) {
    widget_scrollback_clear(tb);
    for (uint32_t i = 0; i < n && data[i] != '\0'; i++) {
        widget_scrollback_putc(tb, data[i]);
    }
}

// Toolbar (Save/Load) clicks, and scrollbar track clicks (page up/down)
// that aren't on the thumb -- thumb clicks never reach here, they're
// claimed by notepad_drag_start() instead (see gui_apps.h's
// on_click/on_drag_start contract). Mirrors terminal.c's terminal_click.
void notepad_click(struct window *win, int cx, int cy) {
    struct notepad_state *st = (struct notepad_state *)window_get_state(win);

    if (cy < TOOLBAR_H) {
        int save_x0 = BTN_MARGIN;
        int load_x0 = save_x0 + BTN_W + BTN_GAP;

        if (widget_hit(save_x0, 0, BTN_W, TOOLBAR_H, cx, cy)) {
            notepad_serialize(&st->tb, g_save_buf, sizeof(g_save_buf));
            fs_write(NOTEPAD_FILE, g_save_buf, 0);
            k_strcpy(st->status, "Saved.");
        } else if (widget_hit(load_x0, 0, BTN_W, TOOLBAR_H, cx, cy)) {
            uint32_t size = 0;
            const char *data = fs_read(NOTEPAD_FILE, &size);
            if (data) {
                notepad_load_text(&st->tb, data, size);
                k_strcpy(st->status, "Loaded.");
            } else {
                k_strcpy(st->status, "No file yet.");
            }
        } else {
            return;
        }
        window_invalidate(win);
        return;
    }

    int text_w, text_h, show_scrollbar;
    notepad_layout(win, &text_w, &text_h, &show_scrollbar);
    if (!show_scrollbar) return;

    int local_cy = cy - TOOLBAR_H;
    int total_lines, visible_rows;
    widget_scrollback_metrics(&st->tb, text_w, text_h, &total_lines, &visible_rows);
    enum scrollbar_zone zone = widget_scrollbar_hit(text_w, 0, NOTEPAD_SCROLLBAR_W, text_h,
                                                      total_lines, visible_rows, st->tb.scroll_offset, cx, local_cy);
    int page = visible_rows > 1 ? visible_rows - 1 : 1;
    if (zone == SCROLLBAR_ZONE_ABOVE) {
        widget_scrollback_scroll(&st->tb, page);
    } else if (zone == SCROLLBAR_ZONE_BELOW) {
        widget_scrollback_scroll(&st->tb, -page);
    } else {
        return; // click landed in the text area (or the bar isn't shown) -- nothing to do
    }
    window_invalidate(win);
}

int notepad_drag_start(struct window *win, int cx, int cy) {
    struct notepad_state *st = (struct notepad_state *)window_get_state(win);
    if (cy < TOOLBAR_H) return 0; // toolbar clicks are ordinary clicks, never a drag

    int text_w, text_h, show_scrollbar;
    notepad_layout(win, &text_w, &text_h, &show_scrollbar);
    if (!show_scrollbar) return 0;

    int local_cy = cy - TOOLBAR_H;
    int total_lines, visible_rows;
    widget_scrollback_metrics(&st->tb, text_w, text_h, &total_lines, &visible_rows);
    enum scrollbar_zone zone = widget_scrollbar_hit(text_w, 0, NOTEPAD_SCROLLBAR_W, text_h,
                                                      total_lines, visible_rows, st->tb.scroll_offset, cx, local_cy);
    if (zone != SCROLLBAR_ZONE_THUMB) return 0;

    int thumb_y, thumb_h;
    widget_scrollbar_thumb_rect(0, text_h, total_lines, visible_rows, st->tb.scroll_offset, &thumb_y, &thumb_h);
    st->scrollbar_grab_offset = local_cy - thumb_y;
    return 1;
}

void notepad_drag(struct window *win, int cx, int cy) {
    (void)cx; // this is a purely vertical scrollbar -- only cy matters
    struct notepad_state *st = (struct notepad_state *)window_get_state(win);
    int text_w, text_h, show_scrollbar;
    notepad_layout(win, &text_w, &text_h, &show_scrollbar);
    (void)show_scrollbar; // a drag only ever starts while true; harmless either way if the window shrank mid-drag

    int local_cy = cy - TOOLBAR_H;
    int total_lines, visible_rows;
    widget_scrollback_metrics(&st->tb, text_w, text_h, &total_lines, &visible_rows);
    st->tb.scroll_offset = widget_scrollbar_offset_for_drag(0, text_h, total_lines, visible_rows,
                                                              local_cy, st->scrollbar_grab_offset);
    window_invalidate(win);
}

// 3 lines per notch -- same convention as terminal.c's terminal_wheel.
#define NOTEPAD_WHEEL_LINES 3

void notepad_wheel(struct window *win, int delta) {
    struct notepad_state *st = (struct notepad_state *)window_get_state(win);
    widget_scrollback_scroll(&st->tb, delta * NOTEPAD_WHEEL_LINES);
    window_invalidate(win);
}
