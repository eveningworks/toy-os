// See crash_notice.h.
//
// FOUR KINDS OF CARD share the timing and the drawing: a crash (polled
// from the kernel), a FILE a client made (WIN_REQ_NOTICE, a screenshot so
// far), which carries a thumbnail and Open / Copy / Folder, the windows a
// Close all left open, and a REMOTE DESKTOP REQUEST. The compositor acts
// on the first three itself, because the client is usually gone by the
// time anyone clicks; a request is the one whose answer goes BACK to the
// client that asked (WIN_EV_REMOTE_ANSWER), which is waiting for it.
#include "wm_internal.h"
#include "wm_overlay.h"
#include "wm_shadow.h"
#include "crash_notice.h"
#include "wm_flash.h"
#include "gui_apps.h"
#include "lib/icon_cache.h"
#include "ui/uui.h"
#include "ui/uui_label.h"   // uui_label_wrap_next() -- the sentence wraps
#include "ui/utheme.h"
#include "query_abi.h"
#include "kapi.h"
#include "rt/sys.h"   // sys_query_record(), sys_spawn(), sys_monotonic_ns()
#include "lib/uimg.h"
#include "lib/uclip.h"
#include "lib/uopen.h"
#include "win_proto.h"
#include "wm_log.h"
#include "ui/ulog.h"
#include <stdio.h>

#define MAX_NOTICES   3
#define SHOW_NS       12000000000ull   // a notice stays 12 s, longer while hovered
#define POLL_NS       500000000ull
#define RECENT_NS     60000000000ull   // "the desktop restarted" only if it just did

// The close box, then up to four actions; what they are is the kind's.
enum { BTN_CLOSE = 0, BTN_A, BTN_B, BTN_C, BTN_D, BTNS };
#define BTN_DETAILS BTN_A   // a crash's
#define BTN_REOPEN  BTN_B
#define BTN_SHOW    BTN_A   // a close-all's
#define BTN_FORCE   BTN_B
#define BTN_DENY    BTN_A   // a remote request's
#define BTN_VIEW    BTN_B
#define BTN_ALLOW   BTN_C
#define BTN_ALWAYS  BTN_D   // its "Always allow" box, a toggle
enum { KIND_CRASH = 0, KIND_SHOT, KIND_STAYED, KIND_REMOTE, KIND_ACTION };
#define ASK_NS ((uint64_t)WIN_REMOTE_ASK_S * 1000000000ull)

#define PATH_MAX_NOTICE (WIN_NOTICE_PIECES_MAX * (WIN_TITLE_LEN - 1) + 1)

