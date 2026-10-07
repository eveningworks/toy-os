// Screenshot's Options window: three pages over the shared dialog
// (ui/uui_prefs.h), turning a struct shot_conf into controls and back.
// The Print Screen keys are read from and written to /etc/shortcuts.conf
// (shot_conf.c), never kept here.
#include "screenshot.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "ui/uapp.h"
#include "ui/ulog.h"
#include "ui/utheme.h"
#include "ui/uui_button.h"
#include "ui/uui_dropdown.h"
#include "ui/uui_filedialog.h"
#include "ui/uui_label.h"
#include "ui/uui_nametpl.h"
#include "ui/uui_prefs.h"
#include "ui/uui_segmented.h"
#include "ui/uui_sidebar.h"

enum {
    OPT_START = 1, OPT_LASTREG, OPT_SIZE, OPT_MAG, OPT_SHADOW, OPT_POINTER, OPT_DELAYS,
    OPT_KEY0, OPT_KEY1, OPT_KEY2,
    OPT_CARD, OPT_COPY, OPT_OPEN, OPT_FLASH,
    OPT_FOLDER, OPT_BROWSE, OPT_NAME, OPT_FORMAT, OPT_ASK,
};

static const struct uui_sidebar_row PAGE_ROWS[] = {
    { "Capture",       UUI_SIDEBAR_TOP, 0, SHOT_PAGE_CAPTURE },
    { "After capture", UUI_SIDEBAR_TOP, 0, SHOT_PAGE_AFTER },
    { "Saving",        UUI_SIDEBAR_TOP, 0, SHOT_PAGE_SAVING },
};
static const char *const START_OPTS[] = { "Last used", "Region", "Screen", "Window" };
static const char *const FORMAT_OPTS[] = { "QOI", "PNG" };
// THE TIMER'S CHOICES AS SETS, not three free numbers: a row holds two
// controls, and nobody wants a 7 s timer. A hand-edited file that names
// some other set keeps it until one of these is chosen.
static const int DELAY_SETS[][SHOT_DELAYS] = { { 3, 5, 10 }, { 2, 5, 10 }, { 5, 10, 30 }, { 1, 3, 5 } };
static const char *const DELAY_OPTS[] = { "3, 5 or 10 seconds", "2, 5 or 10 seconds",
                                          "5, 10 or 30 seconds", "1, 3 or 5 seconds" };
#define DELAY_SET_COUNT 4

static struct shot_conf g_edit;
static void (*g_on_commit)(const struct shot_conf *next);
static struct uapp *g_app;

static struct uui_segmented g_format;
static struct uui_dropdown g_delays, g_key[SHOT_KEYS];
static struct uui_label g_folder;
static struct uui_button g_browse;
static struct uui_nametpl g_name;
static struct shot_vars g_sample;
static char g_folder_text[SHOT_DIR_MAX + 48];
static struct uui_filedialog g_chooser;

// `start` is a mode or SHOT_START_LAST; the row shows "Last used" first.
static const int START_VALUES[] = { SHOT_START_LAST, SHOT_MODE_REGION, SHOT_MODE_SCREEN, SHOT_MODE_WINDOW };

static int delay_set_of(const int d[SHOT_DELAYS]) {
    for (int i = 0; i < DELAY_SET_COUNT; i++)
        if (!memcmp(DELAY_SETS[i], d, sizeof DELAY_SETS[i])) return i;
    return -1;
}

static void show_folder(const char *note) {
    if (note) snprintf(g_folder_text, sizeof g_folder_text, "%s", note);
    else snprintf(g_folder_text, sizeof g_folder_text, "%s", g_edit.folder);
    uui_label_set_text(&g_folder, g_folder_text);
}

