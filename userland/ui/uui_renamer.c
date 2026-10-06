// The rename-many dialog -- see ui/uui_renamer.h.
#include "ui/uui_renamer.h"
#include <stdio.h>
#include <string.h>
#include "ui/uui.h"
#include "ui/uui_layout.h"
#include "ui/uui_label.h"
#include "ui/uui_button.h"
#include "ui/uui_checkbox.h"
#include "ui/uui_segmented.h"
#include "ui/uui_spinbox.h"
#include "ui/uui_textbox.h"
#include "ui/uui_table.h"
#include "ui/uui_focus.h"
#include "ui/utheme.h"

enum { RN_MODE = 1, RN_PATTERN, RN_START, RN_DIGITS, RN_FIND, RN_REPL, RN_CASE_MATCH,
       RN_CASING, RN_KEEP, RN_TABLE, RN_OK, RN_CANCEL };

// Static, as the rest of a dialog's state is in this toolkit: one open
// at a time, and the names alone are tens of KB.
static struct uapp_window *g_win;
static char g_dir[512];
static char g_old[UUI_RENAMER_MAX][URENAME_NAME], g_new[UUI_RENAMER_MAX][URENAME_NAME];
static int g_status[UUI_RENAMER_MAX], g_n, g_changes, g_bad;
static uui_renamer_done g_done;
static void *g_ctx;

static const char *const MODES[] = { "Numbered", "Find and replace", "Change case" };
static const char *const CASES[] = { "lower case", "UPPER CASE", "Title Case" };

static struct uui_segmented g_mode, g_casing;
static struct uui_textbox g_pattern, g_find, g_repl;
static struct uui_spinbox g_start, g_digits;
static struct uui_checkbox g_match, g_keep;
static struct uui_table g_table;
static struct uui_label g_l_name, g_l_start, g_l_digits, g_l_find, g_l_repl, g_l_case, g_status_l, g_spacer;
static char g_status_text[96];
static struct uui_button g_ok, g_cancel;

static struct uui_item g_num_items[6], g_rep_items[5], g_case_items[2], g_btn_items[4], g_root_items[7];
static struct uui_layout g_num_l, g_rep_l, g_case_l, g_btn_l, g_root_l;
static struct uui_focusable g_focusables[12];
static struct uui_focus g_focus;

static const struct uui_table_column COLS[] = {
    { "Name",     0, UUI_TALIGN_LEFT },
    { "New name", 0, UUI_TALIGN_LEFT },
};

static void cell(void *ctx, int row, int col, char *out, int cap) {
    (void)ctx;
    if (row < 0 || row >= g_n) { out[0] = '\0'; return; }
    if (col == 0) { snprintf(out, (size_t)cap, "%s", g_old[row]); return; }
    const char *why = g_status[row] == URENAME_CLASH ? "  (taken)"
                    : g_status[row] == URENAME_BAD ? "(not a name)"
                    : g_status[row] == URENAME_SAME ? "  (unchanged)" : "";
    snprintf(out, (size_t)cap, "%s%s", g_new[row], why);
}

// A clash or a bad name is shaded, so the eye finds it in a long list.
static int heat(void *ctx, int row, int col) {
    (void)ctx;
    return col == 1 && row >= 0 && row < g_n &&
           (g_status[row] == URENAME_CLASH || g_status[row] == URENAME_BAD) ? 160 : 0;
}

// The rule from the controls, the plan from the rule, the window from
// the plan -- on every change, so the preview is never stale.
static void replan(void) {
    struct urename_rule r;
    memset(&r, 0, sizeof r);
    r.mode = g_mode.selected;
    strlcpy(r.pattern, uui_textbox_text(&g_pattern), sizeof r.pattern);
    r.start = uui_spinbox_value(&g_start);
    r.digits = uui_spinbox_value(&g_digits);
    strlcpy(r.find, uui_textbox_text(&g_find), sizeof r.find);
    strlcpy(r.replace, uui_textbox_text(&g_repl), sizeof r.replace);
    r.match_case = g_match.checked;
    r.casing = g_casing.selected;
    r.keep_ext = g_keep.checked;
    g_changes = urename_plan(&r, g_dir, g_old, g_n, g_new, g_status);
    g_bad = 0;
    for (int i = 0; i < g_n; i++)
        if (g_status[i] == URENAME_CLASH || g_status[i] == URENAME_BAD) g_bad++;
    if (g_bad)
        snprintf(g_status_text, sizeof g_status_text, "%d name%s cannot be used -- change the rule",
                 g_bad, g_bad == 1 ? "" : "s");
    else
        snprintf(g_status_text, sizeof g_status_text, "%d of %d will be renamed", g_changes, g_n);
    g_ok.disabled = g_bad || !g_changes;
    g_root_items[1].hidden = r.mode != URENAME_NUMBER;
    g_root_items[2].hidden = r.mode != URENAME_REPLACE;
    g_root_items[3].hidden = r.mode != URENAME_CASE;
    uui_table_set_rows(&g_table, g_n);
}

