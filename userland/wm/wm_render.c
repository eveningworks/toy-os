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
#include "wm_taskbar.h"
#include "cursor_theme.h"
#include "ui/uui.h"
#include "ui/utheme.h"
#include "kapi.h"
#include "wm/wm_log.h"

// The one screen this process composites into -- see wm_internal.h.
struct ugfx_screen g_wm_screen;

int start_btn_w(void) {
    return (int)k_strlen(START_LABEL) * ugfx_char_w() + 24;
}

int win_btn_w(void) {
    return WIN_LABEL_MAX_CHARS * ugfx_char_w() + 24;
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
static enum uui_state title_btn_state(int kind, int is_armed, int is_hovered) {
    if (is_armed && title_btn_armed_kind == kind) {
        return title_btn_pressed_active ? UUI_STATE_PRESSED : UUI_STATE_REST;
    }
    if (is_armed) return UUI_STATE_REST; // a different button on this window is armed
    return (is_hovered && title_hover_kind == kind) ? UUI_STATE_HOVER : UUI_STATE_REST;
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
            ugfx_put_pixel(wm_surface(), x + i + t, y + i, color);
            ugfx_put_pixel(wm_surface(), x + i, y + i + t, color);
            ugfx_put_pixel(wm_surface(), x + i + t, y + (size - 1 - i), color);
            ugfx_put_pixel(wm_surface(), x + i, y + (size - 1 - i) + t, color);
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
// -- see docs/decisions.md and the commit that added it for
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
    uint32_t fill = UTHEME_WHITE, outline = ugfx_rgb(0, 0, 0);
    // Outline first, fill on top -- matches how the two masks were
    // baked (the outline ring sits where fill was subtracted out, so
    // drawing fill second never re-covers outline-only pixels, and
    // pixels both masks touch get the fill's fully-opaque top coat).
    for (int row = 0; row < CURSOR_SPRITE_H; row++) {
        for (int col = 0; col < CURSOR_SPRITE_W; col++) {
            ugfx_blend_pixel(wm_surface(), x + col, y + row, outline, cursor_outline_alpha[row][col]);
        }
    }
    for (int row = 0; row < CURSOR_SPRITE_H; row++) {
        for (int col = 0; col < CURSOR_SPRITE_W; col++) {
            ugfx_blend_pixel(wm_surface(), x + col, y + row, fill, cursor_fill_alpha[row][col]);
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
    uint32_t color = UTHEME_WHITE, outline = ugfx_rgb(0, 0, 0);
    int w = 13, mid = 3;
    for (int i = 0; i < w; i++) {
        int half = (i < 4) ? i : (i >= w - 4 ? (w - 1 - i) : 0);
        for (int t = -half; t <= half; t++) {
            uint32_t c = (half > 0 && (t == half || t == -half)) ? outline : color;
            ugfx_put_pixel(wm_surface(), x + i, y + mid + t, c);
        }
    }
}

static void draw_cursor_v(int x, int y) { // up/down : bottom-edge resize
    uint32_t color = UTHEME_WHITE, outline = ugfx_rgb(0, 0, 0);
    int h = 13, mid = 3;
    for (int j = 0; j < h; j++) {
        int half = (j < 4) ? j : (j >= h - 4 ? (h - 1 - j) : 0);
        for (int t = -half; t <= half; t++) {
            uint32_t c = (half > 0 && (t == half || t == -half)) ? outline : color;
            ugfx_put_pixel(wm_surface(), x + mid + t, y + j, c);
        }
    }
}

static void draw_cursor_diag(int x, int y) { // corner resize -- same shape as H/V, rotated 45deg
    uint32_t color = UTHEME_WHITE, outline = ugfx_rgb(0, 0, 0);
    int n = 13;
    for (int i = 0; i < n; i++) {
        int half = (i < 4) ? i : (i >= n - 4 ? (n - 1 - i) : 0);
        for (int t = -half; t <= half; t++) {
            uint32_t c = (half > 0 && (t == half || t == -half)) ? outline : color;
            ugfx_put_pixel(wm_surface(), x + i + t, y + i - t, c);
        }
    }
}

// A themed shape: two coverage masks, coloured here rather than in the
// file, scaled by whole multiples. Outline first and fill over it, the
// same order draw_cursor_normal() uses and for the same reason -- a
// pixel both masks touch ends up the fill's colour.
//
// Nearest-neighbour at integer scales only: a pointer wants a hard
// edge, and a smoothly scaled mask reads as blurry rather than large.
static void draw_cursor_themed(const struct cursor_shape *s, int x, int y,
                                int scale) {
    uint32_t fill = UTHEME_WHITE, outline = ugfx_rgb(0, 0, 0);
    int ox = x - s->hot_x * scale, oy = y - s->hot_y * scale;
    for (int row = 0; row < s->h; row++) {
        for (int col = 0; col < s->w; col++) {
            unsigned char a = s->outline[row][col];
            if (!a) continue;
            for (int j = 0; j < scale; j++)
                for (int i = 0; i < scale; i++)
                    ugfx_blend_pixel(wm_surface(), ox + col * scale + i, oy + row * scale + j,
                                     outline, a);
        }
    }
    for (int row = 0; row < s->h; row++) {
        for (int col = 0; col < s->w; col++) {
            unsigned char a = s->fill[row][col];
            if (!a) continue;
            for (int j = 0; j < scale; j++)
                for (int i = 0; i < scale; i++)
                    ugfx_blend_pixel(wm_surface(), ox + col * scale + i, oy + row * scale + j,
                                     fill, a);
        }
    }
}

static void draw_cursor(int x, int y, enum wm_cursor_kind kind) {
    // A theme shape when one loaded, the built-in otherwise. That
    // either/or is the vendor-default/override pattern /etc already
    // uses: a missing or malformed theme file degrades to a working
    // pointer rather than to no pointer, which is the one failure this
    // must not have.
    const struct cursor_shape *s = cursor_theme_shape(kind);
    if (s) {
        draw_cursor_themed(s, x, y, cursor_theme_scale());
        return;
    }
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
#define CURSOR_BOX_SIZE 22 // the BUILT-IN shapes' box; themed ones derive theirs

// The themed shapes make the drawn extent variable -- a theme's size,
// its hotspot and the size setting's scale all move it -- so the box can
// no longer be one constant. It is DERIVED from whatever is actually
// going to be drawn, and every consumer (the save/restore pair and the
// damage rect) asks the same function. That is the invariant: if these
// two ever disagree, a cursor move leaves a stale sprite behind, which
// is a bug this file's comments already record paying for twice.
#define CURSOR_UNDER_MAX (CURSOR_SHAPE_MAX * 3 + 2 * CURSOR_BOX_MARGIN)

static void cursor_rect(enum wm_cursor_kind kind, int x, int y,
                         int *ox, int *oy, int *w, int *h) {
    const struct cursor_shape *s = cursor_theme_shape(kind);
    if (s) {
        int sc = cursor_theme_scale();
        *ox = x - s->hot_x * sc - CURSOR_BOX_MARGIN;
        *oy = y - s->hot_y * sc - CURSOR_BOX_MARGIN;
        *w  = s->w * sc + 2 * CURSOR_BOX_MARGIN;
        *h  = s->h * sc + 2 * CURSOR_BOX_MARGIN;
    } else {
        *ox = x - CURSOR_BOX_MARGIN;
        *oy = y - CURSOR_BOX_MARGIN;
        *w = *h = CURSOR_BOX_SIZE;
    }
    if (*w > CURSOR_UNDER_MAX) *w = CURSOR_UNDER_MAX;
    if (*h > CURSOR_UNDER_MAX) *h = CURSOR_UNDER_MAX;
}

static uint32_t cursor_under[CURSOR_UNDER_MAX][CURSOR_UNDER_MAX];
static int cursor_under_valid = 0;
static int cursor_under_x, cursor_under_y, cursor_under_w, cursor_under_h;

static void restore_cursor_under(void) {
    if (!cursor_under_valid) return;
    for (int j = 0; j < cursor_under_h; j++)
        for (int i = 0; i < cursor_under_w; i++)
            ugfx_put_pixel(wm_surface(), cursor_under_x + i, cursor_under_y + j, cursor_under[j][i]);
    cursor_under_valid = 0;
}

static void save_cursor_under(int x, int y, enum wm_cursor_kind kind) {
    int sx, sy, w, h;
    cursor_rect(kind, x, y, &sx, &sy, &w, &h);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            cursor_under[j][i] = ugfx_get_pixel(wm_surface(), sx + i, sy + j);
    cursor_under_x = sx;
    cursor_under_y = sy;
    cursor_under_w = w;
    cursor_under_h = h;
    cursor_under_valid = 1;
}

// --- no hardware cursor here, and that is a decision --------------------
//
// The ring-0 WM had a hardware-cursor path: when the adapter owned a
// cursor plane the WM drew none at all -- no sprite, no saved pixels, no
// damage. It is GONE from the ring-3 compositor rather than ported,
// because M41's R3 measured the capability away: DISPLAY_CAP_CURSOR is
// declared by one driver (vmsvga), which disables it by default because
// a hardware cursor over a RELATIVE PS/2 mouse makes the pointer jump.
// So it is unreachable on every configuration this OS boots, and a TWP
// path to reach it would have been a protocol surface for a capability
// nothing enables. It returns with virtio-input's absolute pointer in
// Milestone 27a, where it can actually be switched on.
//
// The software sprite below needs nothing from the kernel: it draws into
// the framebuffer the compositor was already granted (R1). See
// docs/decisions.md.

// The outline a client-window resize drag is proposing. Drawn instead
// of resizing the window, because a client's buffer is still the old
// size until the client itself changes it -- growing the frame around
// it would show a window with its content stuck in one corner, which is
// the whole reason resize is a handshake (abi/win_proto.h).
//
// Drawn just before the cursor, so it sits over the windows it
// describes. A 1px rectangle: cheap, unmistakable, and the same idiom
// every WM used before compositing made live resize affordable.
static void draw_resize_outline(void) {
    if (resizing < 0 || resize_prop_w <= 0 || resize_prop_h <= 0) return;
    const struct window *w = &windows[resizing];
    ugfx_draw_rect(wm_surface(), w->x, w->y, resize_prop_w, resize_prop_h, UTHEME_WHITE);
    ugfx_draw_rect(wm_surface(), w->x + 1, w->y + 1, resize_prop_w - 2, resize_prop_h - 2,
                   ugfx_rgb(50, 90, 160));
}

// Saves what's under (x, y) before drawing the cursor there, so a later
// cursor-only move can restore it. Used by both render paths.
static void draw_cursor_at(int x, int y) {
    draw_resize_outline();
    enum wm_cursor_kind kind = resolve_cursor_kind(x, y);
    save_cursor_under(x, y, kind);
    draw_cursor(x, y, kind);
}

static void draw_window_chrome(struct window *win, int idx, int focused) {
    uint32_t titlebar = focused ? ugfx_rgb(50, 90, 160) : ugfx_rgb(120, 120, 130);
    uint32_t titletext = UTHEME_WHITE;
    uint32_t winbg = UTHEME_WINDOW_BG;
    int can_resize = win->resizable;

    ugfx_fill_rect(wm_surface(), win->x, win->y, win->w, win->h, winbg);

    // Subtle 1px 3D bevel instead of a flat outline -- a light highlight
    // on the top/left edge and a dark shadow on the bottom/right edge,
    // like the window is a slightly raised panel. Kept local to this
    // function rather than named in theme.h since nothing else draws a
    // bevel yet (see theme.h's top comment on only naming values that
    // actually repeat).
    uint32_t bevel_light = ugfx_rgb(200, 200, 205);
    uint32_t bevel_dark = ugfx_rgb(40, 40, 45);
    ugfx_fill_rect(wm_surface(), win->x, win->y, win->w, 1, bevel_light);            // top
    ugfx_fill_rect(wm_surface(), win->x, win->y, 1, win->h, bevel_light);            // left
    ugfx_fill_rect(wm_surface(), win->x, win->y + win->h - 1, win->w, 1, bevel_dark); // bottom
    ugfx_fill_rect(wm_surface(), win->x + win->w - 1, win->y, 1, win->h, bevel_dark); // right

    ugfx_fill_rect(wm_surface(), win->x + 1, win->y + 1, win->w - 2, WM_TITLEBAR_H, titlebar);

    struct btn_rects r = title_buttons(win);

    // Truncate the title if the window's been resized narrower than it
    // needs -- otherwise it draws over the minimize/maximize/close buttons.
    //
    // A wedged client says so in its title bar, as it does on Windows.
    // Built into the SAME buffer that gets truncated, so the suffix is
    // subject to the same width budget as the name -- appending it after
    // the truncation would draw straight over the buttons, which is this
    // codebase's most-repeated drawing bug (docs/gui-guidelines.md).
    char full[WIN_TITLE_MAX + 20];
    const char *shown = win->title;
    if (win->not_responding) {
        k_snprintf(full, sizeof full, "%s (Not Responding)", win->title);
        shown = full;
    }

    char title_buf[WIN_TITLE_MAX + 20];
    int avail_px = r.min_x - (win->x + 6);
    int max_chars = avail_px > 0 ? avail_px / ugfx_char_w() : 0;
    int i = 0;
    for (; shown[i] && i < max_chars && i < (int)sizeof title_buf - 1; i++) title_buf[i] = shown[i];
    title_buf[i] = '\0';
    ugfx_draw_string(wm_surface(), win->x + 6, win->y + (WM_TITLEBAR_H - ugfx_char_h()) / 2, title_buf, titletext, titlebar);

    uint32_t btnbg = ugfx_rgb(230, 230, 235);
    uint32_t btnfg = UTHEME_TEXT;

    // Hover/press feedback for the three title-bar buttons -- see
    // wm_internal.h's title_btn_armed_win/title_hover_win comments for
    // the full press-hover-commit-on-release story. Each button gets
    // one of three looks: plain (nothing armed or hovered), hover (a
    // lighter tint -- cursor's over it, mouse not held), or pressed
    // (armed AND the cursor's still over it -- the tint PLUS
    // uui_button_draw()'s own inset `pressed` look, same visual language
    // used everywhere else a button can be held in this codebase, e.g.
    // Calculator). Dragging off an armed button (armed but NOT
    // pressed_active) drops back to the plain look, not hover -- a
    // button that's about to be canceled shouldn't look like it's still
    // being interacted with.
    // No hand-picked tints any more: uui_state_bg() derives hover and
    // pressed from each button's OWN base colour, so the red close
    // button gets a lighter red and a darker red for free. The pair of
    // constants that used to live here -- one neutral, one a
    // specially-chosen lighter red -- existed only because a caller had
    // no way to shift an arbitrary packed colour. See
    // docs/gui-guidelines.md.
    int is_armed = (title_btn_armed_win == idx ? UUI_STATE_PRESSED : UUI_STATE_REST);
    int is_hovered = (title_hover_win == idx);

    // Icon-only buttons: uui_button_draw() with label=NULL just fills the
    // background, then each hand-drawn icon goes on top with its own
    // gfx_* calls -- these aren't text, so widgets.h's centered-label
    // path doesn't apply to them (see widgets.h's top comment).
    uui_button_draw(wm_surface(), r.min_x, r.y, r.size, r.size, 0, btnbg, btnfg,
                         title_btn_state(0, is_armed, is_hovered) ? UUI_STATE_PRESSED : UUI_STATE_REST);
    ugfx_fill_rect(wm_surface(), r.min_x + 4, r.y + r.size - 6, r.size - 8, 2, btnfg); // minimize: short bar

    // Maximize/restore: drawn muted and does nothing when the app isn't
    // resizable (Calculator -- see gui_apps.h) -- a visibly "disabled"
    // button rather than removing it, so the title bar layout doesn't
    // shift between fixed and resizable apps. Still shows hover/press
    // feedback either way (it still focuses the window on commit even
    // when disabled -- see wm_update_title_btn_press()), just tinted
    // from its own muted base color instead of btnbg.
    uint32_t max_bg_base = can_resize ? btnbg : ugfx_rgb(210, 210, 212);
    uint32_t max_fg = can_resize ? btnfg : ugfx_rgb(170, 170, 172);
    uui_button_draw(wm_surface(), r.max_x, r.y, r.size, r.size, 0, max_bg_base, max_fg,
                         title_btn_state(1, is_armed, is_hovered));
    ugfx_draw_rect(wm_surface(), r.max_x + 4, r.y + 4, r.size - 8, r.size - 8, max_fg); // maximize/restore: square outline

    // close: red button with a hand-drawn X. This used to draw the font
    // glyph 'x' via gfx_draw_char(), which clipped/overflowed once the
    // font became runtime-resizable (a glyph cell is 11x22 to 20x40px,
    // way bigger than this button at most sizes). A hand-drawn diagonal
    // cross scales cleanly with r.size instead, same approach already
    // used for the minimize/maximize icons above.
    uui_button_draw(wm_surface(), r.close_x, r.y, r.size, r.size, 0, ugfx_rgb(190, 60, 60), btnfg,
                         title_btn_state(2, is_armed, is_hovered));
    draw_close_icon(r.close_x, r.y, r.size, UTHEME_WHITE);
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
    if (!win->resizable) return;
    uint32_t border = UTHEME_BORDER;
    ugfx_fill_rect(wm_surface(), win->x + win->w - 8, win->y + win->h - 3, 5, 2, border);
    ugfx_fill_rect(wm_surface(), win->x + win->w - 3, win->y + win->h - 8, 2, 5, border);
}

static void draw_taskbar(void) {
    uint32_t bg = ugfx_rgb(30, 30, 34), fg = ugfx_rgb(230, 230, 230);
    int ty = screen_h - taskbar_h;
    ugfx_fill_rect(wm_surface(), 0, ty, screen_w, taskbar_h, bg);

    int sbw = start_btn_w();

    uint32_t start_bg = start_menu_open ? ugfx_rgb(70, 70, 90) : ugfx_rgb(50, 50, 60);
    uui_button_draw(wm_surface(), 4, ty + 4, sbw, taskbar_h - 8, START_LABEL, start_bg, fg, UUI_STATE_REST);

    // The buttons come from taskbar_layout(), which is also what
    // wm_input.c hit-tests against -- this loop used to walk the windows
    // itself with a fixed step, and so did both hit-tests, which is how
    // the strip came to run off the screen edge (wm_taskbar.h).
    static struct taskbar_button btns[64];
    int nb = taskbar_layout(btns, 64);
    for (int b = 0; b < nb; b++) {
        int i = btns[b].first;
        int is_front_and_visible = (i == window_count - 1 && windows[i].state != WIN_MINIMIZED);
        uint32_t wbg = is_front_and_visible ? ugfx_rgb(70, 70, 90) : ugfx_rgb(50, 50, 60);
        uui_button_draw(wm_surface(), btns[b].x, ty + 4, btns[b].w, taskbar_h - 8,
                         btns[b].label, wbg, fg, UUI_STATE_REST);
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
        ugfx_set_clip_rect(wm_surface(), damage_x0, damage_y0, damage_x1 - damage_x0, damage_y1 - damage_y0);
    } else {
        ugfx_clear_clip_rect(wm_surface());
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
    // (This sentence used to be a lie -- gfx_set_clip_rect() CLEARED the
    // clip on non-positive sizes, so exactly this case handed the app an
    // unclipped full-screen on_draw(). That was the damage sweep's
    // standing 20px violation: a tick frame's damage strip grazing only
    // a window's bottom border row emptied this intersection, the
    // content repaint escaped, and the resize grip under it was buried.
    // The contract in gfx.c matches the words now.)
    ugfx_set_clip_rect(wm_surface(), x0, y0, x1 - x0, y1 - y0);
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

// Draws the whole scene for this frame. Split out of wm_render_frame()
// so it can be run TWICE: once damage-limited as normal, and once
// unrestricted for the verify mode below, which is only meaningful if
// both renders go through identical code.
static void render_scene(int mx, int my, int has_damage) {
    apply_scene_clip(has_damage);

    desktop_draw(); // background + icon grid -- replaces the old bare ugfx_fill(wm_surface(), ) fill, see desktop.h

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
        } else if (wm_client_is_client_window(&windows[i])) {
            // A ring-3 client's window: its content is just the pixels
            // it has already drawn into its shared buffer, blitted.
            // Same content clip an app's on_draw() gets, for the same
            // reason -- a client that reports a size larger than its
            // window must not be able to paint over the chrome.
            clip_to_window_content(&windows[i], has_damage);
            wm_client_draw(&windows[i]);
            apply_scene_clip(has_damage);
        }
        draw_resize_grip(&windows[i]); // after on_draw() -- see its own comment
    }

    draw_taskbar();
    if (start_menu_open) start_menu_draw(mx, my);
    context_menu_draw(mx, my); // independent of start_menu_open -- the two are mutually exclusive (see wm_input.c)
    file_picker_draw(); // an app-opened modal (e.g. Notepad's Save As...) -- drawn above ordinary chrome/menus
    confirm_dialog_draw(); // drawn last (topmost, short of the cursor) -- the most modal overlay in the WM

    // The cursor is always drawn full/unclipped, regardless of the scene
    // damage rect above -- it doesn't track its own screen position
    // against damage the way windows do, and it's cheap enough (a
    // single small sprite) that there's no real cost to always letting
    // it through.
    ugfx_clear_clip_rect(wm_surface());
    draw_cursor_at(mx, my); // also (re)establishes cursor_under for wm_render_cursor_move()

}

// ---- damage verification (debug) --------------------------------------
//
// **The bug class this exists for.** The compositor is correct only if
// everything that changes on screen is inside the damage rect, and
// nothing enforces that: damage is declared by hand from eight sites
// across three files, and a missed declaration produces stale pixels
// with no crash, no wrong return value and no failing assertion. Every
// rendering bug this project has had was that shape -- a window revealed
// by a raise, a cursor sprite overhanging its rect, an app drawing
// outside its own window.
//
// The check turns it into a loud one: render the frame the normal way,
// snapshot it, render the SAME frame with no damage limit, and compare.
// Any differing pixel is one the damage-limited path got wrong, and its
// coordinates say where to look. Off by default (it renders every frame
// twice and costs a full-screen buffer); `gui damage verify on` enables
// it -- see apps/wm/wm_debug.c.
static int first_frame = 1;
static int overlay_was_open;  // an overlay was up last frame -- see wm_render_frame()
static int prev_cursor_x = -1, prev_cursor_y = -1;
// The box the cursor was last DRAWN in, recorded beside the position
// because a themed shape's extent is not derivable from a position
// alone -- the theme, its hotspot and the size setting all move it, and
// any of the three can change between two frames.
static int prev_cursor_box_x, prev_cursor_box_y;
static int prev_cursor_box_w, prev_cursor_box_h;
static int verify_enabled;
static int verify_reported; // report each distinct failure once, not per frame

// Called by wm_run() when a GUI session begins, so the next frame is a
// full repaint even if the previous session left state behind.
void wm_render_reset(void) {
    first_frame = 1;
    overlay_was_open = 0;
    prev_cursor_x = prev_cursor_y = -1;
    prev_cursor_box_w = prev_cursor_box_h = 0;
}

void wm_damage_verify_set(int on) {
    verify_enabled = on ? 1 : 0;
    verify_reported = 0;
    if (!on) ugfx_verify_release(&g_wm_screen);
    wm_logf("wm: damage verification %s\n", on ? "ON (every frame rendered twice)" : "off");
}

int wm_damage_verify_enabled(void) { return verify_enabled; }

// The cursor's own damage. It is alpha-BLENDED, so it must always be
// composited over a freshly-drawn scene: if the pixels underneath
// aren't redrawn first, each frame blends the sprite over the previous
// frame's sprite and the anti-aliased edges creep steadily more opaque.
//
// That is why the cursor is a damage source like anything else rather
// than having its own save-the-pixels-underneath path beside the damage
// system. That parallel path is what produced the resize trail, and
// `gui damage verify on` then caught the self-compositing too --
// "80 px changed outside the damage rect, first at (641,360)" on frames
// where only the clock ticked, (641,360) being where the cursor sat.
//
// prev_cursor_* is "where the cursor was last actually DRAWN", and is
// recorded at the end of wm_render_frame() on every frame that draws the
// scene -- NOT here. It used to be updated in this function, which
// only runs on damage-limited frames, so a full-repaint frame moved the
// sprite without recording where it went; the next damage-limited frame
// then damaged a position the cursor had already left, and the pixels it
// was actually sitting on were never repainted. The residue is a second
// cursor left behind on screen.
//
// That stayed hidden while full-repaint frames were rare. Making the
// overlay fallback real (above) made them common and
// tools/damage_sweep.py found it immediately: "139 px changed outside
// the damage rect, first at (928,336)" -- 139 px being roughly a cursor
// sprite, and (928,336) exactly where the previous injected click had
// left it.
static void damage_cursor(int mx, int my) {
    // Generous: the sprite is 13x19 and the resize variants differ, so
    // a box comfortably covering any of them costs nothing meaningful
    // and removes a whole family of off-by-a-few-pixels questions.
    // Derived from the box save_cursor_under() actually uses, not from a
    // separately-chosen constant. It used to damage (x-1, y-1) sized
    // CURSOR_BOX_SIZE+2 while the sprite box is anchored at
    // (x-CURSOR_BOX_MARGIN, y-CURSOR_BOX_MARGIN) -- so the box's top row
    // and left column sat one pixel OUTSIDE the damage rect, and every
    // cursor move left a two-sided sliver behind. The extra 1px here is
    // slack, not the anchor: the anchor has to be the margin, or the two
    // drift apart again the moment either constant is retuned.
    // Derived from cursor_rect(), the same function save_cursor_under()
    // uses, with 1px of slack on each side. The PREVIOUS box is stored
    // rather than recomputed, because the shape under the old position
    // may not be the shape that is there now -- recomputing it with
    // today's kind and scale is how a theme or size change leaves the
    // last frame's larger sprite undamaged.
    if (prev_cursor_box_w > 0) {
        wm_damage_rect(prev_cursor_box_x - 1, prev_cursor_box_y - 1,
                        prev_cursor_box_w + 2, prev_cursor_box_h + 2);
    }
    int ox, oy, w, h;
    cursor_rect(resolve_cursor_kind(mx, my), mx, my, &ox, &oy, &w, &h);
    wm_damage_rect(ox - 1, oy - 1, w + 2, h + 2);
}

void wm_render_frame(int mx, int my) {
    compute_window_damage();

    // The FIRST frame of a GUI session is always a full repaint, never
    // damage-limited. wm_run() polls the debug console (and anything
    // else) before its first render, so an event arriving that early
    // reports damage and narrows the one frame that has to establish
    // the whole back buffer -- leaving everything outside it never
    // drawn at all. Caught by `gui damage verify on`: "51200 px changed
    // outside the damage rect, first at (0,0)", 51200 being exactly the
    // 1280x40 strip above a freshly-opened window.
    if (first_frame) {
        first_frame = 0;
        damage_reset();
    }
    // Only when something else already reported damage: with no damage
    // the frame is a full repaint anyway, and adding a rect here would
    // narrow it -- the same trap tray_init() hit (see wm_tray.c).
    if (damage_x1 > damage_x0) damage_cursor(mx, my);

    // Overlays (Start menu, context menu, file picker, confirm dialog)
    // draw OUTSIDE any window's rect and declare no damage of their own
    // -- the design note above calls that "falls back to a full-screen
    // repaint", and for a long time it was true by accident: an overlay
    // frame usually had nothing else reporting damage, so the frame was
    // unrestricted anyway.
    //
    // It stops being true the moment something else declares damage in
    // the SAME frame, and then the fallback silently inverts: the frame
    // becomes damage-limited, the overlay is clipped away, and whatever
    // was on screen before it stays there. Found by
    // tools/damage_sweep.py's random walk (seed 1, step 22): a click
    // that both raised a window and opened Notepad's file picker
    // reported "114932 px changed outside the damage rect, first at
    // (590,173)" -- the damage box was exactly the raised window's rect
    // and the picker was wholly outside it.
    //
    // So make the documented fallback actually hold: while an overlay is
    // up, discard the damage box and repaint the frame in full. That is
    // the correct-by-construction option rather than the fast one, and
    // it is what the compositor's scoped first cut always intended.
    // Giving each overlay a real damage rect of its own is the better
    // end state and needs geometry each of them doesn't expose yet --
    // see docs/roadmap.md's Milestone 12 entry.
    // ...and for one frame AFTER it closes, because the frame that
    // dismisses an overlay has already cleared its `_open` flag by the
    // time this runs, and nothing damages the region the overlay just
    // vacated. That one hid behind a coincidence too: the damage box on
    // such a frame is usually the full-width taskbar strip unioned with
    // the cursor, which covers most of a Start menu sitting just above
    // the taskbar. Dismissing it with a click low on the screen left
    // exactly the rows above that union stale -- "300 px changed
    // outside the damage rect, first at (4,448)", (4,448) being the
    // menu's own top-left corner and 300 being its top two rows.
    int overlay_now = start_menu_open || context_menu_open ||
                      file_picker_open || confirm_dialog_open;
    if (overlay_now || overlay_was_open) {
        damage_reset();
    }
    overlay_was_open = overlay_now;

    // Clip this pass to the accumulated damage region, if any was
    // reported. No damage this frame (menus, dialogs, the clock tick,
    // the first frame) means "unknown, be safe" -- full screen.
    int has_damage = damage_x1 > damage_x0;
    render_scene(mx, my, has_damage);

    if (verify_enabled && has_damage) {
        // Compare against an unrestricted render of the same frame.
        // Only meaningful when damage WAS reported -- an unrestricted
        // frame is trivially equal to itself.
        if (ugfx_verify_snapshot(&g_wm_screen)) {
            render_scene(mx, my, 0);
            struct ugfx_diff d;
            int diff = ugfx_verify_diff(&g_wm_screen, &d);
            if (diff && !verify_reported) {
                verify_reported = 1;
                // THE IDEMPOTENCE PROBE. A difference between the two
                // renders means one of exactly two things, and they need
                // opposite fixes:
                //
                //   (a) the damage-limited render missed a pixel that
                //       genuinely changed -- a real missed declaration,
                //       the bug this whole mode exists to find; or
                //   (b) render_scene() is not a pure function of the
                //       frame's state, so the two renders disagree about
                //       a scene that no damage rect could have covered.
                //
                // (b) is not hypothetical: the unrestricted pass calls
                // every window's on_draw(), while the damage-limited one
                // SKIPS windows outside the damage box (Phase 3 above),
                // so anything an on_draw() mutates happens a different
                // number of times in the two passes.
                //
                // Rendering the same unrestricted frame a THIRD time
                // separates them, in the same frame and under the same
                // load: if it reproduces its own output exactly, the
                // scene is stable and the diff above is a real (a). If
                // it does not, the comparison itself was measuring
                // nothing and (a) cannot be concluded from it.
                struct ugfx_diff again = { 0, -1, -1, 0, 0, 0, 0 };
                int probed = ugfx_verify_snapshot(&g_wm_screen);
                if (probed) {
                    render_scene(mx, my, 0);
                    ugfx_verify_diff(&g_wm_screen, &again);
                }
                // Which window the difference landed in, and whether
                // that window was inside this frame's damage box at
                // all. "63 px at (349,264)" says nothing on its own;
                // "inside Notepad, which this frame did not damage" is
                // the bug report.
                int owner = -1;
                int cx = (d.x0 + d.x1) / 2, cy = (d.y0 + d.y1) / 2;
                for (int i = window_count - 1; i >= 0; i--) {
                    if (windows[i].state == WIN_MINIMIZED) continue;
                    if (cx >= windows[i].x && cx < windows[i].x + windows[i].w &&
                        cy >= windows[i].y && cy < windows[i].y + windows[i].h) {
                        owner = i;
                        break;
                    }
                }
                // The cursor is the other half of the picture: its box
                // is damaged from prev_cursor_* and (mx,my), and
                // wm_render_cursor_move()'s cheap path draws the sprite
                // WITHOUT recording where it put it -- so a diff sitting
                // on the cursor with prev_cursor_* somewhere else is a
                // different bug from a diff in an app's own pixels.
                wm_logf("wm:   cursor now (%d,%d), prev drawn (%d,%d)\n",
                             mx, my, prev_cursor_x, prev_cursor_y);
                wm_logf("wm:   diff is in %s%s%s, damage-intersecting=%d\n",
                             owner < 0 ? "no window (desktop/taskbar/overlay)" : "window '",
                             owner < 0 ? "" : windows[owner].title,
                             owner < 0 ? "" : "'",
                             owner < 0 ? 0 : window_intersects_damage(&windows[owner]));
                wm_logf("wm: DAMAGE BUG -- %d px changed outside the damage rect, "
                             "first at (%d,%d); damage was (%d,%d %dx%d); "
                             "diff bbox (%d,%d %dx%d); %s\n",
                             diff, d.first_x, d.first_y, damage_x0, damage_y0,
                             damage_x1 - damage_x0, damage_y1 - damage_y0,
                             d.x0, d.y0, d.x1 - d.x0, d.y1 - d.y0,
                             !probed ? "probe unavailable"
                             : again.count == 0 ? "scene stable (real missed damage)"
                             : "SCENE UNSTABLE -- verdict void");
                if (probed && again.count) {
                    wm_logf("wm:   probe -- a repeated unrestricted render of the same "
                                 "frame differs from itself by %d px, first at (%d,%d), "
                                 "bbox (%d,%d %dx%d)\n",
                                 again.count, again.first_x, again.first_y,
                                 again.x0, again.y0, again.x1 - again.x0,
                                 again.y1 - again.y0);
                }
            } else if (!diff) {
                verify_reported = 0; // armed again for the next distinct failure
            }
            // The unrestricted render is left in the back buffer on
            // purpose: it is the CORRECT frame, so verification also
            // repairs what it caught rather than presenting the bug.
        }
    }

    // Record where the cursor was drawn, on EVERY frame -- damaged or
    // not. See damage_cursor()'s comment: this is the bookkeeping that
    // lets the next damage-limited frame erase the sprite from the place
    // it is genuinely sitting, and doing it only on damaged frames is
    // what left a second cursor behind.
    prev_cursor_x = mx;
    prev_cursor_y = my;
    cursor_rect(resolve_cursor_kind(mx, my), mx, my,
                 &prev_cursor_box_x, &prev_cursor_box_y,
                 &prev_cursor_box_w, &prev_cursor_box_h);

    ugfx_screen_present(&g_wm_screen);
    // The `rammeter` overlay does NOT move here. It is painted straight
    // at the framebuffer after the present, deliberately outside anything
    // composited -- which in ring 0 meant "after gfx_present()", and in
    // ring 3 would mean a second writer to a surface the compositor
    // believes it owns. It stays kernel-side; if it is ever wanted over a
    // ring-3 desktop it should become a normal overlay this compositor
    // draws, not a second hand reaching into the same framebuffer.
    damage_reset();
}

void wm_render_cursor_move(int mx, int my) {
    restore_cursor_under();
    draw_cursor_at(mx, my);

    // This path DREW the cursor, so it has to say where -- prev_cursor_*
    // is "where the sprite actually is", and the next damage-limited
    // frame erases it from there. Leaving it to wm_render_frame() alone
    // meant a cheap move stranded the sprite: the frame damaged the
    // position before the cheap move and the position after it, and
    // never the one in between, so the sprite sat there until something
    // else happened to repaint that region.
    //
    // Consecutive cheap moves hid it (each one restores what the last
    // saved), which is why this survived as "currently harmless" in
    // docs/roadmap.md. It needs a FULL frame to land while the cursor
    // has already moved on -- an injected event overrides the real mouse
    // for one iteration, so a `gui` test's cursor snaps back to the real
    // PS/2 position every other iteration and gets plenty of chances.
    // `gui damage verify on` reported it as 12x19 and 9x15 boxes -- the
    // sprite is 13x19 -- sitting exactly under the cursor, roughly once
    // in five randomised damage_hunt.py sweeps, labelled as whatever
    // interaction happened to be running.
    prev_cursor_x = mx;
    prev_cursor_y = my;
    cursor_rect(resolve_cursor_kind(mx, my), mx, my,
                 &prev_cursor_box_x, &prev_cursor_box_y,
                 &prev_cursor_box_w, &prev_cursor_box_h);

    ugfx_screen_present(&g_wm_screen);
}
