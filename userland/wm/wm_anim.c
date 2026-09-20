// See wm_anim.h.
#include "wm_internal.h"
#include "wm_anim.h"
#include "wm_shadow.h"
#include "wm_taskbar.h"
#include "wm_log.h"   // the anim-end gap says so when it fires
#include "wm/wm_conf.h"
#include "lib/utween.h"
#include "lib/ueffect.h" // the shatter effect's own options
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
    // THIS FRAME'S PROGRESS, stored rather than re-derived in the draw.
    // The damage is computed from it in the step; a draw reading the
    // clock again would place a tile against a slightly later tween
    // than the one that was damaged for, which is a stale pixel where
    // the two disagree.
    int cv;
    int has_last;
    // THE EFFECT IS FIXED AT START, not read per frame: changing the
    // setting mid-flight would otherwise switch a ghost's shape and
    // its destination halfway across the screen. The grid and the
    // motion ride along for the same reason -- a window that changed
    // from 24 pieces to 192 halfway would be two animations.
    unsigned char effect;
    unsigned char motion;
    short cols, rows;
    int bx, by, bw, bh;           // the taskbar button, for the genie's mouth
};

static struct anim g_anims[WM_ANIM_MAX];
static int g_enabled = 1;
static uint32_t g_seen_generation;
static uint64_t g_seen_fsgen;

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
enum { EFFECT_SCALE = 0, EFFECT_GENIE, EFFECT_SQUASH, EFFECT_GLIDE,
       EFFECT_SHATTER };
static int g_effect = EFFECT_SCALE;

// SHATTER: the window breaks into tiles that pour into its taskbar
// button, and stream back out of it on restore.
//
// **THE TILES CARRY THE WINDOW'S OWN PIXELS**, which is what separates
// this from confetti: a ghost already owns a snapshot, and
// ugfx_blit_scaled_alpha() takes a source PITCH -- so a tile is that
// snapshot blitted from an offset with the full stride, and the effect
// needed no new primitive (unlike the genie).
//
// SIX BY FOUR, and the count is a judgement rather than a measurement:
// the per-frame PIXEL work is the same whatever the count -- every tile
// together covers the window once -- so what a finer grid costs is call
// overhead, and what it costs visually is that a tile stops looking
// like a piece of your window and starts looking like noise. Compiz's
// Explode and KWin's Fall Apart, the two effects this is modelled on,
// both use chunks you can still read.
// THE GRID IS A SETTING (`desktop.shatter_pieces`), and these are its
// three steps. The per-frame PIXEL work is the same at every count --
// the tiles together cover the window once however it is cut -- so what
// a finer grid really costs is per-blit overhead, and what it costs
// VISUALLY is that a tile stops looking like a piece of your window and
// starts looking like noise. Coarse is the default for that reason,
// not for the cost.
#define SHATTER_MAX_COLS 16
#define SHATTER_MAX_ROWS 12
#define SHATTER_MAX_TILES (SHATTER_MAX_COLS * SHATTER_MAX_ROWS)
enum { PIECES_COARSE = 0, PIECES_MEDIUM, PIECES_FINE };
static int g_shatter_pieces = PIECES_COARSE;

