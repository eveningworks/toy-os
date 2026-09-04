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
#include "ui/uui_menubar.h"
#include "ui/uui_textbox.h"
#include "keyboard.h"
#include "ansi.h"   // the kernel's parser, compiled into libuapp too

#define WIN_W 640
#define WIN_H 400
#define MARGIN 6

#define SHELL "/bin/tosh"

// The screen. The grid GROWS to fit the window rather than being a
// fixed 200x60: the display ceiling is 1920x1080 (WIN_CLIENT_MAX_W/H)
// and a maximized terminal there wants ~240x67 cells, so the old fixed
// cap left a dead band below the last row and to the right of the last
// column, silently. xterm reallocates on resize and CROPS (no reflow);
// so does this. What bounds it now is the WINDOW, which the compositor
// already clamps -- a program still cannot ask for an unbounded grid.
//
// ONE ALLOCATED GEOMETRY FOR EVERY SESSION (g_cap_rows x g_cap_cols):
// there is one window, so every tab is the same size, and capacity only
// ever grows -- shrinking the window narrows g_rows/g_cols and leaves
// the buffers alone, exactly as the fixed grid did.
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

    // Heap-backed, g_cap_rows/g_cap_cols geometry, allocated at first
    // use and REALLOCATED by grow_caps() when the window outgrows them.
    // session_start() zeroes the whole struct on slot reuse, so it
    // saves and restores these three pointers around the wipe.
    struct cell *grid;          // g_cap_rows x g_cap_cols
    struct cell *sb;            // SB_ROWS   x g_cap_cols
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
    struct cell *saved;         // g_cap_rows x g_cap_cols
    int alt;
    int saved_cr, saved_cc;

    int cr, cc;                 // the cursor, in cells
    int cursor_shown;

    struct ansi_parser vt;

    int master;                 // our end of the pty
    int child;                  // the shell's pid, for reaping and `ps`

    char title[TITLE_MAX];
    // A HAND-GIVEN TITLE OUTRANKS THE SHELL'S. Konsole's rule: once you
    // name a tab, its shell's OSC sequences stop moving the label --
    // otherwise the next `cd` silently undoes the rename.
    int title_locked;

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

// --- the menu bar -----------------------------------------------------
//
// **THERE IS NO EDIT MENU, AND THAT IS NOT AN OVERSIGHT.** Copy and
// Paste are what an Edit menu is for and this OS has no clipboard at
// all (docs/conventions/gui.md), so the menu would be two permanently
// greyed rows advertising something that does not exist.
//
// The bar can be hidden, as Konsole's can, because a terminal is the one
// app where a row of chrome is a row of the product. **F10 brings it
// back** -- Konsole's own Ctrl+Shift+M is unavailable here, since Ctrl
// folds 'M' to 0x0D and the binding would be indistinguishable from
// Shift+Enter (api/keyboard.h).
enum {
    CMD_NEW_TAB = 1, CMD_CLOSE_TAB, CMD_EXIT,
    CMD_RENAME, CMD_CLEAR_SCREEN, CMD_CLEAR_SB, CMD_RESET,
    CMD_INTR, CMD_EOF,
    CMD_TOP, CMD_BOTTOM, CMD_MENUBAR,
    CMD_NEXT_TAB, CMD_PREV_TAB,
};

static const struct uui_menu_item file_items[] = {
    UUI_MENU("New Tab",       CMD_NEW_TAB,   "Ctrl+Shift+T"),
    UUI_MENU("Close Tab",     CMD_CLOSE_TAB, "Ctrl+Shift+W"),
    UUI_MENU_SEP,
    UUI_MENU("Exit",          CMD_EXIT,      "Alt+F4"),
};

static const struct uui_menu_item term_items[] = {
    UUI_MENU("Rename Tab...", CMD_RENAME,       0),
    UUI_MENU_SEP,
    UUI_MENU("Clear Screen",     CMD_CLEAR_SCREEN, 0),
    UUI_MENU("Clear Scrollback", CMD_CLEAR_SB,     0),
    UUI_MENU("Reset Terminal",   CMD_RESET,        0),
    UUI_MENU_SEP,
    UUI_MENU("Send Interrupt", CMD_INTR, "Ctrl-C"),
    UUI_MENU("Send EOF",       CMD_EOF,  "Ctrl-D"),
};

