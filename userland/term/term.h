#ifndef TERM_H
#define TERM_H

#include <stdint.h>

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

// One field per key in /etc/terminal.conf. Ints rather than strings
// because every consumer wants a number; the file's words are resolved
// once, on load.
struct term_conf {
    char scheme[TERM_NAME_MAX];
    int font_size;        // 0 = follow the desktop's system.font_size
    int scrollback;       // lines kept above the screen
    int cursor;           // enum term_cursor_shape
    int cursor_blink;
    int margin;           // pixels between the grid and the window edge
    int menubar;          // is the bar shown when a window opens
    int copy_on_select;
    int scroll_on_output;
    int confirm_close;    // ask before closing a window with more than one tab
};

#define TERM_FONT_MIN 8
#define TERM_FONT_MAX 32
#define TERM_SB_MIN 100
#define TERM_SB_MAX 10000
#define TERM_MARGIN_MIN 0
#define TERM_MARGIN_MAX 24

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

// --- the Preferences dialog (term_prefs.c) ----------------------------
//
// It owns its own uui_dialog. The app hands the item to its widget
// array, keeps the bounds up to date, and asks after each change
// whether a commit has happened.

struct uui_item;

void term_prefs_init(void);
struct uui_item *term_prefs_item(void);
int  term_prefs_is_open(void);
void term_prefs_set_bounds(int x, int y, int w, int h);
void term_prefs_open(const struct term_conf *c);

// Forwarded from the app's on_widget(). Moves the dialog's key focus
// onto a clicked control and notices the OK button.
void term_prefs_widget(int id, int reason);

// 1 when OK was pressed since the last call, with `out` filled from the
// controls; 0 otherwise. TAKEN, so a second call does not apply twice.
int  term_prefs_take(struct term_conf *out);

#endif // TERM_H
