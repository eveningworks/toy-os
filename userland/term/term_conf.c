// /etc/terminal.conf and the colour schemes beside it -- the reading
// and writing half of the GUI Terminal's preferences. The dialog that
// edits them is term_prefs.c; see term.h for why they are the
// application's rather than the settings registry's.
#include "term.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ansi.h"        // ansi_color(), which is the ANSI -> VGA permutation
#include "lib/dirsort.h"
#include "lib/uconf.h"
#include "rt/sys.h"
#include "ui/ulog.h"

// --- colour schemes ---------------------------------------------------

// The IBM VGA palette, in VGA index order. Also shipped as
// default.scheme; this copy is what a machine with no
// /usr/share/terminal draws with, so the window never depends on the
// data directory having survived.
static const uint32_t VGA_PALETTE[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

void term_scheme_builtin(struct term_scheme *out) {
    memset(out, 0, sizeof *out);
    strlcpy(out->label, "Default (VGA)", sizeof out->label);
    for (int i = 0; i < 16; i++) out->pal[i] = VGA_PALETTE[i];
    out->fg = 7;   // light grey on black, the console's own pair
    out->bg = 0;
    out->cursor = VGA_PALETTE[7];
}

// Six hex digits, and nothing else. A REJECTION rather than a partial
// parse: "aabbc" read as a colour is a wrong colour that looks like a
// right one, where a refused line is a slot left at its default.
static int parse_rgb(const char *s, uint32_t *out) {
    uint32_t v = 0;
    int n = 0;
    for (; s[n]; n++) {
        char c = s[n];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;
        if (n >= 6) return 0;
        v = (v << 4) | (uint32_t)d;
    }
    if (n != 6) return 0;
    *out = v;
    return 1;
}

static int parse_slot(const char *s, int *out) {
    if (!s[0]) return 0;
    for (int i = 0; s[i]; i++) if (s[i] < '0' || s[i] > '9') return 0;
    int v = atoi(s);
    if (v < 0 || v > 15) return 0;
    *out = v;
    return 1;
}

static void scheme_path(const char *name, char *out, int cap) {
    snprintf(out, (size_t)cap, "%s/%s.scheme", TERM_SCHEME_DIR, name);
}

// One document read, twenty keys asked -- uconf_get() would re-read the
// whole file per key, which is what made a nine-entry desktop reload
// cost 54 whole-file reads before anybody measured it.
static struct etc_config_buf g_buf;

int term_scheme_load(const char *name, struct term_scheme *out) {
    term_scheme_builtin(out);
    if (!name || !name[0]) return 0;
    strlcpy(out->name, name, sizeof out->name);

    char path[96];
    scheme_path(name, path, sizeof path);
    if (!uconf_load(path, &g_buf)) return 0;

    char v[TERM_LABEL_MAX];
    if (etc_config_buf_get(&g_buf, "Name", v, sizeof v) && v[0])
        strlcpy(out->label, v, sizeof out->label);
    else
        strlcpy(out->label, name, sizeof out->label);

    // THE FILE IS ANSI-ORDERED AND `pal` IS VGA-ORDERED. ansi_color(i,
    // bright) is the kernel parser's own table, so the two halves of
    // this system cannot drift into disagreeing about where red is.
    for (int i = 0; i < 16; i++) {
        char key[12];
        snprintf(key, sizeof key, "Color%d", i);
        uint32_t rgb;
        if (!etc_config_buf_get(&g_buf, key, v, sizeof v)) continue;
        if (!parse_rgb(v, &rgb)) continue;
        out->pal[(int)ansi_color(i & 7, i >= 8)] = rgb;
    }

    // Indices, not colours: the default pair has to be a palette slot,
    // because SGR 39/49 return the CELL to it and a cell holds an index.
    //
    // **AND THEY NAME A `Color<N>`, SO THEY PERMUTE TOO.** They are
    // read in the file's ANSI numbering like everything else in it, and
    // a cell stores VGA -- taking one as a VGA slot already put
    // Solarized Dark's foreground on light red instead of base0, which
    // looked plausible and was wrong.
    int n;
    if (etc_config_buf_get(&g_buf, "Foreground", v, sizeof v) && parse_slot(v, &n))
        out->fg = (int)ansi_color(n & 7, n >= 8);
    if (etc_config_buf_get(&g_buf, "Background", v, sizeof v) && parse_slot(v, &n))
        out->bg = (int)ansi_color(n & 7, n >= 8);

    out->cursor = out->pal[out->fg];
    if (etc_config_buf_get(&g_buf, "Cursor", v, sizeof v)) parse_rgb(v, &out->cursor);
    return 1;
}

// Strips ".scheme", or returns 0 for a name that does not end in it --
// which is how the README and anything else in the directory stays out
// of the list without the directory needing to hold only schemes.
static int scheme_name_of(const char *file, char *out, int cap) {
    int n = (int)strlen(file);
    const char *ext = ".scheme";
    int e = (int)strlen(ext);
    if (n <= e || strcmp(file + n - e, ext) != 0) return 0;
    if (n - e >= cap) return 0;
    memcpy(out, file, (size_t)(n - e));
    out[n - e] = '\0';
    return 1;
}

int term_scheme_list(char names[][TERM_NAME_MAX],
                      char labels[][TERM_LABEL_MAX], int max) {
    static struct sys_dirent ents[TERM_SCHEME_MAX * 2];
    int n = sys_listdir(TERM_SCHEME_DIR, ents,
                         (int)(sizeof ents / sizeof ents[0]));
    if (n <= 0) return 0;
    dirsort(ents, n, DIRSORT_NAME, 0);

    int count = 0;
    for (int i = 0; i < n && count < max; i++) {
        if (ents[i].is_dir) continue;
        if (!scheme_name_of(ents[i].name, names[count], TERM_NAME_MAX)) continue;
        // The LABEL costs a file read each, and there are at most a
        // dozen -- done once when the dialog opens, never per frame.
        struct term_scheme s;
        term_scheme_load(names[count], &s);
        strlcpy(labels[count], s.label, TERM_LABEL_MAX);
        count++;
    }
    return count;
}

// --- /etc/terminal.conf -----------------------------------------------

void term_conf_defaults(struct term_conf *c) {
    memset(c, 0, sizeof *c);
    strlcpy(c->scheme, "slate", sizeof c->scheme);
    c->font_size = 0;          // follow system.font_size
    c->scrollback = 240;
    c->cursor = TERM_CURSOR_BLOCK;
    // **OFF BY DEFAULT, and the reason is testability rather than
    // taste.** A blinking caret makes a focused terminal ANIMATE, and a
    // settled-frame comparison -- the standard tool for judging
    // anything drawn here (docs/testing.md) -- cannot tell a real
    // change from a caret in the other phase. Turning it on made an
    // unrelated tab-switch check fail intermittently. xterm and VS
    // Code's terminal both ship it off too; Konsole and GNOME Terminal
    // ship it on, so this is a real split and not a lone opinion.
    c->cursor_blink = 0;
    c->menubar = 0;         // the bar's menu button holds every command
    c->panel = 0;
    c->scrollbar = 1;
    c->cols = 120;          // Windows Terminal's default grid
    c->rows = 30;
    c->newtab_dir = TERM_NEWTAB_HERE;
    c->keep_on_exit = 0;
    c->copy_on_select = 1;
    c->scroll_on_output = 1;
    c->confirm_close = 1;
    // Off, and when turned on, the Subtle look -- the one that suits any
    // scheme. Flicker and noise animate the window, so no preset has them.
    c->effect = 0;
    c->crt = ucrt_presets[UCRT_PRESET_SUBTLE];
}

// A WORD, never a number, for anything with names -- `cursor=block`
// survives a reordering of the enum and reads correctly in `edit`.
static const char *const CURSOR_WORDS[] = { "block", "underline", "bar" };
#define CURSOR_WORD_COUNT ((int)(sizeof CURSOR_WORDS / sizeof CURSOR_WORDS[0]))

static const char *const LEVEL_WORDS[] = { "off", "low", "medium", "high" };
static const char *const CURVE_WORDS[] = { "off", "subtle", "strong" };
static const char *const MASK_WORDS[] = { "off", "aperture", "slot" };

static const char *cursor_word(int shape) {
    if (shape < 0 || shape >= CURSOR_WORD_COUNT) shape = TERM_CURSOR_BLOCK;
    return CURSOR_WORDS[shape];
}

static void get_int(const char *key, int *val, int lo, int hi) {
    char v[24];
    if (!etc_config_buf_get(&g_buf, key, v, sizeof v) || !v[0]) return;
    for (int i = 0; v[i]; i++) if (v[i] < '0' || v[i] > '9') return;
    int n = atoi(v);
    // OUT OF RANGE KEEPS THE DEFAULT. Clamping would silently turn a
    // hand-typed 50000 into 10000 and report it back as what was asked
    // for; the same rule uui_spinbox states for typed input.
    if (n < lo || n > hi) return;
    *val = n;
}

static void get_bool(const char *key, int *val) {
    char v[16];
    if (!etc_config_buf_get(&g_buf, key, v, sizeof v) || !v[0]) return;
    if (strcmp(v, "on") == 0 || strcmp(v, "yes") == 0 || strcmp(v, "1") == 0) *val = 1;
    else if (strcmp(v, "off") == 0 || strcmp(v, "no") == 0 || strcmp(v, "0") == 0) *val = 0;
}

// A word from `words`, or the default kept: a misspelling is refused, not guessed.
static void get_word(const char *key, int *val, const char *const *words, int n) {
    char v[16];
    if (!etc_config_buf_get(&g_buf, key, v, sizeof v) || !v[0]) return;
    for (int i = 0; i < n; i++)
        if (strcmp(v, words[i]) == 0) { *val = i; return; }
}

void term_conf_load(struct term_conf *c) {
    term_conf_defaults(c);
    if (!uconf_load(TERM_CONF_PATH, &g_buf)) return;

    char v[TERM_NAME_MAX];
    if (etc_config_buf_get(&g_buf, "scheme", v, sizeof v) && v[0])
        strlcpy(c->scheme, v, sizeof c->scheme);

    // font_size 0 is "follow the desktop", so its floor is 0 rather
    // than TERM_FONT_MIN -- a range with a hole in it, which is why
    // this one key is not a plain get_int() call.
    char fv[16];
    if (etc_config_buf_get(&g_buf, "font_size", fv, sizeof fv) && fv[0]) {
        int ok = 1;
        for (int i = 0; fv[i]; i++) if (fv[i] < '0' || fv[i] > '9') ok = 0;
        int n = ok ? atoi(fv) : -1;
        if (n == 0 || (n >= TERM_FONT_MIN && n <= TERM_FONT_MAX)) c->font_size = n;
    }

    get_int("scrollback", &c->scrollback, TERM_SB_MIN, TERM_SB_MAX);
    get_int("cols", &c->cols, TERM_COLS_MIN, TERM_COLS_MAX);
    get_int("rows", &c->rows, TERM_ROWS_MIN, TERM_ROWS_MAX);

    char sv[TERM_SHELL_MAX];
    if (etc_config_buf_get(&g_buf, "shell", sv, sizeof sv) && sv[0] == '/')
        strlcpy(c->shell, sv, sizeof c->shell);
    char nv[16];
    if (etc_config_buf_get(&g_buf, "new_tab_dir", nv, sizeof nv))
        c->newtab_dir = strcmp(nv, "home") == 0 ? TERM_NEWTAB_HOME : TERM_NEWTAB_HERE;
    if (etc_config_buf_get(&g_buf, "on_exit", nv, sizeof nv))
        c->keep_on_exit = strcmp(nv, "keep") == 0;

    char cv[16];
    if (etc_config_buf_get(&g_buf, "cursor", cv, sizeof cv) && cv[0])
        for (int i = 0; i < CURSOR_WORD_COUNT; i++)
            if (strcmp(cv, CURSOR_WORDS[i]) == 0) { c->cursor = i; break; }

    get_bool("cursor_blink", &c->cursor_blink);
    get_bool("menubar", &c->menubar);
    get_bool("panel", &c->panel);
    get_bool("scrollbar", &c->scrollbar);
    get_bool("copy_on_select", &c->copy_on_select);
    get_bool("scroll_on_output", &c->scroll_on_output);
    get_bool("confirm_close", &c->confirm_close);
    get_bool("effect", &c->effect);
    get_word("crt_scanlines", &c->crt.scanlines, LEVEL_WORDS, UCRT_LEVEL_MAX + 1);
    get_word("crt_glow", &c->crt.glow, LEVEL_WORDS, UCRT_LEVEL_MAX + 1);
    get_word("crt_vignette", &c->crt.vignette, LEVEL_WORDS, UCRT_LEVEL_MAX + 1);
    get_word("crt_curve", &c->crt.curve, CURVE_WORDS, UCRT_CURVE_COUNT);
    get_word("crt_mask", &c->crt.mask, MASK_WORDS, UCRT_MASK_COUNT);
    get_bool("crt_flicker", &c->crt.flicker);
    get_bool("crt_noise", &c->crt.noise);
}

// One key, written only when it moved. `*failed` is sticky: a save that
// writes four keys and fails on the fifth has half-applied the change
// and must say so rather than reporting the four.
static int put(const char *key, const char *value, int *failed) {
    if (!uconf_set(TERM_CONF_PATH, key, value)) { *failed = 1; return 0; }
    return 1;
}

static int put_int(const char *key, int value, int *failed) {
    char v[16];
    snprintf(v, sizeof v, "%d", value);
    return put(key, v, failed);
}

static int put_bool(const char *key, int value, int *failed) {
    return put(key, value ? "on" : "off", failed);
}

int term_conf_save(const struct term_conf *c, const struct term_conf *old) {
    int n = 0, failed = 0;

    if (strcmp(c->scheme, old->scheme) != 0) n += put("scheme", c->scheme, &failed);
    if (c->font_size != old->font_size) n += put_int("font_size", c->font_size, &failed);
    if (c->scrollback != old->scrollback) n += put_int("scrollback", c->scrollback, &failed);
    if (strcmp(c->shell, old->shell) != 0) n += put("shell", c->shell, &failed);
    if (c->cols != old->cols) n += put_int("cols", c->cols, &failed);
    if (c->rows != old->rows) n += put_int("rows", c->rows, &failed);
    if (c->newtab_dir != old->newtab_dir)
        n += put("new_tab_dir", c->newtab_dir == TERM_NEWTAB_HOME ? "home" : "here", &failed);
    if (c->keep_on_exit != old->keep_on_exit)
        n += put("on_exit", c->keep_on_exit ? "keep" : "close", &failed);
    if (c->panel != old->panel) n += put_bool("panel", c->panel, &failed);
    if (c->scrollbar != old->scrollbar) n += put_bool("scrollbar", c->scrollbar, &failed);
    if (c->cursor != old->cursor) n += put("cursor", cursor_word(c->cursor), &failed);
    if (c->cursor_blink != old->cursor_blink) n += put_bool("cursor_blink", c->cursor_blink, &failed);
    if (c->menubar != old->menubar) n += put_bool("menubar", c->menubar, &failed);
    if (c->copy_on_select != old->copy_on_select) n += put_bool("copy_on_select", c->copy_on_select, &failed);
    if (c->scroll_on_output != old->scroll_on_output) n += put_bool("scroll_on_output", c->scroll_on_output, &failed);
    if (c->confirm_close != old->confirm_close) n += put_bool("confirm_close", c->confirm_close, &failed);
    if (c->effect != old->effect) n += put_bool("effect", c->effect, &failed);
    const struct ucrt_look *l = &c->crt, *o = &old->crt;
    if (l->scanlines != o->scanlines) n += put("crt_scanlines", LEVEL_WORDS[l->scanlines], &failed);
    if (l->glow != o->glow) n += put("crt_glow", LEVEL_WORDS[l->glow], &failed);
    if (l->vignette != o->vignette) n += put("crt_vignette", LEVEL_WORDS[l->vignette], &failed);
    if (l->curve != o->curve) n += put("crt_curve", CURVE_WORDS[l->curve], &failed);
    if (l->mask != o->mask) n += put("crt_mask", MASK_WORDS[l->mask], &failed);
    if (l->flicker != o->flicker) n += put_bool("crt_flicker", l->flicker, &failed);
    if (l->noise != o->noise) n += put_bool("crt_noise", l->noise, &failed);

    ulogf("uterm: conf save %d key(s)%s\n", n, failed ? " (a write failed)" : "");
    return failed ? -1 : n;
}

// --- /etc/shells ------------------------------------------------------
//
// One absolute path per line, `#` comments -- the Unix file, and what
// chsh and GDM read. Only paths that exist are offered: a listed shell
// that is not installed would be a menu row that opens a tab and closes
// it again.
int term_shells_list(char paths[][TERM_SHELL_MAX], int max) {
    int n = 0;
    static char buf[1024];
    int got = 0;
    int fd = sys_open(TERM_SHELLS_PATH, 0);
    if (fd >= 0) {
        got = sys_read(fd, buf, sizeof buf - 1);
        sys_close(fd);
    }
    if (got < 0) got = 0;
    buf[got] = '\0';
    for (char *line = buf; *line && n < max; ) {
        char *end = strchr(line, '\n');
        if (end) *end = '\0';
        while (*line == ' ' || *line == '\t') line++;
        int len = (int)strlen(line);
        while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\r')) line[--len] = '\0';
        struct sys_stat st;
        if (line[0] == '/' && len < TERM_SHELL_MAX && sys_stat(line, &st) == 0)
            strlcpy(paths[n++], line, TERM_SHELL_MAX);
        if (!end) break;
        line = end + 1;
    }
    if (n == 0 && max > 0) strlcpy(paths[n++], "/bin/tosh", TERM_SHELL_MAX);
    return n;
}
