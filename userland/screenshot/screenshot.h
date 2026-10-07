#ifndef SCREENSHOT_H
#define SCREENSHOT_H
// Screenshot's options (shot_conf.c) and its Options window
// (shot_prefs.c), shared with the overlay in userland/gui/apps/screenshot.c.
#include "lib/unametpl.h"
#include "ui/uui_nametpl.h"

struct uapp;

#define SHOT_CONF         "/etc/screenshot.conf"   // the app's own (docs/conventions/gui.md)
#define SHOT_DEFAULT_DIR  "/home/screenshots"
#define SHOT_DEFAULT_NAME "shot-<date>-<time>"
// THE COMPOSITOR'S CARD CARRIES A PATH OF AT MOST 124 BYTES
// (WIN_NOTICE_PIECES_MAX pieces), so a folder and a name together must
// fit that with room for "-NN.qoi".
#define SHOT_DIR_MAX  64
#define SHOT_NAME_MAX 48
#define SHOT_PATH_MAX 128

enum { SHOT_MODE_REGION, SHOT_MODE_SCREEN, SHOT_MODE_WINDOW, SHOT_MODES };
#define SHOT_START_LAST SHOT_MODES   // `start`: a mode, or the last one used
#define SHOT_DELAYS 3

struct shot_conf {
    // The pill's own, remembered across runs.
    int mode, pointer, copy, delay;          // delay: the seconds chosen, 0 for none
    // Capture.
    int start, last_region, size_label, magnifier, shadow;
    int delays[SHOT_DELAYS];                 // what the pill's timer cycles through
    int rx, ry, rw, rh, rsw, rsh;            // the last region, and the screen it was on
    // After capture.
    int card, open, flash;
    // Saving.
    char folder[SHOT_DIR_MAX];
    char name[SHOT_NAME_MAX];
    int png, ask;
};

extern const char *const SHOT_MODE_NAME[SHOT_MODES];

void shot_conf_defaults(struct shot_conf *c);
void shot_conf_load(struct shot_conf *c);
void shot_conf_save(const struct shot_conf *c);

// --- naming ---------------------------------------------------------

#define SHOT_CHIP_COUNT 5
extern const struct uui_nametpl_chip SHOT_CHIPS[SHOT_CHIP_COUNT];

// The template's tokens with today's values; `app` is the window's app
// id or "", `mode` a SHOT_MODE_*. The strings live in `store`.
struct shot_vars {
    struct unametpl_var v[SHOT_CHIP_COUNT];
    char date[16], time[16], app[24], n[8];
};
void shot_vars_now(struct shot_vars *s, const char *app, int mode);

// Where the next capture goes: `c->folder`/`c->name` expanded, made
// unique (<n> counts up; without it "-2", "-3"...), with the format's
// extension. 0 when the template is unusable or nothing free is found.
int shot_next_path(const struct shot_conf *c, const char *app, int mode, char *out, int cap);

// A folder the compositor's card will name: absolute, letters, digits
// and ". _ - /" only, no "..", short enough.
int shot_folder_ok(const char *dir);

// --- the Print Screen keys --------------------------------------------
//
// A KEY-FIRST VIEW ONTO /etc/shortcuts.conf, not a second copy: each of
// the three keys belongs to one of the four screenshot actions there
// (lib/ushortcuts.h), and choosing what a key does moves it between
// their binding lists. Other bindings on those actions stay as they are.

enum { SHOT_KEY_BAR, SHOT_KEY_SCREEN, SHOT_KEY_WINDOW, SHOT_KEY_REGION, SHOT_KEY_NONE,
       SHOT_KEY_CHOICES };
#define SHOT_KEYS 3
extern const char *const SHOT_KEY_COMBO[SHOT_KEYS];      // "Print Screen", ...
extern const char *const SHOT_KEY_LABEL[SHOT_KEY_CHOICES];
void shot_keys_get(int which[SHOT_KEYS]);
int  shot_keys_set(const int which[SHOT_KEYS]);

// --- the Options window (shot_prefs.c) ---------------------------------

// Opens it over `a` on `c` (copied); `on_commit` gets the result on OK.
void shot_prefs_open(struct uapp *a, const struct shot_conf *c, int page,
                     void (*on_commit)(const struct shot_conf *next));
int  shot_prefs_is_open(void);
enum { SHOT_PAGE_CAPTURE, SHOT_PAGE_AFTER, SHOT_PAGE_SAVING, SHOT_PAGES };

#endif
