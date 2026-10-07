#ifndef UOPEN_H
#define UOPEN_H

#include "lib/ulaunch.h"

// uopen -- which program opens this file?
//
// FREEDESKTOP'S SPLIT, WITHOUT THE DAEMON. Two layers, asked in order:
//
//   1. /etc/mimeapps.conf -- the USER's choice per extension:
//      `.jpg=imgview`, where the value names a DESKTOP ENTRY (the
//      filename stem under /usr/wm/applications), or a literal /path as the
//      escape hatch, or `-` for "cleared". mimeapps.list's
//      [Default Applications] role. `/bin/open -s` edits it.
//   2. `Handles=` on the .desktop entries -- what each app DECLARES,
//      shipped with the app (mimeinfo.cache's role, scanned live:
//      at this scale a directory read IS the cache).
//
// Resolution is IN-PROCESS. Linux and KDE run no daemon for this
// either; macOS's LaunchServices is the outlier, and a service here
// would need query IPC this OS does not have, to answer what a
// directory scan answers. See docs/decisions.md.
//
// Extensions match WHOLE and case-insensitively, INCLUDING the dot
// (".md" never claims ".mdx").

#define UOPEN_CONF "/etc/mimeapps.conf"

// FREEDESKTOP'S SECTION NAMES, READ BUT NOT WRITTEN. Both files are
// read from their section first and then from the top level, so a
// mimeapps.list or a .desktop copied off a Linux box -- where the
// header is mandatory -- resolves here unchanged.
//
// `open -s` still WRITES the flat form. The alternative is a machine
// whose existing flat file gains a sectioned duplicate of every key it
// already holds, correct to read and untidy forever; a migration is not
// worth it for a file the user can also edit by hand.
#define UOPEN_CONF_SECTION  "Default Applications"
#define UOPEN_ENTRY_SECTION "Desktop Entry"

// Does a space/comma-separated extension list contain `ext`? Matched
// WHOLE and case-insensitively, so ".md" never matches ".mdx" -- the
// thing a substring search gets wrong. `ext` includes the dot.
//
// Exported because it is the rule `Handles=` is written in, and a
// second caller now needs it (a file chooser's "Text files" row). A
// copy would be a second answer to "is this extension in this list".
int uopen_ext_matches(const char *list, const char *ext);

// The program that opens `path`, written into `exec`. Returns 1, or 0
// when nothing claims the extension (or there is none). An override
// naming an entry that no longer exists falls through to the
// declarations rather than failing -- a stale choice must not make a
// type unopenable.
int uopen_resolve(const char *path, char *exec, int cap);

// --- the decision a double-click makes -----------------------------------
//
// ONE ANSWER to "what happens when this is opened", which the File
// Manager, the desktop (through /bin/open) and `open` all act on, so the
// three cannot drift: a program or script is RUN (lib/ulaunch.h) -- an
// app at once, the rest by the user's policy, asking when there is none
// or it cannot run as it is; a folder opens in the File Manager; anything
// else opens with the app its extension resolves to. Only the ACTING
// differs per caller: one with a window shows the card itself.

enum uopen_how {
    UOPEN_NOTHING = 0,  // nothing opens it
    UOPEN_WITH,         // `exec` opens it
    UOPEN_RUN,          // run it: `act` is an ULAUNCH_* act
    UOPEN_ASK,          // a program or script to ask about (uui_runask)
    UOPEN_FOLDER,       // a directory: the File Manager
};

struct uopen_decision {
    int how;                   // enum uopen_how
    int act;                   // UOPEN_RUN's ULAUNCH_* act
    char exec[256];            // UOPEN_WITH's program
    struct ulaunch_info info;  // UOPEN_RUN / UOPEN_ASK: what it is
};

// Fills `d` and returns d->how. Reads the file's first bytes.
int uopen_decide(const char *path, struct uopen_decision *d);

// The argv that carries `d` out for `path`, into `argv` (5 slots); an ASK
// becomes /bin/wm/system/runask's. 0 for UOPEN_NOTHING.
int uopen_decision_argv(const char *path, const struct uopen_decision *d, char **argv);

// Resolve and spawn, NOT waited for -- an opener that waited would
// freeze its caller for as long as the editor stays open. A DIRECTORY
// opens in the File Manager. A PROGRAM OR SCRIPT runs instead, by
// lib/ulaunch.h's policy -- asking through /bin/wm/system/runask unless
// the user chose. Returns the pid, or a negative value.
int uopen_spawn(const char *path);

// --- choosing what opens a type (Properties' "Opens with") ---------------

struct uopen_app {
    char entry[48];   // the desktop entry's stem: "imgview"
    char name[48];    // its Name=: "Image Viewer"
    char exec[64];
};

// The apps whose Handles= claims `path`'s extension, in entry order, then
// every other app that opens files of some kind (any Handles=), plus
// whatever opens it now when that is none of them. `*current` is the
// index of the one that opens it now, -1 for none. Returns the count.
int uopen_apps_for(const char *path, struct uopen_app *out, int max, int *current);

// Make `entry` open every file with `path`'s extension -- the override
// `open -s` writes. 0, or -1 when the path has no extension or the file
// cannot be written.
int uopen_set_default(const char *path, const char *entry);

#endif
