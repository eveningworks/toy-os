// Log Viewer -- the kernel ring and the application ring as one list, a
// stored boot, or any /var/log file: macOS Console's table with a details
// pane, and a boot timeline above it on demand.
//
// **THE TWO RINGS ARE SEPARATE ON PURPOSE AND READ TOGETHER HERE.** klog
// stores BYTES and applog stores RECORDS with a tag, so a chatty program
// cannot flush kernel evidence; both are stamped from coarse_ticks(), and
// lib/ulogset.h merges them in time order. The lines, their repeats and
// the filter are ulogset's; the list is ui/uui_loglist.h; this file is the
// chrome around them and what the commands do.
//
// **A REPEAT IS A FACT ABOUT THE MESSAGE, a mute a fact about the reader.**
// Seeing the same error 33 times is worth one line in the details pane
// ("Seen 33 times") and one command (Only this message); a program that
// logs its layout on every paint is worth muting, and a mute is kept in
// /etc/logview.conf so the next open does not need it again.
#include "ui/uapp.h"
#include "ui/uui.h"
#include "ui/uui_loglist.h"
#include "ui/uui_menubar.h"
#include "ui/uui_toolbar.h"
#include "ui/uui_statusbar.h"
#include "ui/uui_segmented.h"
#include "ui/uui_dropdown.h"
#include "ui/uui_textbox.h"
#include "ui/uui_button.h"
#include "ui/uui_focus.h"
#include "ui/uui_filedialog.h"
#include "ui/utheme.h"
#include "ui/ugfx.h"
#include "ui/ulog.h"
#include "lib/ulogset.h"
#include "lib/uclip.h"
#include "lib/uconf.h"
#include "rt/sys.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Bounded, and the newest win. A quiet boot on the test laptop is ~600
// lines; one with a driver retrying is thousands, and a stored boot is
// read from its END so its start is what is lost (ulogset_read_file).
#define MAX_LINES 4000

#define LOG_DIR    "/var/log"
#define BOOT_DIR   "/var/log/boot"
#define CONF_PATH  "/etc/logview.conf"
#define MAX_SOURCES 64
#define SOURCE_LABEL 32

enum {
    ID_MENU = 1, ID_TB, ID_SOURCE, ID_SHOW, ID_FROM, ID_SEARCH, ID_LIST,
    ID_AROUND, ID_ONLY, ID_MUTE, ID_COPYLINE, ID_UNMUTE,
};

enum {
    CMD_FOLLOW = 100, CMD_COPY, CMD_COPY_ALL, CMD_SAVE, CMD_DETAILS, CMD_TIMELINE, CMD_FIND,
    CMD_CLEAR, CMD_NEXT_ERR, CMD_PREV_ERR, CMD_FIRST, CMD_LAST, CMD_UNMUTE_ALL, CMD_EXIT,
    CMD_SHOW_ALL, CMD_SHOW_ERR, CMD_SHOW_WARN,
};

static struct ulog_line g_lines[MAX_LINES];
static int g_view[MAX_LINES];
static struct ulogset g_set;
static struct ulog_filter g_filter = { .only = -1 };

// --- sources: this boot, stored boots, other files ------------------------

struct source { char label[SOURCE_LABEL]; char path[64]; int live; };
static struct source g_src[MAX_SOURCES];
static const char *g_src_items[MAX_SOURCES];
static int g_nsrc;

static int cmp_boot_desc(const void *a, const void *b) {
    const struct source *x = a, *y = b;
    long nx = strtol(x->label + 5, 0, 10), ny = strtol(y->label + 5, 0, 10);
    return nx < ny ? 1 : nx > ny ? -1 : 0;
}

static void add_source(const char *label, const char *path, int live) {
    if (g_nsrc >= MAX_SOURCES) return;
    struct source *s = &g_src[g_nsrc++];
    snprintf(s->label, sizeof s->label, "%s", label);
    snprintf(s->path, sizeof s->path, "%s", path);
    s->live = live;
}

// "This boot" first, then the stored boots NEWEST first -- the one wanted
// is almost always recent -- then every other *.log in /var/log.
static void scan_sources(void) {
    g_nsrc = 0;
    add_source("This boot", "", 1);
    int first_boot = g_nsrc;
    DIR *d = opendir(BOOT_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && g_nsrc < MAX_SOURCES - 16) {
            if (e->d_type == DT_DIR) continue;
            unsigned long long n = strtoull(e->d_name, 0, 10);
            if (!n) continue;
            char label[SOURCE_LABEL], path[64];
            snprintf(label, sizeof label, "Boot %llu", n);
            snprintf(path, sizeof path, BOOT_DIR "/%s", e->d_name);
            add_source(label, path, 0);
        }
        closedir(d);
    }
    qsort(&g_src[first_boot], (size_t)(g_nsrc - first_boot), sizeof g_src[0], cmp_boot_desc);
    d = opendir(LOG_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && g_nsrc < MAX_SOURCES) {
            if (e->d_type == DT_DIR || !strstr(e->d_name, ".log")) continue;
            char path[64];
            snprintf(path, sizeof path, LOG_DIR "/%s", e->d_name);
            add_source(e->d_name, path, 0);
        }
        closedir(d);
    }
    for (int i = 0; i < g_nsrc; i++) g_src_items[i] = g_src[i].label;
}

