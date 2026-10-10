// A speaker test as a panel -- see uui_sndtest.h.
#include <stdio.h>
#include <string.h>
#include "ui/uui_sndtest.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uui_primitives.h"
#include "rt/sys.h"

#define PERIOD_MS 1000           // one chime a second
#define WINDOW_FRAMES (USND_RATE / 30)

static const char *const LABEL[UUI_SNDTEST_IDS] = { "Left", "Both", "Right" };
static const char *const WHERE[UUI_SNDTEST_IDS] = { "on the left", "on both sides", "on the right" };
static const char IDLE[] = "Press a side to start, and again to stop.";

static uint64_t now_ms(void) { return sys_monotonic_ns() / 1000000ull; }

// A meter: segments bottom to top, the last two warm, as a VU meter is.
static void draw_meter(struct ugfx_surface *s, const struct uui_custom *c) {
    const int *lit = c->state;
    int gap = 2, seg = (c->h - gap * (UUI_SNDTEST_SEGS - 1)) / UUI_SNDTEST_SEGS;
    if (seg < 1) return;
    for (int i = 0; i < UUI_SNDTEST_SEGS; i++) {
        uint32_t col = i >= *lit ? UTHEME_SEPARATOR
                     : i >= UUI_SNDTEST_SEGS - 2 ? utheme_current()->severity[UTHEME_SEV_WARNING]
                     : UTHEME_ACCENT;
        ugfx_fill_rect(s, c->x, c->y + c->h - (i + 1) * seg - i * gap, c->w, seg, col);
    }
}

// --- a speaker tile ------------------------------------------------------
//
// A push button, as uui_button is one -- armed on press, committed by a
// release over it or by Space/Enter -- that draws a speaker cabinet
// (a woofer under a tweeter) above its name, the S2 mockup's tile.

static void tile_natural(const void *w, int *ow, int *oh) {
    const struct uui_sndtest_tile *t = w;
    int ch = ugfx_char_h(), lw = ugfx_text_width(t->label) + 2 * ch;
    *ow = lw > ch * 6 ? lw : ch * 6;
    *oh = ch * 5;
}

static void tile_geometry(void *w, int x, int y, int ww, int hh) {
    struct uui_sndtest_tile *t = w;
    t->x = x; t->y = y; t->w = ww; t->h = hh;
}

static void tile_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_sndtest_tile *t = w;
    *x = t->x; *y = t->y; *ow = t->w; *oh = t->h;
}

static void cabinet(struct ugfx_surface *s, int cx, int top, int ch, uint32_t ink, uint32_t face) {
    int w = ch * 3 / 2, h = ch * 2, x = cx - w / 2, r = ch / 4;
    uui_fill_round_rect(s, x, top, w, h, r, ink);
    uui_fill_round_rect(s, x + 2, top + 2, w - 4, h - 4, r - 1 > 0 ? r - 1 : 1, face);
    int wy = top + h * 5 / 8, wr = w * 3 / 10;
    ugfx_fill_circle(s, cx, wy, wr, ink);
    ugfx_fill_circle(s, cx, wy, wr - 2, face);
    ugfx_fill_circle(s, cx, wy, wr / 3, ink);
    ugfx_fill_circle(s, cx, top + h / 5 + 1, ch / 6 + 1, ink);
}

static void tile_draw(struct ugfx_surface *s, const void *w) {
    const struct uui_sndtest_tile *t = w;
    enum uui_state st = t->pressed ? UUI_STATE_PRESSED : t->hovered ? UUI_STATE_HOVER : UUI_STATE_REST;
    uint32_t face = uui_state_bg(t->latched ? UTHEME_SELECTION : UTHEME_BUTTON_BG, st);
    uint32_t edge = t->latched ? UTHEME_ACCENT : UTHEME_BORDER;
    int ch = ugfx_char_h(), r = ch / 2, b = t->latched ? 2 : 1;
    uui_fill_round_rect(s, t->x, t->y, t->w, t->h, r, edge);
    uui_fill_round_rect(s, t->x + b, t->y + b, t->w - 2 * b, t->h - 2 * b, r - b, face);
    int top = t->y + (t->h - ch * 2 - ch - ch / 2) / 2;
    cabinet(s, t->x + t->w / 2, top, ch, UTHEME_TEXT, face);
    int lw = ugfx_text_width(t->label);
    ugfx_draw_string_clipped(s, t->x + (t->w - lw) / 2, top + ch * 2 + ch / 2, t->w, t->label,
                             UTHEME_TEXT, face);
    if (t->focused) uui_focus_ring(s, t->x + 3, t->y + 3, t->w - 6, t->h - 6);
}

static int tile_hit(const void *w, int cx, int cy) {
    const struct uui_sndtest_tile *t = w;
    return uui_hit(t->x, t->y, t->w, t->h, cx, cy);
}

static int tile_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_sndtest_tile *t = w;
    int hit = tile_hit(t, cx, cy);
    if (t->pressed == hit) return 0;
    t->pressed = hit;
    return hit;
}

