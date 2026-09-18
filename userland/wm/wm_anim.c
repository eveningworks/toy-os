// See wm_anim.h.
#include "wm_internal.h"
#include "wm_anim.h"
#include "wm_shadow.h"
#include "wm_taskbar.h"
#include "wm/wm_conf.h"
#include "lib/utween.h"
#include "rt/sys.h"
#include <stdlib.h>

#define DESKTOP_CONF "/etc/desktop.conf"
#define WM_ANIM_MS   150      // Qt's and DWM's window animations, give or take
#define SCALE_IN_PCT 92       // Windows 11 opens a window from about this size
#define PROGRESS     1000     // the tween runs 0..PROGRESS

enum kind { NONE = 0, OPEN, CLOSE, MINIMIZE, RESTORE };

struct anim {
    enum kind kind;
    int pid; uint32_t win;        // the live window it stands in for, or pid 0
    struct utween tw;
    int fx, fy, fw, fh;           // from
    int tx, ty, tw_, th;          // to
    int a0, a1;                   // alpha from/to
    uint32_t *snap; int sw, sh;   // the ghost's pixels
    int cx, cy, cw, ch, ca;       // this frame's rect and alpha
    int has_last;
};

static struct anim g_anims[WM_ANIM_MAX];
static int g_enabled = 1;
static uint32_t g_seen_generation;

// The surface every wm_surface() call draws into while a snapshot is
// being rendered (wm_internal.h).
struct ugfx_surface *g_wm_surface_override;

int wm_anim_enabled(void) { return g_enabled; }

int wm_anim_active(void) {
    int n = 0;
    for (int i = 0; i < WM_ANIM_MAX; i++) if (g_anims[i].kind) n++;
    return n;
}

int wm_anim_rect(int i, int *x, int *y, int *w, int *h, int *alpha) {
    if (i < 0 || i >= WM_ANIM_MAX || !g_anims[i].kind || !g_anims[i].has_last) return 0;
    const struct anim *a = &g_anims[i];
    *x = a->cx; *y = a->cy; *w = a->cw; *h = a->ch; *alpha = a->ca;
    return (int)a->kind;
}

static void finish(struct anim *a) {
    if (a->has_last) wm_damage_window_rect(a->cx, a->cy, a->cw, a->ch);
    free(a->snap);
    a->snap = 0;
    a->kind = NONE;
    a->pid = 0;
    redraw_pending = 1;
}

static struct anim *slot_for(int pid, uint32_t win) {
    // One ghost per window: a restore that follows a minimize before it
    // has finished replaces it rather than overlapping it.
    if (pid)
        for (int i = 0; i < WM_ANIM_MAX; i++)
            if (g_anims[i].kind && g_anims[i].pid == pid && g_anims[i].win == win) finish(&g_anims[i]);
    for (int i = 0; i < WM_ANIM_MAX; i++) if (!g_anims[i].kind) return &g_anims[i];
    finish(&g_anims[0]);   // all busy: the oldest yields
    return &g_anims[0];
}

// The ghost's pixels: the window's chrome and content, drawn once at
// its full size into a buffer of its own. A window whose content has
// not arrived (no buffer yet) has nothing to show and gets no ghost.
static int snapshot(struct anim *a, int idx) {
    struct window *w = &windows[idx];
    if (!wm_client_is_client_window(w) || !w->client_buf || w->w <= 0 || w->h <= 0) return 0;
    a->sw = w->w; a->sh = w->h;
    a->snap = malloc((size_t)a->sw * (size_t)a->sh * 4);
    if (!a->snap) return 0;
    struct window ghost = *w;
    ghost.x = 0; ghost.y = 0;
    struct ugfx_surface dst = ugfx_surface_for_pixels(a->snap, a->sw, a->sh);
    wm_render_window_into(&dst, &ghost, idx == wm_focus_index());
    return 1;
}

static void start(struct anim *a, enum kind kind, int pid, uint32_t win) {
    a->kind = kind; a->pid = pid; a->win = win;
    a->has_last = 0;
    utween_start(&a->tw, 0, PROGRESS, WM_ANIM_MS, sys_monotonic_ns());
    wm_anim_step();
}

static void scaled_rect(const struct window *w, int pct, int *x, int *y, int *rw, int *rh) {
    *rw = w->w * pct / 100; *rh = w->h * pct / 100;
    *x = w->x + (w->w - *rw) / 2; *y = w->y + (w->h - *rh) / 2;
}

static void button_rect(int idx, int *x, int *y, int *w, int *h) {
    if (taskbar_button_rect_for(idx, x, y, w, h)) return;
    // No button (the taskbar is hidden or full): the bottom centre.
    *w = 64; *h = taskbar_h > 0 ? taskbar_h : 32;
    *x = screen_w / 2 - 32; *y = screen_h - *h;
}

