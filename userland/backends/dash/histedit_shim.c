// See histedit.h: the libedit names dash calls, answered by this
// system's own line editor.
//
// TWO HALVES. `el_gets()` is a front end over kernel/lib/klineedit.c --
// it reads bytes off fd 0, feeds them in as-is (specials arrive as the
// 0x91-0xA6 KEY_* codes klineedit already switches on, which is what
// makes a translation layer unnecessary) and paints with
// userland/lib/uline.c, the same paint /bin/tosh uses. The other half is
// a history list, because `fc` walks one.
//
// **THE EDITING KEYS ARE NOT HERE.** A key this front end seems to lack
// belongs in klineedit's keymap, where all three front ends gain it at
// once -- adding one here would be the second keymap that rule exists to
// prevent.
#include "histedit.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "klineedit.h"
#include "lib/uline.h"
#include "lib/ucomplete.h" // the SHARED completion engine, not a second one
#include "completion.h"
#include "histsearch.h"   // the SHARED reverse-search loop
#include "rt/sys.h"   // sys_tty_raw/_tcgetattr/_tcsetattr

#define HIST_DEFAULT 500

static void put_str(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    if (n) write(1, s, n);
}

struct History {
    char **ent;      // ent[0] is the OLDEST
    int    n, cap;
    int    max;      // H_SETSIZE
    int    base;     // the event number of ent[0]
    int    cursor;   // where the H_PREV/H_NEXT walk has got to
};

struct EditLine {
    struct kline_edit ed;
    struct History   *hist;
    const char *(*prompt)(void *);
    int    row_shown;
    char  *line;     // what el_gets() returns; ours to free
    size_t cap;
    // The terminal as the shell left it. **RAW MODE IS WHAT MAKES AN
    // EDITOR POSSIBLE AT ALL** -- under ICANON the discipline holds
    // every keystroke until Enter, so an arrow key arrives as a byte in
    // the middle of a finished line and nothing can act on it. Restored
    // around the return, because the COMMAND dash is about to run wants
    // an ordinary canonical, echoing terminal -- exactly the reasoning
    // /bin/tosh's g_tio_saved records.
    struct tty_termios saved;
    int    have_saved;
    struct termkey_state keys;   // the terminal decoder, per EditLine
};

// --- history ---------------------------------------------------------

History *history_init(void) {
    History *h = calloc(1, sizeof *h);
    if (h) { h->max = HIST_DEFAULT; h->base = 1; }
    return h;
}

void history_end(History *h) {
    if (!h) return;
    for (int i = 0; i < h->n; i++) free(h->ent[i]);
    free(h->ent);
    free(h);
}

// Drops the oldest entries until the list fits `max`. The event NUMBERS
// do not restart -- `base` moves instead, because `fc 12` means event 12
// however many have scrolled off since.
static void hist_trim(History *h) {
    while (h->max > 0 && h->n > h->max) {
        free(h->ent[0]);
        memmove(h->ent, h->ent + 1, (size_t)(h->n - 1) * sizeof *h->ent);
        h->n--;
        h->base++;
    }
}

static int hist_add(History *h, const char *s) {
    if (!h || !s) return -1;
    if (h->n == h->cap) {
        int cap = h->cap ? h->cap * 2 : 32;
        char **e = realloc(h->ent, (size_t)cap * sizeof *e);
        if (!e) return -1;
        h->ent = e;
        h->cap = cap;
    }
    char *dup = strdup(s);
    if (!dup) return -1;
    h->ent[h->n++] = dup;
    hist_trim(h);
    h->cursor = h->n;          // a new entry resets the walk
    return 0;
}

// Fills `ev` from index `i`, or returns -1 for one out of range. The
// cursor is left ON `i`, which is what makes a following H_NEXT/H_PREV
// continue from here.
static int hist_at(History *h, HistEvent *ev, int i) {
    if (!h || i < 0 || i >= h->n) return -1;
    h->cursor = i;
    if (ev) { ev->num = h->base + i; ev->str = h->ent[i]; }
    return 0;
}