static int tile_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_sndtest_tile *t = w;
    int hit = tile_hit(t, cx, cy);
    int *flag = buttons ? &t->pressed : &t->hovered;
    if (*flag == hit) return 0;
    *flag = hit;
    return 1;
}

static int tile_release(void *w, int cx, int cy) {
    (void)cx; (void)cy;
    struct uui_sndtest_tile *t = w;
    int was = t->pressed;   // dragged off, motion already disarmed it
    t->pressed = 0;
    if (was) t->clicked = 1;
    return was;
}

static int tile_key(void *w, int key, unsigned mods) {
    (void)mods;
    struct uui_sndtest_tile *t = w;
    if (key != ' ' && key != '\n' && key != '\r') return 0;
    t->clicked = 1;
    return 1;
}

static void tile_set_focused(void *w, int f) { ((struct uui_sndtest_tile *)w)->focused = f; }
static int tile_accepts_focus(const void *w) { (void)w; return 1; }

static const struct uui_widget_ops tile_ops = {
    .natural_size = tile_natural,
    .set_geometry = tile_geometry,
    .bounds = tile_bounds,
    .draw = tile_draw,
    .hit = tile_hit,
    .press = tile_press,
    .motion = tile_motion,
    .release = tile_release,
    .key = tile_key,
    .set_focused = tile_set_focused,
    .accepts_focus = tile_accepts_focus,
};

// --- the panel ------------------------------------------------------------

void uui_sndtest_init(struct uui_sndtest *t, int code_base) {
    memset(t, 0, sizeof *t);
    t->code_base = code_base;
    t->playing = -1;
    t->tile[0].label = LABEL[UUI_SNDTEST_LEFT];
    t->tile[1].label = LABEL[UUI_SNDTEST_RIGHT];
    uui_button_init(&t->both, 0, 0, 0, 0, LABEL[UUI_SNDTEST_BOTH], UTHEME_BUTTON_BG, UTHEME_TEXT,
                    code_base + UUI_SNDTEST_BOTH);
    t->both.outlined = 1;
    strlcpy(t->status, IDLE, sizeof t->status);
    uui_label_init(&t->status_l, t->status);
}

// [ pad | meter L | Left | Both | Right | meter R | pad ] over [ pad | status | pad ]:
// the pads take the slack on either side, which centres what is between,
// and Both sits in a column whose pads centre it on the tiles' height.
struct uui_item uui_sndtest_item(struct uui_sndtest *t) {
    int ch = ugfx_char_h();
    for (int k = 0; k < 2; k++)
        t->meter[k] = (struct uui_custom){ .w = ch / 2, .h = ch * 5, .draw = draw_meter,
                                           .state = &t->level[k] };
    for (int k = 0; k < 6; k++) t->pad[k] = (struct uui_custom){ .w = 0, .h = 0 };
    t->mid_it[0] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &t->pad[4], .flags = UUI_FILL_H };
    t->mid_it[1] = (struct uui_item){ .ops = &uui_button_ops, .widget = &t->both,
                                      .id = t->code_base + UUI_SNDTEST_BOTH, .name = "sndtest_both" };
    t->mid_it[2] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &t->pad[5], .flags = UUI_FILL_H };
    t->mid = (struct uui_layout){ .dir = UUI_COLUMN, .items = t->mid_it, .count = 3 };
    int n = 0;
    t->row_it[n++] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &t->pad[0], .flags = UUI_FILL_W };
    t->row_it[n++] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &t->meter[0], .name = "sndtest_meter_l" };
    t->row_it[n++] = (struct uui_item){ .ops = &tile_ops, .widget = &t->tile[0],
                                        .id = t->code_base + UUI_SNDTEST_LEFT, .name = "sndtest_left" };
    t->row_it[n++] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &t->mid, .flags = UUI_FILL_H };
    t->row_it[n++] = (struct uui_item){ .ops = &tile_ops, .widget = &t->tile[1],
                                        .id = t->code_base + UUI_SNDTEST_RIGHT, .name = "sndtest_right" };
    t->row_it[n++] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &t->meter[1], .name = "sndtest_meter_r" };
    t->row_it[n++] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &t->pad[1], .flags = UUI_FILL_W };
    t->row = (struct uui_layout){ .dir = UUI_ROW, .items = t->row_it, .count = n };
    n = 0;
    t->foot_it[n++] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &t->pad[2], .flags = UUI_FILL_W };
    t->foot_it[n++] = (struct uui_item){ .ops = &uui_label_ops, .widget = &t->status_l, .name = "sndtest_status" };
    t->foot_it[n++] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &t->pad[3], .flags = UUI_FILL_W };
    t->foot = (struct uui_layout){ .dir = UUI_ROW, .items = t->foot_it, .count = n };
    t->col_it[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &t->row, .flags = UUI_FILL_W };
    t->col_it[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &t->foot, .flags = UUI_FILL_W };
    t->col = (struct uui_layout){ .dir = UUI_COLUMN, .items = t->col_it, .count = 2 };
    return (struct uui_item){ .ops = &uui_layout_ops, .widget = &t->col, .flags = UUI_FILL_W };
}

