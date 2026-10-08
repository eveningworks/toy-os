// Device Manager -- the machine's devices, the driver driving each, and
// Disable/Enable. Windows' Device Manager in shape (a tree by type, or by
// connection), with the selected device's properties beside the tree
// rather than in a dialog, as System Settings pages sit beside its
// sidebar.
//
// EVERYTHING ABOUT DEVICES IS lib/udevice.c, shared with /bin/devctl --
// this file is the view. The pane is ONE PAGE OF SECTIONS (ui/uui_props.h)
// filled from udevice_props(), so what it shows, what Copy details copies
// and what `devctl show` prints are one list. Disabling is an unbind;
// "keep disabled after restart" writes /etc/devices.conf, which the
// `devices` service re-applies at boot (udevice.h says what each state
// means).
//
// THE LIST IS RE-READ WHEN IT CHANGES, NOT ON A TIMER: naming a device
// streams the 1.6 MB pci.ids, so the tick compares a cheap signature
// (each device's binding and holder, the USB slots, the event count) and
// relists only when it moved -- a USB device plugged in, or a driver let
// go.
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "rt/sys.h"
#include "query_abi.h"
#include "keyboard.h"
#include "ui/uapp.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/ulog.h"
#include "ui/uui_menubar.h"
#include "ui/uui_segmented.h"
#include "ui/uui_textbox.h"
#include "ui/uui_tree.h"
#include "ui/uui_splitter.h"
#include "ui/uui_props.h"
#include "ui/uui_button.h"
#include "ui/uui_checkbox.h"
#include "ui/uui_dropdown.h"
#include "ui/uui_statusbar.h"
#include "ui/uui_dialog.h"
#include "ui/uui_filedialog.h"
#include "ui/uui_focus.h"
#include "ui/uui_route.h"   // UUI_REASON_*
#include "lib/udevice.h"
#include "lib/uclip.h"
#include "ui/uui_sndformat.h"
#include "ui/uui_netadapter.h"
#include "ui/uui_clip.h"

#define ID_MENU    1
#define ID_VIEW    2
#define ID_TREE    3
#define ID_SPLIT   4
#define ID_TOGGLE  5
#define ID_REFRESH 6
#define ID_KEEP    7
#define ID_STATUS  8
#define ID_ASK     9
#define ID_FILTER  10
#define ID_PROPS   11
#define ID_MODS    12
#define ID_LOAD    13
#define ID_UNLOAD  14
#define ID_SNDFMT  32   // .. + UUI_SNDFORMAT_IDS: a sound device's Format
#define ID_NETADP  64   // .. + UUI_NETADAPTER_IDS: a network card's adapter settings

// The controls the pane holds in its sections (uui_props slots).
#define SLOT_FORMAT 1
#define SLOT_DRIVER 2
#define SLOT_NET 3

enum {
    CMD_REFRESH = 1, CMD_EXIT, CMD_BY_TYPE, CMD_BY_CONN, CMD_EXPAND, CMD_COLLAPSE,
    CMD_TOGGLE, CMD_COPY, CMD_EXPORT, CMD_FIND, CMD_LOAD, CMD_UNLOAD,
    ASK_DISABLE, ASK_UNLOAD, ASK_CANCEL,
};

// A category's node id: below every device index, so one int names both.
#define NODE_CAT(t) (-100 - (int)(t))
#define NODE_BUS_PCI -1
#define NODE_BUS_PLAT -2

static struct udevice g_dev[UDEV_MAX];
static int g_n;
static int g_by_conn;

static struct uui_tree_node g_nodes[UDEV_MAX + UDEV_T_COUNT + 4];
static int g_node_count;
static char g_label[UDEV_MAX][120];

static struct uui_menubar g_menu;
// A VIEW SWITCH, so a segmented control -- what Windows and macOS put
// over a list that can be grouped two ways; tabs would promise two pages.
static const char *const VIEWS[] = { "By type", "By connection" };
static struct uui_segmented g_view;
// THE FILTER OVER THE TREE (Win11 Settings, KDE System Settings): it
// hides what does not match, by name, ID, driver, vendor or location.
static struct uui_textbox g_filter;
static struct uui_tree g_tree;
static struct uui_splitter g_split;
static struct uui_props g_props;
static struct udev_prop g_p[UDEV_PROPS_MAX];
static char g_props_for[24];   // the device the pane was filled for
static struct uui_button g_toggle, g_refresh;
static struct uui_checkbox g_keep;
static struct uui_statusbar g_status;
static struct uui_dialog g_ask;
static struct uui_filedialog g_save;

// THE DRIVER'S ACTIONS, in the Driver section: a module to load for a
// device with none, or Unload for a driver that came in a module. A
// built-in driver has neither -- the section says so instead.
static struct uui_dropdown g_mods;
static char g_mod_name[8][16];
static char g_mod_label[8][40];
static const char *g_mod_items[8];
static int g_nmods;
static struct uui_button g_load, g_unload;
static char g_unload_label[40];
static int g_drv_mode;          // 0 nothing, 1 load, 2 unload

// A SOUND DEVICE'S FORMAT, in its own section: the card whose
// `device_id` is the selected device's id (QUERY_SOUND), shown with the
// panel System Settings uses. `g_fmt_card` is the card loaded, so a
// re-selection of the same device does not reset what is open.
static struct uui_sndformat g_fmt;
static char g_fmt_card[16];

// A NETWORK CARD'S ADAPTER SETTINGS, the same way: the card whose
// `device_id` is the selected device's (QUERY_NETDEV), with the panel
// System Settings' Adapters page uses. Keyed by MAC, which a rename by
// netd does not change.
static struct uui_netadapter g_net;
static uint64_t g_net_mac;

// Each panel inside a clip to the pane: it scrolls with the pane and is
// cut at its edges, rather than hidden until all of it fits.
static struct uui_clip g_fmt_clip, g_net_clip;

static char g_st_count[32], g_st_problem[48], g_st_note[96];
static char g_ask_line[2][112];
static const char *g_ask_rows[2];
static char g_ask_id[24];   // by ID: the tick may relist under the open dialog
static char g_ask_module[16];

// Pane geometry, placed by layout_all(): the header's height, and the
// whole pane's rect.
static int g_px, g_py, g_pw, g_ph, g_head_h;

