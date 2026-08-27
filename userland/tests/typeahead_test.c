// Type-ahead, in the two widgets that have it: uui_listbox and
// uui_table. See ui/uui_seek.h.
//
// WHY THIS IS A RING-3 TEST AND NOT A KTEST. The toolkit is ring 3's
// (userland/ui/); the kernel image contains no widget code at all, so
// a KTEST cannot reach a line of this.
//
// THE CHECKS THAT MATTER ARE THE SORTED ONES. A table's rows are
// PULLED, and the app's row indices are not the order on screen -- so a
// search that walks the app's order cycles through matches in an order
// the user cannot see, while every single-match check still passes.
// SORTED_ROWS below is deliberately stored in the reverse of its
// displayed order for that reason.
//
// The second one to keep: the seek column is DECLARED. Column 0 here is
// a number, exactly as it is in Task Manager, so a build that always
// searched column 0 fails "seek-col-is-declared" and nothing else.
//
// Prints one line per check and exits with the number of failures.
#include <stdint.h>
#include "rt/sys.h"
#include <string.h>
#include <stdio.h>
#include "ui/ugfx.h"
#include "keyboard.h"   // KEY_END
#include "ui/uui_listbox.h"
#include "ui/uui_table.h"

static int g_fail;

static void put(const char *s) { sys_write(1, s, strlen(s)); }

static void ok(const char *name, int cond, const char *detail) {
    put(cond ? "  ok    " : "  FAIL  ");
    put(name);
    if (!cond && detail) { put("   -- "); put(detail); }
    put("\n");
    if (!cond) g_fail++;
}

static void oki(const char *name, int got, int want) {
    char d[64];
    snprintf(d, sizeof d, "got %d, want %d", got, want);
    ok(name, got == want, d);
}

// --- the listbox half --------------------------------------------------

static const char *const CITIES[] = {
    "Halifax", "Hanoi", "Helsinki", "Honolulu", "Oslo", "los angeles",
};
#define CITY_COUNT ((int)(sizeof CITIES / sizeof CITIES[0]))

static void listbox_checks(void) {
    struct uui_listbox lb;
    uui_listbox_init(&lb, 0, 0, 200, 200, CITIES, CITY_COUNT);

    lb.selected = -1;
    uui_listbox_key(&lb, 'h');
    oki("listbox: h reaches Halifax", lb.selected, 0);

    // The same letter again cycles; it must not expire, so this is the
    // one behaviour that has to survive a slow tester.
    uui_listbox_key(&lb, 'h');
    oki("listbox: h again reaches Hanoi", lb.selected, 1);
    uui_listbox_key(&lb, 'h');
    oki("listbox: h again reaches Helsinki", lb.selected, 2);

    // A grown prefix searches from the top, so it may go BACKWARDS.
    uui_listbox_init(&lb, 0, 0, 200, 200, CITIES, CITY_COUNT);
    lb.selected = -1;
    uui_listbox_key(&lb, 'h');
    uui_listbox_key(&lb, 'o');
    oki("listbox: 'ho' reaches Honolulu", lb.selected, 3);

    // Case-folded against the LABEL, which is the only string a user
    // can see -- both directions.
    uui_listbox_init(&lb, 0, 0, 200, 200, CITIES, CITY_COUNT);
    lb.selected = -1;
    uui_listbox_key(&lb, 'L');
    oki("listbox: uppercase L reaches 'los angeles'", lb.selected, 5);

    // A movement key ends the search: 'h' after Home must mean "an h",
    // not "the next h after the one I was on".
    uui_listbox_init(&lb, 0, 0, 200, 200, CITIES, CITY_COUNT);
    lb.selected = -1;
    uui_listbox_key(&lb, 'h');
    uui_listbox_key(&lb, 'h');
    uui_listbox_key(&lb, KEY_END);
    uui_listbox_key(&lb, 'h');
    oki("listbox: arrow resets the prefix", lb.selected, 0);

    // A dead end restarts rather than going silent.
    uui_listbox_init(&lb, 0, 0, 200, 200, CITIES, CITY_COUNT);
    lb.selected = -1;
    uui_listbox_key(&lb, 'h');
    uui_listbox_key(&lb, 'z');   // no "hz..." anywhere
    oki("listbox: dead end restarts as 'z'", lb.selected, 0); // no z either: no move
    uui_listbox_key(&lb, 'o');
    oki("listbox: and the next letter still searches", lb.selected, 4);
}

// --- the table half ----------------------------------------------------
//
// Column 0 is a NUMBER and column 1 is the name, the same shape as Task
// Manager's PID/Name.

static const struct uui_table_column COLS[] = {
    { "Id",   6, UUI_TALIGN_RIGHT },
    { "Name", 0, UUI_TALIGN_LEFT  },
};

struct trow { int id; const char *name; };

// STORED IN REVERSE of the order they are displayed in, so a search
// walking app indices cannot pass the cycling check below by accident.
static const struct trow SORTED_ROWS[] = {
    { 40, "delta" }, { 30, "charlie" }, { 20, "bravo" }, { 10, "alpha" },
};
#define TROW_COUNT ((int)(sizeof SORTED_ROWS / sizeof SORTED_ROWS[0]))

static void tcell(void *ctx, int row, int col, char *out, int cap) {
    (void)ctx;
    if (row < 0 || row >= TROW_COUNT) { out[0] = '\0'; return; }
    if (col == 0) snprintf(out, (size_t)cap, "%d", SORTED_ROWS[row].id);
    else          snprintf(out, (size_t)cap, "%s", SORTED_ROWS[row].name);
}

