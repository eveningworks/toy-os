#ifndef ULIB_UFILEOP_H
#define ULIB_UFILEOP_H

#include <stdint.h>
#include "rt/sys.h"

// ufileop -- copying, moving and deleting files and trees, once.
//
// **THE PRIMITIVE AND THE WALK ARE SHARED; THE POLICY IS THE CALLER'S.**
// Windows shares `CopyFileEx` between its shell and its command line and
// lets each build its own engine on top; Linux shares nothing at all
// (coreutils' `copy.c`, GIO's `g_file_copy` and KIO's file worker are
// three unrelated implementations of copying a file on one machine).
// This shares more than Windows does, because at this scale a second
// copy loop is pure cost with no compatibility story to justify it.
//
// So `/bin/cp`, `/bin/mv` and `/bin/rm` are thin front ends over this,
// and so is the File Manager. What differs between them is POLICY --
// what to do when the destination exists, what to report while it runs,
// whether to stop -- and that arrives as callbacks. `cp` says
// "overwrite, silently, run to completion"; the File Manager says "ask
// me, and tell me about every file".
//
// **THE STATE IS THE CALLER'S, not this file's.** A GUI does this on a
// worker thread while its main thread is listing directories, and
// file-scope scratch shared between the two is the re-entrancy bug this
// codebase keeps rediscovering (`tfs3.c` carries a preemption guard for
// the kernel's version of it). `ttf.h` solved the same problem the same
// way and for the same reason: one implementation then serves a
// program, a thread and a test with no shared mutable state at all.
// `struct ufileop` is tens of KB, so it belongs at file scope or on a heap,
// never on a ring-3 frame (those are capped at 2 KiB).

#define UFILEOP_PATH_MAX 256   // NOT FS_PATH_MAX (4096): the walk's own bound
#define UFILEOP_BUF 4096       // one TFS3 block-ish
#define UFILEOP_MAX_DIRS 64    // pending directories in a tree walk

// What a caller decides when a destination already exists.
enum ufileop_decision {
    UFILEOP_OVERWRITE = 0, // replace it
    UFILEOP_SKIP,          // leave both alone, keep going
    UFILEOP_RENAME,        // write a new BASENAME into `rename_out`
    UFILEOP_CANCEL,        // stop the whole operation
};

// How an operation ended. A CANCEL is not a failure -- it is what the
// caller asked for -- and they are kept apart so a status line can say
// "cancelled" rather than "failed".
enum ufileop_result {
    UFILEOP_OK = 0,
    UFILEOP_FAILED,
    UFILEOP_CANCELLED,
};

struct ufileop_policy {
    void *ctx;

    // A destination exists. NULL means UFILEOP_OVERWRITE, which is what
    // `cp` has always done and what makes the /bin front ends one line.
    //
    // For UFILEOP_RENAME, write a basename (not a path) into
    // `rename_out`; ufileop_unique_name() produces the "file (1).txt"
    // form if that is what you want.
    int (*on_conflict)(void *ctx, const char *src, const char *dst,
                        char *rename_out, int cap);

    // Progress. `done`/`total` are BYTES within the current file, so a
    // caller can drive a bar; `path` is what is being worked on.
    // Return 0 to cancel. NULL means no progress and no cancelling.
    //
    // CALLED OFTEN -- once per buffer -- so a caller that repaints here
    // is a caller that will not finish. Record and let the frame do the
    // drawing, which is what the File Manager does.
    int (*on_progress)(void *ctx, const char *path,
                        uint64_t done, uint64_t total);

    // Something failed, by path and errno. The operation CONTINUES: one
    // unreadable file in a tree of a thousand should not abandon the
    // other 999, which is coreutils' behaviour too. NULL discards them,
    // and the overall result still says FAILED.
    void (*on_error)(void *ctx, const char *path, int err);
};

// The working state. Caller-owned; see the header comment for why.
struct ufileop {
    char buf[UFILEOP_BUF];
    struct sys_dirent entries[SYS_LISTDIR_MAX];

    // The tree walk's queue. BREADTH-FIRST OVER AN EXPLICIT QUEUE, not
    // recursion: one listing is 20 KB and a ring-3 frame holds 2 KiB, so
    // what goes on the queue is a PATH and one listing buffer serves
    // every level -- a directory is fully walked before the next pops.
    char qsrc[UFILEOP_MAX_DIRS][UFILEOP_PATH_MAX];
    char qdst[UFILEOP_MAX_DIRS][UFILEOP_PATH_MAX];
    int qhead, qtail;

    // Set when a policy callback asked to stop, so the walk unwinds
    // without every level having to check a return value.
    int cancelled;
    int failed;
};

// --- the operations ---------------------------------------------------
//
// `dst` may be a directory, in which case the source lands INSIDE it
// (`cp f /tmp` means `/tmp/f`) -- cp's oldest behaviour, and what a
// paste into a folder means too.

// Copies a file, or a whole tree when `src` is a directory.
int ufileop_copy(const char *src, const char *dst, struct ufileop *s,
                  const struct ufileop_policy *p);

// Renames when it can, and falls back to copy-then-delete when it
// cannot. **A rename across parents can be REFUSED on a v1 TFS3 volume**
// (five journal credits, four slots -- see fs.h), which arrives as EIO;
// the fallback is what makes a move work anyway rather than reporting a
// journal detail to somebody dragging a file.
int ufileop_move(const char *src, const char *dst, struct ufileop *s,
                  const struct ufileop_policy *p);

// Deletes a file, or a tree when `recursive`. Directories go DEEPEST
// FIRST, since a directory with anything in it cannot be unlinked.
int ufileop_remove(const char *path, int recursive, struct ufileop *s,
                    const struct ufileop_policy *p);

// --- helpers ----------------------------------------------------------

// The name a "keep both" would use: "notes.txt" -> "notes (1).txt",
// counting up until nothing in `dir` has that name.
//
// THE NUMBER GOES BEFORE THE EXTENSION so the copy still opens with the
// same app -- "notes (1).txt", never "notes.txt (1)". Windows starts at
// (2) on the reasoning that the original is number one; KDE starts at
// (1); this starts at (1). A directory has no extension and takes the
// suffix at the end.
int ufileop_unique_name(const char *dir, const char *name, char *out, int cap);

// Where `src` lands when copied "to" `dst` -- into it if it is a
// directory, otherwise dst itself.
int ufileop_resolve_dest(const char *src, const char *dst, char *out, int cap);

// Is `dst` inside `src`? Copying a directory into itself never
// terminates; coreutils refuses it too, and finding out by filling the
// disk is a bad way to learn.
int ufileop_inside(const char *src, const char *dst);

#endif // ULIB_UFILEOP_H