static const struct uui_menu_item file_items[] = {
    UUI_MENU("Refresh", CMD_REFRESH, "F5"),
    UUI_MENU("Copy details", CMD_COPY, "Ctrl+C"),
    UUI_MENU("Export hardware report...", CMD_EXPORT, "Ctrl+E"),
    UUI_MENU_SEP,
    UUI_MENU("Exit", CMD_EXIT, "Alt+F4"),
};
static const struct uui_menu_item view_items[] = {
    UUI_MENU("Devices by type", CMD_BY_TYPE, 0),
    UUI_MENU("Devices by connection", CMD_BY_CONN, 0),
    UUI_MENU_SEP,
    UUI_MENU("Find a device", CMD_FIND, "Ctrl+F"),
    UUI_MENU("Expand all", CMD_EXPAND, 0),
    UUI_MENU("Collapse all", CMD_COLLAPSE, 0),
};
static const struct uui_menu_item action_items[] = {
    UUI_MENU("Disable or enable device", CMD_TOGGLE, 0),
    UUI_MENU("Load driver", CMD_LOAD, 0),
    UUI_MENU("Unload driver", CMD_UNLOAD, 0),
};
static const struct uui_menu_item menu_items[] = {
    UUI_SUBMENU("File", file_items),
    UUI_SUBMENU("View", view_items),
    UUI_SUBMENU("Action", action_items),
};

// The pane's slot controls AFTER the pane, so they draw over it and are
// asked first; the dialog LAST: an open one is on top, and the router
// asks the last item first.
static struct uui_item g_widgets[] = {
    { .ops = &uui_menubar_ops,   .widget = &g_menu,    .id = ID_MENU,    .name = "menu" },
    { .ops = &uui_segmented_ops, .widget = &g_view,    .id = ID_VIEW,    .name = "view" },
    { .ops = &uui_textbox_ops,   .widget = &g_filter,  .id = ID_FILTER,  .name = "filter" },
    { .ops = &uui_tree_ops,      .widget = &g_tree,    .id = ID_TREE,    .name = "tree" },
    { .ops = &uui_splitter_ops,  .widget = &g_split,   .id = ID_SPLIT,   .name = "split" },
    { .ops = &uui_props_ops,     .widget = &g_props,   .id = ID_PROPS,   .name = "props" },
    { .ops = &uui_clip_ops,      .widget = &g_fmt_clip, .name = "sndfmt_clip", .hidden = 1 },
    { .ops = &uui_clip_ops,      .widget = &g_net_clip, .name = "netadapter_clip", .hidden = 1 },
    { .ops = &uui_dropdown_ops,  .widget = &g_mods,    .id = ID_MODS,    .name = "mods",   .hidden = 1 },
    { .ops = &uui_button_ops,    .widget = &g_load,    .id = ID_LOAD,    .name = "load",   .hidden = 1 },
    { .ops = &uui_button_ops,    .widget = &g_unload,  .id = ID_UNLOAD,  .name = "unload", .hidden = 1 },
    { .ops = &uui_checkbox_ops,  .widget = &g_keep,    .id = ID_KEEP,    .name = "keep" },
    { .ops = &uui_button_ops,    .widget = &g_refresh, .id = ID_REFRESH, .name = "refresh" },
    { .ops = &uui_button_ops,    .widget = &g_toggle,  .id = ID_TOGGLE,  .name = "toggle" },
    { .ops = &uui_statusbar_ops, .widget = &g_status,  .id = ID_STATUS,  .name = "status" },
    { .ops = &uui_dialog_ops,    .widget = &g_ask,     .id = ID_ASK,     .name = "ask" },
};

// The ring is REBUILT when the pane's controls come and go: the Format
// panel and the driver's buttons sit between the tree and the buttons
// only while they are shown.
static struct uui_focusable g_focusables[10 + UUI_SNDFORMAT_IDS + UUI_NETADAPTER_IDS];
static struct uui_focus g_focus;

static struct uui_item *item_of(const void *widget) {
    for (unsigned i = 0; i < sizeof g_widgets / sizeof g_widgets[0]; i++)
        if (g_widgets[i].widget == widget) return &g_widgets[i];
    return 0;
}

static void build_focus(void) {
    int n = 0;
    g_focusables[n++] = (struct uui_focusable){ &g_filter, &uui_textbox_ops };
    g_focusables[n++] = (struct uui_focusable){ &g_view, &uui_segmented_ops };
    g_focusables[n++] = (struct uui_focusable){ &g_tree, &uui_tree_ops };
    if (!item_of(&g_fmt_clip)->hidden)
        n += uui_sndformat_focusables(&g_fmt, g_focusables + n, UUI_SNDFORMAT_IDS);
    if (!item_of(&g_net_clip)->hidden)
        n += uui_netadapter_focusables(&g_net, g_focusables + n, UUI_NETADAPTER_IDS);
    if (!item_of(&g_mods)->hidden) {
        g_focusables[n++] = (struct uui_focusable){ &g_mods, &uui_dropdown_ops };
        g_focusables[n++] = (struct uui_focusable){ &g_load, &uui_button_ops };
    }
    if (!item_of(&g_unload)->hidden)
        g_focusables[n++] = (struct uui_focusable){ &g_unload, &uui_button_ops };
    g_focusables[n++] = (struct uui_focusable){ &g_keep, &uui_checkbox_ops };
    g_focusables[n++] = (struct uui_focusable){ &g_refresh, &uui_button_ops };
    g_focusables[n++] = (struct uui_focusable){ &g_toggle, &uui_button_ops };
    int cur = g_focus.current;
    uui_focus_init(&g_focus, g_focusables, n);
    g_focus.current = cur < n ? cur : -1;
}

// Is the selected device a sound card? Then its Format is shown.
static void update_format(const struct udevice *d) {
    struct query_sound q;
    int found = 0;
    if (d) QUERY_FOREACH(QUERY_SOUND, q, qi)
        if (!strcmp(q.device_id, d->id)) { found = 1; break; }
    if (found && strcmp(g_fmt_card, q.name) != 0) {
        uui_sndformat_load(&g_fmt, &q);
        strlcpy(g_fmt_card, q.name, sizeof g_fmt_card);
    }
    if (!found) g_fmt_card[0] = 0;
}

// Is the selected device a network card the stack knows? Then its
// adapter settings are shown -- a card with none says so in the panel.
static void update_net(const struct udevice *d) {
    struct query_netdev q;
    int found = 0;
    if (d && d->id[0]) QUERY_FOREACH(QUERY_NETDEV, q, qi)
        if (!strcmp(q.device_id, d->id)) { found = 1; break; }
    if (found && q.mac != g_net_mac) {
        uui_netadapter_load(&g_net, &q);
        g_net_mac = q.mac;
    }
    if (!found) g_net_mac = 0;
}

// --- the filter --------------------------------------------------------

static int dev_matches(const struct udevice *d, const char *q) {
    return uui_tree_text_matches(d->name, q) || uui_tree_text_matches(d->id, q) ||
           uui_tree_text_matches(d->driver, q) || uui_tree_text_matches(d->vendor_name, q) ||
           uui_tree_text_matches(d->location, q) || uui_tree_text_matches(d->devname, q);
}

