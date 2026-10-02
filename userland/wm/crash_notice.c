// See crash_notice.h.
#include "wm_internal.h"
#include "wm_overlay.h"
#include "wm_shadow.h"
#include "crash_notice.h"
#include "gui_apps.h"
#include "lib/icon_cache.h"
#include "ui/uui.h"
#include "ui/uui_label.h"   // uui_label_wrap_next() -- the sentence wraps
#include "ui/utheme.h"
#include "query_abi.h"
#include "kapi.h"
#include "rt/sys.h"   // sys_query_record(), sys_spawn(), sys_monotonic_ns()
#include <stdio.h>

#define MAX_NOTICES   3
#define SHOW_NS       12000000000ull   // a notice stays 12 s, longer while hovered
#define POLL_NS       500000000ull
#define RECENT_NS     60000000000ull   // "the desktop restarted" only if it just did

enum { BTN_CLOSE = 0, BTN_DETAILS, BTN_REOPEN, BTNS };

struct notice {
    char title[72];
    char sub[96];
    char icon[32];
    char report[QUERY_CRASH_REPORT_MAX];
    char program[QUERY_CRASH_PROGRAM_MAX];
    int can_reopen;
    uint64_t until;
};

int crash_notice_open;
static struct notice g_n[MAX_NOTICES];   // [0] is the newest, drawn lowest
static int g_count;
static uint32_t g_seen;                  // the newest crash seq already told
static uint64_t g_next_poll;
static int g_hover = -1;                 // the button under the pointer: card * BTNS + button
static int g_hover_card = -1;            // the card under it, which does not expire

static const struct gui_app *app_by_exec(const char *exec) {
    for (int i = 0; i < gui_app_visible_count(GUI_SHOW_ALL); i++) {
        const struct gui_app *a = gui_app_visible_at(GUI_SHOW_ALL, i);
        if (a && a->exec_path && k_strcmp(a->exec_path, exec) == 0) return a;
    }
    return 0;
}

static void push(const struct notice *n) {
    for (int i = MAX_NOTICES - 1; i > 0; i--) g_n[i] = g_n[i - 1];
    g_n[0] = *n;
    g_n[0].until = sys_monotonic_ns() + SHOW_NS;
    if (g_count < MAX_NOTICES) g_count++;
    crash_notice_open = 1;
    crash_notice_damage();
}

static void drop(int i) {
    crash_notice_damage();
    for (int j = i; j < g_count - 1; j++) g_n[j] = g_n[j + 1];
    g_count--;
    crash_notice_open = g_count > 0;
}

static const char *basename_of(const char *p) {
    const char *b = p;
    for (const char *c = p; *c; c++) if (*c == '/') b = c + 1;
    return b;
}

// What stopped it, as a sentence: "A page fault stopped it." for a
// fault, "It was killed by SIGSEGV." for a core-dumping signal.
static void cause(const char *fault, char *out, int cap) {
    if (k_strncmp(fault, "Killed by ", 10) == 0) {
        snprintf(out, (size_t)cap, "It was killed by %s.", fault + 10);
        return;
    }
    char f[QUERY_CRASH_FAULT_MAX];
    k_strlcpy(f, fault, sizeof f);
    if (f[0] >= 'A' && f[0] <= 'Z') f[0] = (char)(f[0] + 32);
    snprintf(out, (size_t)cap, "A %s stopped it.", f);
}

static void tell(const struct query_crash *c) {
    struct notice n;
    k_memset(&n, 0, sizeof n);
    const struct gui_app *a = app_by_exec(c->program);
    const char *name = a && a->name ? a->name : basename_of(c->program);
    snprintf(n.title, sizeof n.title, "%s closed unexpectedly", name);
    char why[64];
    cause(c->fault, why, sizeof why);
    snprintf(n.sub, sizeof n.sub, c->report[0] ? "%s A report was saved."
                                                : "%s No report could be saved.", why);
    k_strlcpy(n.icon, a && a->icon_name && a->icon_name[0] ? a->icon_name : "crashreports",
              sizeof n.icon);
    k_strlcpy(n.report, c->report, sizeof n.report);
    k_strlcpy(n.program, c->program, sizeof n.program);
    n.can_reopen = a != 0;   // a desktop app; a daemon is init's to restart
    push(&n);
}

