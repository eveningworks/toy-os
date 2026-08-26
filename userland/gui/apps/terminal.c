// Terminal -- a TERMINAL EMULATOR, with tabs.
//
// It used to be a shell that happened to have a window: it linked
// `tosh` as a LIBRARY, called tosh_run_line() from its key handler, and
// captured output through a sink. That worked, and it left this window
// unable to do the one thing a terminal is for -- `Ctrl-C` did nothing,
// because a window has no console, no foreground group, and no terminal
// at all to have them on.
//
// Now it is what its name has always claimed:
//
//     keys ──► [pty master] ──► line discipline ──► [slave] ──► /bin/tosh
//     paint ◄── [pty master] ◄── line discipline ◄── [slave] ◄──┘
//
// It opens a pty (SYS_OPENPTY), spawns `/bin/tosh` on the slave, writes
// keystrokes into the master and paints what comes out. **The shell in a
// Terminal window is now a REAL PROCESS**, with a pid, visible in `ps`,
// killable, reaped when the window closes.
//
// AND `Ctrl-C` WORKS HERE FOR THE SAME REASON IT WORKS ON THE PHYSICAL
// KEYBOARD -- not for a similar reason. The key arrives as the byte
// 0x03, is written to the master, and `kernel/tty/ldisc.c` recognises
// it as INTR and signals this terminal's foreground group. That is the
// same function, on the same object, as the one the keyboard IRQ feeds.
//
// **ITS SCREEN IS A GRID, WHICH IS WHAT MAKES A FULL-SCREEN PROGRAM
// WORK HERE.** A terminal is a grid of cells with a cursor over it;
// lines that scroll off the top become scrollback, which is the only
// part that is a stream.
//
// **AND THE ANSI PARSER IS THE KERNEL'S OWN, COMPILED TWICE.**
// kernel/lib/ansi.c is a pure state machine that knows nothing about a
// screen, so the physical console and this window resolve `ESC[4;12H`
// through the SAME code. A second parser here would be a second set of
// answers to "what does ESC[0m clear", and the two would drift the
// first time either was extended.
//
// --- TABS, AND WHAT THEY COST ---------------------------------------
//
// **A TAB IS A SESSION: its own pty, its own shell, its own grid,
// scrollback, alternate screen and title.** That is Konsole's model,
// and GNOME Terminal's and Windows Terminal's -- the tab strip belongs
// to the application, because only the application can give a tab a
// terminal's state. Per-tab job control comes free: each shell is
// spawned into its own process group, so `Ctrl-C` reaches the job in
// the tab you are looking at and nothing else.
//
// **EACH SESSION HAS A READER THREAD**, blocked in a real read on its
// pty, which is how a background tab keeps running. Before tabs this
// window polled one master every 30 ms -- 33 wake-ups a second whether
// or not the shell had said anything, and up to 30 ms of lag on every
// echoed keystroke. N tabs would have been N polls. A thread per
// session blocks instead, and there is no cadence left at all: the loop
// wakes on a keystroke, on a compositor event, or because a reader
// posted (ui/uapp.h's uapp_post).
//
// **THE READER TOUCHES NOTHING BUT ITS RING.** It copies bytes into a
// single-producer/single-consumer ring and posts; the MAIN thread
// drains every ring and feeds the parsers. That is the toolkit's rule
// (docs/conventions/gui.md) and it is not a formality here: the grid,
// the scrollback and the parser are read by the painter on every frame.
//
// **A TITLE COMES FROM THE SHELL.** `ESC]0;text BEL` sets the tab's
// label, which is what every terminal does and why a tab can say where
// its shell is standing. The parser handles it (api/ansi.h's ANSI_OSC);
// a session with nothing to say keeps its generated "Shell N".
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "rt/sys.h"
#include "ui/ugfx.h"
#include "ui/uui.h"
#include "ui/uapp.h"
#include "ui/utheme.h"
#include "ui/ulog.h"
#include "ui/uui_tabs.h"
#include "keyboard.h"
#include "ansi.h"   // the kernel's parser, compiled into libuapp too

#define WIN_W 640
#define WIN_H 400
#define MARGIN 6

#define SHELL "/bin/tosh"

// The screen. A fixed grid rather than a resizable one: a window can be
// made large, and 200x60 cells is 36 KiB against a ring-3 heap --
// cheaper than the arithmetic of growing it, and it bounds what a
// program can ask for. What actually varies is g_rows/g_cols, derived
// from the window and told to every child through SYS_TCSETWINSZ.
#define VT_ROWS 60
#define VT_COLS 200
#define SB_ROWS 240   // scrollback lines kept above the screen

// **EIGHT, AND THE REASON IS MEMORY RATHER THAN TASTE.** A session is
// about 220 KiB of grid, scrollback and saved screen, so the cap is
// what bounds this window's heap at under two megabytes. Sessions are
// allocated on demand and their slots REUSED, so a Terminal that never
// opens a second tab pays for one.
#define MAX_TABS 8

