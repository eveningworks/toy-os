#ifndef UUI_FINDBAR_H
#define UUI_FINDBAR_H

#include "ui/uui_textbox.h"
#include "ui/uui_widget.h"

// uui_findbar -- a find control: a lens, a query field, an "N of M"
// readout, previous / next, and close. The shape of Konsole's search
// bar and Windows Terminal's find box.
//
// **THE APP SEARCHES; THE WIDGET ONLY ASKS.** It knows nothing about
// what is being searched: it reports what the person did as an EVENT
// (uui_findbar_take()) and the app answers by setting `matches` and
// `current`, which is all the readout draws. A terminal's scrollback and
// a document are searched differently, and neither belongs in here.
//
// **IT DOES NOT POSITION ITSELF.** A docked bar and a floating pill are
// the same control; `pill` only changes the background. Where it sits is
// the app's.
//
// Keys reach it through the app (uui_findbar_key()), not focus routing,
// because an app with a find bar is usually also the one consuming every
// other key -- a terminal sends them to a shell.

enum uui_findbar_event {
    UUI_FIND_NONE = 0,
    UUI_FIND_CHANGED,   // the query text changed
    UUI_FIND_NEXT,      // Enter, or the down button
    UUI_FIND_PREV,      // Shift+Enter, or the up button
    UUI_FIND_CLOSE,     // Esc, or the close button
};

struct uui_findbar {
    int x, y, w, h;
    struct uui_textbox field;   // OWNED; read the query with uui_findbar_query()

    // Set by the APP after each search. `current` is 1-based; 0 with
    // matches > 0 means "matches exist, none chosen yet".
    int matches;
    int current;

    int pill;                   // floating: a rounded card with an outline

    int hovered;                // button under the pointer (UUI_FB_*), or -1; OWNED
    int pressed;                // button armed by a press, or -1; OWNED
    int event;                  // parked for uui_findbar_take(); OWNED
};

// The three buttons, for a test that clicks one by rect.
enum { UUI_FB_PREV = 0, UUI_FB_NEXT, UUI_FB_CLOSE, UUI_FB_BUTTONS };

void uui_findbar_init(struct uui_findbar *f);
void uui_findbar_set_geometry(struct uui_findbar *f, int x, int y, int w, int h);
void uui_findbar_natural_size(const struct uui_findbar *f, int *out_w, int *out_h);

// Focus the field (caret on, text selected) -- what Ctrl+Shift+F does
// to a find box that is already open, as every editor's does.
void uui_findbar_activate(struct uui_findbar *f);
const char *uui_findbar_query(const struct uui_findbar *f);

// A key while the bar is open. Returns 1 if it was consumed; the event
// it caused, if any, is parked for uui_findbar_take().
int uui_findbar_key(struct uui_findbar *f, int key, unsigned mods);

// The last event, TAKEN, so one press is not acted on twice.
enum uui_findbar_event uui_findbar_take(struct uui_findbar *f);

int uui_findbar_button_rect(const struct uui_findbar *f, int which,
                            int *x, int *y, int *w, int *h);

extern const struct uui_widget_ops uui_findbar_ops;

#endif
