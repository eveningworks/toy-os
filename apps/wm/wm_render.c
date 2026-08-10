// Everything the window manager draws -- window chrome, the taskbar,
// the Start menu, the cursor -- plus the shared layout metrics
// (title_buttons(), start_btn_w(), etc) that wm_input.c also needs for
// hit-testing the exact same regions this file draws. See wm.c's top
// comment for the overall split and wm_internal.h for the shared state.
#include "wm_internal.h"
#include "widgets.h"
#include "theme.h"
#include "kapi.h"

int start_btn_w(void) {
    return (int)k_strlen(START_LABEL) * gfx_char_w() + 24;
}

int win_btn_w(void) {
    return WIN_LABEL_MAX_CHARS * gfx_char_w() + 24;
}

int start_menu_w(void) {
    return 12 * gfx_char_w() + 20; // room for the longest app name we expect
}

// The minimize/maximize/close title-bar buttons used to be a fixed
// BTN_SIZE 18 -- fine back when the font was a fixed 16x16 cell, but once
// the font became runtime-selectable (11x22 up to 20x40) that stopped
// tracking anything. The close button in particular drew a full font
// glyph ('x') inside that fixed 18px box, so it clipped/overflowed badly
// at medium and large sizes. Deriving the button size from WM_TITLEBAR_H
// (itself already font-aware) keeps all three buttons proportional to
// whatever font is active, same fix as the taskbar buttons above.
int btn_size(void) {
    int s = WM_TITLEBAR_H - 6;
    return s < 14 ? 14 : s;
}

struct btn_rects title_buttons(const struct window *win) {
    struct btn_rects r;
    r.size = btn_size();
    int gap = 4, margin = 6;
    r.y = win->y + (WM_TITLEBAR_H - r.size) / 2;
    r.close_x = win->x + win->w - margin - r.size;
    r.max_x = r.close_x - gap - r.size;
    r.min_x = r.max_x - gap - r.size;
    return r;
}

// Hand-drawn diagonal X, sized to fit inside a button/icon area of
// size x size with a small margin. Uses a thickened line (a few
// parallel diagonals) so it stays visible at both the small 14-18px
// buttons and the larger ones on bigger font sizes, without needing
// any glyph/font metrics.
static void draw_close_icon(int x, int y, int size, uint32_t color) {
    int pad = size / 4;
    if (pad < 2) pad = 2;
    int thick = size >= 24 ? 1 : 0; // extra 1px of thickness on bigger buttons
    for (int i = pad; i < size - pad; i++) {
        for (int t = -thick; t <= thick; t++) {
            gfx_put_pixel(x + i + t, y + i, color);
            gfx_put_pixel(x + i, y + i + t, color);
            gfx_put_pixel(x + i + t, y + (size - 1 - i), color);
            gfx_put_pixel(x + i, y + (size - 1 - i) + t, color);
        }
    }
}

static void draw_cursor_normal(int x, int y) {
    uint32_t color = THEME_WHITE, outline = gfx_rgb(0, 0, 0);
    for (int row = 0; row < 14; row++) {
        int w = row + 1;
        if (w > 10) w = 10;
        for (int col = 0; col < w; col++) {
            uint32_t c = (col == w - 1 || row == 11) ? outline : color;
            gfx_put_pixel(x + col, y + row, c);
        }
    }
}

// Directional resize cursors, shown while hovering (or actively
// dragging) a resizable window's edge/corner -- see
// wm_find_resize_zone() (wm_input.c). Same construction as each other
// (and the same "triangle wedge with an outline on its slanted edges"
// style as draw_cursor_normal() above): a short double-headed wedge
// shape, tapering from a single-pixel tip at each end up to a 3px-wide
// base in the middle, connected by a thin 1px shaft.
static void draw_cursor_h(int x, int y) { // <-> : right-edge resize
    uint32_t color = THEME_WHITE, outline = gfx_rgb(0, 0, 0);
    int w = 13, mid = 3;
    for (int i = 0; i < w; i++) {
        int half = (i < 4) ? i : (i >= w - 4 ? (w - 1 - i) : 0);
        for (int t = -half; t <= half; t++) {
            uint32_t c = (half > 0 && (t == half || t == -half)) ? outline : color;
            gfx_put_pixel(x + i, y + mid + t, c);
        }
    }
}

