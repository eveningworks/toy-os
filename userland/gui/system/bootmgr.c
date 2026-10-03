// Boot Manager -- GRUB's menu in a window: each entry's title, kernel
// and boot words, the default, the timeout, a one-shot next restart, a
// trial booted once, and the whole file as text. Grub Customizer's and
// msconfig's Boot tab's shape; the everyday three (default, timeout,
// next restart) are also on System Settings > System > Boot menu.
//
// EVERYTHING ABOUT THE FILE IS lib/ubootcfg.h, shared with /bin/bootcfg:
// the edit, the check and the save. This file is the view. Edits go to a
// WORKING copy; Save checks it against the file on disk and writes it
// with the old one kept as grub.cfg.bak, exactly as bootcfg does.
//
// A BOOT WORD IS KEPT AS WRITTEN unless its row was edited: the list's
// value field is shorter than the longest word (a `kdebug=net,...,key=`
// line), and showing a word must not be able to truncate it.
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "rt/sys.h"
#include "keyboard.h"
#include "ui/uapp.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/ulog.h"
#include "ui/uui_menubar.h"
#include "ui/uui_toolbar.h"
#include "ui/uui_segmented.h"
#include "ui/uui_tree.h"
#include "ui/uui_spinbox.h"
#include "ui/uui_dropdown.h"
#include "ui/uui_textbox.h"
#include "ui/uui_optlist.h"
#include "ui/uui_textview.h"
#include "ui/uui_button.h"
#include "ui/uui_statusbar.h"
#include "ui/uui_dialog.h"
#include "ui/uui_focus.h"
#include "lib/ubootcfg.h"
#include "lib/ubootwords.h"

enum {
    ID_MENU = 1, ID_TB, ID_VIEW, ID_TREE, ID_TIMEOUT, ID_NEXT, ID_TITLE, ID_KERNEL,
    ID_WORDS, ID_REVERT, ID_SAVE, ID_TEXT, ID_STATUS, ID_ASK,
};

enum {
    CMD_SAVE = 1, CMD_REVERT, CMD_UNDO, CMD_EXIT, CMD_COPY, CMD_REMOVE, CMD_DEFAULT,
    CMD_TRY, CMD_KEEP, CMD_DROP, CMD_ENTRIES, CMD_TEXT, CMD_SAVE_ANYWAY,
    ASK_REMOVE, ASK_TRY_NOW, ASK_TRY_LATER, ASK_SAVE_ANYWAY, ASK_KEEP, ASK_DROP, ASK_CANCEL,
};

#define ROWS_MAX   64
#define KERNELS    8
#define PROBS_MAX  16

static struct ubootcfg g_disk, g_cfg;   // as saved; as edited
static int g_loaded;                   // 0: no grub.cfg on this boot
static int g_sel;                      // the entry the pane shows
static int g_text_mode;
static int g_trial_booted;
static struct ubootmenu g_menu_state;  // next_entry and whether one-shot works

static struct ubootcfg_problem g_probs[PROBS_MAX];
static int g_nprobs;

// --- widgets ---------------------------------------------------------------

static struct uui_menubar g_menu;
static struct uui_toolbar g_tb;
static const char *const VIEWS[] = { "Entries", "Text" };
static struct uui_segmented g_view;
static struct uui_tree g_tree;
static struct uui_spinbox g_timeout;
static struct uui_dropdown g_next, g_kernel;
static struct uui_textbox g_title;
static struct uui_optlist g_words;
static struct uui_textview g_text;
static struct uui_button g_revert, g_save;
static struct uui_statusbar g_status;
static struct uui_dialog g_ask;

static char g_textbuf[UBOOTCFG_MAX];
static char g_st_entries[24], g_st_menu[24], g_st_state[96];

static const struct uui_menu_item file_items[] = {
    UUI_MENU("Save", CMD_SAVE, "Ctrl+S"),
    UUI_MENU("Save anyway", CMD_SAVE_ANYWAY, 0),
    UUI_MENU("Revert changes", CMD_REVERT, 0),
    UUI_MENU("Undo last save", CMD_UNDO, 0),
    UUI_MENU_SEP,
    UUI_MENU("Exit", CMD_EXIT, "Alt+F4"),
};
static const struct uui_menu_item entry_items[] = {
    UUI_MENU("Copy entry", CMD_COPY, 0),
    UUI_MENU("Remove entry", CMD_REMOVE, 0),
    UUI_MENU("Make default", CMD_DEFAULT, 0),
    UUI_MENU_SEP,
    UUI_MENU("Try once...", CMD_TRY, 0),
    UUI_MENU("Keep the trial", CMD_KEEP, 0),
    UUI_MENU("Drop the trial", CMD_DROP, 0),
};
static const struct uui_menu_item view_items[] = {
    UUI_MENU("Entries", CMD_ENTRIES, 0),
    UUI_MENU("The file as text", CMD_TEXT, 0),
};
static const struct uui_menu_item menu_items[] = {
    UUI_SUBMENU("File", file_items),
    UUI_SUBMENU("Entry", entry_items),
    UUI_SUBMENU("View", view_items),
};

static const struct uui_toolbar_item tb_items[] = {
    { "tb-copy",    "Copy this entry",                      CMD_COPY,    "Copy entry",   0, 0, UTHEME_ACT_CREATE },
    { "tb-delete",  "Remove this entry",                    CMD_REMOVE,  "Remove",       0, 0, UTHEME_ACT_DANGER },
    UUI_TOOLBAR_SEP,
    { "tb-star",    "Boot this entry when nobody chooses",  CMD_DEFAULT, "Make default", 0, 0, UTHEME_ACT_EDIT },
    { "tb-power",   "Boot this entry once, with these words", CMD_TRY,   "Try once...",  0, 0, UTHEME_ACT_NAV },
    UUI_TOOLBAR_SEP,
    { "tb-undo",    "Put back the file as it was before the last save", CMD_UNDO, "Undo save", 0, 0, UTHEME_ACT_VIEW },
};