// The reader's ring. Big enough that a burst of output does not make the
// reader wait on the UI for every byte, small enough to be noise beside
// the grid it feeds.
#define RING_BYTES 8192

#define TITLE_MAX 40

struct cell { char ch; uint8_t fg, bg; };

struct session {
    // Slot bookkeeping. `live` means this session has a shell; `done`
    // means its reader thread has finished and the slot may be reused.
    int live;
    volatile int done;
    volatile int eof;
    int index;                  // its slot, and what a post carries

    struct cell grid[VT_ROWS][VT_COLS];
    struct cell sb[SB_ROWS][VT_COLS];
    int sb_count;               // lines of scrollback held
    int sb_view;                // how far back the reader has scrolled

    // THE ALTERNATE SCREEN (ESC[?1049h/l). A second grid, plus the
    // cursor the switch saved, so a full-screen program leaves the
    // terminal exactly as it found it -- which is what `less` and `vim`
    // do on every real terminal.
    //
    // A COPY OF THE GRID RATHER THAN A SECOND LIVE ONE. Swapping a
    // pointer between two grids would be cheaper, but everything here
    // indexes the grid directly. The saved copy is written once per
    // switch, not per frame.
    //
    // **SCROLLBACK IS NOT SAVED, DELIBERATELY.** On a real terminal the
    // alternate screen has no scrollback at all -- that is why you
    // cannot scroll back through a `less` session.
    struct cell saved[VT_ROWS][VT_COLS];
    int alt;
    int saved_cr, saved_cc;

    int cr, cc;                 // the cursor, in cells
    int cursor_shown;

    struct ansi_parser vt;

    int master;                 // our end of the pty
    int child;                  // the shell's pid, for reaping and `ps`

    char title[TITLE_MAX];

    // --- the reader thread's ring ------------------------------------
    //
    // SINGLE PRODUCER, SINGLE CONSUMER: the reader writes at `head` and
    // the main thread reads at `tail`, so neither ever writes the
    // other's index and no lock is needed. The acquire/release pairs
    // are what stop the COMPILER reordering a byte's store past the
    // index that publishes it -- x86-64's TSO handles the CPU half
    // (kernel/include/kernel/barrier.h says the same thing about
    // virtqueues).
    volatile unsigned head, tail;
    char ring[RING_BYTES];
    pthread_t reader;
    int reader_started;
};

static struct session *g_slot[MAX_TABS];

// The strip, and the mapping from a TAB position to a session SLOT.
// They differ as soon as a middle tab is closed, and conflating them is
// how a click lands on the wrong shell.
static struct uui_tabs g_strip;
static struct uui_tab  g_tablabels[MAX_TABS];
static int g_tab_slot[MAX_TABS];
static int g_ntabs;

static struct uapp *g_app;
static int g_rows = 24, g_cols = 80;  // one window, so one size for all