void crash_notice_init(void) {
    struct query_crash c, last;
    int have = 0;
    for (int i = 0; sys_query_record(QUERY_CRASH, i, &c, sizeof c) > 0; i++) { last = c; have = 1; }
    if (!have) return;
    g_seen = last.seq;
    // WE ARE THE RESTARTED DESKTOP when the newest crash is a toywm's and
    // recent -- init brings it back within a second or two.
    uint64_t now = sys_monotonic_ns();
    if (k_strcmp(basename_of(last.program), "toywm") != 0 || now - last.uptime_ns > RECENT_NS) return;
    struct notice n;
    k_memset(&n, 0, sizeof n);
    k_strlcpy(n.title, "The desktop restarted after a problem", sizeof n.title);
    char why[64];
    cause(last.fault, why, sizeof why);
    snprintf(n.sub, sizeof n.sub, "%s Windows that were open have closed.", why);
    k_strlcpy(n.icon, "crashreports", sizeof n.icon);
    k_strlcpy(n.report, last.report, sizeof n.report);
    push(&n);
}

void crash_notice_poll(void) {
    uint64_t now = sys_monotonic_ns();
    for (int i = g_count - 1; i >= 0; i--)
        if (now >= g_n[i].until && g_hover_card != i) drop(i);
    if (now < g_next_poll) return;
    g_next_poll = now + POLL_NS;
    struct query_crash c;
    for (int i = 0; sys_query_record(QUERY_CRASH, i, &c, sizeof c) > 0; i++) {
        if (c.seq <= g_seen) continue;
        g_seen = c.seq;
        tell(&c);
    }
}

// --- geometry ------------------------------------------------------------

static int pad(void)    { return ugfx_char_h() * 2 / 3; }
static int card_w(void) { return ugfx_char_advance('n') * 44; }
// A title line and up to two of sentence, then the buttons.
static int card_h(void) { return 2 * pad() + 3 * ugfx_char_h() + ugfx_char_h() / 2 + utheme_control_h() + pad(); }
static int gap(void)    { return pad(); }

static void card_rect(int i, int *x, int *y, int *w, int *h) {
    *w = card_w(); *h = card_h();
    *x = screen_w - *w - gap();
    *y = screen_h - taskbar_h - gap() - (i + 1) * *h - i * gap();
}

static int button_rect(int i, int b, int *x, int *y, int *w, int *h) {
    int cx, cy, cw, chh;
    card_rect(i, &cx, &cy, &cw, &chh);
    const struct notice *n = &g_n[i];
    if (b == BTN_CLOSE) {
        *w = *h = ugfx_char_h() + 4;
        *x = cx + cw - *w - pad() / 2; *y = cy + pad() / 2;
        return 1;
    }
    *h = utheme_control_h();
    *y = cy + chh - pad() - *h;
    int bw = ugfx_text_width("Details") + 2 * ugfx_char_w();
    int rw = ugfx_text_width("Reopen") + 2 * ugfx_char_w();
    int right = cx + cw - pad();
    if (b == BTN_REOPEN) {
        if (!n->can_reopen) return 0;
        *w = rw; *x = right - rw;
        return 1;
    }
    if (!n->report[0]) return 0;
    *w = bw; *x = right - (n->can_reopen ? rw + pad() / 2 : 0) - bw;
    return 1;
}

int crash_notice_rect(int *x, int *y, int *w, int *h) {
    if (!g_count) return 0;
    int x0, y0, ww, hh, x1, y1;
    card_rect(0, &x0, &y1, &ww, &hh);
    y1 += hh;
    card_rect(g_count - 1, &x1, &y0, &ww, &hh);
    int m = wm_shadow_margin();
    *x = x0 - m; *y = y0 - m; *w = ww + 2 * m; *h = y1 - y0 + 2 * m;
    return 1;
}

const char *crash_notice_describe(int details[4], int reopen[4]) {
    if (!g_count) return 0;
    for (int b = 0; b < 2; b++) {
        int *r = b ? reopen : details;
        if (!button_rect(0, b ? BTN_REOPEN : BTN_DETAILS, &r[0], &r[1], &r[2], &r[3]))
            r[0] = r[1] = r[2] = r[3] = 0;
    }
    return g_n[0].title;
}

void crash_notice_damage(void) { wm_overlay_damage("notice"); }

// --- input -----------------------------------------------------------------

#define CARD_TOKEN 1000   // + card index: on a card, not on a button

static int token_at(int mx, int my) {
    for (int i = 0; i < g_count; i++)
        for (int b = 0; b < BTNS; b++) {
            int x, y, w, h;
            if (button_rect(i, b, &x, &y, &w, &h) && uui_hit(x, y, w, h, mx, my)) return i * BTNS + b;
        }
    for (int i = 0; i < g_count; i++) {
        int x, y, w, h;
        card_rect(i, &x, &y, &w, &h);
        if (uui_hit(x, y, w, h, mx, my)) return CARD_TOKEN + i;
    }
    return -1;
}

