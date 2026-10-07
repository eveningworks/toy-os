// The GUI Terminal's Options window over the shared one (ui/uui_prefs.h):
// the rows the window binds by itself, and the ones it cannot -- the
// scheme and look galleries, the CRT controls that move each other, the
// font size whose 0 means "the desktop's", the shell list, the grid size.
// The file it edits is term_conf.c. A modal window of its own: the
// terminal behind it stays visible, which is what a person picking a
// palette is looking at.
#include "term.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "keyboard.h"
#include "lib/usetting.h"
#include "ui/uapp.h"
#include "ui/ugfx.h"
#include "ui/ulog.h"
#include "ui/utheme.h"
#include "ui/uui_checkbox.h"
#include "ui/uui_dropdown.h"
#include "ui/uui_gallery.h"
#include "ui/uui_label.h"
#include "ui/uui_layout.h"
#include "ui/uui_prefs.h"
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

static struct term_conf g_edit;          // what the window shows, until OK
static void (*g_on_commit)(const struct term_conf *next);

static struct uui_gallery g_schemes;
static struct uui_checkbox g_sysfont, g_flicker, g_noise;
static struct uui_gallery g_looks;
static struct uui_slider g_scan, g_glow, g_vig;
static struct uui_segmented g_curve, g_mask;
static struct ucrt g_card_crt[LOOK_CARDS];
static struct uui_spinbox g_font, g_cols, g_rows;
static struct uui_dropdown g_shell;

// Bound rows' value maps (uui_prefs.h).
static const int NEWTAB_VALUES[] = { TERM_NEWTAB_HOME, TERM_NEWTAB_HERE };

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

// The rows the window cannot bind, from `g_edit` and back.
static void to_controls(void) {
    g_schemes.selected = scheme_index(g_edit.scheme);
    int sys_px = 14;
    usetting_get_int("system.font_size", &sys_px);
    g_sysfont.checked = g_edit.font_size == 0;
    uui_spinbox_set_value(&g_font, g_edit.font_size ? g_edit.font_size : sys_px);
    g_font.disabled = g_sysfont.checked;
    uui_dropdown_set_selected(&g_shell, shell_index(g_edit.shell));
    uui_spinbox_set_value(&g_cols, g_edit.cols);
    uui_spinbox_set_value(&g_rows, g_edit.rows);
    look_to_controls();
}

static void from_controls(void) {
    if (g_schemes.selected >= 0 && g_schemes.selected < g_scheme_count)
        strlcpy(g_edit.scheme, g_names[g_schemes.selected], sizeof g_edit.scheme);
    uui_spinbox_commit(&g_font);
    uui_spinbox_commit(&g_cols);
    uui_spinbox_commit(&g_rows);
    g_edit.font_size = g_sysfont.checked ? 0 : uui_spinbox_value(&g_font);
    int sh = uui_dropdown_selected(&g_shell);
    if (sh >= 1 && sh <= g_nshells) strlcpy(g_edit.shell, g_shells[sh - 1], sizeof g_edit.shell);
    else g_edit.shell[0] = '\0';
    g_edit.cols = uui_spinbox_value(&g_cols);
    g_edit.rows = uui_spinbox_value(&g_rows);
    look_from_controls();
}

static void on_close(void) {
    for (int i = 0; i < LOOK_CARDS; i++) ucrt_free(&g_card_crt[i]);
}

int term_prefs_is_open(void) { return uui_prefs_is_open(); }