// The dialog LAST: an open one is on top, and the router asks the last
// item first. The dropdowns after the controls their popups cover.
static struct uui_item g_widgets[] = {
    { .ops = &uui_menubar_ops,   .widget = &g_menu,    .id = ID_MENU,    .name = "menu" },
    { .ops = &uui_toolbar_ops,   .widget = &g_tb,      .id = ID_TB,      .name = "toolbar" },
    { .ops = &uui_segmented_ops, .widget = &g_view,    .id = ID_VIEW,    .name = "view" },
    { .ops = &uui_tree_ops,      .widget = &g_tree,    .id = ID_TREE,    .name = "entries" },
    { .ops = &uui_spinbox_ops,   .widget = &g_timeout, .id = ID_TIMEOUT, .name = "timeout" },
    { .ops = &uui_textbox_ops,   .widget = &g_title,   .id = ID_TITLE,   .name = "title" },
    { .ops = &uui_optlist_ops,   .widget = &g_words,   .id = ID_WORDS,   .name = "words" },
    { .ops = &uui_button_ops,    .widget = &g_revert,  .id = ID_REVERT,  .name = "revert" },
    { .ops = &uui_button_ops,    .widget = &g_save,    .id = ID_SAVE,    .name = "save" },
    { .ops = &uui_textview_ops,  .widget = &g_text,    .id = ID_TEXT,    .name = "text" },
    { .ops = &uui_statusbar_ops, .widget = &g_status,  .id = ID_STATUS,  .name = "status" },
    { .ops = &uui_dropdown_ops,  .widget = &g_kernel,  .id = ID_KERNEL,  .name = "kernel" },
    { .ops = &uui_dropdown_ops,  .widget = &g_next,    .id = ID_NEXT,    .name = "next" },
    { .ops = &uui_dialog_ops,    .widget = &g_ask,     .id = ID_ASK,     .name = "ask" },
};
#define W_COUNT ((int)(sizeof g_widgets / sizeof g_widgets[0]))

// Two rings swapped with the view: the text view takes every key itself
// (on_key), so its ring is empty rather than holding hidden controls.
static struct uui_focusable g_ring_entries[] = {
    { &g_tree,    &uui_tree_ops },
    { &g_title,   &uui_textbox_focus_ops },
    { &g_kernel,  &uui_dropdown_ops },
    { &g_words,   &uui_optlist_ops },
    { &g_timeout, &uui_spinbox_ops },
    { &g_next,    &uui_dropdown_ops },
    { &g_revert,  &uui_button_ops },
    { &g_save,    &uui_button_ops },
};
static struct uui_focus g_focus;

// --- status ----------------------------------------------------------------

static void note(const char *msg) {
    snprintf(g_st_state, sizeof g_st_state, "%s", msg);
    ulogf("bootmgr: %s\n", msg);
}

static int dirty(void) {
    return g_cfg.len != g_disk.len || memcmp(g_cfg.text, g_disk.text, (size_t)g_cfg.len);
}

static void update_status(void) {
    snprintf(g_st_entries, sizeof g_st_entries, "%d entries", g_cfg.count);
    if (g_cfg.timeout >= 0) snprintf(g_st_menu, sizeof g_st_menu, "Menu %d s", g_cfg.timeout);
    else snprintf(g_st_menu, sizeof g_st_menu, "Menu: GRUB's default");
    g_save.disabled = g_revert.disabled = !dirty();
}

// --- the entry list --------------------------------------------------------

static struct uui_tree_node g_nodes[UBOOTMENU_MAX];

static void rebuild_tree(void) {
    int t = ubootcfg_trial_find(&g_cfg);
    for (int i = 0; i < g_cfg.count; i++) {
        int nx = g_menu_state.next;
        int next = nx >= 0 && nx < g_disk.count && !strcmp(g_disk.entry[nx].title, g_cfg.entry[i].title);
        const char *n = i == t && g_trial_booted ? "Booted"
                      : i == t ? "Trial"
                      : i == g_cfg.def ? "Default"
                      : next ? "Next restart" : 0;
        g_nodes[i] = (struct uui_tree_node){ g_cfg.entry[i].title, 0, i, UUI_TREE_AUTO, 0, 0, n, 0, 0, 0 };
    }
    uui_tree_set_nodes_keep(&g_tree, g_nodes, g_cfg.count);
    if (g_sel >= g_cfg.count) g_sel = g_cfg.count - 1;
    if (g_sel < 0) g_sel = 0;
    uui_tree_select_id(&g_tree, g_sel);
}

// --- the boot words --------------------------------------------------------

static struct uui_optlist_item g_rows[ROWS_MAX];
static char g_row_name[ROWS_MAX][UBOOTCFG_WORD];
static char g_row_orig[ROWS_MAX][UBOOTCFG_WORD];   // the word as written, "" if none
static const struct ubootword *g_row_kw[ROWS_MAX];
static int g_row_edited[ROWS_MAX];
static int g_nrows;

static int takes_value(const struct ubootword *w) {
    size_t k = strlen(w->key);
    return w->key[k - 1] == '=' || !strncmp(w->hint, "[=", 2);
}

static void add_row(const struct ubootword *kw, const char *word, int on) {
    if (g_nrows >= ROWS_MAX) return;
    int r = g_nrows++;
    struct uui_optlist_item *it = &g_rows[r];
    memset(it, 0, sizeof *it);
    g_row_kw[r] = kw;
    g_row_edited[r] = 0;
    snprintf(g_row_orig[r], sizeof g_row_orig[r], "%s", word ? word : "");
    if (kw) {
        snprintf(g_row_name[r], sizeof g_row_name[r], "%s", kw->key);
        size_t k = strlen(g_row_name[r]);
        if (k && g_row_name[r][k - 1] == '=') g_row_name[r][k - 1] = 0;
        it->has_value = takes_value(kw);
        it->hint = kw->hint;
        it->desc = kw->desc;
        const char *eq = word ? strchr(word, '=') : 0;
        if (eq && it->has_value) snprintf(it->value, sizeof it->value, "%s", eq + 1);
    } else {
        snprintf(g_row_name[r], sizeof g_row_name[r], "%s", word);
        it->desc = "Not in docs/boot-flags.md -- the kernel does not read it";
        it->hint = "";
    }
    it->name = g_row_name[r];
    it->on = on;
}

