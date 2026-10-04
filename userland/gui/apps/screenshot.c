// Screenshot -- the shell's capture overlay, GNOME 42's shape (mockup
// S3, 2026-10-04): PrtSc freezes the screen, dims it, and puts a pill at
// the bottom -- Region / Screen / Window, the shutter, the pointer, copy
// to the clipboard, a delay, close. There is no window to manage: the
// shutter saves to /home/screenshots, the compositor puts up a card with
// the picture and Open / Copy / Folder / Save as (WIN_REQ_NOTICE), and
// the app is gone. /bin/screenshot is the other front end on lib/ushot.h.
// The card's Save as comes back here as `--save-as PATH` (the bottom of
// this file).
//
// **THE CHOICE IS MADE ON A FROZEN FRAME**, taken before the overlay is
// shown: the thing being photographed cannot move while you choose
// what to photograph (Spectacle and GNOME both do this). It is taken
// twice, with and without the pointer, so the pointer toggle can be
// flipped after the fact.
//
// **THE APP IS NEVER IN ITS OWN PICTURE**: every capture asks
// WIN_SHOT_NO_SELF, so the compositor renders the frame without this
// client's window.
//
// A DELAY IS A RELAUNCH: the overlay goes away, a fresh copy of this
// program sleeps, and the overlay comes back over the screen as it is
// then (Windows' Snipping Tool) -- or, in Screen mode, the picture is
// simply taken. There is no request to hide a window, and a client that
// sleeps with one open is drawn as Not Responding.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include "rt/sys.h"
#include "ui/ugfx.h"
#include "ui/uui.h"
#include "ui/uapp.h"
#include "ui/ulog.h"
#include "ui/utheme.h"
#include "ui/uui_primitives.h"
#include "lib/icon_cache.h"
#include "lib/uclip.h"
#include "lib/uconf.h"
#include "lib/ushot.h"
#include "keyboard.h"
#include "kpath.h"   // k_path_basename/_dirname -- the kernel's, linked into ring 3
#include "lib/uimg.h"
#include "ui/uui_filedialog.h"

#define SHOT_DIR  "/home/screenshots"
#define PREFS     "/etc/screenshot.conf"   // the app's own (docs/conventions/gui.md)
#define SELF_PATH "/bin/wm/apps/screenshot"

enum { MODE_REGION, MODE_SCREEN, MODE_WINDOW, MODES };
static const char *const MODE_NAME[MODES] = { "region", "screen", "window" };
static const char *const MODE_LABEL[MODES] = { "Region", "Screen", "Window" };
static const char *const MODE_ICON[MODES] = { "tb-region", "tb-screen", "tb-window" };

static const int DELAYS[] = { 0, 3, 5, 10 };
#define DELAY_COUNT 4

// The preferences: the last mode, and the three toggles.
static int g_mode = MODE_REGION;
static int g_pointer;
static int g_copy;
static int g_delay_i;

// The two frozen frames, copied out of the ONE capture object a process
// may have (its buffer is named after the pid -- lib/ushot.h), and the
// dimmed copy of whichever is shown.
static struct ushot g_shot;
static uint32_t *g_fr[2];         // [0] without the pointer, [1] with it
static uint32_t *g_dim;
static int g_dim_of = -1;      // which frame g_dim was made from
static int g_sw, g_sh;
static int g_ok;

// --- the selection ----------------------------------------------------

struct rect { int x, y, w, h; };
static struct rect g_sel;         // the region; w == 0 is none yet
static struct rect g_win;         // the window under the pointer
static int g_win_have;

enum { DRAG_NONE, DRAG_NEW, DRAG_MOVE, DRAG_HANDLE };
static int g_drag = DRAG_NONE;
static int g_handle;              // 0..7, clockwise from the top-left
static int g_ax, g_ay;            // the press point
static struct rect g_start;       // the selection when the drag began
static int g_pressed_ctl = -1;    // the pill control a press armed
static int g_hot = -1;            // the pill control under the pointer
static int g_px, g_py;            // the pointer, for the size beside it

static struct uapp *g_app;
static int g_launch_delay;        // this copy was relaunched after a delay
static int g_launch_mode = -1;

static const uint32_t *frame(void) { return g_fr[g_pointer ? 1 : 0]; }

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// --- preferences ------------------------------------------------------