// The 16 ANSI colours as RGB. The parser resolves a sequence to an
// `enum vga_color`, which is an INDEX -- turning an index into light is
// the display's business, and the kernel console does exactly the same
// thing with its own table.
static const uint32_t VGA_RGB[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

#define VT_FG 7   // light grey on black -- the console's own default pair
#define VT_BG 0

static struct session *active(void) {
    if (g_ntabs <= 0) return 0;
    int slot = g_tab_slot[g_strip.selected];
    if (slot < 0 || slot >= MAX_TABS) return 0;
    return g_slot[slot];
}

// --- the screen -------------------------------------------------------

static void row_clear(struct cell *row, int from) {
    for (int c = from; c < VT_COLS; c++) {
        row[c].ch = ' ';
        row[c].fg = VT_FG;
        row[c].bg = VT_BG;
    }
}

static void vt_reset_screen(struct session *s) {
    for (int r = 0; r < VT_ROWS; r++) row_clear(s->grid[r], 0);
    s->cr = s->cc = 0;
}

// The top line leaves the screen and becomes history. THE ONLY PLACE A
// STREAM STILL EXISTS -- and it is the right place: scrollback is a
// record of what went past, while the screen is a thing being drawn on.
static void vt_scroll(struct session *s) {
    if (s->sb_count == SB_ROWS) {
        for (int i = 1; i < SB_ROWS; i++)
            for (int c = 0; c < VT_COLS; c++) s->sb[i - 1][c] = s->sb[i][c];
        s->sb_count--;
    }
    for (int c = 0; c < VT_COLS; c++) s->sb[s->sb_count][c] = s->grid[0][c];
    s->sb_count++;

    for (int r = 1; r < g_rows; r++)
        for (int c = 0; c < VT_COLS; c++) s->grid[r - 1][c] = s->grid[r][c];
    row_clear(s->grid[g_rows - 1], 0);
}

static void vt_newline(struct session *s) {
    s->cr++;
    if (s->cr >= g_rows) { s->cr = g_rows - 1; vt_scroll(s); }
}

static void vt_putc_raw(struct session *s, char c) {
    if (c == '\n') { s->cc = 0; vt_newline(s); return; } // no OPOST: LF is CRLF here
    if (c == '\r') { s->cc = 0; return; }
    if (c == '\b') { if (s->cc > 0) s->cc--; return; }
    if (c == '\t') {
        do { s->cc++; } while (s->cc % 8 && s->cc < g_cols);
        if (s->cc >= g_cols) { s->cc = 0; vt_newline(s); }
        return;
    }
    if ((unsigned char)c < 32) return; // anything else unprintable is dropped

    if (s->cc >= g_cols) { s->cc = 0; vt_newline(s); }
    s->grid[s->cr][s->cc].ch = c;
    s->grid[s->cr][s->cc].fg = (uint8_t)s->vt.fg;
    s->grid[s->cr][s->cc].bg = (uint8_t)s->vt.bg;
    s->cc++;
}

static void clamp_cursor(struct session *s) {
    if (s->cr < 0) s->cr = 0;
    if (s->cr >= g_rows) s->cr = g_rows - 1;
    if (s->cc < 0) s->cc = 0;
    if (s->cc >= g_cols) s->cc = g_cols - 1;
}

// One completed cursor/erase sequence. The parser has already applied
// every default -- "a missing or zero count means 1", 1-based rows and
// columns -- so this only has to act, which is the point of it being a
// shared parser rather than a second reading of the spec.
static void vt_ctrl(struct session *s) {
    switch (s->vt.op) {
    case ANSI_OP_MOVE_TO: s->cr = s->vt.a - 1; s->cc = s->vt.b - 1; break;
    case ANSI_OP_UP:      s->cr -= s->vt.a; break;
    case ANSI_OP_DOWN:    s->cr += s->vt.a; break;
    case ANSI_OP_RIGHT:   s->cc += s->vt.a; break;
    case ANSI_OP_LEFT:    s->cc -= s->vt.a; break;
    case ANSI_OP_COLUMN:  s->cc = s->vt.a - 1; break;
    case ANSI_OP_ROW:     s->cr = s->vt.a - 1; break;
    case ANSI_OP_ERASE_LINE:
        if (s->vt.a == 0) row_clear(s->grid[s->cr], s->cc);
        else if (s->vt.a == 1)
            for (int c = 0; c <= s->cc && c < VT_COLS; c++) s->grid[s->cr][c].ch = ' ';
        else row_clear(s->grid[s->cr], 0);
        break;
    case ANSI_OP_ERASE_DISPLAY:
        if (s->vt.a == 0) {
            row_clear(s->grid[s->cr], s->cc);
            for (int r = s->cr + 1; r < g_rows; r++) row_clear(s->grid[r], 0);
        } else if (s->vt.a == 1) {
            for (int r = 0; r < s->cr; r++) row_clear(s->grid[r], 0);
            for (int c = 0; c <= s->cc && c < VT_COLS; c++) s->grid[s->cr][c].ch = ' ';
        } else {
            for (int r = 0; r < g_rows; r++) row_clear(s->grid[r], 0);
        }
        break;
    case ANSI_OP_ALT_ON:
        // Idempotent: a program that switches twice must not overwrite
        // the screen it saved the first time with the alternate one.
        if (!s->alt) {
            for (int r = 0; r < VT_ROWS; r++)
                for (int c = 0; c < VT_COLS; c++) s->saved[r][c] = s->grid[r][c];
            s->saved_cr = s->cr;
            s->saved_cc = s->cc;
            s->alt = 1;
        }
        for (int r = 0; r < g_rows; r++) row_clear(s->grid[r], 0);
        s->cr = s->cc = 0;
        break;
    case ANSI_OP_ALT_OFF:
        if (s->alt) {
            for (int r = 0; r < VT_ROWS; r++)
                for (int c = 0; c < VT_COLS; c++) s->grid[r][c] = s->saved[r][c];
            s->cr = s->saved_cr;
            s->cc = s->saved_cc;
            s->alt = 0;
        }
        break;
    case ANSI_OP_SHOW: s->cursor_shown = 1; break;
    case ANSI_OP_HIDE: s->cursor_shown = 0; break;
    // SAVE/RESTORE have no users here yet, and a half-remembered
    // position is worse than none.
    default: break;
    }
    clamp_cursor(s);
}

// A title the SHELL sent. Copied rather than pointed at: the parser
// rewrites its own buffer on the next OSC, and the tab strip holds this
// pointer for as long as the tab exists.
static void session_default_title(struct session *s, char *dst) {
    const char *d = "Shell ";
    int n = 0;
    while (d[n]) { dst[n] = d[n]; n++; }
    dst[n++] = (char)('1' + s->index);
    dst[n] = '\0';
}

static void vt_title(struct session *s) {
    char want[TITLE_MAX];
    int i = 0;
    while (s->vt.osc[i] && i < TITLE_MAX - 1) { want[i] = s->vt.osc[i]; i++; }
    want[i] = '\0';
    // An EMPTY title is a request to go back to the default rather than
    // a request for a blank tab -- a tab with no label is unreadable,
    // and a shell clearing its title is common.
    if (i == 0) session_default_title(s, want);

    if (strcmp(want, s->title) == 0) return;   // nothing to say
    strlcpy(s->title, want, sizeof s->title);

    // ON ITS OWN LINE, and only when it CHANGED: a title is text, so a
    // test can assert on it rather than on the pixels of a tab label,
    // and a shell that re-announces the same directory must not fill
    // the log. Not part of the layout line, because a title may contain
    // spaces and that line is parsed field by field.
    ulogf("uterm: tab %d title %s\n", s->index, s->title);
}

static void vt_write(struct session *s, const char *buf, int len) {
    for (int i = 0; i < len; i++) {
        switch (ansi_feed(&s->vt, buf[i])) {
        case ANSI_PASS:  vt_putc_raw(s, buf[i]); break;
        case ANSI_CTRL:  vt_ctrl(s); break;
        case ANSI_OSC:   vt_title(s); break;
        case ANSI_SGR:   break; // the colours are read off the parser per cell
        case ANSI_EATEN: break;
        }
    }
    // NEW OUTPUT PINS THE VIEW TO THE BOTTOM, which is what every
    // terminal does: a program printing while you are reading history
    // brings you back, because otherwise the thing you asked to run
    // appears to have done nothing.
    s->sb_view = 0;
}

// --- the reader thread ------------------------------------------------
//
// **IT TOUCHES `ring`, `head`, `eof` AND `done`, AND NOTHING ELSE.**
// Not the grid, not the parser, not a widget -- see ui/uapp.h. The main
// thread owns everything a frame reads.

static void *reader_main(void *arg) {
    struct session *s = (struct session *)arg;
    char buf[512];

    for (;;) {
        // BLOCKING, which is the whole point: this is the read the old
        // 30 ms poll was standing in for.
        int64_t n = sys_read(s->master, buf, sizeof buf);
        if (n <= 0) break;   // the shell is gone, or the master went away

        for (int64_t i = 0; i < n; i++) {
            unsigned head = __atomic_load_n(&s->head, __ATOMIC_RELAXED);
            unsigned next = (head + 1) % RING_BYTES;
            // BACK-PRESSURE RATHER THAN DROPPING. A full ring means the
            // window has not drained yet; waiting costs this thread a
            // slice, while dropping would corrupt the screen in a way
            // nobody could diagnose from the result.
            while (next == __atomic_load_n(&s->tail, __ATOMIC_ACQUIRE))
                sys_yield();
            s->ring[head] = buf[i];
            __atomic_store_n(&s->head, next, __ATOMIC_RELEASE);
        }
        uapp_post(g_app, s->index, 0);
    }

    s->eof = 1;
    uapp_post(g_app, s->index, 1);
    // LAST, and it is what lets the slot be reused: nothing after this
    // touches the session, so the main thread may hand it to a new tab.
    __atomic_store_n(&s->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

// Move whatever the reader has published into the parser. Returns 1 if
// anything arrived, so the caller repaints only when there is something
// new.
static int drain(struct session *s) {
    int got = 0;
    for (;;) {
        unsigned tail = __atomic_load_n(&s->tail, __ATOMIC_RELAXED);
        if (tail == __atomic_load_n(&s->head, __ATOMIC_ACQUIRE)) break;
        char c = s->ring[tail];
        __atomic_store_n(&s->tail, (tail + 1) % RING_BYTES, __ATOMIC_RELEASE);
        vt_write(s, &c, 1);
        got = 1;
    }
    return got;
}

// --- sessions ---------------------------------------------------------

static void tabs_rebuild(void) {
    g_ntabs = 0;
    for (int i = 0; i < MAX_TABS; i++) {
        struct session *s = g_slot[i];
        if (!s || !s->live) continue;
        g_tab_slot[g_ntabs] = i;
        g_tablabels[g_ntabs].label = s->title;
        // A LONE TAB HAS NO CLOSE BOX, because closing it is closing the
        // window and the window already has an X. Konsole hides the whole
        // strip at one tab, which is the same judgement.
        g_tablabels[g_ntabs].closable = 1;
        g_ntabs++;
    }
    g_strip.count = g_ntabs;
    if (g_strip.selected >= g_ntabs) g_strip.selected = g_ntabs - 1;
    if (g_strip.selected < 0) g_strip.selected = 0;
}

static int session_start(int slot) {
    struct session *s = g_slot[slot];
    if (!s) {
        s = (struct session *)malloc(sizeof *s);
        if (!s) return 0;
        g_slot[slot] = s;
    }
    // Zeroed whether it is new or reused -- a recycled session must not
    // inherit the last shell's screen.
    char *raw = (char *)s;
    for (unsigned i = 0; i < sizeof *s; i++) raw[i] = 0;
    s->index = slot;
    s->master = -1;
    s->cursor_shown = 1;
    ansi_init(&s->vt, VT_FG, VT_BG);
    vt_reset_screen(s);

    session_default_title(s, s->title);

    int slave = -1;
    if (sys_openpty(&s->master, &slave) < 0) return 0;

    // NO sys_set_nonblock() ANY MORE, and its absence is the change: a
    // reader thread WANTS to block. The flag used to be needed because
    // the drain ran on the window's tick and a blocking read there would
    // have frozen the window rather than slowed it.

    // The child's 0/1/2 are the slave. dup2 around the spawn, exactly as
    // tosh's own redirection does -- SYS_SPAWN inherits the descriptor
    // table, so placing them here is placing them in the child.
    int in0 = sys_dup(0), out1 = sys_dup(1), err2 = sys_dup(2);
    sys_dup2(slave, 0);
    sys_dup2(slave, 1);
    sys_dup2(slave, 2);
    // ITS OWN GROUP, which is what makes it interruptible as a unit --
    // and with tabs it is also what keeps one tab's Ctrl-C out of
    // another's: the group is per session, so the signal reaches the job
    // in the tab you are looking at.
    s->child = sys_spawn_group(SHELL, 0, -1, 0, PGID_NEW);
    if (in0  >= 0) { sys_dup2(in0, 0);  sys_close(in0); }
    if (out1 >= 0) { sys_dup2(out1, 1); sys_close(out1); }
    if (err2 >= 0) { sys_dup2(err2, 2); sys_close(err2); }

    // OUR copy of the slave goes now. The child holds its own through
    // the fds above, and keeping this one would mean the master never
    // sees end-of-file when the shell dies.
    sys_close(slave);

    if (s->child < 0) { sys_close(s->master); s->master = -1; return 0; }

    struct tty_winsize ws = { (uint16_t)g_rows, (uint16_t)g_cols };
    sys_tcsetwinsz(s->master, &ws);

    s->live = 1;
    s->done = 0;
    // DETACHED: nothing joins it. Joining would mean blocking the UI
    // until a shell that may be ignoring its terminal decides to exit,
    // and the slot is recycled on `done` instead -- which is the same
    // information a join would have carried.
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&s->reader, &at, reader_main, s) != 0) {
        ulog("uterm: could not start a reader thread\n");
        s->live = 0;
        sys_close(s->master);
        s->master = -1;
        return 0;
    }
    s->reader_started = 1;
    return 1;
}

// END THE SHELL, DO NOT ORPHAN IT. Closing the master alone would give
// it end-of-file and it would exit on its own -- but only when it NEXT
// READS, and a shell waiting on a job does not read for as long as that
// job runs. A tab that has gone must not leave a process on a terminal
// nobody can type at.
//
// SIGTERM rather than SIGKILL: this is the polite one, and the shell is
// not being force-quit -- the person closed its tab.
static void session_stop(struct session *s) {
    if (!s || !s->live) return;
    if (s->child > 0) {
        sys_kill(s->child, SIGTERM);
        int status = 0;
        sys_waitpid(s->child, &status);
        s->child = 0;
    }
    s->live = 0;
    // The master stays OPEN until the reader has finished with it --
    // closing an fd a blocked thread is reading is not something this
    // kernel defines. Killing the shell is what ends that read.
}

static int open_tab(void) {
    int slot = -1;
    for (int i = 0; i < MAX_TABS; i++) {
        struct session *s = g_slot[i];
        if (!s) { slot = i; break; }
        // A slot whose reader has finished is free. One whose reader is
        // still winding down is NOT -- handing it to a new tab would
        // hand two threads one ring.
        if (!s->live && s->done) { slot = i; break; }
    }
    if (slot < 0) return 0;
    if (!session_start(slot)) return 0;
    tabs_rebuild();
    for (int i = 0; i < g_ntabs; i++)
        if (g_tab_slot[i] == slot) g_strip.selected = i;
    return 1;
}

static void close_tab(int tab) {
    if (tab < 0 || tab >= g_ntabs) return;
    struct session *s = g_slot[g_tab_slot[tab]];
    session_stop(s);
    tabs_rebuild();
    if (g_ntabs == 0 && g_app) uapp_quit(g_app, 0);
}

// --- drawing ----------------------------------------------------------
//
// A ROW AT A TIME, IN RUNS OF ONE COLOUR PAIR. Per-cell drawing would be
// 12,000 calls a frame at this size; a run is one ugfx_draw_string() and
// there is usually one run per row. The background is painted per run
// too, which is what makes reverse video -- a status bar -- look like a
// bar rather than like coloured letters.

static int strip_h(void) {
    // ONE TAB SHOWS NO STRIP, which is Konsole's default and what keeps
    // a single-shell Terminal exactly the window it has always been --
    // same geometry, same tests.
    return g_ntabs > 1 ? uui_tabs_height() : 0;
}

static void draw_run(struct ugfx_surface *s, int x, int y,
                     const char *text, int n, uint8_t fg, uint8_t bg) {
    if (n <= 0) return;
    char buf[VT_COLS + 1];
    for (int i = 0; i < n; i++) buf[i] = text[i];
    buf[n] = '\0';

    // **THE BACKGROUND IS A RECTANGLE, NOT THE STRING CALL'S `bg`.**
    // ugfx_draw_string() blends the glyph's own pixels against that
    // colour; it does not fill the CELL. For ordinary text the two look
    // identical, and for REVERSE VIDEO they are not: a status bar came
    // out as dark letters on black instead of black letters on a bar --
    // legible, and not what was asked for. Caught by reading pixel
    // values rather than by looking at the screenshot.
    if ((bg & 15) != VT_BG)
        ugfx_fill_rect(s, x, y, n * ugfx_char_w(), ugfx_char_h(), VGA_RGB[bg & 15]);
    ugfx_draw_string(s, x, y, buf, VGA_RGB[fg & 15], VGA_RGB[bg & 15]);
}

static void draw_row(struct ugfx_surface *s, const struct cell *row, int y) {
    int cw = ugfx_char_w();
    int i = 0;
    while (i < g_cols) {
        // TRAILING BLANKS IN THE DEFAULT COLOURS ARE NOT DRAWN -- the
        // surface is already that colour, and drawing them would cost a
        // full row of glyphs per line for nothing.
        int j = i;
        while (j < g_cols && row[j].fg == row[i].fg && row[j].bg == row[i].bg) j++;
        int blank = 1;
        for (int k = i; k < j; k++) if (row[k].ch != ' ') { blank = 0; break; }
        if (!(blank && row[i].bg == VT_BG)) {
            char run[VT_COLS];
            for (int k = i; k < j; k++) run[k - i] = row[k].ch;
            draw_run(s, MARGIN + i * cw, y, run, j - i, row[i].fg, row[i].bg);
        }
        i = j;
    }
}

static void draw(struct ugfx_surface *s, int focused) {
    struct session *ses = active();
    int top = strip_h();
    // ONLY THE GRID AREA, not the whole surface: the toolkit paints the
    // tab strip after this runs, and a full-surface fill here would wipe
    // whatever it had already put down (ui/uapp.c's draw order).
    ugfx_fill_rect(s, 0, top, s->w, s->h - top, VGA_RGB[VT_BG]);
    if (!ses) return;

    int ch = ugfx_char_h(), cw = ugfx_char_w();

    // Scrolled back: the top rows come from history, the rest from the
    // screen, and they meet without a seam because both are the same
    // grid of cells. That is the payoff of scrollback being made of
    // evicted ROWS rather than of a character stream.
    for (int r = 0; r < g_rows; r++) {
        int back = ses->sb_view - r;      // >0 means this row is history
        const struct cell *row;
        if (back > 0) {
            int idx = ses->sb_count - back;
            if (idx < 0) continue;        // before the oldest line we kept
            row = ses->sb[idx];
        } else {
            row = ses->grid[-back];
        }
        draw_row(s, row, top + MARGIN + r * ch);
    }

    // The caret, only while FOCUSED and only while the program wants it
    // shown (`ESC[?25l` hides it -- a full-screen program parking the
    // caret somewhere meaningless turns it off rather than moving it).
    // An unfocused window drawing a caret claims to be taking input that
    // is going somewhere else.
    if (focused && ses->cursor_shown && ses->sb_view == 0)
        ugfx_fill_rect(s, MARGIN + ses->cc * cw, top + MARGIN + ses->cr * ch,
                       2, ch, VGA_RGB[VT_FG]);
}

// One line, content-relative, on stderr -- the grammar every GUI test
// tool here asserts on. The CURSOR, as a cell index, because that is the
// thing that would be wrong if a cursor sequence were mishandled and it
// is what a test can predict.
static void log_layout(void) {
    struct session *s = active();
    if (!s) return;
    // **THE FIRST FIELD AFTER `cursor` IS A LINEAR CELL OFFSET, and it
    // has to stay one**: tools/uterm_test.py reads it with
    // `split()[0]`, so a row/column PAIR there parses as neither. Extra
    // fields are safe after it and are what a tabbed window adds.
    char b[96];
    snprintf(b, sizeof b, "uterm: layout cursor %d rows %d cols %d tabs %d\n",
             s->cr * g_cols + s->cc, g_rows, g_cols, g_ntabs);
    uapp_log_layout_line(b);
}

static void size_changed(int w, int h);

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    struct ugfx_surface *s = uapp_surface(d);
    // MEASURED HERE AS WELL AS ON RESIZE, because this is the first
    // moment the real surface exists -- and because the FONT can change
    // under a running client (WIN_EV_FONT), which changes the cell size
    // without changing the window's. It early-outs when nothing moved.
    size_changed(s->w, s->h);
    // The strip is positioned here rather than by a layout: this window
    // has one widget over a canvas it paints itself, and running a
    // uui_layout for that would be more machinery than arithmetic.
    uui_tabs_set_geometry(&g_strip, 0, 0, s->w, uui_tabs_height());
    draw(s, uapp_focused(a));
    log_layout();
}