// The rows the window cannot bind: the timer's SETS, the keys (which live
// in /etc/shortcuts.conf), the folder, the template and the format.
static void to_controls(void) {
    int ds = delay_set_of(g_edit.delays);
    uui_dropdown_set_selected(&g_delays, ds >= 0 ? ds : 0);
    int keys[SHOT_KEYS];
    shot_keys_get(keys);
    for (int i = 0; i < SHOT_KEYS; i++) uui_dropdown_set_selected(&g_key[i], keys[i]);
    show_folder(0);
    uui_textbox_set_text(&g_name.field, g_edit.name);
    g_format.selected = g_edit.png ? 1 : 0;
}

static void from_controls(void) {
    int ds = uui_dropdown_selected(&g_delays);
    if (ds >= 0 && ds < DELAY_SET_COUNT && ds != delay_set_of(g_edit.delays))
        memcpy(g_edit.delays, DELAY_SETS[ds], sizeof g_edit.delays);
    // AN UNUSABLE TEMPLATE IS NOT SAVED: the field said so under itself,
    // and the name that works stays.
    if (uui_nametpl_valid(&g_name) && strlen(uui_nametpl_text(&g_name)) < sizeof g_edit.name)
        snprintf(g_edit.name, sizeof g_edit.name, "%s", uui_nametpl_text(&g_name));
    g_edit.png = g_format.selected == 1;
}

static void edit_defaults(void *edit) { shot_conf_defaults(edit); }

static void on_defaults(void) {
    to_controls();
    // The keys' factory bindings: Print Screen opens the bar, Shift saves
    // the screen, Alt the window (lib/ushortcut_actions.c).
    for (int i = 0; i < SHOT_KEYS; i++) uui_dropdown_set_selected(&g_key[i], i);
}

static void on_ok(void) {
    from_controls();
    int keys[SHOT_KEYS];
    for (int i = 0; i < SHOT_KEYS; i++) {
        int k = uui_dropdown_selected(&g_key[i]);
        keys[i] = k >= 0 && k < SHOT_KEY_CHOICES ? k : SHOT_KEY_NONE;
    }
    if (!shot_keys_set(keys)) ulog("screenshot: could not write the Print Screen keys\n");
    ulogf("screenshot: options saved (folder %s, name %s, %s)\n", g_edit.folder, g_edit.name,
          g_edit.png ? "png" : "qoi");
    if (g_on_commit) g_on_commit(&g_edit);
}

static void folder_chosen(void *ctx, const char *path) {
    (void)ctx;
    if (!path) return;
    if (!shot_folder_ok(path)) {
        // REFUSED, with the reason where the folder is shown: the card
        // names the file, and hands the path to Files on one line.
        show_folder("Choose a folder whose name has no spaces or symbols");
    } else {
        snprintf(g_edit.folder, sizeof g_edit.folder, "%s", path);
        show_folder(0);
    }
    uui_prefs_redraw();
}

static void on_action(int code) {
    if (code != OPT_BROWSE || uui_filedialog_is_open(&g_chooser)) return;
    struct uui_filedialog_opts o = {
        .mode = UUI_FILEDIALOG_FOLDER,
        .title = "Save Screenshots In",
        .start_dir = g_edit.folder,
    };
    uui_filedialog_open(g_app, &g_chooser, &o, folder_chosen, 0);
}

// The preview names the file in the format chosen beside it.
static void on_widget(int id, int reason) {
    (void)reason;
    if (id == OPT_FORMAT) g_name.suffix = g_format.selected == 1 ? ".png" : ".qoi";
}

int shot_prefs_is_open(void) { return uui_prefs_is_open(); }

