// Notepad's options file, /etc/notepad.conf: `name=value`, booleans as
// on/off, read and written through lib/uconf.h -- the File Manager's and
// Terminal's arrangement. ONE TABLE names every key, so load, save and
// the defaults cannot disagree about which keys exist.
#include "notepad.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "lib/uconf.h"

enum { K_BOOL, K_INT, K_WORD };

static const char *const PREVIEW_WORDS[] = { "markdown", "always", "never" };

static const struct key {
    const char *name;
    size_t off;
    int kind;
    int lo, hi;                  // K_INT's range; K_WORD's count in `hi`
    const char *const *words;
} KEYS[] = {
    { "font_size",      offsetof(struct np_conf, font_size),      K_INT,  0, NP_FONT_MAX, 0 },
    { "tab_width",      offsetof(struct np_conf, tab_width),      K_INT,  2, 8, 0 },
    { "tab_spaces",     offsetof(struct np_conf, tab_spaces),     K_BOOL, 0, 0, 0 },
    { "auto_indent",    offsetof(struct np_conf, auto_indent),    K_BOOL, 0, 0, 0 },
    { "show_whitespace", offsetof(struct np_conf, show_ws),       K_BOOL, 0, 0, 0 },
    { "wrap",           offsetof(struct np_conf, wrap),           K_BOOL, 0, 0, 0 },
    { "line_numbers",   offsetof(struct np_conf, linenums),       K_BOOL, 0, 0, 0 },
    { "line_highlight", offsetof(struct np_conf, line_highlight), K_BOOL, 0, 0, 0 },
    { "command_bar",    offsetof(struct np_conf, cmdbar),         K_BOOL, 0, 0, 0 },
    { "status_bar",     offsetof(struct np_conf, statusbar),      K_BOOL, 0, 0, 0 },
    { "menu_bar",       offsetof(struct np_conf, menubar),        K_BOOL, 0, 0, 0 },
    { "preview",        offsetof(struct np_conf, preview),        K_WORD, 0, 3, PREVIEW_WORDS },
    { "preview_below",  offsetof(struct np_conf, preview_below),  K_BOOL, 0, 0, 0 },
    { "scroll_margin",  offsetof(struct np_conf, scroll_margin),  K_BOOL, 0, 0, 0 },
    { "reopen",         offsetof(struct np_conf, reopen),         K_BOOL, 0, 0, 0 },
    { "open_window",    offsetof(struct np_conf, open_window),    K_BOOL, 0, 0, 0 },
    { "crlf",           offsetof(struct np_conf, crlf_new),       K_BOOL, 0, 0, 0 },
    { "recent",         offsetof(struct np_conf, recent_max),     K_INT,  0, NP_RECENT_MAX, 0 },
    { "final_newline",  offsetof(struct np_conf, final_newline),  K_BOOL, 0, 0, 0 },
    { "trim_trailing",  offsetof(struct np_conf, trim_trailing),  K_BOOL, 0, 0, 0 },
    { "confirm_close",  offsetof(struct np_conf, confirm_close),  K_BOOL, 0, 0, 0 },
};
#define NKEYS ((int)(sizeof KEYS / sizeof KEYS[0]))

static int *field(struct np_conf *c, const struct key *k) { return (int *)((char *)c + k->off); }
static int value(const struct np_conf *c, const struct key *k) {
    return *(const int *)((const char *)c + k->off);
}

void np_conf_defaults(struct np_conf *c) {
    memset(c, 0, sizeof *c);
    c->tab_width = 4;
    c->auto_indent = 1;
    c->wrap = 1;
    c->linenums = 1;
    c->line_highlight = 1;
    c->cmdbar = 1;
    c->statusbar = 1;
    c->menubar = 1;
    c->preview = NP_PREVIEW_MD;
    c->recent_max = 8;
    c->confirm_close = 1;
}

// A value this cannot read leaves the default: a typo in the file is a
// key ignored, never an option silently set to zero.
static void parse(struct np_conf *c, const struct key *k, const char *v) {
    if (k->kind == K_BOOL) {
        if (!strcmp(v, "on")) *field(c, k) = 1;
        else if (!strcmp(v, "off")) *field(c, k) = 0;
    } else if (k->kind == K_WORD) {
        for (int w = 0; w < k->hi; w++)
            if (!strcmp(v, k->words[w])) *field(c, k) = w;
    } else {
        int n = 0, any = 0;
        for (const char *p = v; *p >= '0' && *p <= '9'; p++) { n = n * 10 + (*p - '0'); any = 1; }
        if (any && n >= k->lo && n <= k->hi) *field(c, k) = n;
    }
}

void np_conf_load(struct np_conf *c) {
    np_conf_defaults(c);
    static struct etc_config_buf buf;   // 4 KiB: never on a ring-3 frame
    if (!uconf_load(NOTEPAD_CONF, &buf)) return;
    for (int i = 0; i < NKEYS; i++) {
        char v[32];
        if (etc_config_buf_get(&buf, KEYS[i].name, v, sizeof v)) parse(c, &KEYS[i], v);
    }
    // A font size the spinbox could not show is the desktop's.
    if (c->font_size && c->font_size < NP_FONT_MIN) c->font_size = 0;
    if (c->tab_width != 2 && c->tab_width != 4 && c->tab_width != 8) c->tab_width = 4;
}

void np_conf_save(const struct np_conf *c, const struct np_conf *was) {
    for (int i = 0; i < NKEYS; i++) {
        const struct key *k = &KEYS[i];
        int v = value(c, k);
        if (was && v == value(was, k)) continue;
        char s[16];
        if (k->kind == K_BOOL) snprintf(s, sizeof s, "%s", v ? "on" : "off");
        else if (k->kind == K_WORD) snprintf(s, sizeof s, "%s", k->words[v]);
        else snprintf(s, sizeof s, "%d", v);
        uconf_set(NOTEPAD_CONF, k->name, s);
    }
}