void wm_anim_open(int idx) {
    if (!g_enabled || idx < 0 || idx >= window_count) return;
    struct window *w = &windows[idx];
    if (w->popup || w->fullscreen) return;
    struct anim *a = slot_for(w->client_pid, w->client_win);
    if (!snapshot(a, idx)) return;
    scaled_rect(w, SCALE_IN_PCT, &a->fx, &a->fy, &a->fw, &a->fh);
    a->tx = w->x; a->ty = w->y; a->tw_ = w->w; a->th = w->h;
    a->a0 = 0; a->a1 = 255;
    start(a, OPEN, w->client_pid, w->client_win);
}

void wm_anim_close(int idx) {
    if (!g_enabled || idx < 0 || idx >= window_count) return;
    struct window *w = &windows[idx];
    if (w->popup || w->fullscreen || w->state == WIN_MINIMIZED) return;
    struct anim *a = slot_for(w->client_pid, w->client_win);
    if (!snapshot(a, idx)) return;
    a->fx = w->x; a->fy = w->y; a->fw = w->w; a->fh = w->h;
    scaled_rect(w, SCALE_IN_PCT, &a->tx, &a->ty, &a->tw_, &a->th);
    a->a0 = 255; a->a1 = 0;
    start(a, CLOSE, 0, 0);   // no live window: it is going
}

void wm_anim_minimize(int idx) {
    if (!g_enabled || idx < 0 || idx >= window_count) return;
    struct window *w = &windows[idx];
    if (w->popup || w->fullscreen || w->state == WIN_MINIMIZED) return;
    struct anim *a = slot_for(w->client_pid, w->client_win);
    if (!snapshot(a, idx)) return;
    a->fx = w->x; a->fy = w->y; a->fw = w->w; a->fh = w->h;
    button_rect(idx, &a->tx, &a->ty, &a->tw_, &a->th);
    a->a0 = 255; a->a1 = 0;
    start(a, MINIMIZE, 0, 0);   // the live window is minimized: not drawn anyway
}

void wm_anim_restore(int idx) {
    if (!g_enabled || idx < 0 || idx >= window_count) return;
    struct window *w = &windows[idx];
    if (w->popup || w->fullscreen || w->state != WIN_MINIMIZED) return;
    struct anim *a = slot_for(w->client_pid, w->client_win);
    if (!snapshot(a, idx)) return;
    button_rect(idx, &a->fx, &a->fy, &a->fw, &a->fh);
    a->tx = w->x; a->ty = w->y; a->tw_ = w->w; a->th = w->h;
    a->a0 = 0; a->a1 = 255;
    start(a, RESTORE, w->client_pid, w->client_win);
}

int wm_anim_hides(const struct window *w) {
    if (!wm_client_is_client_window(w)) return 0;
    for (int i = 0; i < WM_ANIM_MAX; i++) {
        const struct anim *a = &g_anims[i];
        if (!a->kind || !a->pid) continue;
        if (a->pid == w->client_pid && a->win == w->client_win) return 1;
    }
    return 0;
}

static int lerp(int a, int b, int v) { return a + (int)(((long long)(b - a) * v) / PROGRESS); }

void wm_anim_step(void) {
    unsigned long long now = sys_monotonic_ns();
    int any = 0;
    for (int i = 0; i < WM_ANIM_MAX; i++) {
        struct anim *a = &g_anims[i];
        if (!a->kind) continue;
        int v = utween_value(&a->tw, now);
        int x = lerp(a->fx, a->tx, v), y = lerp(a->fy, a->ty, v);
        int w = lerp(a->fw, a->tw_, v), h = lerp(a->fh, a->th, v);
        int al = lerp(a->a0, a->a1, v);
        // Damage where it was and where it is: both padded like a window,
        // since a ghost stands where one stood.
        if (a->has_last) wm_damage_window_rect(a->cx, a->cy, a->cw, a->ch);
        wm_damage_window_rect(x, y, w, h);
        a->cx = x; a->cy = y; a->cw = w; a->ch = h; a->ca = al;
        a->has_last = 1;
        if (!utween_active(&a->tw)) { finish(a); continue; }
        any = 1;
    }
    if (any) redraw_pending = 1;
}

void wm_anim_draw(void) {
    for (int i = 0; i < WM_ANIM_MAX; i++) {
        const struct anim *a = &g_anims[i];
        if (!a->kind || !a->snap || !a->has_last || a->cw <= 0 || a->ch <= 0) continue;
        ugfx_blit_scaled_alpha(wm_surface(), a->cx, a->cy, a->cw, a->ch,
                               a->snap, a->sw, a->sh, a->sw, (uint8_t)a->ca);
    }
}

static void adopt(void) {
    char v[16];
    int on = 1;
    if (wm_conf_get(DESKTOP_CONF, "animations", v, sizeof v) && v[0])
        on = !(v[0] == 'o' && v[1] == 'f' && v[2] == 'f');
    g_enabled = on;
    if (!on) for (int i = 0; i < WM_ANIM_MAX; i++) if (g_anims[i].kind) finish(&g_anims[i]);
}

void wm_anim_poll_config(void) {
    uint32_t gen = wm_setting_generation();
    if (gen == g_seen_generation) return;
    g_seen_generation = gen;
    adopt();
}
