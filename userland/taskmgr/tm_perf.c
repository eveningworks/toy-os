// Task Manager's Performance page: a list of devices, each with its own
// small trace, and the selected one in full -- Windows' Performance tab.
// The network cards are devices here, which is where Windows puts
// Ethernet and what the Network tab on the roadmap asked for: per-card
// throughput, address, lease, and the connection log (QUERY_CONNLOG,
// the ring `netlog` reads).
//
// EVERY DEVICE IS SAMPLED EVERY TICK, the page showing or not, so a
// spike from before the page was opened is still on its chart.
//
// The device list is a widget of this file's own, not the toolkit's: a
// list whose rows draw a trace has one caller, and Toykit takes a
// widget on its second (CLAUDE.md, "a second real caller").
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include "rt/sys.h"
#include "ui/ulog.h"
#include "ui/ugfx.h"
#include "ui/uapp.h"
#include "ui/utheme.h"
#include "ui/uui_widget.h"
#include "ui/uui_primitives.h"
#include "ui/uui_layout.h"
#include "ui/uui_label.h"
#include "ui/uui_chart.h"
#include "ui/uui_table.h"
#include "ui/uui_focus.h"
#include "ui/uui_describe.h"
#include "lib/human.h"
#include "lib/uconf.h"
#include "lib/udevice.h"
#include "keyboard.h"
#include "query_abi.h"
#include "taskmgr/tm_internal.h"

enum { ID_DEVICES = TM_ID_PERF, ID_CHART, ID_CONNS };

enum dev_kind { DEV_CPU, DEV_MEM, DEV_DISK, DEV_NET };
#define NET_MAX 4
#define DEV_MAX (3 + NET_MAX)

struct dev {
    enum dev_kind kind;
    char name[24];         // "Ethernet", the card's own name below it
    char line1[40], line2[40];
    struct uui_chart chart;
    char reading[40];      // the chart's top-right value
    // A network card's last counters, for its rate. By NAME, since the
    // record index can move when a card is hot-plugged.
    char card[16];
    unsigned long long rx, tx;
    unsigned long long rx_rate, tx_rate;
    int seen;
};

static struct dev g_dev[DEV_MAX];
static int g_ndev;
static int g_sel;   // selected device index

// --- sampling -----------------------------------------------------------------

static unsigned long long g_disk_r, g_disk_w, g_disk_rrate, g_disk_wrate;
static int g_disk_seen;

static void rate_str(char *out, int cap, unsigned long long bps) {
    char h[16];
    human_size_iec(h, sizeof h, bps);
    snprintf(out, (size_t)cap, "%s/s", h);
}

static struct dev *net_dev(const char *card) {
    for (int i = 0; i < g_ndev; i++)
        if (g_dev[i].kind == DEV_NET && strcmp(g_dev[i].card, card) == 0) return &g_dev[i];
    if (g_ndev >= DEV_MAX) return 0;
    struct dev *d = &g_dev[g_ndev++];
    memset(d, 0, sizeof *d);
    d->kind = DEV_NET;
    strlcpy(d->name, "Ethernet", sizeof d->name);
    strlcpy(d->card, card, sizeof d->card);
    uui_chart_init(&d->chart, "Receive + send (darker: send)");
    uui_chart_set_interval(&d->chart, TM_REFRESH_MS);
    uui_chart_set_scale(&d->chart, 0);
    return d;
}

// Bytes over the MEASURED interval since the last sample, not the
// nominal tick: a late tick (a blocked service request, a loaded
// machine) would otherwise chart several ticks' traffic as one's.
static unsigned long long g_sample_ns, g_interval_ms = TM_REFRESH_MS;

static unsigned long long per_second(unsigned long long now, unsigned long long was) {
    return (now > was ? now - was : 0) * 1000ULL / g_interval_ms;
}