static int node_matches(void *ctx, int node, const char *q) {
    (void)ctx;
    int id = g_nodes[node].id;
    if (id >= 0 && id < g_n) return dev_matches(&g_dev[id], q);
    return uui_tree_text_matches(g_nodes[node].label, q);
}

static const char *filter_text(void) { return uui_textbox_text(&g_filter); }

// --- the tree ------------------------------------------------------------

// A heading's label with its member count after it -- "Sound (2)", as
// the mockup and Windows' collapsed categories show; "(1 of 2)" while a
// filter hides some. Storage for the tree, which borrows every label;
// reset on each rebuild.
static char g_head[UDEV_T_COUNT + 4][72];
static int g_head_n;

static const char *head(const char *name, int count, int shown) {
    if (g_head_n >= (int)(sizeof g_head / sizeof g_head[0])) return name;
    if (filter_text()[0] && shown != count)
        snprintf(g_head[g_head_n], sizeof g_head[0], "%s (%d of %d)", name, shown, count);
    else
        snprintf(g_head[g_head_n], sizeof g_head[0], "%s (%d)", name, count);
    return g_head[g_head_n++];
}

static int shown(int i) { return !filter_text()[0] || dev_matches(&g_dev[i], filter_text()); }

static void label_device(int i) {
    strlcpy(g_label[i], g_dev[i].name, sizeof g_label[i]);
}

static void add_node(const char *label, int depth, int id, const char *icon) {
    if (g_node_count >= (int)(sizeof g_nodes / sizeof g_nodes[0])) return;
    g_nodes[g_node_count++] = (struct uui_tree_node){ label, depth, id, UUI_TREE_AUTO, icon, 0, 0, 0, 0, 0 };
}

// The state is a BADGE on the icon, Windows' shape: a yellow "!" for a
// device with no driver, a down-arrow for one switched off.
static void add_device(int i, int depth) {
    add_node(g_label[i], depth, i, udevice_type_icon(g_dev[i].type));
    const struct udevice *d = &g_dev[i];
    g_nodes[g_node_count - 1].badge = d->disabled ? "badge-disabled"
                                    : d->problem ? "badge-warning" : 0;
}

// Every node above a device with a problem is OPENED on each rebuild, as
// Windows' Device Manager does, so the "!" is on screen without a click.
static void open_problems(void) {
    for (int k = 0; k < g_node_count; k++) {
        if (!g_nodes[k].badge || strcmp(g_nodes[k].badge, "badge-warning")) continue;
        for (int j = k - 1, depth = g_nodes[k].depth; j >= 0 && depth > 0; j--)
            if (g_nodes[j].depth < depth) {
                uui_tree_set_collapsed(&g_tree, j, 0);
                depth = g_nodes[j].depth;
            }
    }
}

// Windows' default: a heading per type that has any devices.
static void build_by_type(void) {
    for (int t = 0; t < UDEV_T_COUNT; t++) {
        int any = 0, vis = 0;
        for (int i = 0; i < g_n; i++)
            if ((int)g_dev[i].type == t) { any++; vis += shown(i); }
        if (!any) continue;
        add_node(head(udevice_type_name(t), any, vis), 0, NODE_CAT(t), udevice_type_icon(t));
        for (int i = 0; i < g_n; i++)
            if ((int)g_dev[i].type == t) add_device(i, 1);
    }
}

// A device and what hangs off it, depth first: a controller's USB
// devices and disks, a display adapter's monitor.
static void add_subtree(int i, int depth) {
    add_device(i, depth);
    for (int j = 0; j < g_n; j++)
        if (g_dev[j].parent == i && g_dev[j].bus != UDEV_PCI) add_subtree(j, depth + 1);
}

// As the hardware is wired: the PCI bus, with each USB device, disk and
// monitor under what it hangs off; then the platform devices; then what
// hangs off nothing known; then the CPUs.
static void build_by_connection(void) {
    int npci = 0, ncpu = 0, vpci = 0, vcpu = 0;
    for (int i = 0; i < g_n; i++) {
        if (g_dev[i].bus == UDEV_PCI) { npci++; vpci += shown(i); }
        if (g_dev[i].bus == UDEV_CPU) { ncpu++; vcpu += shown(i); }
    }
    add_node(head("PCI bus", npci, vpci), 0, NODE_BUS_PCI, "cat-system");
    for (int i = 0; i < g_n; i++)
        if (g_dev[i].bus == UDEV_PCI) add_subtree(i, 1);
    int plat = 0, vplat = 0;
    for (int i = 0; i < g_n; i++)
        if (g_dev[i].bus == UDEV_PLATFORM || (g_dev[i].parent < 0 && (g_dev[i].bus == UDEV_BLOCK ||
            g_dev[i].bus == UDEV_MONITOR || g_dev[i].bus == UDEV_USB))) { plat++; vplat += shown(i); }
    if (plat) {
        add_node(head("Platform devices", plat, vplat), 0, NODE_BUS_PLAT, "cat-system");
        for (int i = 0; i < g_n; i++)
            if (g_dev[i].bus == UDEV_PLATFORM) add_device(i, 1);
        for (int i = 0; i < g_n; i++)
            if (g_dev[i].parent < 0 && (g_dev[i].bus == UDEV_BLOCK || g_dev[i].bus == UDEV_MONITOR ||
                                        g_dev[i].bus == UDEV_USB)) add_subtree(i, 1);
    }
    add_node(head(udevice_type_name(UDEV_T_CPU), ncpu, vcpu), 0, NODE_CAT(UDEV_T_CPU),
             udevice_type_icon(UDEV_T_CPU));
    for (int i = 0; i < g_n; i++)
        if (g_dev[i].bus == UDEV_CPU) add_device(i, 1);
}

static const struct udevice *selected(void) {
    int id = uui_tree_selected_id(&g_tree);
    return id >= 0 && id < g_n ? &g_dev[id] : 0;
}

static const struct udevice *find_id(const char *id) {
    for (int i = 0; id[0] && i < g_n; i++)
        if (!strcmp(g_dev[i].id, id)) return &g_dev[i];
    return 0;
}

