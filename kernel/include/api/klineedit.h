#ifndef KLINEEDIT_H
#define KLINEEDIT_H

#include <stddef.h>
#include "termkey.h"   // kline_feed() decodes what a terminal sends

// A readline-style line editor, as pure logic: a buffer, a cursor, a
// kill ring and an undo stack, with no idea how any of it gets drawn.
//
// It exists because this kernel has TWO command lines -- the physical
// shell (apps/shell.c) and the GUI Terminal (apps/terminal.c) -- and
// both were append-only: one position that grew at the end, no way to
// go back and fix a typo without holding backspace. Giving each its own
// editing logic would have been the same mistake the three path
// resolvers were (see kpath.h): the copies drift, and the divergence
// only shows up when someone types the same keystroke in the other
// window. So the editing lives here and each front end only renders.
//
// It is deliberately render-free, which is also what makes it testable
// (kernel/lib/klineedit_test.c) -- "Alt-B from the middle of a word
// lands at that word's start" is an assertion here, where in either
// front end it would be a screenshot.
//
// ---- how a front end uses it ----
//
//     struct kline_edit ed;
//     kline_init(&ed);
//     for (;;) {
//         int key = <get a key>;
//         switch (kline_key(&ed, key)) {
//         case KLINE_REDRAW:  <repaint ed.buf, cursor at ed.cursor>; break;
//         case KLINE_ACCEPT:  <run ed.buf>; break;
//         case KLINE_HISTORY_PREV: <kline_set(&ed, older_entry)>; break;
//         ...
//         }
//     }
//
// The front end owns history, completion, and the screen; this owns
// the line. Anything needing one of those comes back as an action code
// rather than a callback, so there is no callback plumbing to get
// wrong and the tests need no fixtures.

// The line a `struct kline_edit` holds WITHOUT an allocator, and what
// this file was bounded by for its whole life. A front end that supplies
// no memory gets exactly the old behaviour: a line this long, refused
// rather than truncated at the end.
#define KLINE_INLINE 128

// Kept as the old name so a caller bounding its own buffer still
// compiles, and because 128 is still the length below which the editor
// allocates nothing at all.
#define KLINE_MAX KLINE_INLINE

// MEMORY THE EDITOR DOES NOT OWN, because it cannot name an allocator.
// klineedit.c is compiled into BOTH rings and the build takes the C
// library off its include path deliberately (see the Makefile's
// SHARED_CFLAGS), so it can say neither `kmalloc` nor `malloc`. The
// front end passes its ring's pair in, the way geom.h takes a plot
// callback and ttf.h takes a caller-supplied scratch.
//
// NULL IS SUPPORTED AND COSTS UNDO. With no allocator the line stops
// growing at KLINE_INLINE and further input is refused -- editing,
// motion, the kill ring for a short cut and every other key still work.
// What does NOT is UNDO: a snapshot is sized to the line it holds, and
// one that cannot be allocated is dropped rather than truncated, so a
// front end that passes NULL gets an editor whose Ctrl-_ does nothing.
// Said here because it is the one capability that disappears silently;
// both real front ends pass their ring's allocator.
struct kline_mem {
    void *(*alloc)(unsigned long bytes);
    void  (*free)(void *p);

    // THE LONGEST LINE THE FRONT END CAN ACTUALLY CARRY, or 0 for no
    // limit. It belongs here rather than in a setter because it must
    // survive the re-init both front ends do per line.
    //
    // A GROWABLE EDITOR IN FRONT OF A FIXED CONSUMER IS WORSE THAN A
    // FIXED EDITOR. The shell copies the finished line into a buffer of
    // its own and runs THAT, so an editor willing to grow past it lets
    // a line be typed and displayed whole and then run short -- silent
    // truncation, which is the failure this whole seam exists to remove.
    // Refusing the keystroke instead is visible: the cursor stops.
    unsigned long limit;
};

// How many undo steps a line remembers. Deliberately shallow: this is a
// command line, not a document. It used to be the expensive number --
// each step carried a FULL copy of the line, so the struct was
// KLINE_MAX x 9 and raising the line length raised it ninefold. A
// snapshot is allocated to the length it actually holds now, so depth
// costs pointers and undo of a short line costs almost nothing.
#define KLINE_UNDO_DEPTH 8

