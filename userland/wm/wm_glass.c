// Transparency: the glass behind the taskbar, Start, menus and windows.
// wm_glass.h has the model; docs/decisions/gui.md has why it is built
// this way.
#include "wm_internal.h"
#include "wm_glass.h"
#include "wm_shadow.h"
#include "desktop.h"
#include "start_menu.h"
#include "network_popup.h"
#include "layout_popup.h"
#include "volume_popup.h"
#include "brightness_popup.h"
#include "context_menu.h"
#include "wm_taskbar.h"
#include "wm/wm_conf.h"
#include "lib/usetting.h"
#include "ui/ugfx.h"
#include "ui/uui_popup.h"
#include "ui/uui_primitives.h"
#include <stdlib.h>
#include <string.h>

enum glass_kind { GLASS_CLEAR, GLASS_FROSTED, GLASS_WALLPAPER };
enum glass_windows { GW_NONE, GW_TITLEBARS, GW_INACTIVE, GW_ALL };

static int g_on;
static int g_kind[4] = { GLASS_FROSTED, GLASS_FROSTED, GLASS_FROSTED, GLASS_FROSTED };
static int g_windows = GW_TITLEBARS;
static int g_moving;
static uint8_t g_alpha[4] = { 191, 217, 217, 217 };   // per wm_glass_surface, 0..255
static uint32_t g_seen_generation;

// --- the two screen-sized scratch backdrops ---------------------------
//
// g_blur holds a frosted rect's blurred scene while it is painted; g_under
// holds what was beneath a see-through window while that window draws
// over it. A frosted TITLE BAR is painted inside a see-through window's
// drawing, so the two are never the same buffer.
static uint32_t *g_blur, *g_under, *g_wall;
static int g_buf_w, g_buf_h;
static uint32_t g_wall_gen = 0xFFFFFFFFu;
static int g_wall_w, g_wall_h;

#define BLUR_CACHE 16    // the blur cache: see frosted() below
static struct blur_entry {
    int x, y, w, h;
    uint64_t hash;
    unsigned used;      // LRU clock
    uint32_t *px;       // w * h, the blurred rect
    uint32_t *ground;   // w * h, the FINISHED glass painted from it, or 0
    uint64_t ground_key;
} g_cache[BLUR_CACHE];

static uint32_t *screen_buf(uint32_t **p) {
    if (g_buf_w != screen_w || g_buf_h != screen_h) {
        free(g_blur); free(g_under); free(g_wall);
        g_blur = g_under = g_wall = 0;
        g_buf_w = screen_w; g_buf_h = screen_h;
        g_wall_gen = 0xFFFFFFFFu;
        for (int i = 0; i < BLUR_CACHE; i++) {
            free(g_cache[i].px); free(g_cache[i].ground);
            g_cache[i].px = g_cache[i].ground = 0;
        }
    }
    if (!*p) *p = malloc((size_t)screen_w * (size_t)screen_h * 4);
    return *p;
}

// The blur's reach, font-derived like every other size here: about a
// line of text for live glass, three for the wallpaper (Mica is softer).
static int blur_radius(void) { int r = ugfx_char_h(); return r < 4 ? 4 : r; }

// THE WALLPAPER, BLURRED ONCE: rebuilt when the background or the screen
// changes, never per frame -- the whole point of the kind.
static const uint32_t *wallpaper_backdrop(void) {
    uint32_t *w = screen_buf(&g_wall);
    if (!w) return 0;
    uint32_t gen = desktop_background_gen();
    if (gen == g_wall_gen && g_wall_w == screen_w && g_wall_h == screen_h) return w;
    struct ugfx_surface tmp = ugfx_surface_for_pixels(w, screen_w, screen_h);
    desktop_draw_background_into(&tmp);
    ugfx_blur_rect(&tmp, 0, 0, screen_w, screen_h, 3 * blur_radius(), w);
    g_wall_gen = gen;
    g_wall_w = screen_w; g_wall_h = screen_h;
    return w;
}

// --- settings ---------------------------------------------------------

static int read_pct(const char *name, int dflt) {
    char v[16];
    if (!usetting_get(name, v, sizeof v) || !v[0]) return dflt;
    int n = 0;
    for (const char *p = v; *p >= '0' && *p <= '9'; p++) n = n * 10 + (*p - '0');
    if (n < 0) n = 0;
    if (n > 100) n = 100;
    return n;
}