static void close_window(void) {
    if (g_win) uapp_window_close(g_win);
    g_win = 0;
}

static void commit(void) {
    replan();
    if (g_ok.disabled) return;
    static char olds[UUI_RENAMER_MAX][URENAME_NAME], news[UUI_RENAMER_MAX][URENAME_NAME];
    int k = 0;
    for (int i = 0; i < g_n; i++)
        if (g_status[i] == URENAME_OK) {
            memcpy(olds[k], g_old[i], URENAME_NAME);
            memcpy(news[k], g_new[i], URENAME_NAME);
            k++;
        }
    uui_renamer_done done = g_done;
    void *ctx = g_ctx;
    close_window();
    if (done) done(ctx, g_dir, olds, news, k);
}

static void on_widget(struct uapp_window *w, int id, int reason) {
    (void)id; (void)reason;
    replan();
    uapp_window_redraw(w);
}

static void on_action(struct uapp_window *w, int code) {
    if (code == RN_OK) { commit(); return; }
    if (code == RN_CANCEL) { close_window(); return; }
    uapp_window_redraw(w);
}

static void on_key(struct uapp_window *w, int key, unsigned mods) {
    (void)mods;
    if (key == 0x1B) { close_window(); return; }
    if (key == '\n' || key == '\r') { commit(); return; }
    replan();
    uapp_window_redraw(w);
}

static void on_close(struct uapp_window *w) { (void)w; close_window(); }

int uui_renamer_is_open(void) { return g_win != 0; }

static struct uui_item item(const struct uui_widget_ops *ops, void *w, int id, const char *name,
                            unsigned flags) {
    return (struct uui_item){ .ops = ops, .widget = w, .id = id, .name = name, .flags = flags };
}

