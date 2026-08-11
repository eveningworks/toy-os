#ifndef CONFIRM_DIALOG_H
#define CONFIRM_DIALOG_H

// A reusable Yes/No modal popup -- same peer-file pattern as
// start_menu.h/.c and context_menu.h/.c (screen-absolute, drawn
// directly over everything, own small hit-testing, state shared with
// the rest of the WM through wm_internal.h). Genuinely generic, same
// spirit as context_menu.h: any WM-level action that's destructive or
// hard to undo can open one with its own message and yes/no callbacks,
// not just the one caller that motivated building it.
//
// First real caller: the Start menu's "Exit to shell" (start_menu.c),
// per docs/roadmap.md's shutdown item -- that entry's own design notes
// call for exactly this (a Yes/No confirm, built as a reusable piece,
// not one-off code in wm.c) even though the item as a whole is about
// a future Shutdown action that still needs a real poweroff mechanism
// (ACPI, not built yet) before it can exist. Confirming "Exit to
// shell" doesn't need that -- it's the first small, real, immediately
// useful place this dialog can be exercised and proven correct ahead
// of Shutdown eventually reusing it.
//
// Deliberately minimal, same philosophy as every popup in this
// codebase: one message, two buttons, click-to-choose,
// click-elsewhere-does-nothing (unlike a context menu, an open confirm
// dialog is modal -- it stays open until Yes or No is actually
// clicked, since silently discarding "are you sure?" on a stray click
// would defeat the point). No keyboard shortcuts (Enter/Esc) yet --
// add only once a real need shows up, same bar every widget here uses.

// Whether a confirm dialog is currently open -- read by wm_render.c
// (draw or not) and wm_input.c (route a left-click here first, before
// anything else, while open -- this is the most modal overlay in the
// WM, so it gets first refusal ahead of even the Start/context menus).
extern int confirm_dialog_open;

// Opens a dialog showing `message` (NOT copied -- caller's string must
// outlive the dialog, same ownership rule context_menu_open_at()'s
// `items` has; in practice this means a `static const char *` or a
// string literal, never a stack buffer). Centered on screen. `on_yes`
// is called if Yes is clicked; `on_no` may be NULL (No/click-elsewhere
// then just closes the dialog with no further action).
void confirm_dialog_open_with(const char *message, void (*on_yes)(void), void (*on_no)(void));

// Draws the dialog at its centered position -- a no-op if
// confirm_dialog_open is 0 (same "caller still checks, this just
// draws" contract as start_menu_draw()/context_menu_draw()).
void confirm_dialog_draw(void);

// Handles a left-click at (mx, my) while the dialog is open: runs
// on_yes()/on_no() if the click landed on the matching button, and
// closes either way (a click anywhere else while it's open is
// swallowed, not passed through -- see this file's top comment on why
// it's modal). Returns 1 if the dialog was open (so wm_input.c knows
// to stop right there, same contract context_menu_handle_click() and
// start_menu_handle_click() already have); returns 0 if it wasn't open
// at all, so the caller keeps routing the click normally.
int confirm_dialog_handle_click(int mx, int my);

#endif
