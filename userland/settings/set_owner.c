// System Settings: a screensaver's or effect's own options, and the
// dialog an effect's are edited in.
#include "settings/settings_internal.h"
#include "lib/ulivewall.h"

const char OWNER_SAVER[]  = "desktop.screensaver";
const char OWNER_EFFECT[] = "desktop.minimize_effect";
const char OWNER_LIVE[]   = "desktop.wallpaper_live";

// THE SELECTED SCREENSAVER'S OPTIONS, as rows past the registry's.
//
// A saver's options are declared by a data file beside the program and
// belong to a process that is not running (lib/usaver.h), so there is
// nothing for the settings registry to hold: what a saver offers
// depends on which saver is selected, and that changes while this page
// is open. Synthesising rows keeps load_slot(), relayout_page(), the
// focus ring and the staging logic on one path -- only the CHOICE LIST
// and the WRITE know an option from a setting.
// THE TWO SETTINGS THAT OWN OPTIONS BELOW THEM. A screensaver declares
// what it lets you change, and since the shatter minimize effect does
// too (userland/lib/ueffect.h) this page carries either kind -- the
// same descriptor format, a different pair of directories.
//
// Keyed on the SETTING rather than the page's name, because a page
// renamed in /etc/settings.d would otherwise silently lose its options.

struct usaver g_saver;
int g_saver_base = -1;  // first synthesised index, or -1 for none
int g_saver_slot = -1;  // first option's slot, or -1

// The saver option behind a synthesised row, or NULL for a real
// setting. Every place that has to tell them apart asks this, so the
// "an index past the registry is an option" rule is written once.
const struct usaver_opt *opt_of(int idx) {
    if (g_saver_base < 0 || idx < g_saver_base) return 0;
    int i = idx - g_saver_base;
    return i < g_saver.opt_count ? &g_saver.opt[i] : 0;
}

// Replaces the page's saver-option rows with `saver`'s, leaving every
// registry slot before them -- and whatever is staged on those --
// alone.
//
// CALLED AGAIN WHENEVER THE SAVER DROPDOWN MOVES, which is why it
// truncates rather than rebuilding the page: re-opening the group would
// discard a staged timeout, so picking a saver would silently undo the
// change above it.
void rebuild_owner_options(const char *kind, const char *saver) {
    if (g_saver_slot < 0) return;
    g_slot_count = g_saver_slot;
    g_saver_base = -1;
    // NOT CLEARED HERE. A caller with no name is a dropdown whose
    // staged value is momentarily empty, not a page without an owner --
    // and clearing on it lost starfield's options when the saver
    // dropdown was moved away and back. The page-has-no-owner case is
    // open_group's, where the owner is known to be absent.
    if (!kind || !saver || !saver[0]) return;

    if (kind == OWNER_EFFECT)    ueffect_load(saver, &g_saver);
    else if (kind == OWNER_LIVE) ulivewall_options(saver, &g_saver);
    else                         usaver_load(saver, &g_saver);
    if (!g_saver.opt_count) return;
    g_saver_base = g_setting_count;

    for (int i = 0; i < g_saver.opt_count && g_slot_count < PAGE_MAX; i++) {
        const struct usaver_opt *o = &g_saver.opt[i];
        int k = g_saver_base + i;

        // A SYNTHESISED ROW IS A REAL ROW. Everything downstream reads
        // these arrays and nothing asks where a row came from, so a
        // field left stale from the last saver is a control bounded by
        // another saver's range.
        strlcpy(g_label[k], o->label, sizeof g_label[k]);
        strlcpy(g_desc[k],  o->desc,  sizeof g_desc[k]);
        strlcpy(g_value[k], o->value, sizeof g_value[k]);
        strlcpy(g_unit[k],  o->unit,  sizeof g_unit[k]);
        // NAMED BY THE FILE IT IS WRITTEN TO, the way a qualified
        // setting name is -- so the status line and the log say
        // `starfield.stars` rather than a bare `stars` that two savers
        // could both claim.
        snprintf(g_name[k], sizeof g_name[k], "%s.%s", saver, o->key);
        g_ns[k][0] = '\0';
        if (kind == OWNER_EFFECT)    ueffect_conf_path(saver, g_file[k], sizeof g_file[k]);
        else if (kind == OWNER_LIVE) ulivewall_conf_path(saver, g_file[k], sizeof g_file[k]);
        else                         usaver_conf_path(saver, g_file[k], sizeof g_file[k]);
        g_cat_of[k][0] = '\0';
        g_group_of[k][0] = '\0';
        g_unavail[k][0] = '\0';
        g_sflags[k] = 0;
        g_order[k] = 0;
        g_type[k] = o->type == USAVER_INT ? SETTING_ABI_TYPE_INT
                                          : SETTING_ABI_TYPE_ENUM;
        g_imin[k] = o->imin;
        g_imax[k] = o->imax;
        g_istep[k] = o->istep;
        g_widget[k] = o->widget == USAVER_WIDGET_RADIO ? SETTING_ABI_WIDGET_RADIO
                    : o->widget == USAVER_WIDGET_DROPDOWN ? SETTING_ABI_WIDGET_DROPDOWN
                    : o->widget == USAVER_WIDGET_SLIDER ? SETTING_ABI_WIDGET_SLIDER
                    : SETTING_ABI_WIDGET_AUTO;

        load_slot(&g_slot[g_slot_count++], k);
    }
    // REPORTED, never silently cut -- the same rule the registry side of
    // this page follows. It can only happen if the Screensaver group
    // gains a third setting, which PAGE_MAX is not sized for.
    if (g_slot_count - g_saver_slot < g_saver.opt_count)
        snprintf(g_status, sizeof g_status,
                 "%s: only %d of %d options fit on this page", saver,
                 g_slot_count - g_saver_slot, g_saver.opt_count);
    ulogf("settings: saver %s options %d shown %d\n", saver,
          g_saver.opt_count, g_slot_count - g_saver_slot);
}