void shot_prefs_open(struct uapp *a, const struct shot_conf *c, int page,
                     void (*on_commit)(const struct shot_conf *next)) {
    if (uui_prefs_is_open()) return;
    g_app = a;
    g_edit = *c;
    g_on_commit = on_commit;

    uui_segmented_init(&g_format, FORMAT_OPTS, 2, 0);
    uui_dropdown_init(&g_delays, 0, 0, 0, 0, DELAY_OPTS, DELAY_SET_COUNT);
    for (int i = 0; i < SHOT_KEYS; i++)
        uui_dropdown_init(&g_key[i], 0, 0, 0, 0, SHOT_KEY_LABEL, SHOT_KEY_CHOICES);
    uui_label_init(&g_folder, 0);
    uui_button_init(&g_browse, 0, 0, 0, 0, "Browse...", UTHEME_BUTTON_BG, UTHEME_TEXT, OPT_BROWSE);
    shot_vars_now(&g_sample, "notepad", SHOT_MODE_REGION);
    uui_nametpl_init(&g_name, g_edit.name, SHOT_CHIPS, SHOT_CHIP_COUNT, g_sample.v, SHOT_CHIP_COUNT);
    g_name.suffix = g_edit.png ? ".png" : ".qoi";
    to_controls();

    uui_prefs_begin(&(struct uui_prefs_desc){
        .title = "Screenshot Options",
        .pages = PAGE_ROWS,
        .page_count = SHOT_PAGES,
        .log_prefix = "options",
        .edit = &g_edit,
        .edit_defaults = edit_defaults,
        .on_defaults = on_defaults,
        .on_ok = on_ok,
        .on_widget = on_widget,
        .on_action = on_action,
        .first_page = page,
    });
    int r;
    uui_prefs_row(SHOT_PAGE_CAPTURE, "Print Screen:", &uui_dropdown_ops, &g_key[0], OPT_KEY0, "key0", 0);
    uui_prefs_row(SHOT_PAGE_CAPTURE, "Shift+Print Screen:", &uui_dropdown_ops, &g_key[1], OPT_KEY1, "key1", 0);
    uui_prefs_row(SHOT_PAGE_CAPTURE, "Alt+Print Screen:", &uui_dropdown_ops, &g_key[2], OPT_KEY2, "key2", 0);
#define F(field) offsetof(struct shot_conf, field)
    uui_prefs_choice(SHOT_PAGE_CAPTURE, "Start in:", START_OPTS, START_VALUES, 4, F(start), "start");
    uui_prefs_check(SHOT_PAGE_CAPTURE, "Region:", "Start from the last region", F(last_region), "lastregion");
    uui_prefs_check(SHOT_PAGE_CAPTURE, "", "Show the size beside the pointer", F(size_label), "sizelabel");
    uui_prefs_check(SHOT_PAGE_CAPTURE, "", "Magnifier while dragging an edge", F(magnifier), "magnifier");
    uui_prefs_check(SHOT_PAGE_CAPTURE, "Window:", "Include the shadow", F(shadow), "shadow");
    uui_prefs_check(SHOT_PAGE_CAPTURE, "Pointer:", "In the picture", F(pointer), "pointer");
    uui_prefs_row(SHOT_PAGE_CAPTURE, "Timer offers:", &uui_dropdown_ops, &g_delays, OPT_DELAYS, "delays", 0);

    uui_prefs_check(SHOT_PAGE_AFTER, "After saving:", "Show the card", F(card), "card");
    uui_prefs_check(SHOT_PAGE_AFTER, "", "Copy to the clipboard", F(copy), "copy");
    uui_prefs_check(SHOT_PAGE_AFTER, "", "Open in Image Viewer", F(open), "open");
    uui_prefs_check(SHOT_PAGE_AFTER, "", "Flash the screen", F(flash), "flash");

    r = uui_prefs_row(SHOT_PAGE_SAVING, "Folder:", &uui_label_ops, &g_folder, OPT_FOLDER, "folder", UUI_FILL_W);
    uui_prefs_also(r, &uui_button_ops, &g_browse, OPT_BROWSE, "browse");
    uui_prefs_row(SHOT_PAGE_SAVING, "File name:", &uui_nametpl_ops, &g_name, OPT_NAME, "name", UUI_FILL_W);
    uui_prefs_row(SHOT_PAGE_SAVING, "Format:", &uui_segmented_ops, &g_format, OPT_FORMAT, "format", 0);
    uui_prefs_check(SHOT_PAGE_SAVING, "Each capture:", "Ask where to save it", F(ask), "ask");
#undef F
    if (!uui_prefs_open(a)) ulog("screenshot: could not open Options\n");
}