int history(History *h, HistEvent *ev, int op, ...) {
    va_list ap;
    va_start(ap, op);
    int rc = -1;
    if (!h) { va_end(ap); return -1; }

    switch (op) { // dispatch-ok: bounded by histedit.h's H_* set
    case H_SETSIZE:
        h->max = va_arg(ap, int);
        hist_trim(h);
        rc = 0;
        break;
    case H_GETSIZE:
        if (ev) ev->num = h->max;
        rc = 0;
        break;
    case H_ENTER:
    case H_ADD:
    case H_APPEND:
        rc = hist_add(h, va_arg(ap, char *));
        break;
    case H_CLEAR:
        for (int i = 0; i < h->n; i++) free(h->ent[i]);
        h->base += h->n;
        h->n = h->cursor = 0;
        rc = 0;
        break;
    // **NEWEST FIRST.** libedit's H_FIRST is the most RECENT event and
    // H_NEXT walks towards older ones -- the opposite of what the names
    // suggest, and dash's fc depends on it (histcmd picks H_PREV when
    // counting forwards). Getting this backwards makes `fc -l` list in
    // the wrong order without failing.
    case H_FIRST: rc = hist_at(h, ev, h->n - 1); break;
    case H_LAST:  rc = hist_at(h, ev, 0); break;
    case H_NEXT:  rc = hist_at(h, ev, h->cursor - 1); break;
    case H_PREV:  rc = hist_at(h, ev, h->cursor + 1); break;
    case H_CURR:  rc = hist_at(h, ev, h->cursor); break;
    case H_NEXT_EVENT: {
        int want = va_arg(ap, int);
        rc = hist_at(h, ev, want - h->base);
        break;
    }
    case H_PREV_EVENT: {
        int want = va_arg(ap, int);
        rc = hist_at(h, ev, want - h->base);
        break;
    }
    // A PREFIX match, walking towards older entries from the cursor --
    // what `fc -s pat` and `!pat` use.
    case H_PREV_STR:
    case H_NEXT_STR: {
        const char *pat = va_arg(ap, char *);
        size_t len = pat ? strlen(pat) : 0;
        for (int i = h->cursor - 1; i >= 0; i--) {
            if (len && strncmp(h->ent[i], pat, len) == 0) {
                rc = hist_at(h, ev, i);
                break;
            }
        }
        break;
    }
    default:
        break;   // H_LOAD/H_SAVE/H_SET: dash never reaches them here
    }
    va_end(ap);
    return rc;
}

// The newest entry, or 0 -- Alt-. inserts its last word.
static const char *hist_newest(History *h) {
    return (h && h->n > 0) ? h->ent[h->n - 1] : 0;
}

// --- Ctrl-R, over the SHARED search loop ------------------------------
//
// kernel/lib/histsearch.c is compiled twice and reached through a
// `struct histsearch_env` of callbacks, which is the seam that lets it
// serve a history it knows nothing about -- the kernel shell's, tosh's,
// and this one. So Ctrl-R here is the same loop and the same key
// handling as everywhere else, not a second search.
static EditLine *g_searching;    // the env's callbacks take a ctx; this is it

static int hs_count(void *ctx) {
    (void)ctx;
    return g_searching && g_searching->hist ? g_searching->hist->n : 0;
}

// NEWEST FIRST, because a reverse search walks backwards in time and
// index 0 must be the most recent thing typed.
static const char *hs_entry(void *ctx, int i) {
    (void)ctx;
    History *h = g_searching ? g_searching->hist : 0;
    if (!h || i < 0 || i >= h->n) return 0;
    return h->ent[h->n - 1 - i];
}

