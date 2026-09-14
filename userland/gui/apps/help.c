// Help -- the system manual, in a window.
//
// **IT PARSES NOTHING AND RENDERS NOTHING.** `lib/umd.h` owns what
// Markdown means and `ui/uui_markdown.h` owns what it looks like; this
// app owns only WHICH page you are reading. That is the same split
// `/bin/doc` sits on the other side of -- it renders these exact files
// to a terminal through umd's other half -- so the two can never
// disagree about what `**` means in a page nobody would check twice.
//
// The pages are `/usr/share/doc/<category>/<name>.md`, staged from this
// repository's own `docs/commands/`. A category is a DIRECTORY, which is
// `doc`'s rule and needs no code here either.
#include "ui/uapp.h"
#include "ui/uui.h"
#include "ui/uui_markdown.h"
#include "ui/uui_sidebar.h"
#include "ui/uui_focus.h"
#include "ui/ulog.h"
#include "lib/ufile.h"
#include <dirent.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define DOC_ROOT "/usr/share/doc"

// The manual is ~120 pages in one category today and the sidebar holds
// whatever it is given (its rows are the CALLER's array), so this bounds
// the app's own storage and nothing else. A tree that outgrows it lists
// the first MAX_ROWS and says so in the log rather than truncating
// silently.
#define MAX_ROWS   256
#define NAME_CAP   64
#define PATH_CAP   192

enum { ID_SIDEBAR = 1, ID_DOC = 2 };

// One row's identity: which file it names. Parallel to g_rows, because
// uui_sidebar_row carries only a label and an id.
struct page {
    char cat[NAME_CAP];
    char name[NAME_CAP];
};

static struct uui_sidebar_row g_rows[MAX_ROWS];
static struct page            g_page[MAX_ROWS];
static char                   g_label[MAX_ROWS][NAME_CAP];
static int                    g_row_count;

static struct uui_sidebar  g_side;
static struct uui_markdown g_md;

// **THE MARKDOWN WIDGET KEEPS A POINTER AND COPIES NOTHING**, so this
// buffer has to outlive every frame that draws it and be freed exactly
// once, when it is replaced or when the app exits.
static char  *g_text;
static size_t g_text_len;

// Shown when a page cannot be read. A document rather than an error box:
// the widget is already here, and a sentence in the reading pane is
// where a reader is looking.
static const char MISSING[] =
    "# Not available\n\n"
    "That page could not be read from `" DOC_ROOT "`.\n";

static struct uui_item g_items[2];
static struct uui_layout g_root;

// The page list is a Tab stop and takes the arrow keys -- a manual you
// cannot page through from the keyboard is a manual you read with a
// mouse in your hand. The document scrolls by wheel and by its own
// scrollbar; it accepts no focus, so the ring is one stop long and Tab
// is effectively a no-op rather than a trap.
static struct uui_focusable g_focusables[] = {
    { .widget = &g_side, .ops = &uui_sidebar_ops },
};
static struct uui_focus g_focus;

// --- the page list ----------------------------------------------------

static int add_row(const char *label, int kind, int id) {
    if (g_row_count >= MAX_ROWS) return 0;
    strlcpy(g_label[g_row_count], label, NAME_CAP);
    g_rows[g_row_count].label = g_label[g_row_count];
    g_rows[g_row_count].kind = kind;
    g_rows[g_row_count].icon = NULL;
    g_rows[g_row_count].id = id;
    g_row_count++;
    return 1;
}

// Sorted by name: readdir's order is the directory's, which is insertion
// order on this filesystem and reads as no order at all in a list.
static int page_names(const char *cat, char names[][NAME_CAP], int cap) {
    char dir[PATH_CAP];
    snprintf(dir, sizeof dir, "%s/%s", DOC_ROOT, cat);
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while (n < cap && (e = readdir(d)) != NULL) {
        if (e->d_type == DT_DIR) continue;
        int len = (int)strlen(e->d_name);
        if (len < 4 || strcmp(e->d_name + len - 3, ".md") != 0) continue;
        strlcpy(names[n], e->d_name, NAME_CAP);
        names[n][len - 3] = 0;      // drop the extension: a page has a NAME
        n++;
    }
    closedir(d);
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
            char t[NAME_CAP];
            strlcpy(t, names[j - 1], NAME_CAP);
            strlcpy(names[j - 1], names[j], NAME_CAP);
            strlcpy(names[j], t, NAME_CAP);
        }
    return n;
}