// HOW BIG THE WINDOW IS, IN CELLS, AND EVERY CHILD IS TOLD. Only this
// process can work that out: the size is pixels and the answer is cells,
// and the conversion needs the font. SYS_TCSETWINSZ is how the shell and
// anything it runs find out -- /bin/edit asks for it before it draws a
// single row.
//
// EVERY SESSION, not just the visible one: a background shell that
// learned the wrong size would draw its next full-screen program wrong
// the moment you switched to it.
static void size_changed(int w, int h) {
    int cw = ugfx_char_w(), ch = ugfx_char_h();
    if (cw <= 0 || ch <= 0) return;
    int rows = (h - strip_h() - 2 * MARGIN) / ch;
    int cols = (w - 2 * MARGIN) / cw;
    if (rows < 2) rows = 2;
    if (cols < 8) cols = 8;
    if (rows > VT_ROWS) rows = VT_ROWS;
    if (cols > VT_COLS) cols = VT_COLS;
    if (rows == g_rows && cols == g_cols) return;

    g_rows = rows;
    g_cols = cols;
    struct tty_winsize ws = { (uint16_t)rows, (uint16_t)cols };
    for (int i = 0; i < MAX_TABS; i++) {
        struct session *s = g_slot[i];
        if (!s || !s->live || s->master < 0) continue;
        clamp_cursor(s);
        sys_tcsetwinsz(s->master, &ws);
    }
}

