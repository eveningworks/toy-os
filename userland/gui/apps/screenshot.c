// Screenshot -- the shell's capture overlay, GNOME 42's shape (mockup
// S3, 2026-10-04): PrtSc freezes the screen, dims it, and puts a pill at
// the bottom -- Region / Screen / Window, the shutter, the pointer, copy
// to the clipboard, a delay, more, options, close. There is no window
// to manage: the shutter saves where the options say, the compositor
// puts up a card with the picture and Open / Copy / Folder / Save as
// (WIN_REQ_NOTICE), and the app is gone. /bin/screenshot is the other
// front end on lib/ushot.h. The card's Save as comes back here as
// `--save-as PATH` (the bottom of this file).
//
// THE OPTIONS (userland/screenshot/): the gear opens the Options window
// (shot_prefs.c), the "..." a popover of the common ones drawn on the
// overlay itself (mockups O1 + P1, 2026-10-07). `--now screen|window`
// is Shift/Alt+PrtSc: the picture is taken with no overlay at all.
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
#include "lib/uopen.h"
#include "lib/ushot.h"
#include "keyboard.h"
#include "kpath.h"   // k_path_basename/_dirname -- the kernel's, linked into ring 3
#include "lib/uimg.h"
#include "ui/uui_filedialog.h"
#include "screenshot/screenshot.h"
#include "ui/uui_prefs.h"

#define SELF_PATH "/bin/wm/apps/screenshot"

enum { MODE_REGION = SHOT_MODE_REGION, MODE_SCREEN = SHOT_MODE_SCREEN,
       MODE_WINDOW = SHOT_MODE_WINDOW, MODES = SHOT_MODES };
#define MODE_NAME SHOT_MODE_NAME
static const char *const MODE_LABEL[MODES] = { "Region", "Screen", "Window" };
static const char *const MODE_ICON[MODES] = { "tb-region", "tb-screen", "tb-window" };

// Everything the options say, the pill's own state among it.
static struct shot_conf g_conf;
static int g_mode = MODE_REGION;
#define g_pointer g_conf.pointer
#define g_copy    g_conf.copy

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
static char g_win_app[16];        // ...and its app id, for <app>
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

static void prefs_save(void) {
    g_conf.mode = g_mode;
    if (g_sel.w >= 2 && g_sel.h >= 2) {
        g_conf.rx = g_sel.x; g_conf.ry = g_sel.y; g_conf.rw = g_sel.w; g_conf.rh = g_sel.h;
        g_conf.rsw = g_sw; g_conf.rsh = g_sh;
    }
    shot_conf_save(&g_conf);
}

// The timer cycles off, then the three the options offer.
static void next_delay(void) {
    int i = 0;
    while (i < SHOT_DELAYS && g_conf.delays[i] != g_conf.delay) i++;
    g_conf.delay = g_conf.delay == 0 ? g_conf.delays[0] : i + 1 < SHOT_DELAYS ? g_conf.delays[i + 1] : 0;
}

// --- the pill ---------------------------------------------------------
//
// Laid out from the font, left to right: the three modes (icon over a
// label), a rule, the shutter, a rule, pointer / copy / delay / more /
// options / close.

enum { C_REGION, C_SCREEN, C_WINDOW, C_SHUTTER, C_POINTER, C_COPY, C_DELAY, C_MORE, C_GEAR,
       C_CLOSE, CTLS };
#define SMALL_CTLS (CTLS - C_POINTER)
static const char *const CTL_NAME[CTLS] = {
    "region", "screen", "window", "shutter", "pointer", "copy", "delay", "more", "gear", "close",
};

static int unit(void) { return ugfx_char_h(); }