static void prefs_load(void) {
    char v[16];
    if (uconf_get(PREFS, "mode", v, sizeof v))
        for (int i = 0; i < MODES; i++) if (!strcmp(v, MODE_NAME[i])) g_mode = i;
    if (uconf_get(PREFS, "pointer", v, sizeof v)) g_pointer = !strcmp(v, "on");
    if (uconf_get(PREFS, "copy", v, sizeof v)) g_copy = !strcmp(v, "on");
    if (uconf_get(PREFS, "delay", v, sizeof v)) {
        int d = atoi(v);
        for (int i = 0; i < DELAY_COUNT; i++) if (DELAYS[i] == d) g_delay_i = i;
    }
}

static void prefs_save(void) {
    char d[8];
    snprintf(d, sizeof d, "%d", DELAYS[g_delay_i]);
    uconf_set(PREFS, "mode", MODE_NAME[g_mode]);
    uconf_set(PREFS, "pointer", g_pointer ? "on" : "off");
    uconf_set(PREFS, "copy", g_copy ? "on" : "off");
    uconf_set(PREFS, "delay", d);
}

// --- the pill ---------------------------------------------------------
//
// Laid out from the font, left to right: the three modes (icon over a
// label), a rule, the shutter, a rule, pointer / copy / delay / close.

enum { C_REGION, C_SCREEN, C_WINDOW, C_SHUTTER, C_POINTER, C_COPY, C_DELAY, C_CLOSE, CTLS };
static const char *const CTL_NAME[CTLS] = {
    "region", "screen", "window", "shutter", "pointer", "copy", "delay", "close",
};

static int unit(void) { return ugfx_char_h(); }

static void ctl_rect(int c, struct rect *r) {
    int u = unit();
    int mw = u * 4, mh = u * 3 + u / 2;   // a mode: icon over its label
    int sd = u * 3 + u / 2;               // the shutter's diameter
    int ib = u * 2 + u / 3;               // a small icon button
    int pad = u * 2 / 3, rule = u;
    int total = pad + 3 * mw + rule + sd + rule + 4 * ib + 3 * (u / 4) + pad;
    int x0 = (g_sw - total) / 2;
    int ph = mh + 2 * pad;
    int y0 = g_sh - ph - u * 4;
    int x = x0 + pad;
    for (int k = 0; k < CTLS; k++) {
        struct rect q;
        if (k <= C_WINDOW) { q.x = x; q.y = y0 + pad; q.w = mw; q.h = mh; x += mw; if (k == C_WINDOW) x += rule; }
        else if (k == C_SHUTTER) { q.x = x; q.y = y0 + (ph - sd) / 2; q.w = q.h = sd; x += sd + rule; }
        else { q.x = x; q.y = y0 + (ph - ib) / 2; q.w = q.h = ib; x += ib + u / 4; }
        if (k == c) { *r = q; return; }
    }
}

static void pill_rect(struct rect *r) {
    struct rect a, b;
    ctl_rect(C_REGION, &a);
    ctl_rect(C_CLOSE, &b);
    int pad = unit() * 2 / 3;
    r->x = a.x - pad;
    r->y = a.y - pad;
    r->w = b.x + b.w + pad - r->x;
    r->h = a.h + 2 * pad;
}

static int ctl_at(int x, int y) {
    for (int c = 0; c < CTLS; c++) {
        struct rect r;
        ctl_rect(c, &r);
        if (uui_hit(r.x, r.y, r.w, r.h, x, y)) return c;
    }
    return -1;
}

static int on_pill(int x, int y) {
    struct rect r;
    pill_rect(&r);
    return uui_hit(r.x, r.y, r.w, r.h, x, y);
}

// --- the region's handles ---------------------------------------------

static void handle_xy(const struct rect *s, int k, int *x, int *y) {
    int xs[8] = { s->x, s->x + s->w / 2, s->x + s->w, s->x + s->w, s->x + s->w, s->x + s->w / 2, s->x, s->x };
    int ys[8] = { s->y, s->y, s->y, s->y + s->h / 2, s->y + s->h, s->y + s->h, s->y + s->h, s->y + s->h / 2 };
    *x = xs[k];
    *y = ys[k];
}

static int handle_at(int x, int y) {
    if (g_sel.w <= 0) return -1;
    int r = unit() / 2 + 2;
    for (int k = 0; k < 8; k++) {
        int hx, hy;
        handle_xy(&g_sel, k, &hx, &hy);
        if (x >= hx - r && x <= hx + r && y >= hy - r && y <= hy + r) return k;
    }
    return -1;
}

// --- capture and finish -----------------------------------------------

