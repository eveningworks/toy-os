#ifndef UUI_DIALOG_H
#define UUI_DIALOG_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"
#include "ui/uui_widget.h"

// A MODAL over the app's own window: a title, some lines, and a row of
// buttons. Qt's QMessageBox, GTK's GtkMessageDialog, Win32's
// MessageBox -- the control every toolkit has because every app
// eventually has to ask something.
//
// **IT IS DRAWN IN THE APP'S OWN WINDOW**, and that is not a shortcut:
// a TWP client draws into its own buffer and nothing else (enforced --
// see docs/gui-guidelines.md), so there is no such thing as a dialog
// window a client can open. Notepad and the File Manager each drew
// their own before this; the WM's `confirm_dialog.c` is the
// compositor's own chrome and not reachable from a client.
//
// **IT IS AN OVERLAY, so the router offers it every press FIRST**
// (`overlay_active`). A modal that could be clicked past is not a
// modal, and the widget underneath one is exactly what a stray click
// would otherwise reach.
//
// The commit is PARKED and taken, `uui_menubar`'s arrangement: the ops
// table's release slot can only say "something changed", not which
// button, so the app collects the code afterwards.
//
// **IT CAN CARRY A BODY** -- one item, usually a `uui_layout`, placed
// between the rows and the button row: QDialog::setLayout(), GtkDialog's
// content_area. That is what makes a file dialog a dialog with a
// fileview in it rather than a second modal. With a body the widget is
// a CONTAINER (`children`), and the router routes the body's items
// before the dialog's own press swallows the rest (ui/uui_route.h). Two
// things follow. A container has no overlay pass, so it is drawn in the
// ITEMS pass -- declare it LAST in the widget array to keep it on top.
// And keys go to `focus` first (a fileview, a field), then to the
// buttons; the app moves that focus when the router names a body item
// on a press, since the router names the child and not the container.

#define UUI_DIALOG_BUTTONS 6
#define UUI_DIALOG_ROWS    6

struct uui_dialog_button {
    const char *label;
    int code;          // handed back by uui_dialog_take_code()
};

struct uui_dialog {
    int x, y, w, h;    // the box, content-relative; derived when opened

    // Where the box may be placed -- the app's content area. Set it
    // whenever the window resizes; the box centres itself inside.
    int bx, by, bw, bh;

    // **DRAGGED BY ITS TITLE STRIP, AS AN OFFSET FROM CENTRE.** Not an
    // absolute position: the box re-centres on every resize and on
    // every re-layout, so storing x/y would be overwritten by the next
    // layout() and the box would jump back. The offset survives both
    // and is clamped so the box can never leave the bounds.
    int off_x, off_y;
    int dragging, grab_dx, grab_dy;

    const char *title;
    const char *rows[UUI_DIALOG_ROWS];   // caller-owned, NULL ends it
    int row_count;

    struct uui_dialog_button buttons[UUI_DIALOG_BUTTONS];
    int button_count;

    // The button a Return commits and an Escape's answer. -1 for none;
    // a dialog with no default still commits on a click.
    int default_button;
    int cancel_code;

    // The body; NULL is a plain message box. `body_w`/`body_h` is the
    // room it is given, font-derived by the caller and clamped to the
    // bounds. `focus` is the body item keys reach first, or NULL.
    struct uui_item *body;
    int body_w, body_h;
    struct uui_item *focus;

    int open;
    int hot;           // hovered or arrowed-to button, -1 for none
    int pressed;       // armed by a press, committed by the release
    int committed;     // parked code, -1 when there is nothing waiting
};

void uui_dialog_init(struct uui_dialog *d);
void uui_dialog_set_bounds(struct uui_dialog *d, int x, int y, int w, int h);

// Opens it. `rows` are caller-owned and must outlive the dialog being
// up -- point them at the app's own buffers, as the File Manager does.
void uui_dialog_open(struct uui_dialog *d, const char *title,
                      const char *const *rows, int row_count,
                      const struct uui_dialog_button *buttons, int count,
                      int default_button, int cancel_code);
void uui_dialog_close(struct uui_dialog *d);
int  uui_dialog_is_open(const struct uui_dialog *d);

// Set before open(); it stays until set again. NULL clears it.
void uui_dialog_set_body(struct uui_dialog *d, struct uui_item *body, int w, int h);
// Moves the key focus to a body item (NULL: the buttons), telling the
// old and new items through their set_focused ops.
void uui_dialog_focus(struct uui_dialog *d, struct uui_item *it);
// Where the body was placed. 0 when there is none or it is closed.
int  uui_dialog_body_rect(const struct uui_dialog *d, int *x, int *y, int *w, int *h);

// The code a commit produced, or -1. TAKEN, so a second call gets -1
// rather than acting twice.
int  uui_dialog_take_code(struct uui_dialog *d);

void uui_dialog_draw(struct ugfx_surface *s, const struct uui_dialog *d);

// The `focus` body item is offered the key first. Then Return commits
// the hot (else default) button, Escape answers `cancel_code`,
// Left/Right and Tab move between buttons. Returns 1 if the key was
// consumed -- which is EVERY key while it is open, because a modal that
// let a keystroke through to what is behind it is not a modal.
int  uui_dialog_key(struct uui_dialog *d, int key);

extern const struct uui_widget_ops uui_dialog_ops;

#endif // UUI_DIALOG_H