// Rebuilds the nodes and keeps the selection on the same DEVICE (by its
// stable id), since the index of a device can change with a relist.
static void rebuild_tree(const char *keep_id) {
    g_node_count = 0;
    g_head_n = 0;
    for (int i = 0; i < g_n; i++) label_device(i);
    if (g_by_conn) build_by_connection(); else build_by_type();
    uui_tree_set_nodes_keep(&g_tree, g_nodes, g_node_count);
    uui_tree_set_filter(&g_tree, filter_text(), node_matches, 0);
    open_problems();
    if (keep_id && keep_id[0])
        for (int i = 0; i < g_n; i++)
            if (!strcmp(g_dev[i].id, keep_id) && shown(i)) { uui_tree_select_id(&g_tree, i); return; }
    // Nothing to keep: the first DEVICE showing, so the pane has something to say.
    for (int n = 0; n < g_node_count; n++)
        if (g_nodes[n].id >= 0 && shown(g_nodes[n].id)) { uui_tree_select_id(&g_tree, g_nodes[n].id); return; }
}

// --- the pane ----------------------------------------------------------------

// The modules that could drive `d`, into the dropdown -- a match first,
// marked as one.
static void load_modules(const struct udevice *d) {
    int m = 0;
    g_nmods = udevice_modules(d, g_mod_name, 8, &m);
    for (int k = 0; k < g_nmods; k++) {
        snprintf(g_mod_label[k], sizeof g_mod_label[k], "%s%s", g_mod_name[k],
                 k < m ? " (matches this device)" : "");
        g_mod_items[k] = g_mod_label[k];
    }
    uui_dropdown_set_items(&g_mods, g_mod_items, g_nmods);
    uui_dropdown_set_selected(&g_mods, 0);
}

// Fills the pane from udevice_props(): "Problem" becomes the notice box,
// and the Format and the driver's buttons get slots in their sections.
static void build_props(const struct udevice *d) {
    int same = d && !strcmp(g_props_for, d->id);
    uui_props_begin(&g_props, same);
    strlcpy(g_props_for, d ? d->id : "", sizeof g_props_for);
    g_drv_mode = 0;
    if (!d) return;
    int idx = (int)(d - g_dev);
    int np = udevice_props(g_dev, g_n, idx, UDEV_PROPS_ALL, g_p, UDEV_PROPS_MAX);
    int ctrl = utheme_control_h(), pad = utheme_pad();

    if (d->bus == UDEV_PCI && !d->driver[0] && !d->holder_pid && !d->disabled) {
        load_modules(d);
        if (g_nmods) g_drv_mode = 1;
    } else if (d->module[0]) {
        snprintf(g_unload_label, sizeof g_unload_label, "Unload module %s", d->module);
        g_unload.label = g_unload_label;
        g_drv_mode = 2;
    }

    const char *cur = 0;
    int fmt_done = !g_fmt_card[0], net_done = !g_net_mac;
    for (int k = 0; k < np; k++) {
        if (!cur || strcmp(cur, g_p[k].section) != 0) {
            cur = g_p[k].section;
            int notice = !strcmp(cur, "Problem");
            // THE FORMAT FIRST, for a sound card -- after a problem, before
            // the facts: it is the one thing here a person changes.
            if (!notice && !fmt_done) {
                int w, h;
                uui_layout_natural_size(&g_fmt.col, &w, &h);
                uui_props_slot(&g_props, uui_props_section(&g_props, "Format", UUI_PROPS_PLAIN),
                               SLOT_FORMAT, h);
                fmt_done = 1;
            }
            if (!notice && !net_done) {
                int w, h;
                uui_layout_natural_size(&g_net.col, &w, &h);
                uui_props_slot(&g_props, uui_props_section(&g_props, "Adapter settings", UUI_PROPS_PLAIN),
                               SLOT_NET, h);
                net_done = 1;
            }
            int sec = uui_props_section(&g_props, cur, notice ? UUI_PROPS_NOTICE : UUI_PROPS_PLAIN);
            if (!strcmp(cur, "Device")) uui_props_set_action(&g_props, sec, "Copy details");
            if (!strcmp(cur, "Events")) uui_props_set_action(&g_props, sec, "Open Log Viewer");
            if (!strcmp(cur, "Driver") && g_drv_mode) uui_props_slot(&g_props, sec, SLOT_DRIVER, ctrl + pad);
        }
        uui_props_row(&g_props, g_p[k].key, "%s", g_p[k].val);
    }
}

static void update_controls(void) {
    const struct udevice *d = selected();
    int can = d && d->can_disable && !d->holder_pid;
    g_toggle.label = d && d->disabled ? "Enable device" : "Disable device";
    g_toggle.disabled = !can;
    g_keep.disabled = !can;
    g_keep.checked = d ? d->persisted : 0;
    update_format(d);
    update_net(d);
    build_props(d);

    int problems = 0, off = 0;
    for (int i = 0; i < g_n; i++) { problems += g_dev[i].problem; off += g_dev[i].disabled; }
    snprintf(g_st_count, sizeof g_st_count, "%d devices", g_n);
    snprintf(g_st_problem, sizeof g_st_problem, "%d without a driver, %d disabled", problems, off);
}

static void relist(void) {
    const struct udevice *d = selected();
    char keep[24] = "";
    if (d) strlcpy(keep, d->id, sizeof keep);
    g_n = udevice_list(g_dev, UDEV_MAX);
    rebuild_tree(keep);
    update_controls();
}

// What would change if a device were bound, unbound or claimed: every
// driver name and holder, the USB slots, and how many device events
// there have been (a disk or a module comes and goes with one). Cheap --
// no database.
static unsigned signature(void) {
    unsigned h = 2166136261u;
    struct query_pcidev p;
    QUERY_FOREACH(QUERY_PCIDEV, p, i) {
        h = (h ^ (unsigned)p.holder_pid) * 16777619u;
        for (const char *c = p.driver; *c; c++) h = (h ^ (unsigned char)*c) * 16777619u;
    }
    struct query_usb u;
    QUERY_FOREACH(QUERY_USB, u, i) {
        h = (h ^ (unsigned)u.slot) * 16777619u;
        for (const char *c = u.driver; *c; c++) h = (h ^ (unsigned char)*c) * 16777619u;
    }
    struct query_devevent e;
    uint32_t last = 0;
    QUERY_FOREACH(QUERY_DEVEVENT, e, i) last = e.seq;
    h = (h ^ last) * 16777619u;
    return h;
}
static unsigned g_sig;

// --- disable / enable, load / unload ------------------------------------------

static void set_note(const char *msg) { strlcpy(g_st_note, msg, sizeof g_st_note); }

static void report(const struct udevice *d, int r, const char *verb) {
    char msg[96];
    if (r == 0) snprintf(msg, sizeof msg, "%s %s", verb, d->name);
    else if (r == -EBUSY) snprintf(msg, sizeof msg, "%s is in use by a program", d->name);
    else if (r == -ENOTSUP) snprintf(msg, sizeof msg, "%s's driver does not support disabling", d->name);
    else snprintf(msg, sizeof msg, "could not change %s (error %d)", d->name, -r);
    set_note(msg);
    ulogf("devmgr: %s %s -> %d\n", verb, d->id, r);
}

