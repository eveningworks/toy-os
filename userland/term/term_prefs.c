// The GUI Terminal's Options window: the controls, and turning them back
// into a struct term_conf. The file it edits is term_conf.c.
//
// A SIDEBAR OF PAGES over Defaults / OK / Cancel -- File Manager
// Options' shape (userland/fm/fm_options.c), which is Dolphin's
// Configure dialog's, so the two option windows read as one design. A
// modal window of its own: the terminal behind it stays visible, which
// is what a person picking a palette is looking at.
#include "term.h"

#include <stdio.h>
#include <string.h>

#include "keyboard.h"
#include "lib/usetting.h"
#include "ui/uapp.h"
#include "ui/ugfx.h"
#include "ui/ulog.h"
#include "ui/utheme.h"
#include "ui/uui_button.h"
#include "ui/uui_checkbox.h"
#include "ui/uui_dropdown.h"
#include "ui/uui_focus.h"
#include "ui/uui_gallery.h"
#include "ui/uui_label.h"
#include "ui/uui_layout.h"
#include "ui/uui_route.h"
#include "ui/uui_segmented.h"
#include "ui/uui_sidebar.h"
#include "ui/uui_slider.h"
#include "ui/uui_spinbox.h"
#include "ui/ucrt.h"

enum { PAGE_LOOK, PAGE_EFFECT, PAGE_SHELL, PAGE_SCROLL, PAGE_WINDOW, PAGE_COUNT };
enum {
    OPT_PAGES = 1, OPT_SCHEME, OPT_SYSFONT, OPT_FONT, OPT_CURSOR, OPT_BLINK,
    OPT_SHELL, OPT_NEWTAB, OPT_ONEXIT, OPT_CONFIRM,
    OPT_SB, OPT_SCROLLBAR, OPT_SCROLLOUT, OPT_COPYSEL,
    OPT_COLS, OPT_ROWS, OPT_MENUBAR, OPT_PANEL,
    OPT_LOOKS, OPT_SCAN, OPT_GLOW, OPT_VIG, OPT_CURVE, OPT_MASK, OPT_FLICKER, OPT_NOISE,
    OPT_DEFAULTS, OPT_OK, OPT_CANCEL,
};

static const struct uui_sidebar_row PAGE_ROWS[] = {
    { "Appearance",          UUI_SIDEBAR_TOP, 0, PAGE_LOOK },
    { "Screen effect",       UUI_SIDEBAR_TOP, 0, PAGE_EFFECT },
    { "Shell and tabs",      UUI_SIDEBAR_TOP, 0, PAGE_SHELL },
    { "Scrolling and mouse", UUI_SIDEBAR_TOP, 0, PAGE_SCROLL },
    { "Window",              UUI_SIDEBAR_TOP, 0, PAGE_WINDOW },
};
static const char *const CURSOR_OPTS[] = { "Block", "Underline", "Bar" };
// Shown Home first, as read; enum term_newtab_dir is not that order.
static const char *const NEWTAB_OPTS[] = { "Home", "This tab's folder" };
static const char *const ONEXIT_OPTS[] = { "Close the tab", "Keep it open" };
static const char *const LEVEL_OPTS[] = { "Off", "Low", "Medium", "High" };
static const char *const CURVE_OPTS[] = { "Off", "Subtle", "Strong" };
static const char *const MASK_OPTS[] = { "Off", "Aperture grille", "Slot mask" };
// Card 0 is the effect off; card 1 + i is ucrt_presets[i].
static const char *const LOOK_OPTS[] = { "Off", "Subtle", "Classic CRT", "Curved" };
#define LOOK_CARDS ((int)(sizeof LOOK_OPTS / sizeof LOOK_OPTS[0]))

// One row per setting: its page, a caption and one or two controls (a
// size beside its "use the desktop's" box, a cursor beside its blink).
#define ROWS 21
static struct {
    int page;
    struct uui_label caption;
    struct uui_item items[3];
    struct uui_layout row;
} g_row[ROWS];

static struct uapp_window *g_win;
static struct term_conf g_edit;          // what the window shows, until OK
static void (*g_on_commit)(const struct term_conf *next);

