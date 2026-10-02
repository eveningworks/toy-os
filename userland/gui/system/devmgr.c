// Device Manager -- the machine's devices, the driver driving each, and
// Disable/Enable. Windows' Device Manager in shape (a tree by type, or by
// connection), with the selected device's properties beside the tree
// rather than in a dialog, as System Settings pages sit beside its
// sidebar.
//
// EVERYTHING ABOUT DEVICES IS lib/udevice.c, shared with /bin/devctl --
// this file is the view. Disabling is an unbind; "keep disabled after
// restart" writes /etc/devices.conf, which the `devices` service
// re-applies at boot (udevice.h says what each state means).
//
// THE LIST IS RE-READ WHEN IT CHANGES, NOT ON A TIMER: naming a device
// streams the 1.6 MB pci.ids, so the tick compares a cheap signature
// (each device's binding and holder) and relists only when it moved --
// a USB device plugged in, or a driver let go.
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
#include "ui/uui_tree.h"
#include "ui/uui_splitter.h"
#include "ui/uui_button.h"
#include "ui/uui_checkbox.h"
#include "ui/uui_statusbar.h"
#include "ui/uui_dialog.h"
#include "ui/uui_focus.h"
#include "ui/uui_route.h"   // UUI_REASON_*
#include "lib/udevice.h"

#define ID_MENU    1
#define ID_VIEW    2
#define ID_TREE    3
#define ID_SPLIT   4
#define ID_TOGGLE  5
#define ID_REFRESH 6
#define ID_KEEP    7
#define ID_STATUS  8
#define ID_ASK     9

enum {
    CMD_REFRESH = 1, CMD_EXIT, CMD_BY_TYPE, CMD_BY_CONN, CMD_EXPAND, CMD_COLLAPSE,
    CMD_TOGGLE, ASK_DISABLE, ASK_CANCEL,
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
static struct uui_tree g_tree;
static struct uui_splitter g_split;
static struct uui_button g_toggle, g_refresh;
static struct uui_checkbox g_keep;
static struct uui_statusbar g_status;
static struct uui_dialog g_ask;

static char g_st_count[32], g_st_problem[32], g_st_note[96];
static char g_ask_line[2][112];
static const char *g_ask_rows[2];
static char g_ask_id[24];   // by ID: the tick may relist under the open dialog

// Pane geometry, placed by layout_all() and drawn by on_draw().
static int g_px, g_py, g_pw, g_ph;

static const struct uui_menu_item file_items[] = {
    UUI_MENU("Refresh", CMD_REFRESH, 0),
    UUI_MENU_SEP,
    UUI_MENU("Exit", CMD_EXIT, "Alt+F4"),
};
static const struct uui_menu_item view_items[] = {
    UUI_MENU("Devices by type", CMD_BY_TYPE, 0),
    UUI_MENU("Devices by connection", CMD_BY_CONN, 0),
    UUI_MENU_SEP,
    UUI_MENU("Expand all", CMD_EXPAND, 0),
    UUI_MENU("Collapse all", CMD_COLLAPSE, 0),
};
static const struct uui_menu_item action_items[] = {
    UUI_MENU("Disable or enable device", CMD_TOGGLE, 0),
};
static const struct uui_menu_item menu_items[] = {
    UUI_SUBMENU("File", file_items),
    UUI_SUBMENU("View", view_items),
    UUI_SUBMENU("Action", action_items),
};

// The dialog LAST: an open one is on top, and the router asks the last
// item first.
static struct uui_item g_widgets[] = {
    { .ops = &uui_menubar_ops,   .widget = &g_menu,    .id = ID_MENU,    .name = "menu" },
    { .ops = &uui_segmented_ops, .widget = &g_view,    .id = ID_VIEW,    .name = "view" },
    { .ops = &uui_tree_ops,      .widget = &g_tree,    .id = ID_TREE,    .name = "tree" },
    { .ops = &uui_splitter_ops,  .widget = &g_split,   .id = ID_SPLIT,   .name = "split" },
    { .ops = &uui_checkbox_ops,  .widget = &g_keep,    .id = ID_KEEP,    .name = "keep" },
    { .ops = &uui_button_ops,    .widget = &g_refresh, .id = ID_REFRESH, .name = "refresh" },
    { .ops = &uui_button_ops,    .widget = &g_toggle,  .id = ID_TOGGLE,  .name = "toggle" },
    { .ops = &uui_statusbar_ops, .widget = &g_status,  .id = ID_STATUS,  .name = "status" },
    { .ops = &uui_dialog_ops,    .widget = &g_ask,     .id = ID_ASK,     .name = "ask" },
};

static struct uui_focusable g_focusables[] = {
    { &g_tree,    &uui_tree_ops },
    { &g_view,    &uui_segmented_ops },
    { &g_keep,    &uui_checkbox_ops },
    { &g_refresh, &uui_button_ops },
    { &g_toggle,  &uui_button_ops },
};
static struct uui_focus g_focus;

// --- the tree ------------------------------------------------------------

// A heading's label with its member count after it -- "Sound (2)", as
// the mockup and Windows' collapsed categories show. Storage for the
// tree, which borrows every label; reset on each rebuild.
static char g_head[UDEV_T_COUNT + 4][72];
static int g_head_n;

static const char *head(const char *name, int count) {
    if (g_head_n >= (int)(sizeof g_head / sizeof g_head[0])) return name;
    snprintf(g_head[g_head_n], sizeof g_head[0], "%s (%d)", name, count);
    return g_head[g_head_n++];
}

static void label_device(int i) {
    strlcpy(g_label[i], g_dev[i].name, sizeof g_label[i]);
}

static void add_node(const char *label, int depth, int id, const char *icon) {
    if (g_node_count >= (int)(sizeof g_nodes / sizeof g_nodes[0])) return;
    g_nodes[g_node_count++] = (struct uui_tree_node){ label, depth, id, UUI_TREE_AUTO, icon, 0 };
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
        int any = 0;
        for (int i = 0; i < g_n; i++) any += (int)g_dev[i].type == t;
        if (!any) continue;
        add_node(head(udevice_type_name(t), any), 0, NODE_CAT(t), udevice_type_icon(t));
        for (int i = 0; i < g_n; i++)
            if ((int)g_dev[i].type == t) add_device(i, 1);
    }
}