// --- widgets ----------------------------------------------------------------

static struct uui_menubar g_menu;
static struct uui_toolbar g_tb;
static struct uui_dropdown g_source;
static struct uui_segmented g_show, g_from;
static struct uui_textbox g_search;
static struct uui_loglist g_list;
static struct uui_button g_around, g_only, g_mute, g_copyline, g_unmute;
static struct uui_statusbar g_status;
static struct uui_filedialog g_fd;
static int g_details = 1;

static char g_show_lbl[3][24];
static const char *g_show_items[3] = { g_show_lbl[0], g_show_lbl[1], g_show_lbl[2] };
static const char *const FROM_ITEMS[] = { "All sources", "Kernel", "Programs" };
static char g_only_lbl[32], g_mute_lbl[48], g_unmute_lbl[32];
static char g_st_lines[32], g_st_err[24], g_st_warn[24], g_st_src[SOURCE_LABEL], g_st_follow[16];

static const struct uui_menu_item file_items[] = {
    UUI_MENU("Save shown lines as...", CMD_SAVE, "Ctrl+S"),
    UUI_MENU_SEP,
    UUI_MENU("Exit", CMD_EXIT, "Alt+F4"),
};
static const struct uui_menu_item edit_items[] = {
    UUI_MENU("Copy line", CMD_COPY, "Ctrl+C"),
    UUI_MENU("Copy shown lines", CMD_COPY_ALL, 0),
    UUI_MENU_SEP,
    UUI_MENU("Find", CMD_FIND, "Ctrl+F"),
    UUI_MENU("Clear filters", CMD_CLEAR, 0),
    UUI_MENU("Unmute all", CMD_UNMUTE_ALL, 0),
};
static const struct uui_menu_item view_items[] = {
    UUI_MENU("Follow new lines", CMD_FOLLOW, 0),
    UUI_MENU("Details", CMD_DETAILS, 0),
    UUI_MENU("Timeline", CMD_TIMELINE, 0),
    UUI_MENU_SEP,
    UUI_MENU("All levels", CMD_SHOW_ALL, 0),
    UUI_MENU("Errors only", CMD_SHOW_ERR, 0),
    UUI_MENU("Warnings only", CMD_SHOW_WARN, 0),
};
static const struct uui_menu_item go_items[] = {
    UUI_MENU("Next error", CMD_NEXT_ERR, "F8"),
    UUI_MENU("Previous error", CMD_PREV_ERR, "F7"),
    UUI_MENU_SEP,
    UUI_MENU("First line", CMD_FIRST, "Home"),
    UUI_MENU("Newest line", CMD_LAST, "End"),
};
static const struct uui_menu_item menu_items[] = {
    UUI_SUBMENU("File", file_items),
    UUI_SUBMENU("Edit", edit_items),
    UUI_SUBMENU("View", view_items),
    UUI_SUBMENU("Go", go_items),
};

static const struct uui_toolbar_item tb_items[] = {
    { "tb-play",    "Show new lines as they arrive",   CMD_FOLLOW,   "Follow",   0, 0, UTHEME_ACT_NAV },
    UUI_TOOLBAR_SEP,
    { "tb-copy",    "Copy the selected line",          CMD_COPY,     "Copy",     0, 0, UTHEME_ACT_EDIT },
    { "tb-save",    "Save the lines shown to a file",  CMD_SAVE,     "Save as...", 0, 0, UTHEME_ACT_CREATE },
    UUI_TOOLBAR_SEP,
    { "tb-find",    "Find in the lines",               CMD_FIND,     "Find",     0, 0, UTHEME_ACT_VIEW },
    { "tb-details", "Show the selected line's details", CMD_DETAILS, "Details",  0, 0, UTHEME_ACT_ARRANGE },
    { "tb-history", "Show when the lines were written", CMD_TIMELINE, "Timeline", 0, 0, UTHEME_ACT_ARRANGE },
};

