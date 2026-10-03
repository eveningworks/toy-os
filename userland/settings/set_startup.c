// System > Boot menu: GRUB's default entry, its menu timeout and a one-shot
// next restart -- Windows' "Startup and Recovery" box; everything else
// about the menu is the Boot Manager's, one button away.
//
// NOT REGISTRY SETTINGS. These live in grub.cfg and grubenv, so they are
// not staged for the footer's Apply: each change is written at once
// through lib/ubootcfg.h, checked and with the old file kept as
// grub.cfg.bak -- the same save as `bootcfg` and the Boot Manager. A
// change the check calls risky (timeout 0) is refused here and named,
// since this page has no "save anyway"; bootcfg --force does it.
#include "settings_internal.h"
#include "lib/ubootcfg.h"

int g_show_startup;
struct uui_setting_row g_su_row[3];
struct uui_custom g_su_list;
struct uui_button g_su_open;

static struct uui_dropdown g_su_default, g_su_next;
static struct uui_spinbox g_su_timeout;
static struct ubootcfg g_su_cfg;
static struct ubootmenu g_su_menu;
static int g_su_ok;
static char g_su_title[UBOOTMENU_MAX + 1][UBOOTMENU_TITLE];
static const char *g_su_def_items[UBOOTMENU_MAX], *g_su_next_items[UBOOTMENU_MAX + 1];

void startup_load(void) {
    g_su_ok = ubootcfg_load(&g_su_cfg, UBOOTMENU_CFG) == 0;
    ubootmenu_read(&g_su_menu, UBOOTMENU_CFG);
    int n = g_su_ok ? g_su_cfg.count : 0;
    snprintf(g_su_title[0], sizeof g_su_title[0], "As the default");
    g_su_next_items[0] = g_su_title[0];
    for (int i = 0; i < n; i++) {
        snprintf(g_su_title[i + 1], sizeof g_su_title[0], "%s", g_su_cfg.entry[i].title);
        g_su_def_items[i] = g_su_next_items[i + 1] = g_su_title[i + 1];
    }
    uui_dropdown_set_items(&g_su_default, g_su_def_items, n);
    uui_dropdown_set_selected(&g_su_default, g_su_cfg.def >= 0 ? g_su_cfg.def : 0);
    uui_dropdown_set_items(&g_su_next, g_su_next_items, n + 1);
    uui_dropdown_set_selected(&g_su_next, g_su_menu.next >= 0 ? g_su_menu.next + 1 : 0);
    uui_spinbox_set_value(&g_su_timeout, g_su_cfg.timeout >= 0 ? g_su_cfg.timeout : 0);
    g_su_default.disabled = g_su_timeout.disabled = !g_su_ok;
    g_su_next.disabled = !g_su_ok || !g_su_menu.oneshot;
    g_su_list.h = (n ? n : 1) * (2 * ugfx_char_h() + utheme_pad()) + 2 * utheme_pad();
}

// Save `c` if the edit introduced no problem; otherwise say which.
static void save_checked(struct ubootcfg *c, const char *done) {
    static struct ubootcfg_problem was[16], now[16];
    int nw = ubootcfg_check(&g_su_cfg, 0, UBOOTCFG_CHECK_FILES, was, 16);
    int nn = ubootcfg_check(c, &g_su_cfg, UBOOTCFG_CHECK_FILES, now, 16);
    for (int i = 0; i < nn; i++) {
        int old = 0;
        for (int j = 0; j < nw && !old; j++) old = was[j].level == now[i].level && !strcmp(was[j].msg, now[i].msg);
        if (old) continue;
        snprintf(g_status, sizeof g_status, "Not saved: %s", now[i].msg);
        ulogf("settings: startup refused: %s\n", now[i].msg);
        return;
    }
    const char *why = 0;
    if (ubootcfg_save(c, UBOOTMENU_CFG, &why) < 0) {
        snprintf(g_status, sizeof g_status, "Not saved: %s", why);
        return;
    }
    snprintf(g_status, sizeof g_status, "%s -- the previous boot menu is kept as grub.cfg.bak", done);
    ulogf("settings: startup %s\n", done);
}

void startup_changed(int id) {
    if (!g_su_ok) return;
    static struct ubootcfg c;
    c = g_su_cfg;
    if (id == ID_SU_DEFAULT) {
        int e = uui_dropdown_selected(&g_su_default);
        if (e != g_su_cfg.def && ubootcfg_set_default(&c, e) == 0) save_checked(&c, "Default entry saved");
    } else if (id == ID_SU_TIMEOUT) {
        int v = uui_spinbox_value(&g_su_timeout);
        if (v != g_su_cfg.timeout && ubootcfg_set_timeout(&c, v) == 0) save_checked(&c, "Menu timeout saved");
    } else if (id == ID_SU_NEXT) {
        int k = uui_dropdown_selected(&g_su_next);
        if (ubootmenu_set_next(k > 0 ? g_su_cfg.entry[k - 1].title : 0) < 0)
            snprintf(g_status, sizeof g_status, "Could not save the next-restart choice");
        else
            snprintf(g_status, sizeof g_status, "%s", k > 0 ? "The next restart takes that entry, once"
                                                            : "The next restart takes the default");
    }
    startup_load();   // what is on disk now, refused or not
}