static int read_word(const char *name, const char *const *words, int count, int dflt) {
    char v[32];
    if (!usetting_get(name, v, sizeof v)) return dflt;
    for (int i = 0; i < count; i++)
        if (!strcmp(v, words[i])) return i;
    return dflt;
}

// What a menu's ground goes through while glass is on (ui/uui_popup.h).
static void menu_painter(struct ugfx_surface *s, int x, int y, int w, int h, int r,
                         uint32_t tint) {
    (void)s;   // always the scene: only the WM installs this
    wm_glass_paint(WM_GLASS_MENU, x, y, w, h, r, tint);
}

static void adopt(void) {
    static const char *const ONOFF[] = { "off", "on" };
    static const char *const KINDS[] = { "clear", "frosted", "wallpaper" };
    static const char *const WINS[] = { "none", "titlebars", "inactive", "all" };
    g_on = read_word("desktop.transparency", ONOFF, 2, 0);
    static const char *const KIND[] = { "desktop.transparency_taskbar_glass",
                                        "desktop.transparency_start_glass",
                                        "desktop.transparency_menus_glass",
                                        "desktop.transparency_window_glass" };
    for (int i = 0; i < 4; i++) g_kind[i] = read_word(KIND[i], KINDS, 3, GLASS_FROSTED);
    g_windows = read_word("desktop.transparency_windows", WINS, 4, GW_TITLEBARS);
    g_moving = read_word("desktop.transparency_moving", ONOFF, 2, 0);
    static const char *const PCT[] = { "desktop.transparency_taskbar", "desktop.transparency_start",
                                       "desktop.transparency_menus",
                                       "desktop.transparency_window_opacity" };
    static const int DFLT[] = { 75, 85, 85, 85 };
    for (int i = 0; i < 4; i++) g_alpha[i] = (uint8_t)(read_pct(PCT[i], DFLT[i]) * 255 / 100);
    uui_popup_set_glass(g_on && g_alpha[WM_GLASS_MENU] < 255 ? menu_painter : 0);
    // EVERY glass surface changes at once: damage it all, since a frame
    // that also carries some other damage would otherwise be limited to it.
    redraw_pending = 1;
    wm_damage_rect(0, 0, screen_w, screen_h);
}

static void track_drag(void);

void wm_glass_poll_config(void) {
    track_drag();
    uint32_t gen = wm_setting_generation();
    if (gen == g_seen_generation) return;
    g_seen_generation = gen;
    adopt();
}

// --- painting ---------------------------------------------------------

int wm_glass_on(enum wm_glass_surface surf) {
    return g_on && g_alpha[surf] < 255;
}

static int clipped(int *x, int *y, int *w, int *h);

// --- the blur cache ---------------------------------------------------
//
// MOST FRAMES THAT REPAINT A FROSTED RECT CHANGE NOTHING BENEATH IT: the
// Start menu's caret blinks, a row lights up, the clock ticks, and each
// damages the whole rect (wm_glass.h) without moving a pixel of the scene
// under it. So the blur is kept per rect, keyed by a hash of the pixels
// it was made from, and a repaint over an unchanged scene copies it back
// -- the scene is still REPAINTED under the rect every time, which is
// what makes the hash a test of what is there rather than a guess. A
// WM surface keeps its finished ground too (tint and band, keyed by
// what painted them), so an unchanged frame is a copy and nothing more.
static unsigned g_clock;

static uint64_t region_hash(const struct ugfx_surface *s, int x, int y, int w, int h) {
    uint64_t hv = 1469598103934665603ull ^ (uint64_t)blur_radius();
    for (int j = y; j < y + h; j++) {
        const uint32_t *row = s->pixels + (size_t)j * s->w + x;
        for (int i = 0; i < w; i++) hv = (hv ^ row[i]) * 1099511628211ull;
    }
    return hv;
}

static struct blur_entry *cache_slot(int x, int y, int w, int h) {
    struct blur_entry *lru = &g_cache[0];
    for (int i = 0; i < BLUR_CACHE; i++) {
        struct blur_entry *e = &g_cache[i];
        if (e->px && e->x == x && e->y == y && e->w == w && e->h == h) return e;
        if (!e->px || e->used < lru->used) lru = e;
    }
    uint32_t *px = realloc(lru->px, (size_t)w * (size_t)h * 4);
    if (!px) return 0;
    lru->px = px;
    free(lru->ground);
    lru->ground = 0;
    lru->x = x; lru->y = y; lru->w = w; lru->h = h;
    lru->hash = ~(uint64_t)0 ^ 1;   // nothing matches a fresh slot's first hash
    return lru;
}

