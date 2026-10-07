#ifndef UVTERM_H
#define UVTERM_H

// uvterm -- ONE TERMINAL: a pty and the program on it, the reader thread
// that drains it, the ANSI parser, the grid, its scrollback and alternate
// screen, the cursor, a selection, and drawing all of that into a box.
// GNOME's VteTerminal is this shape: GNOME Terminal, xfce4-terminal and
// Tilix are hosts that put one per tab and add the chrome. Here the GUI
// Terminal hosts one per tab and keeps the tabs, menus, find bar, Session
// panel, screen effect and scrollbar.
//
// EACH TERMINAL OWNS ITS GEOMETRY. A host with one window sizes them all
// alike (uvterm_resize on each), but nothing here assumes it -- the grid,
// the scrollback depth and the selection are per terminal, so a second
// host with two terminals of different sizes needs nothing new.
//
// THREADS: the reader touches `ring`, `head`, `eof` and `done`, and
// NOTHING ELSE -- not the grid, not the parser. Everything a frame reads
// is the main thread's; the reader wakes it with uapp_post(app, post_id).

#include <stdint.h>
#include <pthread.h>
#include "ansi.h"
#include "ui/ugfx.h"

struct uapp;

struct uvterm_cell { char ch; uint8_t fg, bg; };

// A position in the VIRTUAL buffer: lines 0..sb_count-1 are scrollback
// and the screen follows, so a line names the same text whether it is on
// screen or scrolled back. `col` may be `cols` -- past the last cell --
// which is how a selection reaches a line's break.
struct uvterm_point { int line, col; };

#define UVTERM_RING_BYTES 8192
#define UVTERM_TITLE_MAX  40
#define UVTERM_PROG_MAX   64

struct uvterm {
    // --- set by the host ---------------------------------------------------
    int rows, cols;             // the grid shown (uvterm_resize)
    int sb_rows;                // scrollback depth (uvterm_set_scrollback)
    uint8_t fg, bg;             // the default pair, palette slots (uvterm_set_default_pair)
    int scroll_on_output;       // new output pins the view to the bottom
    struct uapp *app;           // whom the reader posts to, with `post_id`
    int post_id;

    // --- the screen --------------------------------------------------------
    // Heap-backed at cap_rows x cap_cols, GROWN when the grid outgrows
    // them and never shrunk: shrinking narrows rows/cols and leaves the
    // buffers, as xterm's crop-not-reflow does.
    int cap_rows, cap_cols;
    struct uvterm_cell *grid;   // cap_rows x cap_cols
    struct uvterm_cell *sb;     // sb_rows  x cap_cols
    int sb_count;               // lines of scrollback held
    int sb_view;                // how far back the view is scrolled
    // THE ALTERNATE SCREEN (ESC[?1049h/l): a copy of the grid and the
    // cursor, so a full-screen program leaves the screen as it found it.
    // Its scrollback is not saved -- a real terminal's alternate screen
    // has none, which is why `less` cannot be scrolled back through.
    struct uvterm_cell *saved;
    int alt;
    int saved_cr, saved_cc;
    int cr, cc;                 // the cursor, in cells
    int cursor_shown;           // ESC[?25h/l
    struct ansi_parser vt;

    // --- the program -------------------------------------------------------
    int live;                   // a program was started and not stopped
    volatile int done;          // the reader has finished: the struct may be reused
    volatile int eof;           // the pty closed
    int master, child;
    char prog[UVTERM_PROG_MAX]; // what was started
    char title[UVTERM_TITLE_MAX];
    // Where the program last said it was standing: its OSC title when that
    // is an absolute path, which is what tosh sends.
    char cwd[UVTERM_TITLE_MAX];
    // A HAND-GIVEN TITLE OUTRANKS THE PROGRAM'S (Konsole's rule): once set,
    // OSC titles stop moving it; `cwd` still follows.
    int title_locked;
    // What an EMPTY OSC title restores ("Shell"), the host's to set; and
    // set when the title changed, the host's to clear once it has said so.
    const char *default_title;
    int title_moved;
    // TYPE-AHEAD IS HELD UNTIL THE PROGRAM FIRST SPEAKS: a pty starts
    // echoing and tosh only turns that off once loaded, so keys typed
    // meanwhile were echoed twice. Released on the first output, or by the
    // host after a couple of seconds (uvterm_release_held).
    int spoke;
    char held[256];
    int nheld;
    unsigned long long started_ns;

    // The reader's ring: SINGLE PRODUCER (the reader, at `head`), SINGLE
    // CONSUMER (the main thread, at `tail`), acquire/release on the
    // indices, no lock.
    volatile unsigned head, tail;
    char ring[UVTERM_RING_BYTES];
    pthread_t reader;
    int reader_started;

    // --- the selection -----------------------------------------------------
    struct uvterm_point sel_a, sel_b;
    int selecting;              // a drag is live
    int sel_on;                 // there is a selection to draw and copy

    char *runbuf, *rowbuf;      // draw scratch, cap_cols + 1 each
};

// --- life --------------------------------------------------------------------