static void ask(struct uapp *a, const char *title, const char *button, int code) {
    g_ask_rows[0] = g_ask_line[0];
    g_ask_rows[1] = g_ask_line[1];
    static struct uui_dialog_button btns[2];
    btns[0] = (struct uui_dialog_button){ button, code, UUI_DLG_DANGER };
    btns[1] = (struct uui_dialog_button){ "Cancel", ASK_CANCEL, 0 };
    uui_dialog_set_bounds(&g_ask, 0, 0, uapp_width(a), uapp_height(a));
    uui_dialog_open(&g_ask, title, g_ask_rows, 2, btns, 2, 1, ASK_CANCEL);
}

static void ask_disable(struct uapp *a) {
    const struct udevice *d = selected();
    if (!d || !d->can_disable || uui_dialog_is_open(&g_ask)) return;
    strlcpy(g_ask_id, d->id, sizeof g_ask_id);
    snprintf(g_ask_line[0], sizeof g_ask_line[0], "Disable %s?", d->name);
    snprintf(g_ask_line[1], sizeof g_ask_line[1], "%s",
             g_keep.checked ? "It stays disabled after a restart, until you enable it."
                            : "It stays disabled until you enable it or restart.");
    ask(a, "Disable device", "Disable", ASK_DISABLE);
}

static void toggle(struct uapp *a) {
    const struct udevice *d = selected();
    if (!d || !d->can_disable) return;
    if (d->disabled) {
        report(d, udevice_enable(d), "enabled");
        relist();
        g_sig = signature();
    } else {
        ask_disable(a);
    }
}

// The keep box on a device ALREADY disabled changes only whether that
// survives a restart; on a working one it is read when Disable commits.
static void keep_changed(void) {
    const struct udevice *d = selected();
    if (!d || !d->disabled) return;
    report(d, udevice_disable(d, g_keep.checked),
           g_keep.checked ? "kept disabled across restarts:" : "disabled until restart:");
    relist();
}

// LOADING A MODULE is `modload`: the kernel links it and offers it every
// device no driver holds, this one included (kernel/core/module.c).
static void load_driver(void) {
    const struct udevice *d = selected();
    int k = uui_dropdown_selected(&g_mods);
    if (!d || g_drv_mode != 1 || k < 0 || k >= g_nmods) return;
    char path[64], msg[96];
    snprintf(path, sizeof path, "/lib/modules/%s.ko", g_mod_name[k]);
    if (sys_modload(path) < 0) {
        snprintf(msg, sizeof msg, "could not load %s: %s", g_mod_name[k], strerror(sys_errno()));
    } else {
        char id[24];
        strlcpy(id, d->id, sizeof id);
        relist();
        const struct udevice *now = find_id(id);
        snprintf(msg, sizeof msg, "loaded %s -- %s", g_mod_name[k],
                 now && now->driver[0] ? "it drives this device" : "it did not take this device");
    }
    set_note(msg);
    ulogf("devmgr: load %s -> %s\n", g_mod_name[k], msg);
    g_sig = signature();
}

// UNLOADING TAKES THE MODULE AWAY FROM EVERY DEVICE IT DRIVES, so it asks.
static void ask_unload(struct uapp *a) {
    const struct udevice *d = selected();
    if (!d || !d->module[0] || uui_dialog_is_open(&g_ask)) return;
    strlcpy(g_ask_module, d->module, sizeof g_ask_module);
    snprintf(g_ask_line[0], sizeof g_ask_line[0], "Unload the module %s?", d->module);
    snprintf(g_ask_line[1], sizeof g_ask_line[1],
             "Every device it drives is left without a driver until it is loaded again.");
    ask(a, "Unload driver", "Unload", ASK_UNLOAD);
}

static void unload_driver(void) {
    char msg[96];
    if (sys_modunload(g_ask_module) < 0)
        snprintf(msg, sizeof msg, "could not unload %s: %s", g_ask_module, strerror(sys_errno()));
    else
        snprintf(msg, sizeof msg, "unloaded %s", g_ask_module);
    set_note(msg);
    ulogf("devmgr: unload %s -> %s\n", g_ask_module, msg);
    g_ask_module[0] = 0;
    relist();
    g_sig = signature();
}

// --- copy, export, the log ------------------------------------------------------

static void copy_details(void) {
    const struct udevice *d = selected();
    if (!d) return;
    static char text[8192];
    int np = udevice_props(g_dev, g_n, (int)(d - g_dev), UDEV_PROPS_ALL, g_p, UDEV_PROPS_MAX);
    char title[160], st[64];
    snprintf(title, sizeof title, "%s [%s] -- %s", d->name, d->id, udevice_status(d, st, sizeof st));
    int n = udevice_props_text(g_p, np, title, text, sizeof text);
    int ok = n > 0 && uclip_set_text(text, n);
    set_note(ok ? "copied the device's details" : "the details did not fit on the clipboard");
    ulogf("devmgr: copy %s %d bytes %s\n", d->id, n, ok ? "ok" : "refused");
    build_props(d);   // udevice_props() wrote into the pane's own buffer
}

struct sink { FILE *f; int lines; };
static void put_line(void *ctx, const char *line) {
    struct sink *s = ctx;
    fprintf(s->f, "%s\n", line);
    s->lines++;
}

static void export_chosen(void *ctx, const char *path) {
    struct uapp *a = ctx;
    if (!path) return;
    FILE *f = fopen(path, "w");
    char msg[96];
    if (!f) {
        snprintf(msg, sizeof msg, "could not write %s: %s", path, strerror(errno));
    } else {
        struct sink s = { f, 0 };
        udevice_report(g_dev, g_n, UDEV_PROPS_RESOURCES | UDEV_PROPS_EVENTS, put_line, &s);
        int bad = fclose(f) != 0;
        snprintf(msg, sizeof msg, bad ? "could not finish %s" : "saved the hardware report to %s", path);
    }
    set_note(msg);
    ulogf("devmgr: export %s\n", msg);
    const struct udevice *d = selected();
    build_props(d);   // the report used the pane's property buffer
    uapp_redraw(a);
}

static void export_report(struct uapp *a) {
    if (uui_filedialog_is_open(&g_save)) return;
    struct uui_filedialog_opts o = {
        .mode = UUI_FILEDIALOG_SAVE,
        .title = "Export hardware report",
        .start_dir = "/home",
        .initial_name = "hardware-report.txt",
    };
    if (!uui_filedialog_open(a, &g_save, &o, export_chosen, a))
        set_note("cannot open the chooser");
}

// --- layout and drawing ---------------------------------------------------

