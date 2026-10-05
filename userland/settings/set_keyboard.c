// Input > Keyboard's own section: the layout list, its preview and a
// field to try it in. Split out as set_clock.c is, for the same reason --
// a page whose settings need more than their generic rows.
//
// THREE SLOTS, ONE CARD EACH, BUT NOT THE GENERIC ONES:
//   system.keyboard_layouts  -> the Layouts card. It EDITS THE SLOT'S OWN
//                               TEXT ("fi,us,de"), so Apply, Reset and
//                               the changed marker work as for any field
//   system.keyboard_layout   -> nothing: the ACTIVE layout is the
//                               taskbar's and Super+Space's, not a setting
//                               a person stages
//   system.keyboard_dead_keys-> its generic switch, then the Preview card
#include "settings/settings_internal.h"
#include "lib/usetting.h"
#include "lib/ukeymap.h"
#include "ui/uui_keymap.h"
#include "ui/uui_listbox.h"
#include <ctype.h>

#define SET_KB_LIST   "system.keyboard_layouts"
#define SET_KB_ACTIVE "system.keyboard_layout"
#define SET_KB_DEAD   "system.keyboard_dead_keys"
#define KB_MAX        8     // keyboard_config.c's KB_LAYOUTS_MAX
#define KB_ALL        32    // every layout file there is

int g_kbd_page;
static int s_list = -1, s_active = -1, s_dead = -1;

// The staged list, as codes, and its rows as the listbox shows them.
static char g_codes[KB_MAX][16];
static int g_ncodes;
static char g_rows[KB_MAX][SETTING_ABI_VALUE_MAX + 16];
static const char *g_row_ptr[KB_MAX];

static struct uui_listbox g_kb_list;
static struct uui_button g_kb_add, g_kb_remove, g_kb_up, g_kb_down;
static struct uui_item KB_BTNS[5], KB_COL[2];
static struct uui_layout g_kb_btn_row, g_kb_col;
static struct uui_setting_row g_kb_row;
static struct uui_label g_kb_gap;

static struct ukeymap g_kb_map;
static struct uui_keymap g_kb_view;
static struct uui_label g_kb_try_l;
static struct uui_textbox g_kb_try;
static struct uui_item KB_TRY[2], KB_PCOL[2];
static struct uui_layout g_kb_try_row, g_kb_pcol;
static struct uui_setting_row g_kb_prev_row;
static char g_kb_prev_title[SETTING_ABI_VALUE_MAX + 16];

// TRY IT types with the PREVIEWED layout: while the field has focus that
// layout is made active, and the one before it comes back after.
static char g_kb_restore[16];
static int g_kb_trying;

static int slot_named(const char *name) {
    for (int i = 0; i < g_slot_count; i++)
        if (g_slot[i].setting >= 0 && strcmp(g_name[g_slot[i].setting], name) == 0) return i;
    return -1;
}

// A layout's display name, from the active-layout setting's choices
// (/etc/settings.d names them); the code itself when it has none.
static const char *name_of(const char *code) {
    if (s_active >= 0) {
        const struct slot *a = &g_slot[s_active];
        for (int i = 0; i < a->choice_count; i++)
            if (!strcmp(a->choice_raw[i], code)) return a->choice[i];
    }
    return code;
}

static void parse_list(const char *v) {
    g_ncodes = 0;
    for (const char *p = v; *p && g_ncodes < KB_MAX; ) {
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n && n < sizeof g_codes[0]) {
            memcpy(g_codes[g_ncodes], p, n);
            g_codes[g_ncodes][n] = 0;
            g_ncodes++;
        }
        p = e ? e + 1 : p + n;
    }
}

static void build_rows(void) {
    for (int i = 0; i < g_ncodes; i++) {
        snprintf(g_rows[i], sizeof g_rows[i], i == 0 ? "%s   (startup)" : "%s", name_of(g_codes[i]));
        g_row_ptr[i] = g_rows[i];
    }
    uui_listbox_set_items(&g_kb_list, g_row_ptr, g_ncodes);
    if (g_kb_list.selected >= g_ncodes) g_kb_list.selected = g_ncodes - 1;
    int sel = g_kb_list.selected;
    g_kb_remove.disabled = g_ncodes <= 1 || sel < 0;
    g_kb_up.disabled = sel <= 0;
    g_kb_down.disabled = sel < 0 || sel >= g_ncodes - 1;
    g_kb_add.disabled = g_ncodes >= KB_MAX;
}