static void make_dim(void) {
    const uint32_t *f = frame();
    if (g_dim_of == g_pointer && g_dim) return;
    if (!g_dim) g_dim = (uint32_t *)malloc((size_t)g_sw * (size_t)g_sh * 4);
    if (!g_dim) return;
    // A darkened copy, made once per frame shown rather than per paint.
    for (int i = 0; i < g_sw * g_sh; i++) {
        uint32_t p = f[i];
        uint32_t r = ((p >> 16) & 0xFF) * 4 / 10, g = ((p >> 8) & 0xFF) * 4 / 10, b = (p & 0xFF) * 4 / 10;
        g_dim[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    g_dim_of = g_pointer;
}

// The area the shutter would take, or 0 when there is none yet.
static int target(struct rect *r) {
    if (g_mode == MODE_SCREEN) { r->x = r->y = 0; r->w = g_sw; r->h = g_sh; return 1; }
    if (g_mode == MODE_WINDOW) { *r = g_win; return g_win_have && g_win.w > 0; }
    *r = g_sel;
    return g_sel.w >= 2 && g_sel.h >= 2;
}

static int save_crop(const struct rect *r, char *path, int cap) {
    struct ushot *f = &g_shot;
    // The chosen frame back into the capture object, which crops and saves.
    memcpy(f->px, frame(), (size_t)g_sw * (size_t)g_sh * 4);
    f->w = g_sw;
    f->h = g_sh;
    sys_mkdir(SHOT_DIR);
    time_t now = time(NULL);
    struct tm tm;
    char stamp[32];
    if (!(localtime_r(&now, &tm) && strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm)))
        snprintf(stamp, sizeof stamp, "%d", sys_getpid());
    // TWO IN ONE SECOND get -2, -3...: the name is per second, and the
    // first file must not be written over (its card points at it).
    struct sys_stat st;
    snprintf(path, (size_t)cap, SHOT_DIR "/shot-%s.qoi", stamp);
    for (int n = 2; sys_stat(path, &st) == 0 && n < 100; n++)
        snprintf(path, (size_t)cap, SHOT_DIR "/shot-%s-%d.qoi", stamp, n);
    if (r->w != f->w || r->h != f->h) {
        int rc = ushot_crop(f, r->x, r->y, r->w, r->h);
        if (rc < 0) return rc;
    }
    // QOI, which this desktop's Image Viewer opens by default.
    return ushot_save(f, path, NULL);
}

// The shutter: save, copy if asked, the compositor's card, and gone.
static void finish(struct uapp *a) {
    struct rect r;
    if (!target(&r)) return;
    char path[128];
    int rc = save_crop(&r, path, sizeof path);
    if (rc < 0) {
        ulogf("screenshot: could not save: %s\n", ushot_strerror(rc));
        uapp_quit(a, 1);
        return;
    }
    unsigned flags = 0;
    if (g_copy) {
        // THE FILE, as the File Manager's Copy puts one: this clipboard
        // holds files or text, not pixels (lib/uclip.h).
        uclip_begin(0, UCLIP_COPY);
        if (uclip_add(0, path) && uclip_commit(0) > 0) flags |= WIN_NOTICE_F_COPIED;
    }
    ulogf("screenshot: saved %s %dx%d\n", path, r.w, r.h);
    uapp_notice(a, WIN_NOTICE_SCREENSHOT, flags, path);
    prefs_save();
    uapp_quit(a, 0);
}

static void shutter(struct uapp *a) {
    // Not in the copy that already WAITED: its shutter takes the shot,
    // or a delayed capture would relaunch itself for ever.
    if (DELAYS[g_delay_i] > 0 && g_launch_delay <= 0) {
        // A DELAY IS A RELAUNCH (the top of this file): the next copy
        // sleeps with no window and freezes the screen as it is then.
        char args[48];
        snprintf(args, sizeof args, "--delay %d --mode %s", DELAYS[g_delay_i], MODE_NAME[g_mode]);
        prefs_save();
        sys_spawn(SELF_PATH, args, -1);
        uapp_quit(a, 0);
        return;
    }
    finish(a);
}

// --- input ------------------------------------------------------------

static void set_mode(int m) {
    g_mode = m;
    g_win_have = 0;
}

static void activate(struct uapp *a, int c) {
    switch (c) {
    case C_REGION: case C_SCREEN: case C_WINDOW: set_mode(c - C_REGION); break;
    case C_SHUTTER: shutter(a); return;
    case C_POINTER: g_pointer = !g_pointer; break;
    case C_COPY:    g_copy = !g_copy; break;
    case C_DELAY:   g_delay_i = (g_delay_i + 1) % DELAY_COUNT; break;
    case C_CLOSE:   prefs_save(); uapp_quit(a, 0); return;
    }
    uapp_redraw(a);
}

static void probe_window(int x, int y) {
    struct win_shot r;
    if (ushot_probe(&g_shot, WIN_SHOT_WINDOW_AT, x, y, &r) == 0 && r.w > 0) {
        g_win.x = r.x; g_win.y = r.y; g_win.w = r.w; g_win.h = r.h;
        // Clamped to the frame: a window hanging off the screen is
        // captured as much of it as is on it.
        int x1 = clampi(g_win.x + g_win.w, 0, g_sw), y1 = clampi(g_win.y + g_win.h, 0, g_sh);
        g_win.x = clampi(g_win.x, 0, g_sw);
        g_win.y = clampi(g_win.y, 0, g_sh);
        g_win.w = x1 - g_win.x;
        g_win.h = y1 - g_win.y;
        g_win_have = g_win.w > 0 && g_win.h > 0;
    } else {
        g_win_have = 0;
    }
}

static void on_press(struct uapp *a, int x, int y, unsigned mods) {
    unsigned buttons = WIN_MOUSE_BUTTONS(mods);
    if (buttons & 2) { prefs_save(); uapp_quit(a, 0); return; }   // the secondary button closes
    if (on_pill(x, y)) {
        g_pressed_ctl = ctl_at(x, y);
        return;
    }
    if (g_mode == MODE_WINDOW) {
        probe_window(x, y);
        if (g_win_have) shutter(a);
        return;
    }
    if (g_mode == MODE_SCREEN) return;
    g_ax = x;
    g_ay = y;
    g_start = g_sel;
    int h = handle_at(x, y);
    if (h >= 0) { g_drag = DRAG_HANDLE; g_handle = h; }
    else if (g_sel.w > 0 && uui_hit(g_sel.x, g_sel.y, g_sel.w, g_sel.h, x, y)) g_drag = DRAG_MOVE;
    else { g_drag = DRAG_NEW; g_sel.x = x; g_sel.y = y; g_sel.w = g_sel.h = 0; }
    uapp_redraw(a);
}

static void norm(struct rect *r, int x0, int y0, int x1, int y1) {
    if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
    r->x = clampi(x0, 0, g_sw);
    r->y = clampi(y0, 0, g_sh);
    r->w = clampi(x1, 0, g_sw) - r->x;
    r->h = clampi(y1, 0, g_sh) - r->y;
}

static void on_motion(struct uapp *a, int x, int y, unsigned buttons) {
    if (x < 0) return;   // the pointer left
    g_px = x;
    g_py = y;
    int hot = on_pill(x, y) ? ctl_at(x, y) : -1;
    if (hot != g_hot) { g_hot = hot; uapp_redraw(a); }
    if (g_mode == MODE_WINDOW && !on_pill(x, y)) {
        struct rect was = g_win;
        int had = g_win_have;
        probe_window(x, y);
        if (had != g_win_have || memcmp(&was, &g_win, sizeof was)) uapp_redraw(a);
    }
    if (!(buttons & 1) || g_drag == DRAG_NONE) {
        // The cursor says what a press would do.
        int c = WIN_CURSOR_DEFAULT;
        if (g_mode == MODE_REGION && !on_pill(x, y)) {
            int h = handle_at(x, y);
            if (h == 1 || h == 5) c = WIN_CURSOR_RESIZE_V;   // the top and bottom middles
            else if (h >= 0) c = WIN_CURSOR_RESIZE_H;
            else if (g_sel.w > 0 && uui_hit(g_sel.x, g_sel.y, g_sel.w, g_sel.h, x, y)) c = WIN_CURSOR_MOVE;
            else c = WIN_CURSOR_CROSSHAIR;   // a press here starts a new region
        }
        uapp_set_cursor(a, c);
        return;
    }
    int dx = x - g_ax, dy = y - g_ay;
    if (g_drag == DRAG_NEW) {
        norm(&g_sel, g_ax, g_ay, x, y);
    } else if (g_drag == DRAG_MOVE) {
        g_sel.x = clampi(g_start.x + dx, 0, g_sw - g_start.w);
        g_sel.y = clampi(g_start.y + dy, 0, g_sh - g_start.h);
    } else {
        int x0 = g_start.x, y0 = g_start.y, x1 = g_start.x + g_start.w, y1 = g_start.y + g_start.h;
        int k = g_handle;
        if (k == 0 || k == 6 || k == 7) x0 += dx;   // the left edge
        if (k == 2 || k == 3 || k == 4) x1 += dx;   // the right
        if (k == 0 || k == 1 || k == 2) y0 += dy;   // the top
        if (k == 4 || k == 5 || k == 6) y1 += dy;   // the bottom
        norm(&g_sel, x0, y0, x1, y1);
    }
    uapp_redraw(a);
}

static void on_release(struct uapp *a, int x, int y, unsigned buttons) {
    (void)buttons;
    if (g_pressed_ctl >= 0) {
        int c = g_pressed_ctl;
        g_pressed_ctl = -1;
        if (ctl_at(x, y) == c) activate(a, c);   // commit on release, over the same control
        return;
    }
    g_drag = DRAG_NONE;
    if (g_sel.w < 2 || g_sel.h < 2) g_sel.w = g_sel.h = 0;   // a click is no region
    uapp_redraw(a);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    switch (key) {
    case 0x1B: prefs_save(); uapp_quit(a, 0); return;   // Esc closes, saving nothing
    case '\n': case '\r': case ' ': shutter(a); return;
    case 'r': set_mode(MODE_REGION); break;
    case 's': set_mode(MODE_SCREEN); break;
    case 'w': set_mode(MODE_WINDOW); break;
    case 'p': g_pointer = !g_pointer; break;
    case 'c': g_copy = !g_copy; break;
    default: return;
    }
    uapp_redraw(a);
}

// --- drawing ----------------------------------------------------------

static void lit(struct ugfx_surface *s, const struct rect *r) {
    if (r->w <= 0 || r->h <= 0) return;
    ugfx_blit(s, r->x, r->y, r->w, r->h, frame() + (size_t)r->y * (size_t)g_sw + (size_t)r->x, g_sw);
}

static void frame_rect(struct ugfx_surface *s, const struct rect *r) {
    ugfx_draw_rect(s, r->x - 1, r->y - 1, r->w + 2, r->h + 2, UTHEME_WHITE);
    ugfx_draw_rect(s, r->x - 2, r->y - 2, r->w + 4, r->h + 4, ugfx_rgb(0, 0, 0));
}

// Above the selection -- or, while a drag sizes it, beside the pointer,
// where the eye is (Spectacle's shape).
static void size_label(struct ugfx_surface *s, const struct rect *r) {
    char t[32];
    snprintf(t, sizeof t, "%d x %d", r->w, r->h);
    int u = unit(), tw = ugfx_text_width(t) + u, th = u + u / 2;
    int x = r->x + (r->w - tw) / 2, y = r->y - th - u / 2;
    if (y < u / 2) y = r->y + u / 2;
    if (g_drag == DRAG_NEW || g_drag == DRAG_HANDLE) {
        x = g_px + u;
        y = g_py + u;
        if (x + tw > g_sw - 2) x = g_px - u - tw;   // flipped at the edges
        if (y + th > g_sh - 2) y = g_py - u - th;
    }
    x = clampi(x, 2, g_sw - tw - 2);
    y = clampi(y, 2, g_sh - th - 2);
    uui_fill_round_rect(s, x, y, tw, th, UUI_CAPSULE, ugfx_rgb(24, 24, 28));
    ugfx_draw_string(s, x + u / 2, y + (th - ugfx_char_h()) / 2, t, UTHEME_WHITE, ugfx_rgb(24, 24, 28));
}

static void draw_icon(struct ugfx_surface *s, const char *name, int cx, int cy, uint32_t ink) {
    const struct uimg *ico = icon_get(name, unit() + unit() / 4);
    if (ico) ugfx_blit_tinted(s, cx - ico->w / 2, cy - ico->h / 2, ico->w, ico->h, ico->px, ico->w, ink);
}

static void draw_pill(struct ugfx_surface *s) {
    struct rect p;
    pill_rect(&p);
    int u = unit();
    uint32_t ground = ugfx_rgb(244, 244, 246);
    // A soft shadow under the card, then the card.
    for (int k = 4; k >= 1; k--)
        uui_glass_round_rect(s, p.x - k, p.y - k + 3, p.w + 2 * k, p.h + 2 * k, u + k,
                             ugfx_rgb(0, 0, 0), (uint8_t)(18 + 6 * (4 - k)), 0);
    uui_fill_round_rect(s, p.x, p.y, p.w, p.h, u, ground);

    for (int c = 0; c < CTLS; c++) {
        struct rect r;
        ctl_rect(c, &r);
        int on = (c <= C_WINDOW && g_mode == c - C_REGION) ||
                 (c == C_POINTER && g_pointer) || (c == C_COPY && g_copy) ||
                 (c == C_DELAY && g_delay_i > 0);
        int hot = g_hot == c;
        if (c == C_SHUTTER) {
            // A ring and a disc: the camera's button.
            uint32_t ring = hot ? uui_state_bg(UTHEME_ACCENT, UUI_STATE_HOVER) : UTHEME_ACCENT;
            uui_fill_round_rect(s, r.x, r.y, r.w, r.h, UUI_CAPSULE, ring);
            uui_fill_round_rect(s, r.x + 4, r.y + 4, r.w - 8, r.h - 8, UUI_CAPSULE, ground);
            uui_fill_round_rect(s, r.x + 7, r.y + 7, r.w - 14, r.h - 14, UUI_CAPSULE, ring);
            continue;
        }
        uint32_t bg = on ? UTHEME_ACCENT : hot ? uui_state_bg(ground, UUI_STATE_HOVER) : ground;
        uint32_t ink = on ? UTHEME_ACCENT_TEXT : UTHEME_TEXT;
        if (bg != ground) uui_fill_round_rect(s, r.x, r.y, r.w, r.h, u / 2, bg);
        if (c <= C_WINDOW) {
            int m = c - C_REGION;
            draw_icon(s, MODE_ICON[m], r.x + r.w / 2, r.y + r.h / 2 - u / 2, ink);
            int tw = ugfx_text_width(MODE_LABEL[m]);
            ugfx_draw_string(s, r.x + (r.w - tw) / 2, r.y + r.h - u - u / 4, MODE_LABEL[m], ink, bg);
        } else if (c == C_DELAY && g_delay_i > 0) {
            char t[8];
            snprintf(t, sizeof t, "%ds", DELAYS[g_delay_i]);
            int tw = ugfx_text_width(t);
            ugfx_draw_string(s, r.x + (r.w - tw) / 2, r.y + (r.h - ugfx_char_h()) / 2, t, ink, bg);
        } else {
            static const char *const icons[] = { 0, 0, 0, 0, "tb-pointer", "tb-copy", "tb-timer", "tb-close" };
            draw_icon(s, icons[c], r.x + r.w / 2, r.y + r.h / 2, ink);
        }
    }
    // The rules either side of the shutter.
    struct rect w, sh, pt;
    ctl_rect(C_WINDOW, &w);
    ctl_rect(C_SHUTTER, &sh);
    ctl_rect(C_POINTER, &pt);
    uint32_t rule = ugfx_rgb(208, 208, 214);
    ugfx_fill_rect(s, (w.x + w.w + sh.x) / 2, p.y + u, 1, p.h - 2 * u, rule);
    ugfx_fill_rect(s, (sh.x + sh.w + pt.x) / 2, p.y + u, 1, p.h - 2 * u, rule);
}

static void log_layout(void) {
    for (int c = 0; c < CTLS; c++) {
        struct rect r;
        ctl_rect(c, &r);
        uapp_logf_layout("screenshot: layout %s %d %d %d %d\n", CTL_NAME[c], r.x, r.y, r.w, r.h);
    }
    struct rect p, t;
    pill_rect(&p);
    uapp_logf_layout("screenshot: layout pill %d %d %d %d\n", p.x, p.y, p.w, p.h);
    if (target(&t))
        uapp_logf_layout("screenshot: layout selection %d %d %d %d\n", t.x, t.y, t.w, t.h);
    uapp_logf_layout("screenshot: layout mode %s\n", MODE_NAME[g_mode]);
    uapp_logf_layout("screenshot: layout options pointer %d copy %d delay %d\n",
                     g_pointer, g_copy, DELAYS[g_delay_i]);
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    struct ugfx_surface *s = uapp_surface(d);
    if (!g_ok) return;
    // ASKED AGAIN UNTIL IT TAKES: on_open runs before the loop does, and
    // the request is refused until then (uapp.c).
    uapp_set_fullscreen(a, 1);
    make_dim();
    struct rect r;
    int have = target(&r);
    if (g_mode == MODE_SCREEN) {
        lit(s, &r);
    } else {
        if (g_dim) ugfx_blit(s, 0, 0, g_sw, g_sh, g_dim, g_sw);
        if (have) {
            lit(s, &r);
            frame_rect(s, &r);
            size_label(s, &r);
        }
        if (g_mode == MODE_REGION && have && g_drag != DRAG_NEW) {
            int hr = unit() / 3 + 1;
            for (int k = 0; k < 8; k++) {
                int hx, hy;
                handle_xy(&g_sel, k, &hx, &hy);
                uui_fill_round_rect(s, hx - hr - 1, hy - hr - 1, 2 * hr + 2, 2 * hr + 2, UUI_CAPSULE,
                                    ugfx_rgb(0, 0, 0));
                uui_fill_round_rect(s, hx - hr, hy - hr, 2 * hr, 2 * hr, UUI_CAPSULE, UTHEME_WHITE);
            }
        }
        if (!have) {
            const char *hint = g_mode == MODE_REGION ? "Drag to select an area"
                                                     : "Point at a window and click it";
            int tw = ugfx_text_width(hint);
            ugfx_draw_string_shadowed(s, (g_sw - tw) / 2, g_sh / 3, hint, UTHEME_WHITE);
        }
    }
    draw_pill(s);
    log_layout();
}

// --- life -------------------------------------------------------------


static void on_open(struct uapp *a) {
    g_app = a;
    if (ushot_open_on(&g_shot, uapp_wmchan()) < 0) {
        ulog("screenshot: no compositor to capture from\n");
        uapp_quit(a, 1);
        return;
    }
    // BOTH FRAMES BEFORE ANYTHING IS SHOWN, without this window in them.
    static const unsigned how[2] = { WIN_SHOT_NO_SELF, WIN_SHOT_NO_SELF | WIN_SHOT_POINTER };
    for (int k = 0; k < 2; k++) {
        int rc = ushot_take(&g_shot, WIN_SHOT_SCREEN, how[k], 0, 0, 0, 0);
        size_t bytes = (size_t)g_shot.w * (size_t)g_shot.h * 4;
        if (rc == 0 && !g_fr[k]) g_fr[k] = (uint32_t *)malloc(bytes);
        if (rc < 0 || !g_fr[k]) {
            ulogf("screenshot: %s\n", rc < 0 ? ushot_strerror(rc) : "out of memory");
            uapp_quit(a, 1);
            return;
        }
        memcpy(g_fr[k], g_shot.px, bytes);
    }
    g_sw = g_shot.w;
    g_sh = g_shot.h;
    g_ok = 1;
    // A delayed SCREEN capture needs no choosing: that frame is the shot.
    if (g_launch_delay > 0 && g_mode == MODE_SCREEN) { finish(a); return; }
    uapp_set_fullscreen(a, 1);
    ulogf("screenshot: overlay %dx%d mode %s\n", g_sw, g_sh, MODE_NAME[g_mode]);
    uapp_redraw(a);
}

static int on_close(struct uapp *a) {
    (void)a;
    ushot_close(&g_shot);
    free(g_fr[0]);
    free(g_fr[1]);
    free(g_dim);
    return 1;
}

// --- Save as (the card's fourth button) --------------------------------
//
// `--save-as PATH`: a small window showing the picture, the shared
// chooser over it, and a COPY written where it says -- the original
// stays, because it is the folder's record of the capture. The format is
// the new name's extension (uimg_save()).

static char g_save_src[256];
static struct uimg g_src, g_preview;
static struct uui_filedialog g_chooser;
static int g_chooser_asked;
static char g_save_note[128];

static int keep_images(void *ctx, const char *dir, const struct sys_dirent *e) {
    (void)ctx; (void)dir;
    if (e->is_dir) return 1;
    const char *dot = strrchr(e->name, '.');
    return dot && (strcmp(dot, ".qoi") == 0 || strcmp(dot, ".png") == 0);
}

static const struct uui_filedialog_filter SAVE_FILTERS[] = {
    { "Images (.qoi .png)", keep_images, 0 },
    UUI_FILEDIALOG_ALL_FILES,
};

static void save_chosen(void *ctx, const char *path);

static void ask_where(struct uapp *a) {
    char dir[sizeof g_save_src];
    if (!k_path_dirname(g_save_src, dir, sizeof dir)) snprintf(dir, sizeof dir, "%s", SHOT_DIR);
    struct uui_filedialog_opts o = {
        .mode = UUI_FILEDIALOG_SAVE,
        .title = "Save Screenshot As",
        .start_dir = dir,
        .initial_name = k_path_basename(g_save_src),
        .filters = SAVE_FILTERS,
        .filter_count = (int)(sizeof SAVE_FILTERS / sizeof SAVE_FILTERS[0]),
    };
    if (!uui_filedialog_open(a, &g_chooser, &o, save_chosen, a)) uapp_quit(a, 1);
}

static void save_chosen(void *ctx, const char *path) {
    struct uapp *a = (struct uapp *)ctx;
    if (!path || strcmp(path, g_save_src) == 0) { uapp_quit(a, 0); return; }
    int rc = uimg_save(path, &g_src, NULL);
    if (rc == 0) {
        ulogf("screenshot: saved as %s\n", path);
        uapp_quit(a, 0);
        return;
    }
    // Asked again, with the reason under the picture: a refused name is
    // not the end of the save.
    snprintf(g_save_note, sizeof g_save_note, rc == -ENOTSUP ? "Name it .qoi or .png"
             : "Could not save %s", k_path_basename(path));
    ulogf("screenshot: save as %s failed (%d)\n", path, rc);
    uapp_redraw(a);
    ask_where(a);
}

static void save_open(struct uapp *a) {
    if (uimg_load(g_save_src, &g_src) < 0) {
        ulogf("screenshot: cannot read %s\n", g_save_src);
        uapp_quit(a, 1);
    }
}

static void save_draw(struct uapp *a, struct uapp_draw *d) {
    struct ugfx_surface *s = uapp_surface(d);
    int u = unit();
    ugfx_fill_rect(s, 0, 0, s->w, s->h, UTHEME_WINDOW_BG);
    int bw = s->w - 2 * u, bh = s->h - 3 * u;
    if (g_src.px && bw > 0 && bh > 0) {
        int pw, ph;
        uimg_fit_size(g_src.w, g_src.h, bw, bh, UIMG_FIT_CONTAIN, &pw, &ph);
        if (g_preview.w != pw || g_preview.h != ph) {
            uimg_free(&g_preview);
            uimg_scale(&g_src, pw, ph, &g_preview);
        }
        if (g_preview.px) {
            int x = (s->w - pw) / 2, y = u + (bh - ph) / 2;
            ugfx_draw_rect(s, x - 1, y - 1, pw + 2, ph + 2, UTHEME_OUTLINE);
            ugfx_blit(s, x, y, pw, ph, g_preview.px, pw);
        }
    }
    const char *note = g_save_note[0] ? g_save_note : k_path_basename(g_save_src);
    ugfx_draw_string_elided(s, u, s->h - u - u / 2, s->w - 2 * u, note,
                            g_save_note[0] ? utheme_action(UTHEME_ACT_DANGER) : UTHEME_TEXT,
                            UTHEME_WINDOW_BG);
    // ASKED FROM THE FIRST FRAME, not on_open: the loop must be running
    // for the compositor to take a second window.
    if (!g_chooser_asked) { g_chooser_asked = 1; ask_where(a); }
}

// From the font: a 16:9 preview over one line of note. No Esc to close
// it (userland/CLAUDE.md): Cancel in the chooser, or its X, ends it.
static void save_size(int *w, int *h) {
    *w = 36 * ugfx_char_advance('n');
    *h = *w * 9 / 16 + 3 * ugfx_char_h();
}

static int save_close(struct uapp *a) {
    (void)a;
    if (uui_filedialog_is_open(&g_chooser)) uui_filedialog_close_window(&g_chooser);
    uimg_free(&g_preview);
    uimg_free(&g_src);
    return 1;
}

static int save_as_main(void) {
    struct uapp_desc desc = {
        .title    = "Save Screenshot",
        .app_id   = "screenshot",
        .on_size  = save_size,
        .on_open  = save_open,
        .on_draw  = save_draw,
        .on_close = save_close,
    };
    return uapp_run(&desc);
}

int main(int argc, char **argv) {
    // The card's Save as: the rest of the line is the path.
    if (argc > 2 && !strcmp(argv[1], "--save-as")) {
        size_t n = 0;
        for (int i = 2; i < argc; i++)
            n += (size_t)snprintf(g_save_src + n, n < sizeof g_save_src ? sizeof g_save_src - n : 0,
                                  "%s%s", i > 2 ? " " : "", argv[i]);
        if (n >= sizeof g_save_src) return 1;
        return save_as_main();
    }
    prefs_load();
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--delay")) g_launch_delay = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mode"))
            for (int m = 0, j = ++i; m < MODES; m++) if (!strcmp(argv[j], MODE_NAME[m])) g_launch_mode = m;
    }
    if (g_launch_mode >= 0) g_mode = g_launch_mode;
    // THE DELAY HAPPENS BEFORE THERE IS A WINDOW, so nothing is drawn as
    // Not Responding and nothing of this app is on the screen meanwhile.
    if (g_launch_delay > 0 && g_launch_delay <= 60) sleep((unsigned)g_launch_delay);

    struct uapp_desc desc = {
        .title      = "Screenshot",
        .app_id     = "screenshot",
        .w          = 320,
        .h          = 200,
        // RESIZABLE because the compositor only lets a resizable window
        // go fullscreen; the overlay IS the fullscreen window.
        .flags      = UAPP_SINGLE_INSTANCE | UAPP_RESIZABLE,
        .on_open    = on_open,
        .on_draw    = on_draw,
        .on_press   = on_press,
        .on_motion  = on_motion,
        .on_release = on_release,
        .on_key     = on_key,
        .on_close   = on_close,
    };
    return uapp_run(&desc);
}