// The entry's own words first, in their order, then every other known
// word off -- rebuilt only when the entry changes, so rows never jump
// under the pointer.
static void build_rows(void) {
    char keep[UBOOTCFG_WORD] = "";
    if (g_words.selected >= 0 && g_words.selected < g_nrows)
        snprintf(keep, sizeof keep, "%s", g_row_name[g_words.selected]);
    g_nrows = 0;
    const struct ubootcfg_entry *e = g_sel < g_cfg.count ? &g_cfg.entry[g_sel] : 0;
    int used[64] = { 0 };
    for (int k = 0; e && k < e->nwords; k++) {
        const struct ubootword *kw = ubootword_find(e->word[k]);
        int idx = kw ? (int)(kw - ubootword_at(0)) : -1;
        if (idx >= 0 && idx < 64 && used[idx]) kw = 0;     // a repeated key: shown as written
        if (kw && idx < 64) used[idx] = 1;
        add_row(kw, e->word[k], 1);
    }
    for (int i = 0; i < ubootword_count(); i++)
        if (i >= 64 || !used[i]) add_row(ubootword_at(i), 0, 0);
    uui_optlist_set_items(&g_words, g_rows, g_nrows);
    // A rebuild after Save reorders the rows; the selection follows its word.
    for (int r = 0; keep[0] && r < g_nrows; r++)
        if (!strcmp(g_row_name[r], keep)) { uui_optlist_select(&g_words, r); break; }
}

// The rows back into words; an untouched row keeps its word byte for byte.
static int apply_rows(void) {
    static char made[ROWS_MAX][UBOOTCFG_WORD];
    const char *w[UBOOTCFG_WORDS];
    int n = 0;
    for (int r = 0; r < g_nrows; r++) {
        const struct uui_optlist_item *it = &g_rows[r];
        if (!it->on) continue;
        if (n >= UBOOTCFG_WORDS) { note("Too many boot words"); return -1; }
        if (g_row_orig[r][0] && !g_row_edited[r]) { w[n++] = g_row_orig[r]; continue; }
        const struct ubootword *kw = g_row_kw[r];
        if (!kw) { w[n++] = g_row_orig[r]; continue; }
        if (it->has_value && !it->value[0] && kw->key[strlen(kw->key) - 1] == '=') {
            char m[96];
            snprintf(m, sizeof m, "%s needs a value (%s)", kw->key, kw->hint);
            note(m);
            continue;
        }
        if (it->has_value && it->value[0])
            snprintf(made[r], sizeof made[r], "%s%s%s", g_row_name[r],
                     kw->key[strlen(kw->key) - 1] == '=' ? "=" : "=", it->value);
        else
            snprintf(made[r], sizeof made[r], "%s", g_row_name[r]);
        w[n++] = made[r];
    }
    if (ubootcfg_set_words(&g_cfg, g_sel, w, n) < 0) { note(g_cfg.why); return -1; }
    for (int r = 0; r < g_nrows; r++)
        if (g_rows[r].on && g_row_edited[r] && made[r][0]) {
            snprintf(g_row_orig[r], sizeof g_row_orig[r], "%s", made[r]);
            g_row_edited[r] = 0;
        }
    return 0;
}

// --- the fields ------------------------------------------------------------

static char g_kernel_name[KERNELS][UBOOTCFG_PATH];
static const char *g_kernel_items[KERNELS];
static int g_nkernels;
static char g_next_label[UBOOTMENU_MAX + 1][UBOOTMENU_TITLE + 16];
static const char *g_next_items[UBOOTMENU_MAX + 1];

// What /boot/boot holds that looks like a kernel, plus the entry's own.
static void list_kernels(const char *current) {
    g_nkernels = 0;
    DIR *d = opendir("/boot/boot");
    struct dirent *de;
    while (d && (de = readdir(d)) && g_nkernels < KERNELS - 1)
        if (!strncmp(de->d_name, "kernel", 6))
            snprintf(g_kernel_name[g_nkernels++], UBOOTCFG_PATH, "/boot/%s", de->d_name);
    if (d) closedir(d);
    int have = -1;
    for (int i = 0; i < g_nkernels; i++) if (!strcmp(g_kernel_name[i], current)) have = i;
    if (have < 0 && current[0] && g_nkernels < KERNELS)
        snprintf(g_kernel_name[have = g_nkernels++], UBOOTCFG_PATH, "%s", current);
    for (int i = 0; i < g_nkernels; i++) g_kernel_items[i] = g_kernel_name[i];
    uui_dropdown_set_items(&g_kernel, g_kernel_items, g_nkernels);
    uui_dropdown_set_selected(&g_kernel, have < 0 ? 0 : have);
}

// Next restart names a SAVED title: GRUB reads the file on disk.
static void fill_next(void) {
    ubootmenu_read(&g_menu_state, UBOOTMENU_CFG);
    snprintf(g_next_label[0], sizeof g_next_label[0], "As the default");
    g_next_items[0] = g_next_label[0];
    for (int i = 0; i < g_disk.count && i < UBOOTMENU_MAX; i++) {
        snprintf(g_next_label[i + 1], sizeof g_next_label[0], "%s", g_disk.entry[i].title);
        g_next_items[i + 1] = g_next_label[i + 1];
    }
    uui_dropdown_set_items(&g_next, g_next_items, g_disk.count + 1);
    uui_dropdown_set_selected(&g_next, g_menu_state.next >= 0 ? g_menu_state.next + 1 : 0);
    g_next.disabled = !g_menu_state.oneshot;
}

static void show_entry(void) {
    const struct ubootcfg_entry *e = g_sel < g_cfg.count ? &g_cfg.entry[g_sel] : 0;
    uui_textbox_set_text(&g_title, e ? e->title : "");
    list_kernels(e ? e->kernel : "");
    g_kernel.disabled = e ? !e->plain : 1;
    build_rows();
    uui_spinbox_set_value(&g_timeout, g_cfg.timeout >= 0 ? g_cfg.timeout : 0);
    update_status();
}

// A title is applied when it is left (another entry, a command, Save),
// not per key -- half a title is often a duplicate.
static void sync_title(void) {
    if (g_sel >= g_cfg.count) return;
    const char *t = uui_textbox_text(&g_title);
    if (!strcmp(t, g_cfg.entry[g_sel].title)) return;
    if (ubootcfg_set_title(&g_cfg, g_sel, t) < 0) {
        note(g_cfg.why);
        uui_textbox_set_text(&g_title, g_cfg.entry[g_sel].title);
        return;
    }
    rebuild_tree();
}

// --- the text view ---------------------------------------------------------

