// DOOM's front end: everything in its window that is not the game.
//
//   the launcher page   before the game -- the data being played, its
//                       title picture, Start, and the keys (mockup H3)
//   the game-data card  no WAD, or "Change game data...": pick one and
//                       download it, or use a file (mockup W1)
//   the key sheet       F1 over a paused game; F1 again is DOOM's own
//                       help, which the sheet would otherwise hide (H1)
//
// **A DOWNLOAD IS A WORKER THREAD** (System Update's shape): the fetch
// and the unzip block, and the window must keep answering -- a client
// that stops reading its events is drawn Not Responding. The worker
// writes counters and posts; only the main thread draws.
#include "doom_internal.h"
#include "backends/doom/dg_toyos.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "keyboard.h"
#include "kpath.h"
#include "rt/sys.h"
#include "ui/ulog.h"
#include "ui/utheme.h"
#include "ui/uui_checkbox.h"
#include "ui/uui_filedialog.h"
#include "ui/uui_keysheet.h"
#include "ui/uui_primitives.h"
#include "lib/uconf.h"
#include "lib/uzip.h"
#include "uhttp.h"

enum view { V_GAME, V_PAGE, V_DATA, V_FETCH };
static enum view g_view = V_GAME;
static int g_help;
static const struct doom_iwad *g_cur;     // the page's, then the game's

// --- the keys ----------------------------------------------------------

static const struct uui_keysheet_row MOVE[] = {
    { "Walk", "[Up] [Down]" },
    { "Turn", "[Left] [Right]" },
    { "Strafe", "[,] [.]" },
    { "Strafe, held", "[Alt]+[Left] [Right]" },
    { "Run", "hold [Shift]" },
};
static const struct uui_keysheet_row FIGHT[] = {
    { "Fire", "[Ctrl]" },
    { "Use, open doors", "[Space]" },
    { "Weapon", "[1]-[7]" },
    { "Map", "[Tab]" },
};
static const struct uui_keysheet_row GAME[] = {
    { "Menu", "[Esc]" },
    { "Save, load", "[F2] [F3]" },
    { "Quick save, load", "[F6] [F9]" },
    { "Pause", "[Pause]" },
};
static const struct uui_keysheet_row WINDOW[] = {
    { "Full screen", "[Alt]+[Enter]" },
    { "Screen effect", "[Alt]+[C]" },
    { "Keys, in game", "[F1]" },
    { "View size", "[=] [-]" },
};
#define N(a) ((int)(sizeof a / sizeof a[0]))
static const struct uui_keysheet_group KEYS[] = {
    { "MOVE", MOVE, N(MOVE) }, { "FIGHT", FIGHT, N(FIGHT) },
    { "GAME", GAME, N(GAME) }, { "WINDOW", WINDOW, N(WINDOW) },
};
static struct uui_keysheet g_sheet;

// --- shared state with the download worker ----------------------------

#define EV_PROGRESS 1
#define EV_DONE     2
static volatile unsigned long g_got, g_total;
static volatile int g_phase;               // 0 fetching, 1 unpacking
static volatile int g_cancel;
static int g_fetch_i = -1;                 // DOOM_IWADS index being fetched
static int g_fetch_rc;
static char g_fetch_err[192];
static char g_fetch_host[64];              // the host the URL really names -- a mirror's
static unsigned long long g_last_post;
static struct uapp *g_app;

// --- the card's and the page's state ---------------------------------

static int g_rows[8], g_row_n, g_sel;
static int g_back;                         // the card came from the page
static char g_note[160];                   // the card's last error
static struct uui_checkbox g_show;
static struct uui_filedialog g_fd;
static int g_hot = -1, g_pressed = -1;

static uint32_t g_title[DOOM_TITLE_W * DOOM_TITLE_H];
static const struct doom_iwad *g_title_of;
static int g_title_ok;

// --- measures -----------------------------------------------------------

static int u(void)     { return ugfx_char_h(); }
static int btn_h(void) { return utheme_control_h(); }

enum {
    C_START, C_CHANGE, C_SHOW,                 // the page
    C_ROW0, C_ROW_LAST = C_ROW0 + 7,           // the card's options
    C_FILE, C_CANCEL, C_PRIMARY,               // the card's buttons
    C_STOP,                                    // the download's Cancel
    C_RESUME,                                  // the sheet's
    CTLS
};
struct rect { int x, y, w, h; };
static struct rect g_r[CTLS];
static struct rect g_card;                     // the card or sheet frame
static int g_side_w;                           // the page's side panel
// THE SIZE OF THE SURFACE LAST DRAWN, not uapp_width(): after a resize
// request the two disagree until the new buffer arrives, and a layout
// for one drawn into the other is what the pointer would then miss.
static int g_W = 640, g_H = 480;

