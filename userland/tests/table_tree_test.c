// uui_table's groups, tree and heat -- the orderings Task Manager's
// Processes page is built from. A ring-3 test because the toolkit is
// ring 3's (see typeahead_test.c).
//
// THE FIXTURE IS STORED OUT OF SCREEN ORDER, deliberately: children
// before their parents, a group's rows scattered, a two-row cycle and a
// parent in a different group. An ordering that walked the app's array
// would pass every check on a fixture already in screen order.
//
// Prints one line per check and exits with the number of failures.
#include <stdint.h>
#include "rt/sys.h"
#include <string.h>
#include <stdio.h>
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "keyboard.h"
#include "ui/uui_table.h"
#include "lib/utest.h"

static void ok(const char *name, int cond, const char *detail) {
    utest_check_detail(cond, name, detail);
}
static void oki(const char *name, int got, int want) {
    char d[64];
    snprintf(d, sizeof d, "got %d, want %d", got, want);
    ok(name, got == want, d);
}

// name, parent (app row), group
static const struct { const char *name; int parent; int group; } ROWS[] = {
    { "zed",   3, 0 },   // 0: a child of beta
    { "init", -1, 2 },   // 1
    { "alpha",-1, 0 },   // 2
    { "beta", -1, 0 },   // 3
    { "kid",   0, 0 },   // 4: beta > zed > kid
    { "svc",   1, 1 },   // 5: parent init is in group 2 -- a root here
    { "cyc_a", 7, 1 },   // 6: a cycle with 7
    { "cyc_b", 6, 1 },   // 7
};
#define N ((int)(sizeof ROWS / sizeof ROWS[0]))
static const char *const TITLES[] = { "Apps", "Services", "System" };

static int g_collapsed[N];
static int g_hot_row = -1;

static void cell(void *ctx, int row, int col, char *out, int cap) {
    (void)ctx;
    if (col == 0) snprintf(out, cap, "%s", ROWS[row].name);
    else snprintf(out, cap, "%d", row);
}
static int cmp(void *ctx, int a, int b, int col) {
    (void)ctx; (void)col;
    return strcmp(ROWS[a].name, ROWS[b].name);
}
static int group_of(void *ctx, int row) { (void)ctx; return ROWS[row].group; }
static void title_of(void *ctx, int g, char *out, int cap) {
    (void)ctx;
    snprintf(out, cap, "%s", TITLES[g]);
}
static int parent_of(void *ctx, int row) { (void)ctx; return ROWS[row].parent; }
static int collapsed_of(void *ctx, int row) { (void)ctx; return g_collapsed[row]; }
static int heat_of(void *ctx, int row, int col) {
    (void)ctx;
    return (row == g_hot_row && col == 1) ? 255 : 0;
}

// The screen, as app rows with a caption as -2 - group: the same
// encoding as `order`, read back through the PUBLIC functions only.
static void screen(const struct uui_table *t, char *out, int cap) {
    int n = 0;
    out[0] = '\0';
    for (int v = 0; v < uui_table_view_count(t) && n < cap - 12; v++) {
        int g = uui_table_caption_at(t, v);
        if (g >= 0) n += snprintf(out + n, cap - n, "%s[%s]", v ? " " : "", TITLES[g]);
        else n += snprintf(out + n, cap - n, "%s%s", v ? " " : "",
                           ROWS[uui_table_source_row(t, v)].name);
    }
}

static void expect_screen(const char *name, const struct uui_table *t, const char *want) {
    char got[256];
    screen(t, got, sizeof got);
    char d[600];
    snprintf(d, sizeof d, "got '%s', want '%s'", got, want);
    ok(name, strcmp(got, want) == 0, d);
}

#define SURF_W 320
#define SURF_H 320
static uint32_t g_px[SURF_W * SURF_H];

static const struct uui_table_column COLS[] = {
    { "Name", 0, UUI_TALIGN_LEFT },
    { "Row",  6, UUI_TALIGN_RIGHT },
};

