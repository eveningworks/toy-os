// Counter -- the worked example of docs/gui-app-tutorial.md, kept in the
// tree so it BUILDS: a tutorial whose code is only in prose describes an
// API that no longer exists within a month (docs/uapp-design.md's sketch
// did). Change this file and the tutorial together.
//
// A label, a bar drawn by hand, and two buttons in a row: the three
// things most apps are made of, laid out by the toolkit and routed by it.
#include <stdio.h>
#include "ui/uapp.h"
#include "ui/uui.h"
#include "ui/uui_label.h"   // not in the umbrella header
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/ulog.h"

#define GOAL 10          // the bar is full at this count

static int g_count;

// --- the widgets ---------------------------------------------------------

static char g_text[32] = "Clicked 0 times";
static struct uui_label g_label;
static struct uui_custom g_bar;
static struct uui_button g_add, g_reset;

enum { ID_ADD = 1, ID_RESET };

// A ROW of the two buttons, inside a COLUMN of label, bar and row. The
// row is itself an item of the column, which is how layouts nest.
static struct uui_item g_row_items[] = {
    { .ops = &uui_button_ops, .widget = &g_add,   .id = ID_ADD,   .name = "add" },
    { .ops = &uui_button_ops, .widget = &g_reset, .id = ID_RESET, .name = "reset" },
};
static struct uui_layout g_row = {
    .dir = UUI_ROW, .items = g_row_items,
    .count = sizeof g_row_items / sizeof g_row_items[0],
};
static struct uui_item g_items[] = {
    { .ops = &uui_label_ops,  .widget = &g_label, .name = "label" },
    { .ops = &uui_custom_ops, .widget = &g_bar,   .name = "bar", .flags = UUI_FILL_W | UUI_FILL_H },
    { .ops = &uui_layout_ops, .widget = &g_row,   .name = "row" },
};
static struct uui_layout g_root = {
    .dir = UUI_COLUMN, .items = g_items,
    .count = sizeof g_items / sizeof g_items[0],
};

// --- drawing -------------------------------------------------------------

// The bar: an outlined track, filled in the accent in proportion to the
// count. Everything in the widget's OWN rectangle -- the layout decides
// where that is.
static void draw_bar(struct ugfx_surface *s, const struct uui_custom *c) {
    ugfx_fill_rect(s, c->x, c->y, c->w, c->h, UTHEME_OUTLINE);
    ugfx_fill_rect(s, c->x + 1, c->y + 1, c->w - 2, c->h - 2, UTHEME_PANEL_BG);
    int n = g_count > GOAL ? GOAL : g_count;
    int fill = (c->w - 4) * n / GOAL;
    if (fill > 0) ugfx_fill_rect(s, c->x + 2, c->y + 2, fill, c->h - 4, UTHEME_ACCENT);
}

// --- behaviour -----------------------------------------------------------

static void set_count(struct uapp *a, int n) {
    g_count = n;
    snprintf(g_text, sizeof g_text, "Clicked %d time%s", n, n == 1 ? "" : "s");
    uui_label_set_text(&g_label, g_text);
    ulogf("counter: count %d\n", n);     // what a test reads
    uapp_redraw(a);
}

// A button COMMITS on release, over the same button it was pressed on --
// docs/gui-guidelines.md's rule, which the library enforces -- and the
// library hands over the button's code. A press or a hover never reaches
// the app.
static void on_action(struct uapp *a, int code) {
    if (code == ID_ADD) set_count(a, g_count + 1);
    else if (code == ID_RESET) set_count(a, 0);
}

// Keys the app owns. Esc closes nothing; Alt+F4 is the window manager's.
static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    if (key == '+' || key == ' ') set_count(a, g_count + 1);
    else if (key == 'r' || key == 'R') set_count(a, 0);
}

int main(void) {
    // Metrics come from the font, so load it before sizing anything.
    if (!ugfx_font_init()) return 2;

    uui_label_init(&g_label, g_text);
    uui_button_init(&g_add, 0, 0, 0, 0, "+1", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_ADD);
    uui_button_init(&g_reset, 0, 0, 0, 0, "Reset", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_RESET);
    g_add.outlined = g_reset.outlined = 1;

    // A custom item's natural size is whatever it holds before layout:
    // font-derived, never a pixel constant.
    g_bar.w = ugfx_text_width("Clicked 10 times") * 2;
    g_bar.h = ugfx_char_h() * 2;
    g_bar.draw = draw_bar;

    struct uapp_desc desc = {
        .title = "Counter",
        .app_id = "counter",
        .layout = &g_root,          // sizes and draws the window
        .widgets = g_items,         // routes the mouse, nested rows included
        .widget_count = sizeof g_items / sizeof g_items[0],
        .on_action = on_action,
        .on_key = on_key,
        .flags = UAPP_RESIZABLE,
    };
    return uapp_run(&desc);
}
