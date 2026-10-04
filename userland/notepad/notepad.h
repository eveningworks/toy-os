#ifndef NOTEPAD_H
#define NOTEPAD_H

// Notepad's own pieces outside the app file: its options (np_conf.c),
// their window (np_prefs.c), and the state it keeps between runs.

struct uapp;

#define NOTEPAD_CONF        "/etc/notepad.conf"
#define NOTEPAD_STATE_DIR   "/var/lib/notepad"
#define NOTEPAD_RECENT      "/var/lib/notepad/recent"    // one path a line, newest first
#define NOTEPAD_SESSION     "/var/lib/notepad/session"   // the tabs open at the last exit

#define NP_FONT_MIN    8
#define NP_FONT_MAX    40
#define NP_RECENT_MAX  10

enum { NP_PREVIEW_MD = 0, NP_PREVIEW_ALWAYS, NP_PREVIEW_NEVER };

// Every option, as Notepad uses it. A missing key keeps its default, so
// a file written by an older Notepad still reads.
struct np_conf {
    // Editor
    int font_size;       // px; 0 follows the desktop's size
    int tab_width;       // 2, 4 or 8
    int tab_spaces;      // Tab inserts spaces
    int auto_indent;     // a new line keeps the indent of the one above
    int show_ws;         // spaces and tabs drawn
    int wrap;            // new documents wrap
    // View
    int linenums, line_highlight, cmdbar, statusbar, menubar;
    int preview;         // NP_PREVIEW_*
    int preview_below;   // the preview under the text rather than beside it
    int scroll_margin;   // keep lines visible around the caret
    // Files and startup
    int reopen;          // reopen the last session's tabs
    int open_window;     // File > Open starts a new window, not a tab
    int crlf_new;        // new documents end lines with CRLF
    int recent_max;      // how many recent files are kept, 0 .. NP_RECENT_MAX
    int final_newline;   // a saved file ends with a newline
    int trim_trailing;   // trailing spaces and tabs are removed on save
    int confirm_close;   // closing the window with several tabs asks
};

void np_conf_defaults(struct np_conf *c);
void np_conf_load(struct np_conf *c);
// Writes only the keys that differ from `was` -- one rewrite per change.
void np_conf_save(const struct np_conf *c, const struct np_conf *was);

// The Options window: shows `c`, and calls `on_commit` with the edited
// copy on OK. Cancel leaves everything as it was -- except "Clear list",
// a command that runs `on_clear_recent` at once.
void np_prefs_open(struct uapp *a, const struct np_conf *c,
                   void (*on_commit)(const struct np_conf *next),
                   void (*on_clear_recent)(void));
int np_prefs_is_open(void);

#endif
