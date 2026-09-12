// See network_popup.h. A read-only tray flyout over QUERY_NETDEV.
#include "wm_internal.h"
#include "network_popup.h"
#include "wm_tray.h"
#include "wm_overlay.h"
#include "lib/icon_cache.h"
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

// --- reading ----------------------------------------------------------

// 169.254/16. `udhcp.c` hands one of these out when no server answered,
// so an address in this range means "configured itself", not "on a
// network". Inferred from the PREFIX because netd publishes no state --
// see the header.
#define IS_LINK_LOCAL(ip) (((ip) & 0xFFFF0000u) == 0xA9FE0000u)

static void fmt_ip(char *out, unsigned cap, unsigned long long ip) {
    if (!ip) { k_strlcpy(out, "none", cap); return; }
    k_snprintf(out, cap, "%llu.%llu.%llu.%llu",
               (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

// Gb/s to one decimal, because 2.5 Gb/s is not 2 -- the same split
// /bin/ifconfig makes, and for the same reason.
static void fmt_speed(char *out, unsigned cap, const struct network_view *v) {
    if (!v->link_known) { k_strlcpy(out, "unknown", cap); return; }
    if (!v->link_up)    { k_strlcpy(out, "down", cap); return; }
    unsigned long long b = v->link_bps;
    if (!b) k_strlcpy(out, "up", cap);
    else if (b >= 1000000000ull && b % 1000000000ull)
        k_snprintf(out, cap, "up, %llu.%llu Gb/s", b / 1000000000ull,
                   b % 1000000000ull / 100000000ull);
    else if (b >= 1000000000ull) k_snprintf(out, cap, "up, %llu Gb/s", b / 1000000000ull);
    else k_snprintf(out, cap, "up, %llu Mb/s", b / 1000000ull);
}

// THE FIRST DEVICE WITH AN ADDRESS, else the first device at all. A
// tray item is one glyph and cannot show two cards; picking the one
// that is actually carrying traffic is what a person means by "am I
// online". The count is reported so the panel can say there are others.
static void read_devices(struct network_view *v) {
    struct query_netdev d, best;
    int have_best = 0, count = 0;

    k_memset(v, 0, sizeof *v);
    QUERY_FOREACH(QUERY_NETDEV, d, i) {
        if (count == 0 || (!have_best && d.ip) ||
            (d.ip && !best.ip)) { best = d; have_best = d.ip != 0; }
        count++;
    }
    v->device_count = count;
    if (!count) return;

    v->have_device = 1;
    v->link_known = best.link_known != 0;
    v->link_up = best.link_up != 0;
    v->link_bps = best.link_bps;
    v->link_local = best.ip && IS_LINK_LOCAL((unsigned)best.ip);
    // CONNECTED MEANS AN ADDRESS, never a link flag: link_known is 0 on
    // the e1000 every default QEMU guest has, and calling that
    // "disconnected" would be wrong on the one machine this is most
    // often looked at.
    v->connected = best.ip != 0 && !v->link_local;
    k_strlcpy(v->name, best.name, sizeof v->name);
    k_strlcpy(v->driver, best.driver, sizeof v->driver);
    k_strlcpy(v->location, best.location, sizeof v->location);
    fmt_ip(v->ip, sizeof v->ip, best.ip);
    fmt_ip(v->mask, sizeof v->mask, best.netmask);
    fmt_ip(v->gw, sizeof v->gw, best.gateway);
}

static const char *icon_for(const struct network_view *v) {
    if (!v->have_device) return "tray-network-off";
    if (v->connected) return "tray-network";
    return "tray-network-limited";
}

// TWO CLOCKS, because the two questions have different answers.
//
// THE DEVICE is read on a CADENCE, which is the one place this differs
// from every other poll in wm.c: there is no netdev generation in the
// ABI and no change notification, so the choice was a once-a-second
// read or a new kernel counter. The read is one sys_query_record() per
// device -- a memcpy out of a live kernel table with NO I/O behind it --
// which is not what the poll convention was defending against (a
// whole-file disk read for the desktop's entries). If a second caller
// ever wants this, a generation counter is the right answer.
//
// VISIBILITY is on the SETTINGS GENERATION, because tray_want_shown()
// reads /etc/desktop.conf and must not do that once a second forever.
// It cannot ride the device compare: `desktop.tray_network` moving
// changes nothing about the card, and the first version of this
// therefore ignored the setting entirely.
void network_poll(void) {
    struct network_view now;
    read_devices(&now);

    int changed = now.have_device != g_view.have_device ||
                  now.connected != g_view.connected ||
                  now.link_local != g_view.link_local ||
                  now.link_up != g_view.link_up ||
                  now.link_known != g_view.link_known ||
                  now.device_count != g_view.device_count ||
                  k_strcmp(now.ip, g_view.ip) != 0 ||
                  k_strcmp(now.name, g_view.name) != 0;
    g_view = now;

    // VISIBILITY DEPENDS ON THE SETTING TOO, so it cannot hang off the
    // device compare alone -- `desktop.tray_network` moving changes
    // nothing about the card, and the first version of this ignored it
    // entirely. It gets its own generation compare, because
    // tray_want_shown() reads /etc/desktop.conf and must not run once a
    // second forever.
    uint32_t gen = wm_setting_generation();
    if (gen != g_seen_generation || changed) {
        g_seen_generation = gen;
        tray_set_hidden(g_tray_id, !tray_want_shown("network", g_view.have_device));
    }
    if (!changed) return;

    g_icon = icon_for(&g_view);
    tray_set_icon(g_tray_id, g_icon);
    if (network_open) redraw_pending = 1;
}

void network_view(struct network_view *v) { *v = g_view; }
int network_tray_hidden(void) { return tray_is_hidden(g_tray_id); }

void network_tray_init(void) {
    read_devices(&g_view);
    g_icon = icon_for(&g_view);
    g_tray_id = tray_register_icon(g_icon);
    tray_set_hidden(g_tray_id, !tray_want_shown("network", g_view.have_device));
}

// --- geometry ---------------------------------------------------------

// The rows the panel shows. Built here rather than in draw() so that
// drawing, the panel's width and `gui network --json` cannot disagree
// about what is on it -- the one-geometry-function rule every flyout
// here follows.
#define NET_ROWS 5

static int build_rows(char keys[NET_ROWS][12], char vals[NET_ROWS][40]) {
    int n = 0;
    if (!g_view.have_device) {
        k_strlcpy(keys[n], "Network", 12);
        k_strlcpy(vals[n], "no network device", 40);
        return n + 1;
    }
    k_strlcpy(keys[n], "Interface", 12);
    k_snprintf(vals[n], 40, "%s%s%s", g_view.name,
               g_view.location[0] ? " at " : "", g_view.location);
    n++;
    k_strlcpy(keys[n], "Address", 12);
    if (g_view.link_local) k_snprintf(vals[n], 40, "%s (self-assigned)", g_view.ip);
    else k_strlcpy(vals[n], g_view.ip, 40);
    n++;
    k_strlcpy(keys[n], "Netmask", 12);
    k_strlcpy(vals[n], g_view.mask, 40);
    n++;
    k_strlcpy(keys[n], "Gateway", 12);
    k_strlcpy(vals[n], g_view.gw, 40);
    n++;
    k_strlcpy(keys[n], "Link", 12);
    fmt_speed(vals[n], 40, &g_view);
    n++;
    return n;
}

void network_geometry(struct network_geom *g) {
    k_memset(g, 0, sizeof *g);

    int ch = ugfx_char_h();
    int pad = ch / 2 + 2;
    int row_h = ch + 4;

    char keys[NET_ROWS][12], vals[NET_ROWS][40];
    int rows = build_rows(keys, vals);

    int key_w = 0, val_w = 0;
    for (int i = 0; i < rows; i++) {
        int kw = ugfx_text_width(keys[i]);
        int vw = ugfx_text_width(vals[i]);
        if (kw > key_w) key_w = kw;
        if (vw > val_w) val_w = vw;
    }
    int w = pad + key_w + pad + val_w + pad;
    int min_w = ugfx_char_w() * 24 + pad * 2;
    if (w < min_w) w = min_w;
    int h = pad + rows * row_h + pad;

    int tx = 0, ty = 0, tw = 0, th = 0;
    if (!tray_item_rect(g_tray_id, &tx, &ty, &tw, &th)) {
        tx = screen_w - 32; ty = screen_h - taskbar_h; tw = 16; th = taskbar_h;
    }
    int x, y;
    wm_popup_place(tx + tw - w, screen_h - taskbar_h - h, w, h, &x, &y);

    g->x = x; g->y = y; g->w = w; g->h = h;
    g->tray_x = tx; g->tray_y = ty; g->tray_w = tw; g->tray_h = th;
    g->pad = pad; g->row_h = row_h;
}

// --- state ------------------------------------------------------------

void network_damage(void) {
    struct network_geom g;
    network_geometry(&g);
    wm_damage_rect(g.x, g.y, g.w, g.h);
    redraw_pending = 1;
}

static void network_open_now(void) {
    wm_overlay_close_others("network");
    network_poll();          // never show a stale address on opening
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
    if (uui_hit(g.x, g.y, g.w, g.h, mx, my)) return 1;  // inert, but ours
    network_close();
    // A click on the TASKBAR still belongs to the taskbar -- dismissing
    // must not also swallow the window button underneath.
    return my < screen_h - taskbar_h;
}

int network_hover_at(int mx, int my) {
    struct network_geom g;
    network_geometry(&g);
    if (tray_is_hidden(g_tray_id)) return 0;
    g_hover = uui_hit(g.tray_x, g.tray_y, g.tray_w, g.tray_h, mx, my);
    return g_hover;
}

// --- drawing ----------------------------------------------------------

void network_draw(int mx, int my) {
    (void)mx; (void)my;
    struct network_geom g;
    network_geometry(&g);

    uint32_t bg = UTHEME_PANEL_BG, border = UTHEME_BORDER, fg = UTHEME_TEXT;
    ugfx_fill_rect(wm_surface(), g.x, g.y, g.w, g.h, bg);

    char keys[NET_ROWS][12], vals[NET_ROWS][40];
    int rows = build_rows(keys, vals);
    int key_w = 0;
    for (int i = 0; i < rows; i++) {
        int kw = ugfx_text_width(keys[i]);
        if (kw > key_w) key_w = kw;
    }
    for (int i = 0; i < rows; i++) {
        int ry = g.y + g.pad + i * g.row_h;
        // The label is dimmer than the value: the value is what is being
        // read, and utheme's BORDER is the theme's own muted ink rather
        // than a tint picked here.
        ugfx_draw_string_clipped(wm_surface(), g.x + g.pad, ry, key_w + 4,
                                 keys[i], border, bg);
        ugfx_draw_string_clipped(wm_surface(), g.x + g.pad + key_w + g.pad, ry,
                                 g.w - g.pad * 3 - key_w, vals[i], fg, bg);
    }
    ugfx_draw_rect(wm_surface(), g.x, g.y, g.w, g.h, border);
}