// The previewed layout is the selected row's.
static void load_preview(void) {
    int sel = g_kb_list.selected;
    const char *code = sel >= 0 && sel < g_ncodes ? g_codes[sel] : "";
    ukeymap_load(&g_kb_map, code);
    snprintf(g_kb_prev_title, sizeof g_kb_prev_title, "Preview: %s", name_of(code));
}

// THE EDIT: the slot's text becomes the list, and the page treats it as
// it treats a typed value.
static void stage_list(void) {
    char v[SETTING_ABI_VALUE_MAX] = "";
    for (int i = 0; i < g_ncodes; i++) {
        if (i) strlcat(v, ",", sizeof v);
        strlcat(v, g_codes[i], sizeof v);
    }
    uui_textbox_set_text(&g_slot[s_list].text, v);
    control_changed(s_list);
    g_kb_row.changed = g_slot[s_list].row.changed;
    build_rows();
    load_preview();
}

// Makes `code` active without touching the list, and keeps this page
// from reloading over it: the switch moves the registry generation, and
// on_tick reopens a page whose generation moved.
static void set_active(const char *code) {
    if (!code[0]) return;
    usetting_set(SET_KB_ACTIVE, code);
    g_generation = registry_generation();
}

static void try_end(void) {
    if (!g_kb_trying) return;
    g_kb_trying = 0;
    set_active(g_kb_restore);
    ulogf("settings: keyboard try ended, back to %s\n", g_kb_restore);
}

void kbd_init(void) {
    uui_listbox_init(&g_kb_list, 0, 0, 0, 0, g_row_ptr, 0);
    g_kb_list.sel_bg = UTHEME_ACCENT;          // a selected row is the accent,
    g_kb_list.sel_fg = UTHEME_ACCENT_TEXT;     // as the sidebar's is
    uui_button_init(&g_kb_add, 0, 0, 0, 0, "Add a layout...", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_KB_ADD);
    uui_button_init(&g_kb_remove, 0, 0, 0, 0, "Remove", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_KB_REMOVE);
    uui_button_init(&g_kb_up, 0, 0, 0, 0, "Move up", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_KB_UP);
    uui_button_init(&g_kb_down, 0, 0, 0, 0, "Move down", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_KB_DOWN);
    uui_label_init(&g_kb_gap, "");
    uui_keymap_init(&g_kb_view, &g_kb_map);
    g_kb_view.bg = UTHEME_WHITE;
    uui_label_init(&g_kb_try_l, "Try it");
    uui_textbox_init(&g_kb_try, "");
    g_kb_row.desc_rows = 1;
    g_kb_prev_row.desc_rows = 1;
}

void kbd_page_opened(void) {
    int was = g_kbd_page;
    s_list = slot_named(SET_KB_LIST);
    s_active = slot_named(SET_KB_ACTIVE);
    s_dead = slot_named(SET_KB_DEAD);
    g_kbd_page = s_list >= 0;
    if (!g_kbd_page) { try_end(); return; }
    int keep = was ? g_kb_list.selected : 0;
    parse_list(uui_textbox_text(&g_slot[s_list].text));
    g_kb_list.selected = keep >= 0 && keep < g_ncodes ? keep : 0;
    g_kb_row.changed = 0;
    build_rows();
    load_preview();
}

