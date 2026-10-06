#ifndef ULIB_UFILEUNDO_H
#define ULIB_UFILEUNDO_H

// AN UNDO JOURNAL FOR FILE OPERATIONS: what a move, copy, rename, new
// item or delete-to-the-bin did, and how to take it back -- Explorer's
// Ctrl+Z/Ctrl+Y over file operations, Dolphin's KIO undo manager.
//
// One OPERATION is what the user did once (moving three files is one,
// of three steps). The journal is a stack with a cursor: Undo steps the
// cursor back, Redo forward, and recording a new operation drops what
// could have been redone, as every editor's undo does.
//
// **WHAT IS REVERSED, AND HOW** -- each kind has one inverse, and the
// way back from a thing that MADE something is the Recycle Bin, never a
// permanent delete (Explorer's rule: undoing a copy recycles the copy):
//
//   MOVE    a -> b       undo: b back to a      redo: a to b again
//   RENAME  a -> b       the same, always a rename
//   COPY    a -> b       undo: b to the bin     redo: b back out of it
//   CREATE  b            undo: b to the bin     redo: b back out of it
//   TRASH   a -> bin     undo: out of the bin   redo: a to the bin again
//
// A step that cannot be reversed -- its source is gone, something now
// has its name -- fails ALONE and is reported; the rest still run. A
// permanent delete is not recorded: there is nothing to take back.
//
// APPLYING is ufileundo_apply(), one step at a time, so a caller with a
// worker thread (the File Manager) can run a long cross-volume move
// there with a ufileop for progress; the journal itself is plain data
// and the caller's to own -- hundreds of KB, so static.

#include "lib/ufileop.h"

#define UFU_PATH  256
#define UFU_STEPS 64     // one operation's items -- the File Manager's queue
#define UFU_DEPTH 8      // operations kept

enum ufu_kind { UFU_MOVE = 1, UFU_RENAME, UFU_COPY, UFU_CREATE, UFU_TRASH };

struct ufu_step {
    char a[UFU_PATH];    // where it came from ("" for CREATE)
    char b[UFU_PATH];    // where it is after the operation
    char bin[UFU_PATH];  // where it is in the bin, while it is there
};

struct ufu_op {
    int kind;
    int count;
    char label[64];      // "Move 3 items to Pictures", for a menu or a toast
    struct ufu_step step[UFU_STEPS];
};

struct ufileundo {
    struct ufu_op op[UFU_DEPTH];
    int count;           // operations held
    int at;              // how many of them are DONE; the rest can be redone
};

// Start recording an operation (it is not undoable until committed).
// Drops anything that could have been redone.
struct ufu_op *ufileundo_begin(struct ufileundo *u, int kind, const char *label);
// One step of the operation being recorded. 0 when it is full.
int ufileundo_add(struct ufu_op *op, const char *a, const char *b, const char *bin);
// Keep it -- or drop it, when it ended up with no steps.
void ufileundo_commit(struct ufileundo *u, struct ufu_op *op);

// The operation Undo (or Redo) would act on, or NULL.
struct ufu_op *ufileundo_next_undo(struct ufileundo *u);
struct ufu_op *ufileundo_next_redo(struct ufileundo *u);

// Reverse (`redo` 0) or repeat (`redo` 1) step `i` of `op`. 0, or -errno
// for that step alone. `s`/`p` serve a move that has to copy.
int ufileundo_apply(struct ufu_op *op, int i, int redo,
                    struct ufileop *s, const struct ufileop_policy *p);
// The operation has been applied: move the cursor.
void ufileundo_done(struct ufileundo *u, int redo);

#endif