// A cleared screen of rows x cols over `sb_rows` of scrollback, default
// pair fg/bg. Keeps (and reuses) the buffers of a terminal used before,
// so a host may recycle a struct whose reader is `done`. 0 on no memory.
int  uvterm_init(struct uvterm *t, int rows, int cols, int sb_rows, int fg, int bg);
void uvterm_free(struct uvterm *t);

// Starts `prog` on a new pty in `dir` (NULL: this process's own), in its
// own session and process group, and the reader thread. 0 on failure.
int  uvterm_spawn(struct uvterm *t, const char *prog, const char *dir);
// SIGTERM to the program and reaped: a closed tab must not leave a
// process on a terminal nobody can type at. The master stays open until
// the reader has finished with it.
void uvterm_stop(struct uvterm *t);

// --- the stream --------------------------------------------------------------

// What the reader has published, into the screen. 1 if anything arrived.
int  uvterm_drain(struct uvterm *t);
// Bytes into the parser, as if the program had printed them.
void uvterm_write(struct uvterm *t, const char *buf, int len);
// Bytes to the program, as typing. Held until it first speaks.
void uvterm_send(struct uvterm *t, const char *buf, int n);
void uvterm_release_held(struct uvterm *t);
// A key the host did not keep for itself, encoded the way a terminal
// sends it (`ESC [ A` for Up; Alt as an ESC prefix) and sent. 1 if sent.
int  uvterm_key(struct uvterm *t, int key, unsigned mods);

// --- geometry ----------------------------------------------------------------

// The grid to rows x cols: shrinking scrolls rows off the top into
// history and growing takes them back (xterm's round trip), the cursor
// clamped, and the program told (SIGWINCH) last. 0 when no memory: the
// size is then clamped to what is allocated.
int  uvterm_resize(struct uvterm *t, int rows, int cols);
// Keeps the NEWEST lines. A failed malloc keeps the old depth.
void uvterm_set_scrollback(struct uvterm *t, int lines);
// A new default pair: every cell still holding the old one follows.
void uvterm_set_default_pair(struct uvterm *t, int fg, int bg);
// Clamped at both ends. 1 if the view moved.
int  uvterm_scroll_to(struct uvterm *t, int sb_view);

// --- the menu's verbs ----------------------------------------------------------

// THE CURSOR'S LINE SURVIVES, moved to the top: it holds the prompt and
// whatever is half-typed, which the program will not redraw on its own
// (Windows Terminal's Clear Buffer). Not on the alternate screen, whose
// program owns every row.
void uvterm_clear_screen(struct uvterm *t);
void uvterm_clear_scrollback(struct uvterm *t);
// What `reset` does: the screen, the parser's colours and modes, the view.
// Not the scrollback -- `reset` on a real terminal leaves history alone.
void uvterm_reset(struct uvterm *t);

// --- reading -----------------------------------------------------------------

// The virtual line view row `r` shows.
int  uvterm_view_line(const struct uvterm *t, int r);
// A virtual line's cells, or NULL off either end.
const struct uvterm_cell *uvterm_line(const struct uvterm *t, int line);
// The cell under (x, y) in a box whose cells are cw x ch -- clamped, and
// snapped to the nearest gap, so a drag past the edge keeps selecting.
struct uvterm_point uvterm_point_at(const struct uvterm *t, int x, int y, int cw, int ch);

// --- the selection -----------------------------------------------------------

void uvterm_sel_clear(struct uvterm *t);
// A press: starts a selection there, or extends one (Shift).
void uvterm_sel_start(struct uvterm *t, struct uvterm_point p, int extend);
void uvterm_sel_extend(struct uvterm *t, struct uvterm_point p);
// The word under `p` (letters, digits, `_`). 0 when there is none.
int  uvterm_sel_word(struct uvterm *t, struct uvterm_point p);
void uvterm_sel_line(struct uvterm *t, struct uvterm_point p);
// The whole virtual buffer, scrollback and screen.
void uvterm_sel_all(struct uvterm *t);
// The text as a person would retype it -- trailing blanks dropped from
// every line but the last, a newline where it crosses a break. Its
// length; `out` may be NULL to measure.
int  uvterm_sel_text(const struct uvterm *t, char *out, int cap);

// --- drawing -----------------------------------------------------------------

enum { UVTERM_CURSOR_BLOCK, UVTERM_CURSOR_UNDER, UVTERM_CURSOR_BAR };

struct uvterm_look {
    int cw, ch;                 // a cell, in the current font
    const uint32_t *pal;        // the 16 colours, VGA-ordered
    uint32_t cursor_rgb;
    int cursor_shape;           // UVTERM_CURSOR_*
    int caret;                  // draw the caret this frame (focus, blink)
};

// The visible rows into (x, y) in the CURRENT font, a selected cell in
// swapped colours. The ground under the grid is the host's to fill.
void uvterm_draw(const struct uvterm *t, struct ugfx_surface *s, int x, int y,
                 const struct uvterm_look *look);
// The caret, when `look->caret` and the program has not hidden it -- a
// separate call so a host can paint between the two (find highlights).
void uvterm_draw_caret(const struct uvterm *t, struct ugfx_surface *s, int x, int y,
                       const struct uvterm_look *look);

#endif
