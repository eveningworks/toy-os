// Taskbar peek -- see wm_peek.h. One file because it is one concern:
// when the card opens and closes, what it holds, and drawing it. The
// taskbar only reports what the pointer is on (wm_peek_hover()); the
// compositor only asks which window to lift (wm_peek_highlight_index()).
#include "wm_peek.h"
#include "wm_internal.h"
#include "wm_overlay.h"
#include "wm_shadow.h"
#include "wm_taskbar.h"
#include "lib/icon_cache.h"
#include "lib/uimg.h"
#include "ui/uui.h"
#include "ui/uui_toolbar.h"   // UUI_TOOLTIP_DELAY_TICKS -- one delay for the desktop
#include "kapi.h"
#include "rt/sys.h"

#define PEEK_MAX    6     // entries in one card: a group wider than this shows its first six
#define PEEK_PAD    12
#define PEEK_GAP    12    // between entries
#define PEEK_LIFT   8     // between the card and the strip
// How long the card survives the pointer leaving both the button and
// the card -- the time it takes to cross PEEK_LIFT on the way up.
#define PEEK_GRACE_TICKS   30
// A client presenting at 60 Hz must not cost a full box-filtered
// rescale per frame; the card refreshes at most this often.
#define PEEK_RESCALE_TICKS 20

struct peek_entry {
    uint32_t seq;          // struct window.open_seq -- never an index
    struct uimg thumb;     // px NULL until first scaled
    int dirty;
    uint64_t scaled_at;
};

static struct peek_entry g_e[PEEK_MAX];
static int g_n;
static int g_btn_x, g_btn_w;
static uint64_t g_hot_since;
static uint64_t g_grace_since;
// AFTER A CLICK the card stays shut until the pointer leaves the button
// it was on -- a click is an answer, and the card reopening under a
// still pointer would ask the question again.
static int g_suppress;
static int g_hot_entry = -1, g_hot_close;
static uint32_t g_highlight_seq;
static enum wm_peek_mode g_mode = WM_PEEK_PREVIEW;

int wm_peek_open;

void wm_peek_set_mode(enum wm_peek_mode m) {
    if (m == g_mode) return;
    g_mode = m;
    wm_peek_close();
}
enum wm_peek_mode wm_peek_mode(void) { return g_mode; }

static int find_seq(uint32_t seq) {
    if (!seq) return -1;
    for (int i = 0; i < window_count; i++)
        if (windows[i].open_seq == seq) return i;
    return -1;
}

int wm_peek_can_show(int idx) {
    if (idx < 0 || idx >= window_count) return 0;
    const struct window *w = &windows[idx];
    return wm_client_is_client_window(w) && w->client_buf &&
           w->client_w > 0 && w->client_h > 0 &&
           w->client_gen[w->client_front] != 0;
}

// --- geometry -----------------------------------------------------------

// The box a thumbnail fits in: about Windows 11's size on a 1280 screen,
// capped so a 4K one does not get a poster.
static int box_w(void) {
    int w = screen_w * 18 / 100;
    return w < 160 ? 160 : w > 320 ? 320 : w;
}
static int box_h(void) { return box_w() * 5 / 8; }
static int header_h(void) { return ugfx_char_h() + 8; }

static void card_rect(int *x, int *y, int *w, int *h) {
    int n = g_n ? g_n : 1;
    *w = 2 * PEEK_PAD + n * box_w() + (n - 1) * PEEK_GAP;
    *h = 8 + header_h() + 8 + box_h() + PEEK_PAD;
    wm_popup_place(g_btn_x + g_btn_w / 2 - *w / 2,
                   screen_h - taskbar_h - PEEK_LIFT - *h, *w, *h, x, y);
}