void tm_perf_sample(void) {
    char a[16], b[16];
    unsigned long long t = sys_monotonic_ns();
    if (g_sample_ns && t > g_sample_ns) {
        g_interval_ms = (t - g_sample_ns) / 1000000ULL;
        if (!g_interval_ms) g_interval_ms = 1;
    }
    g_sample_ns = t;

    struct dev *cpu = &g_dev[DEV_CPU];
    uui_chart_push(&cpu->chart, g_cpu_pm / 10);
    snprintf(cpu->reading, sizeof cpu->reading, "%u%%", g_cpu_pm / 10);
    snprintf(cpu->line1, sizeof cpu->line1, "%u%%", g_cpu_pm / 10);
    snprintf(cpu->line2, sizeof cpu->line2, "%d processes", g_nproc);

    struct dev *mem = &g_dev[DEV_MEM];
    uui_chart_set_scale(&mem->chart, (uint32_t)(g_mem_total >> 10));
    uui_chart_push(&mem->chart, (uint32_t)(g_mem_used >> 10));
    human_size_iec(a, sizeof a, g_mem_used);
    human_size_iec(b, sizeof b, g_mem_total);
    snprintf(mem->reading, sizeof mem->reading, "%s of %s", a, b);
    snprintf(mem->line1, sizeof mem->line1, "%s / %s", a, b);
    snprintf(mem->line2, sizeof mem->line2, "%llu%%",
             g_mem_total ? g_mem_used * 100ULL / g_mem_total : 0ULL);

    // Disk: every block transfer, read and written -- QUERY_BLKSTAT is
    // the whole machine's, not per device.
    unsigned long long r = 0, w = 0;
    struct query_blkstat bs;
    QUERY_FOREACH(QUERY_BLKSTAT, bs, i) {
        if (!strcmp(bs.name, "read"))  r += bs.sectors * 512ULL;
        if (!strcmp(bs.name, "write")) w += bs.sectors * 512ULL;
    }
    if (g_disk_seen) {
        g_disk_rrate = per_second(r, g_disk_r);
        g_disk_wrate = per_second(w, g_disk_w);
    }
    g_disk_r = r; g_disk_w = w; g_disk_seen = 1;
    struct dev *disk = &g_dev[DEV_DISK];
    uui_chart_push_split(&disk->chart, (uint32_t)(g_disk_rrate + g_disk_wrate),
                         (uint32_t)g_disk_wrate);
    rate_str(a, sizeof a, g_disk_rrate);
    rate_str(b, sizeof b, g_disk_wrate);
    snprintf(disk->reading, sizeof disk->reading, "R %s  W %s", a, b);
    snprintf(disk->line1, sizeof disk->line1, "R %s", a);
    snprintf(disk->line2, sizeof disk->line2, "W %s", b);

    struct query_netdev nd;
    QUERY_FOREACH(QUERY_NETDEV, nd, j) {
        struct dev *d = net_dev(nd.name);
        if (!d) break;
        if (d->seen) {
            d->rx_rate = per_second(nd.rx_bytes, d->rx);
            d->tx_rate = per_second(nd.tx_bytes, d->tx);
        }
        d->rx = nd.rx_bytes; d->tx = nd.tx_bytes; d->seen = 1;
        uui_chart_push_split(&d->chart, (uint32_t)(d->rx_rate + d->tx_rate), (uint32_t)d->tx_rate);
        rate_str(a, sizeof a, d->rx_rate);
        rate_str(b, sizeof b, d->tx_rate);
        snprintf(d->reading, sizeof d->reading, "R %s  S %s", a, b);
        strlcpy(d->line1, d->card, sizeof d->line1);
        snprintf(d->line2, sizeof d->line2, "R %s  S %s", a, b);
    }
    for (int i = 0; i < g_ndev; i++) uui_chart_set_value(&g_dev[i].chart, g_dev[i].reading);
}

// --- the device list: this file's own widget ------------------------------------

struct devlist { int x, y, w, h; int hovered, focused; };
static struct devlist g_list = { .hovered = -1 };

// Every inset from the font (userland/CLAUDE.md, "Layout is
// FONT-DERIVED"): a quarter line of padding, three text lines.
static int pad(void) { int p = ugfx_char_h() / 4; return p > 2 ? p : 2; }
static int item_h(void) { return ugfx_char_h() * 3 + 3 * pad(); }