static void on_resize(struct uapp *a, int w, int h) {
    (void)a;
    size_changed(w, h);
}

// --- input ------------------------------------------------------------

// THE WHEEL SCROLLS THE SCROLLBACK, and it is the same state Page
// Up/Down moves -- not a second notion of "where the reader is".
//
// Three lines per notch, which is what every desktop terminal does and
// what the toolkit's other scroll consumers use; a notch is a detent,
// not a line. `notches` is positive AWAY from the user (win_proto.h),
// and away means BACK INTO HISTORY.
#define WHEEL_LINES 3

static void on_wheel(struct uapp *a, int notches) {
    struct session *s = active();
    if (!s) return;
    int want = s->sb_view + notches * WHEEL_LINES;
    // Clamped rather than wrapped, and clamped at BOTH ends: scrolling
    // past the oldest line must stop there, and scrolling forward past
    // the live screen must land exactly on it (0) rather than going
    // negative, which would index above the top of the buffer.
    if (want > s->sb_count) want = s->sb_count;
    if (want < 0) want = 0;
    if (want == s->sb_view) return;   // nothing moved -- do not repaint
    s->sb_view = want;
    uapp_redraw(a);
}

// Every ring, not just the visible one: a background tab must keep up,
// and its bytes are already waiting whether or not anyone is looking.
static int drain_all(void) {
    int painted = 0;
    for (int i = 0; i < MAX_TABS; i++) {
        struct session *s = g_slot[i];
        if (!s) continue;
        int got = drain(s);
        struct session *cur = active();
        if (got && s == cur) painted = 1;
    }
    return painted;
}