static void ctl_rect(int c, struct rect *r) {
    int u = unit();
    int mw = u * 4, mh = u * 3 + u / 2;   // a mode: icon over its label
    int sd = u * 3 + u / 2;               // the shutter's diameter
    int ib = u * 2 + u / 3;               // a small icon button
    int pad = u * 2 / 3, rule = u;
    int total = pad + 3 * mw + rule + sd + rule + SMALL_CTLS * ib + (SMALL_CTLS - 1) * (u / 4) + pad;
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

// --- the popover (P1) -------------------------------------------------
//
// The options a capture changes most, on the overlay itself, above the
// "..." -- each takes effect at once and is kept with the rest when the
// overlay closes. Everything else is in the Options window.

enum { P_HEAD, P_CHECK, P_SEG, P_FOLDER, P_SEP };
enum { PI_CARD, PI_COPY, PI_OPEN, PI_FORMAT, PI_FOLDER, PI_LASTREG, PI_SHADOW, PI_NONE };
static const struct pop_row {
    int kind;
    int item;          // PI_*, or PI_NONE for a heading or a rule
    const char *label;
    const char *name;  // the layout log's
} POP[] = {
    { P_HEAD,   PI_NONE,    "After capture", 0 },
    { P_CHECK,  PI_CARD,    "Show the card", "card" },
    { P_CHECK,  PI_COPY,    "Copy to the clipboard", "copy" },
    { P_CHECK,  PI_OPEN,    "Open in Image Viewer", "open" },
    { P_SEP,    PI_NONE,    0, 0 },
    { P_HEAD,   PI_NONE,    "Save as", 0 },
    { P_SEG,    PI_FORMAT,  0, "format" },
    { P_FOLDER, PI_FOLDER,  "Change...", "folder" },
    { P_SEP,    PI_NONE,    0, 0 },
    { P_CHECK,  PI_LASTREG, "Start from the last region", "lastregion" },
    { P_CHECK,  PI_SHADOW,  "Include window shadows", "shadow" },
};
#define POP_ROWS ((int)(sizeof POP / sizeof POP[0]))
static const char *const FORMAT_SEG[3] = { "QOI", "PNG", "Ask" };

static int g_pop;                 // the popover is up
static int g_pop_hot = -1, g_pop_pressed = -1;
static int g_pop_seg = -1;        // the segment a press on the format row armed

static int pop_row_h(int k) {
    int u = unit();
    return POP[k].kind == P_SEP ? u / 2 + 1 : POP[k].kind == P_HEAD ? u + u / 3 : u * 2;
}

static void pop_rect(struct rect *r) {
    int u = unit();
    int w = ugfx_text_width("Start from the last region") + u * 4;
    int h = u / 2 * 2;
    for (int k = 0; k < POP_ROWS; k++) h += pop_row_h(k);
    struct rect more, pill;
    ctl_rect(C_MORE, &more);
    pill_rect(&pill);
    r->w = w;
    r->h = h;
    r->x = clampi(more.x + more.w - w + u, 4, g_sw - w - 4);
    r->y = pill.y - h - u / 2;
}

static void pop_row_rect(int k, struct rect *r) {
    struct rect p;
    pop_rect(&p);
    int y = p.y + unit() / 2;
    for (int i = 0; i < k; i++) y += pop_row_h(i);
    r->x = p.x; r->y = y; r->w = p.w; r->h = pop_row_h(k);
}

// The three segments of the format row.
static void seg_rect(int s, struct rect *r) {
    struct rect row;
    pop_row_rect(6, &row);
    int u = unit(), x0 = row.x + u, w = (row.w - 2 * u) / 3;
    r->x = x0 + s * w; r->y = row.y + u / 4; r->w = w; r->h = row.h - u / 2;
}

static int pop_at(int x, int y, int *seg) {
    struct rect p;
    pop_rect(&p);
    if (!uui_hit(p.x, p.y, p.w, p.h, x, y)) return -1;
    for (int k = 0; k < POP_ROWS; k++) {
        struct rect r;
        pop_row_rect(k, &r);
        if (!uui_hit(r.x, r.y, r.w, r.h, x, y) || POP[k].item == PI_NONE) continue;
        if (seg) {
            *seg = -1;
            for (int s = 0; s < 3 && POP[k].kind == P_SEG; s++) {
                struct rect q;
                seg_rect(s, &q);
                if (uui_hit(q.x, q.y, q.w, q.h, x, y)) *seg = s;
            }
        }
        return k;
    }
    return POP_ROWS;   // inside, on no control
}

static int pop_checked(int item) {
    switch (item) {
    case PI_CARD: return g_conf.card;
    case PI_COPY: return g_conf.copy;
    case PI_OPEN: return g_conf.open;
    case PI_LASTREG: return g_conf.last_region;
    case PI_SHADOW: return g_conf.shadow;
    }
    return 0;
}

static int format_seg(void) { return g_conf.ask ? 2 : g_conf.png ? 1 : 0; }

static void open_options(struct uapp *a, int page);

static void pop_activate(struct uapp *a, int k, int seg) {
    switch (POP[k].item) {
    case PI_CARD: g_conf.card = !g_conf.card; break;
    case PI_COPY: g_conf.copy = !g_conf.copy; break;
    case PI_OPEN: g_conf.open = !g_conf.open; break;
    case PI_LASTREG: g_conf.last_region = !g_conf.last_region; break;
    case PI_SHADOW: g_conf.shadow = !g_conf.shadow; g_win_have = 0; break;
    case PI_FORMAT:
        if (seg == 2) g_conf.ask = 1;
        else if (seg >= 0) { g_conf.ask = 0; g_conf.png = seg == 1; }
        break;
    case PI_FOLDER: g_pop = 0; open_options(a, SHOT_PAGE_SAVING); return;
    }
    ulogf("screenshot: option %s now card %d copy %d open %d format %s lastregion %d shadow %d\n",
          POP[k].name, g_conf.card, g_conf.copy, g_conf.open, FORMAT_SEG[format_seg()],
          g_conf.last_region, g_conf.shadow);
}

static void draw_check(struct ugfx_surface *s, int x, int y, int sz, int on, uint32_t ground) {
    uint32_t edge = on ? UTHEME_ACCENT : UTHEME_OUTLINE;
    uui_fill_round_rect(s, x, y, sz, sz, 3, edge);
    uui_fill_round_rect(s, x + 1, y + 1, sz - 2, sz - 2, 2, on ? UTHEME_ACCENT : ground);
    if (on) {
        // A tick, two strokes.
        uint32_t ink = UTHEME_ACCENT_TEXT;
        for (int i = 0; i < sz / 4; i++) ugfx_fill_rect(s, x + sz / 4 + i, y + sz / 2 + i - 1, 2, 2, ink);
        for (int i = 0; i < sz / 2; i++)
            ugfx_fill_rect(s, x + sz / 4 + sz / 4 + i, y + sz / 2 + sz / 4 - i - 2, 2, 2, ink);
    }
}

static void draw_pop(struct ugfx_surface *s) {
    struct rect p;
    pop_rect(&p);
    int u = unit();
    uint32_t ground = ugfx_rgb(244, 244, 246);
    for (int k = 4; k >= 1; k--)
        uui_glass_round_rect(s, p.x - k, p.y - k + 3, p.w + 2 * k, p.h + 2 * k, u / 2 + k,
                             ugfx_rgb(0, 0, 0), (uint8_t)(18 + 6 * (4 - k)), 0);
    uui_fill_round_rect(s, p.x, p.y, p.w, p.h, u / 2, ground);
    for (int k = 0; k < POP_ROWS; k++) {
        struct rect r;
        pop_row_rect(k, &r);
        int ty = r.y + (r.h - ugfx_char_h()) / 2;
        uint32_t bg = ground;
        if ((POP[k].kind == P_CHECK || POP[k].kind == P_FOLDER) && (g_pop_hot == k || g_pop_pressed == k)) {
            bg = uui_state_bg(ground, g_pop_pressed == k ? UUI_STATE_PRESSED : UUI_STATE_HOVER);
            ugfx_fill_rect(s, r.x, r.y, r.w, r.h, bg);
        }
        switch (POP[k].kind) {
        case P_SEP:
            ugfx_fill_rect(s, r.x, r.y + r.h / 2, r.w, 1, ugfx_rgb(208, 208, 214));
            break;
        case P_HEAD:
            ugfx_draw_string_clipped(s, r.x + u, ty, r.w - 2 * u, POP[k].label,
                                     ugfx_rgb(96, 96, 104), bg);
            break;
        case P_CHECK: {
            int sz = u;
            draw_check(s, r.x + u, r.y + (r.h - sz) / 2, sz, pop_checked(POP[k].item), bg);
            ugfx_draw_string_clipped(s, r.x + u * 2 + u / 2, ty, r.w - u * 3, POP[k].label, UTHEME_TEXT, bg);
            break;
        }
        case P_SEG:
            for (int sg = 0; sg < 3; sg++) {
                struct rect q;
                seg_rect(sg, &q);
                int on = format_seg() == sg;
                uint32_t f = on ? UTHEME_ACCENT : UTHEME_WHITE;
                ugfx_fill_rect(s, q.x, q.y, q.w, q.h, UTHEME_OUTLINE);
                ugfx_fill_rect(s, q.x + (sg ? 0 : 1), q.y + 1, q.w - (sg ? 1 : 2), q.h - 2, f);
                int tw = ugfx_text_width(FORMAT_SEG[sg]);
                ugfx_draw_string(s, q.x + (q.w - tw) / 2, q.y + (q.h - ugfx_char_h()) / 2, FORMAT_SEG[sg],
                                 on ? UTHEME_ACCENT_TEXT : UTHEME_TEXT, f);
            }
            break;
        case P_FOLDER: {
            int lw = ugfx_text_width(POP[k].label);
            ugfx_draw_string_elided(s, r.x + u, ty, r.w - 3 * u - lw, g_conf.folder, UTHEME_TEXT, bg);
            ugfx_draw_string(s, r.x + r.w - u - lw, ty, POP[k].label, UTHEME_ACCENT, bg);
            break;
        }
        }
    }
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

// The capture object, cropped to `r` when it is not the whole of it,
// written to the next free name the options give.
static int save_held(const struct rect *r, int mode, const char *app, char *path, int cap) {
    struct ushot *f = &g_shot;
    sys_mkdir(g_conf.folder);
    if (!shot_next_path(&g_conf, app, mode, path, cap)) return -EINVAL;
    if (r && (r->x || r->y || r->w != f->w || r->h != f->h)) {
        int rc = ushot_crop(f, r->x, r->y, r->w, r->h);
        if (rc < 0) return rc;
    }
    return ushot_save(f, path, NULL);   // the format from the extension
}

// What happens to a saved file: the clipboard, the card and the flash,
// the viewer -- or, when the options say to ask, the Save as window,
// which then owns the rest.
static void after_save(const char *path, int w, int h) {
    ulogf("screenshot: saved %s %dx%d\n", path, w, h);
    if (g_conf.ask) {
        if (g_conf.flash) uapp_notice(g_app, WIN_NOTICE_SCREENSHOT, WIN_NOTICE_F_NO_CARD | WIN_NOTICE_F_FLASH, path);
        char args[SHOT_PATH_MAX + 32];
        snprintf(args, sizeof args, "--save-as --move %s", path);
        sys_spawn(SELF_PATH, args, -1);
        return;
    }
    unsigned flags = 0;
    if (g_copy) {
        // THE FILE, as the File Manager's Copy puts one: this clipboard
        // holds files or text, not pixels (lib/uclip.h).
        uclip_begin(0, UCLIP_COPY);
        if (uclip_add(0, path) && uclip_commit(0) > 0) flags |= WIN_NOTICE_F_COPIED;
    }
    if (!g_conf.card) flags |= WIN_NOTICE_F_NO_CARD;
    if (g_conf.flash) flags |= WIN_NOTICE_F_FLASH;
    if (g_conf.card || g_conf.flash) uapp_notice(g_app, WIN_NOTICE_SCREENSHOT, flags, path);
    if (g_conf.open && uopen_spawn(path) < 0) ulogf("screenshot: nothing opens %s\n", path);
}

// The shutter: save, everything after, and gone.
static void finish(struct uapp *a) {
    struct rect r;
    if (!target(&r)) return;
    // The chosen frame back into the capture object, which crops and saves.
    memcpy(g_shot.px, frame(), (size_t)g_sw * (size_t)g_sh * 4);
    g_shot.w = g_sw;
    g_shot.h = g_sh;
    char path[SHOT_PATH_MAX];
    int rc = save_held(&r, g_mode, g_mode == MODE_WINDOW ? g_win_app : "", path, sizeof path);
    if (rc < 0) {
        ulogf("screenshot: could not save: %s\n", ushot_strerror(rc));
        uapp_quit(a, 1);
        return;
    }
    prefs_save();
    after_save(path, r.w, r.h);
    uapp_quit(a, 0);
}

static void shutter(struct uapp *a) {
    // Not in the copy that already WAITED: its shutter takes the shot,
    // or a delayed capture would relaunch itself for ever.
    if (g_conf.delay > 0 && g_launch_delay <= 0) {
        // A DELAY IS A RELAUNCH (the top of this file): the next copy
        // sleeps with no window and freezes the screen as it is then.
        char args[48];
        snprintf(args, sizeof args, "--delay %d --mode %s", g_conf.delay, MODE_NAME[g_mode]);
        prefs_save();
        sys_spawn(SELF_PATH, args, -1);
        uapp_quit(a, 0);
        return;
    }
    finish(a);
}

// --- the Options window -----------------------------------------------

static void options_done(const struct shot_conf *next) {
    // The dialog edits what it shows; the pill's state and the last
    // region are this run's, not the dialog's.
    struct shot_conf keep = g_conf;
    g_conf = *next;
    g_conf.copy = next->copy;
    g_conf.delay = keep.delay;
    if (g_conf.delay && memcmp(g_conf.delays, keep.delays, sizeof keep.delays)) g_conf.delay = g_conf.delays[0];
    prefs_save();
    g_win_have = 0;
    if (g_app) uapp_redraw(g_app);
}

static void open_options(struct uapp *a, int page) {
    prefs_save();   // what the pill and the popover changed, so the dialog shows it
    shot_prefs_open(a, &g_conf, page, options_done);
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
    case C_DELAY:   next_delay(); break;
    case C_MORE:    g_pop = !g_pop; g_pop_hot = -1; break;
    case C_GEAR:    g_pop = 0; open_options(a, SHOT_PAGE_CAPTURE); break;
    case C_CLOSE:   prefs_save(); uapp_quit(a, 0); return;
    }
    uapp_redraw(a);
}

static void probe_window(int x, int y) {
    struct win_shot r;
    unsigned how = g_conf.shadow ? WIN_SHOT_SHADOW : 0;
    if (ushot_probe(&g_shot, WIN_SHOT_WINDOW_AT, how, x, y, &r) == 0 && r.w > 0) {
        g_win.x = r.x; g_win.y = r.y; g_win.w = r.w; g_win.h = r.h;
        memcpy(g_win_app, r.app, sizeof g_win_app);
        g_win_app[sizeof g_win_app - 1] = '\0';
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
    if (shot_prefs_is_open()) return;
    unsigned buttons = WIN_MOUSE_BUTTONS(mods);
    if (buttons & 2) {   // the secondary button closes the popover, then the overlay
        if (g_pop) { g_pop = 0; uapp_redraw(a); return; }
        prefs_save(); uapp_quit(a, 0); return;
    }
    if (g_pop) {
        int seg = -1, k = pop_at(x, y, &seg);
        if (k >= 0) {   // on the popover: armed, acted on at the release
            g_pop_pressed = k < POP_ROWS ? k : -1;
            g_pop_seg = seg;
            uapp_redraw(a);
            return;
        }
        if (!on_pill(x, y) || ctl_at(x, y) != C_MORE) {
            // A press elsewhere closes it and does nothing else, as a
            // menu's dismissing click does.
            g_pop = 0;
            uapp_redraw(a);
            return;
        }
    }
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
    if (g_pop) {
        int k = pop_at(x, y, 0);
        int hot = k >= 0 && k < POP_ROWS ? k : -1;
        if (hot != g_pop_hot) { g_pop_hot = hot; uapp_redraw(a); }
        if (k >= 0) { uapp_set_cursor(a, WIN_CURSOR_DEFAULT); return; }
    }
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
    if (g_pop_pressed >= 0) {
        int k = g_pop_pressed, seg = -1;
        g_pop_pressed = -1;
        // Over the same row (and segment) it was pressed on.
        if (pop_at(x, y, &seg) == k && (POP[k].kind != P_SEG || (seg >= 0 && seg == g_pop_seg)))
            pop_activate(a, k, seg);
        uapp_redraw(a);
        return;
    }
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
    if (shot_prefs_is_open()) return;
    switch (key) {
    case 0x1B:   // Esc closes the popover, then the overlay
        if (g_pop) { g_pop = 0; break; }
        prefs_save(); uapp_quit(a, 0); return;
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

static int sizing(void) { return g_drag == DRAG_NEW || g_drag == DRAG_HANDLE; }

// Above the selection -- or, while a drag sizes it, beside the pointer,
// where the eye is (Spectacle's shape).
static void size_label(struct ugfx_surface *s, const struct rect *r) {
    char t[32];
    snprintf(t, sizeof t, "%d x %d", r->w, r->h);
    int u = unit(), tw = ugfx_text_width(t) + u, th = u + u / 2;
    int x = r->x + (r->w - tw) / 2, y = r->y - th - u / 2;
    if (y < u / 2) y = r->y + u / 2;
    if (sizing()) {
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

// THE MAGNIFIER, while an edge is being placed: the pixels round the
// pointer at eight times, with a crosshair on the one it is on --
// Spectacle's and Snipping Tool's loupe. Up and to the left of the
// pointer, the size label having the other side.
#define MAG_SRC 15
#define MAG_ZOOM 8
static void magnifier(struct ugfx_surface *s) {
    int side = MAG_SRC * MAG_ZOOM, u = unit();
    int x = g_px - u - side, y = g_py - u - side;
    if (x < 2) x = g_px + u;
    if (y < 2) y = g_py + u * 3;
    x = clampi(x, 2, g_sw - side - 2);
    y = clampi(y, 2, g_sh - side - 2);
    ugfx_fill_rect(s, x - 2, y - 2, side + 4, side + 4, ugfx_rgb(0, 0, 0));
    ugfx_fill_rect(s, x - 1, y - 1, side + 2, side + 2, UTHEME_WHITE);
    const uint32_t *f = frame();
    for (int j = 0; j < MAG_SRC; j++)
        for (int i = 0; i < MAG_SRC; i++) {
            int sx = g_px - MAG_SRC / 2 + i, sy = g_py - MAG_SRC / 2 + j;
            uint32_t c = sx >= 0 && sy >= 0 && sx < g_sw && sy < g_sh
                       ? f[(size_t)sy * (size_t)g_sw + (size_t)sx] & 0xFFFFFF : 0;
            ugfx_fill_rect(s, x + i * MAG_ZOOM, y + j * MAG_ZOOM, MAG_ZOOM, MAG_ZOOM, c);
        }
    int c0 = (MAG_SRC / 2) * MAG_ZOOM;
    ugfx_draw_rect(s, x + c0 - 1, y + c0 - 1, MAG_ZOOM + 2, MAG_ZOOM + 2, ugfx_rgb(0, 0, 0));
    ugfx_draw_rect(s, x + c0, y + c0, MAG_ZOOM, MAG_ZOOM, UTHEME_WHITE);
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
                 (c == C_DELAY && g_conf.delay > 0) || (c == C_MORE && g_pop);
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
        } else if (c == C_DELAY && g_conf.delay > 0) {
            char t[8];
            snprintf(t, sizeof t, "%ds", g_conf.delay);
            int tw = ugfx_text_width(t);
            ugfx_draw_string(s, r.x + (r.w - tw) / 2, r.y + (r.h - ugfx_char_h()) / 2, t, ink, bg);
        } else {
            static const char *const icons[CTLS] = {
                0, 0, 0, 0, "tb-pointer", "tb-copy", "tb-timer", "tb-more", "tb-gear", "tb-close",
            };
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
                     g_pointer, g_copy, g_conf.delay);
    uapp_logf_layout("screenshot: layout popover %d\n", g_pop);
    if (g_pop) {
        for (int k = 0; k < POP_ROWS; k++) {
            if (!POP[k].name) continue;
            struct rect r;
            pop_row_rect(k, &r);
            uapp_logf_layout("screenshot: layout pop.%s %d %d %d %d\n", POP[k].name, r.x, r.y, r.w, r.h);
        }
        for (int sg = 0; sg < 3; sg++) {
            struct rect q;
            seg_rect(sg, &q);
            uapp_logf_layout("screenshot: layout pop.seg%d %d %d %d %d\n", sg, q.x, q.y, q.w, q.h);
        }
    }
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
            if (g_conf.size_label) size_label(s, &r);
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
        if (g_mode == MODE_REGION && g_conf.magnifier && sizing()) magnifier(s);
        if (!have) {
            const char *hint = g_mode == MODE_REGION ? "Drag to select an area"
                                                     : "Point at a window and click it";
            int tw = ugfx_text_width(hint);
            ugfx_draw_string_shadowed(s, (g_sw - tw) / 2, g_sh / 3, hint, UTHEME_WHITE);
        }
    }
    draw_pill(s);
    if (g_pop) draw_pop(s);
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
    // THE LAST REGION, on the screen it was drawn on: on another size it
    // could hang off the edge, so it is not offered there.
    if (g_conf.last_region && g_conf.rsw == g_sw && g_conf.rsh == g_sh && g_conf.rw >= 2 &&
        g_conf.rh >= 2 && g_conf.rx + g_conf.rw <= g_sw && g_conf.ry + g_conf.rh <= g_sh) {
        g_sel.x = g_conf.rx; g_sel.y = g_conf.ry; g_sel.w = g_conf.rw; g_sel.h = g_conf.rh;
    }
    // A delayed SCREEN capture needs no choosing: that frame is the shot.
    if (g_launch_delay > 0 && g_mode == MODE_SCREEN) { finish(a); return; }
    uapp_set_fullscreen(a, 1);
    ulogf("screenshot: overlay %dx%d mode %s\n", g_sw, g_sh, MODE_NAME[g_mode]);
    uapp_redraw(a);
}

static int on_close(struct uapp *a) {
    (void)a;
    if (shot_prefs_is_open()) uui_prefs_close();
    ushot_close(&g_shot);
    free(g_fr[0]);
    free(g_fr[1]);
    free(g_dim);
    return 1;
}

// --- Shift/Alt+PrtSc: the picture with no overlay -------------------------
//
// `--now screen|window`: one capture, saved, everything after, and gone --
// no window is ever made, so there is nothing of this program to hide.
// The window is the topmost one (WIN_SHOT_WINDOW), the one with the focus
// when the key was pressed.
static int now_main(int mode) {
    if (ushot_open_on(&g_shot, uapp_wmchan()) < 0) {
        ulog("screenshot: no compositor to capture from\n");
        return 1;
    }
    unsigned how = g_conf.pointer ? WIN_SHOT_POINTER : 0;
    if (mode == MODE_WINDOW && g_conf.shadow) how |= WIN_SHOT_SHADOW;
    int rc = ushot_take(&g_shot, mode == MODE_WINDOW ? WIN_SHOT_WINDOW : WIN_SHOT_SCREEN, how, 0, 0, 0, 0);
    char path[SHOT_PATH_MAX];
    if (rc == 0) rc = save_held(0, mode, g_shot.app, path, sizeof path);
    if (rc < 0) {
        ulogf("screenshot: could not take %s: %s\n", MODE_NAME[mode], ushot_strerror(rc));
        ushot_close(&g_shot);
        return 1;
    }
    after_save(path, g_shot.w, g_shot.h);
    ushot_close(&g_shot);
    return 0;
}

// --- Save as (the card's fourth button) --------------------------------
//
// `--save-as PATH`: a small window showing the picture, the shared
// chooser over it, and a COPY written where it says -- the original
// stays, because it is the folder's record of the capture. The format is
// the new name's extension (uimg_save()).
//
// `--save-as --move PATH` is "Ask where to save it": the capture was
// written to the folder first, so nothing is lost if this window is
// never answered; a save MOVES it, and the card follows the new name.
// Cancel leaves it where it was.

static char g_save_src[256];
static struct uimg g_src, g_preview;
static struct uui_filedialog g_chooser;
static int g_chooser_asked;
static char g_save_note[128];
static int g_save_move;

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
    if (!k_path_dirname(g_save_src, dir, sizeof dir)) snprintf(dir, sizeof dir, "%s", SHOT_DEFAULT_DIR);
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
        if (g_save_move) {
            if (sys_unlink(g_save_src) < 0) ulogf("screenshot: could not remove %s\n", g_save_src);
            g_conf.ask = 0;
            g_conf.flash = 0;   // the flash was the shutter's, already seen
            after_save(path, g_src.w, g_src.h);
        }
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
    shot_conf_load(&g_conf);
    // The card's Save as: the rest of the line is the path.
    if (argc > 2 && !strcmp(argv[1], "--save-as")) {
        int first = 2;
        if (!strcmp(argv[2], "--move")) { g_save_move = 1; first = 3; }
        size_t n = 0;
        for (int i = first; i < argc; i++)
            n += (size_t)snprintf(g_save_src + n, n < sizeof g_save_src ? sizeof g_save_src - n : 0,
                                  "%s%s", i > first ? " " : "", argv[i]);
        if (n == 0 || n >= sizeof g_save_src) return 1;
        return save_as_main();
    }
    int now = -1;
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--delay")) {
            g_launch_delay = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--mode")) {
            for (int m = 0, j = ++i; m < MODES; m++) if (!strcmp(argv[j], MODE_NAME[m])) g_launch_mode = m;
        } else if (!strcmp(argv[i], "--now")) {
            for (int m = 0, j = ++i; m < MODES; m++) if (!strcmp(argv[j], MODE_NAME[m])) now = m;
        }
    }
    if (now == MODE_SCREEN || now == MODE_WINDOW) return now_main(now);
    // The mode it opens in: what the key asked, else what the options say.
    g_mode = g_launch_mode >= 0 ? g_launch_mode : g_conf.start == SHOT_START_LAST ? g_conf.mode : g_conf.start;
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
