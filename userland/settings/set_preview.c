// The pictures in a gallery's cards (Widget=gallery), chosen by the
// setting's `Preview=` word. A TABLE of painters, so the wallpaper picker
// is one more row. Each painter draws choice `i` of the slot it is handed.
#include "settings_internal.h"
#include "lib/ucursor.h"
#include "lib/usetting_schema.h"
#include "lib/usetting_text.h"
#include "lib/ulivewall.h"
#include "lib/uimg.h"
#include "lib/uvid.h"
#include <stdlib.h>

// --- cursor: five shapes of the theme, on a light half and a dark half --

// What a theme is recognised by: the pointer, the I-beam, the link hand,
// busy, a resize -- KDE's Cursors page samples the same handful.
static const char *const CURSOR_SAMPLES[] = { "arrow", "text", "hand", "wait", "resize-diag" };
#define CURSOR_SAMPLE_N ((int)(sizeof CURSOR_SAMPLES / sizeof CURSOR_SAMPLES[0]))
// Themes cached per page; a gallery past this many draws them bare.
#define PREVIEW_THEMES 16

// Decoded ONCE per page, not per frame: an image theme is five QOI
// decodes, and the page repaints on every hover. Freed by preview_reset().
static struct cursor_shape g_pv_shape[PREVIEW_THEMES][CURSOR_SAMPLE_N];
static char g_pv_theme[PREVIEW_THEMES][SETTING_ABI_VALUE_MAX];
static int g_pv_loaded[PREVIEW_THEMES];

static void pictures_reset(void);

void preview_reset(void) {
    pictures_reset();
    for (int t = 0; t < PREVIEW_THEMES; t++) {
        for (int k = 0; k < CURSOR_SAMPLE_N; k++) ucursor_release(&g_pv_shape[t][k]);
        g_pv_loaded[t] = 0;
        g_pv_theme[t][0] = '\0';
    }
}

static void load_theme(int t, const char *theme) {
    for (int k = 0; k < CURSOR_SAMPLE_N; k++) {
        ucursor_release(&g_pv_shape[t][k]);   // parse overwrites `px`: free it first
        ucursor_load(theme, CURSOR_SAMPLES[k], 1, &g_pv_shape[t][k]);
    }
    strlcpy(g_pv_theme[t], theme, sizeof g_pv_theme[t]);
    g_pv_loaded[t] = 1;
}

// The desktop's own colour behind the lower half (desktop.c's DESKTOP_BG):
// a set must read on a window AND on the desktop, and a wallpaper sample
// would make the card depend on which picture is chosen.
#define PREVIEW_DARK ugfx_rgb(24, 60, 90)

static void cursor_tile(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    const struct slot *sl = ctx;
    int r = ugfx_char_h() / 3, half = h / 2;
    uui_fill_round_rect(s, x, y, w, h, r, PREVIEW_DARK);
    uui_fill_round_rect(s, x, y, w, half + r, r, UTHEME_WINDOW_BG);
    ugfx_fill_rect(s, x, y + half - r, w, r, UTHEME_WINDOW_BG);   // square the seam
    ugfx_fill_rect(s, x, y + half, w, r, PREVIEW_DARK);

    if (i < 0 || i >= PREVIEW_THEMES || i >= sl->choice_count) return;
    if (!g_pv_loaded[i] || strcmp(g_pv_theme[i], sl->choice_raw[i]) != 0)
        load_theme(i, sl->choice_raw[i]);

    // Each shape centred in its fifth of the tile, straddling the seam.
    for (int k = 0; k < CURSOR_SAMPLE_N; k++) {
        const struct cursor_shape *c = &g_pv_shape[i][k];
        if (!c->loaded) continue;
        int sx = x + (2 * k + 1) * w / (2 * CURSOR_SAMPLE_N) - c->w / 2;
        int sy = y + half - c->h / 2;
        for (int dy = 0; dy < c->h; dy++)
            for (int dx = 0; dx < c->w; dx++) {
                uint32_t p = ucursor_pixel(c, dx, dy, 1, 0xFFFFFF);
                if (p >> 24) ugfx_blend_pixel(s, sx + dx, sy + dy, p & 0xFFFFFF, (uint8_t)(p >> 24));
            }
    }
}

