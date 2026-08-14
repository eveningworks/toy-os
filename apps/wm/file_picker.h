#ifndef FILE_PICKER_H
#define FILE_PICKER_H

#include <stdint.h>

// A reusable Open/Save file-browser dialog -- same peer-file pattern as
// confirm_dialog.h/context_menu.h/start_menu.h (screen-absolute, drawn
// directly over everything, own state shared with the rest of the
// window manager through wm_internal.h). Genuinely generic: ANY app
// (Notepad today, any future one) opens it with its own title/starting
// directory/callback and gets a real, navigable Windows/KDE-style
// picker -- directories first then files, double-click to enter a
// folder or choose a file, an editable filename field, Open/Cancel or
// Save/Cancel buttons depending on mode. Backed directly by fs_list()/
// fs_is_dir()/fs_exists() (fs.h) -- GUI apps run in kernel space and
// already call fs_* directly (see notepad.c's own Save/Load), so no
// syscall layer is needed here either.
//
// First real caller: apps/notepad.c's Save/Load, replaced by Save
// As.../Open... buttons that pop this instead of Notepad's old
// always-visible inline filename field -- see docs/decisions.md.
//
// Deliberately scoped down from a "real" file manager, same philosophy
// as every popup in this codebase -- built now:
//   - full directory navigation (double-click a folder to enter it, a
//     ".." row to go up when not at root)
//   - directories-first, alphabetical listing, with a scrollbar for
//     more entries than fit
//   - typing an absolute or cwd-relative path directly into the
//     filename field also works, same as a real dialog's field
// deliberately NOT built this round, add only once a real need shows
// up (see this file's own top-of-.c comment for exactly why each is
// missing rather than just forgotten):
//   - Esc-to-cancel (the physical Escape key isn't wired to a scancode
//     in kernel/drivers/keyboard.c at all yet -- confirm_dialog.h hit
//     the same gap first)
//   - drag-to-scroll on the list's scrollbar thumb, or mouse-wheel
//     scrolling (the WM doesn't route wheel events to a screen-level
//     modal today, only to the focused window) -- click-to-page above/
//     below the thumb still works
//   - creating a new directory from inside the dialog
//   - hover highlighting on list rows (context_menu.c's popup doesn't
//     have this either)

enum file_picker_mode {
    FILE_PICKER_OPEN, // "Open" button; typed/selected name must already exist as a file
    FILE_PICKER_SAVE, // "Save" button; typed/selected name may be new
};

// Whether the picker is currently open -- read by wm_render.c (draw or
// not) and wm.c/wm_input.c (route clicks/keys here first while open,
// same "most modal" priority confirm_dialog_open already has).
extern int file_picker_open;

// Opens the picker in `mode`, showing `title` (NOT copied -- same
// ownership rule as confirm_dialog's `message`: a string literal or a
// `static const char *`, never a stack buffer), listing `start_dir`
// initially (an absolute path, e.g. "/" or "/docs" -- falls back to "/"
// if it doesn't resolve to a real directory) with `initial_name`
// pre-filled in the filename field (may be "" or NULL). `on_choose` is
// called with the final resolved absolute path once the user commits
// (the Open/Save button, double-clicking a file row, or Enter in the
// filename field) -- the picker closes itself first, so `on_choose` is
// free to open another modal of its own. `on_cancel` (may be NULL) is
// called if Cancel is clicked instead; either way the picker closes.
void file_picker_open_with(enum file_picker_mode mode, const char *title,
                            const char *start_dir, const char *initial_name,
                            void (*on_choose)(const char *path),
                            void (*on_cancel)(void));

// Draws the dialog at its centered position -- a no-op if
// file_picker_open is 0, same "caller still checks, this just draws"
// contract as confirm_dialog_draw().
void file_picker_draw(void);

// Handles a left-click at (mx, my) while the picker is open: the
// filename field, a list row (single click selects/fills the field,
// double click enters a folder or chooses a file), the scrollbar's
// track, or the Open/Save/Cancel buttons. Returns 1 if the picker was
// open (so wm_input.c knows to stop routing the click any further,
// same contract confirm_dialog_handle_click() has) regardless of
// whether the click landed on anything in particular -- modal, so any
// stray click is swallowed, not passed through. Returns 0 if the
// picker wasn't open at all.
int file_picker_handle_click(int mx, int my);

// Called every tick while the picker is open, with the live cursor and
// button state -- the same shape wm_update_title_btn_press() and
// confirm_dialog_update_press() have. Its Open/Save and Cancel buttons
// arm on press and COMMIT on release over the button they armed, so a
// press dragged off and released does nothing (docs/gui-guidelines.md).
void file_picker_update_press(int mx, int my, uint8_t buttons);

// Hover for those buttons, while nothing is held. Returns 1 if the
// highlight changed and a repaint is needed.
int file_picker_update_hover(int mx, int my);

// Handles one key while the picker is open: routed to the filename
// field if it's active (typing, backspace, arrow keys -- same
// ui_textbox_key() contract every other text field in this codebase
// uses), with Enter committing exactly like clicking Open/Save.
// Returns 1 if the picker was open (so wm.c's main loop knows NOT to
// also deliver this key to the focused window behind it -- modal
// keyboard capture, same idea as the click-routing priority above);
// returns 0 if the picker wasn't open.
int file_picker_handle_key(int key);

#endif