static int in(const struct rect *r, int x, int y) {
    return r->w > 0 && x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

static int btn_w(const char *label) { return ugfx_text_width(label) + 2 * ugfx_char_w() + u(); }

static const char *primary_label(void) {
    if (g_sel < 0 || g_sel >= g_row_n) return "Download";
    return doom_iwad_present(&DOOM_IWADS[g_rows[g_sel]]) ? "Use" : "Download";
}

// One place that says where every control is, for drawing and for the
// pointer alike.
static void layout(struct uapp *a) {
    (void)a;
    int W = g_W, H = g_H, p = u();
    memset(g_r, 0, sizeof g_r);
    memset(&g_card, 0, sizeof g_card);
    if (g_view == V_PAGE) {
        int cw, chh;
        uui_checkbox_natural_size(&g_show, &cw, &chh);
        int sw = p * 13 > cw + 2 * p ? p * 13 : cw + 2 * p;   // the checkbox's label fits
        if (sw > W * 2 / 5) sw = W * 2 / 5;
        g_side_w = sw;
        g_r[C_SHOW] = (struct rect){ p, H - p - chh, cw, chh };
        g_r[C_START] = (struct rect){ p, g_r[C_SHOW].y - p / 2 - btn_h(), sw - 2 * p, btn_h() };
        int tw = sw - 2 * p, th = tw * 3 / 4;
        int ly = p + th + p + u() + u() / 2 + 2 * u() + u() / 2;
        g_r[C_CHANGE] = (struct rect){ p, ly, ugfx_text_width("Change game data..."), u() };
        uui_checkbox_set_geometry(&g_show, g_r[C_SHOW].x, g_r[C_SHOW].y);
    } else if (g_view == V_DATA) {
        int cw = p * 36 < W - 2 * p ? p * 36 : W - 2 * p;
        int rh = 3 * u() + u() / 2 + 4;
        int ch = p + u() * 2 + u() + p + g_row_n * (rh + 6) + (g_note[0] ? u() + 4 : 0) + p + btn_h() + p;
        g_card = (struct rect){ (W - cw) / 2, (H - ch) / 2, cw, ch };
        int y = g_card.y + p + u() * 2 + u() + p / 2;
        for (int i = 0; i < g_row_n; i++, y += rh + 6)
            g_r[C_ROW0 + i] = (struct rect){ g_card.x + p, y, cw - 2 * p, rh };
        int by = g_card.y + ch - p - btn_h();
        g_r[C_FILE] = (struct rect){ g_card.x + p, by, btn_w("Use a file..."), btn_h() };
        int pw = btn_w("Download");
        g_r[C_PRIMARY] = (struct rect){ g_card.x + cw - p - pw, by, pw, btn_h() };
        if (g_back) {
            int kw = btn_w("Cancel");
            g_r[C_CANCEL] = (struct rect){ g_r[C_PRIMARY].x - p / 2 - kw, by, kw, btn_h() };
        }
    } else if (g_view == V_FETCH) {
        int cw = p * 36 < W - 2 * p ? p * 36 : W - 2 * p;
        int ch = p + u() * 2 + u() + p + u() / 2 + p + u() + p + u() + p + btn_h() + p;
        g_card = (struct rect){ (W - cw) / 2, (H - ch) / 2, cw, ch };
        int kw = btn_w("Cancel");
        g_r[C_STOP] = (struct rect){ g_card.x + cw - p - kw, g_card.y + ch - p - btn_h(), kw, btn_h() };
    } else if (g_help) {
        int sw, sh;
        uui_keysheet_natural_size(&g_sheet, &sw, &sh);
        int cw = sw + 2 * p, ch = p + u() * 2 + sh + p + btn_h() + p;
        if (cw > W - 2 * p) cw = W - 2 * p;
        g_card = (struct rect){ (W - cw) / 2, (H - ch) / 2, cw, ch };
        int rw = btn_w("Resume");
        g_r[C_RESUME] = (struct rect){ g_card.x + cw - p - rw, g_card.y + ch - p - btn_h(), rw, btn_h() };
        uui_keysheet_set_geometry(&g_sheet, g_card.x + p, g_card.y + p + u() * 2, cw - 2 * p, sh);
    }
}

// --- drawing helpers ----------------------------------------------------

static void bold(const char *t, struct ugfx_surface *s, int x, int y, int w, uint32_t bg) {
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    ugfx_draw_string_elided(s, x, y, w, t, UTHEME_TEXT, bg);
    ugfx_set_font(was);
}

static void dim(const char *t, struct ugfx_surface *s, int x, int y, int w, uint32_t bg) {
    ugfx_draw_string_elided(s, x, y, w, t, ugfx_blend(UTHEME_TEXT, bg, 150), bg);
}

static void card(struct ugfx_surface *s, const struct rect *r) {
    int rad = u() / 2;
    uui_fill_round_rect(s, r->x, r->y, r->w, r->h, rad, UTHEME_OUTLINE);
    uui_fill_round_rect(s, r->x + 1, r->y + 1, r->w - 2, r->h - 2, rad - 1, UTHEME_WINDOW_BG);
}

static void button(struct ugfx_surface *s, int id, const char *label, int primary) {
    const struct rect *r = &g_r[id];
    enum uui_state st = g_pressed == id && g_hot == id ? UUI_STATE_PRESSED
                      : g_hot == id ? UUI_STATE_HOVER : UUI_STATE_REST;
    uui_button_draw(s, r->x, r->y, r->w, r->h, label, primary ? UTHEME_ACCENT : UTHEME_BUTTON_BG,
                    primary ? UTHEME_ACCENT_TEXT : UTHEME_TEXT, st);
}

static void radio(struct ugfx_surface *s, int x, int y, int on, uint32_t bg) {
    int d = u() * 3 / 4 + 2;
    uui_fill_round_rect(s, x, y, d, d, UUI_CAPSULE, on ? UTHEME_ACCENT : UTHEME_OUTLINE);
    uui_fill_round_rect(s, x + 2, y + 2, d - 4, d - 4, UUI_CAPSULE, on ? bg : UTHEME_WHITE);
    if (on) uui_fill_round_rect(s, x + d / 4 + 1, y + d / 4 + 1, d - 2 * (d / 4) - 2,
                                d - 2 * (d / 4) - 2, UUI_CAPSULE, UTHEME_ACCENT);
}

static void mib(char *out, int cap, unsigned long bytes) {
    unsigned long t = bytes * 10 / (1024 * 1024);
    snprintf(out, (size_t)cap, "%lu.%lu MiB", t / 10, t % 10);
}

// The title picture, aspect-corrected to 4:3 into `w` x `h` (nearest).
static void draw_title(struct ugfx_surface *s, int x, int y, int w, int h) {
    if (g_title_of != g_cur) {
        char p[160];
        g_title_ok = g_cur && doom_iwad_path(g_cur, p, sizeof p) && doom_wad_titlepic(p, g_title) == 0;
        g_title_of = g_cur;
    }
    ugfx_fill_rect(s, x - 1, y - 1, w + 2, h + 2, UTHEME_OUTLINE);
    if (!g_title_ok) { ugfx_fill_rect(s, x, y, w, h, ugfx_rgb(0, 0, 0)); return; }
    for (int dy = 0; dy < h; dy++) {
        const uint32_t *row = g_title + (size_t)(dy * DOOM_TITLE_H / h) * DOOM_TITLE_W;
        for (int dx = 0; dx < w; dx++) ugfx_put_pixel(s, x + dx, y + dy, row[dx * DOOM_TITLE_W / w]);
    }
}

// --- the views ----------------------------------------------------------

// THE PAGE ASKS FOR ROOM ONCE: at a big font the keys do not fit 640
// wide. 4:3, so the game still fills the window it then gets.
static void fit_page(struct uapp *a) {
    static int done, tries;
    if (done) return;
    int kw, kh, p = u();
    uui_keysheet_natural_size(&g_sheet, &kw, &kh);
    int need_w = g_side_w + p + p / 2 + kw + p;
    int need_h = p + 2 * u() + kh + 3 * u();
    if (need_w <= g_W && need_h <= g_H) { done = 1; return; }
    int w = need_w > need_h * 4 / 3 ? need_w : need_h * 4 / 3;
    // Refused before the loop runs (as fullscreen is): asked again on
    // the next frame until it takes.
    if (uapp_resize(a, w, w * 3 / 4) || ++tries >= 10) done = 1;
    uapp_redraw(a);
}

static void draw_page(struct ugfx_surface *s) {
    int W = g_W, H = g_H, p = u(), sw = g_side_w;
    uint32_t side = UTHEME_PANEL_BG, main_bg = UTHEME_WINDOW_BG;
    ugfx_fill_rect(s, 0, 0, sw, H, side);
    ugfx_fill_rect(s, sw, 0, 1, H, UTHEME_SEPARATOR);
    ugfx_fill_rect(s, sw + 1, 0, W - sw - 1, H, main_bg);

    int tw = sw - 2 * p, th = tw * 3 / 4, y = p;
    draw_title(s, p, y, tw, th);
    y += th + p;
    bold(g_cur ? g_cur->name : "No game data", s, p, y, tw, side);
    y += u() + u() / 2;
    if (g_cur) {
        dim(g_cur->file, s, p, y, tw, side);
        dim(DOOM_WAD_DIR, s, p, y + u(), tw, side);
    }
    const struct rect *c = &g_r[C_CHANGE];
    uint32_t link = utheme_action(UTHEME_ACT_NAV);
    ugfx_draw_string_clipped(s, c->x, c->y, tw, "Change game data...", link, side);
    if (g_hot == C_CHANGE) ugfx_fill_rect(s, c->x, c->y + u() - 1, c->w, 1, link);
    button(s, C_START, "Start", 1);
    g_show.bg = side;
    uui_checkbox_draw(s, &g_show);

    int mx = sw + p + p / 2, mw = W - mx - p;
    bold("Keys", s, mx, p, mw, main_bg);
    int kw, kh;
    uui_keysheet_natural_size(&g_sheet, &kw, &kh);
    g_sheet.bg = main_bg;
    uui_keysheet_set_geometry(&g_sheet, mx, p + u() * 2, mw, kh);
    uui_keysheet_draw(s, &g_sheet);
    dim("Keyboard only; the mouse does not aim.", s, mx, H - p - u(), mw, main_bg);
}

static void draw_data(struct uapp *a, struct ugfx_surface *s) {
    ugfx_fill(s, ugfx_rgb(0, 0, 0));
    (void)a;
    const struct rect *k = &g_card;
    int p = u(), x = k->x + p, w = k->w - 2 * p;
    uint32_t bg = UTHEME_WINDOW_BG;
    card(s, k);
    int any = 0;
    for (int i = 0; i < DOOM_IWAD_COUNT; i++) any |= doom_iwad_present(&DOOM_IWADS[i]);
    bold(any ? "Game data" : "DOOM needs game data", s, x, k->y + p, w, bg);
    dim("Choose one. Downloads go to " DOOM_WAD_DIR ".", s, x, k->y + p + u() + u() / 2, w, bg);
    for (int i = 0; i < g_row_n; i++) {
        const struct rect *r = &g_r[C_ROW0 + i];
        const struct doom_iwad *wd = &DOOM_IWADS[g_rows[i]];
        int sel = i == g_sel;
        uint32_t rb = sel ? UTHEME_SELECTION : g_hot == C_ROW0 + i ? uui_state_bg(UTHEME_WHITE, UUI_STATE_HOVER)
                                                                   : UTHEME_WHITE;
        uui_fill_round_rect(s, r->x, r->y, r->w, r->h, p / 3, sel ? UTHEME_ACCENT : UTHEME_SEPARATOR);
        uui_fill_round_rect(s, r->x + 1, r->y + 1, r->w - 2, r->h - 2, p / 3 - 1, rb);
        radio(s, r->x + p / 2, r->y + p / 2, sel, rb);
        int tx = r->x + p / 2 + u() + p / 2, tw = r->x + r->w - tx - p / 2;
        bold(wd->name, s, tx, r->y + p / 3, tw, rb);
        char line[96], sz[24];
        dim(wd->detail, s, tx, r->y + p / 3 + u() + 2, tw, rb);
        mib(sz, sizeof sz, wd->size);
        if (doom_iwad_present(wd)) snprintf(line, sizeof line, "Installed");
        else snprintf(line, sizeof line, "From %s, %s", wd->source, sz);
        dim(line, s, tx, r->y + p / 3 + 2 * u() + 4, tw, rb);
    }
    if (g_note[0]) {
        int ny = g_r[C_ROW0 + g_row_n - 1].y + g_r[C_ROW0 + g_row_n - 1].h + 6;
        ugfx_draw_string_elided(s, x, ny, w, g_note, utheme_action(UTHEME_ACT_DANGER), bg);
    }
    ugfx_fill_rect(s, k->x + 1, g_r[C_FILE].y - p / 2, k->w - 2, 1, UTHEME_SEPARATOR);
    button(s, C_FILE, "Use a file...", 0);
    if (g_back) button(s, C_CANCEL, "Cancel", 0);
    button(s, C_PRIMARY, primary_label(), 1);
}

static void draw_fetch(struct uapp *a, struct ugfx_surface *s) {
    (void)a;
    ugfx_fill(s, ugfx_rgb(0, 0, 0));
    const struct rect *k = &g_card;
    const struct doom_iwad *wd = &DOOM_IWADS[g_fetch_i];
    int p = u(), x = k->x + p, w = k->w - 2 * p, y = k->y + p;
    uint32_t bg = UTHEME_WINDOW_BG;
    card(s, k);
    char t[128];
    snprintf(t, sizeof t, g_phase ? "Unpacking %s" : "Downloading %s", wd->name);
    bold(t, s, x, y, w, bg);
    y += u() + u() / 2;
    snprintf(t, sizeof t, "From %s to %s/%s", g_fetch_host, DOOM_WAD_DIR, wd->file);
    dim(t, s, x, y, w, bg);
    y += u() + p;
    unsigned long got = g_got, total = g_total ? g_total : (g_phase ? 0 : wd->size);
    int fill = total ? (int)((unsigned long long)w * (got > total ? total : got) / total) : 0;
    uui_fill_round_rect(s, x, y, w, u() / 2, UUI_CAPSULE, UTHEME_SEPARATOR);
    if (fill > 0) uui_fill_round_rect(s, x, y, fill < u() / 2 ? u() / 2 : fill, u() / 2, UUI_CAPSULE, UTHEME_ACCENT);
    y += u() / 2 + p / 2;
    char a1[24], a2[24];
    mib(a1, sizeof a1, got);
    mib(a2, sizeof a2, total);
    if (total) snprintf(t, sizeof t, "%s of %s", a1, a2);
    else snprintf(t, sizeof t, g_phase ? "Reading the archive..." : "Connecting...");
    dim(t, s, x, y, w, bg);
    y += u() + p;
    dim("Checked against its known SHA-256 before DOOM uses it.", s, x, y, w, bg);
    button(s, C_STOP, "Cancel", 0);
}

// --- the worker ---------------------------------------------------------

static void post_progress(void) {
    unsigned long long now = sys_monotonic_ns();
    if (now - g_last_post < 100000000ULL) return;   // ten a second is plenty
    g_last_post = now;
    uapp_post(g_app, EV_PROGRESS, 0);
}

static void dl_progress(void *ctx, unsigned long got, unsigned long total) {
    (void)ctx;
    g_got = got;
    g_total = total;
    post_progress();
}
static int dl_cancelled(void *ctx) { (void)ctx; return g_cancel; }
static int zip_progress(void *ctx, unsigned long done, unsigned long total) {
    (void)ctx;
    g_got = done;
    g_total = total;
    post_progress();
    return g_cancel;
}

static void *fetch_worker(void *arg) {
    (void)arg;
    const struct doom_iwad *w = &DOOM_IWADS[g_fetch_i];
    char url[600], dest[160], tmp[160];
    g_fetch_rc = -1;
    if (!doom_iwad_url(w, url, sizeof url) || !doom_iwad_path(w, dest, sizeof dest)) {
        snprintf(g_fetch_err, sizeof g_fetch_err, "no address to download %s from", w->file);
        uapp_post(g_app, EV_DONE, 0);
        return 0;
    }
    mkdir("/usr", 0755);
    mkdir("/usr/share", 0755);
    mkdir(DOOM_WAD_DIR, 0755);
    mkdir("/var", 0755);
    mkdir("/var/tmp", 0755);
    // A zip is fetched beside the game data's scratch, then unpacked.
    const char *base = strrchr(w->url, '/');
    snprintf(tmp, sizeof tmp, "/var/tmp%s", base ? base : "/doom-download");
    struct uhttp_download d = {
        .url = url,
        .path = w->zip_member ? tmp : dest,
        .sha256 = w->sha256,
        // THE CHECKSUM IS THE AUTHORITY, not the certificate: a machine
        // with no CA bundle (the default image) can still fetch, and a
        // substituted file fails the hash. Said on the card.
        .insecure = 1,
        .allow_weak_entropy = 1,
        .max_redirects = 5,
        .progress = dl_progress,
        .cancelled = dl_cancelled,
    };
    ulogf("doom: downloading %s\n", url);
    if (uhttp_download(&d) != 0) {
        snprintf(g_fetch_err, sizeof g_fetch_err, "%s", d.err);
    } else if (w->zip_member) {
        g_phase = 1;
        g_got = 0;
        g_total = 0;
        uapp_post(g_app, EV_PROGRESS, 0);
        if (uzip_extract(tmp, w->zip_member, dest, zip_progress, 0, g_fetch_err,
                         (int)sizeof g_fetch_err) == 0)
            g_fetch_rc = 0;
        unlink(tmp);
    } else {
        g_fetch_rc = 0;
    }
    uapp_post(g_app, EV_DONE, 0);
    return 0;
}

static void start_fetch(struct uapp *a, int iwad) {
    g_fetch_i = iwad;
    char url[600];
    g_fetch_host[0] = 0;
    if (doom_iwad_url(&DOOM_IWADS[iwad], url, sizeof url)) {
        const char *h = strstr(url, "://");
        h = h ? h + 3 : url;
        int n = (int)strcspn(h, "/");
        snprintf(g_fetch_host, sizeof g_fetch_host, "%.*s", n, h);
    }
    g_got = g_total = 0;
    g_phase = 0;
    g_cancel = 0;
    g_fetch_err[0] = 0;
    g_note[0] = 0;
    g_view = V_FETCH;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    if (pthread_create(&th, &at, fetch_worker, 0) != 0) {
        g_view = V_DATA;
        snprintf(g_note, sizeof g_note, "Could not start the download.");
    }
    uapp_redraw(a);
}

// --- views: entering ----------------------------------------------------

static void show_page(struct uapp *a) {
    g_view = V_PAGE;
    g_hot = g_pressed = -1;
    ulogf("doom: launcher %s\n", g_cur ? g_cur->file : "-");
    uapp_redraw(a);
}

static void show_data(struct uapp *a, int back) {
    g_view = V_DATA;
    g_back = back;
    g_hot = g_pressed = -1;
    g_row_n = 0;
    // Every IWAD that can be downloaded, then any other that is here.
    for (int i = 0; i < DOOM_IWAD_COUNT && g_row_n < 8; i++)
        if (DOOM_IWADS[i].url || doom_iwad_present(&DOOM_IWADS[i])) g_rows[g_row_n++] = i;
    // Freedoom first on a machine with nothing: the free one.
    g_sel = 0;
    for (int i = 0; i < g_row_n; i++) {
        const struct doom_iwad *w = &DOOM_IWADS[g_rows[i]];
        if ((g_cur && w == g_cur) || (!g_cur && w->zip_member)) g_sel = i;
    }
    ulogf("doom: game data card, %d choices\n", g_row_n);
    uapp_redraw(a);
}

static void file_chosen(void *ctx, const char *path) {
    struct uapp *a = ctx;
    if (!path) { uapp_redraw(a); return; }
    if (!doom_wad_is_iwad(path)) {
        snprintf(g_note, sizeof g_note, "%s is not an IWAD, a game's main data file.", k_path_basename(path));
        uapp_redraw(a);
        return;
    }
    const struct doom_iwad *w = doom_iwad_named(k_path_basename(path));
    if (!w) {
        snprintf(g_note, sizeof g_note, "DOOM does not know %s; it plays doom1, doom, doom2 and freedoom.",
                 k_path_basename(path));
        uapp_redraw(a);
        return;
    }
    char dest[160];
    doom_iwad_path(w, dest, sizeof dest);
    if (strcmp(dest, path)) {
        // Copied, not linked: the file may be on a stick about to leave.
        mkdir(DOOM_WAD_DIR, 0755);
        int in_fd = open(path, O_RDONLY), out = open(dest, O_WRONLY | O_CREAT | O_TRUNC);
        static char buf[16384];
        long n = 0;
        int ok = in_fd >= 0 && out >= 0;
        while (ok && (n = read(in_fd, buf, sizeof buf)) > 0) ok = write(out, buf, (size_t)n) == n;
        ok = ok && n == 0;
        if (in_fd >= 0) close(in_fd);
        if (out >= 0) close(out);
        if (!ok) {
            unlink(dest);
            snprintf(g_note, sizeof g_note, "Could not copy it to %s.", DOOM_WAD_DIR);
            uapp_redraw(a);
            return;
        }
    }
    doom_iwad_choose(w);
    g_cur = w;
    show_page(a);
}

static void activate(struct uapp *a, int id) {
    switch (id) {
    case C_START:
        if (g_cur) doom_start_game(a, g_cur);
        return;
    case C_CHANGE:
        show_data(a, 1);
        return;
    case C_SHOW:
        g_show.checked = !g_show.checked;
        uconf_set(DOOM_CONF, "show_page", g_show.checked ? "on" : "off");
        break;
    case C_FILE: {
        if (uui_filedialog_is_open(&g_fd)) return;
        struct uui_filedialog_opts o = {
            .mode = UUI_FILEDIALOG_OPEN,
            .title = "Use a WAD file",
            .start_dir = "/home",
        };
        uui_filedialog_open(a, &g_fd, &o, file_chosen, a);
        return;
    }
    case C_CANCEL:
        show_page(a);
        return;
    case C_PRIMARY: {
        if (g_sel < 0 || g_sel >= g_row_n) return;
        const struct doom_iwad *w = &DOOM_IWADS[g_rows[g_sel]];
        if (doom_iwad_present(w)) {
            doom_iwad_choose(w);
            g_cur = w;
            show_page(a);
        } else {
            start_fetch(a, g_rows[g_sel]);
        }
        return;
    }
    case C_STOP:
        g_cancel = 1;
        return;
    case C_RESUME:
        doom_help_close(a);
        return;
    default:
        if (id >= C_ROW0 && id < C_ROW0 + g_row_n) g_sel = id - C_ROW0;
        break;
    }
    uapp_redraw(a);
}

// --- the public face ------------------------------------------------------

static void log_layout(void);

int doom_front_up(void) { return g_view != V_GAME; }
int doom_help_up(void)  { return g_help; }

int doom_front_open(struct uapp *a, int has_args) {
    g_app = a;
    uui_keysheet_init(&g_sheet, KEYS, N(KEYS), 2);
    char v[8];
    int show = !(uconf_get(DOOM_CONF, "show_page", v, sizeof v) && !strcmp(v, "off"));
    uui_checkbox_init(&g_show, 0, 0, 0, "Show this page at start", UUI_COLOR_UNSET, UUI_COLOR_UNSET);
    g_show.accent = 1;
    g_show.checked = show;
    g_cur = doom_iwad_chosen();
    if (!g_cur) {
        ulogf("doom: no IWAD in %s\n", DOOM_WAD_DIR);
        show_data(a, 0);
        return 0;
    }
    if (!show || has_args) return 1;
    show_page(a);
    return 0;
}

void doom_front_draw(struct uapp *a, struct ugfx_surface *s) {
    g_W = s->w;
    g_H = s->h;
    layout(a);
    if (g_view == V_PAGE) fit_page(a);   // after layout: it reads the side panel's width
    if (g_view == V_PAGE) draw_page(s);
    else if (g_view == V_DATA) draw_data(a, s);
    else if (g_view == V_FETCH) draw_fetch(a, s);
    log_layout();
}

void doom_help_open(struct uapp *a) {
    if (g_help || g_view != V_GAME) return;
    g_help = 1;
    g_hot = g_pressed = -1;
    dg_release_all();
    dg_hold(1);
    ulogf("doom: key sheet shown\n");
    uapp_redraw(a);
}

void doom_help_close(struct uapp *a) {
    if (!g_help) return;
    g_help = 0;
    ulogf("doom: key sheet closed, the game was %s\n", dg_paused() ? "paused" : "running");
    dg_hold(0);
    uapp_redraw(a);
}

void doom_help_draw(struct uapp *a, struct ugfx_surface *s) {
    if (!g_help) return;
    g_W = s->w;
    g_H = s->h;
    layout(a);
    int W = g_W, H = g_H, p = u();
    uui_glass_round_rect(s, 0, 0, W, H, 0, ugfx_rgb(0, 0, 0), 140, 0);
    card(s, &g_card);
    uint32_t bg = UTHEME_WINDOW_BG;
    int x = g_card.x + p, w = g_card.w - 2 * p;
    bold("Keys", s, x, g_card.y + p, w / 3, bg);
    if (g_cur) {
        char t[96];
        snprintf(t, sizeof t, "%s, %s", g_cur->name, g_cur->file);
        dim(t, s, x + ugfx_text_width("Keys") + p, g_card.y + p, w - ugfx_text_width("Keys") - p, bg);
    }
    g_sheet.bg = bg;
    uui_keysheet_draw(s, &g_sheet);
    int fy = g_r[C_RESUME].y;
    ugfx_fill_rect(s, g_card.x + 1, fy - p / 2, g_card.w - 2, 1, UTHEME_SEPARATOR);
    dim("Paused. F1 again: DOOM's own help.", s, x, fy + (btn_h() - u()) / 2,
        g_r[C_RESUME].x - x - p, bg);
    button(s, C_RESUME, "Resume", 1);
    log_layout();
}

static const char *const CTL_NAME[CTLS] = {
    [C_START] = "start", [C_CHANGE] = "change", [C_SHOW] = "show",
    [C_FILE] = "usefile", [C_CANCEL] = "cancel", [C_PRIMARY] = "primary",
    [C_STOP] = "stop", [C_RESUME] = "resume",
};

// Every control where it is, for a test to press by asking.
static void log_layout(void) {
    static const char *const VIEW[] = { "game", "page", "data", "fetch" };
    uapp_logf_layout("doom: layout view %s%s\n", VIEW[g_view], g_help ? " sheet" : "");
    for (int i = 0; i < CTLS; i++) {
        if (g_r[i].w <= 0) continue;
        if (CTL_NAME[i]) uapp_logf_layout("doom: layout %s %d %d %d %d\n", CTL_NAME[i],
                                          g_r[i].x, g_r[i].y, g_r[i].w, g_r[i].h);
        else uapp_logf_layout("doom: layout row%d %d %d %d %d %s\n", i - C_ROW0, g_r[i].x,
                              g_r[i].y, g_r[i].w, g_r[i].h, DOOM_IWADS[g_rows[i - C_ROW0]].file);
    }
    if (g_view == V_DATA) uapp_logf_layout("doom: layout selected %d\n", g_sel);
}

static int ctl_at(int x, int y) {
    for (int i = 0; i < CTLS; i++) if (in(&g_r[i], x, y)) return i;
    return -1;
}

void doom_front_press(struct uapp *a, int x, int y, unsigned mods) {
    (void)mods;
    layout(a);
    g_pressed = ctl_at(x, y);
    uapp_redraw(a);
}

void doom_front_motion(struct uapp *a, int x, int y, unsigned buttons) {
    (void)buttons;
    layout(a);
    int h = ctl_at(x, y);
    if (g_view == V_PAGE) uui_checkbox_hover(&g_show, x, y);
    if (h != g_hot) { g_hot = h; uapp_redraw(a); }
}

void doom_front_release(struct uapp *a, int x, int y, unsigned buttons) {
    (void)buttons;
    layout(a);
    int id = g_pressed;
    g_pressed = -1;
    if (id >= 0 && ctl_at(x, y) == id) activate(a, id);   // commit over the same control
    else uapp_redraw(a);
}

int doom_front_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    int enter = key == '\n' || key == '\r';
    if (g_view == V_PAGE) {
        if (enter) { activate(a, C_START); return 1; }
    } else if (g_view == V_DATA) {
        if (key == KEY_ARROW_UP && g_sel > 0) { g_sel--; uapp_redraw(a); return 1; }
        if (key == KEY_ARROW_DOWN && g_sel + 1 < g_row_n) { g_sel++; uapp_redraw(a); return 1; }
        if (enter) { activate(a, C_PRIMARY); return 1; }
        if (key == 0x1B && g_back) { activate(a, C_CANCEL); return 1; }
    } else if (g_view == V_FETCH) {
        if (key == 0x1B) { g_cancel = 1; return 1; }
    }
    return 0;
}

int doom_front_user(struct uapp *a, int a0, int a1) {
    (void)a1;
    if (a0 == EV_PROGRESS) { uapp_redraw(a); return 1; }
    if (a0 != EV_DONE) return 0;
    const struct doom_iwad *w = &DOOM_IWADS[g_fetch_i];
    if (g_fetch_rc == 0) {
        ulogf("doom: downloaded %s, checksum matches\n", w->file);
        doom_iwad_choose(w);
        g_cur = w;
        // Straight into the game, as the card promised.
        doom_start_game(a, w);
        return 1;
    }
    ulogf("doom: download of %s failed: %s\n", w->file, g_fetch_err);
    snprintf(g_note, sizeof g_note, "%s", g_cancel ? "Download cancelled." : g_fetch_err);
    show_data(a, g_back);
    return 1;
}

void doom_front_leave(void) { g_view = V_GAME; }
