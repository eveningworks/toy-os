// Notepad's options file, /etc/notepad.conf: `name=value`, booleans as
// on/off -- one lib/uprefs.h table, so load, save and the defaults cannot
// disagree about which keys exist or what they default to.
#include "notepad.h"

#include <string.h>

#include "lib/uprefs.h"

static const char *const PREVIEW_WORDS[] = { "markdown", "always", "never" };

#define C struct np_conf
static const struct upref KEYS[] = {
    UPREF_INT_KEY("font_size",        C, font_size,      0, NP_FONT_MAX, 0),
    UPREF_INT_KEY("tab_width",        C, tab_width,      2, 8, 4),
    UPREF_BOOL_KEY("tab_spaces",      C, tab_spaces,     0),
    UPREF_BOOL_KEY("auto_indent",     C, auto_indent,    1),
    UPREF_BOOL_KEY("show_whitespace", C, show_ws,        0),
    UPREF_BOOL_KEY("wrap",            C, wrap,           1),
    UPREF_BOOL_KEY("line_numbers",    C, linenums,       1),
    UPREF_BOOL_KEY("line_highlight",  C, line_highlight, 1),
    UPREF_BOOL_KEY("command_bar",     C, cmdbar,         1),
    UPREF_BOOL_KEY("status_bar",      C, statusbar,      1),
    UPREF_BOOL_KEY("menu_bar",        C, menubar,        1),
    UPREF_WORD_KEY("preview",         C, preview,        PREVIEW_WORDS, 3, NP_PREVIEW_MD),
    UPREF_BOOL_KEY("preview_below",   C, preview_below,  0),
    UPREF_BOOL_KEY("scroll_margin",   C, scroll_margin,  0),
    UPREF_BOOL_KEY("reopen",          C, reopen,         0),
    UPREF_BOOL_KEY("open_window",     C, open_window,    0),
    UPREF_BOOL_KEY("crlf",            C, crlf_new,       0),
    UPREF_INT_KEY("recent",           C, recent_max,     0, NP_RECENT_MAX, 8),
    UPREF_BOOL_KEY("final_newline",   C, final_newline,  0),
    UPREF_BOOL_KEY("trim_trailing",   C, trim_trailing,  0),
    UPREF_BOOL_KEY("confirm_close",   C, confirm_close,  1),
};
#undef C
static const struct uprefs PREFS = { NOTEPAD_CONF, KEYS, (int)(sizeof KEYS / sizeof KEYS[0]) };

void np_conf_defaults(struct np_conf *c) {
    memset(c, 0, sizeof *c);
    uprefs_defaults(&PREFS, c);
}

void np_conf_load(struct np_conf *c) {
    memset(c, 0, sizeof *c);
    uprefs_load(&PREFS, c);
    // A font size the spinbox could not show is the desktop's.
    if (c->font_size && c->font_size < NP_FONT_MIN) c->font_size = 0;
    if (c->tab_width != 2 && c->tab_width != 4 && c->tab_width != 8) c->tab_width = 4;
}

void np_conf_save(const struct np_conf *c, const struct np_conf *was) {
    uprefs_save(&PREFS, c, was);
}
