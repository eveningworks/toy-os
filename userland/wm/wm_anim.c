// See wm_anim.h.
#include "wm_internal.h"
#include "wm_anim.h"
#include "wm_shadow.h"
#include "wm_taskbar.h"
#include "wm_log.h"   // the anim-end gap says so when it fires
#include "wm/wm_conf.h"
#include "lib/utween.h"
#include "rt/sys.h"
#include <stdlib.h>
#include "string.h"   // k_strcmp -- the speed names are matched in full

#define DESKTOP_CONF "/etc/desktop.conf"
// THE BASE DURATION, which `desktop.animation_speed` scales. 250 ms is
// KWin's default for its window effects; Windows' minimize is nearer
// 200 and macOS's genie nearer 500, so this sits between them. It was
// 150, which at ~13 ms a frame on a 1080p panel bought about eleven
// frames -- enough to see the steps rather than the motion.
#define WM_ANIM_MS   250
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
    // THE EFFECT IS FIXED AT START, not read per frame: changing the
    // setting mid-flight would otherwise switch a ghost's shape and
    // its destination halfway across the screen.
    unsigned char effect;
    int bx, by, bw, bh;           // the taskbar button, for the genie's mouth
};

static struct anim g_anims[WM_ANIM_MAX];
static int g_enabled = 1;
static uint32_t g_seen_generation;

// `desktop.animation_speed`: ONE multiplier over every animation, which
// is KWin's shape (its Animation Speed slider scales every effect at
// once) rather than a duration per effect. A knob per effect drifts as
// effects are added; a multiplier cannot.
//
// INSTANT IS NOT A DURATION OF ZERO SOMEWHERE DOWNSTREAM -- it skips
// the ghost entirely, so nothing snapshots, allocates or hides the
// real window for a frame. `desktop.animations=off` is the same path.
enum { SPEED_INSTANT = 0, SPEED_FAST, SPEED_NORMAL, SPEED_SLOW, SPEED_VERY_SLOW };
static int g_speed = SPEED_NORMAL;

// `desktop.minimize_effect`, for MINIMIZE and RESTORE only. Opening and
// closing keep SCALE: macOS applies its genie to the Dock alone, and a
// window that genies out of nothing on open reads as a glitch rather
// than as an effect.
//
// SCALE and SQUASH and GLIDE are all the same axis-aligned blit with
// different from/to rects -- ugfx_blit_scaled_alpha() already scales
// the axes independently, so only the geometry differs. GENIE is the
// one that needs its own primitive (ugfx_blit_genie).
enum { EFFECT_SCALE = 0, EFFECT_GENIE, EFFECT_SQUASH, EFFECT_GLIDE };
static int g_effect = EFFECT_SCALE;

// Numerator/denominator rather than a float: there is no floating point
// in this desktop. KWin's slider is the same set of ratios.
static unsigned anim_duration_ms(void) {
    switch (g_speed) {
    case SPEED_INSTANT:   return 0;
    case SPEED_FAST:      return WM_ANIM_MS / 2;
    case SPEED_SLOW:      return WM_ANIM_MS * 2;
    case SPEED_VERY_SLOW: return WM_ANIM_MS * 4;
    default:              return WM_ANIM_MS;
    }
}

// Whether an animation should happen at all: the setting being off and
// the speed being "instant" are the same answer, asked in one place so
// the five entry points below cannot disagree about it.
static int anim_wanted(void) { return g_enabled && anim_duration_ms() > 0; }

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

// Defined below, beside the step that computes the rects it damages.
static void damage_ghost(const struct anim *a, int x, int y, int w, int h);