// The dropdown LAST: an open popup is on top, and the router asks the
// last item first.
static struct uui_item g_widgets[] = {
    { .ops = &uui_menubar_ops,   .widget = &g_menu,     .id = ID_MENU,     .name = "menu" },
    { .ops = &uui_toolbar_ops,   .widget = &g_tb,       .id = ID_TB,       .name = "toolbar" },
    { .ops = &uui_segmented_ops, .widget = &g_show,     .id = ID_SHOW,     .name = "show" },
    { .ops = &uui_segmented_ops, .widget = &g_from,     .id = ID_FROM,     .name = "from" },
    { .ops = &uui_button_ops,    .widget = &g_unmute,   .id = ID_UNMUTE,   .name = "unmute" },
    { .ops = &uui_textbox_ops,   .widget = &g_search,   .id = ID_SEARCH,   .name = "search" },
    { .ops = &uui_loglist_ops,   .widget = &g_list,     .id = ID_LIST,     .name = "log" },
    { .ops = &uui_button_ops,    .widget = &g_around,   .id = ID_AROUND,   .name = "around" },
    { .ops = &uui_button_ops,    .widget = &g_only,     .id = ID_ONLY,     .name = "only" },
    { .ops = &uui_button_ops,    .widget = &g_mute,     .id = ID_MUTE,     .name = "mute" },
    { .ops = &uui_button_ops,    .widget = &g_copyline, .id = ID_COPYLINE, .name = "copyline" },
    { .ops = &uui_statusbar_ops, .widget = &g_status,   .id = 0,           .name = "status" },
    { .ops = &uui_dropdown_ops,  .widget = &g_source,   .id = ID_SOURCE,   .name = "source" },
};
#define W_COUNT ((int)(sizeof g_widgets / sizeof g_widgets[0]))

static struct uui_focusable g_ring[] = {
    { &g_list,   &uui_loglist_ops },
    { &g_search, &uui_textbox_focus_ops },
    { &g_show,   &uui_segmented_ops },
    { &g_from,   &uui_segmented_ops },
    { &g_source, &uui_dropdown_ops },
};
enum { RING_LIST = 0, RING_SEARCH };
static struct uui_focus g_focus;

static void set_hidden(int id, int hidden) {
    for (int i = 0; i < W_COUNT; i++) if (g_widgets[i].id == id) g_widgets[i].hidden = hidden;
}

// --- loading and filtering ------------------------------------------------

static int g_source_index;

static int viewing_live(void) { return g_src[g_source_index].live; }

// The line the reader had selected, by WHAT IT IS: a reload re-reads and
// re-sorts, so its index means nothing afterwards.
struct line_id { int have; unsigned cs; char source[ULOG_SRC_MAX]; char text[ULOG_TEXT_MAX]; };

static void remember(struct line_id *id) {
    int i = uui_loglist_selected_line(&g_list);
    id->have = i >= 0;
    if (!id->have) return;
    id->cs = g_set.lines[i].cs;
    snprintf(id->source, sizeof id->source, "%s", g_set.lines[i].source);
    snprintf(id->text, sizeof id->text, "%s", g_set.lines[i].text);
}

static int find_line(const struct line_id *id) {
    if (!id->have) return -1;
    for (int i = g_set.count - 1; i >= 0; i--)
        if (g_set.lines[i].cs == id->cs && !strcmp(g_set.lines[i].text, id->text) &&
            !strcmp(g_set.lines[i].source, id->source)) return i;
    return -1;
}

static void update_labels(void);

static void refilter(int keep_sel, int reveal) {
    struct line_id id;
    remember(&id);
    ulogset_filter(&g_set, &g_filter);
    uui_loglist_refresh(&g_list);
    if (keep_sel) uui_loglist_select_line(&g_list, find_line(&id), reveal);
    update_labels();
}

// "Only this message" names a LINE, which a reload renumbers: it is kept
// as the message and found again.
static struct line_id g_only_id;

static void reload(void) {
    struct line_id id;
    remember(&id);
    ulogset_clear(&g_set);
    const struct source *s = &g_src[g_source_index];
    if (s->live) {
        ulogset_read_klog(&g_set);
        ulogset_read_applog(&g_set);
    } else {
        const char *name = strrchr(s->path, '/');
        ulogset_read_file(&g_set, s->path, name ? name + 1 : s->path);
    }
    ulogset_finish(&g_set);
    if (g_only_id.have) g_filter.only = find_line(&g_only_id);
    ulogset_filter(&g_set, &g_filter);
    uui_loglist_refresh(&g_list);
    uui_loglist_select_line(&g_list, find_line(&id), 0);
    update_labels();
}

// --- mutes, kept --------------------------------------------------------

static void load_mutes(void) {
    char v[ULOG_MUTE_MAX * ULOG_SUB_MAX];
    if (uconf_get(CONF_PATH, "mute", v, sizeof v) < 0) return;
    for (char *p = v, *tok; (tok = strtok_r(p, ",", &p)) != 0; )
        if (*tok) ulog_filter_mute(&g_filter, tok);
}