int kbd_emit_slot(struct uui_item *out, int n, int i, struct uui_focusable *focus, int *nfocus) {
    if (!g_kbd_page) return -1;
    if (i == s_active) return n;          // the taskbar's, not the page's
    if (i != s_list) return -1;
    int fid = g_slot[i].setting;
    KB_BTNS[0] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_kb_add, .id = ID_KB_ADD, .name = "kb_add" };
    KB_BTNS[1] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_kb_remove, .id = ID_KB_REMOVE, .name = "kb_remove" };
    KB_BTNS[2] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_kb_gap, .flags = UUI_FILL_W };
    KB_BTNS[3] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_kb_up, .id = ID_KB_UP, .name = "kb_up" };
    KB_BTNS[4] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_kb_down, .id = ID_KB_DOWN, .name = "kb_down" };
    g_kb_btn_row = (struct uui_layout){ .dir = UUI_ROW, .items = KB_BTNS, .count = 5 };
    // Rows enough for the whole list: it is at most eight long.
    g_kb_list.h = (g_ncodes < 3 ? 3 : g_ncodes) * uui_listbox_row_h(&g_kb_list) + 4;
    KB_COL[0] = (struct uui_item){ .ops = &uui_listbox_ops, .widget = &g_kb_list, .id = ID_KB_LIST,
                                   .flags = UUI_FILL_W, .main_size = g_kb_list.h, .name = "kb_list" };
    KB_COL[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_kb_btn_row, .flags = UUI_FILL_W };
    g_kb_col = (struct uui_layout){ .dir = UUI_COLUMN, .items = KB_COL, .count = 2 };
    g_kb_row.title = g_label[fid];
    g_kb_row.desc = g_desc[fid];
    g_kb_row.stacked = 1;
    g_kb_row.control = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_kb_col, .flags = UUI_FILL_W };
    out[n++] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_kb_row,
                                  .flags = UUI_FILL_W, .name = "kb_layouts" };
    focus[(*nfocus)++] = (struct uui_focusable){ &g_kb_list, &uui_listbox_focus_ops };
    if (!g_kb_add.disabled) focus[(*nfocus)++] = (struct uui_focusable){ &g_kb_add, &uui_button_ops };
    if (!g_kb_remove.disabled) focus[(*nfocus)++] = (struct uui_focusable){ &g_kb_remove, &uui_button_ops };
    if (!g_kb_up.disabled) focus[(*nfocus)++] = (struct uui_focusable){ &g_kb_up, &uui_button_ops };
    if (!g_kb_down.disabled) focus[(*nfocus)++] = (struct uui_focusable){ &g_kb_down, &uui_button_ops };
    return n;
}

int kbd_emit_after(struct uui_item *out, int n, int i, struct uui_focusable *focus, int *nfocus) {
    if (!g_kbd_page || i != s_dead) return n;
    int kw = 0, kh = 0;
    uui_keymap_natural_size(&g_kb_view, &kw, &kh);
    KB_TRY[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_kb_try_l };
    KB_TRY[1] = (struct uui_item){ .ops = &uui_textbox_ops, .widget = &g_kb_try, .id = ID_KB_TRY,
                                   .flags = UUI_FILL_W, .name = "kb_try" };
    g_kb_try_row = (struct uui_layout){ .dir = UUI_ROW, .items = KB_TRY, .count = 2 };
    KB_PCOL[0] = (struct uui_item){ .ops = &uui_keymap_ops, .widget = &g_kb_view, .flags = UUI_FILL_W,
                                    .main_size = kh, .name = "kb_preview" };
    KB_PCOL[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_kb_try_row, .flags = UUI_FILL_W };
    g_kb_pcol = (struct uui_layout){ .dir = UUI_COLUMN, .items = KB_PCOL, .count = 2 };
    g_kb_prev_row.title = g_kb_prev_title;
    g_kb_prev_row.desc = "Shift above, AltGr in blue; a dashed key is a dead key. Try it types with this layout";
    g_kb_prev_row.stacked = 1;
    g_kb_prev_row.control = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_kb_pcol, .flags = UUI_FILL_W };
    out[n++] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_kb_prev_row,
                                  .flags = UUI_FILL_W, .name = "kb_preview_card" };
    focus[(*nfocus)++] = (struct uui_focusable){ &g_kb_try, &uui_textbox_ops };
    return n;
}

// --- Add a layout... ------------------------------------------------------

enum { KP_FIND = 1, KP_LIST, KP_ADD, KP_CANCEL };
static struct uapp_window *g_kp_win;
static struct uui_textbox g_kp_find;
static struct uui_listbox g_kp_list;
static struct uui_button g_kp_add, g_kp_cancel;
static struct uui_label g_kp_gap;
static struct uui_item KP[3], KP_BTNS[3];
static struct uui_layout g_kp_layout, g_kp_btn_row;
static struct uui_focusable KP_FOCUS[4];
static struct uui_focus g_kp_focus;
static const char *g_kp_codes[KB_ALL], *g_kp_rows[KB_ALL];
static int g_kp_n;

