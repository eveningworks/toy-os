#ifndef CONFIRM_DIALOG_H
#define CONFIRM_DIALOG_H

#include <stdint.h>

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
// called for exactly this (a Yes/No confirm, built as a reusable piece,
// not one-off code in wm.c). Second caller, same file: "Shutdown",
// added once system_poweroff() (kernel/core/power.c) gave it something
// real to confirm into.
//
// Deliberately minimal, same philosophy as every popup in this
// codebase: one message, two buttons, and modal -- unlike a context
// menu it stays open until Yes or No is actually chosen, since silently
// discarding "are you sure?" on a stray click would defeat the point.
//
// Its buttons are a real ui_button_group (apps/ui/), not hand-drawn
// rectangles. They were hand-drawn until an audit against
// docs/gui-guidelines.md: they had no hover state, no pressed state, and
// acted on `handle_click`, which the WM fires on button-DOWN -- so the
// Shutdown confirmation could not be cancelled by pressing Yes and
// dragging off, and gave no feedback that it had heard the press at
// all. Adopting the group fixed all three at once and deleted the
// geometry, which is the argument for the group existing.
//
// No keyboard shortcuts (Enter/Esc) yet -- add only once a real need
// shows up, same bar every widget here uses.

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

// Same, with the buttons named. "Yes"/"No" is right for a question and
// wrong for a choice between two actions -- the force-quit dialog asks
// "Force Quit" or "Wait", and answering that with Yes/No would make the
// user work out which is which. Windows and KDE both label these
// buttons with the verbs.
//
// The labels are NOT copied, same ownership rule as `message`. They
// last until the next dialog opens; confirm_dialog_open_with() resets
// them to Yes/No, so an ordinary dialog can never inherit them.
void confirm_dialog_open_labelled(const char *message, const char *yes, const char *no,
                                   void (*on_yes)(void), void (*on_no)(void));

// Button 0 (yes/affirmative) or 1 (no), and the open dialog's message.
// For the debug console, and therefore for tests -- see the note in the
// .c on why scanning for them by colour was worse. 0 if nothing is open.
int confirm_dialog_button_rect(int index, int *x, int *y, int *w, int *h,
                                const char **label);
const char *confirm_dialog_message(void);

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

// Called every tick while the dialog is open, with the live cursor
// position and button state -- the same shape (and the same reason)
// wm_update_title_btn_press() has. This is where a press is tracked and
// where it COMMITS, on release over the button it armed; handle_click()
// above only swallows the click, so that a press dragged off and
// released does nothing at all (docs/gui-guidelines.md).
void confirm_dialog_update_press(int mx, int my, uint8_t buttons);

// Hover, delivered while nothing is held. Returns 1 if the highlight
// changed and a repaint is needed -- the same contract gui_apps.h's
// on_hover has, for the same "don't repaint every tick" reason.
int confirm_dialog_update_hover(int mx, int my);

#endif