struct notice {
    int kind;
    char title[72];
    char sub[96];
    char icon[32];
    char report[QUERY_CRASH_REPORT_MAX];
    char program[QUERY_CRASH_PROGRAM_MAX];
    int can_reopen;
    char path[PATH_MAX_NOTICE];   // a file notice's file
    struct uimg thumb;            // ...and its picture, scaled to the card; owned
    uint32_t seq[NOTICE_STAYED_MAX];   // a close-all's windows, by open_seq
    int nwin;
    int pid;                      // a remote request's asker: the answer goes there
    int always;                   // ...and its box
    int no_always;                // ...which this request does not offer
    char peer[16];
    int shown_left;               // the countdown's second as last drawn
    char action[24];              // an action card's one button ("Undo")
    void (*act)(void);            // ...and what it does
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

static void card_rect(int i, int *x, int *y, int *w, int *h);

static void push(const struct notice *n) {
    // The oldest falls off a full stack: its thumbnail goes with it.
    if (g_count == MAX_NOTICES) uimg_free(&g_n[MAX_NOTICES - 1].thumb);
    for (int i = MAX_NOTICES - 1; i > 0; i--) g_n[i] = g_n[i - 1];
    g_n[0] = *n;
    g_n[0].until = sys_monotonic_ns() + (n->kind == KIND_REMOTE ? ASK_NS : SHOW_NS);
    if (g_count < MAX_NOTICES) g_count++;
    // THE STACK FITS THE SCREEN: picture cards are tall, and one off the
    // top would have its buttons out of reach. The oldest goes.
    for (;;) {
        int x, y, w, h;
        card_rect(g_count - 1, &x, &y, &w, &h);
        if (y >= 0 || g_count <= 1) break;
        uimg_free(&g_n[g_count - 1].thumb);
        g_count--;
    }
    crash_notice_open = 1;
    crash_notice_damage();
}

static void drop(int i) {
    crash_notice_damage();
    uimg_free(&g_n[i].thumb);
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

// --- the windows a Close all left open -------------------------------------

void crash_notice_stayed(const char *title, const char *sub, const char *icon,
                         const uint32_t *seq, int n) {
    struct notice c;
    k_memset(&c, 0, sizeof c);
    c.kind = KIND_STAYED;
    k_strlcpy(c.title, title, sizeof c.title);
    k_strlcpy(c.sub, sub, sizeof c.sub);
    k_strlcpy(c.icon, icon ? icon : "tb-close-all", sizeof c.icon);
    if (n > NOTICE_STAYED_MAX) n = NOTICE_STAYED_MAX;
    for (int k = 0; k < n; k++) c.seq[k] = seq[k];
    c.nwin = n;
    push(&c);
}

// --- a card with one action -------------------------------------------------

void crash_notice_action(const char *title, const char *sub, const char *icon,
                         const char *action, void (*act)(void)) {
    struct notice c;
    k_memset(&c, 0, sizeof c);
    c.kind = KIND_ACTION;
    k_strlcpy(c.title, title, sizeof c.title);
    k_strlcpy(c.sub, sub ? sub : "", sizeof c.sub);
    k_strlcpy(c.icon, icon ? icon : "tb-undo", sizeof c.icon);
    k_strlcpy(c.action, action, sizeof c.action);
    c.act = act;
    // A newer action replaces the old one's card: only the latest can be taken back.
    for (int i = g_count - 1; i >= 0; i--) if (g_n[i].kind == KIND_ACTION) drop(i);
    push(&c);
}

static int stayed_open(const struct notice *n) {
    for (int k = 0; k < n->nwin; k++)
        if (wm_window_by_seq(n->seq[k]) >= 0) return 1;
    return 0;
}

// --- a file a client made (WIN_REQ_NOTICE) --------------------------------

static const char *basename_of(const char *p);

// The path arrives in pieces; one assembly per sender, the last piece
// puts the card up.
#define ASSEMBLIES 4
static struct { int pid; int have; char buf[PATH_MAX_NOTICE]; } g_asm[ASSEMBLIES];

// A SCREENSHOT'S PATH ONLY: absolute, a .qoi or .png, plain characters,
// no `..`. Any client may send the request, and the card decodes the
// file in the compositor and offers to open it -- so it is held to what
// the one kind ever names, which also keeps it fit for a JSON string.
// The FOLDER is the person's choice (Screenshot's Options), so it is not
// pinned to /home/screenshots; no SPACE, because Folder hands the path
// to Files as its argument line.
static int fit_path(const char *p) {
    if (p[0] != '/') return 0;
    for (const char *c = p; *c; c++) {
        int ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
                 *c == '/' || *c == '-' || *c == '_' || *c == '.';
        if (!ok) return 0;
        if (c[0] == '.' && c[1] == '.') return 0;
    }
    size_t n = k_strlen(p);
    return n > 4 && (!k_strcmp(p + n - 4, ".qoi") || !k_strcmp(p + n - 4, ".png"));
}

#define THUMB_SOURCE_MAX (64u * 1024 * 1024)   // a larger file gets a card without a picture

static void tell_file(int kind, unsigned flags, const char *path) {
    if (kind != WIN_NOTICE_SCREENSHOT || !fit_path(path)) return;
    if (flags & WIN_NOTICE_F_FLASH) {
        wm_flash_start();
        redraw_pending = 1;
    }
    // THE ACTION A TEST WAITS FOR: the flash lasts a quarter of a second.
    ulogf("wm: notice %s%s %s\n", (flags & WIN_NOTICE_F_FLASH) ? "flash " : "",
            (flags & WIN_NOTICE_F_NO_CARD) ? "no-card" : "card", path);
    if (flags & WIN_NOTICE_F_NO_CARD) return;
    struct notice n;
    k_memset(&n, 0, sizeof n);
    n.kind = KIND_SHOT;
    k_strlcpy(n.path, path, sizeof n.path);
    k_strlcpy(n.title, "Screenshot saved", sizeof n.title);
    k_strlcpy(n.icon, "screenshot", sizeof n.icon);
    // THE PICTURE, fitted to the card once: a full-size frame is never
    // kept, and a file that will not decode is a card without one.
    struct uimg full;
    k_memset(&full, 0, sizeof full);
    char dims[24] = "";
    struct sys_stat st;
    if (sys_stat(path, &st) == 0 && st.size <= THUMB_SOURCE_MAX && uimg_load(path, &full) == 0) {
        int tw = ugfx_char_advance('n') * 44 - 2 * (ugfx_char_h() * 2 / 3);
        int th = tw * full.h / (full.w > 0 ? full.w : 1);
        int maxh = tw * 9 / 16;
        if (th > maxh) { th = maxh; tw = th * full.w / (full.h > 0 ? full.h : 1); }
        if (tw > 0 && th > 0) uimg_scale(&full, tw, th, &n.thumb);
        snprintf(dims, sizeof dims, ", %d x %d", full.w, full.h);
        uimg_free(&full);
    }
    snprintf(n.sub, sizeof n.sub, "%s%s%s", basename_of(path), dims,
             (flags & WIN_NOTICE_F_COPIED) ? " -- on the clipboard" : "");
    push(&n);
}

// --- a remote desktop request (WIN_NOTICE_REMOTE) ------------------------

static void answer(const struct notice *n, int a, int always) {
    struct win_event ev;
    k_memset(&ev, 0, sizeof ev);
    ev.type = WIN_EV_REMOTE_ANSWER;
    ev.a = a;
    ev.b = always;
    wm_client_push_event(n->pid, &ev);
    ulogf("wm: remote request %s from %s answered %s%s\n", n->peer, n->peer,
          a == WIN_REMOTE_ALLOW ? "allow" : a == WIN_REMOTE_VIEW ? "view-only" : "deny",
          always ? " always" : "");
}

// A REQUEST IS ONLY /bin/remoted's TO MAKE: any client may send the
// message, and a card that asks "let this address use your screen?" is
// exactly what a hostile program would forge to get a click.
static void tell_remote(int pid, unsigned flags, const char *text) {
    if (!wm_pid_exec_is(pid, "/bin/remoted")) {
        wm_logf("notice: remote request from pid %d refused -- not /bin/remoted\n", pid);
        return;
    }
    struct notice n;
    k_memset(&n, 0, sizeof n);
    n.kind = KIND_REMOTE;
    n.pid = pid;
    n.no_always = (flags & WIN_NOTICE_F_NO_ALWAYS) != 0;
    int i = 0;
    for (; text[i] && text[i] != ' ' && i < (int)sizeof n.peer - 1; i++) n.peer[i] = text[i];
    n.peer[i] = 0;
    const char *proto = text[i] == ' ' ? text + i + 1 : "";
    k_strlcpy(n.title, "Remote desktop request", sizeof n.title);
    snprintf(n.sub, sizeof n.sub, "%s wants to see and use this screen. %s, password accepted.",
             n.peer, proto[0] ? proto : "Remote desktop");
    k_strlcpy(n.icon, "tray-remote", sizeof n.icon);
    push(&n);
    ulogf("wm: remote request from %s (%s)\n", n.peer, proto);
}

// Is the asker still waiting? Its session is in the kernel's list for as
// long as it lives (QUERY_REMOTESESS) -- a viewer that hung up takes its
// question with it.
static int asker_alive(int pid) {
    struct query_remotesess q;
    for (int i = 0; sys_query_record(QUERY_REMOTESESS, i, &q, sizeof q) > 0; i++)
        if ((int)q.pid == pid) return 1;
    return 0;
}

void crash_notice_piece(int pid, int a, int kind, unsigned flags, const char *text) {
    int idx = a & 0xff, pieces = (a >> 8) & 0xff;
    if (pieces < 1 || pieces > WIN_NOTICE_PIECES_MAX || idx >= pieces) return;
    int slot = -1;
    for (int i = 0; i < ASSEMBLIES; i++) if (g_asm[i].pid == pid) slot = i;
    if (slot < 0) for (int i = 0; i < ASSEMBLIES; i++) if (!g_asm[i].pid) { slot = i; break; }
    if (slot < 0) return;     // four senders mid-path already: refused, not stolen
    if (idx == 0) { g_asm[slot].pid = pid; g_asm[slot].have = 0; g_asm[slot].buf[0] = 0; }
    if (g_asm[slot].pid != pid || g_asm[slot].have != idx) { g_asm[slot].pid = 0; return; }
    k_strlcpy(g_asm[slot].buf + idx * (WIN_TITLE_LEN - 1), text,
              sizeof g_asm[slot].buf - (unsigned)(idx * (WIN_TITLE_LEN - 1)));
    g_asm[slot].have = idx + 1;
    if (g_asm[slot].have == pieces) {
        g_asm[slot].pid = 0;
        if (kind == WIN_NOTICE_REMOTE) tell_remote(pid, flags, g_asm[slot].buf);
        else                           tell_file(kind, flags, g_asm[slot].buf);
    }
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
    for (int i = g_count - 1; i >= 0; i--) {
        if (g_n[i].kind == KIND_REMOTE) {
            // NOT KEPT BY HOVERING, unlike the others: the asker's own
            // wait ends a little after this, and the answer must be ours.
            if (now >= g_n[i].until) { answer(&g_n[i], WIN_REMOTE_DENY, 0); drop(i); }
            else if (now >= g_next_poll && !asker_alive(g_n[i].pid)) drop(i);
            else if ((int)((g_n[i].until - now) / 1000000000ull) != g_n[i].shown_left) {
                g_n[i].shown_left = (int)((g_n[i].until - now) / 1000000000ull);
                crash_notice_damage();   // the countdown, once a second
            }
            continue;
        }
        if ((now >= g_n[i].until && g_hover_card != i) ||
            (g_n[i].kind == KIND_STAYED && !stayed_open(&g_n[i])))   // saved and closed meanwhile
            drop(i);
    }
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
static int icon_sz(void) { return ugfx_char_h() * 2; }
// The sentence's width beside the icon: crash_notice_draw() wraps it in this.
static int sentence_w(void) { return card_w() - 3 * pad() - icon_sz(); }

int crash_notice_sub_fits(const char *sub) {
    char line[sizeof g_n[0].sub];
    if (!sub || k_strlen(sub) >= sizeof line) return 0;
    const char *rest = uui_label_wrap_next(sub, sentence_w(), line, sizeof line);
    return !rest || !*rest || ugfx_text_width(rest) <= sentence_w();
}
// A title line and up to two of sentence, then the buttons -- and a
// file's thumbnail above all of it.
static int card_h_of(int i) {
    int h = 2 * pad() + 3 * ugfx_char_h() + ugfx_char_h() / 2 + utheme_control_h() + pad();
    if (g_n[i].kind == KIND_SHOT) {
        h -= ugfx_char_h();                      // one line of sentence is enough
        if (g_n[i].thumb.px) h += g_n[i].thumb.h + pad();
    }
    if (g_n[i].kind == KIND_REMOTE && !g_n[i].no_always)
        h += ugfx_char_h() + pad() / 2;   // the "Always allow" box
    return h;
}
static int gap(void)    { return pad(); }

// A REMOTE REQUEST STANDS AT THE TOP RIGHT, apart from the stack above
// the taskbar: it is a question with a deadline, and it must not sit
// under the tray flyout that shows the session it would open.
static void card_rect(int i, int *x, int *y, int *w, int *h) {
    *w = card_w(); *h = card_h_of(i);
    *x = screen_w - *w - gap();
    int remote = g_n[i].kind == KIND_REMOTE;
    int before = 0;   // the cards of the same stack nearer its edge (newer)
    for (int k = 0; k < i; k++)
        if ((g_n[k].kind == KIND_REMOTE) == remote) before += card_h_of(k) + gap();
    if (remote) *y = gap() + before;
    else        *y = screen_h - taskbar_h - gap() - before - *h;
}

static const char *remote_label(int b) {
    return b == BTN_DENY ? "Deny" : b == BTN_VIEW ? "View only" : "Allow";
}

static const char *stayed_label(int b) { return b == BTN_SHOW ? "Show it" : "Force Quit"; }

static const char *shot_label(int b) {
    return b == BTN_A ? "Open" : b == BTN_B ? "Copy" : b == BTN_C ? "Folder" : "Save as...";
}

static int button_rect(int i, int b, int *x, int *y, int *w, int *h) {
    int cx, cy, cw, chh;
    card_rect(i, &cx, &cy, &cw, &chh);
    const struct notice *n = &g_n[i];
    if (b == BTN_CLOSE) {
        if (n->kind == KIND_REMOTE) return 0;   // a countdown stands there; Deny is the close
        *w = *h = ugfx_char_h() + 4;
        *x = cx + cw - *w - pad() / 2; *y = cy + pad() / 2;
        return 1;
    }
    *h = utheme_control_h();
    *y = cy + chh - pad() - *h;
    if (n->kind == KIND_REMOTE) {
        if (b == BTN_ALWAYS) {
            if (n->no_always) return 0;
            // The whole line is the box's target, as a checkbox's label is.
            *x = cx + pad();
            *w = cw - 2 * pad();
            *h = ugfx_char_h() + 4;
            *y = cy + chh - pad() - utheme_control_h() - pad() / 2 - *h;
            return 1;
        }
        // Right-aligned, Allow outermost and filled: Deny, View only, Allow.
        int right = cx + cw - pad();
        for (int k = BTN_ALLOW; k >= BTN_DENY; k--) {
            int bw = ugfx_text_width(remote_label(k)) + 2 * ugfx_char_w();
            if (k == b) { *w = bw; *x = right - bw; return 1; }
            right -= bw + pad() / 2;
        }
        return 0;
    }
    if (n->kind == KIND_SHOT) {
        // Left to right under the picture: Open (the primary), Copy,
        // Folder, Save as.
        int bx = cx + pad();
        for (int k = BTN_A; k <= b; k++) {
            *w = ugfx_text_width(shot_label(k)) + 2 * ugfx_char_w();
            if (k == b) { *x = bx; return 1; }
            bx += *w + pad() / 2;
        }
        return 0;
    }
    if (b == BTN_C || b == BTN_D) return 0;
    int right = cx + cw - pad();
    if (n->kind == KIND_ACTION) {
        if (b != BTN_A) return 0;
        *w = ugfx_text_width(n->action) + 2 * ugfx_char_w();
        *x = right - *w;
        return 1;
    }
    if (n->kind == KIND_STAYED) {
        // Right-aligned, Force Quit last: Show it, then the one that loses work.
        int fw = ugfx_text_width(stayed_label(BTN_FORCE)) + 2 * ugfx_char_w();
        int sw = ugfx_text_width(stayed_label(BTN_SHOW)) + 2 * ugfx_char_w();
        *w = b == BTN_FORCE ? fw : sw;
        *x = b == BTN_FORCE ? right - fw : right - fw - pad() / 2 - sw;
        return 1;
    }
    int bw = ugfx_text_width("Details") + 2 * ugfx_char_w();
    int rw = ugfx_text_width("Reopen") + 2 * ugfx_char_w();
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

const char *crash_notice_button(int b, int r[4]) {
    if (!g_count || b < BTN_A || b > BTN_D) return 0;
    if (!button_rect(0, b, &r[0], &r[1], &r[2], &r[3])) return 0;
    if (g_n[0].kind == KIND_SHOT) return shot_label(b);
    if (g_n[0].kind == KIND_STAYED) return stayed_label(b);
    if (g_n[0].kind == KIND_REMOTE) return b == BTN_ALWAYS ? "Always allow" : remote_label(b);
    if (g_n[0].kind == KIND_ACTION) return g_n[0].action;
    return b == BTN_DETAILS ? "Details" : "Reopen";
}

const char *crash_notice_path(void) { return g_count && g_n[0].kind == KIND_SHOT ? g_n[0].path : 0; }
const char *crash_notice_sub(void)  { return g_count ? g_n[0].sub : 0; }

const char *crash_notice_describe(int details[4], int reopen[4]) {
    if (!g_count) return 0;
    if (g_n[0].kind != KIND_CRASH) {   // only a crash has Details and Reopen
        for (int i = 0; i < 4; i++) details[i] = reopen[i] = 0;
        return g_n[0].title;
    }
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
    if (g_n[i].kind == KIND_REMOTE) {
        if (b == BTN_ALWAYS) {   // a toggle: the question stays up
            g_n[i].always = !g_n[i].always;
            crash_notice_damage();
            return 1;
        }
        struct notice q = g_n[i];
        drop(i);
        answer(&q, b == BTN_ALLOW ? WIN_REMOTE_ALLOW : b == BTN_VIEW ? WIN_REMOTE_VIEW
                   : WIN_REMOTE_DENY, q.always);
        return 1;
    }
    struct notice n = g_n[i];
    n.thumb.px = 0;   // drop() frees the card's picture; this copy must not
    drop(i);
    if (n.kind == KIND_ACTION) {
        if (b == BTN_A && n.act) n.act();
        return 1;
    }
    if (n.kind == KIND_SHOT) {
        if (b == BTN_A) {
            int pid = uopen_spawn(n.path);
            if (pid > 0) wm_track_launched(pid);
        } else if (b == BTN_B) {
            // THE FILE, as the File Manager's Copy puts one: this
            // clipboard holds files or text, not pixels (lib/uclip.h).
            uclip_begin(0, UCLIP_COPY);
            if (uclip_add(0, n.path)) uclip_commit(0);
        } else if (b == BTN_C) {
            // Handed the FILE, the File Manager opens its folder with it
            // selected (docs/conventions/gui.md).
            int pid = sys_spawn("/bin/wm/apps/files", n.path, -1);
            if (pid > 0) wm_track_launched(pid);
        } else if (b == BTN_D) {
            // The compositor cannot hold a chooser: Screenshot opens one
            // over a preview of the file and writes the copy.
            char args[PATH_MAX_NOTICE + 16];
            snprintf(args, sizeof args, "--save-as %s", n.path);
            int pid = sys_spawn("/bin/wm/apps/screenshot", args, -1);
            if (pid > 0) wm_track_launched(pid);
        }
        return 1;
    }
    if (n.kind == KIND_STAYED) {
        if (b == BTN_SHOW) {
            for (int k = 0; k < n.nwin; k++) {
                int w = wm_window_by_seq(n.seq[k]);
                if (w < 0) continue;
                wm_bring_forward(w);   // its question with it
                break;
            }
        } else if (b == BTN_FORCE) {
            // SIGKILL, as Task Manager's Force Quit: the client's exit
            // takes its windows down through the ordinary path.
            for (int k = 0; k < n.nwin; k++) {
                int w = wm_window_by_seq(n.seq[k]);
                if (w < 0) continue;
                if (windows[w].client_pid > 0) {
                    wm_logf("closeall: force-quitting pid %d\n", windows[w].client_pid);
                    sys_kill(windows[w].client_pid, SIGKILL);
                } else {
                    close_window(w);
                }
            }
        }
        return 1;
    }
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
        int top = y;
        if (n->kind == KIND_SHOT && n->thumb.px) {
            // The picture, centred, on a hairline frame.
            int px = x + (w - n->thumb.w) / 2, py = y + pad() + ugfx_char_h() + 4;
            ugfx_draw_rect(s, px - 1, py - 1, n->thumb.w + 2, n->thumb.h + 2, UTHEME_OUTLINE);
            ugfx_blit(s, px, py, n->thumb.w, n->thumb.h, n->thumb.px, n->thumb.w);
            top = py + n->thumb.h + pad() - pad();
        }
        int isz = icon_sz();
        int tx, tw;
        if (n->kind == KIND_SHOT) {
            // The title rides ABOVE the picture, the file under it.
            tx = x + pad();
            tw = x + w - pad() - (ugfx_char_h() + 4) - tx;
        } else {
            const struct uimg *ico = icon_get(n->icon, isz);
            if (ico) ugfx_blit_alpha(s, x + pad(), y + pad(), ico->w, ico->h, ico->px, ico->w);
            tx = x + pad() + isz + pad();
            tw = x + w - pad() - (ugfx_char_h() + 4) - tx;
        }
        const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
        ugfx_draw_string_elided(s, tx, y + pad(), tw, n->title, UTHEME_TEXT, UTHEME_PANEL_BG);
        ugfx_set_font(was);
        if (n->kind == KIND_REMOTE) {
            // THE TIME LEFT, where the close box would be: unanswered
            // means denied, and the person should see when.
            uint64_t now = sys_monotonic_ns();
            int left = now >= n->until ? 0 : (int)((n->until - now + 999999999ull) / 1000000000ull);
            char cd[8];
            snprintf(cd, sizeof cd, "0:%02d", left);
            int cw = ugfx_text_width(cd);
            ugfx_draw_string(s, x + w - pad() - cw, y + pad(),
                             cd, uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED), UTHEME_PANEL_BG);
        }
        // The sentence WRAPS, two lines at most; the second elides.
        const char *rest = n->sub;
        int sw = x + w - pad() - tx, sy = y + pad() + ugfx_char_h() + ugfx_char_h() / 3;
        if (n->kind == KIND_SHOT) sy = (n->thumb.px ? top + pad() : sy);
        uint32_t dim = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
        for (int line = 0; line < (n->kind == KIND_SHOT ? 1 : 2) && rest && *rest;
             line++, sy += ugfx_char_h()) {
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
            if (n->kind == KIND_REMOTE) {
                if (b == BTN_ALWAYS) {
                    int box = ugfx_char_h() - 2, bx0 = bx, by0 = by + (bh - box) / 2;
                    uui_fill_round_rect(s, bx0, by0, box, box, 3,
                                        n->always ? UTHEME_ACCENT : UTHEME_OUTLINE);
                    if (!n->always)
                        uui_fill_round_rect(s, bx0 + 1, by0 + 1, box - 2, box - 2, 2, UTHEME_PANEL_BG);
                    else {   // the tick
                        ugfx_draw_line(s, bx0 + box / 4, by0 + box / 2, bx0 + box / 2 - 1,
                                       by0 + box * 3 / 4, UTHEME_ACCENT_TEXT, GEOM_AA);
                        ugfx_draw_line(s, bx0 + box / 2 - 1, by0 + box * 3 / 4, bx0 + box * 3 / 4 + 1,
                                       by0 + box / 4, UTHEME_ACCENT_TEXT, GEOM_AA);
                    }
                    char line[48];
                    snprintf(line, sizeof line, "Always allow %s", n->peer);
                    ugfx_draw_string_clipped(s, bx0 + box + pad() / 2, by + (bh - ugfx_char_h()) / 2,
                                             bw - box - pad() / 2, line,
                                             hot ? UTHEME_ACCENT : UTHEME_TEXT, UTHEME_PANEL_BG);
                    continue;
                }
                int allow = b == BTN_ALLOW;
                uui_button_draw(s, bx, by, bw, bh, remote_label(b),
                                allow ? UTHEME_ACCENT : UTHEME_BUTTON_BG,
                                allow ? UTHEME_ACCENT_TEXT : UTHEME_TEXT,
                                hot ? UUI_STATE_HOVER : UUI_STATE_REST);
                continue;
            }
            int shot = n->kind == KIND_SHOT, stayed = n->kind == KIND_STAYED;
            int action = n->kind == KIND_ACTION;
            int primary = shot || action ? b == BTN_A : !stayed && b == BTN_REOPEN;
            int danger = stayed && b == BTN_FORCE;
            uint32_t bg = danger ? utheme_action(UTHEME_ACT_DANGER)
                        : primary ? UTHEME_ACCENT : UTHEME_BUTTON_BG;
            const char *label = action ? n->action : shot ? shot_label(b) : stayed ? stayed_label(b)
                              : b == BTN_REOPEN ? "Reopen" : "Details";
            uui_button_draw(s, bx, by, bw, bh, label, bg,
                            primary || danger ? UTHEME_ACCENT_TEXT : UTHEME_TEXT,
                            hot ? UUI_STATE_HOVER : UUI_STATE_REST);
        }
    }
}