// A tab whose shell has gone closes itself, exactly as the window used
// to. Checked after a drain so the shell's last output is on screen
// before its tab disappears.
static void reap_dead_tabs(void) {
    for (int i = 0; i < MAX_TABS; i++) {
        struct session *s = g_slot[i];
        if (!s || !s->live || !s->eof) continue;
        session_stop(s);
        tabs_rebuild();
    }
    if (g_ntabs == 0 && g_app) uapp_quit(g_app, 0);
}

// Ctrl+Shift+<letter>: the letter has ALREADY been folded to a control
// code by the time it arrives (api/keyboard.h says so plainly), so
// Ctrl+Shift+T is 0x14 with the SHIFT bit still set -- Shift is the one
// modifier that does not fold the key away. Matching the control code
// plus Shift is the only way to spell these here.
#define CTRL_SHIFT_T 0x14
#define CTRL_SHIFT_W 0x17

static void on_key(struct uapp *a, int key, unsigned mods) {
    // --- the tab bindings, which are Konsole's ------------------------
    if ((mods & KEY_MOD_SHIFT) && key == CTRL_SHIFT_T) {
        if (open_tab()) uapp_redraw(a);
        return;
    }
    if ((mods & KEY_MOD_SHIFT) && key == CTRL_SHIFT_W) {
        close_tab(g_strip.selected);
        uapp_redraw(a);
        return;
    }
    if ((mods & KEY_MOD_CTRL) && (key == KEY_PAGE_UP || key == KEY_PAGE_DOWN)) {
        if (g_ntabs > 1) {
            int next = g_strip.selected + (key == KEY_PAGE_DOWN ? 1 : -1);
            if (next < 0) next = g_ntabs - 1;      // wraps, as Konsole does
            if (next >= g_ntabs) next = 0;
            uui_tabs_select(&g_strip, next);
            uapp_redraw(a);
        }
        return;
    }

    struct session *s = active();
    if (!s) return;

    // PAGE UP/DOWN SCROLL AND ARE NOT THE SHELL'S. Everything else --
    // including the arrows, Home, End and every Ctrl combination -- goes
    // to the shell as a byte, because THE SHELL HAS THE LINE EDITOR.
    // This window deciding what Ctrl-A means would be the second
    // implementation kernel/lib/klineedit.c exists to prevent.
    if (key == KEY_PAGE_UP) {
        s->sb_view += g_rows / 2;
        if (s->sb_view > s->sb_count) s->sb_view = s->sb_count;
        uapp_redraw(a);
        return;
    }
    if (key == KEY_PAGE_DOWN) {
        s->sb_view -= g_rows / 2;
        if (s->sb_view < 0) s->sb_view = 0;
        uapp_redraw(a);
        return;
    }

    // EVERY CODE THIS TOOLKIT DELIVERS FITS IN A BYTE -- specials are
    // 0x91-0xA6, which ARE the KEY_* values the shared line editor
    // switches on (api/keyboard.h), and Ctrl/Alt arrive as control codes
    // and an ESC prefix, terminal-style. So there is no translation
    // layer here, which is the point: a translation layer would be a
    // third place for the keymap to drift.
    char b = (char)(unsigned char)key;
    if (s->master >= 0) sys_write(s->master, &b, 1);

    // NO DRAIN HERE ANY MORE. The echo comes back through the discipline
    // and the reader thread is already blocked waiting for it, so it
    // arrives as a post within microseconds instead of on the next tick.
    // Draining here used to be what kept typing feeling immediate.
}