static void draw_cursor_v(int x, int y) { // up/down : bottom-edge resize
    uint32_t color = THEME_WHITE, outline = gfx_rgb(0, 0, 0);
    int h = 13, mid = 3;
    for (int j = 0; j < h; j++) {
        int half = (j < 4) ? j : (j >= h - 4 ? (h - 1 - j) : 0);
        for (int t = -half; t <= half; t++) {
            uint32_t c = (half > 0 && (t == half || t == -half)) ? outline : color;
            gfx_put_pixel(x + mid + t, y + j, c);
        }
    }
}

static void draw_cursor_diag(int x, int y) { // corner resize -- same shape as H/V, rotated 45deg
    uint32_t color = THEME_WHITE, outline = gfx_rgb(0, 0, 0);
    int n = 13;
    for (int i = 0; i < n; i++) {
        int half = (i < 4) ? i : (i >= n - 4 ? (n - 1 - i) : 0);
        for (int t = -half; t <= half; t++) {
            uint32_t c = (half > 0 && (t == half || t == -half)) ? outline : color;
            gfx_put_pixel(x + i + t, y + i - t, c);
        }
    }
}

static void draw_cursor(int x, int y, enum wm_cursor_kind kind) {
    switch (kind) {
        case WM_CURSOR_H:    draw_cursor_h(x, y);    break;
        case WM_CURSOR_V:    draw_cursor_v(x, y);    break;
        case WM_CURSOR_DIAG: draw_cursor_diag(x, y); break;
        default:              draw_cursor_normal(x, y); break;
    }
}