int uui_sndtest_focusables(struct uui_sndtest *t, struct uui_focusable *out, int max) {
    int n = 0;
    if (n < max) out[n++] = (struct uui_focusable){ &t->tile[0], &tile_ops };
    if (n < max) out[n++] = (struct uui_focusable){ &t->both, &uui_button_ops };
    if (n < max) out[n++] = (struct uui_focusable){ &t->tile[1], &tile_ops };
    return n;
}

int uui_sndtest_playing(const struct uui_sndtest *t) { return t->playing >= 0; }

// The side being tested is LATCHED: the soft accent fill with an accent
// edge on a tile, the accent itself on Both.
static void mark(struct uui_sndtest *t) {
    t->tile[0].latched = t->playing == UUI_SNDTEST_LEFT;
    t->tile[1].latched = t->playing == UUI_SNDTEST_RIGHT;
    int both = t->playing == UUI_SNDTEST_BOTH;
    t->both.bg = both ? UTHEME_ACCENT : UTHEME_BUTTON_BG;
    t->both.fg = both ? UTHEME_ACCENT_TEXT : UTHEME_TEXT;
}

static void chime(struct uui_sndtest *t) {
    int l = t->playing == UUI_SNDTEST_RIGHT ? 0 : 256;
    int r = t->playing == UUI_SNDTEST_LEFT ? 0 : 256;
    usnd_voice_stop(t->voice);
    t->voice = usnd_voice_play(&t->clip, l, r);
    t->started_ms = now_ms();
}

void uui_sndtest_stop(struct uui_sndtest *t) {
    if (t->sink) {
        usnd_voice_stop(t->voice);
        if (t->have_clip) usnd_clip_free(&t->clip);
        usnd_shutdown();
    }
    t->sink = t->have_clip = 0;
    t->voice = USND_VOICE_NONE;
    t->playing = -1;
    t->level[0] = t->level[1] = 0;
    mark(t);
}

static void start(struct uui_sndtest *t, int side) {
    if (!t->sink) {
        int rc = usnd_init();
        if (rc != 0) {
            snprintf(t->status, sizeof t->status, "No sound: %s", usnd_last_error());
            return;
        }
        t->sink = 1;
        if (usnd_clip_load(UUI_SNDTEST_SOUND, &t->clip) != 0) {
            snprintf(t->status, sizeof t->status, "No test sound: %s", usnd_last_error());
            uui_sndtest_stop(t);
            return;
        }
        t->have_clip = 1;
    }
    t->playing = side;
    mark(t);
    chime(t);
    snprintf(t->status, sizeof t->status, "Playing %s. Press %s again to stop.", WHERE[side], LABEL[side]);
}

static void toggle(struct uui_sndtest *t, int side) {
    if (side == t->playing) {
        uui_sndtest_stop(t);
        strlcpy(t->status, IDLE, sizeof t->status);
    } else {
        start(t, side);
    }
}

int uui_sndtest_on_widget(struct uui_sndtest *t, int id) {
    int side = id - t->code_base;
    if (side != UUI_SNDTEST_LEFT && side != UUI_SNDTEST_RIGHT) return 0;
    struct uui_sndtest_tile *tile = &t->tile[side == UUI_SNDTEST_RIGHT];
    if (!tile->clicked) return 1;   // a press or a hover on the tile: only a redraw
    tile->clicked = 0;
    toggle(t, side);
    return 1;
}

int uui_sndtest_on_action(struct uui_sndtest *t, int code) {
    if (code != t->code_base + UUI_SNDTEST_BOTH) return 0;
    toggle(t, UUI_SNDTEST_BOTH);
    return 1;
}

// Segments lit for a peak: 6 dB a segment, the top one at full scale --
// a sample's highest set bit is its level to within 6 dB.
static int segs_for(uint32_t peak) {
    int bit = 31;
    while (bit > 0 && !(peak >> bit)) bit--;
    int lit = bit - (31 - UUI_SNDTEST_SEGS);
    return peak == 0 || lit < 0 ? 0 : lit > UUI_SNDTEST_SEGS ? UUI_SNDTEST_SEGS : lit;
}

int uui_sndtest_tick(struct uui_sndtest *t) {
    if (t->playing < 0) return 0;
    uint64_t now = now_ms();
    if (now - t->started_ms >= PERIOD_MS) chime(t);
    uint64_t at = (now - t->started_ms) * USND_RATE / 1000;
    uint32_t peak[2] = { 0, 0 };
    for (uint64_t f = at; f < at + WINDOW_FRAMES && f < t->clip.frames; f++)
        for (int k = 0; k < 2; k++) {
            int32_t v = t->clip.pcm[f * USND_CHANNELS + k];
            uint32_t a = v < 0 ? (uint32_t)-(int64_t)v : (uint32_t)v;
            if (a > peak[k]) peak[k] = a;
        }
    int was0 = t->level[0], was1 = t->level[1];
    t->level[0] = t->playing == UUI_SNDTEST_RIGHT ? 0 : segs_for(peak[0]);
    t->level[1] = t->playing == UUI_SNDTEST_LEFT ? 0 : segs_for(peak[1]);
    return t->level[0] != was0 || t->level[1] != was1;
}
