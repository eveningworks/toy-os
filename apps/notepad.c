// A minimal text editor: type, backspace, enter, and a toolbar with
// Save/Load buttons that persist to the in-memory filesystem (see
// kernel/drivers/fs.c) under a fixed filename. No cursor movement (arrow
// keys are ignored) and no filename picker yet -- see apps/README.md if
// you want to extend it. Also doubles as the WM's keyboard-focus test:
// open it alongside About and confirm keystrokes always land in whichever
// window is on top.
#include "notepad.h"
#include "wm/wm.h"
#include "widgets.h"
#include "theme.h"
#include "kapi.h"

#define NOTEPAD_MAX 1024
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
// (see notepad_draw()'s max_cols/max_rows), this just picks a sensible
// starting size for the current font.
#define NOTEPAD_COLS 44
#define NOTEPAD_ROWS 12
#define STATUS_COLS 14 // room for the longest status text, "No file yet."

// Single static instance -- the window manager only allows one open
// Notepad window at a time (see wm.c's open_app), so this doesn't need
// to be a pool.
struct notepad_state {
    char text[NOTEPAD_MAX];
    int len;
    char status[32]; // brief feedback after Save/Load, shown in the toolbar
};
static struct notepad_state g_notepad;

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
    g_notepad.len = 0;
    g_notepad.text[0] = '\0';
    g_notepad.status[0] = '\0';
    window_set_state(win, &g_notepad);
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
    int ch = window_content_h(win);

    uint32_t bg = THEME_WHITE;
    uint32_t fg = THEME_TEXT;

    draw_toolbar(win, st, cx, cy, cw, fg);

    int text_y = cy + TOOLBAR_H;
    int text_h = ch - TOOLBAR_H;
    gfx_fill_rect(cx, text_y, cw, text_h, bg);

    int char_w = gfx_char_w(), char_h = gfx_char_h();
    int max_cols = cw / char_w;
    int max_rows = text_h / char_h;
    if (max_cols < 1) max_cols = 1;

    int col = 0, row = 0;
    for (int i = 0; i < st->len && row < max_rows; i++) {
        char c = st->text[i];
        if (c == '\n') {
            row++;
            col = 0;
            continue;
        }
        if (col >= max_cols) {
            row++;
            col = 0;
            if (row >= max_rows) break;
        }
        gfx_draw_char(cx + col * char_w, text_y + row * char_h, c, fg, bg);
        col++;
    }

    // Simple end-of-text cursor block (no mid-text editing, so it's
    // always at the end).
    if (row < max_rows) {
        gfx_fill_rect(cx + col * char_w, text_y + row * char_h, 2, char_h, fg);
    }
}

void notepad_key(struct window *win, int key) {
    struct notepad_state *st = (struct notepad_state *)window_get_state(win);

    if (key == '\b') {
        if (st->len > 0) st->len--;
    } else if (key == '\r' || key == '\n') {
        if (st->len < NOTEPAD_MAX - 1) st->text[st->len++] = '\n';
    } else if (key >= 32 && key < 127) {
        if (st->len < NOTEPAD_MAX - 1) st->text[st->len++] = (char)key;
    } else {
        return; // ignore arrows / other control codes for this simple version
    }

    st->status[0] = '\0'; // typing invalidates any stale "Saved."/"Loaded."
    st->text[st->len] = '\0';
    window_invalidate(win);
}

void notepad_click(struct window *win, int cx, int cy) {
    struct notepad_state *st = (struct notepad_state *)window_get_state(win);
    if (cy < 0 || cy >= TOOLBAR_H) return; // only the toolbar row is clickable

    int save_x0 = BTN_MARGIN;
    int load_x0 = save_x0 + BTN_W + BTN_GAP;

    if (widget_hit(save_x0, 0, BTN_W, TOOLBAR_H, cx, cy)) {
        fs_write(NOTEPAD_FILE, st->text, 0);
        k_strcpy(st->status, "Saved.");
    } else if (widget_hit(load_x0, 0, BTN_W, TOOLBAR_H, cx, cy)) {
        uint32_t size = 0;
        const char *data = fs_read(NOTEPAD_FILE, &size);
        if (data) {
            uint32_t n = size < (uint32_t)(NOTEPAD_MAX - 1) ? size : (uint32_t)(NOTEPAD_MAX - 1);
            k_memcpy(st->text, data, n);
            st->len = (int)n;
            st->text[st->len] = '\0';
            k_strcpy(st->status, "Loaded.");
        } else {
            k_strcpy(st->status, "No file yet.");
        }
    } else {
        return;
    }

    window_invalidate(win);
}
