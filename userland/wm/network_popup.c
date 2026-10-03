// See network_popup.h. A tray flyout over QUERY_NETDEV, on the shared
// card (wm_flyout.h); its actions are /bin/netctl's.
#include "lib/unum.h"
#include "wm_internal.h"
#include "network_popup.h"
#include "wm_flyout.h"
#include "wm_tray.h"
#include "wm_overlay.h"
#include "lib/icon_cache.h"
#include "lib/uclip.h"
#include "lib/uconf.h"
#include "ui/uui.h"
#include "ui/utheme.h"
#include "kapi.h"
#include "rt/sys.h"
#include "wm/wm_conf.h"   // wm_setting_generation()

int network_open = 0;

static int g_tray_id = -1;
static const char *g_icon = "tray-network-off";
static struct network_view g_view;
static int g_hover;
static uint32_t g_seen_generation;

// What the card says beyond the device table: the lease netd keeps on
// disk and the DNS server it wrote. Read when the panel opens and when
// the address changes -- small files, never once a second.
static char g_lease[48], g_dns[20];

// TRAFFIC, a sample a second for the last minute -- Task Manager's
// per-second rate (tm_perf.c), kept here because the card draws it.
#define HIST 60
static unsigned long long g_rx_hist[HIST], g_tx_hist[HIST];
static int g_hist_n;
static unsigned long long g_last_rx, g_last_tx, g_last_ns;

// THE SWITCH ANSWERS AT ONCE. netd acts on its next pass, so for a few
// seconds the card shows what was asked rather than flicking back.
static int g_want_down = -1;
// The card the switch last acted on, kept on the panel while it is off
// -- or switching it off would hand the panel to the next card, and it
// could not be switched back on from here.
static char g_pin[NET_ABI_NAME_MAX];
static unsigned long long g_want_until;

#define HOVER_TRAY    1
#define HOVER_SWITCH  2
#define HOVER_COPY    3
#define HOVER_RENEW   4
#define HOVER_DETAILS 5
#define HOVER_GEAR    6

// --- reading ----------------------------------------------------------

// 169.254/16. `udhcp.c` hands one of these out when no server answered,
// so an address in this range means "configured itself", not "on a
// network".
#define IS_LINK_LOCAL(ip) (((ip) & 0xFFFF0000u) == 0xA9FE0000u)