static void dl_natural_size(const void *w, int *ow, int *oh) {
    (void)w;
    if (ow) *ow = ugfx_char_w() * 15;
    if (oh) *oh = item_h() * 3;
}
static void dl_set_geometry(void *w, int x, int y, int ww, int hh) {
    struct devlist *l = w;
    l->x = x; l->y = y; l->w = ww; l->h = hh;
}
static void dl_bounds(const void *w, int *x, int *y, int *ww, int *hh) {
    const struct devlist *l = w;
    *x = l->x; *y = l->y; *ww = l->w; *hh = l->h;
}
static int dl_at(const struct devlist *l, int cx, int cy) {
    if (cx < l->x || cx >= l->x + l->w || cy < l->y) return -1;
    int i = (cy - l->y) / item_h();
    return i < g_ndev ? i : -1;
}

// A trace in a box: the chart's samples, newest at the right edge.
static void spark(struct ugfx_surface *s, int x, int y, int w, int h, const struct uui_chart *c) {
    ugfx_fill_rect(s, x, y, w, h, UTHEME_WHITE);
    ugfx_draw_rect(s, x, y, w, h, UTHEME_OUTLINE);
    int n = c->count < 64 ? c->count : 64;   // the last 64 samples, 32 s
    if (n < 2) return;
    uint32_t max = c->scale_max;
    if (!max) for (int i = 0; i < n; i++) if (uui_chart_recent(c, i) > max) max = uui_chart_recent(c, i);
    if (!max) max = 1;
    int xs[64], ys[64], m = 0;
    for (int i = n - 1; i >= 0; i--, m++) {        // oldest first, left to right
        xs[m] = x + 1 + (w - 3) * m / (n - 1);
        uint32_t v = uui_chart_recent(c, i);
        if (v > max) v = max;
        ys[m] = y + h - 2 - (int)((unsigned long long)(h - 3) * v / max);
    }
    ugfx_draw_polyline(s, xs, ys, m, 0, UTHEME_ACCENT, GEOM_AA);
}

static void dl_draw(struct ugfx_surface *s, const void *w) {
    const struct devlist *l = w;
    int ih = item_h(), ch = ugfx_char_h();
    for (int i = 0; i < g_ndev; i++) {
        int y = l->y + i * ih;
        if (y >= l->y + l->h) break;
        uint32_t bg = i == g_sel ? UTHEME_SELECTION
                    : i == l->hovered ? uui_state_bg(UTHEME_PANEL_BG, UUI_STATE_HOVER)
                    : UTHEME_PANEL_BG;
        int p = pad();
        ugfx_fill_rect(s, l->x, y, l->w, ih - p / 2, bg);
        int sw = ch * 4, sx = l->x + p;
        spark(s, sx, y + p, sw, ih - 2 * p, &g_dev[i].chart);
        int tx = sx + sw + 2 * p, avail = l->x + l->w - tx - p;
        int ty = y + p;
        const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
        ugfx_draw_string_clipped(s, tx, ty, avail, g_dev[i].name, UTHEME_TEXT, bg);
        ugfx_set_font(was);
        uint32_t dim = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
        ugfx_draw_string_clipped(s, tx, ty + ch, avail, g_dev[i].line1, dim, bg);
        ugfx_draw_string_clipped(s, tx, ty + 2 * ch, avail, g_dev[i].line2, dim, bg);
    }
    if (l->focused && g_sel >= 0) uui_focus_ring(s, l->x, l->y + g_sel * ih, l->w, ih - pad() / 2);
}

static int dl_hit(const void *w, int cx, int cy) {
    const struct devlist *l = w;
    return uui_hit(l->x, l->y, l->w, l->h, cx, cy);
}
static void select_dev(int i);
static int dl_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    int i = dl_at(w, cx, cy);
    if (i < 0 || i == g_sel) return 0;
    select_dev(i);
    return 1;
}
static int dl_release(void *w, int cx, int cy) { (void)w; (void)cx; (void)cy; return 0; }
static int dl_motion(void *w, int cx, int cy, unsigned buttons) {
    (void)buttons;
    struct devlist *l = w;
    int i = dl_at(l, cx, cy);
    if (i == l->hovered) return 0;
    l->hovered = i;
    return 1;
}
static int dl_key(void *w, int key, unsigned mods) {
    (void)w; (void)mods;
    if (key == KEY_ARROW_UP && g_sel > 0) { select_dev(g_sel - 1); return 1; }
    if (key == KEY_ARROW_DOWN && g_sel < g_ndev - 1) { select_dev(g_sel + 1); return 1; }
    return 0;
}
static int dl_accepts_focus(const void *w) { (void)w; return 1; }
static void dl_set_focused(void *w, int f) { ((struct devlist *)w)->focused = f; }
static void dl_describe(const void *w, const struct uui_describe *d) {
    const struct devlist *l = w;
    uui_describe_int(d, "item_h", item_h());
    uui_describe_int(d, "count", g_ndev);
    uui_describe_int(d, "selected", g_sel);
    (void)l;
}