static void layout_all(int cw, int ch) {
    int mb = uui_menubar_height(&g_menu), sb = uui_statusbar_height(&g_status);
    int pad = utheme_pad(), bt = utheme_control_h(), chh = ugfx_char_h();
    int top = mb, bottom = ch - sb;

    uui_menubar_set_geometry(&g_menu, 0, 0, cw, mb);
    uui_menubar_set_bounds(&g_menu, 0, 0, cw, ch);
    uui_statusbar_set_geometry(&g_status, 0, bottom, cw, sb);

    int per = ugfx_char_advance('n');
    if (per <= 0) per = 8;
    uui_splitter_set_track(&g_split, 0, cw, per * 18, per * 30);
    int left = uui_splitter_before(&g_split), bar = uui_splitter_thickness();
    uui_splitter_set_geometry(&g_split, uui_splitter_pos(&g_split), top, bar, bottom - top);

    uui_segmented_set_geometry(&g_view, pad, top + pad);
    int fy = top + pad + g_view.h + pad, fh;
    uui_textbox_natural_size(&g_filter, 0, &fh);
    uui_textbox_set_geometry(&g_filter, pad, fy, left - 2 * pad, fh);
    int ty = fy + fh + pad;
    g_tree.x = 0; g_tree.y = ty; g_tree.w = left; g_tree.h = bottom - ty;

    g_px = left + bar;
    g_py = top;
    g_pw = cw - g_px;
    g_ph = bottom - top;

    // The controls along the pane's bottom edge, right-aligned as a
    // dialog's are; the keep box to their left.
    int bw, bh;
    uui_button_natural_size(&g_toggle, &bw, &bh);
    int tw = bw > per * 15 ? bw : per * 15;
    int rw;
    uui_button_natural_size(&g_refresh, &rw, &bh);
    int by = bottom - pad - bt;
    uui_button_set_geometry(&g_toggle, cw - pad - tw, by, tw, bt);
    uui_button_set_geometry(&g_refresh, cw - pad - tw - pad - rw, by, rw, bt);
    uui_checkbox_set_geometry(&g_keep, g_px + pad, by + (bt - chh) / 2);

    // The header: the name, its type, and the status box under them.
    g_head_h = 2 * pad + chh + pad / 2 + chh + pad + chh + pad + pad;
    uui_props_set_geometry(&g_props, g_px, g_py + g_head_h, g_pw, by - pad - (g_py + g_head_h));

    uui_dialog_set_bounds(&g_ask, 0, 0, cw, ch);
}

// Places a panel at its slot wherever the scroll has put it, cut to the
// pane by its clip; hidden only when none of it is in view. A top-level
// layout takes the window-edge margin and this one sits in the pane's
// own, so it is placed that far out to line up.
static int place_panel(struct uui_clip *c, struct uui_layout *col, int slot) {
    int x, y, w, h;
    if (!uui_props_slot_place(&g_props, slot, &x, &y, &w, &h)) return 0;
    int m = uui_layout_margin(col);
    uui_layout_run(col, x - m, y - m, w + 2 * m, h + m);
    c->x = x; c->y = y; c->w = w; c->h = h;
    uui_clip_set_viewport(c, g_props.x, g_props.y, g_props.w, g_props.h);
    return uui_clip_visible(c);
}

// THE SLOTS' CONTROLS FOLLOW THE PANE'S SCROLL. The two panels scroll
// with it, clipped; the driver's buttons, one row, are shown only where
// wholly in view. The focus ring is rebuilt when what shows changes.
static void place_slot_controls(void) {
    int x, y, w, h, changed = 0;
    struct uui_item *fmt = item_of(&g_fmt_clip);
    int fmt_on = g_fmt_card[0] && place_panel(&g_fmt_clip, &g_fmt.col, SLOT_FORMAT);
    if (fmt->hidden == fmt_on) { fmt->hidden = !fmt_on; changed = 1; }
    // The adapter panel's rows wrap with the pane's width, which is known
    // only now: a fit that changes a row's height re-sizes its slot,
    // once -- the width does not move, so the second pass settles.
    struct uui_item *net = item_of(&g_net_clip);
    int net_on = 0;
    for (int pass = 0; pass < 2; pass++) {
        net_on = g_net_mac && place_panel(&g_net_clip, &g_net.col, SLOT_NET);
        if (!g_net_mac || !uui_netadapter_fit(&g_net) || pass) break;
        build_props(selected());
    }
    if (net->hidden == net_on) { net->hidden = !net_on; changed = 1; }

    int drv = uui_props_slot_rect(&g_props, SLOT_DRIVER, &x, &y, &w, &h);
    int load_on = drv && g_drv_mode == 1, unload_on = drv && g_drv_mode == 2;
    struct uui_item *mods = item_of(&g_mods), *load = item_of(&g_load), *unl = item_of(&g_unload);
    if (mods->hidden == load_on) { mods->hidden = load->hidden = !load_on; changed = 1; }
    if (unl->hidden == unload_on) { unl->hidden = !unload_on; changed = 1; }
    int bt = utheme_control_h(), pad = utheme_pad();
    if (load_on) {
        int lw, lh;
        uui_button_natural_size(&g_load, &lw, &lh);
        int dw = w - lw - pad;
        int per = ugfx_char_advance('n') > 0 ? ugfx_char_advance('n') : 8;
        if (dw > per * 34) dw = per * 34;
        uui_dropdown_set_geometry(&g_mods, x, y, dw, bt);
        uui_button_set_geometry(&g_load, x + dw + pad, y, lw, bt);
    }
    if (unload_on) {
        int uw, uh;
        uui_button_natural_size(&g_unload, &uw, &uh);
        uui_button_set_geometry(&g_unload, x, y, uw, bt);
    }
    if (changed) build_focus();
}

