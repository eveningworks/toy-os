// See crash_notice.h.
//
// TWO KINDS OF CARD share the stack, the timing and the drawing: a
// crash (polled from the kernel) and a FILE a client made (WIN_REQ_NOTICE,
// a screenshot so far), which carries a thumbnail and Open / Copy /
// Folder. The compositor acts on those itself, because the client is
// usually gone by the time anyone clicks.
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
#include "lib/uimg.h"
#include "lib/uclip.h"
#include "lib/uopen.h"
#include "win_proto.h"
#include <stdio.h>

#define MAX_NOTICES   3
#define SHOW_NS       12000000000ull   // a notice stays 12 s, longer while hovered
#define POLL_NS       500000000ull
#define RECENT_NS     60000000000ull   // "the desktop restarted" only if it just did

// The close box, then up to three actions; what they are is the kind's.
enum { BTN_CLOSE = 0, BTN_A, BTN_B, BTN_C, BTNS };
#define BTN_DETAILS BTN_A   // a crash's
#define BTN_REOPEN  BTN_B
enum { KIND_CRASH = 0, KIND_SHOT };

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
    // The oldest falls off a full stack: its thumbnail goes with it.
    if (g_count == MAX_NOTICES) uimg_free(&g_n[MAX_NOTICES - 1].thumb);
    for (int i = MAX_NOTICES - 1; i > 0; i--) g_n[i] = g_n[i - 1];
    g_n[0] = *n;
    g_n[0].until = sys_monotonic_ns() + SHOW_NS;
    if (g_count < MAX_NOTICES) g_count++;
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

// --- a file a client made (WIN_REQ_NOTICE) --------------------------------

static const char *basename_of(const char *p);

// The path arrives in pieces; one assembly per sender, the last piece
// puts the card up.
#define ASSEMBLIES 4
static struct { int pid; int have; char buf[PATH_MAX_NOTICE]; } g_asm[ASSEMBLIES];

static void tell_file(int kind, unsigned flags, const char *path) {
    if (kind != WIN_NOTICE_SCREENSHOT) return;
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
    if (uimg_load(path, &full) == 0) {
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

void crash_notice_piece(int pid, int a, int kind, unsigned flags, const char *text) {
    int idx = a & 0xff, pieces = (a >> 8) & 0xff;
    if (pieces < 1 || pieces > WIN_NOTICE_PIECES_MAX || idx >= pieces) return;
    int slot = -1;
    for (int i = 0; i < ASSEMBLIES; i++) if (g_asm[i].pid == pid) slot = i;
    if (slot < 0) for (int i = 0; i < ASSEMBLIES; i++) if (!g_asm[i].pid) { slot = i; break; }
    if (slot < 0) slot = 0;   // a full table: the oldest assembly gives way
    if (idx == 0) { g_asm[slot].pid = pid; g_asm[slot].have = 0; g_asm[slot].buf[0] = 0; }
    if (g_asm[slot].pid != pid || g_asm[slot].have != idx) { g_asm[slot].pid = 0; return; }
    k_strlcpy(g_asm[slot].buf + idx * (WIN_TITLE_LEN - 1), text,
              sizeof g_asm[slot].buf - (unsigned)(idx * (WIN_TITLE_LEN - 1)));
    g_asm[slot].have = idx + 1;
    if (g_asm[slot].have == pieces) {
        g_asm[slot].pid = 0;
        tell_file(kind, flags, g_asm[slot].buf);
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
// A title line and up to two of sentence, then the buttons -- and a
// file's thumbnail above all of it.
static int card_h_of(int i) {
    int h = 2 * pad() + 3 * ugfx_char_h() + ugfx_char_h() / 2 + utheme_control_h() + pad();
    if (g_n[i].kind == KIND_SHOT) {
        h -= ugfx_char_h();                      // one line of sentence is enough
        if (g_n[i].thumb.px) h += g_n[i].thumb.h + pad();
    }
    return h;
}
static int gap(void)    { return pad(); }

static void card_rect(int i, int *x, int *y, int *w, int *h) {
    *w = card_w(); *h = card_h_of(i);
    *x = screen_w - *w - gap();
    int below = 0;   // the cards under this one, which is newer
    for (int k = 0; k < i; k++) below += card_h_of(k) + gap();
    *y = screen_h - taskbar_h - gap() - below - *h;
}

static const char *shot_label(int b) {
    return b == BTN_A ? "Open" : b == BTN_B ? "Copy" : "Folder";
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
    if (n->kind == KIND_SHOT) {
        // Left to right under the picture: Open (the primary), Copy, Folder.
        int bx = cx + pad();
        for (int k = BTN_A; k <= b; k++) {
            *w = ugfx_text_width(shot_label(k)) + 2 * ugfx_char_w();
            if (k == b) { *x = bx; return 1; }
            bx += *w + pad() / 2;
        }
        return 0;
    }
    if (b == BTN_C) return 0;
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

const char *crash_notice_button(int b, int r[4]) {
    if (!g_count || b < BTN_A || b > BTN_C) return 0;
    if (!button_rect(0, b, &r[0], &r[1], &r[2], &r[3])) return 0;
    if (g_n[0].kind == KIND_SHOT) return shot_label(b);
    return b == BTN_DETAILS ? "Details" : "Reopen";
}

const char *crash_notice_path(void) { return g_count && g_n[0].kind == KIND_SHOT ? g_n[0].path : 0; }
const char *crash_notice_sub(void)  { return g_count ? g_n[0].sub : 0; }

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
    n.thumb.px = 0;   // drop() frees the card's picture; this copy must not
    drop(i);
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
        int isz = ugfx_char_h() * 2;
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
            int shot = n->kind == KIND_SHOT;
            int primary = shot ? b == BTN_A : b == BTN_REOPEN;
            uint32_t bg = primary ? UTHEME_ACCENT : UTHEME_BUTTON_BG;
            const char *label = shot ? shot_label(b) : b == BTN_REOPEN ? "Reopen" : "Details";
            uui_button_draw(s, bx, by, bw, bh, label, bg,
                            primary ? UTHEME_ACCENT_TEXT : UTHEME_TEXT,
                            hot ? UUI_STATE_HOVER : UUI_STATE_REST);
        }
    }
}