// The rect's cache entry, when it is being repainted WHOLE -- a part of
// one has its blur's edges clamped where the clip cut it, so it is not
// worth keeping. Its `hash` is refreshed; `*same` says whether the scene
// under it is what the entry was made from.
static struct blur_entry *entry_for(const struct ugfx_surface *src, int x, int y, int w, int h,
                                    int *same) {
    int cx = x, cy = y, cw = w, ch = h;
    *same = 0;
    if (!clipped(&cx, &cy, &cw, &ch) || cx != x || cy != y || cw != w || ch != h) return 0;
    struct blur_entry *e = cache_slot(x, y, w, h);
    if (!e) return 0;
    uint64_t hv = region_hash(src, x, y, w, h);
    e->used = ++g_clock;
    *same = hv == e->hash;
    if (!*same) {
        e->hash = hv;
        free(e->ground);   // made from a scene that is gone
        e->ground = 0;
    }
    return e;
}

static void frosted(const struct ugfx_surface *src, struct blur_entry *e, int same,
                    int x, int y, int w, int h, uint32_t *b) {
    size_t stride = (size_t)src->w;
    if (e && same) {
        for (int j = 0; j < h; j++)
            memcpy(b + (size_t)(y + j) * stride + x, e->px + (size_t)j * w, (size_t)w * 4);
        return;
    }
    ugfx_blur_rect(src, x, y, w, h, blur_radius(), b);
    if (e)
        for (int j = 0; j < h; j++)
            memcpy(e->px + (size_t)j * w, b + (size_t)(y + j) * stride + x, (size_t)w * 4);
}

// What the glass shows through, or 0 for "the scene as it is" (CLEAR,
// or a FROSTED rect the blur could not be prepared for).
static const uint32_t *backdrop_for(enum wm_glass_surface surf, const struct ugfx_surface *src,
                                    int x, int y, int w, int h) {
    if (g_kind[surf] == GLASS_WALLPAPER) return wallpaper_backdrop();
    if (g_kind[surf] != GLASS_FROSTED) return 0;
    uint32_t *b = screen_buf(&g_blur);
    if (!b) return 0;
    int same;
    struct blur_entry *e = entry_for(src, x, y, w, h, &same);
    frosted(src, e, same, x, y, w, h, b);
    return b;
}

static void paint_band(struct ugfx_surface *s, const struct wm_glass_band *band) {
    for (int j = band->y; j < band->y + band->h; j++)
        ugfx_blend_hspan(s, band->x, j, band->w, band->c, 0, band->a);
}

void wm_glass_paint_band(enum wm_glass_surface surf, int x, int y, int w, int h, int r,
                         uint32_t tint, const struct wm_glass_band *band) {
    struct ugfx_surface *s = wm_surface();
    // Into the scene only: a window ghost (wm_render_window_into) has
    // nothing beneath it to show through.
    if (!wm_glass_on(surf) || s != &g_wm_screen.back) {
        uui_fill_round_rect(s, x, y, w, h, r, tint);
        if (band) ugfx_fill_rect(s, band->x, band->y, band->w, band->h, band->c);
        return;
    }
    if (g_kind[surf] != GLASS_FROSTED) {
        uui_backdrop_round_rect(s, x, y, w, h, r, backdrop_for(surf, s, x, y, w, h), tint,
                                g_alpha[surf]);
        if (band) paint_band(s, band);
        return;
    }
    uint32_t *b = screen_buf(&g_blur);
    if (!b) { uui_fill_round_rect(s, x, y, w, h, r, tint); return; }
    int same;
    struct blur_entry *e = entry_for(s, x, y, w, h, &same);
    // WHAT PAINTED THE GROUND, beside what it was painted over.
    uint32_t parts[] = { tint, g_alpha[surf], (uint32_t)r, band != 0,
                         band ? (uint32_t)(band->x - x) : 0, band ? (uint32_t)(band->y - y) : 0,
                         band ? (uint32_t)band->w : 0, band ? (uint32_t)band->h : 0,
                         band ? band->c : 0, band ? band->a : 0 };
    uint64_t key = 1469598103934665603ull;
    for (unsigned k = 0; k < sizeof parts / sizeof parts[0]; k++)
        key = (key ^ parts[k]) * 1099511628211ull;
    size_t stride = (size_t)s->w;
    if (e && same && e->ground && e->ground_key == key) {
        for (int j = 0; j < h; j++)
            memcpy(s->pixels + (size_t)(y + j) * stride + x, e->ground + (size_t)j * w, (size_t)w * 4);
        ugfx_mark_dirty_rect(s, x, y, w, h);
        return;
    }
    frosted(s, e, same, x, y, w, h, b);
    uui_backdrop_round_rect(s, x, y, w, h, r, b, tint, g_alpha[surf]);
    if (band) paint_band(s, band);
    if (e && (e->ground || (e->ground = malloc((size_t)w * (size_t)h * 4)))) {
        for (int j = 0; j < h; j++)
            memcpy(e->ground + (size_t)j * w, s->pixels + (size_t)(y + j) * stride + x, (size_t)w * 4);
        e->ground_key = key;
    }
}

