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
#include "ui/uui_fontsample.h"
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
static void fonts_reset(void);

void preview_reset(void) {
    pictures_reset();
    fonts_reset();
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

// The bezel and stand, centred in (x, y, w, h), for a screen of aspect
// aw:ah; the screen's rect comes back black in *sx.. . 0 when it cannot fit.
static int monitor_frame(struct ugfx_surface *s, int x, int y, int w, int h, int aw, int ah,
                         int *sx, int *sy, int *sw, int *sh) {
    int bez = ugfx_char_h() / 2 + 2, stand = ugfx_char_h();
    *sh = h - 2 * bez - stand;
    *sw = *sh * aw / ah;
    if (*sw + 2 * bez > w) { *sw = w - 2 * bez; *sh = *sw * ah / aw; }
    if (*sw <= 0 || *sh <= 0) return 0;
    int mx = x + (w - *sw - 2 * bez) / 2;
    uui_fill_round_rect(s, mx, y, *sw + 2 * bez, *sh + 2 * bez, bez, 0x2b2d31);
    ugfx_fill_rect(s, mx + (*sw + 2 * bez) / 2 - stand, y + *sh + 2 * bez, 2 * stand, stand / 2, 0x2b2d31);
    ugfx_fill_rect(s, mx + (*sw + 2 * bez) / 2 - 2 * stand, y + *sh + 2 * bez + stand / 2, 4 * stand, stand / 2, 0x2b2d31);
    *sx = mx + bez;
    *sy = y + bez;
    ugfx_fill_rect(s, *sx, *sy, *sw, *sh, 0);
    return 1;
}

void preview_monitor(struct ugfx_surface *s, int x, int y, int w, int h, const char *saver) {
    int sx, sy, sw, sh;
    if (!monitor_frame(s, x, y, w, h, 16, 9, &sx, &sy, &sw, &sh)) return;
    if (!saver || !saver[0]) return;
    if (strcmp(g_mon_name, saver) != 0 || g_mon.w < sw || g_mon.h < sh) {
        uimg_free(&g_mon);
        strlcpy(g_mon_name, saver, sizeof g_mon_name);
        load_cover(SAVER_PICTURES, saver, sw, sh, &g_mon);
    }
    if (!g_mon.px) return;
    int ox = (g_mon.w - sw) / 2, oy = (g_mon.h - sh) / 2;
    ugfx_blit(s, sx, sy, sw, sh, g_mon.px + (size_t)oy * (size_t)g_mon.w + ox, g_mon.w);
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

// --- fonts: each face's sample, drawn in the face itself ------------------
//
// Opened once per page and shared by both galleries (Interface and
// Monospace offer the same faces), keyed by stem. `builtin` has no file:
// it draws through the baked tables (ugfx_font_baked()).
#define PREVIEW_FACES 16
static struct uui_fontface g_faces[PREVIEW_FACES];
static int g_nfaces;

static void fonts_reset(void) {
    for (int i = 0; i < g_nfaces; i++) uui_fontface_close(&g_faces[i]);
    g_nfaces = 0;
}

static struct uui_fontface *face_for(const char *stem) {
    if (!strcmp(stem, "builtin")) return 0;
    for (int i = 0; i < g_nfaces; i++)
        if (!strcmp(g_faces[i].stem, stem)) return &g_faces[i];
    if (g_nfaces < PREVIEW_FACES && uui_fontface_open(&g_faces[g_nfaces], UUI_FONT_DIR, stem))
        return &g_faces[g_nfaces++];
    return 0;
}

static void font_paint(struct ugfx_surface *s, int i, int x, int y, int w, int h,
                       const struct slot *sl, int mono) {
    if (i < 0 || i >= sl->choice_count) return;
    struct uui_fontface *f = face_for(sl->choice_raw[i]);
    uui_fill_round_rect(s, x, y, w, h, ugfx_char_h() / 3, UTHEME_WHITE);
    if (!mono) {
        uui_fontsample_draw(s, f, UUI_FONTSAMPLE_CARD, x, y, w, h, UTHEME_WHITE, 0);
        return;
    }
    // A PROPORTIONAL FACE AS MONOSPACE is allowed -- ring 0 cannot tell --
    // and ruins every column: dimmed, and said so under its sample.
    int prop = f && !f->mono;
    int ch = ugfx_char_h(), note = prop ? ch + ch / 3 : 0;
    uui_fontsample_draw(s, f, UUI_FONTSAMPLE_TERMINAL, x + 4, y + 4, w - 8, h - 8 - note,
                        UTHEME_WHITE, prop);
    if (prop)
        ugfx_draw_string_clipped(s, x + 6, y + h - ch - 4, w - 12, "Not fixed-width",
                                 utheme_current()->severity[UTHEME_SEV_WARNING], UTHEME_WHITE);
}

static void font_tile(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    font_paint(s, i, x, y, w, h, ctx, 0);
}

static void fontmono_tile(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    font_paint(s, i, x, y, w, h, ctx, 1);
}

// A card is named by the family the file calls itself, not its stem:
// "DejaVu Sans Mono", not "dejavu-sans-mono".
static void font_labels(struct slot *sl) {
    for (int i = 0; i < sl->choice_count; i++) {
        struct uui_fontface *f = face_for(sl->choice_raw[i]);
        if (f) snprintf(sl->choice[i], sizeof sl->choice[i], "%s", f->family);
        else if (!strcmp(sl->choice_raw[i], "builtin"))
            snprintf(sl->choice[i], sizeof sl->choice[i], "Built-in");
    }
}

// --- window behaviour: the desktop in miniature ---------------------------
//
// One scene for four settings (mockups W1, W3): the desktop's colour, a
// taskbar strip, and windows drawn the way the choice draws them. Every
// size is a fraction of the tile, which the gallery derives from the font.

#define MINI_TITLE_ON  ugfx_rgb(50, 90, 160)     // wm_render.c's title bars
#define MINI_TITLE_OFF ugfx_rgb(120, 120, 130)
#define MINI_TASKBAR   ugfx_rgb(31, 37, 46)
#define MINI_GLASS_A   140

enum { MW_SHADOW = 1, MW_FOCUSED = 2, MW_GLASS_TITLE = 4, MW_GLASS_BODY = 8 };

// The desktop and its taskbar, on a card's rounded tile or (square) a
// monitor's screen; returns the height above the taskbar.
static int mini_desktop(struct ugfx_surface *s, int x, int y, int w, int h, int icons, int square) {
    uui_fill_round_rect(s, x, y, w, h, square ? 0 : ugfx_char_h() / 3, PREVIEW_DARK);
    int bar = h / 9 < 3 ? 3 : h / 9;
    ugfx_fill_rect(s, x, y + h - bar, w, bar, MINI_TASKBAR);
    // Desktop icons, so a see-through window has something to show.
    static const uint32_t ICON[] = { 0xE2A33B, 0x5FA866, 0x7FA7D6, 0xC8CCD4 };
    int is = h / 6;
    for (int i = 0; i < icons; i++)
        uui_fill_round_rect(s, x + is / 2 + (i / 2) * (is * 3 / 2), y + is / 2 + (i % 2) * (is * 3 / 2),
                            is, is, is / 4, ICON[i]);
    return h - bar;
}

static void mini_window(struct ugfx_surface *s, int x, int y, int w, int h, int flags) {
    int tb = h / 6 < 4 ? 4 : h / 6;
    // Three nested panes, offset down and right: the soft edge a real
    // shadow has, at a size where one dark band would read as a border.
    for (int k = 3; (flags & MW_SHADOW) && k >= 1; k--)
        uui_glass_round_rect(s, x + 2 - k, y + 3 - k, w + 2 * k, h + 2 * k, k + 2, 0, 55, 0);
    uint32_t title = (flags & MW_FOCUSED) ? MINI_TITLE_ON : MINI_TITLE_OFF;
    if (flags & MW_GLASS_BODY) uui_glass_round_rect(s, x, y + tb, w, h - tb, 1, UTHEME_WINDOW_BG, MINI_GLASS_A, 0);
    else ugfx_fill_rect(s, x, y + tb, w, h - tb, UTHEME_WINDOW_BG);
    if (flags & MW_GLASS_TITLE) uui_glass_round_rect(s, x, y, w, tb, 1, title, MINI_GLASS_A, 0);
    else ugfx_fill_rect(s, x, y, w, tb, title);
    // Two lines of content, where they fit.
    for (int k = 0; k < 2; k++) {
        int ly = y + tb * (2 + k * 3 / 2);
        if (ly + tb / 2 < y + h - 2) ugfx_fill_rect(s, x + tb, ly, w * (3 - k) / 5, tb / 2 > 1 ? tb / 2 : 2, UTHEME_BORDER);
    }
}

// The rubber band a move or resize drags instead of the window.
static void dashed_rect(struct ugfx_surface *s, int x, int y, int w, int h) {
    int d = ugfx_char_h() / 5 + 1, t = 2;
    for (int i = 0; i < w; i += 2 * d) {
        int n = w - i < d ? w - i : d;
        ugfx_fill_rect(s, x + i, y, n, t, 0xFFFFFF);
        ugfx_fill_rect(s, x + i, y + h - t, n, t, 0xFFFFFF);
    }
    for (int i = 0; i < h; i += 2 * d) {
        int n = h - i < d ? h - i : d;
        ugfx_fill_rect(s, x, y + i, t, n, 0xFFFFFF);
        ugfx_fill_rect(s, x + w - t, y + i, t, n, 0xFFFFFF);
    }
}

// An arrow pointer with its tip at (x, y), sized from the font.
static void mini_pointer(struct ugfx_surface *s, int x, int y) {
    static const int PX[] = { 0, 0, 4, 7, 9, 7, 11 }, PY[] = { 0, 16, 12, 18, 17, 11, 11 };
    int k = ugfx_char_h() >= 24 ? 2 : 1, n = (int)(sizeof PX / sizeof PX[0]);
    int xs[7], ys[7];
    for (int i = 0; i < n; i++) { xs[i] = x + PX[i] * k * 3 / 4; ys[i] = y + PY[i] * k * 3 / 4; }
    ugfx_fill_polygon(s, xs, ys, n, 0xFFFFFF);
    ugfx_draw_polyline(s, xs, ys, n, 1, 0x141414, GEOM_AA);
}

static const char *choice_word(const struct slot *sl, int i) {
    return i >= 0 && i < sl->choice_count ? sl->choice_raw[i] : "";
}

// desktop.move_mode: the window where it was dragged to, or the window
// left behind with the outline there instead.
static void winmove_tile(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    int dh = mini_desktop(s, x, y, w, h, 0, 0);
    int ww = w * 2 / 5, wh = dh * 3 / 5, tx = x + w - ww - w / 8, ty = y + dh / 4;
    int outline = !strcmp(choice_word(ctx, i), "outline");
    if (outline) {
        mini_window(s, x + w / 10, y + dh / 10, ww, wh, MW_SHADOW | MW_FOCUSED);
        dashed_rect(s, tx, ty, ww, wh);
    } else {
        mini_window(s, tx, ty, ww, wh, MW_SHADOW | MW_FOCUSED);
    }
    mini_pointer(s, tx + ww / 2, ty + 2);
}

// desktop.resize_mode: the window grown to the pointer, or kept and the
// outline grown. Automatic shows what it shows when the app keeps up.
static void winresize_tile(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    int dh = mini_desktop(s, x, y, w, h, 0, 0);
    int wx = x + w / 10, wy = y + dh / 10, bw = w * 3 / 4, bh = dh * 4 / 5;
    if (!strcmp(choice_word(ctx, i), "outline")) {
        mini_window(s, wx, wy, bw * 3 / 5, bh * 3 / 5, MW_SHADOW | MW_FOCUSED);
        dashed_rect(s, wx, wy, bw, bh);
    } else {
        mini_window(s, wx, wy, bw, bh, MW_SHADOW | MW_FOCUSED);
    }
    mini_pointer(s, wx + bw - 2, wy + bh - 2);
}

// Two windows, one behind the other: the shadow and see-through settings
// both read in how the pair overlaps.
static void window_pair(struct ugfx_surface *s, int x, int y, int w, int h, int back, int front) {
    int dh = mini_desktop(s, x, y, w, h, 4, 0);
    mini_window(s, x + w / 40, y + dh / 14, w / 2, dh * 2 / 3, back);
    mini_window(s, x + w * 3 / 10, y + dh / 4, w / 2, dh * 2 / 3, front | MW_FOCUSED);
}

// desktop.shadows
static void shadow_tile(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    int f = !strcmp(choice_word(ctx, i), "off") ? 0 : MW_SHADOW;
    window_pair(s, x, y, w, h, f, f);
}

// desktop.transparency_windows: which of the two lets the desktop through.
static void seethrough_tile(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    const char *c = choice_word(ctx, i);
    int title = MW_GLASS_TITLE, all = MW_GLASS_TITLE | MW_GLASS_BODY, back = 0, front = 0;
    if (!strcmp(c, "titlebars")) back = front = title;
    else if (!strcmp(c, "inactive")) back = all;
    else if (!strcmp(c, "all")) back = front = all;
    window_pair(s, x, y, w, h, back | MW_SHADOW, front | MW_SHADOW);
}

// --- the Screen page's monitor (mockup D1) ---------------------------------
//
// The panel, and the mode on it the way the scaler places it. A mode the
// panel shows natively, or a display with no scaler or no EDID, fills it.

static void place_mode(int pw, int ph, int mw, int mh, const char *scaling,
                       int *ow, int *oh) {
    *ow = pw; *oh = ph;
    if (!strcmp(scaling, "center")) {
        *ow = mw < pw ? mw : pw;
        *oh = mh < ph ? mh : ph;
    } else if (!strcmp(scaling, "aspect")) {
        if ((int64_t)mw * ph > (int64_t)pw * mh) *oh = (int)((int64_t)pw * mh / mw);
        else *ow = (int)((int64_t)ph * mw / mh);
    }
}

void preview_screen(struct ugfx_surface *s, int x, int y, int w, int h, struct preview_screen *ps) {
    int ch = ugfx_char_h(), cap = ch + ch / 2;
    ps->caption[0] = '\0';
    int pw = ps->panel_w, ph = ps->panel_h, mw = ps->mode_w, mh = ps->mode_h;
    if (mw <= 0 || mh <= 0) return;
    int scaled = pw > 0 && ph > 0 && ps->can_scale && (pw != mw || ph != mh);
    if (!scaled) { pw = mw; ph = mh; }
    int sx, sy, sw, sh;
    if (!monitor_frame(s, x, y, w, h - cap, pw, ph, &sx, &sy, &sw, &sh)) return;
    int ow, oh;
    place_mode(pw, ph, mw, mh, scaled ? ps->scaling : "full", &ow, &oh);
    int rw = (int)((int64_t)sw * ow / pw), rh = (int)((int64_t)sh * oh / ph);
    int rx = sx + (sw - rw) / 2, ry = sy + (sh - rh) / 2;
    // The scene in the mode's own pixels, so a stretch shows as one.
    int dh = mini_desktop(s, rx, ry, rw, rh, 0, 1);
    int sun = mh / 7;
    ugfx_fill_ellipse(s, rx + rw * (mw - sun) / mw, ry + rh * sun * 3 / 2 / mh,
                      rw * sun / 2 / mw, rh * sun / 2 / mh, 0xE2A33B);
    mini_window(s, rx + rw / 9, ry + dh / 6, rw / 2, dh / 2, MW_SHADOW | MW_FOCUSED);

    char *text = ps->caption;
    size_t cap_n = sizeof ps->caption;
    if (!scaled)
        snprintf(text, cap_n, "%dx%d%s", mw, mh,
                 ps->panel_w == mw && ps->panel_h == mh ? ", the panel's own size" : "");
    else
        snprintf(text, cap_n, "%dx%d on a %dx%d panel: %s", mw, mh, pw, ph,
                 !strcmp(ps->scaling, "center") ? "centred, not scaled"
                 : !strcmp(ps->scaling, "full") ? "stretched to fill" : "scaled, shape kept");
    int tw = ugfx_text_width(text);
    ugfx_draw_string_clipped(s, x + (tw < w ? (w - tw) / 2 : 0), y + h - ch - ch / 4, w, text,
                             UTHEME_TEXT, UTHEME_WINDOW_BG);
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
    { "font", font_tile },
    { "fontmono", fontmono_tile },
    { "winmove", winmove_tile },
    { "winresize", winresize_tile },
    { "shadow", shadow_tile },
    { "seethrough", seethrough_tile },
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
    if (!strcmp(word, "font") || !strcmp(word, "fontmono")) font_labels(sl);
}
