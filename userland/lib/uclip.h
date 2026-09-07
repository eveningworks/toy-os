#ifndef ULIB_UCLIP_H
#define ULIB_UCLIP_H

#include <stdint.h>
#include "lib/uclip_page.h"   // the page layout, shared with /bin/clipboardd

// uclip -- the system clipboard, for a ring-3 app.
//
// **THE CLIPBOARD IS NOT IN THE KERNEL.** It is a named shared-memory
// object created and owned by `/bin/clipboardd`, and this library maps
// it directly -- so a paste costs no syscall at all, and an app that
// greys out its Paste item can watch the serial every frame for free.
// lib/uclip_page.h has the reasoning and the page layout; the short
// version is that what a clipboard IS is desktop policy, and ring 0
// contains no applications.
//
// **NO SERVICE MEANS AN EMPTY CLIPBOARD, NOT AN ERROR.** A machine can
// boot with clipboardd disabled. Reading then answers UCLIP_NONE and
// writing returns 0, which an app should report -- silence is the one
// outcome a Copy must never have.
//
// This exists as a library, rather than each app poking the page, for
// one reason: the payload is a PACKED list of NUL-terminated strings
// under a seqlock, and an app that packs it by hand is an app that
// will one day forget the terminator on the last entry, or read a
// half-written clipboard.
//
// **TWO KINDS: FILES AND TEXT.** Files are absolute paths, the shape
// `text/uri-list` has on a real desktop; text is one run of characters.
// A commit replaces whatever was there, so copying text discards a
// pending file cut -- one clipboard, as X11, Wayland and Win32 all have
// for this selection. **ALWAYS ASK `uclip_kind()` BEFORE READING**: a
// paster that assumes its own kind reads a path as a line of text, or
// the reverse.
//
// The CUT is a promise, not an act: `UCLIP_CUT` means the files are
// still where they were and a paste is what moves them (Explorer's and
// Dolphin's rule). An app that shows a pending cut should draw it as
// pending -- dimmed rows -- and stop when the serial changes under it.
// **A CUT IS MEANINGLESS FOR TEXT**, because there is nothing left to
// move it from once the page holds a copy: an app cutting text deletes
// its own selection and COPIES, which is what every editor does.
//
// **HOLD A `struct uclip` STATICALLY, NEVER ON THE STACK.** It is a
// SNAPSHOT of the page, payload included, which is far past the ring-3
// frame budget -- the compiler says so rather than the machine.

// A snapshot, taken under the page's seqlock. Not the page itself: the
// pointers uclip_path() and uclip_text() hand back have to stay still
// while the caller uses them, and the page can change under it at any
// moment.
struct uclip {
    uint32_t op;
    uint32_t kind;
    uint32_t count;
    uint32_t len;
    uint32_t serial;
    char data[CLIP_BYTES];
};

#define UCLIP_NONE CLIP_OP_NONE
#define UCLIP_COPY CLIP_OP_COPY
#define UCLIP_CUT  CLIP_OP_CUT

#define UCLIP_KIND_FILES CLIP_KIND_FILES
#define UCLIP_KIND_TEXT  CLIP_KIND_TEXT

// The largest text a copy can carry. A selection past it is REFUSED --
// see uclip_set_text() -- so an app can say so rather than paste short.
#define UCLIP_TEXT_MAX (CLIP_BYTES - 1)

// Reads the clipboard into `c`. Returns 1; an EMPTY clipboard is not a
// failure, it is `uclip_op() == UCLIP_NONE`.
int uclip_load(struct uclip *c);

int uclip_op(const struct uclip *c);
int uclip_count(const struct uclip *c);
unsigned uclip_serial(const struct uclip *c);
int uclip_kind(const struct uclip *c);

// The page's serial WITHOUT taking a snapshot -- one shared-memory
// read. What a per-frame "has the clipboard changed?" poll should use,
// since copying 64 KiB to answer it would be absurd.
unsigned uclip_peek_serial(void);

// Entry `i`, or NULL past the end. Points into `c` -- it does not
// outlive the next uclip_load() on the same object.
const char *uclip_path(const struct uclip *c, int i);

// --- putting something on it -----------------------------------------
//
// Built up, then committed: an app marks N files and cannot know it has
// run out of room until it tries. `uclip_add` returns 0 the moment one
// does not fit, and `uclip_commit` then puts NOTHING on the clipboard --
// a partial cut set is files silently left behind.

// The `struct uclip *` is ignored and kept only so existing call sites
// read the same; the staging buffer is this library's, because it must
// not be on a caller's stack either.
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