static void draw_header(struct ugfx_surface *s) {
    const struct utheme *t = utheme_current();
    int pad = utheme_pad(), ch = ugfx_char_h();
    ugfx_fill_rect(s, g_px, g_py, g_pw, g_head_h, t->panel_bg);
    // The bottom strip under the props, where the buttons sit.
    int bottom = g_props.y + g_props.h;
    ugfx_fill_rect(s, g_px, bottom, g_pw, g_py + g_ph - bottom, t->panel_bg);
    ugfx_fill_rect(s, g_px + 2 * pad, g_py + g_head_h - 1, g_pw - 4 * pad, 1, t->separator);

    const struct udevice *d = selected();
    int x = g_px + 2 * pad, y = g_py + 2 * pad, w = g_pw - 4 * pad;
    if (w <= 0) return;
    if (!d) {
        int id = uui_tree_selected_id(&g_tree), count = 0;
        const char *what = filter_text()[0] ? "No device matches the filter."
                                            : "Select a device to see its properties.";
        char buf[64];
        if (id <= NODE_CAT(0) && id > NODE_CAT(UDEV_T_COUNT)) {
            enum udev_type ty = (enum udev_type)(NODE_CAT(0) - id);
            for (int i = 0; i < g_n; i++) count += g_dev[i].type == ty;
            snprintf(buf, sizeof buf, "%s: %d device%s", udevice_type_name(ty), count, count == 1 ? "" : "s");
            what = buf;
        }
        ugfx_draw_string_clipped(s, x, y, w, what, t->text, t->panel_bg);
        return;
    }

    // The name, and its type under it.
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    ugfx_draw_string_elided(s, x, y, w, d->name, t->text, t->panel_bg);
    ugfx_set_font(was);
    y += ch + pad / 2;
    ugfx_draw_string_clipped(s, x, y, w, udevice_type_name(d->type), t->outline, t->panel_bg);
    y += ch + pad;

    // The status, in a box: a dot says it at a glance, in a colour that
    // also differs in lightness (green, amber, grey).
    char st[96];
    udevice_status(d, st, sizeof st);
    int bh = ch + pad;
    ugfx_fill_rect(s, x, y, w, bh, t->field_bg);
    ugfx_draw_rect(s, x, y, w, bh, t->outline);
    uint32_t dot = d->disabled ? ugfx_rgb(130, 130, 136)
                 : d->problem || d->holder_pid ? ugfx_rgb(191, 110, 0)
                 : ugfx_rgb(46, 125, 50);
    int r = ch / 4 > 2 ? ch / 4 : 2;
    ugfx_fill_circle(s, x + pad + r, y + bh / 2, r, dot);
    ugfx_draw_string_clipped(s, x + 2 * pad + 2 * r, y + (bh - ch) / 2, w - 3 * pad - 2 * r, st,
                             t->text, t->field_bg);
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    layout_all(d->surface->w, d->surface->h);
    place_slot_controls();
    draw_header(d->surface);
    uapp_log_layout(a, "devmgr");
    const struct udevice *sel = selected();
    uapp_logf_layout("devmgr: selected %s view %s format %d\n", sel ? sel->id : "-",
                     g_by_conn ? "connection" : "type", !item_of(&g_fmt_clip)->hidden);
    uapp_logf_layout("devmgr: pane sections %d rows %d scroll %d driver %d filter \"%s\" rows %d\n",
                     g_props.nsec, g_props.nrow, g_props.scroll, g_drv_mode, filter_text(),
                     uui_tree_visible_count(&g_tree));
}

// --- commands and input -------------------------------------------------

static void set_view(struct uapp *a, int by_conn) {
    g_by_conn = by_conn;
    g_view.selected = by_conn;
    const struct udevice *d = selected();
    char keep[24] = "";
    if (d) strlcpy(keep, d->id, sizeof keep);
    rebuild_tree(keep);
    update_controls();
    uapp_redraw(a);
}

static void filter_changed(void) {
    const struct udevice *d = selected();
    char keep[24] = "";
    if (d) strlcpy(keep, d->id, sizeof keep);
    rebuild_tree(keep);
    int vis = 0;
    for (int i = 0; i < g_n; i++) vis += shown(i);
    if (filter_text()[0]) snprintf(g_st_note, sizeof g_st_note, "Filter: %d of %d shown", vis, g_n);
    else g_st_note[0] = 0;
    update_controls();
}

static void focus_filter(void) {
    for (int i = 0; i < g_focus.count; i++)
        if (g_focus.items[i].widget == &g_filter) { uui_focus_set(&g_focus, i); return; }
}

static void do_command(struct uapp *a, int code) {
    switch (code) {
    case CMD_REFRESH:  relist(); g_sig = signature(); set_note("refreshed"); break;
    case CMD_EXIT:     uapp_quit(a, 0); return;
    case CMD_BY_TYPE:  set_view(a, 0); return;
    case CMD_BY_CONN:  set_view(a, 1); return;
    case CMD_EXPAND:   uui_tree_expand_all(&g_tree); break;
    case CMD_COLLAPSE: uui_tree_collapse_all(&g_tree); break;
    case CMD_TOGGLE:   toggle(a); break;
    case CMD_COPY:     copy_details(); uapp_redraw(a); return;
    case CMD_EXPORT:   export_report(a); uapp_redraw(a); return;
    case CMD_FIND:     focus_filter(); uapp_redraw(a); return;
    case CMD_LOAD:
        if (g_drv_mode == 1) load_driver();
        else set_note("this device has a driver, or no module could drive it");
        break;
    case CMD_UNLOAD:
        if (g_drv_mode == 2) { ask_unload(a); uapp_redraw(a); return; }
        set_note(selected() && selected()->driver[0] ? "its driver is built into the kernel"
                                                     : "this device has no driver to unload");
        break;
    default: return;
    }
    update_controls();
    uapp_redraw(a);
}

static void on_action(struct uapp *a, int code) {
    if (uui_netadapter_on_action(&g_net, code)) {
        uapp_logf_layout("devmgr: netadapter %s reset: %s\n", g_net.d.name, g_net.note);
        build_props(selected());
        uapp_redraw(a);
        return;
    }
    switch (code) {
    case ID_REFRESH: do_command(a, CMD_REFRESH); return;
    case ID_LOAD:    do_command(a, CMD_LOAD); return;
    case ID_UNLOAD:  do_command(a, CMD_UNLOAD); return;
    case ID_TOGGLE:
        toggle(a);
        update_controls();
        uapp_redraw(a);
        return;
    }
}

