// See remote_popup.h. A read-only tray flyout over QUERY_REMOTELOG.
#include "wm_internal.h"
#include "remote_popup.h"
#include "wm_shadow.h"
#include "wm_tray.h"
#include "wm_overlay.h"
#include "ui/uui.h"
#include "ui/utheme.h"
#include "kapi.h"
#include "rt/sys.h"
#include "wm/wm_conf.h"   // wm_setting_generation()
#include "ui/uui_actionlist.h"
#include "lib/uremote.h"   // the live sessions, and the two things done to one
#include "ui/ulog.h"

int remote_open = 0;

static int g_tray_id = -1;
static int g_hover;
static uint32_t g_seen_generation;

// The sessions open now, as action rows: the person at the screen can
// end one, and make a remote desktop view only.
#define LIVE_MAX 8
static struct query_remotesess g_live[LIVE_MAX];
static int g_nlive, g_desktop;
static struct uui_actionlist_row g_live_rows[LIVE_MAX];
static struct uui_actionlist g_list;

// What the last poll saw. Kept rather than re-read per draw so the
// panel, its width and `gui remote --json` cannot disagree -- the
// one-geometry rule every flyout here follows.
static struct query_remotelog g_rows[REMOTE_ROWS];
static int g_row_count;
static uint64_t g_last_seq;      // the newest record this has drawn
static uint32_t g_peer;
static int g_sessions;
// What the visibility gate last acted on -- written only there.
static int g_last_present;

// --- reading ----------------------------------------------------------