// `desktop.shatter_motion`. POUR runs every tile straight to the
// button; EXPLODE gives it an outward kick first and lets it fall back
// in, which is Compiz's Explode rather than a plain implosion.
//
// **THE KICK IS BOUNDED, and that is not a detail**: a tile that leaves
// the window's bounds has to be damaged for, and an unbounded burst
// means a near-full-screen repaint every frame on a software
// compositor. SHATTER_KICK_PCT of the window's own size keeps the
// damage a predictable box (see damage_ghost).
enum { MOTION_POUR = 0, MOTION_EXPLODE };
static int g_shatter_motion = MOTION_POUR;
#define SHATTER_KICK_PCT  28   // of the window's half-extent, at the peak
#define SHATTER_KICK_PEAK 300  // where the burst tops out, 0..PROGRESS
// How much of the tween one tile's stagger may eat -- and it is PER
// MOTION, because the stagger is most of what tells the two apart.
//
// A POUR wants it: the window empties from the edge nearest the
// taskbar, row after row, which is what pouring looks like. A BURST
// does not -- an explosion where the bottom row leaves first and the
// top row a third of a second later reads as a window PEELING, which
// is exactly how it looked before this was split. So explode keeps
// just enough to break the rows out of lockstep.
//
// The last tile to leave still gets the rest of the run to travel in,
// so nothing snaps home at the end.
#define SHATTER_STAGGER_POUR    35
#define SHATTER_STAGGER_EXPLODE 8

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
// The grid and the motion this ghost will use, fixed at start with the
// effect and for the same reason. Defaults stand in until the settings
// that choose them are wired (docs/conventions/gui.md).
static void shatter_grid_of(struct anim *a) {
    switch (g_shatter_pieces) {
    case PIECES_FINE:   a->cols = 16; a->rows = 12; break;
    case PIECES_MEDIUM: a->cols = 10; a->rows = 7;  break;
    default:            a->cols = 6;  a->rows = 4;  break;
    }
    a->motion = (unsigned char)g_shatter_motion;
}

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
    shatter_grid_of(a);
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
    shatter_grid_of(a);
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
    // SHATTER paints tiles strung out between the window and the
    // button, so the lerped rect covers none of the ones in flight --
    // the same stale-pixel fault the genie's tube has, with a box
    // instead of a tube. It is bounded: the tiles only ever travel
    // INWARD, so the union of the two ends contains every one of them.
    if (a->effect == EFFECT_SHATTER && (a->kind == MINIMIZE || a->kind == RESTORE)) {
        // **THE WHOLE TRAVEL, NOT THIS FRAME'S RECT** -- the genie's
        // lesson, in a box instead of a tube. The tiles are STAGGERED,
        // so the last of them is still sitting at the window's original
        // position long after the lerped rect has shrunk past it: a
        // union built from that rect stops covering the top rows
        // half-way through, and they stay on screen. Measured as 15500
        // stale pixels starting at the window's own top-left corner.
        //
        // It is bounded by construction: every tile travels from the
        // window to the button, so the union of those two contains all
        // of them for the whole run.
        int wx = (a->kind == RESTORE) ? a->tx  : a->fx;
        int wy = (a->kind == RESTORE) ? a->ty  : a->fy;
        int ww = (a->kind == RESTORE) ? a->tw_ : a->fw;
        int wh = (a->kind == RESTORE) ? a->th  : a->fh;
        int x0 = wx < a->bx ? wx : a->bx;
        int y0 = wy < a->by ? wy : a->by;
        int x1 = wx + ww, y1 = wy + wh;
        if (a->bx + a->bw > x1) x1 = a->bx + a->bw;
        if (a->by + a->bh > y1) y1 = a->by + a->bh;
        // EXPLODE THROWS TILES OUTSIDE THE WINDOW, so the union of the
        // two ends no longer contains them. The kick is bounded on
        // purpose (see SHATTER_KICK_PCT) precisely so this margin can
        // be: the furthest a tile goes is its distance from the centre
        // -- at most half the window -- times the kick plus its jitter.
        if (a->motion == MOTION_EXPLODE) {
            int mx = (ww * (SHATTER_KICK_PCT + 10)) / 200;
            int my = (wh * (SHATTER_KICK_PCT + 10)) / 200;
            x0 -= mx; y0 -= my; x1 += mx; y1 += my;
        }
        (void)x; (void)y; (void)w; (void)h;
        wm_damage_window_rect(x0, y0, x1 - x0, y1 - y0);
        return;
    }
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
        a->cv = v;
        a->has_last = 1;
        if (!utween_active(&a->tw)) { finish(a); continue; }
        any = 1;
    }
    if (any) redraw_pending = 1;
}

