#ifndef UUI_RUNASK_H
#define UUI_RUNASK_H

#include "ui/uapp.h"
#include "lib/ulaunch.h"

// uui_runask -- "Run backup.sh?": the card that asks what to do with a
// program or a script before anything runs. KDE's "Open Executable
// File" dialog, with Windows' OpenWith.exe's arrangement for callers
// that have no window of their own (/bin/wm/system/runask).
//
// It says what the file IS (a script and its interpreter, or a program),
// shows a script's first lines, and offers Run in Terminal (the
// default: a person sees what it printed), Run (detached), Open in
// Notepad (a script) or Cancel. "Always do this" writes the policy
// ulaunch_policy() reads, so the next double-click does not ask.
//
// A file WITHOUT AN EXECUTE BIT gets the other face: it says so and
// offers "Allow running, and run", which sets the bit (ulaunch_allow)
// and runs it in a Terminal. A script whose interpreter is missing has
// its Run buttons disabled, with the reason on the card.
//
// The act is the CALLER's to carry out, through `done`, because only it
// knows how it tracks children; uui_runask_start() is the common way.

// The card's answer: an ULAUNCH_* act, or ULAUNCH_ASK for Cancel.
// `kind` is the classified kind. The policy is already saved when
// "Always" was ticked.
typedef void (*uui_runask_fn)(void *ctx, const char *path, int kind, int act);

// A MODAL WINDOW over `a`. 0 when one is open already or the compositor
// refused. `info` is copied.
int uui_runask_open(struct uapp *a, const char *path, const struct ulaunch_info *info,
                    uui_runask_fn done, void *ctx);
int uui_runask_is_open(void);

// The same card as an APP's main window, for a program that is only the
// question: fills `d`'s title, size, widgets, layout, focus and input
// hooks; the caller runs it. `done` is called once, after which the app
// should quit.
void uui_runask_app_desc(struct uapp_desc *d, const char *path, const struct ulaunch_info *info,
                         uui_runask_fn done, void *ctx);

// Spawns `act` on `path` and tracks the child on `a` (uapp_track_child).
// The pid, or negative.
int uui_runask_start(struct uapp *a, const char *path, int kind, int act);

#endif
