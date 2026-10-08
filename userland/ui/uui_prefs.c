// An app's Options window -- see ui/uui_prefs.h.
#include "ui/uui_prefs.h"

#include <string.h>

#include "ui/uapp.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uui_button.h"
#include "ui/uui_checkbox.h"
#include "ui/uui_focus.h"
#include "ui/uui_gallery.h"
#include "ui/uui_label.h"
#include "ui/uui_layout.h"
#include "ui/uui_route.h"
#include "ui/uui_segmented.h"
#include "ui/uui_sidebar.h"
#include "ui/uui_spinbox.h"

enum { ID_PAGES = UUI_PREFS_ID_BASE, ID_DEFAULTS, ID_OK, ID_CANCEL, ID_BOUND };

static struct uui_prefs_desc g_d;
static struct uapp_window *g_win;

static struct {
    int page;
    struct uui_label caption;
    struct uui_item items[3];
    struct uui_layout row;
} g_row[UUI_PREFS_ROWS_MAX];
static int g_rows;

// The bound rows' controls, owned here (uui_prefs.h).
enum { B_CHECK, B_CHOICE, B_NUMBER };
static struct {
    int kind;
    size_t field;
    const int *values;
    int count;
    struct uui_checkbox check;
    struct uui_segmented choice;
    struct uui_spinbox number;
} g_bound[UUI_PREFS_ROWS_MAX];
static int g_nbound;

static struct uui_sidebar g_pages;
static struct uui_label g_heading, g_spacer;
static char g_heading_text[48];
static struct uui_button g_defaults, g_ok, g_cancel;

static struct uui_item g_page_items[1 + UUI_PREFS_ROWS_MAX], g_body_items[2], g_button_items[4],
                       g_root_items[2];
static struct uui_layout g_page_l, g_body_l, g_button_l, g_root_l;
// The pages, OK and Cancel, then up to three controls a row.
static struct uui_focusable g_focusables[3 + 3 * UUI_PREFS_ROWS_MAX];
static int g_nfocus;
static struct uui_focus g_focus;

static void show_page(int page) {
    if (page < 0 || page >= g_d.page_count) return;
    strlcpy(g_heading_text, g_d.pages[page].label, sizeof g_heading_text);
    for (int i = 0; i < g_rows; i++) g_page_items[1 + i].hidden = g_row[i].page != page;
}

// THE WINDOW FITS ITS LARGEST PAGE, not the one showing --
// QStackedWidget's sizeHint. The caller's w/h is a floor: a segmented row
// is as wide as its labels, and under a fixed width the last ones ran
// off the window's edge. Leaves no page shown; the caller picks one.
static void fit_pages(int *w, int *h) {
    for (int p = 0; p < g_d.page_count; p++) {
        show_page(p);
        int nw = 0, nh = 0;
        uui_layout_natural_size(&g_root_l, &nw, &nh);
        if (nw > *w) *w = nw;
        if (nh > *h) *h = nh;
    }
}

static int *field_of(int b) { return (int *)((char *)g_d.edit + g_bound[b].field); }

static void bound_to_controls(void) {
    for (int b = 0; b < g_nbound; b++) {
        int v = *field_of(b);
        switch (g_bound[b].kind) {
        case B_CHECK: g_bound[b].check.checked = v != 0; break;
        case B_NUMBER: uui_spinbox_set_value(&g_bound[b].number, v); break;
        case B_CHOICE:
            g_bound[b].choice.selected = 0;
            for (int i = 0; i < g_bound[b].count; i++)
                if ((g_bound[b].values ? g_bound[b].values[i] : i) == v) g_bound[b].choice.selected = i;
            break;
        }
    }
}

static void bound_from_controls(void) {
    for (int b = 0; b < g_nbound; b++) {
        switch (g_bound[b].kind) {
        case B_CHECK: *field_of(b) = g_bound[b].check.checked; break;
        case B_NUMBER:
            uui_spinbox_commit(&g_bound[b].number);
            *field_of(b) = uui_spinbox_value(&g_bound[b].number);
            break;
        case B_CHOICE: {
            int i = g_bound[b].choice.selected;
            if (i < 0 || i >= g_bound[b].count) break;   // nothing chosen: unchanged
            *field_of(b) = g_bound[b].values ? g_bound[b].values[i] : i;
            break;
        }
        }
    }
}

void uui_prefs_begin(const struct uui_prefs_desc *d) {
    g_d = *d;
    if (g_d.page_count > UUI_PREFS_PAGES_MAX) g_d.page_count = UUI_PREFS_PAGES_MAX;
    g_rows = 0;
    g_nbound = 0;
    g_nfocus = 3;   // [0] the pages, [1] OK, [2] Cancel -- filled at open
}

