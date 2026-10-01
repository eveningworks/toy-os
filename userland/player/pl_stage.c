// The Audio Player's stage: the cover, the colours the stage takes from
// it, and the spectrum drawn under it.
//
// A COVER IS ALWAYS THERE. A track with an embedded picture (ID3 APIC)
// shows it; one without -- every file this image ships -- gets a tile
// coloured from its TITLE, so the same track always looks the same and
// two tracks look different. The stage's ambient colours come from
// whichever it is (ui/uambient.h, the Image Viewer's).
#include "player/player_internal.h"
#include "lib/utags.h"
#include "lib/usnd.h"
#include <string.h>
#include <stdlib.h>

struct uambient g_amb;
uint8_t g_levels[STAGE_BANDS];

static struct uimg g_cover;      // full size: decoded, or generated at GEN_SIZE
static struct uimg g_scaled;     // ...at the size last asked for
static int g_scaled_size;

// Drawn LARGE and box-filtered down by uimg_scale(): a cover is shown at
// up to ~240 px, and a tile scaled UP from a smaller one is what made
// the first version's note jagged.
#define GEN_SIZE 512

static uint32_t hash(const char *s) {
    uint32_t h = 2166136261u;                 // FNV-1a
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

// Hue 0..359 at a fixed, rich saturation and lightness, in integers.
static uint32_t hue(int h, int light) {
    int sector = (h / 60) % 6, f = (h % 60) * 255 / 60;
    int hi = light, lo = light / 4, up = lo + (hi - lo) * f / 255, dn = hi - (hi - lo) * f / 255;
    int r, g, b;
    switch (sector) {
    case 0: r = hi; g = up; b = lo; break;
    case 1: r = dn; g = hi; b = lo; break;
    case 2: r = lo; g = hi; b = up; break;
    case 3: r = lo; g = dn; b = hi; break;
    case 4: r = up; g = lo; b = hi; break;
    default: r = hi; g = lo; b = dn; break;
    }
    return ugfx_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

// A disc with an anti-aliased rim: each edge pixel blended by how much
// of a 4x4 grid of sub-samples falls inside.
static void put_disc(struct uimg *im, int cx, int cy, int r, uint32_t c) {
    long rr = (long)r * r * 64;
    for (int y = cy - r - 1; y <= cy + r + 1; y++)
        for (int x = cx - r - 1; x <= cx + r + 1; x++) {
            if (x < 0 || y < 0 || x >= im->w || y >= im->h) continue;
            int n = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    long dx = (long)(x - cx) * 8 + sx * 2 - 3, dy = (long)(y - cy) * 8 + sy * 2 - 3;
                    if (dx * dx + dy * dy <= rr) n++;
                }
            if (n) im->px[y * im->w + x] = ugfx_blend(im->px[y * im->w + x], c, (uint8_t)(n * 255 / 16));
        }
}

// A diagonal gradient between two hues a third of the wheel apart, and
// a quaver drawn on it -- the shape every player's empty cover has.
static int generate(const char *title) {
    g_cover.px = malloc(GEN_SIZE * GEN_SIZE * sizeof *g_cover.px);
    if (!g_cover.px) return 0;
    g_cover.w = g_cover.h = GEN_SIZE;
    g_cover.has_alpha = 0;
    int h = (int)(hash(title) % 360);
    uint32_t a = hue(h, 150), b = hue((h + 120) % 360, 110);
    for (int y = 0; y < GEN_SIZE; y++)
        for (int x = 0; x < GEN_SIZE; x++)
            g_cover.px[y * GEN_SIZE + x] = ugfx_blend(a, b, (uint8_t)((x + y) * 255 / (2 * GEN_SIZE)));
    // The quaver, in quarters of the tile so it scales with GEN_SIZE.
    uint32_t ink = ugfx_rgb(250, 250, 252);
    int u = GEN_SIZE / 128;                   // the first version's unit
    put_disc(&g_cover, 50 * u, 84 * u, 12 * u, ink);
    put_disc(&g_cover, 86 * u, 76 * u, 12 * u, ink);
    for (int y = 34 * u; y <= 84 * u; y++)
        for (int x = 58 * u; x <= 62 * u + u - 1; x++) g_cover.px[y * GEN_SIZE + x] = ink;
    for (int y = 26 * u; y <= 76 * u; y++)
        for (int x = 94 * u; x <= 98 * u + u - 1; x++) g_cover.px[y * GEN_SIZE + x] = ink;
    for (int k = 0; k < 9 * u; k++)           // the beam joining the stems
        for (int x = 58 * u; x <= 98 * u + u - 1; x++) {
            int y = 34 * u - (x - 58 * u) * 8 / 40 + k - 8 * u;
            if (y >= 0) g_cover.px[y * GEN_SIZE + x] = ink;
        }
    return 1;
}

void stage_set_track(int i) {
    uimg_free(&g_cover);
    uimg_free(&g_scaled);
    g_scaled_size = 0;
    if (i < 0 || i >= g_track_count) { uambient_default(&g_amb); return; }
    char path[PL_PATH_MAX];
    pl_path(i, path, sizeof path);
    struct utags t;
    int have = 0;
    if (utags_read(path, &t, 1) & UTAGS_ART)
        have = uimg_decode(t.art, t.art_len, &g_cover) == 0;
    utags_free(&t);
    if (!have) generate(g_tracks[i].title);
    uambient_from(&g_amb, &g_cover);
}

const struct uimg *stage_cover(int size) {
    if (!g_cover.px || size <= 0) return 0;
    if (g_scaled_size != size) {
        uimg_free(&g_scaled);
        g_scaled_size = 0;
        if (uimg_scale(&g_cover, size, size, &g_scaled) != 0) return 0;
        g_scaled_size = size;
    }
    return &g_scaled;
}

// --- the spectrum ----------------------------------------------------------
//
// SIXTEEN GOERTZEL FILTERS, not an FFT: a filter per band is all a bar
// display needs, and in integers -- ring 3 has no floating point -- it
// is a multiply and two adds per sample. The bands are log-spaced from
// 50 Hz to 14 kHz; each coefficient is 2*cos(2*pi*f/48000) in Q30.
#define SPEC_FRAMES 1024
static const int64_t COEF[STAGE_BANDS] = {
    2147437652, 2147386149, 2147276974, 2147045556,
    2146555031, 2145515356, 2143312046, 2138644037,
    2128760152, 2107858887, 2063778380, 1971345251,
    1779886931, 1393715779, 659390881, -555809667,
};

// log2 in 1/16ths, from the top set bit and the four below it.
static int log2_16(uint64_t v) {
    if (!v) return 0;
    int b = 63;
    while (!(v >> b)) b--;
    int frac = b >= 4 ? (int)((v >> (b - 4)) & 15) : (int)((v << (4 - b)) & 15);
    return b * 16 + frac;
}

int stage_spectrum_tick(int playing) {
    static int16_t pcm[SPEC_FRAMES * USND_CHANNELS];
    int moved = 0;
    long got = playing ? usnd_peek(pcm, SPEC_FRAMES) : 0;
    // THE MEAN COMES OUT FIRST: near 2.0 the low bands' recurrence grows
    // a DC offset quadratically, and 1024 samples of it overflow int64.
    // Without it a band's state stays under A*N/sin(w), ~1.3e9 at 50 Hz.
    int64_t mean = 0;
    for (long i = 0; i < got; i++) mean += ((int64_t)pcm[2 * i] + pcm[2 * i + 1]) >> 1;
    if (got) mean /= got;
    for (int k = 0; k < STAGE_BANDS; k++) {
        int level = 0;
        if (got == SPEC_FRAMES) {
            int64_t s1 = 0, s2 = 0;
            for (int i = 0; i < SPEC_FRAMES; i++) {
                int64_t x = ((((int64_t)pcm[2 * i] + pcm[2 * i + 1]) >> 1) - mean) >> 3;
                int64_t s0 = x + ((COEF[k] * s1) >> 30) - s2;
                s2 = s1;
                s1 = s0;
            }
            int64_t p = s1 * s1 + s2 * s2 - ((COEF[k] * s1) >> 30) * s2;
            // MEASURED on first-boot.mid as QEMU captured it: bands sit
            // between 2^9 and 2^34, falling toward the treble as music
            // does. The bar spans 2^16..2^36, with half a power of two of
            // lift per band so the top end is not always empty.
            level = (log2_16(p > 0 ? (uint64_t)p : 0) + k * 8 - 16 * 16) * 255 / (20 * 16);
            if (level < 0) level = 0;
            if (level > 255) level = 255;
        }
        // Up at once, down eased: a bar that falls reads as music, one
        // that blinks reads as noise.
        int now = g_levels[k];
        int next = level >= now ? level : now - (now - level + 3) / 4;
        if (next != now) { g_levels[k] = (uint8_t)next; moved = 1; }
    }
    return moved;
}
