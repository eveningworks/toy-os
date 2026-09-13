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

#endif