int uui_prefs_row(int page, const char *caption, const struct uui_widget_ops *ops,
                  void *widget, int id, const char *name, unsigned flags) {
    if (g_rows >= UUI_PREFS_ROWS_MAX) return -1;
    int i = g_rows++;
    g_row[i].page = page;
    uui_label_init(&g_row[i].caption, caption);
    g_row[i].items[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_row[i].caption,
                                           .flags = UUI_FILL_H,
                                           .main_size = ugfx_char_advance('n') * 20 };
    g_row[i].items[1] = (struct uui_item){ .ops = ops, .widget = widget, .id = id, .name = name,
                                           .flags = flags };
    g_row[i].row = (struct uui_layout){ .dir = UUI_ROW, .items = g_row[i].items, .count = 2,
                                        .margin = 1, .gap = 16 };
    // CARDS GET THE PAGE'S WIDTH, under their caption: beside it they fit
    // two to a row and the page outgrows the window.
    if (ops == &uui_gallery_ops) {
        g_row[i].row.dir = UUI_COLUMN;
        g_row[i].items[0].flags = 0;
        g_row[i].items[0].main_size = 0;
    }
    g_page_items[1 + i] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_row[i].row,
                                             .flags = UUI_FILL_W };
    g_focusables[g_nfocus++] = (struct uui_focusable){ widget, ops };
    return i;
}

static int bound(int kind, size_t field) {
    if (!g_d.edit || g_nbound >= UUI_PREFS_ROWS_MAX) return -1;
    int b = g_nbound++;
    g_bound[b].kind = kind;
    g_bound[b].field = field;
    g_bound[b].values = 0;
    g_bound[b].count = 0;
    return b;
}

int uui_prefs_check(int page, const char *caption, const char *text, size_t field,
                    const char *name) {
    int b = bound(B_CHECK, field);
    if (b < 0) return -1;
    uui_checkbox_init(&g_bound[b].check, 0, 0, 0, text, UTHEME_WINDOW_BG, UTHEME_TEXT);
    return uui_prefs_row(page, caption, &uui_checkbox_ops, &g_bound[b].check, ID_BOUND + b, name, 0);
}

int uui_prefs_choice(int page, const char *caption, const char *const *labels,
                     const int *values, int count, size_t field, const char *name) {
    int b = bound(B_CHOICE, field);
    if (b < 0) return -1;
    g_bound[b].values = values;
    g_bound[b].count = count;
    uui_segmented_init(&g_bound[b].choice, labels, count, 0);
    return uui_prefs_row(page, caption, &uui_segmented_ops, &g_bound[b].choice, ID_BOUND + b, name, 0);
}

int uui_prefs_number(int page, const char *caption, int lo, int hi, int step, const char *unit,
                     size_t field, const char *name) {
    int b = bound(B_NUMBER, field);
    if (b < 0) return -1;
    uui_spinbox_init(&g_bound[b].number, lo, lo, hi, step, unit);
    return uui_prefs_row(page, caption, &uui_spinbox_ops, &g_bound[b].number, ID_BOUND + b, name, 0);
}

void uui_prefs_also_check(int row, const char *text, size_t field, const char *name) {
    int b = bound(B_CHECK, field);
    if (b < 0) return;
    uui_checkbox_init(&g_bound[b].check, 0, 0, 0, text, UTHEME_WINDOW_BG, UTHEME_TEXT);
    uui_prefs_also(row, &uui_checkbox_ops, &g_bound[b].check, ID_BOUND + b, name);
}

void uui_prefs_widen(int row, int width) {
    if (row >= 0 && row < g_rows) g_row[row].items[1].main_size = width;
}

void uui_prefs_also(int row, const struct uui_widget_ops *ops, void *widget, int id,
                    const char *name) {
    if (row < 0 || row >= g_rows || g_row[row].row.count >= 3) return;
    g_row[row].items[g_row[row].row.count++] =
        (struct uui_item){ .ops = ops, .widget = widget, .id = id, .name = name };
    g_focusables[g_nfocus++] = (struct uui_focusable){ widget, ops };
}

int uui_prefs_is_open(void) { return g_win != 0; }

void uui_prefs_close(void) {
    if (!g_win) return;
    uapp_window_close(g_win);
    g_win = 0;
    if (g_d.on_close) g_d.on_close();
}

void uui_prefs_redraw(void) {
    if (g_win) uapp_window_redraw(g_win);
}

static void on_widget(struct uapp_window *w, int id, int reason) {
    if (id == ID_PAGES) show_page(uui_sidebar_selected_id(&g_pages));
    else if (g_d.on_widget) g_d.on_widget(id, reason);
    uapp_window_redraw(w);
}

static void keep(void) {
    bound_from_controls();
    void (*ok)(void) = g_d.on_ok;
    uui_prefs_close();   // FIRST: what OK applies may resize or redraw the app
    if (ok) ok();
}