static int has_word(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    if (!n) return 1;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

// The layouts NOT in the list, whose name or code holds what is typed.
static void kp_filter(void) {
    g_kp_n = 0;
    const char *q = uui_textbox_text(&g_kp_find);
    const struct slot *a = &g_slot[s_active];
    for (int i = 0; i < a->choice_count && g_kp_n < KB_ALL; i++) {
        int listed = 0;
        for (int j = 0; j < g_ncodes; j++) if (!strcmp(g_codes[j], a->choice_raw[i])) listed = 1;
        if (listed || !(has_word(a->choice[i], q) || has_word(a->choice_raw[i], q))) continue;
        g_kp_codes[g_kp_n] = a->choice_raw[i];
        g_kp_rows[g_kp_n] = a->choice[i];
        g_kp_n++;
    }
    uui_listbox_set_items(&g_kp_list, g_kp_rows, g_kp_n);
    g_kp_list.selected = g_kp_n ? 0 : -1;
    g_kp_add.disabled = g_kp_n == 0;
}

static void kp_close(struct uapp_window *w) {
    uapp_window_close(w);
    g_kp_win = 0;
    if (g_app) uapp_redraw(g_app);
}

static void kp_add(struct uapp_window *w) {
    int sel = g_kp_list.selected;
    if (sel >= 0 && sel < g_kp_n && g_ncodes < KB_MAX) {
        strlcpy(g_codes[g_ncodes], g_kp_codes[sel], sizeof g_codes[0]);
        g_ncodes++;
        g_kb_list.selected = g_ncodes - 1;
        stage_list();
        ulogf("settings: keyboard added %s\n", g_codes[g_ncodes - 1]);
        relayout_page();
    }
    kp_close(w);
}

static void kp_on_action(struct uapp_window *w, int code) {
    if (code == KP_ADD) kp_add(w);
    if (code == KP_CANCEL) kp_close(w);
}

// ENTER IS THE DEFAULT BUTTON, Add, from the field or the list.
static void kp_on_key(struct uapp_window *w, int key, unsigned mods) {
    (void)mods;
    if (key == '\n' || key == '\r') kp_add(w);
}

static void kp_on_widget(struct uapp_window *w, int id, int reason) {
    if (id == KP_FIND && reason == UUI_REASON_KEY) kp_filter();
    uapp_window_redraw(w);
}

static void kp_open(struct uapp *a) {
    if (g_kp_win && uapp_window_is_open(g_kp_win)) return;
    uui_textbox_init(&g_kp_find, "");
    uui_listbox_init(&g_kp_list, 0, 0, 0, 0, g_kp_rows, 0);
    g_kp_list.sel_bg = UTHEME_ACCENT;
    g_kp_list.sel_fg = UTHEME_ACCENT_TEXT;
    uui_button_init(&g_kp_add, 0, 0, 0, 0, "Add", UTHEME_ACCENT, UTHEME_ACCENT_TEXT, KP_ADD);
    uui_button_init(&g_kp_cancel, 0, 0, 0, 0, "Cancel", UTHEME_BUTTON_BG, UTHEME_TEXT, KP_CANCEL);
    uui_label_init(&g_kp_gap, "");
    kp_filter();
    int rh = uui_listbox_row_h(&g_kp_list);
    KP[0] = (struct uui_item){ .ops = &uui_textbox_ops, .widget = &g_kp_find, .id = KP_FIND,
                               .flags = UUI_FILL_W, .name = "kp_find" };
    KP[1] = (struct uui_item){ .ops = &uui_listbox_ops, .widget = &g_kp_list, .id = KP_LIST,
                               .flags = UUI_FILL_W, .main_size = rh * 8 + 4, .name = "kp_list" };
    KP_BTNS[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_kp_gap, .flags = UUI_FILL_W };
    KP_BTNS[1] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_kp_cancel, .id = KP_CANCEL, .name = "kp_cancel" };
    KP_BTNS[2] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_kp_add, .id = KP_ADD, .name = "kp_add" };
    g_kp_btn_row = (struct uui_layout){ .dir = UUI_ROW, .items = KP_BTNS, .count = 3 };
    KP[2] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_kp_btn_row, .flags = UUI_FILL_W };
    g_kp_layout = (struct uui_layout){ .dir = UUI_COLUMN, .items = KP, .count = 3 };
    int nf = 0;
    KP_FOCUS[nf++] = (struct uui_focusable){ &g_kp_find, &uui_textbox_ops };
    KP_FOCUS[nf++] = (struct uui_focusable){ &g_kp_list, &uui_listbox_focus_ops };
    KP_FOCUS[nf++] = (struct uui_focusable){ &g_kp_cancel, &uui_button_ops };
    KP_FOCUS[nf++] = (struct uui_focusable){ &g_kp_add, &uui_button_ops };
    uui_focus_init(&g_kp_focus, KP_FOCUS, nf);
    uui_focus_set(&g_kp_focus, 0);   // typing searches at once
    int w = 0, h = 0;
    uui_layout_natural_size(&g_kp_layout, &w, &h);
    int want = ugfx_char_w() * 32;
    if (w < want) w = want;
    g_kp_win = uapp_window_open(a, &(struct uapp_window_desc){
        .title = "Add a layout", .w = w, .h = h, .flags = UAPP_WIN_MODAL,
        .widgets = KP, .widget_count = 3, .layout = &g_kp_layout, .focus = &g_kp_focus,
        .on_widget = kp_on_widget, .on_action = kp_on_action, .on_key = kp_on_key,
        .on_close = kp_close,
        .log_prefix = "settings.kblayout",
    });
    ulogf("settings: keyboard add dialog %s\n", g_kp_win ? "open" : "FAILED");
}