static struct uui_sidebar g_pages;
static struct uui_label g_heading;
static char g_heading_text[32];
static struct uui_gallery g_schemes;
static struct uui_checkbox g_sysfont, g_blink, g_confirm, g_scrollbar, g_scrollout, g_copysel,
                           g_menubar, g_panel, g_flicker, g_noise;
static struct uui_gallery g_looks;
static struct uui_slider g_scan, g_glow, g_vig;
static struct uui_segmented g_curve, g_mask;
static struct ucrt g_card_crt[LOOK_CARDS];
static struct uui_spinbox g_font, g_sb, g_cols, g_rows;
static struct uui_segmented g_cursor, g_newtab, g_onexit;
static struct uui_dropdown g_shell;
static struct uui_button g_defaults, g_ok, g_cancel;
static struct uui_label g_spacer;

static struct uui_item g_page_items[1 + ROWS], g_body_items[2], g_button_items[4], g_root_items[2];
static struct uui_layout g_page_l, g_body_l, g_button_l, g_root_l;
static struct uui_focusable g_focusables[3 + 2 * ROWS];
static int g_nfocus;
static struct uui_focus g_focus;

// The schemes, loaded whole so each card can paint its own palette.
static char g_names[TERM_SCHEME_MAX][TERM_NAME_MAX];
static char g_labels[TERM_SCHEME_MAX][TERM_LABEL_MAX];
static const char *g_label_ptr[TERM_SCHEME_MAX];
static struct term_scheme g_sch[TERM_SCHEME_MAX];
static int g_scheme_count;

// The shell list: "System default (tosh)", then /etc/shells.
static char g_shells[TERM_SHELLS_MAX][TERM_SHELL_MAX];
static int g_nshells;
static char g_sysdefault[48];
static const char *g_shell_ptr[TERM_SHELLS_MAX + 1];

// A card: a few lines of a real session in that scheme's colours --
// `ls` with its directories in bright cyan, as ls draws them, and the
// caret. VGA indices, because a scheme's `pal` is in VGA order.
static void draw_scheme(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    (void)ctx;
    if (i < 0 || i >= g_scheme_count) return;
    const struct term_scheme *sc = &g_sch[i];
    uint32_t bg = sc->pal[sc->bg & 15], fg = sc->pal[sc->fg & 15], dir = sc->pal[11];
    uui_fill_round_rect(s, x, y, w, h, ugfx_char_h() / 4, bg);
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_mono(UGFX_FONT_REGULAR));
    int lh = ugfx_char_h(), cw = ugfx_char_w(), px = x + cw / 2, py = y + lh / 3;
    int mw = w - cw;
    ugfx_draw_string_clipped(s, px, py, mw, "/$ ls", fg, bg);
    ugfx_draw_string_clipped(s, px, py + lh, mw, "bin/ etc/ usr/", dir, bg);
    ugfx_draw_string_clipped(s, px, py + 2 * lh, mw, "/$", fg, bg);
    if (py + 3 * lh <= y + h) ugfx_fill_rect(s, px + 3 * cw, py + 2 * lh, cw, lh, sc->cursor);
    ugfx_set_font(was);
}

// A Screen effect card: the same few lines in the scheme chosen on the
// Appearance page, then the look's own passes over it -- the preview is
// the effect, not a picture of it.
static void draw_look(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    (void)ctx;
    if (i < 0 || i >= LOOK_CARDS) return;
    struct term_scheme fallback;
    const struct term_scheme *sc = &fallback;
    if (g_schemes.selected >= 0 && g_schemes.selected < g_scheme_count) sc = &g_sch[g_schemes.selected];
    else term_scheme_builtin(&fallback);
    uint32_t bg = sc->pal[sc->bg & 15], fg = sc->pal[sc->fg & 15], dir = sc->pal[11];
    struct ucrt_look look = { 0 };
    if (i > 0) look = ucrt_presets[i - 1];
    ugfx_fill_rect(s, x, y, w, h, bg);
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_mono(UGFX_FONT_REGULAR));
    // CENTRED, not at the corner: a vignette darkens the edges, and the
    // preview should show the look, not only its darkest part.
    int lh = ugfx_char_h(), cw = ugfx_char_w(), m = ucrt_margin(&look, w, h);
    int px = x + (w - 14 * cw) / 2, py = y + (h - 3 * lh) / 2;
    if (px < x + m) px = x + m;
    if (py < y + m) py = y + m;
    int mw = x + w - px;
    ugfx_draw_string_clipped(s, px, py, mw, "/$ ls", fg, bg);
    ugfx_draw_string_clipped(s, px, py + lh, mw, "bin/ etc/ usr/", dir, bg);
    ugfx_draw_string_clipped(s, px, py + 2 * lh, mw, "/$", fg, bg);
    if (py + 3 * lh <= y + h) ugfx_fill_rect(s, px + 3 * cw, py + 2 * lh, cw, lh, sc->cursor);
    ugfx_set_font(was);
    struct ucrt *c = &g_card_crt[i];
    c->look = look;
    c->period = (lh + 3) / 6;
    c->bezel = ugfx_blend(ugfx_rgb(22, 22, 22), bg, 40);
    ucrt_apply(c, s, x, y, w, h);
}