// A reader posted. `a0` is the session's slot and `a1` is 1 for EOF --
// neither is trusted for anything except knowing there is work: the
// drain reads every ring regardless, so a post that was dropped because
// the queue was full costs nothing.
static int on_user(struct uapp *a, int a0, int a1) {
    (void)a; (void)a0; (void)a1;
    int painted = drain_all();
    reap_dead_tabs();
    return painted;
}

// --- lifecycle --------------------------------------------------------

static void tab_selected(void *ctx, int index) {
    (void)ctx; (void)index;
    if (g_app) uapp_redraw(g_app);
}

static void tab_closed(void *ctx, int index) {
    (void)ctx;
    close_tab(index);
    if (g_app) uapp_redraw(g_app);
}

static void on_open_cb(struct uapp *a) {
    // The whole grid is text, so this is named ONCE rather than tracked
    // from motion -- which is why this app needs no on_motion at all.
    // xterm does the same, scrollbar included.
    uapp_set_cursor(a, WIN_CURSOR_TEXT);
    g_app = a;

    uui_tabs_init(&g_strip, g_tablabels, 0, 0);
    g_strip.on_select = tab_selected;
    g_strip.on_close  = tab_closed;

    if (!open_tab()) {
        ulog("uterm: could not open a pty or start " SHELL "\n");
        uapp_quit(a, 1);
    }
}