// The saver the page is currently showing -- the STAGED one, not what
// is on disk, so the options follow the dropdown immediately.
// Which of them setting `idx` is, or NULL for anything else.
const char *owner_kind(int idx) {
    if (idx < 0) return 0;
    if (!strcmp(g_name[idx], OWNER_SAVER))  return OWNER_SAVER;
    if (!strcmp(g_name[idx], OWNER_EFFECT)) return OWNER_EFFECT;
    if (!strcmp(g_name[idx], OWNER_LIVE))   return OWNER_LIVE;
    return 0;
}

// The page's owner setting and its STAGED value -- what the options
// below must belong to, which is the value the dropdown shows and not
// the one on disk.
// **ONLY AN EFFECT'S OPTIONS MOVE TO THE DIALOG.** A saver's stay on
// its page, where its own comment says they belong: Test previews the
// saver, and a window over the page would cover what is being
// previewed. An effect has no Test, which is why the button is right
// for it and not for the saver.
int owner_uses_dialog(const char *kind) { return kind == OWNER_EFFECT; }

const char *page_owner(const char **kind_out) {
    for (int i = 0; i < g_slot_count; i++) {
        if (g_slot[i].setting < 0 || opt_of(g_slot[i].setting)) continue;
        const char *k = owner_kind(g_slot[i].setting);
        if (!k) continue;
        if (kind_out) *kind_out = k;
        return staged_value(&g_slot[i]);
    }
    return 0;
}