// The Screen effect controls from a look, and the card that names it.
static void look_to_controls(void) {
    const struct ucrt_look *l = &g_edit.crt;
    g_scan.selected = l->scanlines;
    g_glow.selected = l->glow;
    g_vig.selected = l->vignette;
    g_curve.selected = l->curve;
    g_mask.selected = l->mask;
    g_flicker.checked = l->flicker;
    g_noise.checked = l->noise;
    int p = ucrt_preset_of(l);
    g_looks.selected = !g_edit.effect ? 0 : p >= 0 ? 1 + p : -1;   // -1: a look of its own
}

static void look_from_controls(void) {
    struct ucrt_look *l = &g_edit.crt;
    l->scanlines = g_scan.selected > 0 ? g_scan.selected : 0;
    l->glow = g_glow.selected > 0 ? g_glow.selected : 0;
    l->vignette = g_vig.selected > 0 ? g_vig.selected : 0;
    l->curve = g_curve.selected > 0 ? g_curve.selected : 0;
    l->mask = g_mask.selected > 0 ? g_mask.selected : 0;
    l->flicker = g_flicker.checked;
    l->noise = g_noise.checked;
}

static void show_page(int page) {
    strlcpy(g_heading_text, PAGE_ROWS[page].label, sizeof g_heading_text);
    for (int i = 0; i < ROWS; i++) g_page_items[1 + i].hidden = g_row[i].page != page;
}

static int scheme_index(const char *name) {
    for (int i = 0; i < g_scheme_count; i++)
        if (strcmp(g_names[i], name) == 0) return i;
    return -1;
}

static int shell_index(const char *path) {
    if (!path[0]) return 0;
    for (int i = 0; i < g_nshells; i++)
        if (strcmp(g_shells[i], path) == 0) return 1 + i;
    return 0;
}

// The controls from `g_edit`, and back.
static void to_controls(void) {
    g_schemes.selected = scheme_index(g_edit.scheme);
    int sys_px = 14;
    usetting_get_int("system.font_size", &sys_px);
    g_sysfont.checked = g_edit.font_size == 0;
    uui_spinbox_set_value(&g_font, g_edit.font_size ? g_edit.font_size : sys_px);
    g_font.disabled = g_sysfont.checked;
    g_cursor.selected = g_edit.cursor;
    g_blink.checked = g_edit.cursor_blink;
    uui_dropdown_set_selected(&g_shell, shell_index(g_edit.shell));
    g_newtab.selected = g_edit.newtab_dir == TERM_NEWTAB_HOME ? 0 : 1;
    g_onexit.selected = g_edit.keep_on_exit;
    g_confirm.checked = g_edit.confirm_close;
    uui_spinbox_set_value(&g_sb, g_edit.scrollback);
    g_scrollbar.checked = g_edit.scrollbar;
    g_scrollout.checked = g_edit.scroll_on_output;
    g_copysel.checked = g_edit.copy_on_select;
    uui_spinbox_set_value(&g_cols, g_edit.cols);
    uui_spinbox_set_value(&g_rows, g_edit.rows);
    g_menubar.checked = g_edit.menubar;
    g_panel.checked = g_edit.panel;
    look_to_controls();
}