static int on_close_cb(struct uapp *a) {
    (void)a;
    for (int i = 0; i < MAX_TABS; i++) session_stop(g_slot[i]);
    return 1; // yes, close
}

// The tab strip is declared for INPUT only. It is drawn by the toolkit
// after on_draw (ui/uapp.c's order) and positioned by on_draw itself --
// see ui/uapp.h on why a widget array is two declarations and what
// happens when an app writes only one.
static struct uui_item g_widgets[] = {
    { .ops = &uui_tabs_ops, .widget = &g_strip, .id = 1 },
};

int main(void) {
    struct uapp_desc desc = {
        .title   = "Terminal",
        .app_id  = "terminal",
        .w       = WIN_W,
        .h       = WIN_H,
        .x       = 120,
        .y       = 120,
        .flags   = UAPP_RESIZABLE,
        .min_w   = 280,
        .min_h   = 140,
        // **NO tick_ms AND NO on_tick.** This window used to poll its
        // one master every 30 ms because there is no poll() to wait on
        // two things at once. A reader thread per session blocks on the
        // pty instead and posts when it has bytes, so the loop now
        // blocks on the compositor alone and wakes only when something
        // has actually happened.
        .widgets = g_widgets,
        .widget_count = (int)(sizeof g_widgets / sizeof g_widgets[0]),
        .on_open = on_open_cb,
        .on_draw = on_draw,
        .on_key  = on_key,
        .on_user = on_user,
        .on_wheel = on_wheel,
        .on_resize = on_resize,
        .on_close = on_close_cb,
    };
    return uapp_run(&desc);
}