static void fmt_ip(char *out, unsigned cap, unsigned long long ip) {
    if (!ip) { k_strlcpy(out, "none", cap); return; }
    k_snprintf(out, cap, "%llu.%llu.%llu.%llu",
               (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

// HOW MANY SESSIONS ARE OPEN COMES OFF THE NEWEST RECORD, which is the
// kernel's own live count at the moment it was written (query_abi.h).
// Replaying `opened` minus `closed` across the ring was the first
// version and it reported zero as soon as the ring wrapped past an
// `opened` whose `closed` it still held -- the indicator then stayed up
// with nobody connected, which is the one reading this item must never
// produce.
static void read_records(void) {
    struct query_remotelog r;
    int total = 0;
    for (int i = 0; ; i++) {
        if (sys_query_record(QUERY_REMOTELOG, i, &r, sizeof r) < (int)sizeof r) break;
        total = i + 1;
    }

    int first = total > REMOTE_ROWS ? total - REMOTE_ROWS : 0;
    g_row_count = 0;
    for (int i = first; i < total && g_row_count < REMOTE_ROWS; i++) {
        if (sys_query_record(QUERY_REMOTELOG, i, &r, sizeof r) < (int)sizeof r) break;
        g_rows[g_row_count++] = r;
    }

    g_sessions = 0;
    if (g_row_count) {
        const struct query_remotelog *newest = &g_rows[g_row_count - 1];
        g_sessions = (int)newest->sessions;
        // The peer is remembered past the close, so the flyout can still
        // say who the listed records came from.
        if (newest->remote_ip) g_peer = (uint32_t)newest->remote_ip;
    }
    g_last_seq = g_row_count ? g_rows[g_row_count - 1].seq : 0;

    g_nlive = uremote_sessions(g_live, LIVE_MAX);
    g_desktop = 0;
    for (int i = 0; i < g_nlive; i++) {
        struct uui_actionlist_row *r = &g_live_rows[i];
        k_memset(r, 0, sizeof *r);
        char ip[20], what[QUERY_REMOTESESS_STATUS_MAX];
        fmt_ip(ip, sizeof ip, g_live[i].remote_ip);
        uremote_describe(&g_live[i], what, sizeof what);
        int desk = uremote_is_desktop(&g_live[i]);
        g_desktop |= desk;
        k_snprintf(r->title, sizeof r->title, "%s", ip);
        k_snprintf(r->sub, sizeof r->sub, "%s", what);
        if (desk) r->btn[0] = k_strstr(what, "view only") ? "Full control" : "View only";
        r->btn[1] = "Disconnect";
        r->primary = 1;
    }
    if (!g_list.rows) uui_actionlist_init(&g_list, g_live_rows, 0, 0);
    g_list.count = g_nlive;
}

void remote_poll(void) {
    uint64_t before = g_last_seq;
    read_records();

    // **THE GATE TRACKS WHAT IT LAST APPLIED, NOT WHAT THE LAST POLL
    // SAW.** Comparing against the session count at poll entry looks
    // equivalent and is not: `read_records()` also runs when the flyout
    // is OPENED, so a session that ended between two polls was already
    // folded into the count by that click and the next poll saw no
    // change -- leaving the indicator up with nobody connected, which
    // is the one reading this item must never produce.
    // The LIVE list is the present: the records' count is what the
    // newest record said, which is the same answer when nothing is lost.
    int present = g_nlive > 0 || g_sessions > 0;
    tray_set_accent(g_tray_id, g_desktop);
    uint32_t gen = wm_setting_generation();
    if (gen != g_seen_generation || present != g_last_present) {
        g_last_present = present;
        // tray_want_shown() reads /etc/desktop.conf, so it is asked on a
        // CHANGE rather than once a second forever (network_popup.c has
        // the measurement).
        g_seen_generation = gen;
        tray_set_hidden(g_tray_id, !tray_want_shown("remote", present));
    }
    static int live_was = -1;
    if ((g_last_seq != before || g_nlive != live_was) && remote_open) remote_damage();
    live_was = g_nlive;
}

void remote_tray_init(void) {
    read_records();
    g_tray_id = tray_register_icon("tray-remote");
    g_last_present = g_sessions > 0;
    tray_set_hidden(g_tray_id, !tray_want_shown("remote", g_last_present));
}

int remote_tray_hidden(void) { return tray_is_hidden(g_tray_id); }
int remote_session_count(void) { return g_sessions; }
void remote_peer(char *out, unsigned cap) { fmt_ip(out, cap, g_peer); }
int remote_row_count(void) { return g_row_count; }
int remote_live_count(void) { return g_nlive; }
int remote_desktop_live(void) { return g_desktop; }

// --- rows -------------------------------------------------------------

// The four kinds, as a word that fits beside the text. A word rather
// than a colour, for the reason the network item carries state by
// shape: a tray flyout is drawn in the panel's ink.
static const char *kind_word(uint64_t kind) {
    switch (kind) {
    // A session row's own text already says "session opened" -- a word
    // in front of it read "session session opened".
    case QUERY_REMOTE_SESSION: return "";
    case QUERY_REMOTE_COMMAND: return "$";
    case QUERY_REMOTE_SPAWN:   return "run";
    case QUERY_REMOTE_XFER:    return "file";
    }
    return "?";
}

void remote_row_text(int i, char *out, unsigned cap) {
    if (i < 0 || i >= g_row_count) { k_strlcpy(out, "", cap); return; }
    const char *kw = kind_word(g_rows[i].kind);
    if (kw[0]) k_snprintf(out, cap, "%s %s", kw, g_rows[i].text);
    else       k_strlcpy(out, g_rows[i].text, cap);
}

// --- geometry ---------------------------------------------------------

// With sessions open the records make room for them: the newest few.
#define REMOTE_ROWS_WITH_LIVE 5

void remote_geometry(struct remote_geom *g) {
    k_memset(g, 0, sizeof *g);

    int ch = ugfx_char_h();
    int pad = ch / 2 + 2;
    int row_h = ch + 2;

    int log_rows = g_row_count;
    if (g_nlive && log_rows > REMOTE_ROWS_WITH_LIVE) log_rows = REMOTE_ROWS_WITH_LIVE;
    // The header, the live sessions, then a row per record (or a line
    // when there are none -- an empty panel that says nothing reads as
    // broken), then the link to the settings.
    char buf[QUERY_REMOTELOG_TEXT_MAX + 16];
    int text_w = 0;
    for (int i = g_row_count - log_rows; i < g_row_count; i++) {
        remote_row_text(i, buf, sizeof buf);
        int tw = ugfx_text_width(buf);
        if (tw > text_w) text_w = tw;
    }
    int w = pad + text_w + pad;
    int min_w = ugfx_char_w() * (g_nlive ? 44 : 30) + pad * 2;
    if (w < min_w) w = min_w;
    int max_w = screen_w - pad * 2;
    if (w > max_w) w = max_w;
    int lw, lh = 0;
    if (g_nlive) uui_actionlist_ops.natural_size(&g_list, &lw, &lh);
    int h = pad + row_h + pad / 2 + lh + (g_nlive ? pad / 2 : 0) +
            (log_rows ? log_rows : 1) * row_h + pad / 2 + row_h + pad;

    int tx = 0, ty = 0, tw = 0, th = 0;
    if (!tray_item_rect(g_tray_id, &tx, &ty, &tw, &th)) {
        tx = screen_w - 32; ty = screen_h - taskbar_h; tw = 16; th = taskbar_h;
    }
    int x, y;
    wm_popup_place(tx + tw - w, screen_h - taskbar_h - h, w, h, &x, &y);

    g->x = x; g->y = y; g->w = w; g->h = h;
    g->tray_x = tx; g->tray_y = ty; g->tray_w = tw; g->tray_h = th;
    g->pad = pad; g->row_h = row_h;
    g->list_y = y + pad + row_h + pad / 2;
    g->list_h = lh;
    g->log_y = g->list_y + lh + (g_nlive ? pad / 2 : 0);
    g->log_rows = log_rows;
    g->link_y = g->log_y + (log_rows ? log_rows : 1) * row_h + pad / 2;
    // The list draws itself at its geometry: it is placed where the
    // flyout puts it, inset by the padding the rows already carry.
    uui_actionlist_ops.set_geometry(&g_list, x, g->list_y, w, lh);
}

int remote_live_button(int row, int btn, int r[4]) {
    struct remote_geom g;
    remote_geometry(&g);
    return uui_actionlist_button_rect(&g_list, row, btn, &r[0], &r[1], &r[2], &r[3]);
}

// --- state ------------------------------------------------------------

int remote_rect(int *x, int *y, int *w, int *h) {
    struct remote_geom g;
    remote_geometry(&g);
    *x = g.x; *y = g.y; *w = g.w; *h = g.h;
    return 1;
}

// The rules -- the shadow, and the rect it last occupied -- are
// the core's now (wm_overlay.h).
void remote_damage(void) { wm_overlay_damage("remote"); }

static void remote_open_now(void) {
    wm_overlay_close_others("remote");
    read_records();          // never open on a stale list
    remote_open = 1;
    g_hover = 0;
    remote_damage();
}

void remote_close(void) {
    if (!remote_open) return;
    remote_open = 0;   // where it was drawn is damaged by the core (wm_overlay.h)
    g_hover = 0;
}

// --- input ------------------------------------------------------------

int remote_handle_click(int mx, int my) {
    struct remote_geom g;
    remote_geometry(&g);

    if (!remote_open) {
        if (tray_is_hidden(g_tray_id)) return 0;
        if (uui_hit(g.tray_x, g.tray_y, g.tray_w, g.tray_h, mx, my)) {
            remote_open_now();
            return 1;
        }
        return 0;
    }
    if (uui_hit(g.tray_x, g.tray_y, g.tray_w, g.tray_h, mx, my)) {
        remote_close();      // a second click on the icon closes it
        return 1;
    }
    if (uui_hit(g.x, g.y, g.w, g.h, mx, my)) {
        // A ROW'S BUTTON ACTS ON THE PRESS, as every overlay here does --
        // the compositor hands an overlay one click, not a press and a
        // release.
        if (uui_actionlist_press(&g_list, mx, my) && uui_actionlist_release(&g_list, mx, my) == 2) {
            int r = g_list.fired_row, b = g_list.fired_btn;
            g_list.fired_row = -1;
            if (r >= 0 && r < g_nlive) {
                int pid = (int)g_live[r].pid;
                if (b == 1) uremote_disconnect(pid);
                else uremote_set_view_only(pid, !k_strstr(g_live_rows[r].sub, "view only"));
                ulogf("wm: remote session %d: %s\n", pid, b == 1 ? "disconnect" : "view only toggled");
            }
            read_records();
            remote_damage();
            return 1;
        }
        if (my >= g.link_y && my < g.link_y + g.row_h) {
            // System Settings, opened on this page.
            remote_close();
            int pid = sys_spawn("/bin/wm/system/settings", "remotedesktop", -1);
            if (pid > 0) wm_track_launched(pid);
            return 1;
        }
        return 1;   // the rest is ours, and inert
    }
    remote_close();
    // A click on the TASKBAR is still the taskbar's -- dismissing must
    // not swallow the window button underneath.
    return my < screen_h - taskbar_h;
}

int remote_hover_at(int mx, int my) {
    struct remote_geom g;
    remote_geometry(&g);
    if (tray_is_hidden(g_tray_id)) return 0;
    g_hover = uui_hit(g.tray_x, g.tray_y, g.tray_w, g.tray_h, mx, my);
    if (remote_open && uui_actionlist_hover(&g_list, mx, my)) remote_damage();
    return g_hover;
}

// --- drawing ----------------------------------------------------------

void remote_draw(int mx, int my) {
    (void)mx; (void)my;
    struct remote_geom g;
    remote_geometry(&g);

    uint32_t bg = UTHEME_PANEL_BG, border = UTHEME_BORDER, fg = UTHEME_TEXT;
    wm_shadow_draw(g.x, g.y, g.w, g.h, 0, WM_SHADOW_POPUP);
    ugfx_fill_rect(wm_surface(), g.x, g.y, g.w, g.h, bg);

    char head[64], ip[20];
    fmt_ip(ip, sizeof ip, g_nlive ? (uint32_t)g_live[0].remote_ip : g_peer);
    if (g_nlive > 1)      k_snprintf(head, sizeof head, "%d remote sessions", g_nlive);
    else if (g_nlive)     k_snprintf(head, sizeof head, "Remote session from %s", ip);
    else if (g_sessions)  k_snprintf(head, sizeof head, "Remote session from %s", ip);
    else                  k_strlcpy(head, "No remote session", sizeof head);
    ugfx_draw_string_clipped(wm_surface(), g.x + g.pad, g.y + g.pad,
                             g.w - g.pad * 2, head, fg, bg);
    ugfx_fill_rect(wm_surface(), g.x + g.pad, g.list_y - g.pad / 4, g.w - g.pad * 2, 1, border);
    if (g_nlive) {
        uui_actionlist_draw(wm_surface(), &g_list, bg);
        ugfx_fill_rect(wm_surface(), g.x + g.pad, g.log_y - g.pad / 4, g.w - g.pad * 2, 1, border);
    }

    if (!g_row_count) {
        ugfx_draw_string_clipped(wm_surface(), g.x + g.pad, g.log_y,
                                 g.w - g.pad * 2, "nothing recorded", border, bg);
    }
    char buf[QUERY_REMOTELOG_TEXT_MAX + 16];
    for (int k = 0; k < g.log_rows; k++) {
        remote_row_text(g_row_count - g.log_rows + k, buf, sizeof buf);
        ugfx_draw_string_clipped(wm_surface(), g.x + g.pad, g.log_y + k * g.row_h,
                                 g.w - g.pad * 2, buf, fg, bg);
    }
    ugfx_fill_rect(wm_surface(), g.x + g.pad, g.link_y - g.pad / 4, g.w - g.pad * 2, 1, border);
    ugfx_draw_string_clipped(wm_surface(), g.x + g.pad, g.link_y + 1, g.w - g.pad * 2,
                             "Remote Desktop settings", UTHEME_ACCENT, bg);
    ugfx_draw_rect(wm_surface(), g.x, g.y, g.w, g.h, border);
}
