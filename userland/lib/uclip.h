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
// **FILES, NOT TEXT, for now.** The entries are absolute paths -- the
// shape `text/uri-list` has on a real desktop. The same buffer carries
// text when something needs a text clipboard; only the interpretation
// would differ.
//
// The CUT is a promise, not an act: `UCLIP_CUT` means the files are
// still where they were and a paste is what moves them (Explorer's and
// Dolphin's rule). An app that shows a pending cut should draw it as
// pending -- dimmed rows -- and stop when the serial changes under it.

#define UCLIP_NONE WIN_CLIP_OP_NONE
#define UCLIP_COPY WIN_CLIP_OP_COPY
#define UCLIP_CUT  WIN_CLIP_OP_CUT

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

#endif // ULIB_UCLIP_H
