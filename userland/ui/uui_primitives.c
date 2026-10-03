// Interaction states and the button painter. Split out of uui.c --
// see ui/uui_primitives.h.
#include "ui/uui_primitives.h"
#include "ui/utheme.h"

#define UUI_PRESSED_NUDGE 1

// Wash strengths out of 255, carried over from apps/ui/ui_primitives.c
// unchanged so a client's controls feel identical to the desktop's.
// Deliberately gentle: the guidelines want feedback that is
// unmistakable when you look for it and invisible when you don't -- a
// hover that shouts is worse than none, because the cursor is over
// SOMETHING at all times.
#define UUI_HOVER_ALPHA    22
#define UUI_PRESSED_ALPHA  46
#define UUI_DISABLED_ALPHA 110

// Which way to shift, read from the control's own brightness rather
// than assumed. A light control has to go DARKER to register; only a
// dark one goes lighter. See uui.h's header comment for the bug that
// this exists to prevent.
static uint32_t shift_from(uint32_t base, uint8_t alpha) {
    uint32_t toward = (ugfx_luminance(base) > 128) ? ugfx_rgb(0, 0, 0)
                                                    : ugfx_rgb(255, 255, 255);
    return ugfx_blend(base, toward, alpha);
}

uint32_t uui_state_bg(uint32_t base, enum uui_state state) {
    switch (state) {
    case UUI_STATE_HOVER:    return shift_from(base, UUI_HOVER_ALPHA);
    case UUI_STATE_PRESSED:  return shift_from(base, UUI_PRESSED_ALPHA);
    case UUI_STATE_DISABLED: return ugfx_blend(base, UTHEME_PANEL_BG,
                                                UUI_DISABLED_ALPHA);
    case UUI_STATE_REST:
    default:                 return base;
    }
}

int uui_hit(int x, int y, int w, int h, int px, int py) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

void uui_button_draw(struct ugfx_surface *s, int x, int y, int w, int h,
                      const char *label, uint32_t bg, uint32_t fg,
                      enum uui_state state) {
    uint32_t fill = uui_state_bg(bg, state);
    ugfx_fill_rect(s, x, y, w, h, fill);
    uui_button_draw_label(s, x, y, w, h, label, fg, fill, state);
}

void uui_button_draw_label(struct ugfx_surface *s, int x, int y, int w, int h,
                           const char *label, uint32_t fg, uint32_t fill,
                           enum uui_state state) {
    if (!label) return;

    int lx = x + (w - ugfx_text_width(label)) / 2;
    int ly = y + (h - ugfx_char_h()) / 2;
    // The nudge is what makes a press feel physical; the darker fill on
    // its own reads as a colour change.
    if (state == UUI_STATE_PRESSED) { lx += UUI_PRESSED_NUDGE; ly += UUI_PRESSED_NUDGE; }
    // A disabled button's LABEL fades as well as its face -- the face
    // alone moved a few levels and read as live (uui_checkbox fades its
    // label the same way).
    if (state == UUI_STATE_DISABLED) fg = uui_state_bg(fg, UUI_STATE_DISABLED);
    ugfx_draw_string_clipped(s, lx, ly, w, label, fg, fill);
}

void uui_focus_ring(struct ugfx_surface *s, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    ugfx_draw_rect(s, x, y, w, h, UTHEME_ACCENT);
}

// --- the rounded rect --------------------------------------------------
//
// THE SHAPE, in one rasteriser for every caller. It lived in
// uui_scrollbar.c until the taskbar's tray grew a pressed pill and
// became its second real caller.

// Coverage of pixel (px, py) -- counted inward from a corner's outer
// edge, 0 being the outermost row/column -- by a disc of radius `r`
// centred on the arc centre (r, r), in 0..255. Sixteen sub-samples per
// pixel, the same arithmetic wm_render.c's window corners use; r is a
// handful of pixels, so a corner is a few hundred compares.
static uint8_t arc_coverage(int r, int px, int py) {
    int in = 0;
    for (int sy = 0; sy < 4; sy++) {
        for (int sx = 0; sx < 4; sx++) {
            int cx = 8 * px + 2 * sx + 1 - 8 * r;
            int cy = 8 * py + 2 * sy + 1 - 8 * r;
            if (cx * cx + cy * cy <= 64 * r * r) in++;
        }
    }
    return (uint8_t)(in * 255 / 16);
}

// A requested radius resolved against the rect it has to fit in. Half
// the SHORT axis is the ceiling, which is what makes UUI_CAPSULE and
// "any radius too big for this rect" the same answer -- and what lets
// one function round a vertical thumb and a horizontal one without
// knowing which it has.
static int clamp_radius(int req, int w, int h) {
    int max = (w < h ? w : h) / 2;
    if (req < 0 || req > max) return max;   // UUI_CAPSULE, or clamped
    return req;
}