// --- wallpaper tiles: drawn ONCE per page, then blitted ----------------
//
// A gallery repaints on every hover, so a tile drawn per paint was a
// decode per hover -- and a live effect rendered afresh each time came out
// different (Fireflies and Ember are seeded at random), so the tiles
// twitched under the pointer. Cached per choice and per tile size.

#define PREVIEW_TILES 16
struct tile_cache {
    struct uimg img[PREVIEW_TILES];
    char name[PREVIEW_TILES][SETTING_ABI_VALUE_MAX];
};
static struct tile_cache g_pv_pic, g_pv_live;

static void cache_reset(struct tile_cache *c) {
    for (int i = 0; i < PREVIEW_TILES; i++) {
        uimg_free(&c->img[i]);
        c->name[i][0] = '\0';
    }
}

static struct uimg g_mon;
static char g_mon_name[SETTING_ABI_VALUE_MAX];

static void pictures_reset(void) {
    cache_reset(&g_pv_pic);
    cache_reset(&g_pv_live);
    uimg_free(&g_mon);
    g_mon_name[0] = '\0';
}

// `dir/stem.<ext>` for the first extension that decodes, scaled to COVER
// w x h -- cropped the way "Fill the screen" crops, not squashed.
static int load_cover(const char *dir, const char *stem, int w, int h, struct uimg *out) {
    static const char *const EXT[] = { "jpg", "gif", "png", "qoi" };
    struct uimg full;
    memset(&full, 0, sizeof full);
    char path[128];
    for (unsigned e = 0; e < sizeof EXT / sizeof EXT[0]; e++) {
        snprintf(path, sizeof path, "%s/%s.%s", dir, stem, EXT[e]);
        if (uimg_load(path, &full) == 0) break;
    }
    // A VIDEO stands for itself by a frame a second in.
    for (int e = 0; !full.px && LIVEWALL_VIDEO_EXT[e]; e++) {
        snprintf(path, sizeof path, "%s/%s.%s", dir, stem, LIVEWALL_VIDEO_EXT[e]);
        if (uvid_still(path, 1000, 0, 0, &full) != 0) memset(&full, 0, sizeof full);
    }
    int ok = 0;
    if (full.px && full.w > 0 && full.h > 0) {
        int sw = w, sh = (int)((int64_t)full.h * w / full.w);
        if (sh < h) { sh = h; sw = (int)((int64_t)full.w * h / full.h); }
        ok = uimg_scale(&full, sw, sh, out) == 0;
    }
    uimg_free(&full);
    return ok;
}

// One still frame of an effect, from the effect's own code
// (lib/ulivewall.c), a fixed distance into its clock so it shows what it
// looks like moving rather than its first instant.
static int render_effect(const char *name, int w, int h, struct uimg *out) {
    static struct ulivewall fx;
    if (!ulivewall_open(&fx, name)) return 0;
    ulivewall_step(&fx, 20000);
    out->px = malloc((size_t)w * (size_t)h * 4);
    if (out->px) {
        out->w = w;
        out->h = h;
        struct ugfx_surface t = ugfx_surface_for_pixels(out->px, w, h);
        ulivewall_render(&fx, &t, 4);
    }
    ulivewall_close(&fx);
    return out->px != 0;
}

enum { TILE_PICTURE, TILE_LIVE, TILE_SAVER };
#define SAVER_PICTURES "/usr/wm/savers"   // a saver's thumbnail beside its descriptor

static void draw_cached(struct ugfx_surface *s, struct tile_cache *c, const struct slot *sl,
                        int i, int x, int y, int w, int h, int kind) {
    uui_fill_round_rect(s, x, y, w, h, ugfx_char_h() / 3, PREVIEW_DARK);
    if (i < 0 || i >= PREVIEW_TILES || i >= sl->choice_count || w <= 0 || h <= 0) return;
    struct uimg *p = &c->img[i];
    const char *name = sl->choice_raw[i];
    if (strcmp(c->name[i], name) != 0 || p->w < w || p->h < h) {
        uimg_free(p);
        strlcpy(c->name[i], name, sizeof c->name[i]);
        // A LIVE CHOICE is an effect, or else an animated picture, whose
        // first frame stands for it.
        if (kind != TILE_LIVE || !render_effect(name, w, h, p))
            load_cover(kind == TILE_LIVE ? LIVEWALL_ANIMATED_DIR
                       : kind == TILE_SAVER ? SAVER_PICTURES : "/usr/share/wallpapers",
                       name, w, h, p);
    }
    if (!p->px) return;
    int ox = (p->w - w) / 2, oy = (p->h - h) / 2;
    ugfx_blit(s, x, y, w, h, p->px + (size_t)oy * (size_t)p->w + ox, p->w);
}

