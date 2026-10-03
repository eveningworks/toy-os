// Option List demo -- a known target for ui/uui_optlist.h, in the shape
// UI Demo is for the older widgets: a window holding one optlist taller
// than it (so the scrollbar is live) and a status line, reporting every
// change as one parseable line on stderr.
//
// LOG GRAMMAR, prefix "optdemo: ":
//   optdemo: open
//   optdemo: change <row> <on|value|select>... on=<0|1> value="<text>"
// The widget's rects come from the layout log (uapp_log_layout()):
// "optdemo: layout options.check <row> x y w h" and ".value <row> ...".
#include <stdio.h>
#include <string.h>
#include "ui/uapp.h"
#include "ui/uui.h"
#include "ui/uui_label.h"
#include "ui/uui_optlist.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/ulog.h"

enum { ID_OPTIONS = 1 };

// Real kernel words (docs/boot-flags.md), so the demo shows what the
// widget was drawn for; nothing in the widget knows them.
static struct uui_optlist_item g_opts[] = {
    { .name = "video",     .has_value = 1, .hint = "<W>x<H>",
      .desc = "Screen size from a modesetting display driver" },
    { .name = "nokaslr",   .desc = "Run the kernel where it was linked" },
    { .name = "loglevel",  .has_value = 1, .hint = "0-7", .on = 1, .value = "4",
      .desc = "How much of the kernel log reaches the console" },
    { .name = "debugcon",  .desc = "The serial debug console on COM2" },
    { .name = "root",      .has_value = 1, .hint = "<device>",
      .desc = "Which device carries the root filesystem" },
    { .name = "novirtio",  .desc = "Keep the filesystem on the ATA disk" },
    { .name = "noahci",    .desc = "Keep the filesystem off the SATA drive" },
    { .name = "nonvme",    .desc = "Register no NVMe namespace" },
    { .name = "notrim",    .desc = "Stop every block backend discarding" },
    { .name = "nousb",     .desc = "Skip xHCI bring-up entirely" },
    { .name = "notsc",     .desc = "Keep the TSC from being the clocksource" },
    { .name = "panic",     .has_value = 1, .hint = "<seconds>",
      .desc = "Restart this long after a kernel panic" },
    { .name = "target",    .has_value = 1, .hint = "text|graphical|rescue",
      .desc = "The startup target for this boot only" },
    { .name = "kbd",       .has_value = 1, .hint = "<name>",
      .desc = "A keyboard layout for this boot only" },
};
#define OPT_COUNT ((int)(sizeof g_opts / sizeof g_opts[0]))

static struct uui_optlist g_list;
static char g_status[96] = "ready";
static struct uui_label g_label;

static struct uui_item g_items[] = {
    { .ops = &uui_optlist_ops, .widget = &g_list, .id = ID_OPTIONS,
      .flags = UUI_FILL_W | UUI_FILL_H, .name = "options" },
    { .ops = &uui_label_ops, .widget = &g_label, .flags = UUI_FILL_W, .name = "status" },
};
static struct uui_layout g_root = {
    .dir = UUI_COLUMN, .items = g_items,
    .count = sizeof g_items / sizeof g_items[0],
};

static struct uui_focusable g_focus_items[] = {
    { &g_list, &uui_optlist_ops },
};
static struct uui_focus g_focus;

static void drain(struct uapp *a) {
    int row, what;
    while ((what = uui_optlist_take_change(&g_list, &row))) {
        const struct uui_optlist_item *it = &g_opts[row];
        snprintf(g_status, sizeof g_status, "change %d%s%s%s on=%d value=\"%s\"", row,
                 what & UUI_OPTLIST_CH_ON ? " on" : "",
                 what & UUI_OPTLIST_CH_VALUE ? " value" : "",
                 what & UUI_OPTLIST_CH_SELECT ? " select" : "",
                 it->on, it->value);
        ulogf("optdemo: %s\n", g_status);
        uui_label_set_text(&g_label, g_status);
        uapp_redraw(a);
    }
}

static void on_widget(struct uapp *a, int id, int reason) {
    (void)id; (void)reason;
    drain(a);
}

// A commit by focus leaving reaches no on_widget (uui_optlist.h).
static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)key; (void)mods;
    drain(a);
}

static void on_open(struct uapp *a) {
    (void)a;
    ulogf("optdemo: open\n");
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    (void)d;
    uapp_log_layout(a, "optdemo");
}

// As wide as the list wants, eight rows tall: fewer than it holds, so
// the scrollbar is always there to exercise.
static void on_size(int *w, int *h) {
    int nw;
    uui_optlist_natural_size(&g_list, &nw, 0);
    *w = nw + 2 * ugfx_char_w();
    *h = 8 * uui_optlist_row_h(&g_list) + 3 * ugfx_char_h();
}

int main(void) {
    if (!ugfx_font_init()) return 2;
    uui_optlist_init(&g_list, g_opts, OPT_COUNT);
    uui_label_init(&g_label, g_status);
    uui_focus_init(&g_focus, g_focus_items, 1);

    struct uapp_desc desc = {
        .title = "Option List",
        .app_id = "optdemo",
        .layout = &g_root,
        .widgets = g_items,
        .widget_count = sizeof g_items / sizeof g_items[0],
        .focus = &g_focus,
        .on_widget = on_widget,
        .on_key = on_key,
        .on_open = on_open,
        .on_draw = on_draw,
        .on_size = on_size,
        .flags = UAPP_RESIZABLE,
    };
    return uapp_run(&desc);
}