static const struct uui_menu_item view_items[] = {
    UUI_MENU("Scroll to Top",    CMD_TOP,    "PgUp"),
    UUI_MENU("Scroll to Bottom", CMD_BOTTOM, "PgDn"),
    UUI_MENU_SEP,
    UUI_MENU("Menu Bar",         CMD_MENUBAR, "F10"),
};

static const struct uui_menu_item tabs_items[] = {
    UUI_MENU("Next Tab",     CMD_NEXT_TAB, "Ctrl+PgDn"),
    UUI_MENU("Previous Tab", CMD_PREV_TAB, "Ctrl+PgUp"),
};

static const struct uui_menu_item menu_bar[] = {
    UUI_SUBMENU("File",     file_items),
    UUI_SUBMENU("Terminal", term_items),
    UUI_SUBMENU("View",     view_items),
    UUI_SUBMENU("Tabs",     tabs_items),
};

static struct uui_menubar g_menu;
static int g_menu_shown = 1;

// --- renaming a tab ---------------------------------------------------
//
// A field over the GRID, never over the strip: the grid carries no
// routed widget, so the prompt cannot be clicked through onto a tab.
// A press anywhere outside it cancels, which is what a lightweight
// prompt does -- this is not a modal and does not pretend to be one.
static struct uui_textbox g_rename;
static int g_rename_open;
static int g_rename_slot = -1;

// The chrome, declared for INPUT and drawn by the toolkit after on_draw
// (ui/uapp.c's order); on_draw positions both -- see ui/uapp.h on why a
// widget array is two declarations and what happens when an app writes
// only one.
//
// **THE MENU BAR IS FIRST, AND THE ORDER IS LOAD-BEARING.** Its popup
// drops over the tab strip, and a routed widget claiming an overlay is
// offered every press before anything is hit-tested (uui_route.c) -- so
// declaring the menu here is what stops a click on the File menu's first
// row ALSO landing on the tab underneath it.
#define ID_MENU 1
#define ID_TABS 2
static struct uui_item g_widgets[] = {
    { .ops = &uui_menubar_ops, .widget = &g_menu,  .id = ID_MENU, .name = "menu" },
    { .ops = &uui_tabs_ops,    .widget = &g_strip, .id = ID_TABS, .name = "tabs" },
};

static struct uapp *g_app;
static int g_rows = 24, g_cols = 80;  // one window, so one size for all

// The ALLOCATED geometry every session's buffers share, and the draw
// scratch sized to it. Grows in grow_caps(), never shrinks.
static int g_cap_rows, g_cap_cols;
static char *g_runbuf;   // draw_run's text scratch, g_cap_cols + 1
static char *g_rowbuf;   // draw_row's run scratch,  g_cap_cols + 1

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

// Row accessors: the buffers are flat, strided by the ALLOCATED width,
// which is what lets one realloc change the geometry without touching
// any of the code below.
static struct cell *grid_row(struct session *s, int r) {
    return s->grid + (size_t)r * g_cap_cols;
}
static struct cell *sb_row(struct session *s, int r) {
    return s->sb + (size_t)r * g_cap_cols;
}
static struct cell *saved_row(struct session *s, int r) {
    return s->saved + (size_t)r * g_cap_cols;
}

static struct cell *cells_new(int rows, int cols) {
    size_t n = (size_t)rows * (size_t)cols;
    struct cell *p = (struct cell *)malloc(n * sizeof *p);
    if (!p) return 0;
    for (size_t i = 0; i < n; i++) {
        p[i].ch = ' '; p[i].fg = VT_FG; p[i].bg = VT_BG;
    }
    return p;
}

