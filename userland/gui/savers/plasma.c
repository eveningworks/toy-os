// AN INTERFERENCE FIELD OF DRIFTING SINE WAVES. The demoscene one.
//
// **IT RENDERS COARSE AND FILLS BLOCKS, and that is not a shortcut to
// undo later.** A per-pixel effect at 1280x720 is 921,600 sine lookups
// and writes a frame, which under TCG does not hold a frame rate -- and
// a screensaver that stutters is worse than one that is blocky. So the
// field is evaluated on a grid of square cells and each is filled flat,
// which at the default cell of 8 is 1/64th the work and reads as
// deliberate rather than broken. The `detail` option is that cell, and
// its ends are a factor of SIXTEEN apart in cost.
//
// The math is api/fixed.h's, and its angles are in TURNS: FX_ONE is a
// full rotation, so the phases below advance by fractions of a turn and
// wrap by masking. No floating point, and none wanted -- fx_sin is a
// table lookup where sinf() would be a call per cell.
#include "ui/uapp.h"
#include "ui/ugfx.h"
#include "fixed.h"
#include "lib/usaver.h"
#include <string.h>

static int g_cell = 8;
// Turns per tick, as a numerator over 1200: 5 is FX_ONE/240, which is
// what this drifted at before the option existed.
static int g_speed = 5;
// Which palette() branch runs. A number rather than a string so the
// per-cell call stays a switch and not a strcmp.
enum { PAL_RAINBOW = 0, PAL_FIRE, PAL_OCEAN, PAL_GREY };
static int g_palette = PAL_RAINBOW;

static fx_t g_t;

// Four waves: two axis-aligned, one diagonal, one RADIAL about a centre
// that itself drifts. The radial term is what makes it look organic --
// three straight waves alone beat against each other into a repeating
// lattice, which is what this looked like before it had one.
//
// THE WAVELENGTHS ARE LONG ON PURPOSE: FX_ONE/512 over a 1280-wide
// screen is about two and a half turns across it, so the blobs are
// hundreds of pixels wide. The first version used FX_ONE/96, thirteen
// turns across, and produced tight polka dots.
static int field(int x, int y, fx_t t) {
    fx_t a = fx_sin((fx_t)x * (FX_ONE / 512) + t);
    fx_t b = fx_sin((fx_t)y * (FX_ONE / 384) - t);
    fx_t c = fx_sin((fx_t)(x + y) * (FX_ONE / 700) + (t << 1));
    // The centre drifts on two rates that do not divide each other, so
    // it wanders rather than orbiting.
    int cx = 640 + (int)((fx_sin(t) * 320) >> FX_SHIFT);
    int cy = 360 + (int)((fx_sin(t + FX_ONE / 5) * 220) >> FX_SHIFT);
    int dx = x - cx, dy = y - cy;
    // An approximate distance -- octagonal, which at this scale is
    // indistinguishable from a circle and costs no square root.
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int dist = (adx > ady) ? adx + ady / 2 : ady + adx / 2;
    fx_t d = fx_sin((fx_t)dist * (FX_ONE / 300) - (t << 1));

    fx_t sum = (a + b + c + d) / 4;
    int v = 128 + (int)((sum * 127) >> FX_SHIFT);
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

// THE PALETTE IS THREE SINES A THIRD OF A TURN APART, which is the
// standard way to get a smooth cycle through the hues without a hue
// conversion: every channel peaks somewhere different, so the field
// glides through colour instead of crossing a muddy grey between two
// saturated ends. The first version did exactly that, and read as red
// and blue dots on grey.
//
// THE OTHER THREE ARE THE SAME SINE WITH THE CHANNELS BROUGHT TOGETHER.
// Phases a third of a turn apart give the full hue circle; phases close
// together give a ramp through one part of it, which is what a named
// palette is. Fire is red into yellow, ocean blue into cyan, grey all
// three in step.
static uint32_t palette(int v) {
    fx_t a = (fx_t)v * (FX_ONE / 256);
    fx_t pg, pb;
    switch (g_palette) {
    case PAL_FIRE:  pg = FX_ONE / 12; pb = FX_ONE / 5; break;
    case PAL_OCEAN: pg = FX_ONE / 12; pb = -FX_ONE / 12; break;
    case PAL_GREY:  pg = 0; pb = 0; break;
    default:        pg = FX_ONE / 3; pb = 2 * (FX_ONE / 3); break;
    }
    int r = 128 + (int)((fx_sin(a) * 127) >> FX_SHIFT);
    int g = 128 + (int)((fx_sin(a + pg) * 127) >> FX_SHIFT);
    int b = 128 + (int)((fx_sin(a + pb) * 127) >> FX_SHIFT);
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    // FIRE AND OCEAN LEAN THE CHANNELS, or a ramp through neighbouring
    // phases is still mostly grey -- the three sines never separate far
    // enough on their own to read as one colour family.
    if (g_palette == PAL_FIRE)  { b = b / 3; }
    if (g_palette == PAL_OCEAN) { r = r / 3; }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    struct ugfx_surface *s = d->surface;
    for (int y = 0; y < s->h; y += g_cell) {
        int ch = (y + g_cell <= s->h) ? g_cell : s->h - y;
        for (int x = 0; x < s->w; x += g_cell) {
            int cw = (x + g_cell <= s->w) ? g_cell : s->w - x;
            // SAMPLED AT THE CELL'S CENTRE, not its corner: a corner
            // sample shifts the whole field half a cell up and left,
            // which is visible as the pattern crawling against its own
            // drift.
            ugfx_fill_rect(s, x, y, cw, ch,
                           palette(field(x + cw / 2, y + ch / 2, g_t)));
        }
    }
}

static int on_tick(struct uapp *a) {
    (void)a;
    g_t = (g_t + (fx_t)(FX_ONE * g_speed / 1200)) & (FX_ONE - 1);
    return 1;
}

static void on_open(struct uapp *a) { uapp_set_fullscreen(a, 1); }

static void load_options(void) {
    static struct usaver cfg;   // past the 2 KB ring-3 frame cap
    usaver_load("plasma", &cfg);
    const char *d = usaver_str(&cfg, "detail", "medium");
    if (!strcmp(d, "coarse")) g_cell = 16;
    else if (!strcmp(d, "fine")) g_cell = 4;
    g_speed = usaver_int(&cfg, "speed", g_speed);
    const char *p = usaver_str(&cfg, "palette", "rainbow");
    if (!strcmp(p, "fire")) g_palette = PAL_FIRE;
    else if (!strcmp(p, "ocean")) g_palette = PAL_OCEAN;
    else if (!strcmp(p, "grey")) g_palette = PAL_GREY;
}

int main(void) {
    load_options();
    struct uapp_desc desc = {
        .title = "Plasma",
        .app_id = "saver-plasma",
        .flags = UAPP_RESIZABLE,
        .w = 640, .h = 480,
        .tick_ms = 50,
        .on_open = on_open,
        .on_tick = on_tick,
        .on_draw = on_draw,
    };
    return uapp_run(&desc);
}
