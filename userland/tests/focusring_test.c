// EVERY WIDGET THAT ACCEPTS FOCUS DRAWS AN INDICATOR, asserted as pixels.
//
// WHY THIS IS A RING-3 TEST AND NOT A KTEST. The toolkit lives entirely
// in ring 3 and its measurements need a loaded FONT (ugfx_font_init()),
// which a KTEST cannot reach -- the same reason wrap_test.c exists.
//
// WHY IT IS NOT A GUI TOOL EITHER. Only three of these widgets are in
// any app's focus ring today, so a screenshot-driven check could cover
// three; drawing into a plain surface covers all of them, in a few
// milliseconds, with no compositor involved.
//
// EACH WIDGET IS ASSERTED BOTH WAYS -- focusing it must ADD accent
// pixels that were not there unfocused. A one-sided check passes on a
// widget that rings itself unconditionally, which is a control that lies
// about holding the keyboard rather than one that says nothing. It is a
// DIFFERENCE, not "no accent at all when unfocused", because a chosen
// radio, an on switch and a selected segment wear the accent too.
//
// AND THE ROW WIDGETS ARE ASSERTED ON THE RING'S HEIGHT, which is what
// tells "on the selected row" from "round the whole box" -- both draw
// accent pixels and only one of them is the behaviour.
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "rt/sys.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uui_widget.h"
#include "ui/uui_checkbox.h"
#include "ui/uui_dropdown.h"
#include "ui/uui_fileview.h"
#include "ui/uui_listbox.h"
#include "ui/uui_radio_list.h"
#include "ui/uui_sidebar.h"
#include "ui/uui_slider.h"
#include "ui/uui_spinbox.h"
#include "ui/uui_table.h"
#include "ui/uui_textbox.h"
#include "ui/uui_tree.h"
#include "ui/uui_switch.h"
#include "ui/uui_segmented.h"
#include "ui/uui_button.h"

#define SURF_W 320
#define SURF_H 240

static uint32_t g_px[SURF_W * SURF_H];
static uint32_t g_unfocused[SURF_W * SURF_H];
static struct ugfx_surface g_surf;
// UTEST_VERDICT_FILE carries the per-check lines rather than only the
// count -- a spawned program's console output arrives while the harness
// is between commands and is dropped, so "0 failure(s)" alone would be
// the only thing anyone could ever read back. See wrap_test.c on why
// the file exists at all.
#include "lib/utest.h"

// The call sites here read `ok(name, cond, detail)`; the harness takes
// the boolean first. One adapter rather than transposing every call
// site: a transposed argument pair compiles and INVERTS the check,
// which is the failure a green suite hides.
static void ok(const char *name, int cond, const char *detail) {
    utest_check_detail(cond, name, detail);
}