// Grows the shared geometry to hold rows x cols, migrating EVERY live
// session's buffers (a crop-preserving copy -- xterm's behaviour; there
// is no reflow). Two passes on purpose: allocate everything first, then
// swap, so a failed malloc leaves no session with a stride the globals
// do not describe. Returns 0 on failure with everything as it was --
// the caller then clamps to the old capacity, which is exactly the old
// fixed-grid behaviour.
static int grow_caps(int rows, int cols) {
    if (rows <= g_cap_rows && cols <= g_cap_cols) return 1;
    int nr = rows > g_cap_rows ? rows : g_cap_rows;
    int nc = cols > g_cap_cols ? cols : g_cap_cols;

    struct cell *ng[MAX_TABS] = {0}, *nb[MAX_TABS] = {0}, *nv[MAX_TABS] = {0};
    char *rb = (char *)malloc((size_t)nc + 1);
    char *ob = (char *)malloc((size_t)nc + 1);
    int ok = rb && ob;
    for (int i = 0; ok && i < MAX_TABS; i++) {
        struct session *s = g_slot[i];
        if (!s || !s->grid) continue;
        ng[i] = cells_new(nr, nc);
        nb[i] = cells_new(SB_ROWS, nc);
        nv[i] = cells_new(nr, nc);
        if (!ng[i] || !nb[i] || !nv[i]) ok = 0;
    }
    if (!ok) {
        for (int i = 0; i < MAX_TABS; i++) { free(ng[i]); free(nb[i]); free(nv[i]); }
        free(rb); free(ob);
        return 0;
    }

    for (int i = 0; i < MAX_TABS; i++) {
        struct session *s = g_slot[i];
        if (!s || !s->grid) continue;
        for (int r = 0; r < g_cap_rows; r++)
            for (int c = 0; c < g_cap_cols; c++) {
                ng[i][(size_t)r * nc + c] = grid_row(s, r)[c];
                nv[i][(size_t)r * nc + c] = saved_row(s, r)[c];
            }
        for (int r = 0; r < SB_ROWS; r++)
            for (int c = 0; c < g_cap_cols; c++)
                nb[i][(size_t)r * nc + c] = sb_row(s, r)[c];
        free(s->grid); free(s->sb); free(s->saved);
        s->grid = ng[i]; s->sb = nb[i]; s->saved = nv[i];
    }
    free(g_runbuf); g_runbuf = rb;
    free(g_rowbuf); g_rowbuf = ob;
    g_cap_rows = nr;
    g_cap_cols = nc;
    return 1;
}

static void row_clear(struct cell *row, int from) {
    for (int c = from; c < g_cap_cols; c++) {
        row[c].ch = ' ';
        row[c].fg = VT_FG;
        row[c].bg = VT_BG;
    }
}

static void vt_reset_screen(struct session *s) {
    for (int r = 0; r < g_cap_rows; r++) row_clear(grid_row(s, r), 0);
    s->cr = s->cc = 0;
}