// Coverage of corner pixel (px, py) by a disc of radius `rad` centred on
// the arc centre (r, r) -- arc_coverage() with the disc free to be the
// inner edge of a 1px frame.
static uint8_t disc_coverage(int r, int rad, int px, int py) {
    if (rad <= 0) return 0;
    int in = 0;
    for (int sy = 0; sy < 4; sy++) {
        for (int sx = 0; sx < 4; sx++) {
            int cx = 8 * px + 2 * sx + 1 - 8 * r;
            int cy = 8 * py + 2 * sy + 1 - 8 * r;
            if (cx * cx + cy * cy <= 64 * rad * rad) in++;
        }
    }
    return (uint8_t)(in * 255 / 16);
}

void uui_glass_round_rect(struct ugfx_surface *s, int x, int y, int w, int h,
                          int radius, uint32_t c, uint8_t fill_a, uint8_t edge_a) {
    if (w <= 2 || h <= 2) return;
    int r = clamp_radius(radius, w, h);
    for (int j = 0; j < h; j++) {
        int cy = j < r ? j : (j >= h - r ? h - 1 - j : -1);
        if (cy < 0 && j != 0 && j != h - 1) {
            // A straight row: an edge pixel, a constant run, an edge pixel.
            ugfx_blend_pixel(s, x, y + j, c, edge_a);
            ugfx_blend_hspan(s, x + 1, y + j, w - 2, c, 0, fill_a);
            ugfx_blend_pixel(s, x + w - 1, y + j, c, edge_a);
            continue;
        }
        for (int i = 0; i < w; i++) {
            int cx = i < r ? i : (i >= w - r ? w - 1 - i : -1);
            int outer, inner;
            if (cx >= 0 && cy >= 0) {
                outer = disc_coverage(r, r, cx, cy);
                inner = disc_coverage(r, r - 1, cx, cy);
            } else {
                outer = 255;
                inner = (i == 0 || j == 0 || i == w - 1 || j == h - 1) ? 0 : 255;
            }
            int a = (fill_a * inner + edge_a * (outer - inner)) / 255;
            if (a > 0) ugfx_blend_pixel(s, x + i, y + j, c, (uint8_t)a);
        }
    }
}

static inline uint32_t mix(uint32_t under, uint32_t over, unsigned a) {
    unsigned r = (((under >> 16) & 0xFF) * (255 - a) + ((over >> 16) & 0xFF) * a + 127) / 255;
    unsigned g = (((under >> 8) & 0xFF) * (255 - a) + ((over >> 8) & 0xFF) * a + 127) / 255;
    unsigned b = ((under & 0xFF) * (255 - a) + (over & 0xFF) * a + 127) / 255;
    return r << 16 | g << 8 | b;
}

void uui_backdrop_round_rect(struct ugfx_surface *s, int x, int y, int w, int h,
                             int radius, const uint32_t *backdrop, uint32_t c,
                             uint8_t tint_a) {
    if (!s || !s->pixels || w <= 0 || h <= 0) return;
    int r = clamp_radius(radius, w, h);
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    if (s->clip_active) {
        if (x0 < s->clip_x0) x0 = s->clip_x0;
        if (y0 < s->clip_y0) y0 = s->clip_y0;
        if (x1 > s->clip_x1) x1 = s->clip_x1;
        if (y1 > s->clip_y1) y1 = s->clip_y1;
    }
    if (x1 <= x0 || y1 <= y0) return;
    uint32_t stride = (uint32_t)s->w;
    for (int py = y0; py < y1; py++) {
        int j = py - y;
        int cy = j < r ? j : (j >= h - r ? h - 1 - j : -1);
        uint32_t *row = s->pixels + (uint32_t)py * stride;
        const uint32_t *brow = backdrop ? backdrop + (uint32_t)py * stride : row;
        for (int px = x0; px < x1; px++) {
            int i = px - x;
            uint32_t v = mix(brow[px], c, tint_a);
            int cx = i < r ? i : (i >= w - r ? w - 1 - i : -1);
            if (cx >= 0 && cy >= 0) {
                uint8_t cov = arc_coverage(r, cx, cy);
                if (cov < 255) v = mix(row[px], v, cov);
            }
            row[px] = v;
        }
    }
    ugfx_mark_dirty_rect(s, x0, y0, x1 - x0, y1 - y0);
}

void uui_fill_round_rect(struct ugfx_surface *s, int x, int y, int w, int h,
                         int radius, uint32_t c) {
    if (w <= 0 || h <= 0) return;
    int r = clamp_radius(radius, w, h);
    if (r < 1) { ugfx_fill_rect(s, x, y, w, h, c); return; }

    ugfx_fill_rect(s, x, y + r, w, h - 2 * r, c);          // the waist
    ugfx_fill_rect(s, x + r, y, w - 2 * r, r, c);          // between the top corners
    ugfx_fill_rect(s, x + r, y + h - r, w - 2 * r, r, c);  // and the bottom ones

    for (int q = 0; q < 4; q++) {
        for (int py = 0; py < r; py++) {
            for (int px = 0; px < r; px++) {
                uint8_t cov = arc_coverage(r, px, py);
                if (!cov) continue;
                int sx = (q & 1) ? x + w - 1 - px : x + px;
                int sy = (q & 2) ? y + h - 1 - py : y + py;
                ugfx_blend_pixel(s, sx, sy, c, cov);
            }
        }
    }
}