static void finish(struct anim *a) {
    if (a->has_last) damage_ghost(a, a->cx, a->cy, a->cw, a->ch);
    // AND THE WINDOW AS IT IS *NOW*, which is not where the ghost ended.
    // A client may resize while its ghost is in flight -- Notepad does,
    // once it has laid out the file it was opened with -- and the real
    // window is hidden for the whole animation (wm_anim_hides), so the
    // damage that resize reported was consumed by a frame that could not
    // draw it. Damaging only the ghost's last rect then paints the window
    // clipped to the size it had BEFORE the resize: no title-bar buttons,
    // no scrollbar, no status bar, all of which live past that edge.
    //
    // AND IT DOES NOT HEAL. The taskbar clock damages the taskbar strip,
    // not the screen, so the once-a-second repaint never covers the gap;
    // only a full repaint does, which is why taking a screenshot appeared
    // to fix it (wm_screenshot.c renders a frame and sets redraw_pending).
    if (a->pid)
        for (int i = 0; i < window_count; i++) {
            const struct window *w = &windows[i];
            if (w->client_pid != a->pid || w->client_win != a->win ||
                w->state == WIN_MINIMIZED) continue;
            // Says so when it actually had work to do -- the window
            // reaching past where the ghost ended is the fault itself,
            // and it cannot be photographed (a screenshot renders a
            // frame and heals it) or reproduced in QEMU, where the
            // window does not move or resize under its own ghost.
            if (a->has_last && (w->x < a->cx || w->y < a->cy ||
                                w->x + w->w > a->cx + a->cw ||
                                w->y + w->h > a->cy + a->ch))
                wm_logf("wm: anim end: window %d,%d %dx%d reaches past the ghost's "
                            "%d,%d %dx%d -- damaging the window\n",
                            w->x, w->y, w->w, w->h, a->cx, a->cy, a->cw, a->ch);
            wm_damage_window_rect(w->x, w->y, w->w, w->h);
        }
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
    // TRAVEL EASES IN AND OUT; something appearing in place eases OUT.
    // A minimize crosses the screen, and ease-out starts it at full
    // speed, which reads as the window being thrown at the taskbar
    // rather than moving there. Open and close scale in place, where
    // ease-out is right and is what every toolkit uses.
    enum utween_curve curve = (kind == MINIMIZE || kind == RESTORE)
                            ? UTWEEN_EASE_IN_OUT : UTWEEN_EASE_OUT;
    utween_start_curve(&a->tw, 0, PROGRESS, anim_duration_ms(),
                       sys_monotonic_ns(), curve);
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

// The from/to rects for a MINIMIZE, by effect. RESTORE is the same
// pair reversed, which is why this is one function and not two.
//
// GENIE keeps the ghost's rect as the whole TUBE -- window top down to
// the button -- and wm_anim_draw() shapes what is inside it. The other
// three are plain rects and differ only in where they end:
//   SCALE   shrinks into the button (what this has always done)
//   SQUASH  keeps its width and collapses onto the taskbar (KWin's)
//   GLIDE   travels to the button at 60%, fading, with no distortion
static void minimize_rects(const struct window *w, int bx, int by, int bw, int bh,
                           int effect, int *tx, int *ty, int *tw, int *th,
                           int *a0, int *a1) {
    *a0 = 255; *a1 = 0;
    switch (effect) {
    case EFFECT_SQUASH:
        // Width HOLDS, height collapses onto the taskbar's top edge.
        *tx = w->x; *tw = w->w;
        *ty = by;   *th = 1;
        *a1 = 128;          // still visible as it flattens; the collapse reads it
        break;
    case EFFECT_GLIDE:
        *tw = w->w * 3 / 5; *th = w->h * 3 / 5;
        *tx = bx + bw / 2 - *tw / 2;
        *ty = by + bh / 2 - *th / 2;
        break;
    case EFFECT_GENIE:
        // The tube: from the window's top edge down to the button. The
        // BAND is what the rect reports; the neck is drawn inside it.
        *tx = w->x; *tw = w->w;
        *ty = by;   *th = 1;
        break;
    default:            // EFFECT_SCALE
        *tx = bx; *ty = by; *tw = bw; *th = bh;
        break;
    }
}

void wm_anim_open(int idx) {
    if (!anim_wanted() || idx < 0 || idx >= window_count) return;
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
    if (!anim_wanted() || idx < 0 || idx >= window_count) return;
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
    if (!anim_wanted() || idx < 0 || idx >= window_count) return;
    struct window *w = &windows[idx];
    if (w->popup || w->fullscreen || w->state == WIN_MINIMIZED) return;
    struct anim *a = slot_for(w->client_pid, w->client_win);
    if (!snapshot(a, idx)) return;
    a->fx = w->x; a->fy = w->y; a->fw = w->w; a->fh = w->h;
    button_rect(idx, &a->bx, &a->by, &a->bw, &a->bh);
    a->effect = (unsigned char)g_effect;
    minimize_rects(w, a->bx, a->by, a->bw, a->bh, g_effect,
                   &a->tx, &a->ty, &a->tw_, &a->th, &a->a0, &a->a1);
    start(a, MINIMIZE, 0, 0);   // the live window is minimized: not drawn anyway
}

void wm_anim_restore(int idx) {
    if (!anim_wanted() || idx < 0 || idx >= window_count) return;
    struct window *w = &windows[idx];
    if (w->popup || w->fullscreen || w->state != WIN_MINIMIZED) return;
    struct anim *a = slot_for(w->client_pid, w->client_win);
    if (!snapshot(a, idx)) return;
    // THE SAME PAIR, REVERSED. A restore that took a different route
    // from its minimize reads as two unrelated animations rather than
    // one thing going and coming back.
    button_rect(idx, &a->bx, &a->by, &a->bw, &a->bh);
    a->effect = (unsigned char)g_effect;
    int a0, a1;
    minimize_rects(w, a->bx, a->by, a->bw, a->bh, g_effect,
                   &a->fx, &a->fy, &a->fw, &a->fh, &a0, &a1);
    a->tx = w->x; a->ty = w->y; a->tw_ = w->w; a->th = w->h;
    a->a0 = a1; a->a1 = a0;        // the minimize's alphas, backwards
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

// WHAT A GHOST ACTUALLY PAINTS, which is not always its rect. The
// genie fills the whole tube from the window's current top edge down
// to the button, so damaging the lerped rect alone would leave the
// neck on screen -- the same stale-pixel fault the shadow and the
// resize-under-a-ghost both were.
static void damage_ghost(const struct anim *a, int x, int y, int w, int h) {
    if (a->effect == EFFECT_GENIE && (a->kind == MINIMIZE || a->kind == RESTORE)) {
        int top = y < a->by ? y : a->by;
        int bot = a->by + a->bh;
        if (bot < y + h) bot = y + h;
        int x0 = x < a->bx ? x : a->bx;
        int x1 = x + w;
        if (a->bx + a->bw > x1) x1 = a->bx + a->bw;
        wm_damage_window_rect(x0, top, x1 - x0, bot - top);
        return;
    }
    wm_damage_window_rect(x, y, w, h);
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
        if (a->has_last) damage_ghost(a, a->cx, a->cy, a->cw, a->ch);
        damage_ghost(a, x, y, w, h);
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
        if (a->effect == EFFECT_GENIE && (a->kind == MINIMIZE || a->kind == RESTORE)) {
            // THE TUBE IS THE WHOLE TRAVEL, not this frame's rect: the
            // band runs from wherever the window's top edge has reached
            // down to the button, and the neck lives inside it. Feeding
            // it the lerped rect instead would give a tube that shrinks
            // to nothing rather than one the window slides down.
            int top = a->cy, bot = a->by;
            if (bot <= top) bot = top + 1;
            ugfx_blit_genie(wm_surface(), top, bot - top,
                            a->cx + a->cw / 2, a->cw,
                            a->bx + a->bw / 2, a->bw,
                            a->snap, a->sw, a->sh, a->sw, (uint8_t)a->ca);
            continue;
        }
        ugfx_blit_scaled_alpha(wm_surface(), a->cx, a->cy, a->cw, a->ch,
                               a->snap, a->sw, a->sh, a->sw, (uint8_t)a->ca);
    }
}

// The speed NAMES, matched in full. A prefix match would make "slow"
// and "slower" the same answer, and the enum is ordered so a future
// value can be added at either end.
static int effect_of(const char *v) {
    if (!k_strcmp(v, "genie"))  return EFFECT_GENIE;
    if (!k_strcmp(v, "squash")) return EFFECT_SQUASH;
    if (!k_strcmp(v, "glide"))  return EFFECT_GLIDE;
    return EFFECT_SCALE;           // including an unknown value
}

static int speed_of(const char *v) {
    if (!k_strcmp(v, "instant"))   return SPEED_INSTANT;
    if (!k_strcmp(v, "fast"))      return SPEED_FAST;
    if (!k_strcmp(v, "slow"))      return SPEED_SLOW;
    if (!k_strcmp(v, "very-slow")) return SPEED_VERY_SLOW;
    return SPEED_NORMAL;           // including an unknown value
}

static void adopt(void) {
    char v[16];
    int on = 1;
    if (wm_conf_get(DESKTOP_CONF, "animations", v, sizeof v) && v[0])
        on = !(v[0] == 'o' && v[1] == 'f' && v[2] == 'f');
    g_enabled = on;
    if (wm_conf_get(DESKTOP_CONF, "animation_speed", v, sizeof v) && v[0])
        g_speed = speed_of(v);
    else
        g_speed = SPEED_NORMAL;
    if (wm_conf_get(DESKTOP_CONF, "minimize_effect", v, sizeof v) && v[0])
        g_effect = effect_of(v);
    else
        g_effect = EFFECT_SCALE;
    if (!on) for (int i = 0; i < WM_ANIM_MAX; i++) if (g_anims[i].kind) finish(&g_anims[i]);
}

void wm_anim_poll_config(void) {
    uint32_t gen = wm_setting_generation();
    if (gen == g_seen_generation) return;
    g_seen_generation = gen;
    adopt();
}