// As the hardware is wired: the PCI bus, with each USB device under the
// controller it hangs off; then the platform devices; then the CPUs.
static void build_by_connection(void) {
    int npci = 0, ncpu = 0;
    for (int i = 0; i < g_n; i++) { npci += g_dev[i].bus == UDEV_PCI; ncpu += g_dev[i].bus == UDEV_CPU; }
    add_node(head("PCI bus", npci), 0, NODE_BUS_PCI, "cat-system");
    for (int i = 0; i < g_n; i++) {
        if (g_dev[i].bus != UDEV_PCI) continue;
        add_device(i, 1);
        for (int j = 0; j < g_n; j++)
            if (g_dev[j].bus == UDEV_USB && g_dev[j].parent == i) add_device(j, 2);
    }
    int plat = 0;
    for (int i = 0; i < g_n; i++) plat += g_dev[i].bus == UDEV_PLATFORM;
    if (plat) {
        add_node(head("Platform devices", plat), 0, NODE_BUS_PLAT, "cat-system");
        for (int i = 0; i < g_n; i++)
            if (g_dev[i].bus == UDEV_PLATFORM) add_device(i, 1);
    }
    add_node(head(udevice_type_name(UDEV_T_CPU), ncpu), 0, NODE_CAT(UDEV_T_CPU),
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
    open_problems();
    if (keep_id && keep_id[0])
        for (int i = 0; i < g_n; i++)
            if (!strcmp(g_dev[i].id, keep_id)) { uui_tree_select_id(&g_tree, i); return; }
    // Nothing to keep: the first DEVICE, so the pane has something to say.
    for (int n = 0; n < g_node_count; n++)
        if (g_nodes[n].id >= 0) { uui_tree_select_id(&g_tree, g_nodes[n].id); return; }
}

// --- the controls under the properties ------------------------------------

static void update_controls(void) {
    const struct udevice *d = selected();
    int can = d && d->can_disable && !d->holder_pid;
    g_toggle.label = d && d->disabled ? "Enable device" : "Disable device";
    g_toggle.disabled = !can;
    g_keep.disabled = !can;
    g_keep.checked = d ? d->persisted : 0;

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
// driver name and holder, and the USB slots. Cheap -- no database.
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
    return h;
}
static unsigned g_sig;

// --- disable / enable ------------------------------------------------------

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

static void ask_disable(struct uapp *a) {
    const struct udevice *d = selected();
    if (!d || !d->can_disable || uui_dialog_is_open(&g_ask)) return;
    strlcpy(g_ask_id, d->id, sizeof g_ask_id);
    snprintf(g_ask_line[0], sizeof g_ask_line[0], "Disable %s?", d->name);
    snprintf(g_ask_line[1], sizeof g_ask_line[1], "%s",
             g_keep.checked ? "It stays disabled after a restart, until you enable it."
                            : "It stays disabled until you enable it or restart.");
    g_ask_rows[0] = g_ask_line[0];
    g_ask_rows[1] = g_ask_line[1];
    static const struct uui_dialog_button btns[] = {
        { "Disable", ASK_DISABLE, UUI_DLG_DANGER }, { "Cancel", ASK_CANCEL, 0 },
    };
    uui_dialog_set_bounds(&g_ask, 0, 0, uapp_width(a), uapp_height(a));
    uui_dialog_open(&g_ask, "Disable device", g_ask_rows, 2, btns, 2, 1, ASK_CANCEL);
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

// --- layout and drawing ---------------------------------------------------

static void layout_all(int cw, int ch) {
    int mb = uui_menubar_height(&g_menu), sb = uui_statusbar_height(&g_status);
    int pad = utheme_pad(), bt = utheme_control_h();
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
    int th = g_view.h + 2 * pad;
    g_tree.x = 0; g_tree.y = top + th; g_tree.w = left; g_tree.h = bottom - top - th;

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
    uui_checkbox_set_geometry(&g_keep, g_px + pad, by + (bt - ugfx_char_h()) / 2);

    uui_dialog_set_bounds(&g_ask, 0, 0, cw, ch);
}

// One "Key   value" row; returns the next row's y, or 0 when out of room.
static int row(struct ugfx_surface *s, int y, const char *k, const char *v) {
    const struct utheme *t = utheme_current();
    int pad = utheme_pad(), lh = ugfx_char_h() + pad / 2, kw = ugfx_char_advance('n') * 13;
    if (y + lh > g_py + g_ph - utheme_control_h() - 2 * pad) return 0;
    ugfx_draw_string_clipped(s, g_px + 2 * pad, y, kw - pad, k, t->outline, t->panel_bg);
    ugfx_draw_string_clipped(s, g_px + 2 * pad + kw, y, g_pw - 3 * pad - kw, v, t->text, t->panel_bg);
    return y + lh;
}

static void section(struct ugfx_surface *s, int *y, const char *title) {
    if (!*y) return;
    const struct utheme *t = utheme_current();
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    *y += utheme_pad() / 2;
    ugfx_draw_string_clipped(s, g_px + 2 * utheme_pad(), *y, g_pw - 3 * utheme_pad(), title,
                             t->accent, t->panel_bg);
    ugfx_set_font(was);
    *y += ugfx_char_h() + utheme_pad() / 2;
}

// The driver's source file and description, from QUERY_DRIVER.
static void driver_info(const char *name, char *file, int fcap) {
    file[0] = '\0';
    struct query_driver q;
    QUERY_FOREACH(QUERY_DRIVER, q, i)
        if (!strcmp(q.name, name)) { strlcpy(file, q.file, (size_t)fcap); return; }
}

static void draw_pane(struct ugfx_surface *s) {
    const struct utheme *t = utheme_current();
    int pad = utheme_pad(), ch = ugfx_char_h();
    ugfx_fill_rect(s, g_px, g_py, g_pw, g_ph, t->panel_bg);
    const struct udevice *d = selected();
    int x = g_px + 2 * pad, y = g_py + 2 * pad, w = g_pw - 4 * pad;
    if (w <= 0) return;

    if (!d) {
        int id = uui_tree_selected_id(&g_tree), count = 0;
        const char *what = "Select a device to see its properties.";
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
    y += bh + pad;

    char buf[176], file[64];
    section(s, &y, "Device");
    if (y) y = row(s, y, "Location", d->location);
    if (y && d->bus != UDEV_CPU && d->bus != UDEV_PLATFORM) {
        snprintf(buf, sizeof buf, "%04x:%04x", d->vendor, d->device);
        y = row(s, y, "IDs", buf);
    }
    if (y && d->vendor_name[0]) y = row(s, y, "Vendor", d->vendor_name);
    // The class LEVEL BY LEVEL, each by name with its code after it; a
    // level the database has no name for is left out, and a device with
    // no names at all shows the bare code.
    if (y && (d->bus == UDEV_PCI || d->bus == UDEV_USB)) {
        const char *third = d->bus == UDEV_USB ? "Protocol" : "Interface";
        if (!d->class_name[0] && !d->subclass_name[0] && !d->progif_name[0]) {
            snprintf(buf, sizeof buf, "%02x/%02x/%02x", d->cls, d->subclass, d->prog_if);
            y = row(s, y, "Class", buf);
        }
        if (y && d->class_name[0]) {
            snprintf(buf, sizeof buf, "%s (%02x)", d->class_name, d->cls);
            y = row(s, y, "Class", buf);
        }
        if (y && d->subclass_name[0]) {
            snprintf(buf, sizeof buf, "%s (%02x)", d->subclass_name, d->subclass);
            y = row(s, y, "Subclass", buf);
        }
        if (y && d->progif_name[0]) {
            snprintf(buf, sizeof buf, "%s (%02x)", d->progif_name, d->prog_if);
            y = row(s, y, third, buf);
        }
    }
    if (y && d->bus != UDEV_CPU) {
        section(s, &y, "Driver");
        if (y) y = row(s, y, "Driver", d->driver[0] ? d->driver : "(none)");
        if (y && d->driver[0]) {
            driver_info(d->driver, file, sizeof file);
            if (file[0]) y = row(s, y, "Source", file);
        }
        if (y && d->holder_pid) {
            snprintf(buf, sizeof buf, "pid %d", d->holder_pid);
            y = row(s, y, "Held by", buf);
        }
        if (y) y = row(s, y, "Can disable", d->can_disable ? "Yes"
                        : d->holder_pid ? "No (in use by a program)"
                        : d->driver[0] ? "No (not supported by its driver)" : "No");
    }
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    layout_all(d->surface->w, d->surface->h);
    draw_pane(d->surface);
    uapp_log_layout(a, "devmgr");
    const struct udevice *sel = selected();
    uapp_logf_layout("devmgr: selected %s view %s\n", sel ? sel->id : "-",
                     g_by_conn ? "connection" : "type");
}

// --- commands and input -------------------------------------------------

static void set_view(struct uapp *a, int by_conn) {
    g_by_conn = by_conn;
    g_view.selected = by_conn;
    const struct udevice *d = selected();
    char keep[24] = "";
    if (d) strlcpy(keep, d->id, sizeof keep);
    rebuild_tree(keep);
    uapp_redraw(a);
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
    default: return;
    }
    update_controls();
    uapp_redraw(a);
}

static void on_action(struct uapp *a, int code) {
    if (code == ID_REFRESH) { do_command(a, CMD_REFRESH); return; }
    if (code != ID_TOGGLE) return;
    toggle(a);
    update_controls();
    uapp_redraw(a);
}

static void on_widget(struct uapp *a, int id, int reason) {
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
        const struct udevice *d = code == ASK_DISABLE ? find_id(g_ask_id) : 0;
        if (d && d->can_disable) {
            report(d, udevice_disable(d, g_keep.checked), "disabled");
            relist();
            g_sig = signature();
        }
        g_ask_id[0] = 0;
        uapp_redraw(a);
        return;
    }
    case ID_VIEW:
        if (g_view.selected != g_by_conn) set_view(a, g_view.selected);
        return;
    }
    if (reason != UUI_REASON_RELEASE && reason != UUI_REASON_KEY) {
        if (id == ID_TREE) { update_controls(); uapp_redraw(a); }
        return;
    }
    switch (id) {
    case ID_KEEP:    keep_changed(); break;
    case ID_TREE:    break;
    default: return;
    }
    update_controls();
    uapp_redraw(a);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    int code;
    if (uui_menubar_key(&g_menu, key, &code)) {
        if (code >= 0) do_command(a, code);
        else uapp_redraw(a);
        return;
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
    *w = per * 100;
    *h = ugfx_char_h() * 34;
}

int main(void) {
    uui_menubar_init(&g_menu, menu_items, (int)(sizeof menu_items / sizeof menu_items[0]));
    uui_segmented_init(&g_view, VIEWS, 2, 0);
    uui_tree_init(&g_tree, 0, 0, 0, 0, g_nodes, 0);
    g_tree.sel_style = UUI_SEL_STRONG;
    uui_splitter_init(&g_split, 1, 430);
    uui_button_init(&g_toggle, 0, 0, 0, 0, "Disable device", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_TOGGLE);
    uui_button_init(&g_refresh, 0, 0, 0, 0, "Refresh", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_REFRESH);
    // The mockup's look: outlined buttons, and the selected device in
    // the accent -- both opt-in styles of the toolkit.
    g_toggle.outlined = g_refresh.outlined = 1;
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

    uui_focus_init(&g_focus, g_focusables, (int)(sizeof g_focusables / sizeof g_focusables[0]));

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
