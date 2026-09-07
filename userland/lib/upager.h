#ifndef ULIB_UPAGER_H
#define ULIB_UPAGER_H

// The pager: show a block of text one screenful at a time, on whichever
// of fd 0 and fd 1 is a terminal.
//
// It was /bin/less and nothing else until /bin/doc wanted the same
// thing. Two pagers would be two keymaps, two status bars and two ideas
// of what "a page" means -- the shape CLAUDE.md's shared toolkit rule
// exists to stop, and the shape klineedit and ansi.c already avoid.
//
// **IT OWNS NO TEXT BUFFER.** The caller passes bytes it already holds:
// less reads a file or a pipe into its own static buffer, doc renders
// markdown into a malloc'd one. A buffer in here would be .bss inside
// libuapp.so, which every /bin and GUI program links -- 64 KB per
// process for a feature two of them use. What it does allocate is sized
// from the input and the real terminal (a line index, one frame) and
// freed before it returns; if that fails it writes the text through and
// returns, which is what a pager with nowhere to page owes its caller.
#include <stddef.h>

// Page `text` (`len` bytes). `label` names it in its own status line --
// "less", or "doc cmd/ls". `truncated` puts TRUNCATED there; the CALLER
// decides, because the caller owns the buffer that filled up.
//
// With no terminal on either fd this writes `text` through verbatim and
// returns -- `doc ls > ls.txt` is a dump, not a hang waiting for a
// keypress that can never come. Returns 0, or -1 if the write failed.
int upager_run(const char *text, int len, const char *label, int truncated);

// The keymap, spelled out, for a caller's --help. One list rather than
// one per front end: the status line and every usage message read from
// here, so a key added in upager.c cannot go unmentioned.
const char *upager_keys(void);

// The terminal this pager would draw on: its size, and which fd it is.
// A caller that RENDERS text to be paged needs the same width the pager
// will clip at -- asked here so there is one answer rather than two.
// Returns the terminal's fd, or -1 when there is none (and leaves 24x80
// in *rows/*cols, which is the right guess for a dump).
int upager_term(int *rows, int *cols);

// Display columns occupied by `n` bytes of `s`, counting an ANSI escape
// sequence as zero. Exposed because a caller that draws its own status
// or pads a line needs the SAME accounting the pager clips with -- two
// copies of this arithmetic is how a coloured line ends up cut in the
// wrong column.
int upager_width(const char *s, int n);

// The longest prefix of `s` (at most `n` bytes) that occupies no more
// than `cols` display columns. Never splits an escape sequence.
int upager_clip(const char *s, int n, int cols);

#endif // ULIB_UPAGER_H