// --- the effect's own options, in a window of their own ---------------
//
// **A BUTTON AND A DIALOG, NOT ROWS ON THE PAGE.** Windows puts
// Settings... beside the screensaver dropdown and KDE puts a configure
// button beside each effect that has one; both keep "which effect" and
// "how that effect behaves" apart, and the page stops growing with
// every effect that gains an option.
//
// The saver page keeps its options inline for now, and its own comment
// says why that was right THERE: a dialog would cover the thing Test
// is previewing. An effect has no Test, so the argument does not carry.
struct uui_button    g_opts_btn;   // "Settings..."
struct uui_button    g_opts_ok, g_opts_cancel;
struct uapp_window  *g_opts_win;
// Which owner the OPEN page has, so the page, its prose and the button
// agree about where the options are drawn.
const char          *g_page_owner_kind;
struct uui_item      DLG[PAGE_MAX + 4];
int                  DLG_COUNT;
struct uui_focusable DFOCUS[PAGE_MAX + 2]; // + OK and Cancel
int                  DFOCUS_COUNT;
struct uui_focus     g_dlg_focus;
struct uui_layout    DLG_LAYOUT;
struct uui_layout    DLG_ROW;      // the OK/Cancel strip
struct uui_item      DLG_ROW_ITEMS[2];

// The options dialog's own item list: the SAME slots the page would
// have shown inline, emitted into this window's array instead.
int control_changed(int slot_index);

void relayout_dialog(int content_w) {
    int n = 0;
    DFOCUS_COUNT = 0;
    for (int i = g_saver_slot; i >= 0 && i < g_slot_count; i++) {
        // FIT EACH CARD TO THIS WINDOW, not the page's width: the dialog
        // has no on_draw to refit from, so the width it was CREATED with
        // is the only number there is before its layout runs. Emitted
        // first, since the fit measures the card's text and control.
        n = emit_slot(DLG, n, i, DFOCUS, &DFOCUS_COUNT);
        struct slot *sl = &g_slot[i];
        if (sl->setting >= 0) {
            sl->row.w = content_w;
            uui_setting_row_fit(&sl->row);
        }
    }

    DLG_ROW_ITEMS[0] = (struct uui_item){ .ops = &uui_button_ops,
                                          .widget = &g_opts_ok, .id = ID_OPTS_OK };
    DLG_ROW_ITEMS[1] = (struct uui_item){ .ops = &uui_button_ops,
                                          .widget = &g_opts_cancel, .id = ID_OPTS_CANCEL };
    DLG_ROW = (struct uui_layout){ .dir = UUI_ROW, .items = DLG_ROW_ITEMS,
                                   .count = 2, .margin = 0 };
    DLG[n++] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &DLG_ROW,
                                  .id = 0, .flags = UUI_FILL_W };

    DLG_LAYOUT = (struct uui_layout){ .dir = UUI_COLUMN, .items = DLG,
                                      .count = n, .margin = 0 };
    DLG_COUNT = n;
    DFOCUS[DFOCUS_COUNT++] = (struct uui_focusable){ &g_opts_ok, &uui_button_ops };
    DFOCUS[DFOCUS_COUNT++] = (struct uui_focusable){ &g_opts_cancel, &uui_button_ops };
    uui_focus_init(&g_dlg_focus, DFOCUS, DFOCUS_COUNT);
}

// How wide the dialog is -- FONT-DERIVED, so it reflows with the font.
// Its height is the content's, measured in open_options_dialog().
void opts_window_size(int *w, int *h) {
    *w = ugfx_char_w() * 52;
    *h = 0;   // measured from the cards once they are built -- see below
}

// The dialog's options back to what the file holds. Its controls are
// the page's own slots, so a cancelled edit left staged here is one the
// page's Apply would write.
static void revert_options(void) {
    for (int i = g_saver_slot; i >= 0 && i < g_slot_count; i++)
        if (g_slot[i].setting >= 0 && g_slot[i].staged != g_slot[i].baseline)
            load_slot(&g_slot[i], g_slot[i].setting);
}

