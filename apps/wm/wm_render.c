// Everything the window manager draws -- window chrome, the taskbar,
// the cursor -- plus the shared layout metrics (title_buttons(),
// start_btn_w(), etc) that wm_input.c also needs for hit-testing the
// exact same regions this file draws. See wm.c's top comment for the
// overall split and wm_internal.h for the shared state. The Start
// menu POPUP's own drawing lives in start_menu.c now (see that file) --
// this file only draws the taskbar Start BUTTON that opens it, a
// separate piece of chrome.
#include "wm_internal.h"
#include "start_menu.h"
#include "context_menu.h"
#include "confirm_dialog.h"
#include "file_picker.h"
#include "desktop.h"
#include "ui/ui.h"
#include "theme.h"
#include "kapi.h"

int start_btn_w(void) {
    return (int)k_strlen(START_LABEL) * gfx_char_w() + 24;
}

int win_btn_w(void) {
    return WIN_LABEL_MAX_CHARS * gfx_char_w() + 24;
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

// Anti-aliased arrow cursor -- two baked 8-bit alpha masks (outline in
// black, fill in white), the same "baked alpha, blended per-pixel"
// approach font_ttf.h's glyphs use (see gfx_draw_char()), just for a
// one-off 13x19 sprite instead of a whole font -- not worth a new
// tools/gen_*.py baking step for a single asset, so these are pasted
// literal data, generated once with PIL (supersampled polygon fill +
// dilate/erode for the outline ring, downsampled to this size) rather
// than hand-drawn pixel by pixel. Replaces the old hard-edged
// staircase shape (a capped `row+1` triangle, no anti-aliasing at all)
// -- see docs/decisions.md and CHANGELOG.md's `[Unreleased]` entry for
// the "why" and the before/after screenshots.
#define CURSOR_SPRITE_W 13
#define CURSOR_SPRITE_H 19

static const unsigned char cursor_outline_alpha[CURSOR_SPRITE_H][CURSOR_SPRITE_W] = {
    {255,136,0,0,0,0,0,0,0,0,0,0,0},
    {255,255,126,0,0,0,0,0,0,0,0,0,0},
    {255,130,255,120,0,0,0,0,0,0,0,0,0},
    {255,0,136,255,120,0,0,0,0,0,0,0,0},
    {255,0,0,136,255,105,0,0,0,0,0,0,0},
    {255,0,0,0,150,254,105,0,0,0,0,0,0},
    {255,0,0,0,1,150,254,101,0,0,0,0,0},
    {255,0,0,0,0,1,154,252,91,0,0,0,0},
    {255,0,0,0,0,0,3,164,252,91,0,0,0},
    {255,0,0,0,0,0,0,3,164,252,82,0,0},
    {255,0,0,0,0,9,48,48,51,206,249,72,0},
    {255,0,0,17,20,46,255,221,207,207,207,116,0},
    {255,0,59,238,216,3,223,137,0,0,0,0,0},
    {255,76,245,206,254,64,119,235,8,0,0,0,0},
    {255,251,179,10,191,164,20,247,96,0,0,0,0},
    {255,159,4,0,91,247,18,159,203,0,0,0,0},
    {16,1,0,0,8,237,127,148,255,28,0,0,0},
    {0,0,0,0,0,143,255,239,140,10,0,0,0},
    {0,0,0,0,0,24,76,12,0,0,0,0,0},
};

static const unsigned char cursor_fill_alpha[CURSOR_SPRITE_H][CURSOR_SPRITE_W] = {
    {0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,125,0,0,0,0,0,0,0,0,0,0,0},
    {0,255,119,0,0,0,0,0,0,0,0,0,0},
    {0,255,255,119,0,0,0,0,0,0,0,0,0},
    {0,255,255,255,104,0,0,0,0,0,0,0,0},
    {0,255,255,255,254,104,0,0,0,0,0,0,0},
    {0,255,255,255,255,254,100,0,0,0,0,0,0},
    {0,255,255,255,255,255,252,88,0,0,0,0,0},
    {0,255,255,255,255,255,255,252,88,0,0,0,0},
    {0,255,255,255,255,246,207,207,204,43,0,0,0},
    {0,255,255,238,235,209,0,0,0,0,0,0,0},
    {0,255,196,0,3,252,0,0,0,0,0,0,0},
    {0,179,0,0,0,191,136,0,0,0,0,0,0},
    {0,0,0,0,0,84,235,0,0,0,0,0,0},
    {0,0,0,0,0,0,237,91,0,0,0,0,0},
    {0,0,0,0,0,0,128,107,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0},
};

static void draw_cursor_normal(int x, int y) {
    uint32_t fill = THEME_WHITE, outline = gfx_rgb(0, 0, 0);
    // Outline first, fill on top -- matches how the two masks were
    // baked (the outline ring sits where fill was subtracted out, so
    // drawing fill second never re-covers outline-only pixels, and
    // pixels both masks touch get the fill's fully-opaque top coat).
    for (int row = 0; row < CURSOR_SPRITE_H; row++) {
        for (int col = 0; col < CURSOR_SPRITE_W; col++) {
            gfx_blend_pixel(x + col, y + row, outline, cursor_outline_alpha[row][col]);
        }
    }
    for (int row = 0; row < CURSOR_SPRITE_H; row++) {
        for (int col = 0; col < CURSOR_SPRITE_W; col++) {
            gfx_blend_pixel(x + col, y + row, fill, cursor_fill_alpha[row][col]);
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

// Which cursor shape to show at (mx, my) right now -- shared by the full
// frame path and the cursor-only-moved path below, so hovering a resize
// edge shows the right cursor either way. While actively dragging a
// resize, keep showing the cursor for whichever edge(s) that drag started
// on (don't re-query by position -- the mouse may have moved past the
// window's edge mid-drag); otherwise ask the same hit-test the click
// handler uses (wm_find_resize_zone(), wm_input.c) so hovering shows the
// cursor before you click, not just while dragging.
static enum wm_cursor_kind resolve_cursor_kind(int mx, int my) {
    int cur_right = 0, cur_bottom = 0;
    if (resizing >= 0) {
        cur_right = resize_right;
        cur_bottom = resize_bottom;
    } else if (wm_find_resize_zone(mx, my, &cur_right, &cur_bottom) < 0) {
        cur_right = cur_bottom = 0;
    }
    if (cur_right && cur_bottom) return WM_CURSOR_DIAG;
    if (cur_right) return WM_CURSOR_H;
    if (cur_bottom) return WM_CURSOR_V;
    return WM_CURSOR_NORMAL;
}

// ---- cursor sprite save/restore ----
//
// Used only by wm_render_cursor_move() (the cheap "mouse moved, nothing
// else changed" path) to avoid a full-scene redraw for the single most
// common event this loop sees. Classic sprite trick: before drawing the
// cursor somewhere, save the back-buffer pixels it's about to overwrite;
// next time the cursor moves, restore exactly those pixels (undrawing the
// cursor) before drawing it at the new spot. wm_render_frame() (the full
// path) also goes through draw_cursor_at() so this stays correct across
// both paths -- a cursor-only move after a full repaint restores exactly
// what that repaint actually drew underneath, not stale content.
//
// CURSOR_BOX is anchored 2px above/left of the cursor's own (x,y) and
// sized generously to comfortably cover all four hand-drawn cursor
// shapes above, including draw_cursor_h()/draw_cursor_v()'s asymmetric
// bounding boxes (they draw up to 1px above/left of their own anchor --
// see their own comments) and draw_cursor_normal()'s sprite (13x19,
// drawn from the anchor down/right -- the tallest shape here, hence
// this needing to be taller than it is wide) -- getting this too small
// would leave a stale cursor-colored pixel behind on every move, so
// it's deliberately oversized rather than tightly fit to each shape.
#define CURSOR_BOX_MARGIN 2
#define CURSOR_BOX_SIZE 22

static uint32_t cursor_under[CURSOR_BOX_SIZE][CURSOR_BOX_SIZE];
static int cursor_under_valid = 0;
static int cursor_under_x, cursor_under_y;

static void restore_cursor_under(void) {
    if (!cursor_under_valid) return;
    for (int j = 0; j < CURSOR_BOX_SIZE; j++)
        for (int i = 0; i < CURSOR_BOX_SIZE; i++)
            gfx_put_pixel(cursor_under_x + i, cursor_under_y + j, cursor_under[j][i]);
    cursor_under_valid = 0;
}

static void save_cursor_under(int x, int y) {
    int sx = x - CURSOR_BOX_MARGIN, sy = y - CURSOR_BOX_MARGIN;
    for (int j = 0; j < CURSOR_BOX_SIZE; j++)
        for (int i = 0; i < CURSOR_BOX_SIZE; i++)
            cursor_under[j][i] = gfx_get_pixel(sx + i, sy + j);
    cursor_under_x = sx;
    cursor_under_y = sy;
    cursor_under_valid = 1;
}

// Saves what's under (x, y) before drawing the cursor there, so a later
// cursor-only move can restore it. Used by both render paths.
static void draw_cursor_at(int x, int y) {
    save_cursor_under(x, y);
    draw_cursor(x, y, resolve_cursor_kind(x, y));
}

static void draw_window_chrome(struct window *win, int idx, int focused) {
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

    // Hover/press feedback for the three title-bar buttons -- see
    // wm_internal.h's title_btn_armed_win/title_hover_win comments for
    // the full press-hover-commit-on-release story. Each button gets
    // one of three looks: plain (nothing armed or hovered), hover (a
    // lighter tint -- cursor's over it, mouse not held), or pressed
    // (armed AND the cursor's still over it -- the tint PLUS
    // widget_button()'s own inset `pressed` look, same visual language
    // used everywhere else a button can be held in this codebase, e.g.
    // Calculator). Dragging off an armed button (armed but NOT
    // pressed_active) drops back to the plain look, not hover -- a
    // button that's about to be canceled shouldn't look like it's still
    // being interacted with.
    uint32_t hover_tint = gfx_rgb(200, 215, 235);       // minimize/maximize's neutral hover tint
    uint32_t hover_tint_close = gfx_rgb(215, 110, 110);  // close needs a lighter RED, not the neutral tint
    int is_armed = (title_btn_armed_win == idx);
    int is_hovered = (title_hover_win == idx);

    // Icon-only buttons: widget_button() with label=NULL just fills the
    // background, then each hand-drawn icon goes on top with its own
    // gfx_* calls -- these aren't text, so widgets.h's centered-label
    // path doesn't apply to them (see widgets.h's top comment).
    int min_pressed = is_armed && title_btn_armed_kind == 0 && title_btn_pressed_active;
    int min_tint = is_armed ? min_pressed : (is_hovered && title_hover_kind == 0);
    widget_button(r.min_x, r.y, r.size, r.size, 0, min_tint ? hover_tint : btnbg, btnfg, min_pressed);
    gfx_fill_rect(r.min_x + 4, r.y + r.size - 6, r.size - 8, 2, btnfg); // minimize: short bar

    // Maximize/restore: drawn muted and does nothing when the app isn't
    // resizable (Calculator -- see gui_apps.h) -- a visibly "disabled"
    // button rather than removing it, so the title bar layout doesn't
    // shift between fixed and resizable apps. Still shows hover/press
    // feedback either way (it still focuses the window on commit even
    // when disabled -- see wm_update_title_btn_press()), just tinted
    // from its own muted base color instead of btnbg.
    uint32_t max_bg_base = can_resize ? btnbg : gfx_rgb(210, 210, 212);
    uint32_t max_fg = can_resize ? btnfg : gfx_rgb(170, 170, 172);
    int max_pressed = is_armed && title_btn_armed_kind == 1 && title_btn_pressed_active;
    int max_tint = is_armed ? max_pressed : (is_hovered && title_hover_kind == 1);
    widget_button(r.max_x, r.y, r.size, r.size, 0, max_tint ? hover_tint : max_bg_base, max_fg, max_pressed);
    gfx_draw_rect(r.max_x + 4, r.y + 4, r.size - 8, r.size - 8, max_fg); // maximize/restore: square outline

    // close: red button with a hand-drawn X. This used to draw the font
    // glyph 'x' via gfx_draw_char(), which clipped/overflowed once the
    // font became runtime-resizable (a glyph cell is 11x22 to 20x40px,
    // way bigger than this button at most sizes). A hand-drawn diagonal
    // cross scales cleanly with r.size instead, same approach already
    // used for the minimize/maximize icons above.
    int close_pressed = is_armed && title_btn_armed_kind == 2 && title_btn_pressed_active;
    int close_tint = is_armed ? close_pressed : (is_hovered && title_hover_kind == 2);
    uint32_t close_bg = close_tint ? hover_tint_close : gfx_rgb(190, 60, 60);
    widget_button(r.close_x, r.y, r.size, r.size, 0, close_bg, btnfg, close_pressed);
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
    widget_button(4, ty + 4, sbw, taskbar_h - 8, START_LABEL, start_bg, fg, 0);

    int bx = 4 + sbw + 8;
    for (int i = 0; i < window_count; i++) {
        int is_front_and_visible = (i == window_count - 1 && windows[i].state != WIN_MINIMIZED);
        uint32_t wbg = is_front_and_visible ? gfx_rgb(70, 70, 90) : gfx_rgb(50, 50, 60);

        char label[WIN_LABEL_MAX_CHARS + 1];
        int n = 0;
        for (; windows[i].title[n] && n < WIN_LABEL_MAX_CHARS; n++) label[n] = windows[i].title[n];
        label[n] = '\0';
        widget_button(bx, ty + 4, wbw, taskbar_h - 8, label, wbg, fg, 0);
        bx += wbw + 4;
    }

    draw_clock_area(ty, bg, fg);
}

void wm_render_frame(int mx, int my) {
    desktop_draw(); // background + icon grid -- replaces the old bare gfx_clear() fill, see desktop.h

    for (int i = 0; i < window_count; i++) {
        if (windows[i].state == WIN_MINIMIZED) continue;
        draw_window_chrome(&windows[i], i, i == window_count - 1);
        if (windows[i].app && windows[i].app->on_draw) windows[i].app->on_draw(&windows[i]);
        draw_resize_grip(&windows[i]); // after on_draw() -- see its own comment
    }

    draw_taskbar();
    if (start_menu_open) start_menu_draw(mx, my);
    context_menu_draw(); // independent of start_menu_open -- the two are mutually exclusive (see wm_input.c)
    file_picker_draw(); // an app-opened modal (e.g. Notepad's Save As...) -- drawn above ordinary chrome/menus
    confirm_dialog_draw(); // drawn last (topmost, short of the cursor) -- the most modal overlay in the WM

    draw_cursor_at(mx, my); // also (re)establishes cursor_under for wm_render_cursor_move()

    gfx_present(); // blits only what actually got touched -- see gfx_present()'s
                    // own comment; for a full repaint that's normally still the
                    // whole screen (gfx_clear() at the top touches every pixel).
}

// The cheap path for "only the mouse moved, nothing else changed" --
// wm.c's wm_run() loop takes this instead of wm_render_frame() whenever
// redraw_pending is clear, which is most ticks most of the time (a mouse
// that isn't moving or clicking generates no work at all; a mouse that IS
// moving no longer forces a full window/taskbar/start-menu redraw and
// full-screen blit just to slide the cursor a few pixels). Undraws the
// cursor at its old spot, draws it at the new one, and gfx_present() then
// blits only the union of those two small boxes -- see this file's
// cursor-sprite comment above and gfx_present()'s own comment for the two
// halves that make this cheap.
void wm_render_cursor_move(int mx, int my) {
    restore_cursor_under();
    draw_cursor_at(mx, my);
    gfx_present();
}