// ONE TILE'S RECT THIS FRAME, and whether it is on screen at all.
//
// Every tile travels the same way -- from where it sits in the window
// to a point inside the taskbar button -- and differs only in WHEN it
// starts. So a restore is the same walk with the progress reversed,
// which is why there is no second copy of this for the other direction.
//
// Integer math throughout: this desktop has no floating point.
static int shatter_tile(const struct anim *a, int i, int q,
                        int *out_x, int *out_y, int *out_w, int *out_h,
                        int *out_sx, int *out_sy, int *out_sw, int *out_sh) {
    int cols = a->cols, rows = a->rows;
    if (cols <= 0 || rows <= 0) return 0;
    int col = i % cols, row = i / cols;

    // The source tile, in snapshot pixels. The last column and row take
    // the remainder so no strip of the window is left undrawn.
    int tw = a->sw / cols, th = a->sh / rows;
    if (tw <= 0 || th <= 0) return 0;
    int sx = col * tw, sy = row * th;
    int sw = (col == cols - 1) ? a->sw - sx : tw;
    int sh = (row == rows - 1) ? a->sh - sy : th;
    if (sw <= 0 || sh <= 0) return 0;

    // THE BOTTOM ROWS LEAVE FIRST, which is what makes it read as
    // pouring rather than dissolving -- the window empties from the
    // edge nearest the taskbar. The column offset is small and only
    // breaks up the row marching in lockstep.
    int stagger = (a->motion == MOTION_EXPLODE) ? SHATTER_STAGGER_EXPLODE
                                                : SHATTER_STAGGER_POUR;
    int delay = ((rows - 1 - row) * stagger * PROGRESS) / (rows * 100)
              + (col * stagger * PROGRESS) / (cols * 400);
    int span = PROGRESS - delay;
    if (span <= 0) span = 1;
    int p = ((q - delay) * PROGRESS) / span;
    if (p < 0) p = 0;
    if (p > PROGRESS) p = PROGRESS;

    // Where this tile starts: its place in the window, which is the
    // FROM rect for a minimize and the TO rect for a restore.
    int wx = (a->kind == RESTORE) ? a->tx  : a->fx;
    int wy = (a->kind == RESTORE) ? a->ty  : a->fy;
    int ww = (a->kind == RESTORE) ? a->tw_ : a->fw;
    int wh = (a->kind == RESTORE) ? a->th  : a->fh;
    if (ww <= 0 || wh <= 0) return 0;

    int x0 = wx + (sx * ww) / a->sw, y0 = wy + (sy * wh) / a->sh;
    int w0 = (sw * ww) / a->sw,      h0 = (sh * wh) / a->sh;

    // ...and where it lands: spread across the button's width rather
    // than all on one point, so the stream has a mouth the width of the
    // thing it is pouring into.
    int x1 = a->bx + (col * a->bw) / cols;
    int y1 = a->by + a->bh / 2;
    int w1 = a->bw / cols; if (w1 < 1) w1 = 1;
    int h1 = 1;

    *out_x = lerp(x0, x1, p); *out_y = lerp(y0, y1, p);
    *out_w = lerp(w0, w1, p); *out_h = lerp(h0, h1, p);

    // THE BURST, and the jitter that makes a grid look like debris.
    //
    // Every tile on a straight line to its own slot keeps its
    // neighbours as neighbours, and the whole thing reads as a diagonal
    // WIPE rather than a window coming apart -- which is exactly how
    // the first version looked. The kick pushes each tile away from the
    // window's centre and lets it fall back, so the pieces separate
    // before they converge.
    //
    // A HASH, NOT A RANDOM: the offset has to be the same every frame
    // of one animation, or a tile jitters in place instead of
    // travelling. The tile index is the whole seed.
    if (a->motion == MOTION_EXPLODE) {
        int bump = (p < SHATTER_KICK_PEAK)
                 ? (p * PROGRESS) / SHATTER_KICK_PEAK
                 : ((PROGRESS - p) * PROGRESS) / (PROGRESS - SHATTER_KICK_PEAK);
        // Away from the centre, so the corners throw furthest -- which
        // is what a burst looks like and what a uniform push does not.
        int cx = wx + ww / 2, cy = wy + wh / 2;
        int dx = (x0 + w0 / 2) - cx, dy = (y0 + h0 / 2) - cy;
        int h1_ = (i * 2654435761u) >> 13;      // Knuth's multiplicative hash
        int jx = (int)(h1_ % 21) - 10;          // +/-10% of the kick
        int jy = (int)((h1_ >> 5) % 21) - 10;
        *out_x += ((dx * SHATTER_KICK_PCT / 100) * bump / PROGRESS)
                + (dx * jx / 100) * bump / PROGRESS;
        *out_y += ((dy * SHATTER_KICK_PCT / 100) * bump / PROGRESS)
                + (dy * jy / 100) * bump / PROGRESS;
    }
    *out_sx = sx; *out_sy = sy; *out_sw = sw; *out_sh = sh;
    return (*out_w > 0 && *out_h > 0);
}