static void save_mutes(void) {
    char v[ULOG_MUTE_MAX * ULOG_SUB_MAX] = "";
    int n = 0;
    for (int k = 0; k < g_filter.nmute; k++)
        n += snprintf(v + n, sizeof v - (size_t)n, "%s%s", k ? "," : "", g_filter.mute[k]);
    if (g_filter.nmute) uconf_set(CONF_PATH, "mute", v);
    else uconf_unset(CONF_PATH, "mute");
}

// --- labels and the status bar -------------------------------------------

static void update_labels(void) {
    struct ulog_counts c;
    ulogset_counts(&g_set, &c);
    snprintf(g_show_lbl[0], sizeof g_show_lbl[0], "All  %d", g_set.count);
    snprintf(g_show_lbl[1], sizeof g_show_lbl[1], "Errors  %d", c.errors);
    snprintf(g_show_lbl[2], sizeof g_show_lbl[2], "Warnings  %d", c.warnings);

    if (g_set.view_count == g_set.count)
        snprintf(g_st_lines, sizeof g_st_lines, "%d lines", g_set.count);
    else
        snprintf(g_st_lines, sizeof g_st_lines, "%d of %d lines", g_set.view_count, g_set.count);
    snprintf(g_st_err, sizeof g_st_err, "%d error%s", c.errors, c.errors == 1 ? "" : "s");
    snprintf(g_st_warn, sizeof g_st_warn, "%d warning%s", c.warnings, c.warnings == 1 ? "" : "s");
    snprintf(g_st_src, sizeof g_st_src, "%s", g_src[g_source_index].label);
    snprintf(g_st_follow, sizeof g_st_follow, "%s",
             !viewing_live() ? "" : g_list.follow ? "Following" : "Paused");

    int sel = uui_loglist_selected_line(&g_list);
    snprintf(g_only_lbl, sizeof g_only_lbl, "%s", g_filter.only >= 0 ? "All messages" : "Only this message");
    if (sel >= 0) {
        const char *who = ulogset_who(&g_set, sel);
        snprintf(g_mute_lbl, sizeof g_mute_lbl, "%s %s", ulog_filter_muted(&g_filter, who) ? "Unmute" : "Mute", who);
    } else {
        snprintf(g_mute_lbl, sizeof g_mute_lbl, "Mute");
    }
    if (g_filter.nmute == 1) snprintf(g_unmute_lbl, sizeof g_unmute_lbl, "Unmute %s", g_filter.mute[0]);
    else snprintf(g_unmute_lbl, sizeof g_unmute_lbl, "Unmute %d", g_filter.nmute);
    set_hidden(ID_UNMUTE, g_filter.nmute == 0);
    g_around.disabled = g_only.disabled = g_mute.disabled = g_copyline.disabled = sel < 0;
}

// --- commands -------------------------------------------------------------

static void copy_text(const char *t, int n) {
    if (uclip_set_text(t, n) < 0) ulogf("logview: the clipboard refused %d bytes\n", n);
}

static void copy_line(void) {
    int i = uui_loglist_selected_line(&g_list);
    if (i < 0) return;
    char buf[ULOG_TEXT_MAX + 64];
    int n = ulogset_format(&g_set, i, buf, sizeof buf);
    if (n > 0) copy_text(buf, n);
}

static void copy_shown(void) {
    static char buf[64 * 1024];
    int n = 0;
    for (int r = 0; r < g_set.view_count && n < (int)sizeof buf - 1; r++) {
        int k = ulogset_format(&g_set, g_set.view[r], buf + n, (int)sizeof buf - n);
        if (k <= 0 || n + k >= (int)sizeof buf) break;
        n += k;
    }
    copy_text(buf, n);
}

static void save_done(void *ctx, const char *path) {
    struct uapp *a = ctx;
    if (!path) return;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { ulogf("logview: could not write %s\n", path); return; }
    char buf[ULOG_TEXT_MAX + 64];
    int ok = 1;
    for (int r = 0; r < g_set.view_count && ok; r++) {
        int n = ulogset_format(&g_set, g_set.view[r], buf, sizeof buf);
        if (n > 0 && write(fd, buf, (size_t)n) != n) ok = 0;
    }
    close(fd);
    ulogf("logview: saved %d lines to %s%s\n", g_set.view_count, path, ok ? "" : " (SHORT)");
    uapp_redraw(a);
}

static void save_as(struct uapp *a) {
    if (uui_filedialog_is_open(&g_fd)) return;
    static char name[48];
    const char *label = g_src[g_source_index].label;
    if (viewing_live()) snprintf(name, sizeof name, "this-boot.log");
    else if (!strncmp(label, "Boot ", 5)) snprintf(name, sizeof name, "boot-%s.log", label + 5);
    else snprintf(name, sizeof name, "%s", label);
    struct uui_filedialog_opts o = { .mode = UUI_FILEDIALOG_SAVE, .title = "Save shown lines",
                                     .start_dir = "/home", .initial_name = name };
    uui_filedialog_open(a, &g_fd, &o, save_done, a);
}