static const struct uui_widget_ops devlist_ops = {
    .natural_size = dl_natural_size, .set_geometry = dl_set_geometry, .bounds = dl_bounds,
    .draw = dl_draw, .hit = dl_hit, .press = dl_press, .release = dl_release,
    .motion = dl_motion, .key = dl_key, .accepts_focus = dl_accepts_focus,
    .set_focused = dl_set_focused, .describe = dl_describe,
};

// --- the detail side ---------------------------------------------------------------

#define STATS 12
static struct uui_label g_title, g_subtitle, g_spacer;
static char g_title_text[32], g_subtitle_text[64];
static struct uui_label g_key[STATS], g_val[STATS];
static char g_key_text[STATS][24], g_val_text[STATS][48];
static int g_nstats;

// The connection log, newest first.
#define CONNS_MAX 32
static struct query_connlog g_conns[CONNS_MAX];
static int g_nconns;
static long long g_local_offset;   // netlog.c's: time() is local, QUERY_CLOCK's utc is not
static struct uui_table g_conntable;
static const struct uui_table_column CONN_COLS[] = {
    { "Time", 8, UUI_TALIGN_LEFT }, { "Dir", 4, UUI_TALIGN_LEFT },
    { "Proto", 6, UUI_TALIGN_LEFT }, { "Program", 10, UUI_TALIGN_LEFT },
    { "Remote", 0, UUI_TALIGN_LEFT },
};

