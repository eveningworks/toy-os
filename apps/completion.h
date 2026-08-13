#ifndef COMPLETION_H
#define COMPLETION_H

// Tab-completion for the shell -- candidate generation ONLY. This file
// knows nothing about keyboards, cursors, or how to draw a list: it
// takes a line of text plus a cursor position and answers "what could
// the word under the cursor become?". Deliberately shaped that way
// because there are two shells with two completely separate input
// loops (apps/shell.c's shell_read_line(), driven by keyboard_getchar()
// and vga_backspace(); apps/terminal.c's on_key handler, driven by the
// window manager and drawing through a text_scrollback widget), and the
// interesting logic should exist once even though the two of them
// insert and display the result in totally different ways.
//
// Behaviour is zsh's default rather than bash's: one Tab extends the
// word as far as every candidate agrees (`insert` below), and if more
// than one candidate remains the caller lists them. There is no state
// between calls -- no "second Tab does something different", no menu
// cycling -- which is what keeps this a pure function and keeps both
// callers honest.

#include <stdint.h>

#define COMPLETION_MAX_CANDIDATES 48
#define COMPLETION_MAX_LEN        64

struct completion_result {
    // Candidates whose prefix matched the word under the cursor. `count`
    // is how many were actually stored; `truncated` is 1 if there were
    // more than COMPLETION_MAX_CANDIDATES and the rest were dropped (a
    // caller listing them should say so rather than implying the list is
    // complete).
    int count;
    int truncated;
    char candidates[COMPLETION_MAX_CANDIDATES][COMPLETION_MAX_LEN];

    // What the caller should insert at the cursor: the part of the
    // common prefix that isn't typed yet. Empty when there's nothing to
    // add (no candidates, or the word is already the common prefix --
    // the case where the caller should list instead).
    char insert[COMPLETION_MAX_LEN];

    // 1 when exactly one candidate matched and it's now complete, so the
    // caller should also append a space. Never set for a directory
    // candidate -- those end in '/' and the user is probably continuing
    // the path.
    int add_space;
};

// Fills *out for the word under `cursor` in `line` (`cursor` is a byte
// index into `line`, normally its length since neither shell supports
// mid-line editing yet). Returns the number of candidates found, which
// is also out->count.
//
// What gets completed depends on where the cursor is:
//   - first word           -> command names (COMPLETION_COMMANDS below)
//   - argument of a command with a known argument set -> that set
//     (`run`, `color`, `debug`, `keyboard`, `timezone`, `fontsize`,
//     `help`) -- this is the part that makes it feel like zsh rather
//     than like plain readline
//   - anything else        -> filesystem paths, resolved against the
//     shell's current directory
int completion_run(const char *line, int cursor, struct completion_result *out);

// Every command name the shell dispatches, NULL-terminated. Lives here
// rather than in shell.c because this is the only thing that needs the
// list as data -- dispatch() is a hand-written if/else chain, and
// converting it to a table would mean ~40 wrapper functions for
// handlers whose signatures genuinely differ.
//
// That does mean the list can drift from the dispatcher. One direction
// is self-reporting: dispatch() checks this table before printing
// "Unknown command", so a name listed here but not dispatched says so
// explicitly. The other direction (dispatched but not listed) shows up
// as "tab doesn't complete my new command", which announces itself the
// first time you use it. Adding a shell command means touching both.
extern const char *const COMPLETION_COMMANDS[];

// 1 if `name` is in COMPLETION_COMMANDS. Used by dispatch()'s unknown-
// command branch for the drift check described above.
int completion_is_known_command(const char *name);

#endif