void wm_glass_paint(enum wm_glass_surface surf, int x, int y, int w, int h, int r,
                    uint32_t tint) {
    wm_glass_paint_band(surf, x, y, w, h, r, tint, 0);
}

uint32_t wm_glass_ink_bg(enum wm_glass_surface surf, uint32_t bg, uint32_t ground) {
    return wm_glass_on(surf) && bg == ground && wm_surface() == &g_wm_screen.back
        ? UGFX_TRANSPARENT : bg;
}

void wm_glass_draw_client(const struct window *w) {
    int x = window_content_x(w), y = window_content_y(w);
    const uint32_t *bd = 0;
    uint8_t a = 255;
    if (wm_glass_on(WM_GLASS_MENU)) {
        bd = backdrop_for(WM_GLASS_MENU, wm_surface(), x, y, w->client_w, w->client_h);
        a = g_alpha[WM_GLASS_MENU];
    }
    ugfx_blit_glass(wm_surface(), x, y, w->client_w, w->client_h,
                    w->client_buf, w->client_w, bd, a);
}

int wm_glass_titlebars(void) {
    return g_on && g_windows == GW_TITLEBARS && g_alpha[WM_GLASS_WINDOW] < 255;
}

uint8_t wm_glass_window_alpha(int i, int focus) {
    if (!g_on || g_alpha[WM_GLASS_WINDOW] >= 255) return 255;
    const struct window *w = &windows[i];
    if (w->popup || w->fullscreen) return 255;
    if (g_windows == GW_ALL || (g_windows == GW_INACTIVE && i != focus) ||
        (g_moving && i == dragging))
        return g_alpha[WM_GLASS_WINDOW];
    return 255;
}

// The rect clipped to the screen and the active clip -- the only pixels
// either half below touches.
static int clipped(int *x, int *y, int *w, int *h) {
    const struct ugfx_surface *s = wm_surface();
    int x0 = *x, y0 = *y, x1 = *x + *w, y1 = *y + *h;
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
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return *w > 0 && *h > 0;
}

void wm_glass_window_begin(int x, int y, int w, int h) {
    uint32_t *u = screen_buf(&g_under);
    if (!u || !clipped(&x, &y, &w, &h)) return;
    const struct ugfx_surface *s = wm_surface();
    for (int j = y; j < y + h; j++)
        memcpy(u + (size_t)j * s->w + x, s->pixels + (size_t)j * s->w + x, (size_t)w * 4);
}

// What a see-through window lays itself over: the scene that was under it
// (saved by _begin), blurred from that copy for FROSTED, or the wallpaper.
static const uint32_t *window_backdrop(int x, int y, int w, int h) {
    if (g_kind[WM_GLASS_WINDOW] != GLASS_FROSTED)
        return g_kind[WM_GLASS_WINDOW] == GLASS_WALLPAPER ? wallpaper_backdrop() : g_under;
    struct ugfx_surface *s = wm_surface();
    struct ugfx_surface under = ugfx_surface_for_pixels(g_under, s->w, s->h);
    struct ugfx_clip c;
    ugfx_clip_save(s, &c);
    ugfx_clip_restore(&under, &c);
    const uint32_t *b = backdrop_for(WM_GLASS_WINDOW, &under, x, y, w, h);
    return b ? b : g_under;
}

