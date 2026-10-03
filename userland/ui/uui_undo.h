#ifndef UUI_UNDO_H
#define UUI_UNDO_H

// uui_undo -- an edit HISTORY for editable text: what Ctrl+Z and Ctrl+Y
// step through, for every buffer the shared edit core (ui/uui_edit.h)
// drives. It belongs to the text, as GtkTextBuffer's and QTextDocument's
// undo stacks do, not to an app: Notepad, a field in a dialog and the
// Boot Manager's editor get it by pointing `uui_edit.undo` here.
//
// A LOG OF OPERATIONS, NOT SNAPSHOTS. Each record is "inserted these
// bytes at P" or "erased these bytes from P"; undo applies the inverse,
// redo the record again. A snapshot per step is what the kernel line
// editor keeps (klineedit.c), and is right for a 256-byte line and wrong
// for a 4 MB document.
//
// THE CALLER OWNS THE STORAGE (utext's rule): a history fills its
// buffer and then FORGETS ITS OLDEST STEPS, whole steps at a time. One
// edit bigger than the whole buffer -- deleting a selected 4 MB file --
// empties the history rather than half-recording it.
//
// STEPS, NOT KEYSTROKES. Typing a word is one step (a run of inserts
// coalesces until a space follows a non-space), so is a run of
// Backspaces, and so is anything bracketed by begin/end -- a paste over
// a selection is an erase and an insert that undo together. Moving the
// caret ends a run (uui_undo_break()).
//
// CLEAN is a POSITION in the history: the state the file on disk has.
// Undoing back to it makes the document clean again, as in every editor;
// an edit made after undoing past it makes clean unreachable.

#include "ui/uui_edit.h"

struct uui_undo {
    unsigned char *buf;  // the caller's; NULL = no history
    int cap;
    int used;            // bytes of records, undoable then redoable
    int at;              // [0, at) can be undone, [at, used) redone
    int clean;           // `at` when the text matched the file; -1 = gone
    int depth;           // begin/end nesting
    int fresh;           // the next record starts a new step
    int run;             // the last record may be extended by typing
};

void uui_undo_init(struct uui_undo *u, void *storage, int cap);
// Forgets everything and calls the present text clean -- a load, a New.
void uui_undo_reset(struct uui_undo *u);

// Record an edit that has HAPPENED (insert) or is ABOUT to happen
// (erase -- the bytes must still be readable). `pos` is where.
void uui_undo_note_insert(struct uui_undo *u, int pos, const char *s, int n);
void uui_undo_note_erase(struct uui_undo *u, int pos, const char *s, int n);
// The same, reading the `n` bytes at `pos` through `ops->at`.
void uui_undo_note_erase_from(struct uui_undo *u, int pos, int n,
                              const struct uui_edit_ops *ops, void *text);

// Everything recorded between these is one step. They nest.
void uui_undo_begin(struct uui_undo *u);
void uui_undo_end(struct uui_undo *u);
// The next edit starts a new step even if it would have coalesced.
void uui_undo_break(struct uui_undo *u);

int uui_undo_can_undo(const struct uui_undo *u);
int uui_undo_can_redo(const struct uui_undo *u);

// Applies one step backwards (redo = 0) or forwards through `ops`, which
// must NOT record (the raw storage ops), and puts the caret where the
// step happened. Returns 1 if there was a step.
int uui_undo_apply(struct uui_undo *u, int redo, const struct uui_edit_ops *ops,
                   void *text, int *cursor);

void uui_undo_mark_clean(struct uui_undo *u);
int  uui_undo_is_clean(const struct uui_undo *u);

#endif