static void conn_cell(void *ctx, int row, int col, char *out, int cap) {
    (void)ctx;
    out[0] = '\0';
    if (row < 0 || row >= g_nconns) return;
    const struct query_connlog *r = &g_conns[row];
    unsigned long long ip = r->remote_ip;
    switch (col) {
    case 0: {
        time_t t = (time_t)((long long)r->utc + g_local_offset);
        struct tm tm;
        if (r->utc && gmtime_r(&t, &tm))
            snprintf(out, (size_t)cap, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
        break;
    }
    case 1: strlcpy(out, r->direction == QUERY_CONNLOG_IN ? "in" : "out", (size_t)cap); break;
    case 2: strlcpy(out, r->proto == 6 ? "tcp" : r->proto == 17 ? "udp" : r->proto == 1 ? "icmp" : "?",
                    (size_t)cap); break;
    case 3: strlcpy(out, r->comm[0] ? r->comm : "-", (size_t)cap); break;
    case 4:
        if (r->remote_port)
            snprintf(out, (size_t)cap, "%llu.%llu.%llu.%llu:%llu%s%s%s", (ip >> 24) & 255,
                     (ip >> 16) & 255, (ip >> 8) & 255, ip & 255,
                     (unsigned long long)r->remote_port, r->host[0] ? " (" : "", r->host,
                     r->host[0] ? ")" : "");
        else
            snprintf(out, (size_t)cap, "%llu.%llu.%llu.%llu", (ip >> 24) & 255,
                     (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
        break;
    default: break;
    }
}

static void read_conns(void) {
    static struct query_connlog all[128];
    int n = 0;
    struct query_connlog r;
    QUERY_FOREACH(QUERY_CONNLOG, r, i) {
        if (n < 128) all[n++] = r;
    }
    g_nconns = 0;
    for (int i = n - 1; i >= 0 && g_nconns < CONNS_MAX; i--) g_conns[g_nconns++] = all[i];
    uui_table_set_rows(&g_conntable, g_nconns);
}

static void add_stat(const char *k, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void add_stat(const char *k, const char *fmt, ...) {
    if (g_nstats >= STATS) return;
    strlcpy(g_key_text[g_nstats], k, sizeof g_key_text[0]);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_val_text[g_nstats], sizeof g_val_text[0], fmt, ap);
    va_end(ap);
    g_nstats++;
}

static void ip_str(char *out, int cap, unsigned long long ip) {
    snprintf(out, (size_t)cap, "%llu.%llu.%llu.%llu", (ip >> 24) & 255, (ip >> 16) & 255,
             (ip >> 8) & 255, ip & 255);
}

static int mask_bits(unsigned long long m) {
    int n = 0;
    for (int i = 0; i < 32; i++) if (m & (1ULL << i)) n++;
    return n;
}

static void fill_stats(void) {
    char a[16], b[16];
    g_nstats = 0;
    const struct dev *d = &g_dev[g_sel];
    switch (d->kind) {
    case DEV_CPU: {
        strlcpy(g_title_text, "CPU", sizeof g_title_text);
        // Both asked ONCE: neither changes, and CPUID is a VM exit.
        static char brand[64];
        static int ncpu = -1;
        if (!brand[0]) udevice_cpu_brand(brand, sizeof brand);
        if (ncpu < 0) {
            ncpu = 0;
            struct query_cpu qc;
            QUERY_FOREACH(QUERY_CPUS, qc, i) ncpu++;
        }
        strlcpy(g_subtitle_text, brand, sizeof g_subtitle_text);
        unsigned long long s = sys_monotonic_ns() / 1000000000ULL;
        add_stat("Utilization", "%u%%", g_cpu_pm / 10);
        add_stat("Processes", "%d", g_nproc);
        add_stat("Threads", "%d", g_threads);
        add_stat("Up time", "%llu:%02llu:%02llu", s / 3600, (s / 60) % 60, s % 60);
        add_stat("Logical CPUs", "%d", ncpu ? ncpu : 1);
        break;
    }
    case DEV_MEM: {
        strlcpy(g_title_text, "Memory", sizeof g_title_text);
        human_size_iec(a, sizeof a, g_mem_total);
        snprintf(g_subtitle_text, sizeof g_subtitle_text, "%s usable", a);
        human_size_iec(a, sizeof a, g_mem_used);
        add_stat("In use", "%s", a);
        human_size_iec(a, sizeof a, g_mem_total - g_mem_used);
        add_stat("Available", "%s", a);
        struct query_meminfo mi;
        if (sys_query_record(QUERY_MEMINFO, 0, &mi, sizeof mi) >= (int)sizeof mi) {
            human_size_iec(a, sizeof a, mi.heap_used_bytes);
            human_size_iec(b, sizeof b, mi.heap_total_bytes);
            add_stat("Kernel heap", "%s of %s", a, b);
            add_stat("Page size", "%llu bytes", (unsigned long long)mi.frame_bytes);
        }
        break;
    }
    case DEV_DISK: {
        strlcpy(g_title_text, "Disk", sizeof g_title_text);
        strlcpy(g_subtitle_text, "every block device", sizeof g_subtitle_text);
        rate_str(a, sizeof a, g_disk_rrate);
        add_stat("Read", "%s", a);
        rate_str(a, sizeof a, g_disk_wrate);
        add_stat("Write", "%s", a);
        human_size_iec(a, sizeof a, g_disk_r);
        add_stat("Read since boot", "%s", a);
        human_size_iec(a, sizeof a, g_disk_w);
        add_stat("Written since boot", "%s", a);
        struct query_fsinfo fs;
        QUERY_FOREACH(QUERY_FSINFO, fs, i) {
            if (!(fs.flags & QUERY_FS_MOUNTED) || !fs.total_bytes) continue;
            human_size_iec(a, sizeof a, fs.used_bytes);
            human_size_iec(b, sizeof b, fs.total_bytes);
            char k[24];
            snprintf(k, sizeof k, "%s (%s)", fs.point, fs.name);
            add_stat(k, "%s of %s", a, b);
        }
        break;
    }
    case DEV_NET: {
        strlcpy(g_title_text, "Ethernet", sizeof g_title_text);
        struct query_netdev nd;
        int found = 0;
        QUERY_FOREACH(QUERY_NETDEV, nd, i) if (!strcmp(nd.name, d->card)) { found = 1; break; }
        snprintf(g_subtitle_text, sizeof g_subtitle_text, "%s, %s", d->card,
                 found ? nd.driver : "gone");
        if (!found) break;
        rate_str(a, sizeof a, d->rx_rate);
        add_stat("Receive", "%s", a);
        rate_str(a, sizeof a, d->tx_rate);
        add_stat("Send", "%s", a);
        human_size_iec(a, sizeof a, nd.rx_bytes);
        add_stat("Received", "%s, %llu packets", a, (unsigned long long)nd.rx_packets);
        human_size_iec(a, sizeof a, nd.tx_bytes);
        add_stat("Sent", "%s, %llu packets", a, (unsigned long long)nd.tx_packets);
        add_stat("Dropped", "%llu in, %llu out", (unsigned long long)nd.rx_dropped,
             (unsigned long long)nd.tx_dropped);
        if (nd.ip) {
            ip_str(a, sizeof a, nd.ip);
            add_stat("IPv4 address", "%s / %d", a, mask_bits(nd.netmask));
            ip_str(a, sizeof a, nd.gateway);
            add_stat("Gateway", "%s", a);
        } else {
            add_stat("IPv4 address", "none");
        }
        // udhcp.c's lease file: the length and the server that granted it.
        char path[48], secs[16], server[24];
        snprintf(path, sizeof path, "/var/dhcp-%s.lease", d->card);
        if (uconf_get(path, "seconds", secs, sizeof secs) &&
            uconf_get(path, "server", server, sizeof server)) {
            unsigned long long sec = strtoull(secs, 0, 10);
            if (sec >= 3600) add_stat("DHCP lease", "%llu h, from %s", sec / 3600, server);
            else add_stat("DHCP lease", "%llu min, from %s", sec / 60, server);
        }
        unsigned long long m = nd.mac;
        add_stat("MAC", "%02llx:%02llx:%02llx:%02llx:%02llx:%02llx", m & 255, (m >> 8) & 255,
             (m >> 16) & 255, (m >> 24) & 255, (m >> 32) & 255, (m >> 40) & 255);
        if (nd.link_known)
            add_stat("Link", nd.link_up ? "up, %llu Mb/s" : "down", nd.link_bps / 1000000ULL);
        add_stat("Bus", "%s, MTU %llu", nd.location, (unsigned long long)nd.mtu);
        break;
    }
    }
    for (int i = 0; i < STATS; i++) {
        uui_label_set_text(&g_key[i], i < g_nstats ? g_key_text[i] : "");
        uui_label_set_text(&g_val[i], i < g_nstats ? g_val_text[i] : "");
    }
}

// --- layout and hooks ----------------------------------------------------------

static struct uui_item HEAD_ITEMS[3];
static struct uui_layout HEAD;
static struct uui_item STAT_ITEMS[STATS * 2];
static struct uui_layout STATS_GRID;
static struct uui_item SIDE_ITEMS[4];
static struct uui_layout SIDE;
static struct uui_item PAGE_ITEMS[2];
static struct uui_layout PAGE;
static struct uui_item g_root = { .ops = &uui_layout_ops, .widget = &PAGE, .name = "perf" };

static void select_dev(int i) {
    if (i < 0 || i >= g_ndev) return;
    g_sel = i;
    SIDE_ITEMS[1].widget = &g_dev[i].chart;
    SIDE_ITEMS[3].hidden = g_dev[i].kind != DEV_NET;
    if (!SIDE_ITEMS[3].hidden) read_conns();
    fill_stats();
    // A different chart and a shown/hidden log: lay out NOW, or both
    // draw at the old (or no) geometry until the next tick.
    tm_relayout();
    ulogf("taskmgr: device %d %s\n", i, g_dev[i].name);
}

static int on_widget(struct uapp *a, int id, int reason) {
    (void)a; (void)reason;
    if (id == ID_DEVICES) return 1;   // select_dev() already ran from the press or key
    if (id == ID_CHART) return 1;
    return 0;
}

static void tick(struct uapp *a, int shown) {
    (void)a;
    if (!shown) return;
    fill_stats();
    if (g_dev[g_sel].kind == DEV_NET) read_conns();
}

static void page_open(struct uapp *a) {
    (void)a;
    select_dev(g_sel);
}

static int focusables(struct uui_focusable *out, int cap) {
    if (cap < 1) return 0;
    out[0] = (struct uui_focusable){ &g_list, &devlist_ops };
    return 1;
}

void tm_perf_init(struct tm_page *page) {
    struct query_clock c;
    if (sys_query_record(QUERY_CLOCK, 0, &c, sizeof c) >= (int)sizeof c)
        g_local_offset = (long long)time(0) - (long long)c.utc;

    static const char *const NAMES[3] = { "CPU", "Memory", "Disk" };
    for (int i = 0; i < 3; i++) {
        struct dev *d = &g_dev[i];
        memset(d, 0, sizeof *d);
        d->kind = (enum dev_kind)i;
        strlcpy(d->name, NAMES[i], sizeof d->name);
        uui_chart_init(&d->chart, i == DEV_CPU ? "Utilization" : i == DEV_MEM ? "In use"
                                                                          : "Read + write (darker: write)");
        uui_chart_set_interval(&d->chart, TM_REFRESH_MS);
    }
    uui_chart_set_scale(&g_dev[DEV_CPU].chart, 100);
    uui_chart_set_scale(&g_dev[DEV_DISK].chart, 0);
    g_ndev = 3;

    uui_label_init(&g_title, g_title_text);
    g_title.font = ugfx_font_session(UGFX_FONT_BOLD);
    uui_label_init(&g_subtitle, g_subtitle_text);
    uui_label_init(&g_spacer, "");
    HEAD_ITEMS[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_title, .name = "perftitle" };
    HEAD_ITEMS[1] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_spacer, .flags = UUI_FILL_W };
    HEAD_ITEMS[2] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_subtitle };
    HEAD = (struct uui_layout){ .dir = UUI_ROW, .items = HEAD_ITEMS, .count = 3 };

    for (int i = 0; i < STATS; i++) {
        uui_label_init(&g_key[i], "");
        g_key[i].fg = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
        uui_label_init(&g_val[i], "");
        STAT_ITEMS[2 * i] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_key[i] };
        STAT_ITEMS[2 * i + 1] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_val[i],
                                                   .flags = UUI_FILL_W };
    }
    // Four columns -- key, value, key, value -- so twelve facts take six
    // rows and leave the chart its height.
    STATS_GRID = (struct uui_layout){ .dir = UUI_GRID, .cols = 4, .items = STAT_ITEMS,
                                      .count = STATS * 2 };

    uui_table_init(&g_conntable, 0, 0, 100, 100, CONN_COLS,
                   (int)(sizeof CONN_COLS / sizeof CONN_COLS[0]), conn_cell, 0);
    uui_table_set_seek_col(&g_conntable, 3);

    SIDE_ITEMS[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &HEAD, .flags = UUI_FILL_W };
    SIDE_ITEMS[1] = (struct uui_item){ .ops = &uui_chart_ops, .widget = &g_dev[0].chart,
                                        .id = ID_CHART, .name = "perfchart", .flags = UUI_FILL_W };
    SIDE_ITEMS[2] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &STATS_GRID,
                                        .name = "perfstats", .flags = UUI_FILL_W };
    SIDE_ITEMS[3] = (struct uui_item){ .ops = &uui_table_ops, .widget = &g_conntable,
                                        .id = ID_CONNS, .name = "connlog",
                                        .flags = UUI_FILL_W | UUI_FILL_H, .hidden = 1 };
    SIDE = (struct uui_layout){ .dir = UUI_COLUMN, .items = SIDE_ITEMS, .count = 4 };

    PAGE_ITEMS[0] = (struct uui_item){ .ops = &devlist_ops, .widget = &g_list, .id = ID_DEVICES,
                                        .name = "devices", .flags = UUI_FILL_H };
    PAGE_ITEMS[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &SIDE,
                                        .flags = UUI_FILL_W | UUI_FILL_H };
    PAGE = (struct uui_layout){ .dir = UUI_ROW, .items = PAGE_ITEMS, .count = 2 };

    *page = (struct tm_page){
        .label = "Performance", .icon = "tb-chart", .root = &g_root,
        .open = page_open, .tick = tick, .widget = on_widget, .focusables = focusables,
    };
}