// Each entry as a title and the line it boots, on one card.
static void draw_list(struct ugfx_surface *s, const struct uui_custom *cu) {
    const struct utheme *t = utheme_current();
    int pad = utheme_pad(), lh = ugfx_char_h();
    int r = ugfx_char_h() / 2;
    uui_fill_round_rect(s, cu->x, cu->y, cu->w, cu->h, r, t->separator);
    uui_fill_round_rect(s, cu->x + 1, cu->y + 1, cu->w - 2, cu->h - 2, r - 1, t->field_bg);
    int y = cu->y + pad, x = cu->x + 2 * pad, w = cu->w - 4 * pad;
    if (!g_su_ok) {
        ugfx_draw_string_clipped(s, x, y, w, "No boot menu on this boot -- /boot has no grub.cfg",
                                 t->text, t->field_bg);
        return;
    }
    for (int i = 0; i < g_su_cfg.count; i++) {
        const struct ubootcfg_entry *e = &g_su_cfg.entry[i];
        char head[UBOOTMENU_TITLE + 24];
        snprintf(head, sizeof head, "%s%s", e->title, i == g_su_cfg.def ? "  (default)" : "");
        const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
        ugfx_draw_string_clipped(s, x, y, w, head, t->text, t->field_bg);
        ugfx_set_font(ugfx_font_mono(UGFX_FONT_REGULAR));
        char line[UBOOTCFG_PATH + 256];
        int n = snprintf(line, sizeof line, "%s", e->kernel);
        for (int k = 0; k < e->nwords && n < (int)sizeof line; k++)
            n += snprintf(line + n, sizeof line - (size_t)n, " %s", e->word[k]);
        ugfx_draw_string_elided(s, x, y + lh, w, line, t->outline, t->field_bg);
        ugfx_set_font(was);
        y += 2 * lh + pad;
    }
}

int startup_emit(struct uui_item *out, int n, struct uui_focusable *focus, int *nfocus) {
    static const int ids[3] = { ID_SU_DEFAULT, ID_SU_TIMEOUT, ID_SU_NEXT };
    void *w[3] = { &g_su_default, &g_su_timeout, &g_su_next };
    const struct uui_widget_ops *ops[3] = { &uui_dropdown_ops, &uui_spinbox_ops, &uui_dropdown_ops };
    static const char *const names[3] = { "su_default", "su_timeout", "su_next" };
    for (int i = 0; i < 3; i++) {
        g_su_row[i].control = (struct uui_item){ .ops = ops[i], .widget = w[i], .id = ids[i], .name = names[i] };
        out[n++] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_su_row[i], .flags = UUI_FILL_W };
        focus[(*nfocus)++] = (struct uui_focusable){ w[i], ops[i] };
    }
    out[n++] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &g_su_list, .flags = UUI_FILL_W,
                                  .name = "su_list" };
    out[n++] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_su_open, .id = ID_SU_OPEN,
                                  .name = "su_open" };
    focus[(*nfocus)++] = (struct uui_focusable){ &g_su_open, &uui_button_ops };
    return n;
}

int startup_fit(void) {
    int changed = 0;
    for (int i = 0; i < 3; i++) if (uui_setting_row_fit(&g_su_row[i])) changed = 1;
    return changed;
}

void startup_init(void) {
    uui_dropdown_init(&g_su_default, 0, 0, 0, 0, g_su_def_items, 0);
    uui_dropdown_init(&g_su_next, 0, 0, 0, 0, g_su_next_items, 0);
    uui_spinbox_init(&g_su_timeout, 5, 0, 3600, 1, "s");
    uui_setting_row_init(&g_su_row[0], "Default boot entry",
                         "What starts when nobody picks from the menu.", (struct uui_item){ 0 });
    uui_setting_row_init(&g_su_row[1], "Show the boot menu for",
                         "At 0 the menu is hidden, and System Update cannot install a kernel.",
                         (struct uui_item){ 0 });
    uui_setting_row_init(&g_su_row[2], "Next restart",
                         "Boot another entry once; the restart after uses the default.",
                         (struct uui_item){ 0 });
    g_su_list = (struct uui_custom){ .draw = draw_list };
    uui_button_init(&g_su_open, 0, 0, 0, 0, "Open Boot Manager", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_SU_OPEN);
}