// The screen y of the middle of view row v.
static int row_mid(const struct uui_table *t, int v) {
    return t->y + uui_table_header_h(t) + v * uui_table_row_h(t) + uui_table_row_h(t) / 2;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    ugfx_font_init();
    utest_begin("table_tree_test", "uui_table groups, tree and heat", UTEST_VERDICT_FILE);
    ok("the font loaded, so metrics are real", ugfx_char_h() > 0, "char_h is 0");

    static struct uui_table t;
    uui_table_init(&t, 0, 0, SURF_W, SURF_H, COLS, 2, cell, 0);
    uui_table_set_compare(&t, cmp);
    uui_table_set_sort(&t, 0, 1);
    uui_table_set_rows(&t, N);

    // --- plain: nothing changes for a table that asks for neither ------
    oki("plain: every row is on screen, and only rows", uui_table_view_count(&t), N);
    oki("plain: no captions", uui_table_caption_at(&t, 0), -1);
    expect_screen("plain: sorted by name", &t,
                  "alpha beta cyc_a cyc_b init kid svc zed");

    // --- groups and the tree -----------------------------------------
    uui_table_set_groups(&t, group_of, title_of);
    uui_table_set_tree(&t, parent_of, collapsed_of);
    expect_screen("groups ascending, children under parents, siblings sorted", &t,
                  "[Apps] alpha beta zed kid [Services] cyc_a cyc_b svc [System] init");
    oki("depth of a grandchild", t.depth[4], 2);
    oki("a parent in another group does not nest (svc is a root)", t.depth[5], 0);
    oki("a cycle is cut: one member becomes a root", t.depth[6] + t.depth[7], 1);
    oki("a caption has no app row", uui_table_source_row(&t, 0), -1);

    uui_table_set_sort(&t, 0, -1);
    expect_screen("reversed: siblings reverse, groups and nesting do not", &t,
                  "[Apps] beta zed kid alpha [Services] svc cyc_a cyc_b [System] init");
    uui_table_set_sort(&t, 0, 1);

    // --- collapse: the app's state, the table's view -------------------
    t.selected = 4;                       // kid
    g_collapsed[3] = 1;                   // beta
    uui_table_set_rows(&t, N);
    expect_screen("a collapsed parent hides its whole subtree", &t,
                  "[Apps] alpha beta [Services] cyc_a cyc_b svc [System] init");
    oki("a hidden row has no view position", uui_table_view_row(&t, 4), -1);
    oki("a selection folded away moves to the visible ancestor", t.selected, 3);

    // --- keys ----------------------------------------------------------
    uui_table_key(&t, KEY_ARROW_RIGHT);
    oki("Right on a collapsed parent asks the app to open it",
        uui_table_take_toggled(&t), 3);
    oki("...and taking it clears it", uui_table_take_toggled(&t), -1);
    g_collapsed[3] = 0;
    uui_table_set_rows(&t, N);
    uui_table_key(&t, KEY_ARROW_RIGHT);
    oki("Right on an open parent steps to its first child", t.selected, 0);
    uui_table_key(&t, KEY_ARROW_LEFT);
    oki("Left on an open parent asks to close it", uui_table_take_toggled(&t), 0);
    t.selected = 4;
    uui_table_key(&t, KEY_ARROW_LEFT);
    oki("Left on a leaf steps to its parent", t.selected, 0);

    t.selected = 4;                       // kid, last row of Apps
    uui_table_key(&t, KEY_ARROW_DOWN);
    oki("Down across a caption lands on the next group's first row", t.selected, 6);
    uui_table_key(&t, KEY_ARROW_UP);
    oki("...and Up across it comes back", t.selected, 4);
    uui_table_key(&t, KEY_HOME);
    oki("Home lands on the first ROW, not the caption", t.selected, 2);
    uui_table_key(&t, KEY_ARROW_UP);
    oki("Up from the first row stays (no caption stop)", t.selected, 2);

    // --- the pointer ---------------------------------------------------
    oki("a caption is not hit", uui_table_hit(&t, 40, row_mid(&t, 0)), -1);
    int cx, cw;
    uui_table_column_rect(&t, 0, &cx, &cw);
    int beta_v = uui_table_view_row(&t, 3);
    t.selected = 2;
    uui_table_click(&t, cx + UUI_TABLE_PAD_X + ugfx_char_h() / 2, row_mid(&t, beta_v));
    oki("a click on the expander asks to toggle that row", uui_table_take_toggled(&t), 3);
    oki("...and selects it", t.selected, 3);
    t.selected = 2;
    uui_table_click(&t, cx + UUI_TABLE_PAD_X + ugfx_char_h() * 3, row_mid(&t, beta_v));
    oki("a click on the name only selects", uui_table_take_toggled(&t), -1);
    oki("...the row", t.selected, 3);

    // --- heat ----------------------------------------------------------
    struct ugfx_surface s = { .pixels = g_px, .w = SURF_W, .h = SURF_H };
    g_hot_row = 2;                         // alpha, column 1
    t.selected = 3;
    uui_table_set_heat(&t, heat_of);
    uui_table_draw(&s, &t);
    int hx, hw;
    uui_table_column_rect(&t, 1, &hx, &hw);
    uint32_t hot  = g_px[row_mid(&t, uui_table_view_row(&t, 2)) * SURF_W + hx + 1];
    uint32_t cold = g_px[row_mid(&t, uui_table_view_row(&t, 6)) * SURF_W + hx + 1];
    char d[80];
    snprintf(d, sizeof d, "hot %06x, cold %06x", (unsigned)(hot & 0xffffff), (unsigned)(cold & 0xffffff));
    ok("a hot cell is shaded", (hot & 0xffffff) != (cold & 0xffffff), d);
    ok("...towards the accent (bluer than it is red)",
       (hot & 0xff) > ((hot >> 16) & 0xff), d);
    t.selected = 2;
    uui_table_draw(&s, &t);
    uint32_t sel = g_px[row_mid(&t, uui_table_view_row(&t, 2)) * SURF_W + hx + 1];
    snprintf(d, sizeof d, "got %06x, selection %06x", (unsigned)(sel & 0xffffff),
             (unsigned)(uui_table_c_sel_bg(&t) & 0xffffff));
    ok("selection outranks heat", (sel & 0xffffff) == (uui_table_c_sel_bg(&t) & 0xffffff), d);

    return utest_end();
}
