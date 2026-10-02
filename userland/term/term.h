#ifndef TERM_H
#define TERM_H

#include <stdint.h>
#include "ui/ucrt.h"

// The GUI Terminal's own preferences: what is in /etc/terminal.conf,
// what a colour scheme is, and the dialog that edits both.
//
// **THESE ARE THE TERMINAL'S, NOT THE REGISTRY'S.** Everything else
// with a knob in this system registers a setting and gets a row in
// System Settings for free (api/setting.h), and that is the right home
// for anything the SYSTEM owns -- the font, the wallpaper, the shell.
// A terminal's palette is not one of those: Konsole, GNOME Terminal and
// Windows Terminal all keep theirs in the application, because the
// people who change them are changing this window rather than the
// desktop. Following the registry here would also mean ring-0 code
// (setting_register() is the kernel's) for a ring-3 app's appearance.
//
// The file is etc_config's format, so `edit /etc/terminal.conf` and
// `config` read it like any other -- no second parser (uconf.h).

#define TERM_SCHEME_DIR  "/usr/share/terminal"
#define TERM_CONF_PATH   "/etc/terminal.conf"

#define TERM_NAME_MAX    24   // a scheme's file name, without .scheme
#define TERM_LABEL_MAX   32   // its `Name=`, which is what a UI shows
#define TERM_SCHEME_MAX  16   // schemes one directory may offer

// --- a colour scheme --------------------------------------------------

// **`pal` IS IN VGA INDEX ORDER, AND THE FILE IS IN ANSI ORDER.** A
// cell holds an `enum vga_color` (api/ansi.h), so blue is 1 and red is
// 4 -- while every palette anyone publishes is written red-first.
// term_scheme_load() permutes through ansi_color(), so a scheme file
// can be copied in verbatim.
struct term_scheme {
    char name[TERM_NAME_MAX];    // the file name; "" for the built-in
    char label[TERM_LABEL_MAX];  // `Name=`, or the file name
    uint32_t pal[16];            // VGA order
    int fg, bg;                  // which slots the default pair uses
    uint32_t cursor;             // RGB, not a slot: no terminal has one
};

// Fills `out` with the compiled-in VGA palette. The fallback for a
// machine whose /usr/share/terminal is absent -- a fresh flash, a
// diskless boot -- so a missing directory costs the chosen scheme and
// never the window.
void term_scheme_builtin(struct term_scheme *out);

// Loads `name`.scheme. Returns 1, or 0 with `out` left holding the
// built-in palette, so a caller needs no separate failure path.
int term_scheme_load(const char *name, struct term_scheme *out);

// Every scheme in TERM_SCHEME_DIR, sorted, names only. Returns the
// count; `labels[i]` is the matching `Name=` for a UI to show.
int term_scheme_list(char names[][TERM_NAME_MAX],
                      char labels[][TERM_LABEL_MAX], int max);

// --- the preferences --------------------------------------------------

enum term_cursor_shape { TERM_CURSOR_BLOCK = 0, TERM_CURSOR_UNDER, TERM_CURSOR_BAR };

// Where a new tab's shell starts: the home directory, or wherever the
// selected tab's shell last said it was (its OSC title).
enum term_newtab_dir { TERM_NEWTAB_HERE = 0, TERM_NEWTAB_HOME };

#define TERM_SHELL_MAX 64   // a path from /etc/shells

// One field per key in /etc/terminal.conf. Ints rather than strings
// because every consumer wants a number; the file's words are resolved
// once, on load.
struct term_conf {
    char scheme[TERM_NAME_MAX];
    char shell[TERM_SHELL_MAX]; // "" = the system shell (`system.shell`)
    int font_size;        // 0 = follow the desktop's system.font_size
    int scrollback;       // lines kept above the screen
    int cursor;           // enum term_cursor_shape
    int cursor_blink;
    int menubar;          // is the bar shown when a window opens
    int panel;            // is the Session panel shown when a window opens
    int scrollbar;        // draw the scrollbar (and reserve its gutter)
    int cols, rows;       // the grid a new window opens at
    int newtab_dir;       // enum term_newtab_dir
    int keep_on_exit;     // a tab whose shell exits stays, until closed
    int copy_on_select;
    int scroll_on_output;
    int confirm_close;    // ask before closing a window with more than one tab
    int effect;           // a new window opens with the screen effect on
    struct ucrt_look crt; // what the screen effect looks like, on or off
};

// Between the grid and the window edge, in pixels.
#define TERM_MARGIN 6

#define TERM_FONT_MIN 8
#define TERM_FONT_MAX 32
#define TERM_SB_MIN 100
#define TERM_SB_MAX 10000
#define TERM_COLS_MIN 40
#define TERM_COLS_MAX 300
#define TERM_ROWS_MIN 10
#define TERM_ROWS_MAX 100

// The shipped defaults -- what a machine with no /etc/terminal.conf
// runs, and what the file is compared against when saving.
void term_conf_defaults(struct term_conf *c);

// Reads the file over the defaults. A key that is missing, unparseable
// or out of range keeps its default rather than failing the load: a
// hand-edited file with one bad line should cost that line and nothing
// else.
void term_conf_load(struct term_conf *c);

// Writes back every key whose value differs from `old`. Returns the
// number of keys written, or -1 if a write failed. Only the differences
// because uconf_set() rewrites the whole document per call, and a save
// of ten unchanged keys is ten whole-file rewrites.
int term_conf_save(const struct term_conf *c, const struct term_conf *old);

// The shells /etc/shells lists, in file order. Returns the count. A
// missing file still offers SHELL_FALLBACK, so the list is never empty.
#define TERM_SHELLS_PATH "/etc/shells"
#define TERM_SHELLS_MAX  8
int term_shells_list(char paths[][TERM_SHELL_MAX], int max);

// --- the Options window (term_prefs.c) --------------------------------
//
// A modal window of its own -- File Manager Options' shape: a sidebar of
// pages over Defaults / OK / Cancel. OK hands the edited copy to
// `on_commit`; Cancel and Esc forget it.

struct uapp;
void term_prefs_open(struct uapp *a, const struct term_conf *c,
                     void (*on_commit)(const struct term_conf *next));
int  term_prefs_is_open(void);

// --- the Session panel (term_panel.c) ---------------------------------

enum { TERM_PANEL_INTR = 0, TERM_PANEL_EOF, TERM_PANEL_KILL, TERM_PANEL_BUTTONS };
#define TERM_PANEL_ID_BASE 40   // the buttons' widget ids, in that order

struct term_panel_info {
    const char *shell;    // its path
    const char *cwd;      // "" when the shell never said
    const char *scheme;   // the scheme's label
    int pid;              // the shell; its group is the same number
    int exited;           // the shell has gone and the tab is being kept
    int cols, rows;
    int sb_count, sb_cap;
};

struct uui_item;
struct ugfx_surface;
void term_panel_init(void);
int  term_panel_width(void);          // font-derived
struct uui_item *term_panel_item(int i);
void term_panel_layout(int x, int y, int w, int h);
void term_panel_set_exited(int exited);
void term_panel_draw(struct ugfx_surface *s, const struct term_panel_info *in);

#endif // TERM_H