static void goto_error(int dir) {
    int n = g_set.view_count;
    if (!n) return;
    int r = g_list.selected < 0 ? (dir > 0 ? -1 : n) : g_list.selected;
    for (r += dir; r >= 0 && r < n; r += dir) {
        if (ulog_severity(g_set.lines[g_set.view[r]].level) != UTHEME_SEV_ERROR) continue;
        uui_loglist_select_line(&g_list, g_set.view[r], 1);
        return;
    }
}

static void set_show(int v) {
    g_filter.show = v;
    g_show.selected = v;
    refilter(1, 1);
}

static void clear_filters(void) {
    g_filter.show = g_filter.from = 0;
    g_filter.search[0] = 0;
    g_filter.only = -1;
    g_only_id.have = 0;
    g_filter.range_lo = g_filter.range_hi = 0;
    g_list.range_lo = g_list.range_hi = 0;
    g_show.selected = g_from.selected = 0;
    uui_textbox_set_text(&g_search, "");
}

static void command(struct uapp *a, int code) {
    switch (code) {
    case CMD_FOLLOW:   uui_loglist_set_follow(&g_list, !g_list.follow); update_labels(); break;
    case CMD_COPY:     copy_line(); break;
    case CMD_COPY_ALL: copy_shown(); break;
    case CMD_SAVE:     save_as(a); break;
    case CMD_DETAILS:  g_details = !g_details; break;
    case CMD_TIMELINE: g_list.timeline = !g_list.timeline; uui_loglist_refresh(&g_list); break;
    case CMD_FIND:     uui_focus_set(&g_focus, RING_SEARCH); break;
    case CMD_CLEAR:    clear_filters(); refilter(1, 1); break;
    case CMD_UNMUTE_ALL: g_filter.nmute = 0; save_mutes(); refilter(1, 1); break;
    case CMD_NEXT_ERR: goto_error(1); break;
    case CMD_PREV_ERR: goto_error(-1); break;
    case CMD_FIRST:    if (g_set.view_count) uui_loglist_select_line(&g_list, g_set.view[0], 1); break;
    case CMD_LAST:     uui_loglist_set_follow(&g_list, 1); break;
    case CMD_SHOW_ALL: set_show(ULOG_SHOW_ALL); break;
    case CMD_SHOW_ERR: set_show(ULOG_SHOW_ERRORS); break;
    case CMD_SHOW_WARN: set_show(ULOG_SHOW_WARNINGS); break;
    case CMD_EXIT:     uapp_request_close_pid(a, sys_getpid()); return;
    default: return;
    }
    update_labels();
    uapp_redraw(a);
}

static unsigned item_flags(int code) {
    switch (code) {
    case CMD_FOLLOW:   return !viewing_live() ? UUI_MI_DISABLED : g_list.follow ? UUI_MI_CHECKED : 0;
    case CMD_DETAILS:  return g_details ? UUI_MI_CHECKED : 0;
    case CMD_TIMELINE: return g_list.timeline ? UUI_MI_CHECKED : 0;
    case CMD_COPY:     return uui_loglist_selected_line(&g_list) < 0 ? UUI_MI_DISABLED : 0;
    case CMD_UNMUTE_ALL: return g_filter.nmute ? 0 : UUI_MI_DISABLED;
    case CMD_SHOW_ALL: return g_filter.show == ULOG_SHOW_ALL ? UUI_MI_CHECKED : 0;
    case CMD_SHOW_ERR: return g_filter.show == ULOG_SHOW_ERRORS ? UUI_MI_CHECKED : 0;
    case CMD_SHOW_WARN: return g_filter.show == ULOG_SHOW_WARNINGS ? UUI_MI_CHECKED : 0;
    }
    return 0;
}

// --- layout and drawing ---------------------------------------------------

static int g_filter_y, g_filter_h, g_det_y, g_bottom;

static int per(void) { int p = ugfx_char_advance('n'); return p > 0 ? p : 8; }