static int hs_getkey(void *ctx) {
    (void)ctx;
    char c;
    for (;;) {
        ssize_t n = read(0, &c, 1);
        if (n == 1) return (unsigned char)c;
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
}

static void hs_paint(void *ctx, const char *pattern, const char *match) {
    (void)ctx;
    // One row, rewritten in place: the search prompt replaces the
    // shell's while it is up, exactly as bash's does.
    put_str("\r\x1b[K(reverse-i-search)`");
    put_str(pattern ? pattern : "");
    put_str("': ");
    put_str(match ? match : "");
}

// --- the editor ------------------------------------------------------

EditLine *el_init(const char *prog, FILE *fin, FILE *fout, FILE *ferr) {
    (void)prog; (void)fin; (void)fout; (void)ferr;
    EditLine *el = calloc(1, sizeof *el);
    if (!el) return 0;
    kline_init(&el->ed);
    return el;
}

void el_end(EditLine *el) {
    if (!el) return;
    kline_free(&el->ed);
    free(el->line);
    free(el);
}

int el_set(EditLine *el, int op, ...) {
    if (!el) return -1;
    va_list ap;
    va_start(ap, op);
    int rc = 0;
    switch (op) { // dispatch-ok: bounded by histedit.h's EL_* set
    case EL_PROMPT:
    case EL_PROMPT_ESC:
        el->prompt = (const char *(*)(void *))va_arg(ap, void *);
        break;
    case EL_HIST:
        (void)va_arg(ap, void *);              // the history() function
        el->hist = va_arg(ap, History *);
        break;
    // ACCEPTED AND IGNORED, deliberately. There is one editing mode
    // (klineedit's), one terminal, and signals are the shell's -- so
    // `set -o vi` changes nothing rather than failing, which is the
    // honest answer for a system with one editor.
    case EL_EDITOR:
    case EL_TERMINAL:
    case EL_SIGNAL:
    case EL_BIND:
        break;
    default:
        rc = -1;
        break;
    }
    va_end(ap);
    return rc;
}

// No ~/.editrc here: there is nothing it could configure.
int el_source(EditLine *el, const char *file) { (void)el; (void)file; return 0; }

static void restore_tty(EditLine *el) {
    if (el->have_saved) sys_tcsetattr(0, &el->saved);
    el->have_saved = 0;
}

// Hands the caller the finished line, newline included -- el_gets()'s
// contract, and what dash's preadbuffer() copies out.
static const char *finish(EditLine *el, int *count) {
    size_t need = (size_t)el->ed.len + 2;
    if (need > el->cap) {
        char *p = realloc(el->line, need);
        if (!p) { *count = 0; return 0; }
        el->line = p;
        el->cap = need;
    }
    memcpy(el->line, el->ed.buf, (size_t)el->ed.len);
    el->line[el->ed.len] = '\n';
    el->line[el->ed.len + 1] = '\0';
    *count = el->ed.len + 1;
    return el->line;
}

const char *el_gets(EditLine *el, int *count) {
    if (!el || !count) return 0;
    *count = 0;

    kline_init(&el->ed);
    el->row_shown = 0;

    el->have_saved = sys_tcgetattr(0, &el->saved) == 0;
    sys_tty_raw(0);

    const char *p = el->prompt ? el->prompt(0) : "$ ";
    uline_paint(p, &el->ed, &el->row_shown);

    for (;;) {
        char buf[64];
        ssize_t n = read(0, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;   // a signal, not end of input
            restore_tty(el);
            return 0;
        }
        if (n == 0) { restore_tty(el); return 0; }

        for (ssize_t i = 0; i < n; i++) {
            int key = (unsigned char)buf[i];
            // Decoded: fd 0 is a terminal, so a special key arrives as
            // an ANSI sequence rather than a private byte.
            switch (kline_feed(&el->ed, &el->keys, key)) {
            case KLINE_REDRAW:
                uline_paint(p, &el->ed, &el->row_shown);
                break;

            case KLINE_ACCEPT:
                write(1, "\n", 1);
                el->row_shown = 0;
                if (el->ed.len > 0 && el->hist)
                    hist_add(el->hist, el->ed.buf);
                restore_tty(el);
                return finish(el, count);

            // dash's own SIGINT handling reprints the prompt, so this
            // only has to throw the line away.
            case KLINE_CANCEL:
                write(1, "\n", 1);
                el->row_shown = 0;
                kline_init(&el->ed);
                uline_paint(p, &el->ed, &el->row_shown);
                break;

            case KLINE_EOF:
                // Ctrl-D on an EMPTY line is end of input; on a line
                // with text klineedit has already deleted a character.
                write(1, "\n", 1);
                restore_tty(el);
                return 0;

            case KLINE_HISTORY_PREV: {
                HistEvent ev;
                if (el->hist && history(el->hist, &ev, el->hist->cursor >= el->hist->n
                                        ? H_FIRST : H_NEXT) == 0) {
                    kline_set(&el->ed, ev.str);
                    uline_paint(p, &el->ed, &el->row_shown);
                }
                break;
            }

            case KLINE_HISTORY_NEXT: {
                HistEvent ev;
                if (el->hist && history(el->hist, &ev, H_PREV) == 0) {
                    kline_set(&el->ed, ev.str);
                    uline_paint(p, &el->ed, &el->row_shown);
                }
                break;
            }

            // --- the four that used to fall through here --------------
            //
            // The editor RECOGNISES all of these; acting on them is the
            // front end's job, and ignoring them is why Tab, Ctrl-L,
            // Ctrl-R and Alt-. did nothing in dash while working in
            // /bin/tosh. **A key that still does nothing belongs in
            // klineedit's keymap, not here** -- adding one here would be
            // the second keymap that rule exists to prevent.
            case KLINE_CLEAR_SCREEN:
                uline_clear_screen(p, &el->ed, &el->row_shown);
                break;

            case KLINE_LAST_ARG:
                uline_insert_last_arg(&el->ed, hist_newest(el->hist));
                uline_paint(p, &el->ed, &el->row_shown);
                break;

            case KLINE_COMPLETE: {
                // ~3 KB, and a ring-3 frame is budgeted at 2 KB, so
                // static rather than stack -- tosh's has the same note.
                static struct completion_result cr;
                if (!completion_run_env(ucomplete_env(), el->ed.buf,
                                        el->ed.cursor, &cr))
                    break;
                if (cr.insert[0]) kline_insert_str(&el->ed, cr.insert);
                if (cr.add_space) kline_insert_str(&el->ed, " ");
                if (cr.count <= 1) {          // one candidate needs no list
                    uline_paint(p, &el->ed, &el->row_shown);
                    break;
                }
                uline_list_candidates(p, &el->ed, &el->row_shown,
                                      (const char *)cr.candidates, cr.count,
                                      COMPLETION_MAX_LEN);
                break;
            }

            case KLINE_SEARCH: {
                static const struct histsearch_env env = {
                    0, hs_count, hs_entry, hs_getkey, hs_paint,
                };
                g_searching = el;
                const char *match = 0;
                enum histsearch_result r = histsearch_run(&env, &match);
                g_searching = 0;

                put_str("\r\x1b[K");
                el->row_shown = 0;
                if (r != HISTSEARCH_CANCELLED && match)
                    kline_set(&el->ed, match);
                // ACCEPTED means Enter ended the search, which in bash
                // RUNS the match rather than merely recalling it.
                if (r == HISTSEARCH_ACCEPTED) {
                    write(1, "\n", 1);
                    el->row_shown = 0;
                    if (el->ed.len > 0 && el->hist)
                        hist_add(el->hist, el->ed.buf);
                    restore_tty(el);
                    return finish(el, count);
                }
                uline_paint(p, &el->ed, &el->row_shown);
                break;
            }

            default:
                break;
            }
        }
    }
}