static void text_from_cfg(void) {
    utext_clear(&g_text.tb);
    utext_insert_text(&g_text.tb, g_cfg.text, g_cfg.len);
    utext_scroll_top(&g_text.tb);
}

static int cfg_from_text(void) {
    static char buf[UBOOTCFG_MAX];
    int n = g_text.tb.count < UBOOTCFG_MAX - 1 ? g_text.tb.count : UBOOTCFG_MAX - 1;
    for (int i = 0; i < n; i++) buf[i] = utext_at(&g_text.tb, i);
    return ubootcfg_parse(&g_cfg, buf, n);
}

static int is_new(const struct ubootcfg_problem *p) {
    static struct ubootcfg_problem was[PROBS_MAX];
    int n = ubootcfg_check(&g_disk, 0, UBOOTCFG_CHECK_FILES, was, PROBS_MAX);
    for (int i = 0; i < n; i++)
        if (was[i].level == p->level && !strcmp(was[i].msg, p->msg)) return 0;
    return 1;
}

static void recheck(void) {
    g_nprobs = ubootcfg_check(&g_cfg, &g_disk, UBOOTCFG_CHECK_FILES, g_probs, PROBS_MAX);
}

// --- mode ------------------------------------------------------------------

static void set_hidden(int id, int hidden) {
    for (int i = 0; i < W_COUNT; i++) if (g_widgets[i].id == id) g_widgets[i].hidden = hidden;
}

static void set_mode(int text) {
    if (text == g_text_mode) return;
    if (text) {
        sync_title();
        text_from_cfg();
        g_focus.items = 0;
        g_focus.count = 0;
        g_focus.current = -1;
    } else {
        if (cfg_from_text() < 0) { note("The text is too big to use"); g_view.selected = 1; return; }
        recheck();
        if (ubootcfg_has(g_probs, g_nprobs, UBOOTCFG_BROKEN)) {
            note("Fix the problems below before going back to Entries");
            g_view.selected = 1;
            return;
        }
        g_focus.items = g_ring_entries;
        g_focus.count = (int)(sizeof g_ring_entries / sizeof g_ring_entries[0]);
        g_focus.current = 0;
        rebuild_tree();
        show_entry();
    }
    g_text_mode = text;
    g_view.selected = text;
    static const int entries_ids[] = { ID_TREE, ID_TIMEOUT, ID_NEXT, ID_TITLE, ID_KERNEL, ID_WORDS };
    for (unsigned i = 0; i < sizeof entries_ids / sizeof entries_ids[0]; i++) set_hidden(entries_ids[i], text);
    set_hidden(ID_TEXT, !text);
    recheck();
}

// --- load, save ------------------------------------------------------------

static void load(void) {
    g_loaded = ubootcfg_load(&g_disk, UBOOTMENU_CFG) == 0;
    if (!g_loaded) {
        ubootcfg_parse(&g_disk, "", 0);
        note("No boot menu on this boot -- /boot/boot/grub/grub.cfg cannot be read");
    }
    g_cfg = g_disk;
    static char line[UBOOTCFG_WORDS * UBOOTCFG_WORD];
    g_trial_booted = ubootcfg_cmdline(line, sizeof line) >= 0 && ubootcfg_trial_booted(line);
    fill_next();
    rebuild_tree();
    show_entry();
    if (g_text_mode) text_from_cfg();
    recheck();
}

static const char *g_ask_rows[6];
static char g_ask_line[6][UBOOTCFG_PROBLEM + 16];

static void ask(const char *title, int nrows, const struct uui_dialog_button *b, int nb, int cancel) {
    for (int i = 0; i < nrows; i++) g_ask_rows[i] = g_ask_line[i];
    uui_dialog_open(&g_ask, title, g_ask_rows, nrows, b, nb, 0, cancel);
}

static int do_save(int force) {
    sync_title();
    if (g_text_mode && cfg_from_text() < 0) { note("The text is too big to save"); return -1; }
    if (!dirty()) { note("No changes to save"); return 0; }
    recheck();
    int broken = 0, risky = 0, rows = 0;
    for (int i = 0; i < g_nprobs; i++) {
        if (!is_new(&g_probs[i])) continue;
        if (g_probs[i].level == UBOOTCFG_BROKEN) broken++; else risky++;
        if (rows < 4) snprintf(g_ask_line[rows++], sizeof g_ask_line[0], "%s", g_probs[i].msg);
    }
    if (broken) {
        static const struct uui_dialog_button ok[] = { { "OK", ASK_CANCEL, 0 } };
        ask("Not saved: GRUB would stop at this file", rows, ok, 1, ASK_CANCEL);
        note("Not saved -- fix the problems first");
        return -1;
    }
    if (risky && !force) {
        static const struct uui_dialog_button b[] = {
            { "Save anyway", ASK_SAVE_ANYWAY, UUI_DLG_DANGER }, { "Cancel", ASK_CANCEL, 0 },
        };
        ask("Save with these warnings?", rows, b, 2, ASK_CANCEL);
        return -1;
    }
    const char *why = 0;
    if (ubootcfg_save(&g_cfg, UBOOTMENU_CFG, &why) < 0) { note(why); return -1; }
    int sel = g_sel;
    load();
    g_sel = sel;
    rebuild_tree();
    show_entry();
    note("Saved -- the previous file is kept as grub.cfg.bak");
    return 0;
}

static void revert(void) {
    g_cfg = g_disk;
    rebuild_tree();
    show_entry();
    if (g_text_mode) text_from_cfg();
    recheck();
    note("Changes reverted");
}

static void undo_save(void) {
    const char *why = 0;
    if (ubootcfg_undo(UBOOTMENU_CFG, &why) < 0) { note(why); return; }
    load();
    note("The file before the last save is back (undo again to redo)");
}

