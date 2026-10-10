#ifndef UUI_KEYCAPTURE_H
#define UUI_KEYCAPTURE_H

#include "uui_widget.h"
#include "keycombo.h"

// A CONTROL THAT RECORDS A KEY COMBINATION BY HAVING YOU PRESS IT.
//
// Click it and it says "Press the new shortcut"; the next real
// combination becomes its value. That is what KDE's and GNOME's shortcut
// editors both do, and the reason is that the alternative -- a text box
// -- makes you know a spelling. Nobody guesses "Shift+Super+S" on the
// first try, and a typo in a text box is silent until the key does not
// work.
//
// **IT CANNOT WORK WITHOUT THE COMPOSITOR STANDING DOWN**, which is the
// one thing to know before using it. Global shortcuts are matched before
// any client sees a key (userland/wm/wm_shortcut.c), so while this is
// listening the app must hold uapp_inhibit_shortcuts() -- otherwise
// pressing Super+E to bind it launches a file manager instead, and the
// control can never record the combinations it exists for. `on_arm`
// below is how the app is told to do that.
//
// WHAT IT DOES NOT DO: it records ONE combination. A setting whose value
// lists several (the screenshot tool answers to two) is replaced by what
// is captured -- see kernel/lib/shortcut_actions.c, which is also the
// only place that list is authored today.

struct uui_keycapture;

// Armed or disarmed. The app MUST hold uapp_inhibit_shortcuts() for as
// long as `armed` is 1, and release it when it goes 0.
typedef void (*uui_keycapture_arm_fn)(void *ctx, int armed);

// A combination was captured, or capture was cancelled (`text` NULL).
typedef void (*uui_keycapture_done_fn)(void *ctx, const char *text);

struct uui_keycapture {
    int x, y, w, h;

    // The value, canonically spelled. Empty means unbound, which this
    // draws as "None" rather than as a blank box -- a blank control
    // reads as one that failed to load.
    char text[KEYCOMBO_TEXT_MAX];

    int armed;        // listening for the next combination
    int focused;
    int hovered;

    uui_keycapture_arm_fn  on_arm;
    uui_keycapture_done_fn on_done;
    void *ctx;
};

void uui_keycapture_init(struct uui_keycapture *k, const char *value);

// Replace the shown value without going through a capture.
void uui_keycapture_set(struct uui_keycapture *k, const char *value);

// Stop listening without recording anything. Safe when not armed.
void uui_keycapture_cancel(struct uui_keycapture *k);

extern const struct uui_widget_ops uui_keycapture_ops;

#endif