static void on_widget(int id, int reason) {
    (void)reason;
    if (id == OPT_SYSFONT) {
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
}

static void edit_defaults(void *edit) { term_conf_defaults(edit); }
static void on_defaults(void) { to_controls(); }

static void on_ok(void) {
    from_controls();
    if (g_on_commit) g_on_commit(&g_edit);
}

void term_prefs_open(struct uapp *a, const struct term_conf *c,
                     void (*on_commit)(const struct term_conf *next)) {
    if (uui_prefs_is_open()) return;   // one at a time
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

    uui_gallery_init(&g_schemes, g_label_ptr, g_scheme_count, 0);
    g_schemes.draw_tile = draw_scheme;
    uint32_t bg = UTHEME_WINDOW_BG, fg = UTHEME_TEXT;
    uui_checkbox_init(&g_sysfont,   0, 0, 0, "Use the desktop's size", bg, fg);
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
    uui_spinbox_init(&g_cols, 120, TERM_COLS_MIN, TERM_COLS_MAX, 1, "columns");
    uui_spinbox_init(&g_rows, 30, TERM_ROWS_MIN, TERM_ROWS_MAX, 1, "rows");
    uui_dropdown_init(&g_shell, 0, 0, 0, 0, g_shell_ptr, 1 + g_nshells);
    to_controls();

    uui_prefs_begin(&(struct uui_prefs_desc){
        .title = "Terminal Options",
        .pages = PAGE_ROWS,
        .page_count = PAGE_COUNT,
        .w = ugfx_char_advance('n') * 118,   // four gallery cards to a row (uui_gallery's min_card)
        .h = ugfx_char_h() * 36,
        .log_prefix = "options",
        .edit = &g_edit,
        .edit_defaults = edit_defaults,
        .on_defaults = on_defaults,
        .on_ok = on_ok,
        .on_widget = on_widget,
        .on_close = on_close,
    });
#define F(field) offsetof(struct term_conf, field)
    int r;
    uui_prefs_row(PAGE_LOOK, "Colour scheme:", &uui_gallery_ops, &g_schemes, OPT_SCHEME, "scheme", UUI_FILL_W);
    r = uui_prefs_row(PAGE_LOOK, "Font size:", &uui_spinbox_ops, &g_font, OPT_FONT, "font", 0);
    uui_prefs_also(r, &uui_checkbox_ops, &g_sysfont, OPT_SYSFONT, "sysfont");
    r = uui_prefs_choice(PAGE_LOOK, "Cursor:", CURSOR_OPTS, 0, 3, F(cursor), "cursor");
    uui_prefs_also_check(r, "Blink", F(cursor_blink), "blink");
    uui_prefs_row(PAGE_EFFECT, "Look:", &uui_gallery_ops, &g_looks, OPT_LOOKS, "looks", UUI_FILL_W);
    // The sliders get a track a hand can travel: their natural width is
    // the labels', which makes four stops a flick of the wrist.
    for (int k = 0; k < 3; k++) {
        static const char *const cap[] = { "Scanlines:", "Glow:", "Vignette:" };
        static const char *const nm[] = { "scanlines", "glow", "vignette" };
        struct uui_slider *sl[] = { &g_scan, &g_glow, &g_vig };
        r = uui_prefs_row(PAGE_EFFECT, cap[k], &uui_slider_ops, sl[k], OPT_SCAN + k, nm[k], 0);
        uui_prefs_widen(r, ugfx_char_advance('n') * 34);
    }
    uui_prefs_row(PAGE_EFFECT, "Curvature:", &uui_segmented_ops, &g_curve, OPT_CURVE, "curve", 0);
    uui_prefs_row(PAGE_EFFECT, "Phosphor mask:", &uui_segmented_ops, &g_mask, OPT_MASK, "mask", 0);
    r = uui_prefs_row(PAGE_EFFECT, "Animation:", &uui_checkbox_ops, &g_flicker, OPT_FLICKER, "flicker", 0);
    uui_prefs_also(r, &uui_checkbox_ops, &g_noise, OPT_NOISE, "noise");
    uui_prefs_row(PAGE_SHELL, "Shell for new tabs:", &uui_dropdown_ops, &g_shell, OPT_SHELL, "shell", 0);
    uui_prefs_choice(PAGE_SHELL, "New tabs open in:", NEWTAB_OPTS, NEWTAB_VALUES, 2, F(newtab_dir), "newtab");
    uui_prefs_choice(PAGE_SHELL, "When a shell exits:", ONEXIT_OPTS, 0, 2, F(keep_on_exit), "onexit");
    uui_prefs_check(PAGE_SHELL, "Closing:", "Ask before closing several tabs", F(confirm_close), "confirm");
    uui_prefs_number(PAGE_SCROLL, "Scrollback:", TERM_SB_MIN, TERM_SB_MAX, 100, "lines", F(scrollback), "scrollback");
    uui_prefs_check(PAGE_SCROLL, "Scrollbar:", "Show the scrollbar", F(scrollbar), "scrollbar");
    uui_prefs_check(PAGE_SCROLL, "Output:", "Scroll to the bottom on output", F(scroll_on_output), "scrollout");
    uui_prefs_check(PAGE_SCROLL, "Selection:", "Copy on select", F(copy_on_select), "copysel");
    r = uui_prefs_row(PAGE_WINDOW, "Size on open:", &uui_spinbox_ops, &g_cols, OPT_COLS, "cols", 0);
    uui_prefs_also(r, &uui_spinbox_ops, &g_rows, OPT_ROWS, "rows");
    uui_prefs_check(PAGE_WINDOW, "Menu bar:", "Show the menu bar", F(menubar), "menubar");
    uui_prefs_check(PAGE_WINDOW, "Session panel:", "Show the Session panel", F(panel), "panel");
#undef F
    if (!uui_prefs_open(a)) ulog("uterm: could not open Options\n");
    ulogf("uterm: options open %d scheme(s) %d shell(s)\n", g_scheme_count, g_nshells);
}