static void layout_all(int cw, int ch) {
    int pad = utheme_pad(), bt = utheme_control_h(), lh = ugfx_char_h();
    int mb = uui_menubar_height(&g_menu), tb = uui_toolbar_height(&g_tb), sb = uui_statusbar_height(&g_status);
    uui_menubar_set_geometry(&g_menu, 0, 0, cw, mb);
    uui_menubar_set_bounds(&g_menu, 0, 0, cw, ch);
    int dw = per() * 22;
    uui_dropdown_set_geometry(&g_source, cw - pad - dw, mb + (tb - bt) / 2, dw, bt);
    uui_toolbar_ops.set_geometry(&g_tb, 0, mb, cw - dw - 2 * pad, tb);
    uui_toolbar_set_bounds(&g_tb, 0, 0, cw, ch);

    g_filter_y = mb + tb;
    g_filter_h = bt + 2 * pad;
    int fy = g_filter_y + pad;
    uui_segmented_set_geometry(&g_show, pad * 2, fy);
    uui_segmented_set_geometry(&g_from, g_show.x + g_show.w + 2 * pad, fy);
    int x = g_from.x + g_from.w + 2 * pad;
    if (g_filter.nmute) {
        int bw, bh;
        uui_button_natural_size(&g_unmute, &bw, &bh);
        uui_button_set_geometry(&g_unmute, x, fy, bw, bt);
        x += bw + 2 * pad;
    }
    int sw = cw - x - 2 * pad;
    if (sw < per() * 12) sw = per() * 12;
    uui_textbox_set_geometry(&g_search, x, fy, sw, bt);

    g_bottom = ch - sb;
    uui_statusbar_set_geometry(&g_status, 0, g_bottom, cw, sb);
    int det_h = g_details ? 4 * lh + bt + 4 * pad : 0;
    g_det_y = g_bottom - det_h;
    int ly = g_filter_y + g_filter_h;
    uui_loglist_set_geometry(&g_list, 0, ly, cw, g_det_y - ly);

    int by = g_bottom - pad - bt, bx = pad * 2;
    struct uui_button *bs[] = { &g_around, &g_only, &g_mute, &g_copyline };
    for (int i = 0; i < 4; i++) {
        int bw, bh;
        uui_button_natural_size(bs[i], &bw, &bh);
        uui_button_set_geometry(bs[i], bx, by, bw, bt);
        bx += bw + pad;
    }
    for (int id = ID_AROUND; id <= ID_COPYLINE; id++) set_hidden(id, !g_details);
}

static void draw_details(struct ugfx_surface *s, int cw) {
    const struct utheme *t = utheme_current();
    int pad = utheme_pad(), lh = ugfx_char_h();
    uint32_t bg = t->panel_bg, muted = ugfx_blend(t->text, bg, 140);
    ugfx_fill_rect(s, 0, g_det_y, cw, g_bottom - g_det_y, bg);
    ugfx_fill_rect(s, 0, g_det_y, cw, 1, t->separator);
    int i = uui_loglist_selected_line(&g_list);
    int x = 2 * pad, y = g_det_y + pad, w = cw - 4 * pad;
    if (i < 0) {
        ugfx_draw_string_clipped(s, x, y, w, "Select a line to see it whole.", muted, bg);
        return;
    }
    const struct ulog_line *l = &g_set.lines[i];
    int sev = ulog_severity(l->level);
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_mono(UGFX_FONT_BOLD));
    ugfx_draw_string_elided(s, x, y, w, l->text, sev ? utheme_severity(sev) : t->text, bg);
    ugfx_set_font(was);

    char when[48], lvl[24], src[64], seen[96];
    if (l->stamped) snprintf(when, sizeof when, "%u.%02u s after boot", l->cs / 100, l->cs % 100);
    else if (l->clock[0]) snprintf(when, sizeof when, "%s", l->clock);
    else snprintf(when, sizeof when, "-- (no time on this line)");
    if (l->level >= 0) snprintf(lvl, sizeof lvl, "%s (%d)", sev == 1 ? "Error" : sev == 2 ? "Warning" :
                                l->level == 7 ? "Debug" : "Info", l->level);
    else snprintf(lvl, sizeof lvl, "-- (none declared)");
    if (l->subsys[0]) snprintf(src, sizeof src, "%s  /  %s", l->source, l->subsys);
    else snprintf(src, sizeof src, "%s", l->source);
    if (l->count > 1) {
        const struct ulog_line *f = &g_set.lines[l->first], *z = &g_set.lines[l->last];
        snprintf(seen, sizeof seen, "%d times -- first at %u.%02u s, last at %u.%02u s", l->count,
                 f->cs / 100, f->cs % 100, z->cs / 100, z->cs % 100);
    } else {
        snprintf(seen, sizeof seen, "once");
    }
    const char *k[] = { "Time", "Level", "Source", "Seen" };
    const char *v[] = { when, lvl, src, seen };
    int kw = per() * 8;
    for (int r = 0; r < 4; r++) {
        int ry = y + (r + 1) * lh;
        ugfx_draw_string_clipped(s, x, ry, kw, k[r], muted, bg);
        ugfx_draw_string_clipped(s, x + kw, ry, w - kw, v[r], t->text, bg);
    }
}