void wm_glass_window_end(int x, int y, int w, int h, uint8_t alpha) {
    if (!g_under) return;
    const uint32_t *u = window_backdrop(x, y, w, h);
    if (!u || !clipped(&x, &y, &w, &h)) return;
    struct ugfx_surface *s = wm_surface();
    unsigned a = alpha;
    for (int j = y; j < y + h; j++) {
        uint32_t *row = s->pixels + (size_t)j * s->w;
        const uint32_t *ur = u + (size_t)j * s->w;
        for (int i = x; i < x + w; i++) {
            uint32_t o = row[i], b = ur[i];
            unsigned rr = (((o >> 16) & 0xFF) * a + ((b >> 16) & 0xFF) * (255 - a) + 127) / 255;
            unsigned gg = (((o >> 8) & 0xFF) * a + ((b >> 8) & 0xFF) * (255 - a) + 127) / 255;
            unsigned bb = ((o & 0xFF) * a + (b & 0xFF) * (255 - a) + 127) / 255;
            row[i] = rr << 16 | gg << 8 | bb;
        }
    }
    ugfx_mark_dirty_rect(s, x, y, w, h);
}

// --- damage -----------------------------------------------------------

static int add(struct wm_glass_rect *out, int n, int max, int x, int y, int w, int h) {
    if (n >= max || w <= 0 || h <= 0) return n;
    out[n].x = x; out[n].y = y; out[n].w = w; out[n].h = h;
    return n + 1;
}

static int frosted_on(enum wm_glass_surface surf) {
    return wm_glass_on(surf) && g_kind[surf] == GLASS_FROSTED;
}

int wm_glass_frosted_rects(struct wm_glass_rect *out, int max) {
    if (!g_on) return 0;
    int n = 0;
    int focus = wm_focus_index();
    if (frosted_on(WM_GLASS_TASKBAR) && !wm_top_covers_screen()) {
        struct taskbar_geom g;
        taskbar_geom(&g);
        n = add(out, n, max, g.px, g.py, g.pw, g.ph);
    }
    int x, y, w, h;
    if (frosted_on(WM_GLASS_START) && start_menu_open && start_menu_rect(&x, &y, &w, &h))
        n = add(out, n, max, x, y, w, h);
    // THE TRAY FLYOUTS are cards on the menus' glass (wm_flyout.h).
    if (frosted_on(WM_GLASS_MENU)) {
        if (network_open && network_rect(&x, &y, &w, &h)) n = add(out, n, max, x, y, w, h);
        if (volume_open && volume_rect(&x, &y, &w, &h)) n = add(out, n, max, x, y, w, h);
        if (brightness_open && brightness_rect(&x, &y, &w, &h)) n = add(out, n, max, x, y, w, h);
        if (layout_open && layout_rect(&x, &y, &w, &h)) n = add(out, n, max, x, y, w, h);
        // The panel's right-click menu, each open level (a menu is glass
        // through uui_popup's painter).
        for (int l = 0; context_menu_level_rect(l, &x, &y, &w, &h); l++)
            n = add(out, n, max, x, y, w, h);
    }
    for (int i = 0; i < window_count; i++) {
        const struct window *win = &windows[i];
        if (win->state == WIN_MINIMIZED) continue;
        if (win->popup && win->popup_glass && frosted_on(WM_GLASS_MENU))
            n = add(out, n, max, window_content_x(win), window_content_y(win),
                    win->client_w, win->client_h);
        else if (win->popup || !frosted_on(WM_GLASS_WINDOW))
            continue;
        else if (wm_glass_titlebars() && window_has_chrome(win))
            n = add(out, n, max, win->x + 1, win->y + 1, win->w - 2, WM_TITLEBAR_H);
        else if (wm_glass_window_alpha(i, focus) < 255)
            n = add(out, n, max, win->x, win->y, win->w, win->h);
    }
    return n;
}

// A drag starting or ending changes how its window is drawn without
// moving it, and may end in an iteration that paints nothing else.
static void track_drag(void) {
    static int last_pid = -1;
    static uint32_t last_win;
    int pid = -1;
    uint32_t id = 0;
    if (g_on && g_moving && dragging >= 0 && dragging < window_count) {
        pid = windows[dragging].client_pid;
        id = windows[dragging].client_win;
    }
    if (pid == last_pid && id == last_win) return;
    for (int i = 0; i < window_count; i++) {
        const struct window *w = &windows[i];
        if ((w->client_pid == pid && w->client_win == id) ||
            (w->client_pid == last_pid && w->client_win == last_win))
            wm_damage_window_rect(w->x, w->y, w->w, w->h);
    }
    redraw_pending = 1;
    last_pid = pid;
    last_win = id;
}
