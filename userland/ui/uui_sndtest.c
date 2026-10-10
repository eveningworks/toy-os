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

void uui_sndtest_init(struct uui_sndtest *t, int code_base) {
    memset(t, 0, sizeof *t);
    t->code_base = code_base;
    t->playing = -1;
    for (int i = 0; i < UUI_SNDTEST_IDS; i++) {
        uui_button_init(&t->btn[i], 0, 0, 0, 0, LABEL[i], UTHEME_BUTTON_BG, UTHEME_TEXT, code_base + i);
        t->btn[i].outlined = 1;
    }
    strlcpy(t->status, IDLE, sizeof t->status);
    uui_label_init(&t->status_l, t->status);
}

struct uui_item uui_sndtest_item(struct uui_sndtest *t) {
    int ch = ugfx_char_h();
    for (int k = 0; k < 2; k++)
        t->meter[k] = (struct uui_custom){ .w = ch / 2, .h = ch * 2, .draw = draw_meter,
                                           .state = &t->level[k] };
    static const char *const NAME[UUI_SNDTEST_IDS] = { "sndtest_left", "sndtest_both", "sndtest_right" };
    int n = 0;
    t->row_it[n++] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &t->meter[0], .name = "sndtest_meter_l" };
    for (int i = 0; i < UUI_SNDTEST_IDS; i++)
        t->row_it[n++] = (struct uui_item){ .ops = &uui_button_ops, .widget = &t->btn[i],
                                            .id = t->code_base + i, .name = NAME[i] };
    t->row_it[n++] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &t->meter[1], .name = "sndtest_meter_r" };
    t->row = (struct uui_layout){ .dir = UUI_ROW, .items = t->row_it, .count = n };
    t->col_it[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &t->row };
    t->col_it[1] = (struct uui_item){ .ops = &uui_label_ops, .widget = &t->status_l,
                                      .flags = UUI_FILL_W, .name = "sndtest_status" };
    t->col = (struct uui_layout){ .dir = UUI_COLUMN, .items = t->col_it, .count = 2 };
    return (struct uui_item){ .ops = &uui_layout_ops, .widget = &t->col, .flags = UUI_FILL_W };
}

int uui_sndtest_focusables(struct uui_sndtest *t, struct uui_focusable *out, int max) {
    int n = 0;
    for (int i = 0; i < UUI_SNDTEST_IDS && n < max; i++)
        out[n++] = (struct uui_focusable){ &t->btn[i], &uui_button_ops };
    return n;
}

int uui_sndtest_playing(const struct uui_sndtest *t) { return t->playing >= 0; }

// The side being tested wears the accent, as a latched control does.
static void mark(struct uui_sndtest *t) {
    for (int i = 0; i < UUI_SNDTEST_IDS; i++) {
        t->btn[i].bg = i == t->playing ? UTHEME_ACCENT : UTHEME_BUTTON_BG;
        t->btn[i].fg = i == t->playing ? UTHEME_ACCENT_TEXT : UTHEME_TEXT;
    }
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

int uui_sndtest_on_action(struct uui_sndtest *t, int code) {
    int side = code - t->code_base;
    if (side < 0 || side >= UUI_SNDTEST_IDS) return 0;
    if (side == t->playing) {
        uui_sndtest_stop(t);
        strlcpy(t->status, IDLE, sizeof t->status);
    } else {
        start(t, side);
    }
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
