#ifndef KLINEEDIT_H
#define KLINEEDIT_H

#include <stddef.h>

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

#define KLINE_MAX 128 // matches the shell's LINE_MAX and Terminal's TERM_LINE_MAX

// How many undo steps a line remembers. Deliberately shallow: this is
// a command line, not a document, and `struct kline_edit` is held by
// value by both front ends -- at 8 deep it is already ~1.2KB, which is
// why both hold it as file-scope state rather than on the stack.
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
    char buf[KLINE_MAX];
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

    // Undo snapshots. Small on purpose: this is a command line, not a
    // document, and the struct is held by value by both front ends.
    struct {
        char buf[KLINE_MAX];
        int len, cursor;
    } undo[KLINE_UNDO_DEPTH];
    int undo_count;
};

void kline_init(struct kline_edit *e);

// Replaces the whole line (history recall, reverse search, an external
// edit) and puts the cursor at the end, like readline does.
void kline_set(struct kline_edit *e, const char *s);

// Inserts a string at the cursor -- for what the front end owns:
// completion's agreed text, Alt-.'s last argument.
void kline_insert_str(struct kline_edit *e, const char *s);

// Feeds one key in. See enum kline_action for what the return means.
enum kline_action kline_key(struct kline_edit *e, int key);

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
