#ifndef HISTEDIT_H
#define HISTEDIT_H

// The libedit surface `dash` expects, over THIS system's line editor.
//
// **WHY A SHIM AND NOT A PORT OF libedit.** dash builds against libedit
// for interactive editing, and Debian ships it without one -- which is
// why `userland/backends/dash/config.h` used to `#define SMALL 1` and
// dash had no arrow keys, no history and no Tab. This repo already HAS
// the editor: `kernel/lib/klineedit.c`, compiled twice so the physical
// shell, the GUI Terminal and /bin/tosh all edit with the same code. A
// second editor would be the thing "there is one line editor" forbids,
// so what the port gets is an ADAPTER: the names libedit exports,
// answered by klineedit and userland/lib/uline.c.
//
// Only what dash actually calls is here. This is not libedit and does
// not try to be -- `el_set()` accepts the four options dash passes and
// ignores the rest, which is exactly the amount of libedit dash needs.
#include <stdio.h>

typedef struct EditLine EditLine;
typedef struct History History;

// dash reads `num` (the event's number) and `str` (its text), and
// nothing else.
typedef struct {
    int num;
    const char *str;
} HistEvent;

// el_set() options. Only these four reach us; the values are ours, since
// nothing outside this port ever sees them.
enum {
    EL_PROMPT = 1,
    EL_PROMPT_ESC,
    EL_EDITOR,
    EL_SIGNAL,
    EL_HIST,
    EL_TERMINAL,
    EL_BIND,
};

// history() actions. `H_SETSIZE` and `H_ENTER`/`H_APPEND` are the write
// side; the rest walk the list for `fc`.
enum {
    H_FUNC = 1,
    H_SETSIZE,
    H_GETSIZE,
    H_FIRST,
    H_LAST,
    H_PREV,
    H_NEXT,
    H_CURR,
    H_SET,
    H_ADD,
    H_ENTER,
    H_APPEND,
    H_NEXT_STR,
    H_PREV_STR,
    H_NEXT_EVENT,
    H_PREV_EVENT,
    H_LOAD,
    H_SAVE,
    H_CLEAR,
};

EditLine *el_init(const char *prog, FILE *fin, FILE *fout, FILE *ferr);
void      el_end(EditLine *el);
// Returns the line INCLUDING its newline, or NULL at end of input;
// `*count` is its length. The storage is the EditLine's and stays valid
// until the next el_gets().
const char *el_gets(EditLine *el, int *count);
int       el_set(EditLine *el, int op, ...);
int       el_source(EditLine *el, const char *file);

History  *history_init(void);
void      history_end(History *h);
int       history(History *h, HistEvent *ev, int op, ...);

#endif