// A trial is made from the SAVED file plus this entry's words -- so it
// refuses while other unsaved edits exist, rather than saving them too.
static void ask_try(void) {
    sync_title();
    if (g_sel >= g_cfg.count || !g_menu_state.oneshot) {
        note(g_menu_state.why_not ? g_menu_state.why_not : "This machine cannot boot an entry once");
        return;
    }
    if (ubootcfg_trial_source(&g_cfg, g_sel) >= 0) { note("That is a trial already: keep or drop it"); return; }
    static struct ubootcfg probe;
    probe = g_disk;
    int de = ubootcfg_find(&probe, g_cfg.entry[g_sel].title);
    const char *w[UBOOTCFG_WORDS];
    for (int i = 0; i < g_cfg.entry[g_sel].nwords; i++) w[i] = g_cfg.entry[g_sel].word[i];
    if (de < 0 || ubootcfg_set_words(&probe, de, w, g_cfg.entry[g_sel].nwords) < 0 ||
        probe.len != g_cfg.len || memcmp(probe.text, g_cfg.text, (size_t)probe.len)) {
        note("Save or revert your other changes first -- a trial changes only this entry's words");
        return;
    }
    snprintf(g_ask_line[0], sizeof g_ask_line[0], "\"%s\" with these boot words is saved as a trial entry", g_cfg.entry[g_sel].title);
    snprintf(g_ask_line[1], sizeof g_ask_line[1], "and started on the next restart only.");
    snprintf(g_ask_line[2], sizeof g_ask_line[2], "If it does not start, switch off and on: the default boots again.");
    static const struct uui_dialog_button b[] = {
        { "Restart now", ASK_TRY_NOW, 0 }, { "Restart later", ASK_TRY_LATER, 0 }, { "Cancel", ASK_CANCEL, 0 },
    };
    ask("Try this entry once", 3, b, 3, ASK_CANCEL);
}

static void do_try(int now) {
    static struct ubootcfg t;
    t = g_disk;
    int de = ubootcfg_find(&t, g_cfg.entry[g_sel].title);
    const char *w[UBOOTCFG_WORDS];
    for (int i = 0; i < g_cfg.entry[g_sel].nwords; i++) w[i] = g_cfg.entry[g_sel].word[i];
    int tr = de < 0 ? -1 : ubootcfg_try(&t, de, w, g_cfg.entry[g_sel].nwords);
    if (tr < 0) { note(t.why ? t.why : "Could not make the trial"); return; }
    static struct ubootcfg_problem p[PROBS_MAX];
    int n = ubootcfg_check(&t, &g_disk, UBOOTCFG_CHECK_FILES, p, PROBS_MAX);
    for (int i = 0; i < n; i++)
        if (p[i].level == UBOOTCFG_BROKEN && is_new(&p[i])) { note(p[i].msg); return; }
    const char *why = 0;
    if (ubootcfg_save(&t, UBOOTMENU_CFG, &why) < 0) { note(why); return; }
    if (ubootmenu_set_next(t.entry[tr].title) < 0) { note("Saved the trial, but could not choose it for the next boot"); load(); return; }
    load();
    if (now) sys_poweroff(1);
    note("The trial starts on the next restart");
}

static void trial_end(int keep) {
    int t = ubootcfg_trial_find(&g_cfg);
    if (t < 0) { note("There is no trial entry"); return; }
    if (dirty()) { note("Save or revert your changes first"); return; }
    if ((keep ? ubootcfg_keep(&g_cfg, t) : ubootcfg_remove(&g_cfg, t)) < 0) { note(g_cfg.why); return; }
    if (g_sel >= g_cfg.count) g_sel = g_cfg.count - 1;
    do_save(1);
    note(keep ? "The trial's words are this entry's now" : "The trial is removed");
}

// --- commands --------------------------------------------------------------

static void do_command(struct uapp *a, int code) {
    if (code != CMD_TEXT && code != CMD_ENTRIES && code != CMD_EXIT) sync_title();
    switch (code) {
    case CMD_SAVE:        do_save(0); break;
    case CMD_SAVE_ANYWAY: do_save(1); break;
    case CMD_REVERT:      revert(); break;
    case CMD_UNDO:        undo_save(); break;
    case CMD_EXIT:        uapp_quit(a, 0); return;
    case CMD_ENTRIES:     set_mode(0); break;
    case CMD_TEXT:        set_mode(1); break;
    case CMD_COPY: {
        if (g_text_mode) break;
        char title[UBOOTMENU_TITLE];
        int t = -1;
        for (int k = 1; k < 10 && t < 0; k++) {
            if (k == 1) snprintf(title, sizeof title, "%.80s (copy)", g_cfg.entry[g_sel].title);
            else snprintf(title, sizeof title, "%.80s (copy %d)", g_cfg.entry[g_sel].title, k);
            if (ubootcfg_find(&g_cfg, title) < 0) t = ubootcfg_copy(&g_cfg, g_sel, title);
        }
        if (t < 0) { note(g_cfg.why ? g_cfg.why : "Could not copy"); break; }
        g_sel = t;
        rebuild_tree();
        show_entry();
        note("Copied -- rename it, change its words, then Save");
        break;
    }
    case CMD_REMOVE:
        if (g_text_mode) break;
        if (g_sel == g_cfg.def) { note("That is the default entry -- make another one the default first"); break; }
        if (g_cfg.count <= 1) { note("The last entry cannot be removed"); break; }
        snprintf(g_ask_line[0], sizeof g_ask_line[0], "Remove \"%s\" from the boot menu?", g_cfg.entry[g_sel].title);
        snprintf(g_ask_line[1], sizeof g_ask_line[1], "Nothing changes on disk until you Save.");
        {
            static const struct uui_dialog_button b[] = {
                { "Remove", ASK_REMOVE, UUI_DLG_DANGER }, { "Cancel", ASK_CANCEL, 0 },
            };
            ask("Remove entry", 2, b, 2, ASK_CANCEL);
        }
        break;
    case CMD_DEFAULT:
        if (g_text_mode) break;
        if (ubootcfg_set_default(&g_cfg, g_sel) < 0) note(g_cfg.why);
        else { rebuild_tree(); note("Default changed -- Save to keep it"); }
        break;
    case CMD_TRY:  ask_try(); break;
    case CMD_KEEP: trial_end(1); break;
    case CMD_DROP: trial_end(0); break;
    default: return;
    }
    recheck();
    update_status();
    uapp_redraw(a);
}

