#ifndef UUI_ACTIONLIST_H
#define UUI_ACTIONLIST_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_widget.h"

// ROWS OF THINGS, EACH WITH ITS OWN BUTTONS: a title, a dim line under
// it, and up to two buttons at the right of the row -- "Disconnect" on a
// remote session, "Remove" on a trusted address. Windows 11 Settings'
// list of paired devices, GNOME's list of shared folders.
//
// One widget, not a row of widgets per entry, so a list that changes
// length (sessions come and go) is a count and a strings array rather
// than items to create and lay out again. A press arms a button and the
// release over the same one fires it: the app hears its id
// (on_widget) and reads `fired_row` / `fired_btn`.
//
// THE CALLER OWNS THE ROWS and may rewrite them between frames; the
// widget keeps only which row and button are hovered or pressed, and
// forgets a press whose row has gone.
#define UUI_ACTIONLIST_BTNS 2

struct uui_actionlist_row {
    char title[64];
    char sub[96];
    const char *btn[UUI_ACTIONLIST_BTNS];   // NULL for no button there
    int primary;                            // which button is filled in the accent; -1 none
};

struct uui_actionlist {
    int x, y, w, h;
    struct uui_actionlist_row *rows;        // caller-owned
    int count;
    const char *empty;                      // shown when count is 0; may be NULL
    int hover_row, hover_btn;               // OWNED
    int press_row, press_btn;               // OWNED
    int fired_row, fired_btn;               // the last button released over; -1 before
};

void uui_actionlist_init(struct uui_actionlist *l, struct uui_actionlist_row *rows, int count,
                         const char *empty);
// The rect of row `row`'s button `btn`, content-relative; 0 if there is none.
int uui_actionlist_button_rect(const struct uui_actionlist *l, int row, int btn,
                               int *x, int *y, int *w, int *h);
// Draws the list into `s` at its geometry -- for a caller that paints
// without a router (the compositor's tray flyout).
void uui_actionlist_draw(struct ugfx_surface *s, const struct uui_actionlist *l, uint32_t bg);
// The same press/hover/release the ops do, for such a caller. Each
// returns 1 when the list changed (repaint); release returns 2 when a
// button fired.
int uui_actionlist_hover(struct uui_actionlist *l, int cx, int cy);
int uui_actionlist_press(struct uui_actionlist *l, int cx, int cy);
int uui_actionlist_release(struct uui_actionlist *l, int cx, int cy);

extern const struct uui_widget_ops uui_actionlist_ops;

#endif