// Every subdirectory of DOC_ROOT is a category; a loose file there is
// not a page, which is `doc`'s rule applied to the same tree.
static void scan(void) {
    static char names[MAX_ROWS][NAME_CAP];
    DIR *d = opendir(DOC_ROOT);
    if (!d) return;

    static char cats[32][NAME_CAP];
    int ncat = 0;
    struct dirent *e;
    while (ncat < 32 && (e = readdir(d)) != NULL) {
        if (e->d_type != DT_DIR) continue;
        if (e->d_name[0] == '.') continue;
        strlcpy(cats[ncat++], e->d_name, NAME_CAP);
    }
    closedir(d);

    for (int i = 1; i < ncat; i++)
        for (int j = i; j > 0 && strcmp(cats[j - 1], cats[j]) > 0; j--) {
            char t[NAME_CAP];
            strlcpy(t, cats[j - 1], NAME_CAP);
            strlcpy(cats[j - 1], cats[j], NAME_CAP);
            strlcpy(cats[j], t, NAME_CAP);
        }

    for (int c = 0; c < ncat; c++) {
        int n = page_names(cats[c], names, MAX_ROWS);
        if (n <= 0) continue;
        if (!add_row(cats[c], UUI_SIDEBAR_HEADING, 0)) break;
        for (int i = 0; i < n; i++) {
            int id = g_row_count;       // the row's own index, so it is unique
            if (!add_row(names[i], UUI_SIDEBAR_ITEM, id)) {
                ulogf("help: more than %d rows; the rest are not listed", MAX_ROWS);
                return;
            }
            strlcpy(g_page[id].cat, cats[c], NAME_CAP);
            strlcpy(g_page[id].name, names[i], NAME_CAP);
        }
    }
}

// --- reading ----------------------------------------------------------

static void show(int id) {
    if (id < 0 || id >= MAX_ROWS || !g_page[id].name[0]) return;

    char path[PATH_CAP];
    snprintf(path, sizeof path, "%s/%s/%s.md",
             DOC_ROOT, g_page[id].cat, g_page[id].name);

    uint8_t *buf = NULL;
    size_t len = 0;
    if (ufile_slurp(path, 1u << 20, &buf, &len) != UFILE_OK) {
        // Free the OLD text only once the new read has been attempted:
        // the widget is still pointing at it until set_text() replaces
        // it, and a failed load must not leave it pointing at freed
        // memory.
        uui_markdown_set_text(&g_md, MISSING, (int)sizeof MISSING - 1);
        free(g_text);
        g_text = NULL;
        g_text_len = 0;
        return;
    }

    uui_markdown_set_text(&g_md, (const char *)buf, (int)len);
    free(g_text);
    g_text = (char *)buf;
    g_text_len = len;
}

// --- the app ----------------------------------------------------------

static void on_widget(struct uapp *a, int id, int reason) {
    (void)reason;
    if (id != ID_SIDEBAR) return;
    show(uui_sidebar_selected_id(&g_side));
    uapp_redraw(a);
}

// **THE LAYOUT OUTRANKS `.w/.h`, AND THE SIDEBAR'S NATURAL HEIGHT IS
// EVERY ROW IT HAS.** Left to the layout this opened 2270px tall -- one
// row per page in the manual -- so the size is stated here instead:
// `on_size` outranks both, and unlike a constant evaluated in main() it
// runs AFTER the font is up, which is the only time these measurements
// mean anything.
static void on_size(int *w, int *h) {
    *w = ugfx_char_advance('n') * 88;
    *h = ugfx_char_h() * 34;
}

static void on_open(struct uapp *a) {
    (void)a;
    show(uui_sidebar_selected_id(&g_side));
}

static int on_close(struct uapp *a) {
    (void)a;
    // BOTH of the toolkit's memory-owning widgets have to be released,
    // and the document buffer is this app's own.
    uui_markdown_free(&g_md);
    free(g_text);
    g_text = NULL;
    return 1;
}

int main(void) {
    scan();

    uui_sidebar_init(&g_side, 0, 0, 0, 0, g_rows, g_row_count);
    uui_focus_init(&g_focus, g_focusables,
                   (int)(sizeof g_focusables / sizeof g_focusables[0]));
    uui_focus_set(&g_focus, 0);   // the list has the keyboard on open
    uui_markdown_init(&g_md);

    // A fixed reading column beside a list that fills the height: the
    // shape KHelpCenter and yelp both have. The sidebar's width is
    // font-derived, never a pixel constant.
    g_items[0].ops = &uui_sidebar_ops;
    g_items[0].widget = &g_side;
    g_items[0].id = ID_SIDEBAR;
    g_items[0].name = "pages";
    g_items[0].flags = UUI_FILL_H;
    g_items[0].main_size = ugfx_char_advance('n') * 22;

    g_items[1].ops = &uui_markdown_ops;
    g_items[1].widget = &g_md;
    g_items[1].id = ID_DOC;
    g_items[1].name = "doc";
    g_items[1].flags = UUI_FILL_W | UUI_FILL_H;

    g_root.dir = UUI_ROW;
    g_root.items = g_items;
    g_root.count = 2;

    struct uapp_desc desc = {
        .title        = "Help",
        .app_id       = "help",
        .on_size      = on_size,
        .layout       = &g_root,
        .widgets      = g_items,
        .widget_count = 2,
        .on_widget    = on_widget,
        .on_open      = on_open,
        .on_close     = on_close,
        .focus        = &g_focus,
        .flags        = UAPP_RESIZABLE,
    };
    return uapp_run(&desc);
}