static void draw_filter_band(struct ugfx_surface *s, int cw) {
    const struct utheme *t = utheme_current();
    ugfx_fill_rect(s, 0, g_filter_y, cw, g_filter_h, t->chrome);
    ugfx_fill_rect(s, 0, g_filter_y, cw, 1, t->separator);   // under the bar AND the dropdown
    ugfx_fill_rect(s, 0, g_filter_y + g_filter_h - 1, cw, 1, t->separator);
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    int cw = d->surface->w;
    layout_all(cw, d->surface->h);
    draw_filter_band(d->surface, cw);
    if (g_details) draw_details(d->surface, cw);
    // STATE ONLY, NO COUNTS: a layout line is logged when it changes, and
    // the live view re-reads every second -- a count here would log the
    // viewer into its own log once a second.
    uapp_log_layout(a, "logview");
    int sel = uui_loglist_selected_line(&g_list);
    uapp_logf_layout("logview: follow %d timeline %d details %d mutes %d show %d from %d only %d "
                     "sel %d seen %d\n",
                     g_list.follow, g_list.timeline, g_details, g_filter.nmute, g_filter.show,
                     g_filter.from, g_filter.only >= 0, sel, sel >= 0 ? g_set.lines[sel].count : 0);
    // A FINISHED log's counts do not move, so they may be reported.
    if (!viewing_live())
        uapp_logf_layout("logview: source %s lines %d shown %d\n", g_src[g_source_index].label,
                         g_set.count, g_set.view_count);
}

// --- input ------------------------------------------------------------------

static void on_widget(struct uapp *a, int id, int reason) {
    (void)reason;
    switch (id) {
    case ID_MENU: {
        int code = uui_menubar_take_code(&g_menu);
        if (code > 0) command(a, code); else uapp_redraw(a);
        return;
    }
    case ID_TB: {
        int code = uui_toolbar_take_code(&g_tb);
        if (code > 0) command(a, code);
        return;
    }
    case ID_SOURCE: {
        int k = uui_dropdown_selected(&g_source);
        if (k >= 0 && k < g_nsrc && k != g_source_index) {
            g_source_index = k;
            g_only_id.have = 0;
            g_filter.only = -1;
            uui_loglist_select_line(&g_list, -1, 0);
            // A stored log is finished: start at its top, not following.
            uui_loglist_set_follow(&g_list, viewing_live());
            g_list.top = 0;
            reload();
        }
        break;
    }
    case ID_SHOW: set_show(g_show.selected < 0 ? 0 : g_show.selected); break;
    case ID_FROM: g_filter.from = g_from.selected < 0 ? 0 : g_from.selected; refilter(1, 1); break;
    case ID_SEARCH:
        snprintf(g_filter.search, sizeof g_filter.search, "%s", uui_textbox_text(&g_search));
        refilter(1, 1);
        break;
    case ID_LIST: {
        int c = uui_loglist_take_changes(&g_list);
        if (c & UUI_LOGLIST_RANGE) {
            g_filter.range_lo = g_list.range_lo;
            g_filter.range_hi = g_list.range_hi;
            refilter(1, 0);
        }
        break;
    }
    default: return;
    }
    update_labels();
    uapp_redraw(a);
}

static void on_action(struct uapp *a, int code) {
    int i = uui_loglist_selected_line(&g_list);
    switch (code) {
    case ID_AROUND:
        // THE SAME LINE, WITH EVERYTHING AROUND IT: every narrowing goes
        // but the mutes, which the reader chose to keep.
        clear_filters();
        refilter(1, 1);
        break;
    case ID_ONLY:
        if (g_filter.only >= 0) { g_filter.only = -1; g_only_id.have = 0; }
        else if (i >= 0) { g_filter.only = i; remember(&g_only_id); }
        refilter(1, 1);
        break;
    case ID_MUTE:
        if (i >= 0) {
            const char *who = ulogset_who(&g_set, i);
            if (ulog_filter_muted(&g_filter, who)) ulog_filter_unmute(&g_filter, who);
            else ulog_filter_mute(&g_filter, who);
            save_mutes();
            refilter(0, 0);
        }
        break;
    case ID_COPYLINE: copy_line(); break;
    case ID_UNMUTE: command(a, CMD_UNMUTE_ALL); return;
    default: return;
    }
    update_labels();
    uapp_redraw(a);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    int code;
    if (uui_menubar_key(&g_menu, key, &code)) {
        if (code >= 0) command(a, code); else uapp_redraw(a);
        return;
    }
    switch (key) {
    case 0x13: command(a, CMD_SAVE); return;        // Ctrl+S
    case 0x06: command(a, CMD_FIND); return;        // Ctrl+F
    case 0x03: if (g_focus.current == RING_LIST) { command(a, CMD_COPY); return; } break;  // Ctrl+C
    case KEY_F8: command(a, CMD_NEXT_ERR); return;
    case KEY_F7: command(a, CMD_PREV_ERR); return;
    }
    // The list's own keys moved the selection or the follow state.
    int c = uui_loglist_take_changes(&g_list);
    (void)c;
    update_labels();
    uapp_redraw(a);
}