static void draw_window_chrome(struct window *win, int focused) {
    uint32_t titlebar = focused ? gfx_rgb(50, 90, 160) : gfx_rgb(120, 120, 130);
    uint32_t titletext = THEME_WHITE;
    uint32_t winbg = THEME_WINDOW_BG;
    int can_resize = win->app && win->app->resizable;

    gfx_fill_rect(win->x, win->y, win->w, win->h, winbg);

    // Subtle 1px 3D bevel instead of a flat outline -- a light highlight
    // on the top/left edge and a dark shadow on the bottom/right edge,
    // like the window is a slightly raised panel. Kept local to this
    // function rather than named in theme.h since nothing else draws a
    // bevel yet (see theme.h's top comment on only naming values that
    // actually repeat).
    uint32_t bevel_light = gfx_rgb(200, 200, 205);
    uint32_t bevel_dark = gfx_rgb(40, 40, 45);
    gfx_fill_rect(win->x, win->y, win->w, 1, bevel_light);            // top
    gfx_fill_rect(win->x, win->y, 1, win->h, bevel_light);            // left
    gfx_fill_rect(win->x, win->y + win->h - 1, win->w, 1, bevel_dark); // bottom
    gfx_fill_rect(win->x + win->w - 1, win->y, 1, win->h, bevel_dark); // right

    gfx_fill_rect(win->x + 1, win->y + 1, win->w - 2, WM_TITLEBAR_H, titlebar);

    struct btn_rects r = title_buttons(win);

    // Truncate the title if the window's been resized narrower than it
    // needs -- otherwise it draws over the minimize/maximize/close buttons.
    char title_buf[WIN_TITLE_MAX];
    int avail_px = r.min_x - (win->x + 6);
    int max_chars = avail_px > 0 ? avail_px / gfx_char_w() : 0;
    int i = 0;
    for (; win->title[i] && i < max_chars && i < WIN_TITLE_MAX - 1; i++) title_buf[i] = win->title[i];
    title_buf[i] = '\0';
    gfx_draw_string(win->x + 6, win->y + (WM_TITLEBAR_H - gfx_char_h()) / 2, title_buf, titletext, titlebar);

    uint32_t btnbg = gfx_rgb(230, 230, 235);
    uint32_t btnfg = THEME_TEXT;

    // Icon-only buttons: widget_button() with label=NULL just fills the
    // background, then each hand-drawn icon goes on top with its own
    // gfx_* calls -- these aren't text, so widgets.h's centered-label
    // path doesn't apply to them (see widgets.h's top comment).
    widget_button(r.min_x, r.y, r.size, r.size, 0, btnbg, btnfg);
    gfx_fill_rect(r.min_x + 4, r.y + r.size - 6, r.size - 8, 2, btnfg); // minimize: short bar

    // Maximize/restore: drawn muted and does nothing when the app isn't
    // resizable (Calculator -- see gui_apps.h) -- a visibly "disabled"
    // button rather than removing it, so the title bar layout doesn't
    // shift between fixed and resizable apps.
    uint32_t max_bg = can_resize ? btnbg : gfx_rgb(210, 210, 212);
    uint32_t max_fg = can_resize ? btnfg : gfx_rgb(170, 170, 172);
    widget_button(r.max_x, r.y, r.size, r.size, 0, max_bg, max_fg);
    gfx_draw_rect(r.max_x + 4, r.y + 4, r.size - 8, r.size - 8, max_fg); // maximize/restore: square outline

    // close: red button with a hand-drawn X. This used to draw the font
    // glyph 'x' via gfx_draw_char(), which clipped/overflowed once the
    // font became runtime-resizable (a glyph cell is 11x22 to 20x40px,
    // way bigger than this button at most sizes). A hand-drawn diagonal
    // cross scales cleanly with r.size instead, same approach already
    // used for the minimize/maximize icons above.
    widget_button(r.close_x, r.y, r.size, r.size, 0, gfx_rgb(190, 60, 60), btnfg);
    draw_close_icon(r.close_x, r.y, r.size, THEME_WHITE);
}

// Resize grip hint in the bottom-right corner -- drawn as a separate
// pass AFTER the app's on_draw() (see wm_render_frame()), not as part
// of draw_window_chrome() above. The grip's pixels sit right at the
// edge of the content area, and every app's on_draw() repaints its
// whole content area every frame (e.g. notepad_draw()'s background
// gfx_fill_rect()) -- drawing the grip before that content redraw meant
// it was invisible, silently painted over a moment later. Not shown
// when maximized or when the app isn't resizable (see gui_apps.h).
static void draw_resize_grip(const struct window *win) {
    if (win->state == WIN_MAXIMIZED) return;
    if (!(win->app && win->app->resizable)) return;
    uint32_t border = THEME_BORDER;
    gfx_fill_rect(win->x + win->w - 8, win->y + win->h - 3, 5, 2, border);
    gfx_fill_rect(win->x + win->w - 3, win->y + win->h - 8, 2, 5, border);
}

static void draw_clock_area(int taskbar_y, uint32_t bg, uint32_t fg) {
    struct rtc_time t;
    rtc_read_local(&t); // local time for the selected `timezone`, not raw UTC

    char buf[9];
    buf[0] = '0' + (t.hour / 10);
    buf[1] = '0' + (t.hour % 10);
    buf[2] = ':';
    buf[3] = '0' + (t.minute / 10);
    buf[4] = '0' + (t.minute % 10);
    buf[5] = ':';
    buf[6] = '0' + (t.second / 10);
    buf[7] = '0' + (t.second % 10);
    buf[8] = '\0';

    int text_w = 8 * gfx_char_w();
    int cx = screen_w - text_w - 16;
    int text_y = taskbar_y + (taskbar_h - gfx_char_h()) / 2;
    gfx_fill_rect(cx - 4, taskbar_y, text_w + 8, taskbar_h, bg);
    gfx_draw_string(cx, text_y, buf, fg, bg);
}

