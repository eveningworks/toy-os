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
#include "wm_tray.h"
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

// Which visual state a title-bar button is in. Encodes the rule
// docs/gui-guidelines.md states for every armed control: a button that
// has been pressed and then dragged off falls back to REST, NOT hover
// -- something about to be cancelled must not look like it is still
// being interacted with, even though the cursor is elsewhere and the
// button is technically still armed.
static enum ui_state title_btn_state(int kind, int is_armed, int is_hovered) {
    if (is_armed && title_btn_armed_kind == kind) {
        return title_btn_pressed_active ? UI_STATE_PRESSED : UI_STATE_REST;
    }
    if (is_armed) return UI_STATE_REST; // a different button on this window is armed
    return (is_hovered && title_hover_kind == kind) ? UI_STATE_HOVER : UI_STATE_REST;
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

// --- hardware cursor ---------------------------------------------------
//
// When the display adapter has a cursor of its own (see gfx.h's
// gfx_hw_cursor_*() and kernel/drivers/vmsvga.c), the WM stops drawing
// one entirely: no sprite blit, no saving the pixels underneath, no
// damage. Moving it is a handful of register writes. That also removes
// the whole class of bug the software path has -- a stale sprite left
// behind is not expressible when nothing was ever painted into the
// framebuffer.
//
// Nothing here is conditional on WHICH adapter: gfx answers whether a
// hardware cursor exists, and on plain VGA (and any real machine) it
// says no and the software path below runs exactly as before.
static int hw_cursor_ready = 0;

// The sprite's two baked alpha masks, flattened into the 32-bit ARGB an
// adapter wants. Composited the same way draw_cursor_normal() does:
// outline first, fill over it, so a pixel both masks touch ends up the
// fill's colour.
static void hw_cursor_upload(void) {
    static uint32_t argb[CURSOR_SPRITE_H * CURSOR_SPRITE_W];
    for (int row = 0; row < CURSOR_SPRITE_H; row++) {
        for (int col = 0; col < CURSOR_SPRITE_W; col++) {
            uint8_t o = cursor_outline_alpha[row][col];
            uint8_t f = cursor_fill_alpha[row][col];
            uint8_t a = o > f ? o : f;
            // Fill wins where both are present, matching the software
            // draw order; the colour is white for fill, black outline.
            uint32_t rgb = f ? 0x00FFFFFFu : 0x00000000u;
            argb[row * CURSOR_SPRITE_W + col] = ((uint32_t)a << 24) | rgb;
        }
    }
    hw_cursor_ready = gfx_hw_cursor_define(argb, CURSOR_SPRITE_W, CURSOR_SPRITE_H, 0, 0);
}

// Saves what's under (x, y) before drawing the cursor there, so a later
// cursor-only move can restore it. Used by both render paths.
static void draw_cursor_at(int x, int y) {
    if (gfx_hw_cursor_available()) {
        if (!hw_cursor_ready) hw_cursor_upload();
        if (hw_cursor_ready) {
            gfx_hw_cursor_move(x, y);
            return; // nothing painted, so nothing to save or restore
        }
    }
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
    // No hand-picked tints any more: ui_state_bg() derives hover and
    // pressed from each button's OWN base colour, so the red close
    // button gets a lighter red and a darker red for free. The pair of
    // constants that used to live here -- one neutral, one a
    // specially-chosen lighter red -- existed only because a caller had
    // no way to shift an arbitrary packed colour. See
    // docs/gui-guidelines.md.
    int is_armed = (title_btn_armed_win == idx);
    int is_hovered = (title_hover_win == idx);

    // Icon-only buttons: widget_button() with label=NULL just fills the
    // background, then each hand-drawn icon goes on top with its own
    // gfx_* calls -- these aren't text, so widgets.h's centered-label
    // path doesn't apply to them (see widgets.h's top comment).
    widget_button_state(r.min_x, r.y, r.size, r.size, 0, btnbg, btnfg,
                         title_btn_state(0, is_armed, is_hovered));
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
    widget_button_state(r.max_x, r.y, r.size, r.size, 0, max_bg_base, max_fg,
                         title_btn_state(1, is_armed, is_hovered));
    gfx_draw_rect(r.max_x + 4, r.y + 4, r.size - 8, r.size - 8, max_fg); // maximize/restore: square outline

    // close: red button with a hand-drawn X. This used to draw the font
    // glyph 'x' via gfx_draw_char(), which clipped/overflowed once the
    // font became runtime-resizable (a glyph cell is 11x22 to 20x40px,
    // way bigger than this button at most sizes). A hand-drawn diagonal
    // cross scales cleanly with r.size instead, same approach already
    // used for the minimize/maximize icons above.
    widget_button_state(r.close_x, r.y, r.size, r.size, 0, gfx_rgb(190, 60, 60), btnfg,
                         title_btn_state(2, is_armed, is_hovered));
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

    draw_tray(ty, bg, fg);
}

// ---- scene damage region (compositor) ----
//
// Separate from gfx.c's own dirty-PIXEL tracking (which operates on the
// framebuffer, after drawing, purely to shrink gfx_present()'s blit) --
// this tracks, at the SCENE level, which screen region actually needs
// repainting BEFORE drawing happens, so wm_render_frame() can clip the
// whole pass to it instead of always touching the full screen (every
// draw call bottoms out at gfx_put_pixel(), which silently skips
// anything outside the active clip -- see gfx_set_clip_rect()). See
// docs/decisions.md for the overall design and why window geometry
// changes are handled precisely here (comparing each window's
// last-rendered rect to its current one, below) while most other
// redraw_pending sources (menus, taskbar, dialogs, the once-a-second
// clock) still fall back to a full-screen repaint for now -- a
// deliberately scoped first cut, not the final word; see the roadmap's
// Milestone 12 entry for what's still open.
static int damage_x0, damage_y0, damage_x1, damage_y1;

void wm_damage_rect(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    int x1 = x + w, y1 = y + h;
    if (damage_x1 <= damage_x0) { // was empty
        damage_x0 = x; damage_x1 = x1;
        damage_y0 = y; damage_y1 = y1;
        return;
    }
    if (x < damage_x0) damage_x0 = x;
    if (x1 > damage_x1) damage_x1 = x1;
    if (y < damage_y0) damage_y0 = y;
    if (y1 > damage_y1) damage_y1 = y1;
}

static void damage_reset(void) {
    damage_x0 = damage_y0 = damage_x1 = damage_y1 = 0;
}

// Compares every window's rect/visibility against what it was as of
// the last repaint and reports whatever changed as damage, before this
// frame draws anything -- see wm_damage_rect()'s comment above.
// Handles the three cases that don't need a caller elsewhere to
// explicitly report anything:
//   - a window just opened (last_w == 0, the "never rendered" sentinel)
//     -- damage its own (now-visible) rect; nothing to reveal from an
//     "old" position since there wasn't one.
//   - visibility flipped (minimized <-> restored) -- damage whichever
//     rect is relevant (current if now visible, last-known if not).
//     Sufficient on its own even though other windows may now be
//     revealed/covered underneath: redrawing everything within that
//     rect, back-to-front, naturally repaints whatever's really there
//     now, the same reason a plain move/resize's union rect below is
//     sufficient too.
//   - geometry changed (move/resize) -- damage the union of its old
//     and new rect.
// bring_to_front()/close_window() (wm.c) report their own precise
// damage directly via wm_damage_rect() at the point they mutate
// windows[], since by the time this runs a closed window is already
// gone from the array and a reordered window's geometry didn't change
// (nothing here would notice either on its own).
static void compute_window_damage(void) {
    for (int i = 0; i < window_count; i++) {
        struct window *w = &windows[i];
        int visible_now = (w->state != WIN_MINIMIZED);

        if (w->last_w == 0) {
            if (visible_now) wm_damage_rect(w->x, w->y, w->w, w->h);
        } else if (visible_now != w->last_visible) {
            wm_damage_rect(visible_now ? w->x : w->last_x,
                            visible_now ? w->y : w->last_y,
                            visible_now ? w->w : w->last_w,
                            visible_now ? w->h : w->last_h);
            // draw_taskbar()'s per-button tint depends on whether the
            // FRONTMOST window is visible (see wm.c's bring_to_front()
            // comment on the same point) -- minimizing/restoring it
            // changes that button's look even though no window's
            // geometry or z-order changed. Only actually matters when
            // i == window_count - 1, but damaging the strip either way
            // is cheap and simpler than special-casing which index.
            wm_damage_rect(0, screen_h - taskbar_h, screen_w, taskbar_h);
        } else if (visible_now && (w->x != w->last_x || w->y != w->last_y ||
                                    w->w != w->last_w || w->h != w->last_h)) {
            int ux0 = w->x < w->last_x ? w->x : w->last_x;
            int uy0 = w->y < w->last_y ? w->y : w->last_y;
            int ux1_a = w->x + w->w, ux1_b = w->last_x + w->last_w;
            int uy1_a = w->y + w->h, uy1_b = w->last_y + w->last_h;
            int ux1 = ux1_a > ux1_b ? ux1_a : ux1_b;
            int uy1 = uy1_a > uy1_b ? uy1_a : uy1_b;
            wm_damage_rect(ux0, uy0, ux1 - ux0, uy1 - uy0);
        }

        w->last_x = w->x; w->last_y = w->y; w->last_w = w->w; w->last_h = w->h;
        w->last_visible = visible_now;
    }
}

// The damage box as it stood at the end of the last frame, for
// apps/wm/wm_debug.c's `gui state`. Reports w/h <= 0 when nothing was
// damaged (a full-screen repaint) rather than pretending to a rect --
// that distinction is exactly what someone debugging a repaint wants.
void wm_debug_damage(int *out_x, int *out_y, int *out_w, int *out_h) {
    *out_x = damage_x0;
    *out_y = damage_y0;
    *out_w = damage_x1 - damage_x0;
    *out_h = damage_y1 - damage_y0;
}

// Re-applies whatever clip the current frame's scene should be drawn
// under -- the accumulated damage box, or none. Paired with
// clip_to_window_content() below, which narrows it temporarily.
static void apply_scene_clip(int has_damage) {
    if (has_damage) {
        gfx_set_clip_rect(damage_x0, damage_y0, damage_x1 - damage_x0, damage_y1 - damage_y0);
    } else {
        gfx_clear_clip_rect();
    }
}

// Narrows the clip to a window's CONTENT area for the duration of its
// on_draw(), so nothing an app draws can land outside its own window.
//
// **This is a containment boundary, not an optimisation.** Nothing else
// enforced it: an app whose content didn't fit painted straight over the
// desktop and any window behind it. Shrinking the Control Panel is how
// it turned up -- System Info's lower rows carried on down the desktop,
// perfectly legible, well outside the frame. gfx_draw_string_clipped()
// doesn't help there: it bounds a string's WIDTH and has no notion of a
// row budget, so the horizontal edge was clipped correctly while the
// bottom had nothing stopping it at all.
//
// Intersects with the scene clip by hand because gfx_set_clip_rect()
// REPLACES the active rect rather than intersecting -- setting the
// content rect naively would have widened the damage clip back out and
// quietly undone the compositor's whole point.
static void clip_to_window_content(const struct window *w, int has_damage) {
    int x0 = window_content_x(w), y0 = window_content_y(w);
    int x1 = x0 + window_content_w(w), y1 = y0 + window_content_h(w);

    if (has_damage) {
        if (damage_x0 > x0) x0 = damage_x0;
        if (damage_y0 > y0) y0 = damage_y0;
        if (damage_x1 < x1) x1 = damage_x1;
        if (damage_y1 < y1) y1 = damage_y1;
    }
    // A non-positive w/h is gfx_set_clip_rect()'s "nothing draws", which
    // is exactly right for a window with no visible content this frame.
    gfx_set_clip_rect(x0, y0, x1 - x0, y1 - y0);
}

// Does this window's current rect overlap the accumulated damage box at
// all? Used by wm_render_frame() (Phase 3, see docs/decisions.md) to
// skip a whole window's chrome/on_draw()/resize-grip work, not just the
// pixels it would have touched -- gfx_set_clip_rect() already drops
// those pixel writes for free, but the CALLS themselves (an app's
// on_draw() walking its own widgets/content) still cost real CPU even
// when every write they make gets clipped away. Only meaningful when a
// damage box was actually reported this frame (see the has_damage check
// at the call site) -- with no damage, every window is "unaffected" by
// this definition too, which would wrongly skip everyone.
static int window_intersects_damage(const struct window *w) {
    return w->x < damage_x1 && w->x + w->w > damage_x0 &&
           w->y < damage_y1 && w->y + w->h > damage_y0;
}

void wm_render_frame(int mx, int my) {
    // Undraw the cursor FIRST, before anything else repaints.
    //
    // Without this, the old cursor sprite is erased only where the scene
    // happens to repaint over it -- and during a resize that isn't
    // everywhere. Shrinking a window damages union(old, new), whose
    // bottom-right edge is exactly the old corner, which is exactly
    // where the grip (and therefore the cursor) is; the sprite extends
    // down-right PAST that edge, so the overhang was never repainted
    // and every frame of the drag left one behind. Growing hid the same
    // bug, because the window expands over the old position.
    //
    // Deliberately before apply_scene_clip() below: this write must not
    // be confined to the damage rect, since the whole point is that the
    // stale pixels are outside it. The pixels it restores are the real
    // pre-cursor scene content, so anywhere the repaint doesn't cover
    // they are already correct, and anywhere it does they're overwritten
    // a moment later.
    restore_cursor_under();

    compute_window_damage();

    // Clip this whole pass to the accumulated damage region, if any was
    // reported. No damage reported this frame (menus, taskbar, dialogs,
    // the clock tick, or the very first frame) means "unknown, be
    // safe" -- fall back to the full screen, same as every frame before
    // Phase 1+2, and every window is drawn (has_damage below is false,
    // so window_intersects_damage() is never even consulted).
    int has_damage = damage_x1 > damage_x0;
    apply_scene_clip(has_damage);

    desktop_draw(); // background + icon grid -- replaces the old bare gfx_clear() fill, see desktop.h

    // Phase 3: a window whose rect doesn't overlap this frame's damage
    // box gets skipped entirely -- not just clipped. Its chrome and
    // on_draw() would write nothing visible anyway (every write lands
    // outside the active clip and gfx_put_pixel() drops it), so calling
    // them is pure wasted CPU once the damage region is known to be
    // precise. See docs/decisions.md for why this needed
    // bring_to_front() (wm.c) to start damaging the previously-frontmost
    // window's rect too, not just the newly-promoted one -- that gap
    // was harmless before this skip existed (the call still happened,
    // just clipped away) and became a real visible bug once it didn't.
    for (int i = 0; i < window_count; i++) {
        if (windows[i].state == WIN_MINIMIZED) continue;
        if (has_damage && !window_intersects_damage(&windows[i])) continue;
        draw_window_chrome(&windows[i], i, i == window_count - 1);
        if (windows[i].app && windows[i].app->on_draw) {
            // Only the app's own draw is confined to its content area.
            // The chrome above and the grip below are the WM's own
            // pixels and deliberately live at/outside that boundary.
            clip_to_window_content(&windows[i], has_damage);
            windows[i].app->on_draw(&windows[i]);
            apply_scene_clip(has_damage);
        }
        draw_resize_grip(&windows[i]); // after on_draw() -- see its own comment
    }

    draw_taskbar();
    if (start_menu_open) start_menu_draw(mx, my);
    context_menu_draw(); // independent of start_menu_open -- the two are mutually exclusive (see wm_input.c)
    file_picker_draw(); // an app-opened modal (e.g. Notepad's Save As...) -- drawn above ordinary chrome/menus
    confirm_dialog_draw(); // drawn last (topmost, short of the cursor) -- the most modal overlay in the WM

    // The cursor is always drawn full/unclipped, regardless of the scene
    // damage rect above -- it doesn't track its own screen position
    // against damage the way windows do, and it's cheap enough (a
    // single small sprite) that there's no real cost to always letting
    // it through.
    gfx_clear_clip_rect();
    draw_cursor_at(mx, my); // also (re)establishes cursor_under for wm_render_cursor_move()

    gfx_present(); // blits only what actually got touched -- see gfx_present()'s
                    // own comment; bounded by the damage clip above instead of
                    // always being the whole screen, when damage was reported.

    damage_reset();
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
    if (gfx_hw_cursor_available() && hw_cursor_ready) {
        // The entire cheap path collapses to this: the adapter composites
        // the cursor, so a mouse move touches no pixels and needs no blit.
        gfx_hw_cursor_move(mx, my);
        return;
    }
    restore_cursor_under();
    draw_cursor_at(mx, my);
    gfx_present();
}
