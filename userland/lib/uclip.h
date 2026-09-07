#ifndef ULIB_UCLIP_H
#define ULIB_UCLIP_H

#include <stdint.h>
#include "win_proto.h"   // WIN_CLIP_* and struct win_clip_msg

// uclip -- the system clipboard, for a ring-3 app.
//
// A thin wrapper over SYS_WIN_CLIP that exists for one reason: the
// payload is a PACKED list of NUL-terminated strings, and an app that
// packs and unpacks it by hand is an app that will one day forget the
// terminator on the last entry. Two callers pack it the same way or
// they do not interoperate, which is the whole point of a clipboard.
//
// **TWO KINDS: FILES AND TEXT.** Files are absolute paths, the shape
// `text/uri-list` has on a real desktop; text is one run of characters.
// A SET replaces whatever was there, so copying text discards a pending
// file cut and every client is told (`WIN_EV_CLIPBOARD`) -- one
// clipboard, as X11, Wayland and Win32 all have for this selection.
// **ALWAYS ASK `uclip_kind()` BEFORE READING**: a paster that assumes
// its own kind reads a path as a line of text, or the reverse.
//
// The CUT is a promise, not an act: `UCLIP_CUT` means the files are
// still where they were and a paste is what moves them (Explorer's and
// Dolphin's rule). An app that shows a pending cut should draw it as
// pending -- dimmed rows -- and stop when the serial changes under it.
// **A CUT IS REFUSED FOR TEXT**, because there is nothing left to move
// it from once the copy is in the server: an app cutting text deletes
// its own selection and COPIES, which is what every editor does.
//
// **HOLD A `struct uclip` STATICALLY, NEVER ON THE STACK.** It embeds
// the whole payload (WIN_CLIP_BYTES), which is far past the ring-3
// frame budget -- the compiler says so rather than the machine, which
// is the only reason the struct is allowed to stay this shape.

#define UCLIP_NONE WIN_CLIP_OP_NONE
#define UCLIP_COPY WIN_CLIP_OP_COPY
#define UCLIP_CUT  WIN_CLIP_OP_CUT

#define UCLIP_KIND_FILES WIN_CLIP_KIND_FILES
#define UCLIP_KIND_TEXT  WIN_CLIP_KIND_TEXT

// The largest text a copy can carry. A selection past it is REFUSED --
// see uclip_set_text() -- so an app can say so rather than paste short.
#define UCLIP_TEXT_MAX (WIN_CLIP_BYTES - 1)

// What is on the clipboard. `op` is UCLIP_*, `count` how many entries,
// `serial` changes whenever anybody replaces it -- which is what a
// client compares to notice that its pending cut is no longer the one
// that matters.
struct uclip {
    struct win_clip_msg msg;
};

// Reads the clipboard into `c`. Returns 1; an EMPTY clipboard is not a
// failure, it is `uclip_op() == UCLIP_NONE`.
int uclip_load(struct uclip *c);

int uclip_op(const struct uclip *c);
int uclip_count(const struct uclip *c);
unsigned uclip_serial(const struct uclip *c);
int uclip_kind(const struct uclip *c);

// Entry `i`, or NULL past the end. Points into `c` -- it does not
// outlive the next uclip_load() on the same object.
const char *uclip_path(const struct uclip *c, int i);

// --- putting something on it -----------------------------------------
//
// Built up, then committed: an app marks N files and cannot know it has
// run out of room until it tries. `uclip_add` returns 0 the moment one
// does not fit, and `uclip_commit` then puts NOTHING on the clipboard --
// a partial cut set is files silently left behind.

void uclip_begin(struct uclip *c, int op);
int  uclip_add(struct uclip *c, const char *path);
int  uclip_commit(struct uclip *c);

// Empties it. Not `uclip_commit` on an empty set -- that is REFUSED, so
// that an app which failed to add anything cannot wipe the clipboard by
// accident. Clearing is a deliberate act and says so.
int  uclip_clear(void);

// --- text ------------------------------------------------------------
//
// One call each way, because a text clipboard has nothing to build up:
// there is exactly one entry and the app already holds it.

// Puts `n` characters on the clipboard as TEXT. Returns 0 without
// touching the clipboard if they do not fit (`n > UCLIP_TEXT_MAX`) or
// the server refused -- a caller should say which to the person, since
// "nothing happened" is the one outcome a Copy must never have.
//
// It takes its own buffer, so nothing the CALLER holds has to be
// clipboard-sized.
int uclip_set_text(const char *s, int n);

// The clipboard's text, or NULL when it holds something else (ALWAYS
// check -- a file list read as text is a path pasted into a document).
// `*out_len` gets the length without the NUL. Points into `c`.
const char *uclip_text(const struct uclip *c, int *out_len);

#endif // ULIB_UCLIP_H