static void fmt_ip(char *out, unsigned cap, unsigned long long ip) {
    if (!ip) { k_strlcpy(out, "none", cap); return; }
    k_snprintf(out, cap, "%llu.%llu.%llu.%llu",
               (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

// Gb/s to one decimal, because 2.5 Gb/s is not 2 -- the split
// /bin/netctl makes, and for the same reason.
static void fmt_speed(char *out, unsigned cap, const struct network_view *v) {
    unsigned long long b = v->link_bps;
    if (!b) { out[0] = 0; return; }
    if (b >= 1000000000ull && b % 1000000000ull) {
        k_snprintf(out, cap, "%llu.%llu Gb/s", b / 1000000000ull, b % 1000000000ull / 100000000ull);
        unum_localize(out, (unsigned long)cap, 0);
    } else if (b >= 1000000000ull) k_snprintf(out, cap, "%llu Gb/s", b / 1000000000ull);
    else k_snprintf(out, cap, "%llu Mb/s", b / 1000000ull);
}

static void fmt_rate(char *out, unsigned cap, unsigned long long bps) {
    if (bps >= 1000000ull) {
        k_snprintf(out, cap, "%llu.%llu MB/s", bps / 1000000ull, bps % 1000000ull / 100000ull);
        unum_localize(out, (unsigned long)cap, 0);
    } else if (bps >= 1000ull) {
        k_snprintf(out, cap, "%llu.%llu KB/s", bps / 1000ull, bps % 1000ull / 100ull);
        unum_localize(out, (unsigned long)cap, 0);
    } else k_snprintf(out, cap, "%llu B/s", bps);
}

static void read_files(void) {
    g_lease[0] = 0;
    if (g_view.have_device) {
        char path[64], secs[16], server[20];
        k_snprintf(path, sizeof path, "/var/dhcp-%s.lease", g_view.name);
        uint32_t s = 0;
        if (uconf_get(path, "seconds", secs, sizeof secs) && k_parse_u32(secs, &s) && s &&
            uconf_get(path, "server", server, sizeof server)) {
            if (s % 3600 == 0) k_snprintf(g_lease, sizeof g_lease, "%u h, from %s", s / 3600, server);
            else k_snprintf(g_lease, sizeof g_lease, "%u s, from %s", s, server);
        }
    }
    if (!uconf_get("/etc/resolv.conf", "nameserver", g_dns, sizeof g_dns)) g_dns[0] = 0;
}

// THE FIRST CARD THAT IS UP WITH AN ADDRESS, else the first device at
// all -- unless the switch put a card down, which stays on the panel
// while it is off. A tray item is one glyph and cannot show two cards;
// picking the one actually carrying traffic is what a person means by
// "am I online". The count is reported so the panel can say there are
// others.
static void read_devices(struct network_view *v, struct query_netdev *out) {
    struct query_netdev d, best, pinned;
    int have_best = 0, have_pin = 0, count = 0;

    k_memset(v, 0, sizeof *v);
    k_memset(&best, 0, sizeof best);
    QUERY_FOREACH(QUERY_NETDEV, d, i) {
        int live = d.ip && !d.admin_down;
        if (count == 0 || (!have_best && live)) { best = d; have_best = live; }
        if (g_pin[0] && !k_strcmp(d.name, g_pin) && d.admin_down) { pinned = d; have_pin = 1; }
        count++;
    }
    if (have_pin) best = pinned;
    else g_pin[0] = 0;   // back up, or gone: the panel follows traffic again
    v->device_count = count;
    if (out) *out = best;
    if (!count) return;

    v->have_device = 1;
    v->link_known = best.link_known != 0;
    v->link_up = best.link_up != 0;
    v->link_bps = best.link_bps;
    v->admin_down = best.admin_down != 0;
    v->link_local = best.ip && IS_LINK_LOCAL((unsigned)best.ip);
    // CONNECTED MEANS AN ADDRESS, never a link flag: link_known is 0 on
    // the e1000 every default QEMU guest has.
    v->connected = best.ip != 0 && !v->link_local;
    k_strlcpy(v->name, best.name, sizeof v->name);
    k_strlcpy(v->driver, best.driver, sizeof v->driver);
    k_strlcpy(v->location, best.location, sizeof v->location);
    fmt_ip(v->ip, sizeof v->ip, best.ip);
    fmt_ip(v->mask, sizeof v->mask, best.netmask);
    fmt_ip(v->gw, sizeof v->gw, best.gateway);
    unsigned long long m = best.netmask;
    for (int b = 0; b < 32; b++) v->prefix += (int)((m >> b) & 1);
    unsigned long long mac = best.mac;
    k_snprintf(v->mac, sizeof v->mac, "%02llx:%02llx:%02llx:%02llx:%02llx:%02llx",
               mac & 0xFF, (mac >> 8) & 0xFF, (mac >> 16) & 0xFF,
               (mac >> 24) & 0xFF, (mac >> 32) & 0xFF, (mac >> 40) & 0xFF);
}

static const char *icon_for(const struct network_view *v) {
    if (!v->have_device || v->admin_down) return "tray-network-off";
    if (v->connected) return "tray-network";
    return "tray-network-limited";
}

static int shown_down(void) {
    if (g_want_down >= 0 && sys_monotonic_ns() < g_want_until) return g_want_down;
    return g_view.admin_down;
}

// TWO CLOCKS, because the two questions have different answers. THE
// DEVICE is read on a once-a-second CADENCE: there is no netdev
// generation in the ABI, and the read is a memcpy out of a live kernel
// table with no I/O behind it. VISIBILITY is on the SETTINGS GENERATION,
// because tray_want_shown() reads /etc/desktop.conf.
void network_poll(void) {
    struct network_view now;
    struct query_netdev d;
    read_devices(&now, &d);

    // The traffic sample, a rate over the measured interval.
    unsigned long long t = sys_monotonic_ns();
    if (g_last_ns && now.have_device && !k_strcmp(now.name, g_view.name) &&
        d.rx_bytes >= g_last_rx && d.tx_bytes >= g_last_tx) {
        unsigned long long dt = t - g_last_ns;
        if (dt) {
            now.rx_rate = (d.rx_bytes - g_last_rx) * 1000000000ull / dt;
            now.tx_rate = (d.tx_bytes - g_last_tx) * 1000000000ull / dt;
            for (int i = 0; i < HIST - 1; i++) {
                g_rx_hist[i] = g_rx_hist[i + 1];
                g_tx_hist[i] = g_tx_hist[i + 1];
            }
            g_rx_hist[HIST - 1] = now.rx_rate;
            g_tx_hist[HIST - 1] = now.tx_rate;
            if (g_hist_n < HIST) g_hist_n++;
        }
    }
    g_last_rx = d.rx_bytes; g_last_tx = d.tx_bytes; g_last_ns = t;

    int changed = now.have_device != g_view.have_device ||
                  now.connected != g_view.connected ||
                  now.link_local != g_view.link_local ||
                  now.link_up != g_view.link_up ||
                  now.link_known != g_view.link_known ||
                  now.admin_down != g_view.admin_down ||
                  now.device_count != g_view.device_count ||
                  k_strcmp(now.ip, g_view.ip) != 0 ||
                  k_strcmp(now.name, g_view.name) != 0;
    g_view = now;
    if (g_want_down >= 0 && (g_view.admin_down == g_want_down || t >= g_want_until))
        g_want_down = -1;

    uint32_t gen = wm_setting_generation();
    if (gen != g_seen_generation || changed) {
        g_seen_generation = gen;
        tray_set_hidden(g_tray_id, !tray_want_shown("network", g_view.have_device));
    }
    if (changed) {
        if (network_open) read_files();
        g_icon = icon_for(&g_view);
        tray_set_icon(g_tray_id, g_icon);
    }
    // The graph moves every second while the card is up.
    if (network_open) network_damage();
}

void network_view(struct network_view *v) { *v = g_view; }
int network_tray_hidden(void) { return tray_is_hidden(g_tray_id); }

void network_tray_init(void) {
    read_devices(&g_view, 0);
    g_icon = icon_for(&g_view);
    g_tray_id = tray_register_icon(g_icon);
    tray_set_hidden(g_tray_id, !tray_want_shown("network", g_view.have_device));
}

// --- geometry ---------------------------------------------------------

// The DETAILS rows. Built here rather than in draw() so that drawing,
// the panel's height and `gui network --json` cannot disagree about
// what is on it -- the one-geometry-function rule every flyout follows.
#define NET_ROWS 6

static int build_rows(char keys[NET_ROWS][16], char vals[NET_ROWS][48]) {
    int n = 0;
    k_strlcpy(keys[n], "IPv4 address", 16);
    if (g_view.ip[0] && k_strcmp(g_view.ip, "none"))
        k_snprintf(vals[n], 48, "%s/%d%s", g_view.ip, g_view.prefix,
                   g_view.link_local ? " (self-assigned)" : "");
    else k_strlcpy(vals[n], "none", 48);
    n++;
    if (k_strcmp(g_view.gw, "none")) {
        k_strlcpy(keys[n], "Gateway", 16); k_strlcpy(vals[n], g_view.gw, 48); n++;
    }
    if (g_dns[0]) { k_strlcpy(keys[n], "DNS", 16); k_strlcpy(vals[n], g_dns, 48); n++; }
    if (g_lease[0]) { k_strlcpy(keys[n], "Lease", 16); k_strlcpy(vals[n], g_lease, 48); n++; }
    k_strlcpy(keys[n], "Adapter", 16);
    k_snprintf(vals[n], 48, "%s%s%s", g_view.driver, g_view.location[0] ? " at " : "",
               g_view.location);
    n++;
    k_strlcpy(keys[n], "MAC", 16); k_strlcpy(vals[n], g_view.mac, 48); n++;
    return n;
}

void network_geometry(struct network_geom *g) {
    k_memset(g, 0, sizeof *g);
    struct wm_flyout_metrics m;
    wm_flyout_metrics(&m);
    int ch = ugfx_char_h();

    char keys[NET_ROWS][16], vals[NET_ROWS][48];
    int rows = g_view.have_device ? build_rows(keys, vals) : 0;
    // AT LEAST AS WIDE AS THE FOOTER'S BUTTONS, which at a small font
    // are wider than the details: a fixed width overlapped them.
    int w = ch * 21;
    int foot_w = 2 * (m.pad - 6) + wm_flyout_button_w("Copy address", "tb-copy") + 6 +
                 wm_flyout_button_w("Renew", "tb-refresh") + 12 +
                 wm_flyout_button_w("Details...", 0) + 4 + m.btn_h;
    if (w < foot_w) w = foot_w;
    int key_w = ugfx_text_width("IPv4 address") + m.pad;
    int h = m.hero_h + 1;
    int graph = g_view.have_device && !shown_down();
    if (!g_view.have_device) h += m.vpad + 2 * (ch + 4) + m.vpad;
    if (graph) h += m.vpad + m.cap_h + ch * 3 + 4 + ch + 4 + m.vpad + 1;
    if (rows) h += m.vpad + m.cap_h + rows * m.row_h + m.vpad;
    h += m.foot_h;

    int tx = 0, ty = 0, tw = 0, th = 0;
    if (!tray_item_rect(g_tray_id, &tx, &ty, &tw, &th)) {
        tx = screen_w - 32; ty = screen_h - taskbar_h; tw = 16; th = taskbar_h;
    }
    int x, y;
    wm_popup_place(tx + tw - w, screen_h - taskbar_h - h, w, h, &x, &y);

    g->x = x; g->y = y; g->w = w; g->h = h;
    g->tray_x = tx; g->tray_y = ty; g->tray_w = tw; g->tray_h = th;
    g->pad = m.pad; g->row_h = m.row_h;
    g->hero_h = m.hero_h;
    if (g_view.have_device) {
        g->sw_w = m.sw_w; g->sw_h = m.sw_h;
        g->sw_x = x + w - m.pad - m.sw_w;
        g->sw_y = y + (m.hero_h - m.sw_h) / 2;
    }

    int cy = y + m.hero_h + 1;
    if (!g_view.have_device) { g->note_y = cy + m.vpad; cy += m.vpad + 2 * (ch + 4) + m.vpad; }
    if (graph) {
        g->cap1_y = cy + m.vpad;
        g->graph_x = x + m.pad;
        g->graph_y = g->cap1_y + m.cap_h;
        g->graph_w = w - 2 * m.pad;
        g->graph_h = ch * 3;
        g->legend_y = g->graph_y + g->graph_h + 4;
        cy = g->legend_y + ch + 4 + m.vpad;
        g->rule2_y = cy;
        cy += 1;
    }
    if (rows) {
        g->cap2_y = cy + m.vpad;
        g->kv_y = g->cap2_y + m.cap_h;
        g->kv_rows = rows;
        g->kv_key_w = key_w;
    }

    g->foot_h = m.foot_h;
    g->foot_y = y + h - m.foot_h;
    g->btn_h = m.btn_h;
    g->btn_y = g->foot_y + (m.foot_h - m.btn_h) / 2;
    int bx = x + m.pad - 6;
    if (g_view.have_device) {
        g->copy_x = bx; g->copy_w = wm_flyout_button_w("Copy address", "tb-copy");
        bx += g->copy_w + 6;
        g->renew_x = bx; g->renew_w = wm_flyout_button_w("Renew", "tb-refresh");
    }
    g->gear_w = m.btn_h;
    g->gear_x = x + w - (m.pad - 6) - g->gear_w;
    if (g_view.have_device) {
        g->details_w = wm_flyout_button_w("Details...", 0);
        g->details_x = g->gear_x - 4 - g->details_w;
    }
}

// --- state ------------------------------------------------------------

int network_rect(int *x, int *y, int *w, int *h) {
    struct network_geom g;
    network_geometry(&g);
    *x = g.x; *y = g.y; *w = g.w; *h = g.h;
    return 1;
}

// The rules -- the shadow, and the rect it last occupied -- are
// the core's (wm_overlay.h).
void network_damage(void) { wm_overlay_damage("network"); }

static void network_open_now(void) {
    wm_overlay_close_others("network");
    network_poll();          // never show a stale address on opening
    read_files();
    network_open = 1;
    g_hover = 0;
    network_damage();
}

void network_close(void) {
    if (!network_open) return;
    // Damaged BEFORE the flag drops, or the rect is computed for a
    // panel the frame is no longer drawing.
    network_damage();
    network_open = 0;
    g_hover = 0;
}

// --- input ------------------------------------------------------------

static int hit(int x, int w, const struct network_geom *g, int mx, int my) {
    return w > 0 && uui_hit(x, g->btn_y, w, g->btn_h, mx, my);
}

// netctl, as a child, never waited on: `args` are its command line.
static void run_netctl(const char *verb) {
    char args[64];
    k_snprintf(args, sizeof args, "%s %s --no-wait", verb, g_view.name);
    { int pid_ = sys_spawn("/bin/netctl", args, -1); if (pid_ > 0) wm_track_launched(pid_); }   // reaped by the poll
}

int network_handle_click(int mx, int my) {
    struct network_geom g;
    network_geometry(&g);

    if (!network_open) {
        if (tray_is_hidden(g_tray_id)) return 0;
        if (uui_hit(g.tray_x, g.tray_y, g.tray_w, g.tray_h, mx, my)) {
            network_open_now();
            return 1;
        }
        return 0;
    }
    if (uui_hit(g.tray_x, g.tray_y, g.tray_w, g.tray_h, mx, my)) {
        network_close();     // a second click on the icon closes it
        return 1;
    }
    if (!uui_hit(g.x, g.y, g.w, g.h, mx, my)) {
        network_close();
        // A click on the TASKBAR still belongs to the taskbar.
        return my < screen_h - taskbar_h;
    }
    if (g.sw_w && uui_hit(g.sw_x - 4, g.sw_y - 4, g.sw_w + 8, g.sw_h + 8, mx, my)) {
        // ONE REQUEST IN FLIGHT: each runs in its own netctl, and netd
        // takes its rings in no particular order, so a quick second
        // click could be applied first.
        if (g_want_down >= 0 && sys_monotonic_ns() < g_want_until) return 1;
        int down = !shown_down();
        if (down) k_strlcpy(g_pin, g_view.name, sizeof g_pin);
        run_netctl(down ? "down" : "up");
        g_want_down = down;
        g_want_until = sys_monotonic_ns() + 8000000000ull;
        network_damage();
    } else if (hit(g.copy_x, g.copy_w, &g, mx, my) && g_view.connected && !shown_down()) {
        uclip_set_text(g_view.ip, (int)k_strlen(g_view.ip));
    } else if (hit(g.renew_x, g.renew_w, &g, mx, my) && !shown_down()) {
        run_netctl("renew");
    } else if (hit(g.details_x, g.details_w, &g, mx, my)) {
        network_close();
        { int pid_ = sys_spawn("/bin/wm/system/taskmgr", "", -1); if (pid_ > 0) wm_track_launched(pid_); }   // reaped by the poll
    } else if (hit(g.gear_x, g.gear_w, &g, mx, my)) {
        network_close();
        { int pid_ = sys_spawn("/bin/wm/system/settings", "system.net_recover", -1); if (pid_ > 0) wm_track_launched(pid_); }   // reaped by the poll
    }
    return 1;   // anything else inside the card is swallowed
}

int network_hover_at(int mx, int my) {
    struct network_geom g;
    network_geometry(&g);
    if (tray_is_hidden(g_tray_id)) return 0;
    g_hover = 0;
    if (uui_hit(g.tray_x, g.tray_y, g.tray_w, g.tray_h, mx, my)) g_hover = HOVER_TRAY;
    else if (!network_open) g_hover = 0;
    else if (g.sw_w && uui_hit(g.sw_x - 4, g.sw_y - 4, g.sw_w + 8, g.sw_h + 8, mx, my))
        g_hover = HOVER_SWITCH;
    else if (hit(g.copy_x, g.copy_w, &g, mx, my)) g_hover = HOVER_COPY;
    else if (hit(g.renew_x, g.renew_w, &g, mx, my)) g_hover = HOVER_RENEW;
    else if (hit(g.details_x, g.details_w, &g, mx, my)) g_hover = HOVER_DETAILS;
    else if (hit(g.gear_x, g.gear_w, &g, mx, my)) g_hover = HOVER_GEAR;
    return g_hover;
}

// --- drawing ----------------------------------------------------------

static void draw_graph(const struct network_geom *g) {
    struct ugfx_surface *s = wm_surface();
    uui_fill_round_rect(s, g->graph_x, g->graph_y, g->graph_w, g->graph_h, 4, UTHEME_OUTLINE);
    uui_fill_round_rect(s, g->graph_x + 1, g->graph_y + 1, g->graph_w - 2, g->graph_h - 2, 3,
                        UTHEME_WHITE);
    unsigned long long top = 1000;   // a quiet card still has a scale: 1 KB/s
    for (int i = 0; i < HIST; i++) {
        if (g_rx_hist[i] > top) top = g_rx_hist[i];
        if (g_tx_hist[i] > top) top = g_tx_hist[i];
    }
    int xs[HIST], ys_rx[HIST], ys_tx[HIST];
    int ih = g->graph_h - 6;
    for (int i = 0; i < HIST; i++) {
        xs[i] = g->graph_x + 2 + i * (g->graph_w - 5) / (HIST - 1);
        ys_rx[i] = g->graph_y + 3 + ih - (int)(g_rx_hist[i] * (unsigned long long)ih / top);
        ys_tx[i] = g->graph_y + 3 + ih - (int)(g_tx_hist[i] * (unsigned long long)ih / top);
    }
    ugfx_draw_polyline(s, xs, ys_tx, HIST, 0, ugfx_rgb(176, 112, 58), GEOM_AA);
    ugfx_draw_polyline(s, xs, ys_rx, HIST, 0, UTHEME_ACCENT, GEOM_AA);

    // The legend: a swatch and the last second's rate for each.
    char rate[24], line[48];
    int ch = ugfx_char_h(), half = g->graph_w / 2;
    fmt_rate(rate, sizeof rate, g_view.rx_rate);
    k_snprintf(line, sizeof line, "Receiving %s", rate);
    uui_fill_round_rect(s, g->graph_x, g->legend_y + ch / 2 - 3, 10, 6, 2, UTHEME_ACCENT);
    ugfx_draw_string_clipped(s, g->graph_x + 14, g->legend_y, half - 16, line,
                             wm_flyout_dim(), wm_flyout_ink());
    fmt_rate(rate, sizeof rate, g_view.tx_rate);
    k_snprintf(line, sizeof line, "Sending %s", rate);
    uui_fill_round_rect(s, g->graph_x + half, g->legend_y + ch / 2 - 3, 10, 6, 2,
                        ugfx_rgb(176, 112, 58));
    ugfx_draw_string_clipped(s, g->graph_x + half + 14, g->legend_y, half - 16, line,
                             wm_flyout_dim(), wm_flyout_ink());
}

void network_draw(int mx, int my) {
    (void)mx; (void)my;
    if (!network_open) return;
    struct network_geom g;
    network_geometry(&g);
    struct ugfx_surface *s = wm_surface();
    int ch = ugfx_char_h();
    int down = shown_down();

    wm_flyout_card(g.x, g.y, g.w, g.h, g.foot_h);

    // THE HEADER: the state in a word, then how.
    char title[32], sub[64], speed[24];
    uint32_t badge = UTHEME_ACCENT;
    fmt_speed(speed, sizeof speed, &g_view);
    if (!g_view.have_device) {
        k_strlcpy(title, "No network device", sizeof title);
        k_strlcpy(sub, "Nothing to connect with", sizeof sub);
        badge = ugfx_rgb(140, 140, 150);
    } else if (down) {
        k_strlcpy(title, "Switched off", sizeof title);
        k_snprintf(sub, sizeof sub, "Wired, %s", g_view.name);
        badge = ugfx_rgb(140, 140, 150);
    } else if (g_view.connected) {
        k_strlcpy(title, "Connected", sizeof title);
        k_snprintf(sub, sizeof sub, "Wired, %s%s%s", g_view.name, speed[0] ? ", " : "", speed);
    } else {
        k_strlcpy(title, "No address", sizeof title);
        k_snprintf(sub, sizeof sub, "%s, %s%s%s",
                   g_view.link_known && !g_view.link_up ? "Cable out" : "Waiting for one",
                   g_view.name, speed[0] ? ", " : "", speed);
        badge = ugfx_rgb(200, 144, 42);
    }
    wm_flyout_hero(g.x, g.y, g.w - (g.sw_w ? g.sw_w + g.pad + 8 : g.pad), badge,
                   icon_for(&g_view) == g_icon ? g_icon : icon_for(&g_view), title, sub);
    if (g.sw_w) wm_flyout_switch(g.sw_x, g.sw_y, !down, g_hover == HOVER_SWITCH);
    wm_flyout_rule(g.x, g.y + g.hero_h, g.w);

    if (!g_view.have_device) {
        ugfx_draw_string_clipped(s, g.x + g.pad, g.note_y, g.w - 2 * g.pad,
                                 "No wired adapter was found. A USB Ethernet",
                                 wm_flyout_dim(), wm_flyout_ink());
        ugfx_draw_string_clipped(s, g.x + g.pad, g.note_y + ch + 4, g.w - 2 * g.pad,
                                 "adapter works when plugged in.", wm_flyout_dim(), wm_flyout_ink());
    }
    if (g.graph_h) {
        wm_flyout_caption(g.graph_x, g.cap1_y, g.graph_w, "TRAFFIC");
        draw_graph(&g);
        wm_flyout_rule(g.x, g.rule2_y, g.w);
    }
    if (g.kv_rows) {
        char keys[NET_ROWS][16], vals[NET_ROWS][48];
        build_rows(keys, vals);
        wm_flyout_caption(g.x + g.pad, g.cap2_y, g.w - 2 * g.pad, "DETAILS");
        for (int i = 0; i < g.kv_rows; i++)
            wm_flyout_kv(g.x + g.pad, g.kv_y + i * g.row_h, g.w - 2 * g.pad, g.kv_key_w,
                         keys[i], vals[i]);
    }

    if (g.copy_w) wm_flyout_button(g.copy_x, g.btn_y, "Copy address", "tb-copy", 1,
                                   g_hover == HOVER_COPY, !g_view.connected || down);
    if (g.renew_w) wm_flyout_button(g.renew_x, g.btn_y, "Renew", "tb-refresh", 1,
                                    g_hover == HOVER_RENEW, down);
    if (g.details_w) wm_flyout_button(g.details_x, g.btn_y, "Details...", 0, 0,
                                      g_hover == HOVER_DETAILS, 0);
    wm_flyout_icon_button(g.gear_x, g.btn_y, g.gear_w, "tb-gear", g_hover == HOVER_GEAR);
}