static unsigned item_flags(int code) {
    switch (code) {
    case CMD_REMOVE:  return g_text_mode || g_sel == g_cfg.def || g_cfg.count <= 1 ? UUI_MI_DISABLED : 0;
    case CMD_COPY: case CMD_DEFAULT:
        return g_text_mode || !g_cfg.count ? UUI_MI_DISABLED : 0;
    case CMD_TRY:     return g_text_mode || !g_menu_state.oneshot ? UUI_MI_DISABLED : 0;
    case CMD_KEEP: case CMD_DROP:
        return ubootcfg_trial_find(&g_cfg) < 0 ? UUI_MI_DISABLED : 0;
    case CMD_SAVE: case CMD_SAVE_ANYWAY: case CMD_REVERT:
        return dirty() || g_text_mode ? 0 : UUI_MI_DISABLED;
    case CMD_ENTRIES: return g_text_mode ? 0 : UUI_MI_CHECKED;
    case CMD_TEXT:    return g_text_mode ? UUI_MI_CHECKED : 0;
    }
    return 0;
}

// --- layout and drawing ----------------------------------------------------

static int g_left_w, g_top, g_bottom, g_px, g_lbl_w, g_menu_y, g_words_y, g_prob_y;

static int per(void) { int p = ugfx_char_advance('n'); return p > 0 ? p : 8; }

static void layout_all(int cw, int ch) {
    int pad = utheme_pad(), bt = utheme_control_h(), lh = ugfx_char_h();
    int mb = uui_menubar_height(&g_menu), tb = uui_toolbar_height(&g_tb), sb = uui_statusbar_height(&g_status);
    uui_menubar_set_geometry(&g_menu, 0, 0, cw, mb);
    uui_menubar_set_bounds(&g_menu, 0, 0, cw, ch);
    uui_segmented_set_geometry(&g_view, 0, 0);
    int segw = g_view.w;
    uui_segmented_set_geometry(&g_view, cw - pad - segw, mb + (tb - g_view.h) / 2);
    uui_toolbar_ops.set_geometry(&g_tb, 0, mb, cw - segw - 2 * pad, tb);
    uui_toolbar_set_bounds(&g_tb, 0, 0, cw, ch);
    g_top = mb + tb;
    g_bottom = ch - sb;
    uui_statusbar_set_geometry(&g_status, 0, g_bottom, cw, sb);

    // Text mode: the file, then its problems under it.
    int nshow = g_nprobs < 4 ? g_nprobs : 4;
    int probh = (nshow + 1) * (lh + pad / 2) + pad;
    g_prob_y = g_bottom - probh;
    uui_textview_set_geometry(&g_text, pad, g_top, cw - pad, g_prob_y - g_top);

    // Entries: the list and the menu settings left, the entry right.
    g_left_w = per() * 34;
    int rowh = bt + pad;
    g_menu_y = g_bottom - 2 * rowh - lh - 2 * pad;
    g_tree.x = 0; g_tree.y = g_top + lh + 2 * pad; g_tree.w = g_left_w; g_tree.h = g_menu_y - g_tree.y;
    int cw2 = per() * 12;
    uui_spinbox_set_geometry(&g_timeout, g_left_w - pad - cw2, g_menu_y + lh + pad, cw2, bt);
    uui_dropdown_set_geometry(&g_next, g_left_w - pad - per() * 18, g_menu_y + lh + pad + rowh, per() * 18, bt);

    g_px = g_left_w + 1 + 2 * pad;
    g_lbl_w = per() * 8;
    int fw = cw - g_px - 2 * pad - g_lbl_w;
    uui_textbox_set_geometry(&g_title, g_px + g_lbl_w, g_top + 2 * pad, fw, bt);
    uui_dropdown_set_geometry(&g_kernel, g_px + g_lbl_w, g_top + 3 * pad + bt, fw, bt);
    g_words_y = g_top + 5 * pad + 2 * bt + lh;
    int by = g_bottom - pad - bt;
    g_words.x = g_px; g_words.y = g_words_y; g_words.w = cw - g_px - 2 * pad; g_words.h = by - pad - g_words_y;
    uui_optlist_ops.set_geometry(&g_words, g_words.x, g_words.y, g_words.w, g_words.h);
    int bw, bh;
    uui_button_natural_size(&g_save, &bw, &bh);
    if (bw < per() * 10) bw = per() * 10;
    uui_button_set_geometry(&g_save, cw - 2 * pad - bw, by, bw, bt);
    uui_button_set_geometry(&g_revert, cw - 3 * pad - 2 * bw, by, bw, bt);
    uui_dialog_set_bounds(&g_ask, 0, 0, cw, ch);
}

static void caption(struct ugfx_surface *s, int x, int y, int w, const char *t, uint32_t c, uint32_t bg) {
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    ugfx_draw_string_clipped(s, x, y, w, t, c, bg);
    ugfx_set_font(was);
}

static void draw_entries(struct ugfx_surface *s, int cw) {
    const struct utheme *t = utheme_current();
    int pad = utheme_pad(), lh = ugfx_char_h(), bt = utheme_control_h();
    ugfx_fill_rect(s, 0, g_top, g_left_w, g_bottom - g_top, t->panel_bg);
    ugfx_fill_rect(s, g_left_w, g_top, 1, g_bottom - g_top, t->separator);
    caption(s, 2 * pad, g_top + pad, g_left_w - 3 * pad, "Boot entries", t->outline, t->panel_bg);
    caption(s, 2 * pad, g_menu_y + pad / 2, g_left_w - 3 * pad, "Menu", t->outline, t->panel_bg);
    ugfx_draw_string_clipped(s, 2 * pad, g_timeout.y + (bt - lh) / 2, g_timeout.x - 3 * pad,
                             "Show the menu for", t->text, t->panel_bg);
    ugfx_draw_string_clipped(s, 2 * pad, g_next.y + (bt - lh) / 2, g_next.x - 3 * pad,
                             "Next restart", t->text, t->panel_bg);

    ugfx_fill_rect(s, g_left_w + 1, g_top, cw - g_left_w - 1, g_bottom - g_top, t->window_bg);
    if (!g_cfg.count) {
        ugfx_draw_string_clipped(s, g_px, g_top + 2 * pad, cw - g_px - pad,
                                 g_loaded ? "This grub.cfg has no menuentry." : "No boot menu on this boot.",
                                 t->text, t->window_bg);
        return;
    }
    ugfx_draw_string_clipped(s, g_px, g_title.y + (bt - lh) / 2, g_lbl_w - pad, "Title", t->text, t->window_bg);
    ugfx_draw_string_clipped(s, g_px, g_kernel.y + (bt - lh) / 2, g_lbl_w - pad, "Kernel", t->text, t->window_bg);
    const struct ubootcfg_entry *e = &g_cfg.entry[g_sel];
    caption(s, g_px, g_words_y - lh - pad, cw - g_px - pad,
            e->plain ? "Boot words" : "Boot words -- this entry's line uses GRUB quoting: edit it as Text",
            utheme_action(UTHEME_ACT_NAV), t->window_bg);

    // The line GRUB will run, in the mono face, on a field-coloured strip.
    static char line[UBOOTCFG_WORDS * UBOOTCFG_WORD + 160];
    int n = snprintf(line, sizeof line, "multiboot2 %s", e->kernel);
    for (int k = 0; k < e->nwords && n < (int)sizeof line; k++)
        n += snprintf(line + n, sizeof line - (size_t)n, " %s", e->word[k]);
    int sw = g_revert.x - g_px - 2 * pad;
    ugfx_fill_rect(s, g_px, g_save.y, sw, bt, t->panel_bg);
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_mono(UGFX_FONT_REGULAR));
    ugfx_draw_string_elided(s, g_px + pad, g_save.y + (bt - ugfx_char_h()) / 2, sw - 2 * pad, line,
                            t->text, t->panel_bg);
    ugfx_set_font(was);
}