int crash_notice_hover_at(int mx, int my) {
    int t = token_at(mx, my);
    g_hover = t >= 0 && t < CARD_TOKEN ? t : -1;
    g_hover_card = t < 0 ? -1 : t >= CARD_TOKEN ? t - CARD_TOKEN : t / BTNS;
    return t + 1;   // the core compares it, and 0 means "nothing"
}

int crash_notice_handle_click(int mx, int my) {
    int t = token_at(mx, my);
    if (t < 0) return 0;
    if (t >= CARD_TOKEN) return 1;   // on a card: swallowed, nothing to do
    int i = t / BTNS, b = t % BTNS;
    struct notice n = g_n[i];
    drop(i);
    if (b == BTN_DETAILS && n.report[0]) {
        char args[QUERY_CRASH_REPORT_MAX + 16];
        snprintf(args, sizeof args, "--report %s", n.report);
        int pid = sys_spawn("/bin/wm/apps/crashreports", args, -1);
        if (pid > 0) wm_track_launched(pid);
    } else if (b == BTN_REOPEN && n.program[0]) {
        int pid = sys_spawn(n.program, 0, -1);
        if (pid > 0) wm_track_launched(pid);
    }
    return 1;
}

// --- drawing -----------------------------------------------------------

void crash_notice_draw(int mx, int my) {
    (void)mx; (void)my;
    struct ugfx_surface *s = wm_surface();
    for (int i = 0; i < g_count; i++) {
        const struct notice *n = &g_n[i];
        int x, y, w, h;
        card_rect(i, &x, &y, &w, &h);
        int r = ugfx_char_h() / 2;
        wm_shadow_draw(x, y, w, h, r, WM_SHADOW_POPUP);
        uui_fill_round_rect(s, x, y, w, h, r, UTHEME_OUTLINE);
        uui_fill_round_rect(s, x + 1, y + 1, w - 2, h - 2, r - 1, UTHEME_PANEL_BG);
        int isz = ugfx_char_h() * 2;
        const struct uimg *ico = icon_get(n->icon, isz);
        if (ico) ugfx_blit_alpha(s, x + pad(), y + pad(), ico->w, ico->h, ico->px, ico->w);
        int tx = x + pad() + isz + pad(), tw = x + w - pad() - (ugfx_char_h() + 4) - tx;
        const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
        ugfx_draw_string_elided(s, tx, y + pad(), tw, n->title, UTHEME_TEXT, UTHEME_PANEL_BG);
        ugfx_set_font(was);
        // The sentence WRAPS, two lines at most; the second elides.
        const char *rest = n->sub;
        int sw = x + w - pad() - tx, sy = y + pad() + ugfx_char_h() + ugfx_char_h() / 3;
        uint32_t dim = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
        for (int line = 0; line < 2 && rest && *rest; line++, sy += ugfx_char_h()) {
            char buf[sizeof n->sub];
            const char *next = line == 0 ? uui_label_wrap_next(rest, sw, buf, sizeof buf) : 0;
            ugfx_draw_string_elided(s, tx, sy, sw, line == 0 ? buf : rest, dim, UTHEME_PANEL_BG);
            rest = next;
        }
        for (int b = 0; b < BTNS; b++) {
            int bx, by, bw, bh;
            if (!button_rect(i, b, &bx, &by, &bw, &bh)) continue;
            int hot = g_hover == i * BTNS + b;
            if (b == BTN_CLOSE) {
                if (hot) uui_fill_round_rect(s, bx, by, bw, bh, bh / 4,
                                             uui_state_bg(UTHEME_PANEL_BG, UUI_STATE_HOVER));
                int cx = bx + bw / 2, cy = by + bh / 2, k = bh / 5;
                ugfx_draw_line(s, cx - k, cy - k, cx + k, cy + k, UTHEME_TEXT, GEOM_AA);
                ugfx_draw_line(s, cx + k, cy - k, cx - k, cy + k, UTHEME_TEXT, GEOM_AA);
                continue;
            }
            int primary = b == BTN_REOPEN;
            uint32_t bg = primary ? UTHEME_ACCENT : UTHEME_BUTTON_BG;
            uui_button_draw(s, bx, by, bw, bh, b == BTN_REOPEN ? "Reopen" : "Details", bg,
                            primary ? UTHEME_ACCENT_TEXT : UTHEME_TEXT,
                            hot ? UUI_STATE_HOVER : UUI_STATE_REST);
        }
    }
}