static void entry_rects(int k, int *ex, int *hy, int *ty, int *close_x) {
    int cx, cy, cw, ch;
    card_rect(&cx, &cy, &cw, &ch);
    *ex = cx + PEEK_PAD + k * (box_w() + PEEK_GAP);
    *hy = cy + 8;
    *ty = *hy + header_h() + 8;
    *close_x = *ex + box_w() - header_h();
}

// --- thumbnails ---------------------------------------------------------

static void drop_thumbs(void) {
    for (int k = 0; k < PEEK_MAX; k++) {
        if (g_e[k].thumb.px) uimg_free(&g_e[k].thumb);
        g_e[k].thumb.px = 0;
        g_e[k].thumb.w = g_e[k].thumb.h = 0;
    }
}

// The CONTENT, not the chrome: the card has its own title row, and a
// scaled title bar is a stripe of illegible text. Box-averaged by
// uimg_scale(), which is what keeps a scaled-down window readable as a
// window rather than as nearest-neighbour noise.
static int rescale(int k, uint64_t now) {
    struct peek_entry *e = &g_e[k];
    int idx = find_seq(e->seq);
    if (!wm_peek_can_show(idx)) return 0;
    const struct window *w = &windows[idx];
    int tw, th;
    uimg_fit_size(w->client_w, w->client_h, box_w(), box_h(), UIMG_FIT_CONTAIN, &tw, &th);
    int resized = !e->thumb.px || e->thumb.w != tw || e->thumb.h != th;
    if (!resized && !e->dirty) return 0;
    if (!resized && now - e->scaled_at < PEEK_RESCALE_TICKS) return 0;   // stays dirty
    struct uimg src = { w->client_w, w->client_h, w->client_buf, 0 };
    struct uimg out = { 0, 0, 0, 0 };
    if (uimg_scale(&src, tw, th, &out) != 0) return 0;
    if (e->thumb.px) uimg_free(&e->thumb);
    e->thumb = out;
    e->dirty = 0;
    e->scaled_at = now;
    return 1;
}

void wm_peek_presented(uint32_t open_seq) {
    for (int k = 0; k < g_n; k++)
        if (g_e[k].seq == open_seq) g_e[k].dirty = 1;
}

// --- open, switch, close ------------------------------------------------

static void set_highlight(uint32_t seq) {
    if (seq == g_highlight_seq) return;
    g_highlight_seq = seq;
    // The whole desktop above the strip dims or undims.
    wm_damage_rect(0, 0, screen_w, screen_h - taskbar_h);
    redraw_pending = 1;
}

void wm_peek_close(void) {
    if (wm_peek_open) wm_peek_damage();
    wm_peek_open = 0;
    drop_thumbs();
    g_hot_entry = -1;
    g_hot_close = 0;
    g_grace_since = 0;
    set_highlight(0);
    redraw_pending = 1;
}

static int same_button(const int *wins, int n, int bx) {
    if (n != g_n || bx != g_btn_x) return 0;
    for (int k = 0; k < n; k++)
        if (windows[wins[k]].open_seq != g_e[k].seq) return 0;
    return 1;
}

static int on_card(int mx, int my) {
    if (!wm_peek_open) return 0;
    int x, y, w, h;
    card_rect(&x, &y, &w, &h);
    return uui_hit(x, y, w, h, mx, my);
}

