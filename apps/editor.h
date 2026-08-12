#ifndef EDITOR_H
#define EDITOR_H

#include "ui/ui.h" // struct text_scrollback

// A small nano/pico-style full-screen text editor. Two surfaces share
// the same editing core (load/save/cursor-move/insert/delete, all just
// text_scrollback's own cursor-aware API from widgets.h -- see its top
// comment): the physical CLI (editor_run(), a classic blocking
// keyboard-read loop, invoked by the shell's `edit`/`nano` command) and
// the GUI Terminal app (apps/terminal.c), which can't block its own
// input loop the way the CLI can (see terminal.c's top comment on why
// interactive multi-keystroke commands are otherwise excluded there) --
// so it drives the exact same editor_handle_key() one keystroke at a
// time from its own non-blocking on_key callback instead, rendering the
// buffer with widget_scrollback_draw() (the ordinary GUI text widget,
// now cursor-aware) rather than editor_run()'s vga_* console redraw.
// Same data, same edit logic, two renderers -- the same split every
// other GUI-vs-CLI pair in this codebase already makes (shell.c's
// vga_write() vs notepad.c's gfx_draw_string()).
//
// Deliberately minimal (see CHANGELOG.md's build 377 entry for the
// choices this was built to): arrow-key/Home/End/Delete navigation and
// editing, F2 to save, F3 to exit -- no Ctrl-key shortcuts (real nano's
// Ctrl+O/Ctrl+X/Ctrl+K/...), no search, no cut/paste, no
// unsaved-changes prompt on exit. F2/F3 stand in for nano's Ctrl+O/
// Ctrl+X specifically because this build's keyboard driver has no
// Ctrl-key chording yet (see keyboard.h) -- adding that was explicitly
// scoped out in favor of a smaller keyboard-driver surface for this
// pass. Esc also exits, but only from the physical console -- it's
// NOT the documented/advertised key because apps/wm/wm.c intercepts
// Esc globally to leave the whole GUI desktop before any window's
// on_key callback ever runs, so a Terminal-embedded editor session can
// never actually receive it. F3 was picked specifically because it
// doesn't have that conflict on either surface.

// Loads `path` into `tb` (already widget_scrollback_init()'d by the
// caller) if it exists, leaves `tb` empty otherwise -- either way,
// `tb->cursor` ends up at 0 (the very start), not the end, since
// opening a file for editing should land the cursor where a person
// would expect to start reading/editing, unlike Notepad's Load (which
// keeps its append-only "cursor follows the write point" behavior).
void editor_load(struct text_scrollback *tb, const char *path);

// Flattens `tb` and writes it to `path` (fs_write(), fs.h) -- silently
// truncated to SCROLLBACK_CAP (editor.c's own in-RAM buffer size) if
// longer, the same exposure apps/notepad.c's own Save already has.
// Not bounded by any filesystem per-file limit -- TFS2 v2 has none
// (see fs.h's fs_write() doc comment); SCROLLBACK_CAP is the real
// ceiling here. Returns 1 on
// success, 0 if fs_write() itself failed (bad path, disk full).
int editor_save(struct text_scrollback *tb, const char *path);

// Handles one keystroke against `tb`: cursor movement (arrows/Home/
// End), Backspace/Delete, Enter, ordinary printable characters, F2
// (calls editor_save() and writes "Saved."/"Save failed." into
// `status`), and F3 or Esc (sets *out_should_exit, doesn't touch `tb`;
// see this header's top comment for why F3 -- not Esc -- is the one
// that's actually reliable on both surfaces). `status` is left
// untouched (not cleared) by any key other than F2 -- callers that want
// stale status cleared on the next keystroke should clear it themselves
// before checking whether this call changed it, the same convention
// apps/notepad.c already uses for its own status field.
void editor_handle_key(struct text_scrollback *tb, const char *path, int key,
                        int *out_should_exit, char *status, unsigned int status_max);

// The physical-console entry point: loads `path` (or starts a new,
// empty buffer if it doesn't exist yet), then blocks in its own
// keyboard-read loop -- full-screen redraw via vga_clear()+vga_write()
// on every keystroke, cursor shown as a single reverse-video character
// -- until F3 (or Esc, which also works here -- see this header's top
// comment) exits back to the calling shell. Registered as the
// `edit`/`nano` shell commands (see apps/shell.c); NOT available from
// inside the GUI Terminal (see this header's own top comment for why,
// and apps/terminal.c for how it gets equivalent editing there instead
// through editor_handle_key() directly).
void editor_run(const char *path);

#endif