// The top line leaves the screen and becomes history. THE ONLY PLACE A
// STREAM STILL EXISTS -- and it is the right place: scrollback is a
// record of what went past, while the screen is a thing being drawn on.
static void vt_scroll(struct session *s) {
    size_t rowbytes = (size_t)g_cap_cols * sizeof(struct cell);
    if (s->sb_count == SB_ROWS) {
        memmove(sb_row(s, 0), sb_row(s, 1), (size_t)(SB_ROWS - 1) * rowbytes);
        s->sb_count--;
    }
    memcpy(sb_row(s, s->sb_count), grid_row(s, 0), rowbytes);
    s->sb_count++;

    memmove(grid_row(s, 0), grid_row(s, 1), (size_t)(g_rows - 1) * rowbytes);
    row_clear(grid_row(s, g_rows - 1), 0);
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
    struct cell *cell = &grid_row(s, s->cr)[s->cc];
    cell->ch = c;
    cell->fg = (uint8_t)s->vt.fg;
    cell->bg = (uint8_t)s->vt.bg;
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
        if (s->vt.a == 0) row_clear(grid_row(s, s->cr), s->cc);
        else if (s->vt.a == 1)
            for (int c = 0; c <= s->cc && c < g_cap_cols; c++)
                grid_row(s, s->cr)[c].ch = ' ';
        else row_clear(grid_row(s, s->cr), 0);
        break;
    case ANSI_OP_ERASE_DISPLAY:
        if (s->vt.a == 0) {
            row_clear(grid_row(s, s->cr), s->cc);
            for (int r = s->cr + 1; r < g_rows; r++) row_clear(grid_row(s, r), 0);
        } else if (s->vt.a == 1) {
            for (int r = 0; r < s->cr; r++) row_clear(grid_row(s, r), 0);
            for (int c = 0; c <= s->cc && c < g_cap_cols; c++)
                grid_row(s, s->cr)[c].ch = ' ';
        } else {
            for (int r = 0; r < g_rows; r++) row_clear(grid_row(s, r), 0);
        }
        break;
    case ANSI_OP_ALT_ON:
        // Idempotent: a program that switches twice must not overwrite
        // the screen it saved the first time with the alternate one.
        if (!s->alt) {
            memcpy(s->saved, s->grid,
                   (size_t)g_cap_rows * g_cap_cols * sizeof(struct cell));
            s->saved_cr = s->cr;
            s->saved_cc = s->cc;
            s->alt = 1;
        }
        for (int r = 0; r < g_rows; r++) row_clear(grid_row(s, r), 0);
        s->cr = s->cc = 0;
        break;
    case ANSI_OP_ALT_OFF:
        if (s->alt) {
            memcpy(s->grid, s->saved,
                   (size_t)g_cap_rows * g_cap_cols * sizeof(struct cell));
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
// **JUST "Shell", WITH NO NUMBER.** It used to append the session's SLOT,
// which the strip's own numbering (uui_tabs.numbered) then contradicted:
// a recycled slot made the third tab read `3: Shell 1`. The strip numbers
// by position; the title says what the tab IS.
static void session_default_title(struct session *s, char *dst) {
    (void)s;
    const char *d = "Shell";
    int n = 0;
    while (d[n]) { dst[n] = d[n]; n++; }
    dst[n] = '\0';
}

static void vt_title(struct session *s) {
    if (s->title_locked) return;
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

// **THE STRIP'S ORDER IS `g_tab_slot`, AND IT IS THE ORDER TABS WERE
// OPENED IN -- not the order their sessions sit in memory.** This walked
// the slot array instead, and slots are RECYCLED: closing a middle tab
// freed its slot, and the next new tab silently reappeared in that hole
// rather than at the right end. Every terminal and every browser appends.
//
// So this function only DROPS entries (a tab whose shell has gone) and
// refreshes their labels; appending is open_tab()'s job.
static void tabs_refresh(void) {
    // FOLLOW THE SELECTED SLOT, NOT ITS INDEX. Closing a tab to the LEFT
    // of the selected one shifts every index right of it, so clamping an
    // index moves the selection onto a different shell.
    int want = (g_strip.selected >= 0 && g_strip.selected < g_ntabs)
                 ? g_tab_slot[g_strip.selected] : -1;
    int old_index = g_strip.selected;

    int n = 0;
    for (int i = 0; i < g_ntabs; i++) {
        int slot = g_tab_slot[i];
        struct session *s = (slot >= 0 && slot < MAX_TABS) ? g_slot[slot] : 0;
        if (!s || !s->live) continue;
        g_tab_slot[n] = slot;
        g_tablabels[n].label = s->title;
        // EVERY TAB HAS A CLOSE BOX, the lone one included: the strip is
        // drawn at one tab now (chrome_h()), so hiding the box would
        // leave a control-shaped gap rather than no control.
        g_tablabels[n].closable = 1;
        n++;
    }
    g_ntabs = n;
    g_strip.count = n;

    int sel = -1;
    for (int i = 0; i < n; i++) if (g_tab_slot[i] == want) { sel = i; break; }
    // The selected tab itself went. Its INDEX is then the right answer:
    // it lands on whatever took its place, or on the new last tab --
    // which is what Konsole and every browser do.
    if (sel < 0) sel = old_index;
    if (sel >= n) sel = n - 1;
    if (sel < 0) sel = 0;
    g_strip.selected = sel;
}

static int session_start(int slot) {
    struct session *s = g_slot[slot];
    if (!s) {
        s = (struct session *)malloc(sizeof *s);
        if (!s) return 0;
        // A FRESH STRUCT HAS NO BUFFERS YET, and malloc does not zero:
        // the pointers carried across the wipe below would otherwise be
        // whatever last lived here -- after grow_caps() has freed the
        // old grids, exactly them -- and the allocation would be skipped.
        s->grid = 0; s->sb = 0; s->saved = 0;
        g_slot[slot] = s;
    }
    // Zeroed whether it is new or reused -- a recycled session must not
    // inherit the last shell's screen. The cell buffers are heap-backed
    // now, so their pointers are carried across the wipe (the CONTENT
    // is cleared by vt_reset_screen below, and scrollback by sb_count
    // going to 0).
    struct cell *keep_grid = s->grid, *keep_sb = s->sb, *keep_saved = s->saved;
    char *raw = (char *)s;
    for (unsigned i = 0; i < sizeof *s; i++) raw[i] = 0;
    s->grid = keep_grid; s->sb = keep_sb; s->saved = keep_saved;
    if (!grow_caps(g_rows, g_cols)) return 0;   // first call sizes the caps
    if (!s->grid) {
        s->grid  = cells_new(g_cap_rows, g_cap_cols);
        s->sb    = cells_new(SB_ROWS, g_cap_cols);
        s->saved = cells_new(g_cap_rows, g_cap_cols);
        if (!s->grid || !s->sb || !s->saved) {
            free(s->grid); free(s->sb); free(s->saved);
            s->grid = 0; s->sb = 0; s->saved = 0;
            return 0;
        }
    }
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
    if (g_ntabs >= MAX_TABS) return 0;
    if (!session_start(slot)) return 0;
    // APPENDED, so a new tab is always the RIGHT-HAND one whatever slot
    // it got -- see tabs_refresh() on what walking the slots did instead.
    g_tab_slot[g_ntabs] = slot;
    g_ntabs++;
    g_strip.selected = g_ntabs - 1;
    tabs_refresh();
    return 1;
}

static void close_tab(int tab) {
    if (tab < 0 || tab >= g_ntabs) return;
    struct session *s = g_slot[g_tab_slot[tab]];
    session_stop(s);
    tabs_refresh();
    if (g_ntabs == 0 && g_app) uapp_quit(g_app, 0);
}

// --- drawing ----------------------------------------------------------
//
// A ROW AT A TIME, IN RUNS OF ONE COLOUR PAIR. Per-cell drawing would be
// 12,000 calls a frame at this size; a run is one ugfx_draw_string() and
// there is usually one run per row. The background is painted per run
// too, which is what makes reverse video -- a status bar -- look like a
// bar rather than like coloured letters.

static int menubar_h(void) {
    return g_menu_shown ? uui_menubar_height(&g_menu) : 0;
}

// **THE STRIP IS ALWAYS DRAWN NOW, EVEN AT ONE TAB.** It used to hide
// itself below two, which is Konsole's default -- but the strip carries
// the "+" and a control that is only there once you already have what it
// creates is no control at all. The cost is one row of chrome on a
// single-shell window and a geometry that no longer jumps when a second
// tab opens, which is the half worth having.
static int chrome_h(void) {
    return menubar_h() + uui_tabs_height();
}

static void draw_run(struct ugfx_surface *s, int x, int y,
                     const char *text, int n, uint8_t fg, uint8_t bg) {
    if (n <= 0 || !g_runbuf) return;
    char *buf = g_runbuf;
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
        if (!(blank && row[i].bg == VT_BG) && g_rowbuf) {
            char *run = g_rowbuf;
            for (int k = i; k < j; k++) run[k - i] = row[k].ch;
            draw_run(s, MARGIN + i * cw, y, run, j - i, row[i].fg, row[i].bg);
        }
        i = j;
    }
}

static void draw(struct ugfx_surface *s, int focused) {
    struct session *ses = active();
    int top = chrome_h();
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
            row = sb_row(ses, idx);
        } else {
            row = grid_row(ses, -back);
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
// The ORDER, folded to one number. Part of the signature below because
// closing a middle tab and opening another changes which slot sits
// where without changing the count -- and that order is exactly what a
// test asserting "a new tab went to the right end" has to see.
static int order_sig(void) {
    int v = 0;
    for (int i = 0; i < g_ntabs; i++) v = v * 11 + g_tab_slot[i] + 1;
    return v;
}

// Has anything the rect lines below describe moved since the last
// frame? A signature rather than a comparison of the text: the values
// are small integers and this runs on every draw.
static int chrome_moved(void) {
    static int prev = -1;
    int sig = g_strip.count
            + 7 * order_sig()
            + 17 * g_strip.selected
            + 313 * g_menu_shown
            + 1021 * uui_menubar_depth(&g_menu)
            + 4093 * (g_menu.open_root + 1)
            + 65537 * g_strip.w;
    if (sig == prev) return 0;
    prev = sig;
    return 1;
}

static void log_layout(void) {
    struct session *s = active();
    if (!s) return;
    // **THE FIRST FIELD AFTER `cursor` IS A LINEAR CELL OFFSET, and it
    // has to stay one**: tools/uterm_test.py reads it with
    // `split()[0]`, so a row/column PAIR there parses as neither. Extra
    // fields are safe after it and are what a tabbed window adds.
    char b[96];
    // The CHROME's height, so a pixel check aiming at the grid does not
    // guess where it starts -- the menu bar's arrival moved that edge
    // and a hardcoded band would have gone on comparing the strip.
    snprintf(b, sizeof b,
             "uterm: layout cursor %d rows %d cols %d tabs %d sel %d menu %d "
             "chrome %d rename %d\n",
             s->cr * g_cols + s->cc, g_rows, g_cols, g_ntabs,
             g_strip.selected, g_menu_shown, chrome_h(), g_rename_open);
    uapp_log_layout_line(b);

    // The chrome's rects, the same grammar Notepad reports -- a test
    // clicking a menu row must be told where it is, not derive it.
    //
    // THROUGH uapp_log_layout_line(), NEVER ulogf(): that call is what
    // the `desktop.layout_log` gate and the per-frame dedupe hang off
    // (docs/conventions/gui.md), and a raw log here would write this
    // whole block on every frame with a window open.
    //
    // **AND ONLY WHEN THE CHROME ACTUALLY MOVED.** The dedupe is per
    // BLOCK, so a cursor that advanced by one cell re-emits everything
    // in the block with it -- which made a scrolling program log ~17
    // geometry lines a frame instead of the one line that changed.
    // These rects move when a tab opens or a menu does, not when a
    // shell prints, so they carry their own guard.
    if (!chrome_moved()) return;

    // The chrome's own rects -- the strip's slots, the menu's titles and
    // rows -- come from the widgets themselves, in the shared vocabulary
    // (ui/uui_describe.h). WHICH SESSION IS AT WHICH POSITION stays the
    // app's: a rect says where a tab is drawn and nothing about which
    // shell it holds, so a test checking that a new tab was APPENDED
    // rather than dropped into a recycled slot's hole needs this.
    uapp_log_layout(g_app, "uterm");
    for (int i = 0; i < g_ntabs; i++) {
        char b[64];
        snprintf(b, sizeof b, "uterm: layout tabslot %d %d\n", i, g_tab_slot[i]);
        uapp_log_layout_line(b);
    }
}

static void size_changed(int w, int h);

// --- renaming ---------------------------------------------------------

// The field, centred over the GRID. Derived from the window rather than
// stored, so it survives a resize and a font change with nothing to keep
// in step -- the same reason nothing else here caches a pixel.
static void rename_rect(int w, int h, int *x, int *y, int *fw, int *fh) {
    int width = w / 2;
    if (width < ugfx_char_w() * 16) width = ugfx_char_w() * 16;
    if (width > w - 4 * utheme_pad()) width = w - 4 * utheme_pad();
    *fw = width;
    *fh = utheme_control_h();
    *x = (w - width) / 2;
    *y = chrome_h() + (h - chrome_h() - *fh) / 2;
}

static void rename_begin(void) {
    struct session *s = active();
    if (!s) return;
    g_rename_slot = s->index;
    uui_textbox_init(&g_rename, s->title);
    uui_textbox_set_active(&g_rename, 1);
    g_rename_open = 1;
}

// An EMPTY name hands the tab back to its shell rather than blanking it:
// a tab with no label is unreadable, and "undo the rename" needs somewhere
// to live. That is the same judgement vt_title() already makes about an
// empty OSC.
static void rename_commit(void) {
    g_rename_open = 0;
    struct session *s = (g_rename_slot >= 0 && g_rename_slot < MAX_TABS)
                          ? g_slot[g_rename_slot] : 0;
    g_rename_slot = -1;
    if (!s || !s->live) return;
    const char *want = uui_textbox_text(&g_rename);
    if (!want || !want[0]) {
        s->title_locked = 0;
        session_default_title(s, s->title);
    } else {
        strlcpy(s->title, want, sizeof s->title);
        s->title_locked = 1;
    }
    ulogf("uterm: tab %d title %s\n", s->index, s->title);
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    struct ugfx_surface *s = uapp_surface(d);
    // MEASURED HERE AS WELL AS ON RESIZE, because this is the first
    // moment the real surface exists -- and because the FONT can change
    // under a running client (WIN_EV_FONT), which changes the cell size
    // without changing the window's. It early-outs when nothing moved.
    size_changed(s->w, s->h);
    // The chrome is positioned here rather than by a layout: this window
    // is two strips over a canvas it paints itself, and running a
    // uui_layout for that would be more machinery than arithmetic.
    int mh = menubar_h();
    uui_menubar_set_geometry(&g_menu, 0, 0, s->w, mh);
    // The whole content area, so a menu that will not fit below the bar
    // may flip or slide against the window rather than off it.
    uui_menubar_set_bounds(&g_menu, 0, 0, s->w, s->h);
    uui_tabs_set_geometry(&g_strip, 0, mh, s->w, uui_tabs_height());
    g_widgets[0].hidden = !g_menu_shown;
    draw(s, uapp_focused(a));
    log_layout();
}

// The rename prompt, over the grid and after everything else -- see the
// note beside g_rename. uapp runs on_draw_over last (ui/uapp.c), which
// is the same z-order rule uui_menubar_draw_popup() states.
static void on_draw_over(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    if (!g_rename_open) return;
    struct ugfx_surface *s = uapp_surface(d);
    int fx, fy, fw, fh;
    rename_rect(s->w, s->h, &fx, &fy, &fw, &fh);
    uui_textbox_set_geometry(&g_rename, fx, fy, fw, fh);
    int pad = utheme_pad();
    ugfx_fill_rect(s, fx - pad, fy - pad - ugfx_char_h() - pad,
                    fw + 2 * pad, fh + 3 * pad + ugfx_char_h(),
                    UTHEME_PANEL_BG);
    ugfx_draw_rect(s, fx - pad, fy - pad - ugfx_char_h() - pad,
                    fw + 2 * pad, fh + 3 * pad + ugfx_char_h(),
                    UTHEME_BORDER);
    ugfx_draw_string(s, fx, fy - pad - ugfx_char_h(), "Rename tab:",
                      UTHEME_TEXT, UTHEME_PANEL_BG);
    uui_textbox_draw(s, &g_rename);
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
    int rows = (h - chrome_h() - 2 * MARGIN) / ch;
    int cols = (w - 2 * MARGIN) / cw;
    if (rows < 2) rows = 2;
    if (cols < 8) cols = 8;
    // Grow the buffers to fit; on a failed malloc keep the old
    // capacity and clamp, which is the old fixed-grid behaviour.
    if (!grow_caps(rows, cols)) {
        if (rows > g_cap_rows) rows = g_cap_rows;
        if (cols > g_cap_cols) cols = g_cap_cols;
    }
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
        tabs_refresh();
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

// One byte to the shell, the way a keystroke would arrive. THE MENU
// SENDS THE SAME BYTE THE KEY DOES rather than reaching for
// sys_kill(): Ctrl-C is interpreted by kernel/tty/ldisc.c, which is
// what makes it reach the foreground JOB rather than the shell.
static void send_byte(char b) {
    struct session *s = active();
    if (s && s->master >= 0) sys_write(s->master, &b, 1);
}

static unsigned menu_item_flags(int code) {
    switch (code) {
    case CMD_MENUBAR:   return g_menu_shown ? UUI_MI_CHECKED : 0;
    case CMD_NEXT_TAB:
    case CMD_PREV_TAB:  return g_ntabs > 1 ? 0 : UUI_MI_DISABLED;
    default:            return 0;
    }
}

static void step_tab(int delta) {
    if (g_ntabs <= 1) return;
    int next = g_strip.selected + delta;
    if (next < 0) next = g_ntabs - 1;          // wraps, as Konsole does
    if (next >= g_ntabs) next = 0;
    uui_tabs_select(&g_strip, next);
}

static void do_command(struct uapp *a, int code) {
    struct session *s = active();
    switch (code) {
    case CMD_NEW_TAB:   open_tab(); break;
    case CMD_CLOSE_TAB: close_tab(g_strip.selected); break;
    case CMD_EXIT:      uapp_quit(a, 0); return;
    case CMD_RENAME:    rename_begin(); break;
    case CMD_CLEAR_SCREEN:
        if (s) { vt_reset_screen(s); s->sb_view = 0; }
        break;
    case CMD_CLEAR_SB:
        if (s) { s->sb_count = 0; s->sb_view = 0; }
        break;
    case CMD_RESET:
        // WHAT `reset` DOES: the screen, the parser's colours and modes,
        // and the view. Not the scrollback -- `reset` on a real terminal
        // leaves history alone, which is why Clear Scrollback is its own
        // row rather than part of this one.
        if (s) {
            ansi_init(&s->vt, VT_FG, VT_BG);
            vt_reset_screen(s);
            s->alt = 0;
            s->cursor_shown = 1;
            s->sb_view = 0;
        }
        break;
    case CMD_INTR:      send_byte(0x03); break;
    case CMD_EOF:       send_byte(0x04); break;
    case CMD_TOP:       if (s) s->sb_view = s->sb_count; break;
    case CMD_BOTTOM:    if (s) s->sb_view = 0; break;
    case CMD_MENUBAR:   g_menu_shown = !g_menu_shown; size_changed(uapp_width(a), uapp_height(a)); break;
    case CMD_NEXT_TAB:  step_tab(+1); break;
    case CMD_PREV_TAB:  step_tab(-1); break;
    default: return;
    }
    uapp_redraw(a);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    // --- the rename prompt owns every key while it is up --------------
    if (g_rename_open) {
        if (key == '\n' || key == '\r') { rename_commit(); tabs_refresh(); }
        else if (key == 0x1B)             { g_rename_open = 0; g_rename_slot = -1; }
        else if (!uui_textbox_key_mods(&g_rename, key, mods)) return;
        uapp_redraw(a);
        return;
    }

    // --- an open menu owns it next ------------------------------------
    if (uui_menubar_is_open(&g_menu)) {
        int code = -1;
        if (uui_menubar_key(&g_menu, key, &code)) {
            if (code >= 0) do_command(a, code);
            uapp_redraw(a);
            return;
        }
    }

    // F10 REVEALS A HIDDEN BAR AS WELL AS OPENING IT, so hiding the menu
    // is never a one-way door. It is intercepted rather than sent to the
    // shell, which is the same trade every app with a menu bar makes.
    if (key == KEY_F10) {
        if (!g_menu_shown) { g_menu_shown = 1; size_changed(uapp_width(a), uapp_height(a)); }
        else { int c = -1; uui_menubar_key(&g_menu, key, &c); }
        uapp_redraw(a);
        return;
    }

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
        step_tab(key == KEY_PAGE_DOWN ? +1 : -1);
        uapp_redraw(a);
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

static void tab_new(void *ctx) {
    (void)ctx;
    open_tab();
    if (g_app) uapp_redraw(g_app);
}

// The router names a widget to the app; the menu bar's commit is parked
// in the widget and taken here (ui/uui_menubar.h). Only the menu reports
// this way -- the strip's own callbacks say what happened directly.
static void on_widget(struct uapp *a, int id, int reason) {
    (void)reason;
    if (id != ID_MENU) return;
    int code = uui_menubar_take_code(&g_menu);
    if (code >= 0) do_command(a, code);
}

// A press the router did not consume. Its only job is the rename prompt:
// clicking away cancels, and clicking IN it places the caret.
static void on_press(struct uapp *a, int x, int y, unsigned buttons) {
    (void)buttons;
    if (!g_rename_open) return;
    int fx, fy, fw, fh;
    rename_rect(uapp_width(a), uapp_height(a), &fx, &fy, &fw, &fh);
    uui_textbox_set_geometry(&g_rename, fx, fy, fw, fh);
    if (uui_textbox_hit(&g_rename, x, y))
        g_rename.ed.cursor = uui_textbox_index_at_x(&g_rename, x);
    else
        { g_rename_open = 0; g_rename_slot = -1; }
    uapp_redraw(a);
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
    g_strip.on_new    = tab_new;
    g_strip.show_new  = 1;
    g_strip.numbered  = 1;   // every tab in one directory reports the same title

    uui_menubar_init(&g_menu, menu_bar,
                      (int)(sizeof menu_bar / sizeof menu_bar[0]));
    g_menu.item_flags = menu_item_flags;

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
        .on_draw_over = on_draw_over,
        .on_widget = on_widget,
        .on_press = on_press,
        .on_key  = on_key,
        .on_user = on_user,
        .on_wheel = on_wheel,
        .on_resize = on_resize,
        .on_close = on_close_cb,
    };
    return uapp_run(&desc);
}