void wm_peek_hover(const int *wins, int n, int bx, int bw, int mx, int my) {
    if (g_mode == WM_PEEK_OFF) { if (wm_peek_open) wm_peek_close(); return; }
    uint64_t now = sys_ticks();
    if (n > PEEK_MAX) n = PEEK_MAX;

    if (n > 0) {
        int same = same_button(wins, n, bx);
        if (g_suppress && same) return;
        g_suppress = 0;
        g_grace_since = 0;
        if (!same) {
            // A NEW BUTTON. Already open: switch at once, as Windows does
            // along the strip -- the delay is for the first card, not for
            // every button under a moving pointer.
            if (wm_peek_open) wm_peek_damage();
            drop_thumbs();
            g_n = n;
            for (int k = 0; k < n; k++) {
                g_e[k].seq = windows[wins[k]].open_seq;
                g_e[k].dirty = 1;
            }
            g_btn_x = bx;
            g_btn_w = bw;
            g_hot_entry = -1;
            set_highlight(0);
            g_hot_since = now;
        }
        // Not over another popup: a card must not open on top of the
        // Start menu the pointer is passing under.
        if (!wm_peek_open && now - g_hot_since >= UUI_TOOLTIP_DELAY_TICKS &&
            !wm_overlay_any_open()) {
            wm_peek_open = 1;
            redraw_pending = 1;
        }
    } else if (on_card(mx, my)) {
        g_grace_since = 0;
    } else if (wm_peek_open) {
        if (!g_grace_since) g_grace_since = now;
        else if (now - g_grace_since > PEEK_GRACE_TICKS) wm_peek_close();
    } else {
        g_n = 0;          // disarmed; also ends a click's suppression
        g_suppress = 0;
    }

    if (!wm_peek_open) return;
    // Every entry's window must still exist; one closing under the card
    // takes the card down rather than leaving a hole in it.
    int changed = 0;
    for (int k = 0; k < g_n; k++) {
        if (find_seq(g_e[k].seq) < 0) { wm_peek_close(); return; }
        changed |= rescale(k, now);
    }
    if (changed) wm_peek_damage();
}

int wm_peek_highlight_index(void) {
    return wm_peek_open ? find_seq(g_highlight_seq) : -1;
}

// --- the overlay's ops --------------------------------------------------

int wm_peek_rect(int *x, int *y, int *w, int *h) {
    if (!wm_peek_open) return 0;
    card_rect(x, y, w, h);
    return 1;
}

void wm_peek_damage(void) { wm_overlay_damage("peek"); }

// Which entry, and whether its close button: the hovered one lights and
// shows its x, as Windows 11's does.
static int entry_at(int mx, int my, int *on_close) {
    int hh = header_h();
    for (int k = 0; k < g_n; k++) {
        int ex, hy, ty, clx;
        entry_rects(k, &ex, &hy, &ty, &clx);
        if (!uui_hit(ex - 6, hy - 4, box_w() + 12, ty + box_h() - hy + 8, mx, my)) continue;
        *on_close = uui_hit(clx, hy, hh, hh, mx, my);
        return k;
    }
    *on_close = 0;
    return -1;
}

int wm_peek_hover_at(int mx, int my) {
    if (!wm_peek_open) return 0;
    int cl;
    int k = entry_at(mx, my, &cl);
    g_hot_entry = k;
    g_hot_close = cl;
    set_highlight(g_mode == WM_PEEK_HIGHLIGHT && k >= 0 ? g_e[k].seq : 0);
    return k < 0 ? 0 : k * 2 + cl + 1;
}

int wm_peek_click(int mx, int my) {
    if (!wm_peek_open) return 0;
    g_suppress = 1;
    int cl;
    int k = entry_at(mx, my, &cl);
    int inside = on_card(mx, my);
    int idx = k >= 0 ? find_seq(g_e[k].seq) : -1;
    wm_peek_close();
    // Outside the card the click FALLS THROUGH -- to the taskbar button
    // under it, most often, which then acts on the same click.
    if (!inside) return 0;
    if (idx >= 0) {
        if (cl) wm_request_close(idx);   // ASKS the client, as every close here does
        else taskbar_activate(idx);
    }
    return 1;
}