// Ascending by name, which reverses the array above.
static int tcmp(void *ctx, int a, int b, int col) {
    (void)ctx; (void)col;
    return strcmp(SORTED_ROWS[a].name, SORTED_ROWS[b].name);
}

// THE DISCRIMINATING FIXTURE. Two names start with 'b', and the app's
// order disagrees with the sorted view about which comes first:
//
//   app rows   0 bravo   1 alpha   2 bishop
//   view       0 alpha   1 bishop  2 bravo
//
// So 'b' reaches bishop (app row 2) walking the SCREEN and bravo (app
// row 0) walking the array -- a single keystroke tells the two apart.
// An earlier version of this fixture had one match per letter, and a
// control that walked app order passed every check in the file.
static const struct trow BRAVO_ROWS[] = {
    { 1, "bravo" }, { 2, "alpha" }, { 3, "bishop" },
};
static void bcell(void *ctx, int row, int col, char *out, int cap) {
    (void)ctx;
    if (row < 0 || row >= 3) { out[0] = '\0'; return; }
    if (col == 0) snprintf(out, (size_t)cap, "%d", BRAVO_ROWS[row].id);
    else          snprintf(out, (size_t)cap, "%s", BRAVO_ROWS[row].name);
}

static int bcmp(void *ctx, int a, int b, int col) {
    (void)ctx; (void)col;
    return strcmp(BRAVO_ROWS[a].name, BRAVO_ROWS[b].name);
}

static void table_checks(void) {
    struct uui_table t;

    // Unsorted first: the plain case.
    uui_table_init(&t, 0, 0, 300, 200, COLS, 2, tcell, 0);
    uui_table_set_seek_col(&t, 1);
    uui_table_set_rows(&t, TROW_COUNT);
    t.selected = -1;
    uui_table_key(&t, 'c');
    oki("table: c reaches charlie", t.selected, 1);

    // THE LOAD-BEARING ONE. Sorted ascending by name the view is
    // alpha, bravo, charlie, delta -- the reverse of the array. Typing
    // 'a' must reach "alpha", which is app row 3 and view row 0.
    uui_table_init(&t, 0, 0, 300, 200, COLS, 2, tcell, 0);
    uui_table_set_seek_col(&t, 1);
    uui_table_set_rows(&t, TROW_COUNT);
    uui_table_set_compare(&t, tcmp);
    uui_table_set_sort(&t, 1, 1);
    t.selected = -1;
    uui_table_key(&t, 'a');
    oki("table: sorted, a reaches alpha (app row)", t.selected, 3);
    oki("table: sorted, alpha is view row 0", uui_table_view_row(&t, t.selected), 0);

    // THE OTHER LOAD-BEARING ONE, and the one a wrong implementation
    // fails on a SINGLE keystroke: sorted, 'b' must reach the first b
    // on SCREEN (bishop, app row 2), not the first in the array
    // (bravo, app row 0). Then cycling runs down the screen to bravo.
    uui_table_init(&t, 0, 0, 300, 200, COLS, 2, bcell, 0);
    uui_table_set_seek_col(&t, 1);
    uui_table_set_rows(&t, 3);
    uui_table_set_compare(&t, bcmp);
    uui_table_set_sort(&t, 1, 1);
    t.selected = -1;
    uui_table_key(&t, 'b');
    oki("table: sorted, b reaches bishop (app row 2)", t.selected, 2);
    uui_table_key(&t, 'b');
    oki("table: b again cycles down the screen to bravo", t.selected, 0);

    // The seek column is DECLARED. With it on column 0 -- the number --
    // a letter matches nothing and a digit does.
    uui_table_init(&t, 0, 0, 300, 200, COLS, 2, tcell, 0);
    uui_table_set_seek_col(&t, 0);
    uui_table_set_rows(&t, TROW_COUNT);
    t.selected = -1;
    uui_table_key(&t, 'c');
    oki("seek-col-is-declared: letters miss a number column", t.selected, -1);
    uui_table_key(&t, '3');
    oki("seek-col-is-declared: a digit finds 30", t.selected, 1);

    // A negative column turns it off entirely.
    uui_table_init(&t, 0, 0, 300, 200, COLS, 2, tcell, 0);
    uui_table_set_seek_col(&t, -1);
    uui_table_set_rows(&t, TROW_COUNT);
    t.selected = -1;
    ok("table: a negative seek column is off",
        uui_table_key(&t, 'c') == 0 && t.selected == -1, "it moved anyway");

    // set_rows() must NOT reset the prefix: Task Manager calls it on
    // every refresh, so a resetting version drops the second letter of
    // anything typed across a tick.
    uui_table_init(&t, 0, 0, 300, 200, COLS, 2, tcell, 0);
    uui_table_set_seek_col(&t, 1);
    uui_table_set_rows(&t, TROW_COUNT);
    t.selected = -1;
    uui_table_key(&t, 'd');
    uui_table_set_rows(&t, TROW_COUNT);   // a refresh lands mid-prefix
    uui_table_key(&t, 'e');
    oki("table: a refresh does not drop the prefix", t.selected, 0); // "delta"
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    put("typeahead_test: type-ahead in uui_listbox and uui_table\n");
    listbox_checks();
    table_checks();

    char line[64];
    snprintf(line, sizeof line, "typeahead_test: %d failure(s)\n", g_fail);
    put(line);
    return g_fail;
}