// The accent pixels' bounding box. Returns the count; `h` is the box's
// height, which is the discriminating measurement for a row widget.
static int accent_box(int *out_h) {
    int n = 0, y0 = SURF_H, y1 = -1;
    for (int y = 0; y < SURF_H; y++)
        for (int x = 0; x < SURF_W; x++)
            if (g_px[y * SURF_W + x] == UTHEME_ACCENT) {
                n++;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
    if (out_h) *out_h = (y1 >= y0) ? (y1 - y0 + 1) : 0;
    return n;
}

// Draw the widget through its OPS TABLE, not its own draw function: the
// ops table is what the toolkit actually calls, and a widget whose
// `draw` slot points somewhere else would pass a direct call.
static int paint(const struct uui_widget_ops *ops, void *w, int focused, int *out_h) {
    if (ops->set_focused) ops->set_focused(w, focused);
    for (int i = 0; i < SURF_W * SURF_H; i++) g_px[i] = 0x00202020;
    ops->draw(&g_surf, w);
    return accent_box(out_h);
}

// The pair every widget below is held to.
static void check(const char *name, const struct uui_widget_ops *ops, void *w) {
    char detail[96];
    int off = paint(ops, w, 0, 0);
    memcpy(g_unfocused, g_px, sizeof g_px);
    int on = paint(ops, w, 1, 0);
    int added = 0;
    for (int i = 0; i < SURF_W * SURF_H; i++)
        if (g_px[i] == UTHEME_ACCENT && g_unfocused[i] != UTHEME_ACCENT) added++;
    snprintf(detail, sizeof detail, "%s: focusing added %d accent px", name, added);
    ok(name, added > 0, detail);
    if (added <= 0) utest_notef("(%s: unfocused %d, focused %d)", name, off, on);
    ops->set_focused(w, 0);
}

// --- fixtures ----------------------------------------------------------

static const char *const ITEMS[] = { "alpha", "bravo", "charlie", "delta" };

static void cell_fn(void *ctx, int row, int col, char *out, int cap) {
    (void)ctx; (void)col;
    snprintf(out, cap, "row%d", row);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    // Without this every metric is zero and every check below passes for
    // the wrong reason -- see uapp.c on the font not being free in ring 3.
    ugfx_font_init();

    g_surf.pixels = g_px;
    g_surf.w = SURF_W;
    g_surf.h = SURF_H;

    utest_begin("focusring_test", "focus indicators", UTEST_VERDICT_FILE);
    ok("the font loaded, so metrics are real", ugfx_char_w() > 0, "char_w is 0");

    char detail[96];

    // --- the four that already drew one -------------------------------
    struct uui_checkbox cb;
    uui_checkbox_init(&cb, 10, 10, 0, "Alpha", 0x00202020, 0x00e0e0e0);
    check("uui_checkbox", &uui_checkbox_ops, &cb);

    static struct uui_radio_list rl;
    memset(&rl, 0, sizeof rl);
    rl.options = ITEMS; rl.count = 4; rl.cols = 1; rl.selected = 1;
    rl.armed_prev = -1; rl.hovered = -1;
    rl.bg = 0x00202020; rl.fg = 0x00e0e0e0;
    uui_radio_list_set_geometry(&rl, 10, 10);
    check("uui_radio_list", &uui_radio_list_ops, &rl);

    static struct uui_textbox tb;
    uui_textbox_init(&tb, "hello");
    uui_textbox_set_geometry(&tb, 10, 10, 200, 24);
    check("uui_textbox", &uui_textbox_ops, &tb);

    static struct uui_dropdown dd;
    uui_dropdown_init(&dd, 10, 10, 200, 24, ITEMS, 4);
    check("uui_dropdown", &uui_dropdown_ops, &dd);

    // --- the seven that did not ---------------------------------------
    struct uui_slider sl;
    uui_slider_init(&sl, ITEMS, 4);
    uui_slider_set_geometry(&sl, 10, 10, 200, 40);
    check("uui_slider", &uui_slider_ops, &sl);

    static struct uui_spinbox sp;
    uui_spinbox_init(&sp, 50, 0, 100, 5, "%");
    uui_spinbox_set_geometry(&sp, 10, 10, 120, 24);
    check("uui_spinbox", &uui_spinbox_ops, &sp);

    static struct uui_listbox lb;
    uui_listbox_init(&lb, 10, 10, 200, 120, ITEMS, 4);
    lb.selected = 2;
    check("uui_listbox", &uui_listbox_ops, &lb);

    struct uui_table_column cols[] = { { "Name", 10, 0 }, { "Value", 0, 0 } };
    static struct uui_table tt;
    uui_table_init(&tt, 10, 10, 260, 140, cols, 2, cell_fn, 0);
    uui_table_set_rows(&tt, 6);
    tt.selected = 2;
    check("uui_table", &uui_table_ops, &tt);

    struct uui_tree_node nodes[] = {
        { "root", 0, 1, UUI_TREE_AUTO, 0 }, { "child", 1, 2, UUI_TREE_AUTO, 0 },
        { "other", 0, 3, UUI_TREE_AUTO, 0 },
    };
    static struct uui_tree tr;
    uui_tree_init(&tr, 10, 10, 200, 120, nodes, 3);
    tr.selected = 1;
    check("uui_tree", &uui_tree_ops, &tr);

    struct uui_sidebar_row rows[] = {
        { "Section", UUI_SIDEBAR_HEADING, 0, 0 },
        { "One", UUI_SIDEBAR_ITEM, 0, 1 },
        { "Two", UUI_SIDEBAR_ITEM, 0, 2 },
    };
    static struct uui_sidebar sb;
    uui_sidebar_init(&sb, 10, 10, 160, 120, rows, 3);
    check("uui_sidebar", &uui_sidebar_ops, &sb);

    // --- the ones that joined with System Settings' redesign ----------
    static struct uui_switch swc;
    uui_switch_init(&swc, 1);
    uui_switch_set_geometry(&swc, 10, 10);
    check("uui_switch", &uui_switch_ops, &swc);

    static struct uui_segmented seg;
    uui_segmented_init(&seg, ITEMS, 3, 1);
    uui_segmented_set_geometry(&seg, 10, 10);
    check("uui_segmented", &uui_segmented_ops, &seg);

    static struct uui_button btn;
    uui_button_init(&btn, 10, 10, 90, 24, "Apply", UTHEME_ACCENT, UTHEME_ACCENT_TEXT, 1);
    check("uui_button, even an accent one", &uui_button_ops, &btn);
    btn.disabled = 1;
    ok("a disabled uui_button refuses focus", !uui_button_ops.accepts_focus(&btn),
       "the ring would park on it");

    static struct uui_fileview fv;
    memset(&fv, 0, sizeof fv);
    ok("uui_fileview forwards set_focused to its table",
          uui_fileview_ops.set_focused != 0
              && (uui_fileview_ops.set_focused(&fv, 1), fv.table.focused == 1)
              && (uui_fileview_ops.set_focused(&fv, 0), fv.table.focused == 0),
          "the table was not told");

    // --- THE ROW RULE -------------------------------------------------
    //
    // A ring on the selected ROW and a ring round the WHOLE BOX both put
    // accent pixels on the surface. Only the height tells them apart,
    // and getting it wrong is the difference between "this row" and "this
    // widget", which for a 120px list is the entire point.
    int h;
    lb.selected = 2;
    paint(&uui_listbox_ops, &lb, 1, &h);
    snprintf(detail, sizeof detail, "ring is %d px tall, row is %d",
             h, uui_listbox_row_h(&lb));
    ok("uui_listbox rings the selected ROW", h == uui_listbox_row_h(&lb), detail);

    // ...AND FALLS BACK TO THE BOX when there is no selected row to ring.
    // An indicator that disappears when the selection does is the bug
    // this whole change exists to remove.
    lb.selected = -1;
    paint(&uui_listbox_ops, &lb, 1, &h);
    snprintf(detail, sizeof detail, "ring is %d px tall, box is %d", h, lb.h);
    ok("...and the whole box with nothing selected", h == lb.h, detail);

    tt.selected = 2;
    paint(&uui_table_ops, &tt, 1, &h);
    snprintf(detail, sizeof detail, "ring is %d px tall, row is %d",
             h, uui_table_row_h(&tt));
    ok("uui_table rings the selected ROW", h == uui_table_row_h(&tt), detail);

    tr.selected = 1;
    paint(&uui_tree_ops, &tr, 1, &h);
    snprintf(detail, sizeof detail, "ring is %d px tall, row is %d",
             h, uui_tree_row_h(&tr));
    ok("uui_tree rings the selected ROW", h == uui_tree_row_h(&tr), detail);

    paint(&uui_sidebar_ops, &sb, 1, &h);
    snprintf(detail, sizeof detail, "ring is %d px tall, row is %d",
             h, uui_sidebar_row_h(&sb));
    ok("uui_sidebar rings the selected ROW", h == uui_sidebar_row_h(&sb), detail);

    return utest_end();
}