void wm_peek_draw(int mx, int my) {
    (void)mx; (void)my;
    if (!wm_peek_open) return;
    struct ugfx_surface *s = wm_surface();
    const struct taskbar_palette *p = taskbar_palette();
    // The card is a step off the strip's own colour: lighter on the dark
    // strip, white on the light one -- a popup of the panel, not a window.
    uint32_t card = taskbar_dark() ? p->hover : p->focus;
    int x, y, w, h;
    card_rect(&x, &y, &w, &h);
    int r = ugfx_char_h() / 2;
    if (wm_shadow_enabled()) wm_shadow_draw(x, y, w, h, r, WM_SHADOW_POPUP);
    uui_fill_round_rect(s, x, y, w, h, r, p->edge);
    uui_fill_round_rect(s, x + 1, y + 1, w - 2, h - 2, r - 1, card);

    int hh = header_h();
    int isz = hh - 8;
    for (int k = 0; k < g_n; k++) {
        int idx = find_seq(g_e[k].seq);
        if (idx < 0) continue;
        int ex, hy, ty, clx;
        entry_rects(k, &ex, &hy, &ty, &clx);
        uint32_t ground = card;
        if (k == g_hot_entry) {
            ground = uui_state_bg(card, UUI_STATE_HOVER);
            uui_fill_round_rect(s, ex - 6, hy - 4, box_w() + 12, ty + box_h() - hy + 8,
                                r > 6 ? r - 4 : 2, ground);
        }
        // THE TITLE ROW: icon, title, and -- on the hovered entry -- x.
        int tx = ex;
        const char *iname = wm_window_icon_name(idx);
        const struct uimg *ico = iname ? icon_get(iname, isz) : 0;
        if (ico) {
            ugfx_blit_alpha(s, ex, hy + (hh - ico->h) / 2, ico->w, ico->h, ico->px, ico->w);
            tx += ico->w + 8;
        }
        int title_w = clx - 4 - tx;
        ugfx_draw_string_elided(s, tx, hy + (hh - ugfx_char_h()) / 2, title_w,
                                windows[idx].title, p->text, ground);
        if (k == g_hot_entry) {
            uint32_t cbg = g_hot_close ? ugfx_rgb(190, 60, 60)
                                       : uui_state_bg(ground, UUI_STATE_HOVER);
            uui_fill_round_rect(s, clx, hy, hh, hh, 4, cbg);
            uint32_t ink = g_hot_close ? ugfx_rgb(255, 255, 255) : p->text;
            int c = hh / 2, a = hh / 5;
            ugfx_draw_line(s, clx + c - a, hy + c - a, clx + c + a, hy + c + a, ink, GEOM_AA);
            ugfx_draw_line(s, clx + c + a, hy + c - a, clx + c - a, hy + c + a, ink, GEOM_AA);
        }
        // THE THUMBNAIL, centred in its box; the strip's colour until the
        // first scale lands.
        const struct uimg *t = &g_e[k].thumb;
        if (t->px) {
            int px = ex + (box_w() - t->w) / 2, py = ty + (box_h() - t->h) / 2;
            ugfx_blit(s, px, py, t->w, t->h, t->px, t->w);
            ugfx_draw_rect(s, px, py, t->w, t->h, p->edge);
        } else {
            ugfx_fill_rect(s, ex, ty, box_w(), box_h(), p->bar);
        }
    }
}

int wm_peek_state(int *x, int *y, int *w, int *h,
                  struct wm_peek_entry_info *out, int max) {
    if (!wm_peek_open) return 0;
    card_rect(x, y, w, h);
    int n = 0;
    for (int k = 0; k < g_n && n < max; k++) {
        int idx = find_seq(g_e[k].seq);
        if (idx < 0) continue;
        int ex, hy, ty, clx;
        entry_rects(k, &ex, &hy, &ty, &clx);
        const struct uimg *t = &g_e[k].thumb;
        out[n].title = windows[idx].title;
        out[n].thumb_x = ex + (box_w() - t->w) / 2;
        out[n].thumb_y = ty + (box_h() - t->h) / 2;
        out[n].thumb_w = t->w;
        out[n].thumb_h = t->h;
        out[n].close_x = clx;
        out[n].close_y = hy;
        out[n].close_s = header_h();
        n++;
    }
    return n;
}