static void from_controls(void) {
    if (g_schemes.selected >= 0 && g_schemes.selected < g_scheme_count)
        strlcpy(g_edit.scheme, g_names[g_schemes.selected], sizeof g_edit.scheme);
    uui_spinbox_commit(&g_font);
    uui_spinbox_commit(&g_sb);
    uui_spinbox_commit(&g_cols);
    uui_spinbox_commit(&g_rows);
    g_edit.font_size = g_sysfont.checked ? 0 : uui_spinbox_value(&g_font);
    g_edit.cursor = g_cursor.selected >= 0 ? g_cursor.selected : TERM_CURSOR_BLOCK;
    g_edit.cursor_blink = g_blink.checked;
    int sh = uui_dropdown_selected(&g_shell);
    if (sh >= 1 && sh <= g_nshells) strlcpy(g_edit.shell, g_shells[sh - 1], sizeof g_edit.shell);
    else g_edit.shell[0] = '\0';
    g_edit.newtab_dir = g_newtab.selected == 0 ? TERM_NEWTAB_HOME : TERM_NEWTAB_HERE;
    g_edit.keep_on_exit = g_onexit.selected == 1;
    g_edit.confirm_close = g_confirm.checked;
    g_edit.scrollback = uui_spinbox_value(&g_sb);
    g_edit.scrollbar = g_scrollbar.checked;
    g_edit.scroll_on_output = g_scrollout.checked;
    g_edit.copy_on_select = g_copysel.checked;
    g_edit.cols = uui_spinbox_value(&g_cols);
    g_edit.rows = uui_spinbox_value(&g_rows);
    g_edit.menubar = g_menubar.checked;
    g_edit.panel = g_panel.checked;
    look_from_controls();
}

static void close_window(void) {
    if (g_win) uapp_window_close(g_win);
    g_win = 0;
    for (int i = 0; i < LOOK_CARDS; i++) ucrt_free(&g_card_crt[i]);
}

int term_prefs_is_open(void) { return g_win != 0; }

static void on_widget(struct uapp_window *w, int id, int reason) {
    (void)reason;
    if (id == OPT_PAGES) {
        int page = uui_sidebar_selected_id(&g_pages);
        if (page >= 0 && page < PAGE_COUNT) show_page(page);
    } else if (id == OPT_SYSFONT) {
        g_font.disabled = g_sysfont.checked;
    } else if (id == OPT_LOOKS) {
        // A card is a whole look: Off turns the effect off and keeps the
        // look for next time, a preset is copied into the controls.
        int c = g_looks.selected;
        if (c == 0) g_edit.effect = 0;
        else if (c > 0 && c < LOOK_CARDS) { g_edit.effect = 1; g_edit.crt = ucrt_presets[c - 1]; }
        look_to_controls();
    } else if (id >= OPT_SCAN && id <= OPT_NOISE) {
        // Moving any control is choosing a look of one's own, on.
        look_from_controls();
        g_edit.effect = ucrt_look_on(&g_edit.crt);
        look_to_controls();
    }
    uapp_window_redraw(w);
}

// Defaults, OK, Cancel.
static void on_action(struct uapp_window *w, int code) {
    if (code == OPT_DEFAULTS) {
        term_conf_defaults(&g_edit);
        to_controls();
    } else if (code == OPT_OK) {
        from_controls();
        void (*commit)(const struct term_conf *) = g_on_commit;
        struct term_conf next = g_edit;
        close_window();
        if (commit) commit(&next);
        return;
    } else if (code == OPT_CANCEL) {
        close_window();
        return;
    }
    uapp_window_redraw(w);
}

static void on_key(struct uapp_window *w, int key, unsigned mods) {
    (void)mods;
    // Esc cancels and Return keeps, as in every dialog.
    if (key == 0x1B) { close_window(); return; }
    if (key == '\n' || key == '\r') { on_action(w, OPT_OK); return; }
    uapp_window_redraw(w);
}

static void on_close(struct uapp_window *w) {
    (void)w;
    close_window();
}

static void add_row(int i, int page, const char *caption, const struct uui_widget_ops *ops,
                    void *widget, int id, const char *name, unsigned flags) {
    int n = 2;
    int cw = ugfx_char_advance('n') * 18;
    g_row[i].page = page;
    uui_label_init(&g_row[i].caption, caption);
    g_row[i].items[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_row[i].caption,
                                           .flags = UUI_FILL_H, .main_size = cw };
    g_row[i].items[1] = (struct uui_item){ .ops = ops, .widget = widget, .id = id, .name = name,
                                           .flags = flags };
    g_row[i].row = (struct uui_layout){ .dir = UUI_ROW, .items = g_row[i].items, .count = n,
                                        .margin = 1, .gap = 16 };
    // THE CARDS GET THE PAGE'S WIDTH, under their caption: beside it they
    // fit two to a row and the page outgrows the window.
    if (ops == &uui_gallery_ops) {
        g_row[i].row.dir = UUI_COLUMN;
        g_row[i].items[0].flags = 0;
        g_row[i].items[0].main_size = 0;
    }
    g_page_items[1 + i] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_row[i].row,
                                             .flags = UUI_FILL_W };
    g_focusables[g_nfocus++] = (struct uui_focusable){ widget, ops };
}