static void picture_tile(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    draw_cached(s, &g_pv_pic, ctx, i, x, y, w, h, TILE_PICTURE);
}

static void saver_tile(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    draw_cached(s, &g_pv_pic, ctx, i, x, y, w, h, TILE_SAVER);
}

// THE PREVIEW MONITOR above the screensaver gallery (mockup G2): the
// chosen saver's picture on a screen with a bezel and a stand. A STILL --
// Windows' preview runs the saver into a child window, and nothing here
// can host another program's window yet.

void preview_monitor(struct ugfx_surface *s, int x, int y, int w, int h, const char *saver) {
    int bez = ugfx_char_h() / 2 + 2, stand = ugfx_char_h();
    int sh = h - 2 * bez - stand, sw = sh * 16 / 9;
    if (sw + 2 * bez > w) { sw = w - 2 * bez; sh = sw * 9 / 16; }
    if (sw <= 0 || sh <= 0) return;
    int mx = x + (w - sw - 2 * bez) / 2;
    uui_fill_round_rect(s, mx, y, sw + 2 * bez, sh + 2 * bez, bez, 0x2b2d31);
    ugfx_fill_rect(s, mx + (sw + 2 * bez) / 2 - stand, y + sh + 2 * bez, 2 * stand, stand / 2, 0x2b2d31);
    ugfx_fill_rect(s, mx + (sw + 2 * bez) / 2 - 2 * stand, y + sh + 2 * bez + stand / 2, 4 * stand, stand / 2, 0x2b2d31);
    ugfx_fill_rect(s, mx + bez, y + bez, sw, sh, 0);
    if (!saver || !saver[0]) return;
    if (strcmp(g_mon_name, saver) != 0 || g_mon.w < sw || g_mon.h < sh) {
        uimg_free(&g_mon);
        strlcpy(g_mon_name, saver, sizeof g_mon_name);
        load_cover(SAVER_PICTURES, saver, sw, sh, &g_mon);
    }
    if (!g_mon.px) return;
    int ox = (g_mon.w - sw) / 2, oy = (g_mon.h - sh) / 2;
    ugfx_blit(s, mx + bez, y + bez, sw, sh, g_mon.px + (size_t)oy * (size_t)g_mon.w + ox, g_mon.w);
}

static void live_tile(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    draw_cached(s, &g_pv_live, ctx, i, x, y, w, h, 1);
}

// --- colour: the swatch -------------------------------------------------

static void colour_tile(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    const struct slot *sl = ctx;
    uint32_t c = i >= 0 && i < sl->choice_count ? ulivewall_colour(sl->choice_raw[i]) : PREVIEW_DARK;
    uui_fill_round_rect(s, x, y, w, h, ugfx_char_h() / 3, c);
}

// --- the table -----------------------------------------------------------

static const struct {
    const char *word;
    void (*paint)(struct ugfx_surface *, int, int, int, int, int, void *);
} PAINTERS[] = {
    { "cursor", cursor_tile },
    { "picture", picture_tile },
    { "live", live_tile },
    { "colour", colour_tile },
    { "saver", saver_tile },
};

void preview_attach(struct slot *sl, int idx) {
    sl->gallery.draw_tile = 0;
    sl->gallery.ctx = sl;
    // g_name is "<ns>.<name>"; the text file is named the same way.
    const char *name = g_name[idx];
    size_t nl = strlen(g_ns[idx]);
    if (nl && !strncmp(name, g_ns[idx], nl) && name[nl] == '.') name += nl + 1;
    char word[16];
    if (!uschema_text_word(g_ns[idx], name, SETTING_TEXT_KEY_PREVIEW, word, sizeof word)) return;
    for (unsigned p = 0; p < sizeof PAINTERS / sizeof PAINTERS[0]; p++)
        if (!strcmp(word, PAINTERS[p].word)) sl->gallery.draw_tile = PAINTERS[p].paint;
    // Seven savers on a page half as wide as the wallpapers': smaller
    // cards, four across, under the monitor that shows the chosen one big.
    if (!strcmp(word, "saver")) sl->gallery.min_w = ugfx_char_h() * 8;
}
