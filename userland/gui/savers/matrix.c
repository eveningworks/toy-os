// GLYPHS FALLING DOWN THE SCREEN. The one everybody recognises.
//
// IT DRAWS THROUGH THE SESSION FONT, so it follows whatever face and
// size the desktop is set to -- which is the reason this one is worth
// having beyond the look: a saver made of text costs one glyph per
// cell rather than work per pixel, and that is what lets it hold a
// frame rate at 1280x720 under emulation where a per-pixel effect
// cannot.
//
// One column per character cell. A column is either falling (a head at
// some row, leaving a trail behind it) or waiting, and it restarts from
// the top at a random moment after it leaves the bottom -- so the
// columns drift out of step with each other on their own rather than
// needing a phase table.
#include "ui/uapp.h"
#include "ui/ugfx.h"
#include "lib/usaver.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define COLS_MAX 256

static int g_trail = 14;     // cells behind the head that still glow
// Rows a column falls per tick, as a range: the two ends differ so the
// columns drift out of step on their own (see seed()).
static int g_fall_min = 1, g_fall_max = 2;
// The trail's colour, as a per-channel weight. Green is the identity.
static int g_ink_r = 0, g_ink_g = 255, g_ink_b = 0;

static struct {
    int head;                // row the bright cell is on, < 0 while waiting
    int speed;               // rows per tick
    int wait;                // ticks before this column starts again
} g_col[COLS_MAX];
static int g_cols, g_rows, g_seeded;
// The surface the grid was built for. A saver opens at its descriptor's
// size and is resized to the screen a frame later, so a grid computed
// once covers a 640x480 corner of a 1280x720 screen -- which is what
// this did, and it looks exactly like a saver that only half works.
static int g_for_w, g_for_h;

// THE GLYPHS ARE PRINTABLE ASCII, not a character set nobody has: the
// session font is whatever the user chose, and a saver that reached for
// a codepoint outside the 101 glyphs this build draws would show boxes.
static char glyph(void) { return (char)('!' + (rand() % 94)); }

static void seed(int w, int h) {
    if (!g_seeded) srand((unsigned)time(0));
    g_for_w = w; g_for_h = h;
    int cw = ugfx_char_w(), ch = ugfx_char_h();
    // text-measure-ok: on_draw() selects the mono family; this is a real grid.
    g_cols = cw > 0 ? w / cw : 0;
    g_rows = ch > 0 ? h / ch : 0;
    if (g_cols > COLS_MAX) g_cols = COLS_MAX;
    for (int i = 0; i < g_cols; i++) {
        g_col[i].head = -1;
        g_col[i].wait = rand() % 120;
        g_col[i].speed = g_fall_min + (rand() % (g_fall_max - g_fall_min + 1));
    }
    g_seeded = 1;
}

// **A GRID OF FALLING GLYPHS IS A GRID, so this draws in the MONOSPACE
// family.** Its columns are a width divided by a cell, which is only
// true of a fixed advance -- with a proportional interface face the
// columns would drift apart and the rain would lean. A saver owns the
// whole screen and has no chrome, so the selection is made once here
// and never put back.
static void on_draw(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    struct ugfx_surface *s = d->surface;
    ugfx_set_font(ugfx_font_mono(UGFX_FONT_REGULAR));
    if (!g_seeded || s->w != g_for_w || s->h != g_for_h) seed(s->w, s->h);
    ugfx_fill_rect(s, 0, 0, s->w, s->h, 0x000000);
    int cw = ugfx_char_w(), ch = ugfx_char_h();
    if (cw <= 0 || ch <= 0) return;

    for (int c = 0; c < g_cols; c++) {
        if (g_col[c].head < 0) continue;
        for (int t = 0; t < g_trail; t++) {
            int row = g_col[c].head - t;
            if (row < 0 || row >= g_rows) continue;
            // THE HEAD IS WHITE AND THE TRAIL FADES, which is the whole
            // illusion: a column of one colour reads as a line of text
            // scrolling, not as something falling.
            uint32_t col;
            if (t == 0) col = 0xE8FFE8;
            else {
                int g = 255 - (t * 255) / g_trail;
                if (g < 24) g = 24;
                col = (uint32_t)(g * g_ink_r / 255) << 16 |
                      (uint32_t)(g * g_ink_g / 255) << 8 |
                      (uint32_t)(g * g_ink_b / 255);
            }
            // text-measure-ok: mono family, selected above -- a cell grid by design.
        ugfx_draw_char(s, c * cw, row * ch, glyph(), col, 0x000000);
        }
    }
}

static int on_tick(struct uapp *a) {
    (void)a;
    if (!g_seeded) return 1;
    for (int c = 0; c < g_cols; c++) {
        if (g_col[c].head < 0) {
            if (--g_col[c].wait <= 0) {
                g_col[c].head = 0;
                g_col[c].speed = g_fall_min +
                                 (rand() % (g_fall_max - g_fall_min + 1));
            }
            continue;
        }
        g_col[c].head += g_col[c].speed;
        if (g_col[c].head - g_trail > g_rows) {
            g_col[c].head = -1;
            g_col[c].wait = 10 + (rand() % 90);
        }
    }
    return 1;
}

static void on_open(struct uapp *a) { uapp_set_fullscreen(a, 1); }

static void load_options(void) {
    static struct usaver cfg;   // past the 2 KB ring-3 frame cap
    usaver_load("matrix", &cfg);
    g_trail = usaver_int(&cfg, "trail", g_trail);
    const char *sp = usaver_str(&cfg, "speed", "normal");
    if (!strcmp(sp, "slow"))      { g_fall_min = 1; g_fall_max = 1; }
    else if (!strcmp(sp, "fast")) { g_fall_min = 2; g_fall_max = 4; }
    const char *ink = usaver_str(&cfg, "colour", "green");
    if (!strcmp(ink, "amber")) { g_ink_r = 255; g_ink_g = 176; g_ink_b = 0; }
    else if (!strcmp(ink, "ice")) { g_ink_r = 128; g_ink_g = 200; g_ink_b = 255; }
    else if (!strcmp(ink, "white")) { g_ink_r = 255; g_ink_g = 255; g_ink_b = 255; }
}

int main(void) {
    load_options();
    struct uapp_desc desc = {
        .title = "Matrix",
        .app_id = "saver-matrix",
        .flags = UAPP_RESIZABLE,
        .w = 640, .h = 480,
        .tick_ms = 60,
        .on_open = on_open,
        .on_tick = on_tick,
        .on_draw = on_draw,
    };
    return uapp_run(&desc);
}