// What the front end should do about the key it just fed in.
enum kline_action {
    KLINE_IGNORED = 0,   // unrecognised -- nothing changed, nothing to do
    KLINE_REDRAW,        // the line and/or cursor changed; repaint
    KLINE_ACCEPT,        // Enter -- run ed.buf
    KLINE_CANCEL,        // Ctrl-C -- abandon this line, start a fresh one
    KLINE_EOF,           // Ctrl-D on an empty line -- bash exits the shell here
    KLINE_HISTORY_PREV,  // Up / Ctrl-P -- feed the older entry back via kline_set()
    KLINE_HISTORY_NEXT,  // Down / Ctrl-N -- ditto, newer
    KLINE_COMPLETE,      // Tab -- run completion against ed.buf/ed.cursor
    KLINE_CLEAR_SCREEN,  // Ctrl-L -- clear, then repaint the prompt and line
    KLINE_SEARCH,        // Ctrl-R -- enter/advance reverse history search
    KLINE_LAST_ARG,      // Alt-. -- insert the last word of the previous command
};

struct kline_edit {
    // Points at `inln` until the line outgrows it, and at allocated
    // memory after. Callers read it as a NUL-terminated string, which is
    // why every growth path keeps the terminator.
    char *buf;
    int cap;    // bytes addressable through buf, terminator included
    int len;
    int cursor; // [0, len] -- "sits just before buf[cursor]"

    // ---- state that spans keystrokes ----
    // A pending ESC, waiting to find out whether it's a lone Esc or the
    // first half of a Meta sequence (see keyboard.h on the encoding).
    int meta_pending;
    // A pending Ctrl-X, for the two-key bindings (Ctrl-X Ctrl-U).
    int ctrl_x_pending;
    // Whether the previous key was a yank, which is what makes Alt-Y
    // (yank-pop) legal -- readline only allows it directly after a yank
    // or another yank-pop, and so does this.
    int last_was_yank;
    int yank_start, yank_end; // region the last yank inserted, for yank-pop to replace

    // Undo snapshots, each sized to the line it holds. A snapshot that
    // cannot be allocated is DROPPED rather than truncated: losing an
    // undo step is a small, visible loss, and a half-line restored over
    // a whole one is a wrong line.
    struct {
        char *buf;
        int len, cursor;
    } undo[KLINE_UNDO_DEPTH];
    int undo_count;

    const struct kline_mem *mem;   // NULL: the line stops at KLINE_INLINE

    // LAST, and inline: the common case is a short line, and keeping it
    // here means an ordinary command allocates nothing at all.
    char inln[KLINE_INLINE];
};

void kline_init(struct kline_edit *e);

// Same, but the line may grow past KLINE_INLINE using `mem`, which must
// outlive `e`. Pass NULL for `mem` and this is kline_init().
void kline_init_mem(struct kline_edit *e, const struct kline_mem *mem);

// Returns anything the editor grew. Safe on an editor that never grew,
// and safe to call twice. A front end holding one for the life of the
// process need not call it; one per session must.
void kline_free(struct kline_edit *e);

// Replaces the whole line (history recall, reverse search, an external
// edit) and puts the cursor at the end, like readline does.
void kline_set(struct kline_edit *e, const char *s);

// Inserts a string at the cursor -- for what the front end owns:
// completion's agreed text, Alt-.'s last argument.
void kline_insert_str(struct kline_edit *e, const char *s);

// Feeds one key in. See enum kline_action for what the return means.
enum kline_action kline_key(struct kline_edit *e, int key);

// **WHAT A FRONT END READING A TERMINAL SHOULD CALL.** Feeds one BYTE,
// decoding the ANSI escape sequences a terminal sends for special keys
// (api/termkey.h) before dispatching to kline_key() above. A byte that
// is not part of a sequence reaches the keymap unchanged, so control
// codes and ordinary characters behave exactly as they always have.
//
// `st` is the caller's decoder state, one per input stream, zeroed
// before the first call. A sequence in progress returns KLINE_IGNORED
// and changes nothing -- the action arrives with the byte that
// completes it.
//
// It is here rather than in each front end because there are three of
// them, and a decoder per front end is the drift this file exists to
// prevent.
enum kline_action kline_feed(struct kline_edit *e, struct termkey_state *st,
                             int byte);

// The word boundaries the editor uses, exposed because the front ends
// need the same notion for completion. Two DIFFERENT definitions, and
// the difference is real bash behavior, not an inconsistency:
//   - a "word" for Alt-B/Alt-F/Alt-D is a run of alphanumerics, so
//     punctuation is skipped over;
//   - a "word" for Ctrl-W is whitespace-delimited, so Ctrl-W over
//     "/bin/ls" kills the whole path where Alt-Backspace kills "ls".
int kline_word_start(const char *buf, int len, int from);       // alnum words
int kline_word_end(const char *buf, int len, int from);
int kline_ws_word_start(const char *buf, int len, int from);    // whitespace-delimited

#endif
