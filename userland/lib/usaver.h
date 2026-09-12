#ifndef ULIB_USAVER_H
#define ULIB_USAVER_H

#include <stdint.h>
#include <stddef.h>

// A SCREENSAVER'S OPTIONS -- what it DECLARES, and what the user chose.
//
// A saver is a program in /bin/wm/savers with no arguments and no
// settings of its own (wm_idle.h). This is the pair of files that give
// one options without giving it a settings UI:
//
//   /usr/wm/savers/<name>.saver   what options exist -- shipped, read-only
//   /etc/savers/<name>.conf       what they are set to -- written by the user
//
// XScreenSaver's arrangement, and the reason for it: each hack there
// ships an XML descriptor of its options and `xscreensaver-settings`
// GENERATES the dialog from it, so a hack carries no UI code and the
// settings app carries no list of hacks. Windows' model is the other
// one -- a `.scr` run with /c draws its own dialog -- and it puts a
// whole settings window in every saver. Declarative wins here for the
// same reason .desktop files build the Start menu: the thing that knows
// is a data file, so there is one place to change.
//
// TWO CALLERS, and each uses one half. System Settings reads the
// descriptor to build controls and writes the conf; the saver reads
// both and asks for values. Neither parses anything itself -- the
// format is etc_config's, so `edit` and `config` understand these files
// too.
//
// A SAVER WITH NO DESCRIPTOR HAS NO OPTIONS, which is a state rather
// than an error: `blank` ships without one and draws a black screen.

#define USAVER_OPT_MAX     4  // options one saver may declare
#define USAVER_CHOICE_MAX  6  // values one enum option may offer
#define USAVER_KEY_MAX     16
#define USAVER_LABEL_MAX   32
#define USAVER_DESC_MAX    96
#define USAVER_VALUE_MAX   24
#define USAVER_UNIT_MAX    8

// An option is a BOUNDED INT or a NAMED CHOICE, and deliberately not a
// bool: the settings registry has neither a bool type nor a checkbox
// control, so a toggle here is `enum:off,on` and gets the same two
// radio buttons every other two-way choice in this system gets.
enum usaver_type { USAVER_INT = 0, USAVER_ENUM = 1 };

// Which control an enum option wants, when the count alone would
// choose wrong -- a slider for an ORDERED enum (low/medium/high), where
// a radio list says nothing about the order. Mirrors the setting ABI's
// widget hints, because System Settings renders both through one path.
enum usaver_widget {
    USAVER_WIDGET_AUTO = 0,
    USAVER_WIDGET_RADIO,
    USAVER_WIDGET_DROPDOWN,
    USAVER_WIDGET_SLIDER,
};

struct usaver_opt {
    char key[USAVER_KEY_MAX];       // the /etc/savers/<name>.conf key
    char label[USAVER_LABEL_MAX];   // what a UI shows; never `key`
    char desc[USAVER_DESC_MAX];     // one line, or ""
    int  type;
    int  imin, imax, istep;         // INT only
    char unit[USAVER_UNIT_MAX];     // INT only: "%", "px", or ""
    int  widget;                    // ENUM only
    int  choice_count;              // ENUM only
    char choice[USAVER_CHOICE_MAX][USAVER_VALUE_MAX];
    char def[USAVER_VALUE_MAX];     // the declared default
    // What it is set to: the conf file's value when it has one and the
    // descriptor still allows it, else `def`. Resolved at load, so a
    // caller never has to decide -- and an out-of-range value left in
    // the file by hand reads as the default rather than as itself.
    char value[USAVER_VALUE_MAX];
};

struct usaver {
    char name[USAVER_KEY_MAX * 2];  // the program name, e.g. "starfield"
    int  opt_count;                 // 0 for a saver with no descriptor
    struct usaver_opt opt[USAVER_OPT_MAX];
};

// Loads `saver`'s descriptor and its saved values. Returns 1 when a
// descriptor was read, 0 when there is none -- and `out` is usable
// either way, with opt_count 0, so no caller needs the distinction.
//
// Reads two files and nothing more; a saver calls this once at startup
// rather than asking per value (uconf_get() re-reads a whole document
// per key, which is what made a nine-entry desktop reload cost 54 file
// reads before anybody measured it).
int usaver_load(const char *saver, struct usaver *out);

// The option named `key`, or NULL.
const struct usaver_opt *usaver_find(const struct usaver *s, const char *key);

// The current value of `key`. `fallback` is returned when the saver
// declares no such option -- which is what a saver reading a key it
// once had, from a descriptor that has since dropped it, gets.
int usaver_int(const struct usaver *s, const char *key, int fallback);
const char *usaver_str(const struct usaver *s, const char *key,
                       const char *fallback);
// An ENUM option's value as an INDEX into its choice list, which is
// what a saver switching on it wants -- the strings are for the UI.
int usaver_index(const struct usaver *s, const char *key, int fallback);

// Where `saver`'s values are written. One place, because System
// Settings writes this path and the saver reads it.
void usaver_conf_path(const char *saver, char *out, size_t cap);

// A stored value as a UI should show it: "amber" -> "Amber". The
// descriptor carries no display names on purpose -- a choice here is a
// word the author chose, not an opaque token like a timezone's
// `losangeles`, so capitalising is the whole of the job and a
// Choice.<key>.<value>= line per choice would be format for its own
// sake.
void usaver_display(const char *value, char *out, size_t cap);

#endif // ULIB_USAVER_H