// A second control on row `i`, after the first.
static void add_second(int i, const struct uui_widget_ops *ops, void *widget, int id,
                       const char *name) {
    g_row[i].items[2] = (struct uui_item){ .ops = ops, .widget = widget, .id = id, .name = name };
    g_row[i].row.count = 3;
    g_focusables[g_nfocus++] = (struct uui_focusable){ widget, ops };
}

void term_prefs_open(struct uapp *a, const struct term_conf *c,
                     void (*on_commit)(const struct term_conf *next)) {
    if (g_win) return;   // one at a time
    g_edit = *c;
    g_on_commit = on_commit;

    g_scheme_count = term_scheme_list(g_names, g_labels, TERM_SCHEME_MAX);
    for (int i = 0; i < g_scheme_count; i++) {
        g_label_ptr[i] = g_labels[i];
        term_scheme_load(g_names[i], &g_sch[i]);
    }
    g_nshells = term_shells_list(g_shells, TERM_SHELLS_MAX);
    char sys[64];
    if (!usetting_get("system.shell", sys, sizeof sys) || !sys[0]) strlcpy(sys, "/bin/tosh", sizeof sys);
    const char *base = strrchr(sys, '/');
    snprintf(g_sysdefault, sizeof g_sysdefault, "System default (%s)", base ? base + 1 : sys);
    g_shell_ptr[0] = g_sysdefault;
    for (int i = 0; i < g_nshells; i++) g_shell_ptr[1 + i] = g_shells[i];

    uui_sidebar_init(&g_pages, 0, 0, 0, 0, PAGE_ROWS, PAGE_COUNT);
    g_pages.sel_bg = UTHEME_ACCENT;   // the page you are on, as File Manager Options does
    g_pages.sel_fg = UTHEME_ACCENT_TEXT;
    uui_label_init(&g_heading, g_heading_text);
    g_heading.font = ugfx_font_session(UGFX_FONT_BOLD);

    uui_gallery_init(&g_schemes, g_label_ptr, g_scheme_count, 0);
    g_schemes.draw_tile = draw_scheme;
    uint32_t bg = UTHEME_WINDOW_BG, fg = UTHEME_TEXT;
    uui_checkbox_init(&g_sysfont,   0, 0, 0, "Use the desktop's size", bg, fg);
    uui_checkbox_init(&g_blink,     0, 0, 0, "Blink", bg, fg);
    uui_checkbox_init(&g_confirm,   0, 0, 0, "Ask before closing several tabs", bg, fg);
    uui_checkbox_init(&g_scrollbar, 0, 0, 0, "Show the scrollbar", bg, fg);
    uui_checkbox_init(&g_scrollout, 0, 0, 0, "Scroll to the bottom on output", bg, fg);
    uui_checkbox_init(&g_copysel,   0, 0, 0, "Copy on select", bg, fg);
    uui_checkbox_init(&g_menubar,   0, 0, 0, "Show the menu bar", bg, fg);
    uui_checkbox_init(&g_panel,     0, 0, 0, "Show the Session panel", bg, fg);
    uui_checkbox_init(&g_flicker,   0, 0, 0, "Flicker", bg, fg);
    uui_checkbox_init(&g_noise,     0, 0, 0, "Static noise", bg, fg);
    uui_gallery_init(&g_looks, LOOK_OPTS, LOOK_CARDS, 0);
    g_looks.draw_tile = draw_look;
    for (int i = 0; i < LOOK_CARDS; i++) ucrt_init(&g_card_crt[i]);
    uui_slider_init(&g_scan, LEVEL_OPTS, UCRT_LEVEL_MAX + 1);
    uui_slider_init(&g_glow, LEVEL_OPTS, UCRT_LEVEL_MAX + 1);
    uui_slider_init(&g_vig, LEVEL_OPTS, UCRT_LEVEL_MAX + 1);
    uui_segmented_init(&g_curve, CURVE_OPTS, UCRT_CURVE_COUNT, 0);
    uui_segmented_init(&g_mask, MASK_OPTS, UCRT_MASK_COUNT, 0);
    uui_spinbox_init(&g_font, 14, TERM_FONT_MIN, TERM_FONT_MAX, 1, "px");
    uui_spinbox_init(&g_sb, 240, TERM_SB_MIN, TERM_SB_MAX, 100, "lines");
    uui_spinbox_init(&g_cols, 120, TERM_COLS_MIN, TERM_COLS_MAX, 1, "columns");
    uui_spinbox_init(&g_rows, 30, TERM_ROWS_MIN, TERM_ROWS_MAX, 1, "rows");
    uui_segmented_init(&g_cursor, CURSOR_OPTS, 3, 0);
    uui_segmented_init(&g_newtab, NEWTAB_OPTS, 2, 1);
    uui_segmented_init(&g_onexit, ONEXIT_OPTS, 2, 0);
    uui_dropdown_init(&g_shell, 0, 0, 0, 0, g_shell_ptr, 1 + g_nshells);
    uui_button_init(&g_defaults, 0, 0, 0, 0, "Defaults", UTHEME_BUTTON_BG, UTHEME_TEXT, OPT_DEFAULTS);
    uui_button_init(&g_ok, 0, 0, 0, 0, "OK", UTHEME_ACCENT, UTHEME_ACCENT_TEXT, OPT_OK);
    uui_button_init(&g_cancel, 0, 0, 0, 0, "Cancel", UTHEME_BUTTON_BG, UTHEME_TEXT, OPT_CANCEL);
    uui_label_init(&g_spacer, "");
    to_controls();

    g_focusables[0] = (struct uui_focusable){ &g_pages, &uui_sidebar_ops };
    g_nfocus = 3;
    g_page_items[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_heading,
                                         .flags = UUI_FILL_W, .name = "heading" };
    int r = 0;
    add_row(r++, PAGE_LOOK, "Colour scheme:", &uui_gallery_ops, &g_schemes, OPT_SCHEME, "scheme", UUI_FILL_W);
    add_row(r, PAGE_LOOK, "Font size:", &uui_spinbox_ops, &g_font, OPT_FONT, "font", 0);
    add_second(r++, &uui_checkbox_ops, &g_sysfont, OPT_SYSFONT, "sysfont");
    add_row(r, PAGE_LOOK, "Cursor:", &uui_segmented_ops, &g_cursor, OPT_CURSOR, "cursor", 0);
    add_second(r++, &uui_checkbox_ops, &g_blink, OPT_BLINK, "blink");
    add_row(r++, PAGE_EFFECT, "Look:", &uui_gallery_ops, &g_looks, OPT_LOOKS, "looks", UUI_FILL_W);
    // The sliders get a track a hand can travel: their natural width is
    // the labels', which makes four stops a flick of the wrist.
    for (int k = 0; k < 3; k++) {
        static const char *const cap[] = { "Scanlines:", "Glow:", "Vignette:" };
        static const char *const nm[] = { "scanlines", "glow", "vignette" };
        struct uui_slider *sl[] = { &g_scan, &g_glow, &g_vig };
        add_row(r, PAGE_EFFECT, cap[k], &uui_slider_ops, sl[k], OPT_SCAN + k, nm[k], 0);
        g_row[r++].items[1].main_size = ugfx_char_advance('n') * 34;
    }
    add_row(r++, PAGE_EFFECT, "Curvature:", &uui_segmented_ops, &g_curve, OPT_CURVE, "curve", 0);
    add_row(r++, PAGE_EFFECT, "Phosphor mask:", &uui_segmented_ops, &g_mask, OPT_MASK, "mask", 0);
    add_row(r, PAGE_EFFECT, "Animation:", &uui_checkbox_ops, &g_flicker, OPT_FLICKER, "flicker", 0);
    add_second(r++, &uui_checkbox_ops, &g_noise, OPT_NOISE, "noise");
    add_row(r++, PAGE_SHELL, "Shell for new tabs:", &uui_dropdown_ops, &g_shell, OPT_SHELL, "shell", 0);
    add_row(r++, PAGE_SHELL, "New tabs open in:", &uui_segmented_ops, &g_newtab, OPT_NEWTAB, "newtab", 0);
    add_row(r++, PAGE_SHELL, "When a shell exits:", &uui_segmented_ops, &g_onexit, OPT_ONEXIT, "onexit", 0);
    add_row(r++, PAGE_SHELL, "Closing:", &uui_checkbox_ops, &g_confirm, OPT_CONFIRM, "confirm", 0);
    add_row(r++, PAGE_SCROLL, "Scrollback:", &uui_spinbox_ops, &g_sb, OPT_SB, "scrollback", 0);
    add_row(r++, PAGE_SCROLL, "Scrollbar:", &uui_checkbox_ops, &g_scrollbar, OPT_SCROLLBAR, "scrollbar", 0);
    add_row(r++, PAGE_SCROLL, "Output:", &uui_checkbox_ops, &g_scrollout, OPT_SCROLLOUT, "scrollout", 0);
    add_row(r++, PAGE_SCROLL, "Selection:", &uui_checkbox_ops, &g_copysel, OPT_COPYSEL, "copysel", 0);
    add_row(r, PAGE_WINDOW, "Size on open:", &uui_spinbox_ops, &g_cols, OPT_COLS, "cols", 0);
    add_second(r++, &uui_spinbox_ops, &g_rows, OPT_ROWS, "rows");
    add_row(r++, PAGE_WINDOW, "Menu bar:", &uui_checkbox_ops, &g_menubar, OPT_MENUBAR, "menubar", 0);
    add_row(r++, PAGE_WINDOW, "Session panel:", &uui_checkbox_ops, &g_panel, OPT_PANEL, "panel", 0);
    g_focusables[1] = (struct uui_focusable){ &g_ok, &uui_button_ops };
    g_focusables[2] = (struct uui_focusable){ &g_cancel, &uui_button_ops };
    show_page(PAGE_LOOK);

    g_page_l = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_page_items, .count = 1 + ROWS,
                                    .margin = 12, .gap = 10 };
    g_body_items[0] = (struct uui_item){ .ops = &uui_sidebar_ops, .widget = &g_pages, .id = OPT_PAGES,
                                         .flags = UUI_FILL_H, .name = "pages",
                                         .main_size = ugfx_char_advance('n') * 22 };
    g_body_items[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_page_l,
                                         .flags = UUI_FILL_W | UUI_FILL_H };
    g_body_l = (struct uui_layout){ .dir = UUI_ROW, .items = g_body_items, .count = 2, .margin = 1 };
    g_button_items[0] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_defaults,
                                           .id = OPT_DEFAULTS, .name = "defaults" };
    g_button_items[1] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_spacer,
                                           .flags = UUI_FILL_W };
    g_button_items[2] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_ok,
                                           .id = OPT_OK, .name = "ok" };
    g_button_items[3] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_cancel,
                                           .id = OPT_CANCEL, .name = "cancel" };
    g_button_l = (struct uui_layout){ .dir = UUI_ROW, .items = g_button_items, .count = 4,
                                      .margin = 10 };
    g_root_items[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_body_l,
                                         .flags = UUI_FILL_W | UUI_FILL_H };
    g_root_items[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_button_l,
                                         .flags = UUI_FILL_W };
    g_root_l = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_root_items, .count = 2,
                                    .margin = 1, .gap = 1 };
    uui_focus_init(&g_focus, g_focusables, g_nfocus);
    uui_focus_set(&g_focus, 0);

    struct uapp_window_desc d = {
        .title = "Terminal Options",
        .w = ugfx_char_advance('n') * 118,   // four gallery cards to a row (uui_gallery's min_card)
        .h = ugfx_char_h() * 36,
        .flags = UAPP_WIN_MODAL,
        .widgets = g_root_items,
        .widget_count = 2,
        .layout = &g_root_l,
        .focus = &g_focus,
        .on_widget = on_widget,
        .on_action = on_action,
        .on_key = on_key,
        .on_close = on_close,
        .log_prefix = "options",
    };
    g_win = uapp_window_open(a, &d);
    ulogf("uterm: options open %d scheme(s) %d shell(s)\n", g_scheme_count, g_nshells);
}