static int g_ticks;

static int on_tick(struct uapp *a) {
    int redraw = uui_toolbar_tick(&g_tb);
    // THE LIVE VIEW RE-READS ONCE A SECOND; a stored log is finished.
    if (viewing_live() && ++g_ticks >= 4) {
        g_ticks = 0;
        reload();
        redraw = 1;
    }
    if (redraw) uapp_redraw(a);
    return 1;
}

static void on_open(struct uapp *a) {
    layout_all(uapp_width(a), uapp_height(a));
    reload();
    ulogf("logview: open lines %d errors %s sources %d\n", g_set.count, g_st_err, g_nsrc);
}

static void on_resize(struct uapp *a, int w, int h) {
    (void)a;
    layout_all(w, h);
}

static void on_size(int *w, int *h) {
    *w = per() * 120;
    *h = ugfx_char_h() * 36;
}

int main(int argc, char **argv) {
    ulogset_init(&g_set, g_lines, g_view, MAX_LINES);
    scan_sources();
    load_mutes();
    // `logview <file>`: open that log -- a .log from the File Manager, or
    // one outside /var/log -- listed with the rest and chosen.
    if (argc > 1 && argv[1][0]) {
        int k = -1;
        for (int i = 0; i < g_nsrc; i++) if (!strcmp(g_src[i].path, argv[1])) k = i;
        if (k < 0 && g_nsrc < MAX_SOURCES) {
            const char *name = strrchr(argv[1], '/');
            add_source(name ? name + 1 : argv[1], argv[1], 0);
            g_src_items[g_nsrc - 1] = g_src[g_nsrc - 1].label;
            k = g_nsrc - 1;
        }
        if (k >= 0) g_source_index = k;
    }

    uui_menubar_init(&g_menu, menu_items, (int)(sizeof menu_items / sizeof menu_items[0]));
    g_menu.item_flags = item_flags;
    uui_toolbar_init(&g_tb, tb_items, (int)(sizeof tb_items / sizeof tb_items[0]));
    g_tb.item_flags = item_flags;
    uui_dropdown_init(&g_source, 0, 0, 0, 0, g_src_items, g_nsrc);
    uui_dropdown_set_selected(&g_source, g_source_index);
    update_labels();
    uui_segmented_init(&g_show, g_show_items, 3, 0);
    uui_segmented_init(&g_from, FROM_ITEMS, 3, 0);
    uui_textbox_init(&g_search, "");
    g_search.placeholder = "Filter lines";
    uui_loglist_init(&g_list, &g_set);
    g_list.follow = viewing_live();     // a finished log opens at its top
    uui_button_init(&g_around, 0, 0, 0, 0, "Show the lines around it", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_AROUND);
    uui_button_init(&g_only, 0, 0, 0, 0, g_only_lbl, UTHEME_BUTTON_BG, UTHEME_TEXT, ID_ONLY);
    uui_button_init(&g_mute, 0, 0, 0, 0, g_mute_lbl, UTHEME_BUTTON_BG, UTHEME_TEXT, ID_MUTE);
    uui_button_init(&g_copyline, 0, 0, 0, 0, "Copy line", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_COPYLINE);
    uui_button_init(&g_unmute, 0, 0, 0, 0, g_unmute_lbl, UTHEME_BUTTON_BG, UTHEME_TEXT, ID_UNMUTE);
    g_around.outlined = g_only.outlined = g_mute.outlined = g_copyline.outlined = g_unmute.outlined = 1;

    uui_statusbar_init(&g_status);
    g_status.panes[0].text = g_st_lines;  g_status.panes[0].chars = 18;
    g_status.panes[1].text = g_st_err;    g_status.panes[1].chars = 12;
    g_status.panes[2].text = g_st_warn;   g_status.panes[2].chars = 13;
    g_status.panes[3].text = g_st_src;    g_status.panes[3].chars = 18;
    g_status.panes[4].text = g_st_follow; g_status.panes[4].chars = 0;
    g_status.count = 5;

    uui_focus_init(&g_focus, g_ring, (int)(sizeof g_ring / sizeof g_ring[0]));
    uui_focus_set(&g_focus, RING_LIST);

    struct uapp_desc desc = {
        .title        = "Log Viewer",
        .app_id       = "logview",
        .flags        = UAPP_RESIZABLE,
        .on_size      = on_size,
        .min_w        = 640,
        .min_h        = 400,
        .widgets      = g_widgets,
        .widget_count = W_COUNT,
        .focus        = &g_focus,
        .on_open      = on_open,
        .on_draw      = on_draw,
        .on_widget    = on_widget,
        .on_action    = on_action,
        .on_key       = on_key,
        .on_resize    = on_resize,
        .on_tick      = on_tick,
        .tick_ms      = 250,
    };
    return uapp_run(&desc);
}