// How many pieces a ghost is drawn as: 1 for every effect but this one.
// Reported through `gui state --json` so a test can tell a shatter from
// a scale -- both move one bounding box toward the same button, so
// nothing else about the report distinguishes them.
int wm_anim_pieces(int i) {
    if (i < 0 || i >= WM_ANIM_MAX || !g_anims[i].kind) return 0;
    const struct anim *a = &g_anims[i];
    if (a->effect != EFFECT_SHATTER) return 1;
    return (a->kind == MINIMIZE || a->kind == RESTORE) ? a->cols * a->rows : 1;
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
        if (a->effect == EFFECT_SHATTER &&
            (a->kind == MINIMIZE || a->kind == RESTORE)) {
            // The progress this frame, reversed for a restore so the
            // same walk runs backwards and the tiles stream OUT.
            int q = (a->kind == RESTORE) ? PROGRESS - a->cv : a->cv;
            for (int t = 0; t < a->cols * a->rows; t++) {
                int x, y, w, h, sx, sy, sw, sh;
                if (!shatter_tile(a, t, q, &x, &y, &w, &h, &sx, &sy, &sw, &sh))
                    continue;
                // THE SOURCE PITCH IS THE WHOLE SNAPSHOT'S. That is what
                // makes this a sub-rect blit and not a copy -- the
                // pointer picks the tile, the pitch keeps the rows.
                ugfx_blit_scaled_alpha(wm_surface(), x, y, w, h,
                                       a->snap + (size_t)sy * a->sw + sx,
                                       sw, sh, a->sw, (uint8_t)a->ca);
            }
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
    if (!k_strcmp(v, "shatter")) return EFFECT_SHATTER;
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

// THE EFFECT'S OWN OPTIONS, from its descriptor and whatever has been
// chosen (lib/ueffect.h).
//
// **SEPARATE FROM adopt(), AND WATCHED ON THE FILESYSTEM**, because an
// option is NOT a registered setting: System Settings writes it with
// uconf_set() straight to /etc/effects/<name>.conf, which moves no
// generation the settings registry reports. Reading these in adopt()
// meant the compositor kept whatever it had at boot -- every window
// shattered into the default 24 pieces however the option was set, and
// the only thing that ever fixed it was changing some OTHER setting.
//
// The screensavers do not have this problem and that is why it was not
// noticed: a saver is a PROGRAM that reads its own conf each time it
// starts, where the compositor is long-lived and caches.
static void adopt_effect_opts(void) {
    g_shatter_pieces = PIECES_COARSE;
    g_shatter_motion = MOTION_POUR;
    if (g_effect != EFFECT_SHATTER) return;   // no other effect has options
    struct usaver o;
    if (!ueffect_load("shatter", &o) || !o.opt_count) return;
    const char *p = usaver_str(&o, "pieces", "coarse");
    g_shatter_pieces = !k_strcmp(p, "fine")   ? PIECES_FINE
                     : !k_strcmp(p, "medium") ? PIECES_MEDIUM
                                              : PIECES_COARSE;
    const char *m = usaver_str(&o, "motion", "pour");
    g_shatter_motion = !k_strcmp(m, "explode") ? MOTION_EXPLODE : MOTION_POUR;

    // ON A CHANGE ONLY. This runs on every filesystem generation bump,
    // so an unconditional line here is a log that outruns itself -- and
    // the answer it exists to give ("did my option take") is worth
    // nothing once the ring has rolled over.
    static int last_p = -1, last_m = -1;
    if (g_shatter_pieces != last_p || g_shatter_motion != last_m) {
        last_p = g_shatter_pieces;
        last_m = g_shatter_motion;
        wm_logf("anim: shatter pieces=%s motion=%s",
                g_shatter_pieces == PIECES_FINE ? "fine"
              : g_shatter_pieces == PIECES_MEDIUM ? "medium" : "coarse",
                g_shatter_motion == MOTION_EXPLODE ? "explode" : "pour");
    }
}

void wm_anim_poll_config(void) {
    uint32_t gen = wm_setting_generation();
    if (gen != g_seen_generation) {
        g_seen_generation = gen;
        adopt();
        adopt_effect_opts();   // the effect may have just changed
    }
    // AND THE FILESYSTEM, for the options -- see adopt_effect_opts().
    // The taskbar watches the same clock for the same reason: a file
    // nobody registered changes without the registry hearing about it.
    uint64_t fsg = sys_fs_generation();
    if (fsg != g_seen_fsgen) {
        g_seen_fsgen = fsg;
        adopt_effect_opts();
    }
}
