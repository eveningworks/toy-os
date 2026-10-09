#ifndef UUI_EDITMENU_H
#define UUI_EDITMENU_H

#include "ui/uui_edit.h"
#include "ui/uui_widget.h"

// uui_editmenu -- THE right-click menu of every editable text: Undo,
// Redo / Cut, Copy, Paste, Delete / Select All, run through the edit
// core on whatever widget answered uui_widget_ops.edit_target.
//
// ONE PER PROCESS, OWNED BY uapp, never declared by an app: the Win32
// EDIT control and Qt's QLineEdit carry this menu built in, and an app
// that wanted one had to write it -- so only Notepad had one. uapp opens
// it on the secondary release over editable text (or the Menu key /
// Shift+F10 on the focused field), and routes it through the router's
// `extra` item (ui/uui_route.h) while it is up.
//
// What a row may do is asked of the target as the menu draws: a masked
// field greys Cut and Copy, read-only text drops every editing row, an
// empty clipboard greys Paste. The menu is a uui_menubar with no bar,
// so it looks and behaves like every other menu here.

// The id the router names when the menu took an event. Never an app's:
// uapp catches it before on_widget.
#define UUI_EDITMENU_ID 0x7EDE0001

// The item to hang on a router's `extra`.
struct uui_item *uui_editmenu_item(void);

// Opens the menu for `t` at (x, y) inside a bounds rect (the window's
// content). `target_id` is the router id of the widget, told the
// change after a command (0 for one the router does not know).
void uui_editmenu_open(const struct uui_edit_target *t, int target_id,
                       int x, int y, int bw, int bh);
int  uui_editmenu_is_open(void);
void uui_editmenu_close(void);

// After the router named UUI_EDITMENU_ID: runs the row a release
// committed, if any. Returns 1 when the TEXT changed and fills the
// target's id, so the app can be told as if it had been typed.
int uui_editmenu_take(int *out_target_id);

// A key while the menu is open: arrows, Enter, Esc. 0 = not taken; 1 =
// taken; 2 = taken, committed a row and changed the text (*out_target_id).
int uui_editmenu_key(int key, int *out_target_id);

#endif