static void on_action(struct uapp_window *w, int code) {
    if (code == ID_DEFAULTS) {
        if (g_d.edit_defaults && g_d.edit) {
            g_d.edit_defaults(g_d.edit);
            bound_to_controls();
        }
        if (g_d.on_defaults) g_d.on_defaults();
    } else if (code == ID_OK) {
        keep();
        return;
    } else if (code == ID_CANCEL) {
        uui_prefs_close();
        return;
    } else if (g_d.on_action) {
        g_d.on_action(code);
    }
    uapp_window_redraw(w);
}

static void on_key(struct uapp_window *w, int key, unsigned mods) {
    (void)mods;
    if (key == 0x1B) { uui_prefs_close(); return; }
    if (key == '\n' || key == '\r') { keep(); return; }
    uapp_window_redraw(w);
}

static void on_close(struct uapp_window *w) {
    (void)w;
    uui_prefs_close();
}

int uui_prefs_open(struct uapp *a) {
    if (g_win || g_d.page_count <= 0) return 0;
    bound_to_controls();

    uui_sidebar_init(&g_pages, 0, 0, 0, 0, g_d.pages, g_d.page_count);
    g_pages.sel_bg = UTHEME_ACCENT;   // the page you are on
    g_pages.sel_fg = UTHEME_ACCENT_TEXT;
    uui_label_init(&g_heading, g_heading_text);
    g_heading.font = ugfx_font_session(UGFX_FONT_BOLD);
    uui_label_init(&g_spacer, "");
    uui_button_init(&g_defaults, 0, 0, 0, 0, "Defaults", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_DEFAULTS);
    uui_button_init(&g_ok, 0, 0, 0, 0, "OK", UTHEME_ACCENT, UTHEME_ACCENT_TEXT, ID_OK);
    uui_button_init(&g_cancel, 0, 0, 0, 0, "Cancel", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_CANCEL);

    g_focusables[0] = (struct uui_focusable){ &g_pages, &uui_sidebar_ops };
    g_focusables[1] = (struct uui_focusable){ &g_ok, &uui_button_ops };
    g_focusables[2] = (struct uui_focusable){ &g_cancel, &uui_button_ops };
    g_page_items[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_heading,
                                         .flags = UUI_FILL_W, .name = "heading" };
    int first = g_d.first_page >= 0 && g_d.first_page < g_d.page_count ? g_d.first_page : 0;
    show_page(first);
    uui_sidebar_select_id(&g_pages, first);

    g_page_l = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_page_items, .count = 1 + g_rows,
                                    .margin = 12, .gap = 10 };
    g_body_items[0] = (struct uui_item){ .ops = &uui_sidebar_ops, .widget = &g_pages, .id = ID_PAGES,
                                         .flags = UUI_FILL_H, .name = "pages",
                                         .main_size = ugfx_char_advance('n') * 22 };
    g_body_items[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_page_l,
                                         .flags = UUI_FILL_W | UUI_FILL_H };
    g_body_l = (struct uui_layout){ .dir = UUI_ROW, .items = g_body_items, .count = 2, .margin = 1 };
    g_button_items[0] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_defaults,
                                           .id = ID_DEFAULTS, .name = "defaults" };
    g_button_items[1] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_spacer,
                                           .flags = UUI_FILL_W };
    g_button_items[2] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_ok,
                                           .id = ID_OK, .name = "ok" };
    g_button_items[3] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_cancel,
                                           .id = ID_CANCEL, .name = "cancel" };
    g_button_l = (struct uui_layout){ .dir = UUI_ROW, .items = g_button_items, .count = 4,
                                      .margin = 10 };
    g_root_items[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_body_l,
                                         .flags = UUI_FILL_W | UUI_FILL_H };
    g_root_items[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_button_l,
                                         .flags = UUI_FILL_W };
    g_root_l = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_root_items, .count = 2,
                                    .margin = 1, .gap = 1 };
    uui_focus_init(&g_focus, g_focusables, g_nfocus);
    uui_focus_set(&g_focus, 0);

    int w = g_d.w > 0 ? g_d.w : ugfx_char_advance('n') * 96;
    int h = g_d.h > 0 ? g_d.h : ugfx_char_h() * 30;
    fit_pages(&w, &h);
    show_page(first);

    struct uapp_window_desc d = {
        .title = g_d.title,
        .w = w,
        .h = h,
        .flags = UAPP_WIN_MODAL,
        .widgets = g_root_items,
        .widget_count = 2,
        .layout = &g_root_l,
        .focus = &g_focus,
        .on_widget = on_widget,
        .on_action = on_action,
        .on_key = on_key,
        .on_close = on_close,
        .log_prefix = g_d.log_prefix,
    };
    g_win = uapp_window_open(a, &d);
    return g_win != 0;
}