void uui_renamer_open(struct uapp *a, const char *dir, char (*names)[URENAME_NAME], int n,
                      uui_renamer_done done, void *ctx) {
    if (g_win || n <= 0) return;
    if (n > UUI_RENAMER_MAX) n = UUI_RENAMER_MAX;
    strlcpy(g_dir, dir, sizeof g_dir);
    for (int i = 0; i < n; i++) strlcpy(g_old[i], names[i], URENAME_NAME);
    g_n = n;
    g_done = done;
    g_ctx = ctx;

    uint32_t bg = UTHEME_WINDOW_BG, fg = UTHEME_TEXT;
    uui_segmented_init(&g_mode, MODES, 3, URENAME_NUMBER);
    uui_segmented_init(&g_casing, CASES, 3, URENAME_TITLE);
    // The folder's own name as the pattern's start, Dolphin's default.
    char base[URENAME_NAME];
    const char *slash = strrchr(dir, '/');
    snprintf(base, sizeof base, "%s-##", slash && slash[1] ? slash + 1 : "file");
    uui_textbox_init(&g_pattern, base);
    uui_textbox_init(&g_find, "");
    uui_textbox_init(&g_repl, "");
    uui_spinbox_init(&g_start, 1, 0, 99999, 1, 0);
    uui_spinbox_init(&g_digits, 0, 0, 6, 1, 0);
    uui_checkbox_init(&g_match, 0, 0, 0, "Match case", bg, fg);
    uui_checkbox_init(&g_keep, 0, 0, 0, "Keep extensions", bg, fg);
    g_keep.checked = 1;
    uui_label_init(&g_l_name, "Name:");
    uui_label_init(&g_l_start, "Start at:");
    uui_label_init(&g_l_digits, "Digits:");
    uui_label_init(&g_l_find, "Find:");
    uui_label_init(&g_l_repl, "Replace with:");
    uui_label_init(&g_l_case, "Make it:");
    uui_label_init(&g_status_l, g_status_text);
    uui_label_init(&g_spacer, "");
    uui_table_init(&g_table, 0, 0, 0, 0, COLS, 2, cell, 0);
    uui_table_set_heat(&g_table, heat);
    uui_button_init(&g_ok, 0, 0, 0, 0, "Rename", UTHEME_ACCENT, UTHEME_ACCENT_TEXT, RN_OK);
    uui_button_init(&g_cancel, 0, 0, 0, 0, "Cancel", UTHEME_BUTTON_BG, UTHEME_TEXT, RN_CANCEL);

    g_num_items[0] = item(&uui_label_ops, &g_l_name, 0, 0, 0);
    g_num_items[1] = item(&uui_textbox_ops, &g_pattern, RN_PATTERN, "pattern", UUI_FILL_W);
    g_num_items[2] = item(&uui_label_ops, &g_l_start, 0, 0, 0);
    g_num_items[3] = item(&uui_spinbox_ops, &g_start, RN_START, "start", 0);
    g_num_items[4] = item(&uui_label_ops, &g_l_digits, 0, 0, 0);
    g_num_items[5] = item(&uui_spinbox_ops, &g_digits, RN_DIGITS, "digits", 0);
    g_num_l = (struct uui_layout){ .dir = UUI_ROW, .items = g_num_items, .count = 6, .gap = 8 };
    g_rep_items[0] = item(&uui_label_ops, &g_l_find, 0, 0, 0);
    g_rep_items[1] = item(&uui_textbox_ops, &g_find, RN_FIND, "find", UUI_FILL_W);
    g_rep_items[2] = item(&uui_label_ops, &g_l_repl, 0, 0, 0);
    g_rep_items[3] = item(&uui_textbox_ops, &g_repl, RN_REPL, "replace", UUI_FILL_W);
    g_rep_items[4] = item(&uui_checkbox_ops, &g_match, RN_CASE_MATCH, "match", 0);
    g_rep_l = (struct uui_layout){ .dir = UUI_ROW, .items = g_rep_items, .count = 5, .gap = 8 };
    g_case_items[0] = item(&uui_label_ops, &g_l_case, 0, 0, 0);
    g_case_items[1] = item(&uui_segmented_ops, &g_casing, RN_CASING, "casing", 0);
    g_case_l = (struct uui_layout){ .dir = UUI_ROW, .items = g_case_items, .count = 2, .gap = 8 };
    g_btn_items[0] = item(&uui_label_ops, &g_status_l, 0, "status", UUI_FILL_W);
    g_btn_items[1] = item(&uui_label_ops, &g_spacer, 0, 0, 0);
    g_btn_items[2] = item(&uui_button_ops, &g_ok, RN_OK, "ok", 0);
    g_btn_items[3] = item(&uui_button_ops, &g_cancel, RN_CANCEL, "cancel", 0);
    g_btn_l = (struct uui_layout){ .dir = UUI_ROW, .items = g_btn_items, .count = 4, .gap = 8 };

    g_root_items[0] = item(&uui_segmented_ops, &g_mode, RN_MODE, "mode", 0);
    g_root_items[1] = item(&uui_layout_ops, &g_num_l, 0, 0, UUI_FILL_W);
    g_root_items[2] = item(&uui_layout_ops, &g_rep_l, 0, 0, UUI_FILL_W);
    g_root_items[3] = item(&uui_layout_ops, &g_case_l, 0, 0, UUI_FILL_W);
    g_root_items[4] = item(&uui_checkbox_ops, &g_keep, RN_KEEP, "keep", 0);
    g_root_items[5] = item(&uui_table_ops, &g_table, RN_TABLE, "preview", UUI_FILL_W | UUI_FILL_H);
    g_root_items[6] = item(&uui_layout_ops, &g_btn_l, 0, 0, UUI_FILL_W);
    g_root_l = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_root_items, .count = 7,
                                    .margin = 12, .gap = 10 };

    int f = 0;
    g_focusables[f++] = (struct uui_focusable){ &g_mode, &uui_segmented_ops };
    g_focusables[f++] = (struct uui_focusable){ &g_pattern, &uui_textbox_ops };
    g_focusables[f++] = (struct uui_focusable){ &g_start, &uui_spinbox_ops };
    g_focusables[f++] = (struct uui_focusable){ &g_digits, &uui_spinbox_ops };
    g_focusables[f++] = (struct uui_focusable){ &g_find, &uui_textbox_ops };
    g_focusables[f++] = (struct uui_focusable){ &g_repl, &uui_textbox_ops };
    g_focusables[f++] = (struct uui_focusable){ &g_match, &uui_checkbox_ops };
    g_focusables[f++] = (struct uui_focusable){ &g_casing, &uui_segmented_ops };
    g_focusables[f++] = (struct uui_focusable){ &g_keep, &uui_checkbox_ops };
    g_focusables[f++] = (struct uui_focusable){ &g_ok, &uui_button_ops };
    g_focusables[f++] = (struct uui_focusable){ &g_cancel, &uui_button_ops };
    uui_focus_init(&g_focus, g_focusables, f);
    uui_focus_set(&g_focus, 1);
    replan();

    static char title[48];
    snprintf(title, sizeof title, "Rename %d item%s", n, n == 1 ? "" : "s");
    struct uapp_window_desc d = {
        .title = title,
        .w = ugfx_char_advance('n') * 80,
        .h = ugfx_char_h() * 26,
        .flags = UAPP_WIN_MODAL,
        .widgets = g_root_items,
        .widget_count = 7,
        .layout = &g_root_l,
        .focus = &g_focus,
        .on_widget = on_widget,
        .on_action = on_action,
        .on_key = on_key,
        .on_close = on_close,
        .log_prefix = "renamer",
    };
    g_win = uapp_window_open(a, &d);
}