// Writes one option slot's staged value to its owner's file. On success
// the written value becomes the slot's baseline AND its g_value, so a
// later load_slot() (a revert) reloads what is on disk now.
int commit_option(struct slot *sl) {
    const struct usaver_opt *o = sl->setting >= 0 ? opt_of(sl->setting) : 0;
    if (!o || sl->staged < 0 || sl->staged == sl->baseline) return 0;
    const char *v = staged_value(sl);
    if (!uconf_set(g_file[sl->setting], o->key, v)) return -1;
    strlcpy(g_value[sl->setting], v, sizeof g_value[sl->setting]);
    sl->baseline = sl->staged;
    sl->row.changed = 0;
    return 1;
}

void dlg_on_widget(struct uapp_window *win, int id, int reason) {
    // A release or a key: a dialog opens CENTRED UNDER THE CURSOR, and a
    // control must not take that pointer's arrival for a change.
    if (reason != UUI_REASON_RELEASE && reason != UUI_REASON_KEY) return;
    if (id >= ID_CONTROL_BASE && id < ID_CONTROL_BASE + PAGE_MAX) {
        control_changed(id - ID_CONTROL_BASE);
        uapp_window_redraw(win);
    }
}

// OK and Cancel. A hover can never reach here: commands arrive only as a
// completed click or a key (ui/uapp.h).
void dlg_on_action(struct uapp_window *win, int code) {
    if (code == ID_OPTS_OK) {
        // The staged values, written to the effect's own file. The
        // page's Apply writes registered settings; an option is not
        // one, so this is where it lands -- the same uconf_set() call
        // the inline rows used.
        int failed = 0;
        for (int i = g_saver_slot; i >= 0 && i < g_slot_count; i++)
            if (commit_option(&g_slot[i]) < 0) failed++;
        if (failed)
            snprintf(g_status, sizeof g_status, "%d option(s) could not be saved", failed);
        revert_options();   // a refused write is not left staged either
        uapp_window_close(win);
        if (g_app) uapp_redraw(g_app);  // the footer's change count
        return;
    }
    if (code == ID_OPTS_CANCEL) dlg_on_close(win);
}

void dlg_on_close(struct uapp_window *win) {
    // NOTHING IS WRITTEN ON A CLOSE, which is Cancel's rule: the X and
    // Cancel are the same answer, and a dialog that committed on the X
    // would be the one shape of this nobody expects.
    revert_options();
    uapp_window_close(win);
    if (g_app) uapp_redraw(g_app);
}

void open_options_dialog(struct uapp *a) {
    if (g_opts_win && uapp_window_is_open(g_opts_win)) return;
    if (g_saver_slot < 0 || g_slot_count <= g_saver_slot) return;
    int w, h;
    opts_window_size(&w, &h);
    relayout_dialog(w - ugfx_char_w() * 2);
    // AS TALL AS ITS CONTENT, measured rather than guessed per option, so
    // OK and Cancel sit at the bottom edge whatever the cards came to.
    uui_layout_natural_size(&DLG_LAYOUT, 0, &h);

    // THE TITLE IS A NAME, NOT A TOKEN. `shatter` is the word the
    // descriptor's author chose, and usaver_display() is what turns one
    // into something a title bar should say -- the same call the choice
    // lists use, so "Shatter options" and the `Shatter` radio row agree.
    //
    // STATIC, because uapp_window_open() copies the DESC and not the
    // strings in it: a stack buffer here is a title that outlives its
    // own storage.
    static char title[SETTING_ABI_LABEL_MAX + 16];
    char shown[SETTING_ABI_LABEL_MAX];
    usaver_display(g_saver.name, shown, sizeof shown);
    snprintf(title, sizeof title, "%s options", shown);
    g_opts_win = uapp_window_open(a, &(struct uapp_window_desc){
        .title = title, .w = w, .h = h, .flags = UAPP_WIN_MODAL,
        .widgets = DLG, .widget_count = DLG_COUNT,
        .layout = &DLG_LAYOUT, .focus = &g_dlg_focus,
        .on_widget = dlg_on_widget, .on_action = dlg_on_action, .on_close = dlg_on_close,
        // A window a test cannot ask about is one a test has to guess
        // pixels at (ui/uapp.h).
        .log_prefix = "settings.options",
    });
}
