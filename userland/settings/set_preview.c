// The pictures in a gallery's cards (Widget=gallery), chosen by the
// setting's `Preview=` word. A TABLE of painters, so the wallpaper picker
// is one more row. Each painter draws choice `i` of the slot it is handed.
#include "settings_internal.h"
#include "lib/ucursor.h"
#include "lib/usetting_schema.h"
#include "lib/usetting_text.h"
#include "lib/ulivewall.h"
#include "lib/uimg.h"
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

// --- wallpaper: the picture, decoded and scaled once per page ----------

#define PREVIEW_PICTURES 16
static struct uimg g_pv_pic[PREVIEW_PICTURES];
static char g_pv_pic_name[PREVIEW_PICTURES][SETTING_ABI_VALUE_MAX];

static void pictures_reset(void) {
    for (int i = 0; i < PREVIEW_PICTURES; i++) {
        uimg_free(&g_pv_pic[i]);
        g_pv_pic_name[i][0] = '\0';
    }
}

// Scaled to cover the tile, so a picture of another shape is cropped
// the way "Fill the screen" crops it rather than squashed.
static void picture_tile(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    const struct slot *sl = ctx;
    uui_fill_round_rect(s, x, y, w, h, ugfx_char_h() / 3, PREVIEW_DARK);
    if (i < 0 || i >= PREVIEW_PICTURES || i >= sl->choice_count) return;
    struct uimg *p = &g_pv_pic[i];
    if (strcmp(g_pv_pic_name[i], sl->choice_raw[i]) != 0) {
        uimg_free(p);
        strlcpy(g_pv_pic_name[i], sl->choice_raw[i], sizeof g_pv_pic_name[i]);
        char path[128];
        struct uimg full;
        memset(&full, 0, sizeof full);
        // The stem is the value; the file is whichever of these it is.
        static const char *const EXT[] = { "jpg", "gif", "png", "qoi" };
        for (unsigned e = 0; e < sizeof EXT / sizeof EXT[0]; e++) {
            snprintf(path, sizeof path, "/usr/share/wallpapers/%s.%s", sl->choice_raw[i], EXT[e]);
            if (uimg_load(path, &full) == 0) break;
        }
        if (full.px && full.w > 0 && full.h > 0) {
            int sw = w, sh = (int)((int64_t)full.h * w / full.w);
            if (sh < h) { sh = h; sw = (int)((int64_t)full.w * h / full.h); }
            uimg_scale(&full, sw, sh, p);
        }
        uimg_free(&full);
    }
    if (!p->px) return;
    int ox = (p->w - w) / 2, oy = (p->h - h) / 2;
    ugfx_blit(s, x, y, w, h, p->px + (size_t)oy * (size_t)p->w + ox, p->w);
}

// --- live: one still frame of the effect, from the same code -----------

// A FRAME FROM THE EFFECT ITSELF (lib/ulivewall.c), a fixed distance into
// its clock so it shows what it looks like moving, not its first instant.
// Re-rendered on every paint: a tile is a few thousand grid points.
static void live_tile(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    const struct slot *sl = ctx;
    uui_fill_round_rect(s, x, y, w, h, ugfx_char_h() / 3, PREVIEW_DARK);
    if (i < 0 || i >= sl->choice_count || w <= 0 || h <= 0) return;
    static struct ulivewall fx;
    if (!ulivewall_open(&fx, sl->choice_raw[i])) return;
    ulivewall_step(&fx, 20000);
    uint32_t *px = malloc((size_t)w * (size_t)h * 4);
    if (px) {
        struct ugfx_surface tile = ugfx_surface_for_pixels(px, w, h);
        ulivewall_render(&fx, &tile, 4);
        ugfx_blit(s, x, y, w, h, px, w);
        free(px);
    }
    ulivewall_close(&fx);
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
}