static void draw_problems(struct ugfx_surface *s, int cw) {
    const struct utheme *t = utheme_current();
    int pad = utheme_pad(), lh = ugfx_char_h();
    ugfx_fill_rect(s, 0, g_prob_y, cw, g_bottom - g_prob_y, t->panel_bg);
    ugfx_fill_rect(s, 0, g_prob_y, cw, 1, t->separator);
    int y = g_prob_y + pad;
    uint32_t red = utheme_action(UTHEME_ACT_DANGER);
    char buf[UBOOTCFG_PROBLEM + 32];
    if (!g_nprobs) {
        ugfx_draw_string_clipped(s, 2 * pad, y, cw - 3 * pad, "No problems -- this file checks out",
                                 utheme_action(UTHEME_ACT_CREATE), t->panel_bg);
        return;
    }
    snprintf(buf, sizeof buf, "%d problem%s -- broken ones are never saved; warnings need Save anyway",
             g_nprobs, g_nprobs == 1 ? "" : "s");
    caption(s, 2 * pad, y, cw - 3 * pad, buf, red, t->panel_bg);
    for (int i = 0; i < g_nprobs && i < 4; i++) {
        y += lh + pad / 2;
        if (g_probs[i].line >= 0)
            snprintf(buf, sizeof buf, "%s  line %d: %s", g_probs[i].level == UBOOTCFG_BROKEN ? "broken " : "warning",
                     g_probs[i].line + 1, g_probs[i].msg);
        else
            snprintf(buf, sizeof buf, "%s  %s", g_probs[i].level == UBOOTCFG_BROKEN ? "broken " : "warning", g_probs[i].msg);
        ugfx_draw_string_clipped(s, 2 * pad, y, cw - 3 * pad, buf,
                                 g_probs[i].level == UBOOTCFG_BROKEN ? red : t->text, t->panel_bg);
    }
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    int cw = d->surface->w;
    layout_all(cw, d->surface->h);
    if (g_text_mode) {
        ugfx_fill_rect(d->surface, 0, g_top, utheme_pad(), g_prob_y - g_top, utheme_current()->field_bg);
        draw_problems(d->surface, cw);
    }
    else draw_entries(d->surface, cw);
    uapp_log_layout(a, "bootmgr");
    uapp_logf_layout("bootmgr: mode %s sel %d dirty %d problems %d\n",
                     g_text_mode ? "text" : "entries", g_sel, dirty(), g_nprobs);
}

// --- input -----------------------------------------------------------------

static void drain_words(void) {
    int row, what, any = 0;
    while ((what = uui_optlist_take_change(&g_words, &row))) {
        if (what & UUI_OPTLIST_CH_VALUE) g_row_edited[row] = 1;
        if (what & (UUI_OPTLIST_CH_ON | UUI_OPTLIST_CH_VALUE)) any = 1;
    }
    if (any && g_sel < g_cfg.count && g_cfg.entry[g_sel].plain) {
        apply_rows();
        recheck();
        update_status();
    }
}

static void on_widget(struct uapp *a, int id, int reason) {
    (void)reason;
    switch (id) {
    case ID_MENU: {
        int code = uui_menubar_take_code(&g_menu);
        if (code > 0) do_command(a, code); else uapp_redraw(a);
        return;
    }
    case ID_TB: {
        int code = uui_toolbar_take_code(&g_tb);
        if (code > 0) do_command(a, code);
        return;
    }
    case ID_ASK: {
        int code = uui_dialog_take_code(&g_ask);
        if (code < 0) return;
        if (code == ASK_REMOVE) {
            if (ubootcfg_remove(&g_cfg, g_sel) < 0) note(g_cfg.why);
            else { if (g_sel >= g_cfg.count) g_sel = g_cfg.count - 1; rebuild_tree(); show_entry(); note("Removed -- Save to keep it"); }
        } else if (code == ASK_TRY_NOW || code == ASK_TRY_LATER) {
            do_try(code == ASK_TRY_NOW);
        } else if (code == ASK_SAVE_ANYWAY) {
            do_save(1);
        } else if (code == ASK_KEEP || code == ASK_DROP) {
            trial_end(code == ASK_KEEP);
        }
        recheck();
        update_status();
        uapp_redraw(a);
        return;
    }
    case ID_VIEW:
        do_command(a, g_view.selected ? CMD_TEXT : CMD_ENTRIES);
        return;
    case ID_TREE: {
        int id2 = uui_tree_selected_id(&g_tree);
        if (id2 >= 0 && id2 != g_sel) {
            sync_title();
            g_sel = id2 < g_cfg.count ? id2 : g_sel;
            show_entry();
        }
        break;
    }
    case ID_WORDS:   drain_words(); break;
    case ID_TIMEOUT: {
        int v = uui_spinbox_value(&g_timeout);
        if (v != g_cfg.timeout && ubootcfg_set_timeout(&g_cfg, v) < 0) note(g_cfg.why);
        break;
    }
    case ID_KERNEL: {
        int k = uui_dropdown_selected(&g_kernel);
        if (k >= 0 && k < g_nkernels && strcmp(g_kernel_name[k], g_cfg.entry[g_sel].kernel) &&
            ubootcfg_set_kernel(&g_cfg, g_sel, g_kernel_name[k]) < 0) note(g_cfg.why);
        break;
    }
    case ID_NEXT: {
        int k = uui_dropdown_selected(&g_next);
        if (ubootmenu_set_next(k > 0 ? g_disk.entry[k - 1].title : 0) < 0) note("Could not save the next-restart choice");
        else note(k > 0 ? "The next restart takes that entry, once" : "The next restart takes the default");
        fill_next();
        rebuild_tree();
        break;
    }
    default: return;
    }
    recheck();
    update_status();
    uapp_redraw(a);
}