static void on_widget(struct uapp *a, int id, int reason) {
    if (id >= ID_NETADP && id < ID_NETADP + UUI_NETADAPTER_IDS) {
        if ((reason == UUI_REASON_RELEASE || reason == UUI_REASON_KEY) &&
            uui_netadapter_on_widget(&g_net, id)) {
            uapp_logf_layout("devmgr: netadapter %s rates %#llx eee %llu flow %llu moderation %llu: %s\n",
                             g_net.d.name, (unsigned long long)g_net.d.rates,
                             (unsigned long long)g_net.d.eee, (unsigned long long)g_net.d.flow,
                             (unsigned long long)g_net.d.moderation, g_net.note);
            build_props(selected());
            build_focus();
        }
        uapp_redraw(a);
        return;
    }
    if (id >= ID_SNDFMT && id < ID_SNDFMT + UUI_SNDFORMAT_IDS) {
        if ((reason == UUI_REASON_RELEASE || reason == UUI_REASON_KEY) &&
            uui_sndformat_on_widget(&g_fmt, id)) {
            uapp_logf_layout("devmgr: sndfmt %s match %d fixed %u allowed %#x bits %u\n",
                             g_fmt.card, g_fmt.f.match, (unsigned)g_fmt.f.fixed,
                             (unsigned)g_fmt.f.allowed, (unsigned)g_fmt.f.bits);
            build_focus();
        }
        uapp_redraw(a);
        return;
    }
    switch (id) {
    case ID_MENU: {
        int code = uui_menubar_take_code(&g_menu);   // parked in the widget
        if (code > 0) do_command(a, code);
        else uapp_redraw(a);
        return;
    }
    case ID_ASK: {
        int code = uui_dialog_take_code(&g_ask);     // -1 on every press
        if (code < 0) return;
        if (code == ASK_UNLOAD) {
            unload_driver();
        } else {
            const struct udevice *d = code == ASK_DISABLE ? find_id(g_ask_id) : 0;
            if (d && d->can_disable) {
                report(d, udevice_disable(d, g_keep.checked), "disabled");
                relist();
                g_sig = signature();
            }
        }
        g_ask_id[0] = 0;
        update_controls();
        uapp_redraw(a);
        return;
    }
    case ID_VIEW:
        if (g_view.selected != g_by_conn) set_view(a, g_view.selected);
        return;
    case ID_FILTER:
        filter_changed();
        uapp_redraw(a);
        return;
    case ID_PROPS: {
        int s = uui_props_take_action(&g_props);
        if (s >= 0) {
            if (!strcmp(g_props.sec[s].title, "Device")) copy_details();
            else if (!strcmp(g_props.sec[s].title, "Events")) uapp_spawn(a, "/bin/wm/apps/logview", 0);
        }
        uapp_redraw(a);
        return;
    }
    }
    if (reason != UUI_REASON_RELEASE && reason != UUI_REASON_KEY) {
        if (id == ID_TREE) { update_controls(); uapp_redraw(a); }
        return;
    }
    switch (id) {
    case ID_KEEP:    keep_changed(); break;
    case ID_TREE:    break;
    case ID_MODS:    uapp_redraw(a); return;
    default: return;
    }
    update_controls();
    uapp_redraw(a);
}

// Ctrl+letter arrives as its control code (as in Notepad).
static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    int code;
    if (uui_menubar_key(&g_menu, key, &code)) {
        if (code >= 0) do_command(a, code);
        else uapp_redraw(a);
        return;
    }
    switch (key) {
    case 0x06:   do_command(a, CMD_FIND); return;    // Ctrl+F
    case 0x03:   do_command(a, CMD_COPY); return;    // Ctrl+C
    case 0x05:   do_command(a, CMD_EXPORT); return;  // Ctrl+E
    case KEY_F5: do_command(a, CMD_REFRESH); return;
    case 0x1B:                                        // Esc empties the filter
        if (filter_text()[0]) {
            uui_textbox_set_text(&g_filter, "");
            filter_changed();
            uapp_redraw(a);
            return;
        }
        break;
    }
    // The ring already moved the tree's selection; the pane follows it.
    update_controls();
    uapp_redraw(a);
}

static int on_tick(struct uapp *a) {
    (void)a;
    unsigned s = signature();
    if (s == g_sig) return 0;
    g_sig = s;
    relist();
    return 1;
}

// LAYOUT FIRST: selecting a row scrolls it into view, and a tree with no
// height yet has no view -- the first row would scroll off the top.
static void on_open(struct uapp *a) {
    layout_all(uapp_width(a), uapp_height(a));
    relist();
    g_sig = signature();
}

static void on_resize(struct uapp *a, int w, int h) {
    (void)a;
    layout_all(w, h);
}

static void on_size(int *w, int *h) {
    int per = ugfx_char_advance('n');
    if (per <= 0) per = 8;
    *w = per * 110;
    *h = ugfx_char_h() * 38;
}

int main(void) {
    uui_menubar_init(&g_menu, menu_items, (int)(sizeof menu_items / sizeof menu_items[0]));
    uui_segmented_init(&g_view, VIEWS, 2, 0);
    uui_textbox_init(&g_filter, "");
    g_filter.placeholder = "Find a device";
    uui_tree_init(&g_tree, 0, 0, 0, 0, g_nodes, 0);
    g_tree.sel_style = UUI_SEL_STRONG;
    uui_splitter_init(&g_split, 1, 430);
    uui_props_init(&g_props);
    uui_button_init(&g_toggle, 0, 0, 0, 0, "Disable device", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_TOGGLE);
    uui_button_init(&g_refresh, 0, 0, 0, 0, "Refresh", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_REFRESH);
    uui_button_init(&g_load, 0, 0, 0, 0, "Load driver", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_LOAD);
    uui_button_init(&g_unload, 0, 0, 0, 0, "Unload driver", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_UNLOAD);
    uui_dropdown_init(&g_mods, 0, 0, 0, 0, g_mod_items, 0);
    // The mockup's look: outlined buttons, and the selected device in
    // the accent -- both opt-in styles of the toolkit.
    g_toggle.outlined = g_refresh.outlined = g_load.outlined = g_unload.outlined = 1;
    uui_checkbox_init(&g_keep, 0, 0, 0, "Keep disabled after restart", UUI_COLOR_UNSET, UUI_COLOR_UNSET);
    uui_dialog_init(&g_ask);

    uui_statusbar_init(&g_status);
    g_status.panes[0].text = g_st_count;
    g_status.panes[0].chars = 12;
    g_status.panes[1].text = g_st_problem;
    g_status.panes[1].chars = 30;
    g_status.panes[2].text = g_st_note;
    g_status.panes[2].chars = 0;
    g_status.count = 3;

    uui_sndformat_init(&g_fmt, ID_SNDFMT);
    uui_netadapter_init(&g_net, ID_NETADP);
    uui_clip_init(&g_fmt_clip, (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_fmt.col,
                                                  .name = "sndfmt" });
    uui_clip_init(&g_net_clip, (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_net.col,
                                                  .name = "netadapter" });
    build_focus();

    struct uapp_desc desc = {
        .title        = "Device Manager",
        .app_id       = "devmgr",
        .flags        = UAPP_RESIZABLE | UAPP_SINGLE_INSTANCE,
        .on_size      = on_size,
        .min_w        = 480,
        .min_h        = 320,
        .widgets      = g_widgets,
        .widget_count = (int)(sizeof g_widgets / sizeof g_widgets[0]),
        .focus        = &g_focus,
        .on_open      = on_open,
        .on_draw      = on_draw,
        .on_widget    = on_widget,
        .on_action    = on_action,
        .on_key       = on_key,
        .on_resize    = on_resize,
        .on_tick      = on_tick,
        .tick_ms      = 2000,
    };
    return uapp_run(&desc);
}