static void draw_taskbar(void) {
    uint32_t bg = gfx_rgb(30, 30, 34), fg = gfx_rgb(230, 230, 230);
    int ty = screen_h - taskbar_h;
    gfx_fill_rect(0, ty, screen_w, taskbar_h, bg);

    int sbw = start_btn_w(), wbw = win_btn_w();

    uint32_t start_bg = start_menu_open ? gfx_rgb(70, 70, 90) : gfx_rgb(50, 50, 60);
    widget_button(4, ty + 4, sbw, taskbar_h - 8, START_LABEL, start_bg, fg);

    int bx = 4 + sbw + 8;
    for (int i = 0; i < window_count; i++) {
        int is_front_and_visible = (i == window_count - 1 && windows[i].state != WIN_MINIMIZED);
        uint32_t wbg = is_front_and_visible ? gfx_rgb(70, 70, 90) : gfx_rgb(50, 50, 60);

        char label[WIN_LABEL_MAX_CHARS + 1];
        int n = 0;
        for (; windows[i].title[n] && n < WIN_LABEL_MAX_CHARS; n++) label[n] = windows[i].title[n];
        label[n] = '\0';
        widget_button(bx, ty + 4, wbw, taskbar_h - 8, label, wbg, fg);
        bx += wbw + 4;
    }

    draw_clock_area(ty, bg, fg);
}

static void draw_start_menu(void) {
    int item_h = gfx_char_h() + 6;
    int menu_w = start_menu_w();
    int menu_x = 4;
    int menu_h = item_h * gui_app_registry_count;
    int menu_y = (screen_h - taskbar_h) - menu_h;

    uint32_t bg = THEME_PANEL_BG, border = THEME_BORDER, fg = THEME_TEXT;
    gfx_fill_rect(menu_x, menu_y, menu_w, menu_h, bg);
    gfx_draw_rect(menu_x, menu_y, menu_w, menu_h, border);
    for (int i = 0; i < gui_app_registry_count; i++) {
        gfx_draw_string(menu_x + 8, menu_y + i * item_h + 3, gui_app_registry[i].name, fg, bg);
    }
}

void wm_render_frame(int mx, int my) {
    gfx_clear(gfx_rgb(24, 60, 90));

    for (int i = 0; i < window_count; i++) {
        if (windows[i].state == WIN_MINIMIZED) continue;
        draw_window_chrome(&windows[i], i == window_count - 1);
        if (windows[i].app && windows[i].app->on_draw) windows[i].app->on_draw(&windows[i]);
        draw_resize_grip(&windows[i]); // after on_draw() -- see its own comment
    }

    draw_taskbar();
    if (start_menu_open) draw_start_menu();

    // Resize cursor: while actively dragging a resize, keep showing the
    // cursor for whichever edge(s) that drag started on (don't re-query
    // by position -- the mouse may have moved past the window's edge
    // mid-drag). Otherwise ask the same hit-test the click handler uses
    // (wm_find_resize_zone(), wm_input.c) so hovering shows the cursor
    // before you click, not just while dragging.
    enum wm_cursor_kind kind = WM_CURSOR_NORMAL;
    int cur_right = 0, cur_bottom = 0;
    if (resizing >= 0) {
        cur_right = resize_right;
        cur_bottom = resize_bottom;
    } else if (wm_find_resize_zone(mx, my, &cur_right, &cur_bottom) < 0) {
        cur_right = cur_bottom = 0;
    }
    if (cur_right && cur_bottom) kind = WM_CURSOR_DIAG;
    else if (cur_right) kind = WM_CURSOR_H;
    else if (cur_bottom) kind = WM_CURSOR_V;
    draw_cursor(mx, my, kind);

    gfx_present(); // flip the finished frame to the display in one pass
}