static void on_action(struct uapp *a, int code) {
    if (code == ID_SAVE) do_command(a, CMD_SAVE);
    else if (code == ID_REVERT) do_command(a, CMD_REVERT);
}

static void text_key(int key, unsigned mods) {
    if (key == KEY_PAGE_UP)   { utext_scroll(&g_text.tb, 5);  return; }
    if (key == KEY_PAGE_DOWN) { utext_scroll(&g_text.tb, -5); return; }
    if (utext_key(&g_text.tb, key, mods)) return;
    if (key == '\n' || key == '\r') { utext_sel_delete(&g_text.tb); utext_insert(&g_text.tb, '\n'); }
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    int code;
    if (uui_menubar_key(&g_menu, key, &code)) {
        if (code >= 0) do_command(a, code); else uapp_redraw(a);
        return;
    }
    if (key == 0x13) { do_command(a, CMD_SAVE); return; }        // Ctrl+S
    if (g_text_mode) {
        if (!uui_dialog_is_open(&g_ask)) {
            text_key(key, mods);
            utext_reveal_cursor(&g_text.tb, uui_textview_text_w(&g_text), g_text.h);
            if (cfg_from_text() == 0) recheck();
        }
        update_status();
        uapp_redraw(a);
        return;
    }
    if ((key == '\n' || key == '\r') && g_focus.current == 1) sync_title();
    drain_words();       // a commit by focus leaving reaches no on_widget
    update_status();
    uapp_redraw(a);
}

static int on_tick(struct uapp *a) {
    (void)a;
    return uui_toolbar_tick(&g_tb);
}

static int on_close(struct uapp *a) {
    (void)a;
    if (!dirty() || uui_dialog_is_open(&g_ask)) return 1;
    note("Unsaved changes -- Save, or Revert, then close");
    return 0;
}

static void on_open(struct uapp *a) {
    layout_all(uapp_width(a), uapp_height(a));
    load();
    if (g_trial_booted) {
        snprintf(g_ask_line[0], sizeof g_ask_line[0], "This boot came from the trial entry.");
        snprintf(g_ask_line[1], sizeof g_ask_line[1], "Keep its boot words, or drop the trial?");
        static const struct uui_dialog_button b[] = {
            { "Keep", ASK_KEEP, 0 }, { "Drop", ASK_DROP, 0 }, { "Later", ASK_CANCEL, 0 },
        };
        ask("The trial booted", 2, b, 3, ASK_CANCEL);
    }
    ulogf("bootmgr: open entries %d\n", g_cfg.count);
}

static void on_resize(struct uapp *a, int w, int h) {
    (void)a;
    layout_all(w, h);
}

static void on_size(int *w, int *h) {
    *w = per() * 120;
    *h = ugfx_char_h() * 38;
}

int main(void) {
    uui_menubar_init(&g_menu, menu_items, (int)(sizeof menu_items / sizeof menu_items[0]));
    g_menu.item_flags = item_flags;
    uui_toolbar_init(&g_tb, tb_items, (int)(sizeof tb_items / sizeof tb_items[0]));
    g_tb.item_flags = item_flags;
    uui_segmented_init(&g_view, VIEWS, 2, 0);
    uui_tree_init(&g_tree, 0, 0, 0, 0, g_nodes, 0);
    g_tree.sel_style = UUI_SEL_ROUNDED;
    uui_spinbox_init(&g_timeout, 5, 0, 3600, 1, "s");
    uui_dropdown_init(&g_next, 0, 0, 0, 0, g_next_items, 0);
    uui_dropdown_init(&g_kernel, 0, 0, 0, 0, g_kernel_items, 0);
    uui_textbox_init(&g_title, "");
    uui_optlist_init(&g_words, g_rows, 0);
    g_words.value_chars = 12;
    uui_textview_init(&g_text, 0, 0, 0, 0, UTHEME_TEXT, UTHEME_WHITE, UTHEME_PANEL_BG,
                      UTHEME_OUTLINE, UTHEME_SELECTION, g_textbuf, sizeof g_textbuf);
    g_text.show_caret = 1;
    utext_set_wrap(&g_text.tb, UTEXT_WRAP_OFF);
    uui_button_init(&g_save, 0, 0, 0, 0, "Save", UTHEME_ACCENT, UTHEME_ACCENT_TEXT, ID_SAVE);
    uui_button_init(&g_revert, 0, 0, 0, 0, "Revert", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_REVERT);
    g_revert.outlined = 1;
    uui_dialog_init(&g_ask);
    set_hidden(ID_TEXT, 1);

    uui_statusbar_init(&g_status);
    g_status.panes[0].text = UBOOTMENU_CFG;
    g_status.panes[0].chars = 26;
    g_status.panes[1].text = g_st_entries;
    g_status.panes[1].chars = 11;
    g_status.panes[2].text = g_st_menu;
    g_status.panes[2].chars = 11;
    g_status.panes[3].text = g_st_state;
    g_status.panes[3].chars = 0;
    g_status.count = 4;

    uui_focus_init(&g_focus, g_ring_entries, (int)(sizeof g_ring_entries / sizeof g_ring_entries[0]));

    struct uapp_desc desc = {
        .title        = "Boot Manager",
        .app_id       = "bootmgr",
        .flags        = UAPP_RESIZABLE | UAPP_SINGLE_INSTANCE,
        .on_size      = on_size,
        .min_w        = 640,
        .min_h        = 420,
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
        .on_close     = on_close,
        .tick_ms      = 100,
    };
    return uapp_run(&desc);
}