// --- events ---------------------------------------------------------------

int kbd_on_widget(struct uapp *a, int id) {
    (void)a;
    if (id == ID_KB_LIST) {
        build_rows();
        load_preview();
        relayout_page();
        return 1;
    }
    return id == ID_KB_TRY;
}

int kbd_on_action(struct uapp *a, int code) {
    int sel = g_kb_list.selected;
    switch (code) {
    case ID_KB_ADD:
        kp_open(a);
        return 1;
    case ID_KB_REMOVE:
        if (g_ncodes <= 1 || sel < 0) return 1;
        for (int i = sel; i < g_ncodes - 1; i++) strlcpy(g_codes[i], g_codes[i + 1], sizeof g_codes[0]);
        g_ncodes--;
        if (sel >= g_ncodes) g_kb_list.selected = g_ncodes - 1;
        break;
    case ID_KB_UP:
    case ID_KB_DOWN: {
        int to = code == ID_KB_UP ? sel - 1 : sel + 1;
        if (sel < 0 || to < 0 || to >= g_ncodes) return 1;
        char t[16];
        strlcpy(t, g_codes[sel], sizeof t);
        strlcpy(g_codes[sel], g_codes[to], sizeof g_codes[0]);
        strlcpy(g_codes[to], t, sizeof g_codes[0]);
        g_kb_list.selected = to;
        break;
    }
    default:
        return 0;
    }
    stage_list();
    relayout_page();
    return 1;
}

// Once a frame: the Try it field took focus, or lost it.
int kbd_tick(void) {
    if (!g_kbd_page) return 0;
    int sel = g_kb_list.selected;
    if (g_kb_try.active && !g_kb_trying && sel >= 0 && sel < g_ncodes) {
        char cur[SETTING_ABI_VALUE_MAX];
        if (!usetting_get(SET_KB_ACTIVE, cur, sizeof cur)) return 0;
        strlcpy(g_kb_restore, cur, sizeof g_kb_restore);
        g_kb_trying = 1;
        set_active(g_codes[sel]);
        ulogf("settings: keyboard try %s (was %s)\n", g_codes[sel], g_kb_restore);
        return 1;
    }
    if (!g_kb_try.active && g_kb_trying) { try_end(); return 1; }
    return 0;
}

void kbd_shutdown(void) { try_end(); }
