#ifndef ULINE_H
#define ULINE_H

// uline -- the PAINTING half of a ring-3 line-editing front end.
//
// `kernel/lib/klineedit.c` is the editor and is compiled twice; what a
// front end adds is putting the result on a screen. That part used to
// exist once, inside /bin/tosh, and `/bin/dash` needing it made a second
// copy the obvious move -- which is the drift CLAUDE.md's "there is one
// line editor" rule exists to stop. So the paint is here and both call
// it.
//
// **NOT a read loop.** Reading bytes and feeding kline_key() stays with
// each front end, because that is where they genuinely differ: tosh has
// background-job news, a Ctrl-R search and Tab completion over its own
// command table, and dash has none of those. Sharing the loop would mean
// a callback per difference; sharing the paint costs nothing.
#include <stddef.h>
#include "klineedit.h"

// The terminal's width in columns, asked of the kernel rather than
// assumed -- a resized window reports the new width. 80 if fd 0 cannot
// answer.
int uline_cols(void);

// Repaints `prompt` + the edit buffer and leaves the caret at
// `ed->cursor`.
//
// **IT COUNTS SCREEN ROWS, because a line can be wider than the window.**
// `*row_shown` is how the NEXT repaint finds the top of this one: pass
// the same int back every time, and reset it to 0 yourself after
// printing anything else (a finished command, a job report, a fresh
// prompt), or the next paint erases from the wrong row.
//
// THE TRAP THIS SHAPE AVOIDS: the two terminals disagree about where the
// caret sits after a row has been filled to its last column (one wraps
// lazily, on the next character), so nothing here depends on an
// automatic wrap -- rows end with an explicit '\n' and the caret is
// placed from a row/column computed here.
void uline_paint(const char *prompt, const struct kline_edit *ed, int *row_shown);

// Erases the painted line and everything below it, leaving the caret at
// the start of the row the paint began on, and sets `*row_shown` to 0.
// What a front end calls before printing anything of its own.
void uline_erase(int *row_shown);

// Parks the caret past the last character, then ends the line with a
// newline and resets `*row_shown`. What to call before printing
// anything of the front end's own: the paint leaves the caret wherever
// `ed->cursor` is, and a '\n' from the middle of a wrapped line would
// start the next output on top of the rest of it.
void uline_end(const char *prompt, struct kline_edit *ed, int *row_shown);

// Ctrl-L: clear the display, put the caret at the top, and repaint the
// prompt and the line being typed -- which is what "keeping the line
// you're typing" means. The CORE decides what Ctrl-L means and each
// front end owns how; this is the ANSI how, shared by the two ring-3
// front ends (the kernel shell calls the console directly).
void uline_clear_screen(const char *prompt, struct kline_edit *ed, int *row_shown);

// Alt-. : insert the last whitespace-delimited word of `previous` at the
// cursor. A no-op for a NULL or empty `previous`.
void uline_insert_last_arg(struct kline_edit *ed, const char *previous);

// Tab: lists `count` candidates in columns fitted to the real terminal
// width, then repaints the prompt and line so the caller is back where
// they were with more information. Only the LISTING -- running the
// completion engine and inserting its result is the caller's, because
// the engine is reached through a per-ring `completion_env`.
// `candidates` is `count` fixed-width rows of `stride` bytes each, which
// is how `struct completion_result` stores them -- a FLAT char array,
// not an array of pointers. Taking a stride keeps this file independent
// of completion.h; casting the 2-D array to `char **` instead reads the
// first candidate's TEXT as a pointer, which faults with the characters
// visible in CR2 (that is how this signature was arrived at).
void uline_list_candidates(const char *prompt, struct kline_edit *ed, int *row_shown,
                           const char *candidates, int count, int stride);

#endif
