// Terminal -- a TERMINAL EMULATOR, at last.
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
// WHAT THAT DELETED, which is the argument for it: the linked-in shell,
// the output sink, `struct tosh`'s `stdin_ok` dance, a second copy of
// the line editor and a second command history. None of it was wrong;
// all of it existed because this window was not a terminal.
//
// AND `Ctrl-C` WORKS HERE FOR THE SAME REASON IT WORKS ON THE PHYSICAL
// KEYBOARD -- not for a similar reason. The key arrives as the byte
// 0x03, is written to the master, and `kernel/tty/ldisc.c` recognises
// it as INTR and signals this terminal's foreground group. That is the
// same function, on the same object, as the one the keyboard IRQ feeds.
//
// **ITS SCREEN IS A GRID, WHICH IS WHAT MAKES A FULL-SCREEN PROGRAM
// WORK HERE.** It was a character STREAM in a scrollback -- right for a
// shell transcript, and unable to express "put the caret at row 4,
// column 12", so `/bin/edit` printed its escape sequences instead of
// obeying them. A terminal is a grid of cells with a cursor over it;
// lines that scroll off the top become scrollback, which is the only
// part that is a stream.
//
// **AND THE ANSI PARSER IS THE KERNEL'S OWN, COMPILED TWICE.**
// kernel/lib/ansi.c is a pure state machine that knows nothing about a
// screen -- its own header says so -- so the physical console and this
// window resolve `ESC[4;12H` through the SAME code. A second parser
// here would be a second set of answers to "what does ESC[0m clear",
// and the two would drift the first time either was extended. Same
// shared-source rule as geom.c and klineedit.c.
#include <stdint.h>
#include "rt/sys.h"
#include "ui/ugfx.h"
#include "ui/uui.h"
#include "ui/uapp.h"
#include "ui/utheme.h"
#include "keyboard.h"
#include "ansi.h"   // the kernel's parser, compiled into libuapp too

#define WIN_W 640
#define WIN_H 400
#define MARGIN 6

#define SHELL "/bin/tosh"

// The screen. A fixed grid rather than a resizable one: a window can be
// made large, and 200x60 cells is 24 KiB of statics against a ring-3
// heap -- cheaper than the arithmetic of growing it, and it bounds what
// a program can ask for. What actually varies is g_rows/g_cols, derived
// from the window and told to the child through SYS_TCSETWINSZ.
#define VT_ROWS 60
#define VT_COLS 200
#define SB_ROWS 240   // scrollback lines kept above the screen

struct cell { char ch; uint8_t fg, bg; };

static struct cell g_grid[VT_ROWS][VT_COLS];
static struct cell g_sb[SB_ROWS][VT_COLS];
static int g_sb_count;      // lines of scrollback held
static int g_sb_view;       // how far back the reader has scrolled, in lines

// THE ALTERNATE SCREEN (ESC[?1049h/l). A second grid, plus the cursor
// the switch saved, so a full-screen program leaves the terminal
// exactly as it found it -- which is what `less` and `vim` do on every
// real terminal and the reason they leave no wreckage behind.
//
// A COPY OF THE GRID RATHER THAN A SECOND LIVE ONE. Swapping a pointer
// between two grids would be cheaper, but everything here indexes
// g_grid directly and a pointer would have to be threaded through all
// of it; 36 KB of statics is the cost of not doing that, against a
// ring-3 heap that has it. The saved copy is written once per switch,
// not per frame.
//
// **SCROLLBACK IS NOT SAVED, DELIBERATELY.** On a real terminal the
// alternate screen has no scrollback at all -- that is why you cannot
// scroll back through a `less` session -- and keeping the shell's
// scrollback live underneath is what makes leaving feel like nothing
// happened.
static struct cell g_saved[VT_ROWS][VT_COLS];
static int g_alt;           // on the alternate screen right now
static int g_saved_cr, g_saved_cc;

static int g_rows = 24, g_cols = 80;
static int g_cr, g_cc;      // the cursor, in cells
static int g_cursor_shown = 1;

static struct ansi_parser g_vt;

static int g_master = -1;    // our end of the pty
static int g_child;          // the shell's pid, for reaping and for `ps`

// The 16 ANSI colours as RGB. The parser resolves a sequence to an
// `enum vga_color`, which is an INDEX -- turning an index into light is
// the display's business, and the kernel console does exactly the same
// thing with its own table. Standard VGA values, so a screenshot of this
// window and one of the console are the same colours.
static const uint32_t VGA_RGB[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

#define VT_FG 7   // light grey on black -- the console's own default pair
#define VT_BG 0

static void row_clear(struct cell *row, int from) {
    for (int c = from; c < VT_COLS; c++) {
        row[c].ch = ' ';
        row[c].fg = VT_FG;
        row[c].bg = VT_BG;
    }
}

static void vt_reset_screen(void) {
    for (int r = 0; r < VT_ROWS; r++) row_clear(g_grid[r], 0);
    g_cr = g_cc = 0;
}

// The top line leaves the screen and becomes history. THE ONLY PLACE A
// STREAM STILL EXISTS -- and it is the right place: scrollback is a
// record of what went past, while the screen is a thing being drawn on.
static void vt_scroll(void) {
    if (g_sb_count == SB_ROWS) {
        for (int i = 1; i < SB_ROWS; i++)
            for (int c = 0; c < VT_COLS; c++) g_sb[i - 1][c] = g_sb[i][c];
        g_sb_count--;
    }
    for (int c = 0; c < VT_COLS; c++) g_sb[g_sb_count][c] = g_grid[0][c];
    g_sb_count++;

    for (int r = 1; r < g_rows; r++)
        for (int c = 0; c < VT_COLS; c++) g_grid[r - 1][c] = g_grid[r][c];
    row_clear(g_grid[g_rows - 1], 0);
}

static void vt_newline(void) {
    g_cr++;
    if (g_cr >= g_rows) { g_cr = g_rows - 1; vt_scroll(); }
}

static void vt_putc_raw(char c) {
    if (c == '\n') { g_cc = 0; vt_newline(); return; } // no OPOST: LF is CRLF here
    if (c == '\r') { g_cc = 0; return; }
    if (c == '\b') { if (g_cc > 0) g_cc--; return; }
    if (c == '\t') { do { g_cc++; } while (g_cc % 8 && g_cc < g_cols); if (g_cc >= g_cols) { g_cc = 0; vt_newline(); } return; }
    if ((unsigned char)c < 32) return; // anything else unprintable is dropped

    if (g_cc >= g_cols) { g_cc = 0; vt_newline(); }
    g_grid[g_cr][g_cc].ch = c;
    g_grid[g_cr][g_cc].fg = (uint8_t)g_vt.fg;
    g_grid[g_cr][g_cc].bg = (uint8_t)g_vt.bg;
    g_cc++;
}

static void clamp_cursor(void) {
    if (g_cr < 0) g_cr = 0;
    if (g_cr >= g_rows) g_cr = g_rows - 1;
    if (g_cc < 0) g_cc = 0;
    if (g_cc >= g_cols) g_cc = g_cols - 1;
}

// One completed cursor/erase sequence. The parser has already applied
// every default -- "a missing or zero count means 1", 1-based rows and
// columns -- so this only has to act, which is the point of it being a
// shared parser rather than a second reading of the spec.
static void vt_ctrl(void) {
    switch (g_vt.op) {
    case ANSI_OP_MOVE_TO: g_cr = g_vt.a - 1; g_cc = g_vt.b - 1; break;
    case ANSI_OP_UP:      g_cr -= g_vt.a; break;
    case ANSI_OP_DOWN:    g_cr += g_vt.a; break;
    case ANSI_OP_RIGHT:   g_cc += g_vt.a; break;
    case ANSI_OP_LEFT:    g_cc -= g_vt.a; break;
    case ANSI_OP_COLUMN:  g_cc = g_vt.a - 1; break;
    case ANSI_OP_ROW:     g_cr = g_vt.a - 1; break;
    case ANSI_OP_ERASE_LINE:
        if (g_vt.a == 0) row_clear(g_grid[g_cr], g_cc);
        else if (g_vt.a == 1) for (int c = 0; c <= g_cc && c < VT_COLS; c++) g_grid[g_cr][c].ch = ' ';
        else row_clear(g_grid[g_cr], 0);
        break;
    case ANSI_OP_ERASE_DISPLAY:
        if (g_vt.a == 0) {
            row_clear(g_grid[g_cr], g_cc);
            for (int r = g_cr + 1; r < g_rows; r++) row_clear(g_grid[r], 0);
        } else if (g_vt.a == 1) {
            for (int r = 0; r < g_cr; r++) row_clear(g_grid[r], 0);
            for (int c = 0; c <= g_cc && c < VT_COLS; c++) g_grid[g_cr][c].ch = ' ';
        } else {
            for (int r = 0; r < g_rows; r++) row_clear(g_grid[r], 0);
        }
        break;
    case ANSI_OP_ALT_ON:
        // Idempotent: a program that switches twice must not overwrite
        // the screen it saved the first time with the alternate one.
        if (!g_alt) {
            for (int r = 0; r < VT_ROWS; r++)
                for (int c = 0; c < VT_COLS; c++) g_saved[r][c] = g_grid[r][c];
            g_saved_cr = g_cr;
            g_saved_cc = g_cc;
            g_alt = 1;
        }
        for (int r = 0; r < g_rows; r++) row_clear(g_grid[r], 0);
        g_cr = g_cc = 0;
        break;
    case ANSI_OP_ALT_OFF:
        if (g_alt) {
            for (int r = 0; r < VT_ROWS; r++)
                for (int c = 0; c < VT_COLS; c++) g_grid[r][c] = g_saved[r][c];
            g_cr = g_saved_cr;
            g_cc = g_saved_cc;
            g_alt = 0;
        }
        break;
    case ANSI_OP_SHOW: g_cursor_shown = 1; break;
    case ANSI_OP_HIDE: g_cursor_shown = 0; break;
    // SAVE/RESTORE have no users here yet, and a half-remembered
    // position is worse than none.
    default: break;
    }
    clamp_cursor();
}

static void vt_write(const char *buf, int len) {
    for (int i = 0; i < len; i++) {
        switch (ansi_feed(&g_vt, buf[i])) {
        case ANSI_PASS:  vt_putc_raw(buf[i]); break;
        case ANSI_CTRL:  vt_ctrl(); break;
        case ANSI_SGR:   break; // the colours are read off the parser per cell
        case ANSI_EATEN: break;
        }
    }
    // NEW OUTPUT PINS THE VIEW TO THE BOTTOM, which is what every
    // terminal does: a program printing while you are reading history
    // brings you back, because otherwise the thing you asked to run
    // appears to have done nothing.
    g_sb_view = 0;
}

// Drain whatever the shell has printed. NON-BLOCKING -- this runs on the
// window's tick, and a blocking read here would stop the window
// answering the compositor at all (a frozen window, not a slow one).
//
// Returns 1 if anything arrived, so the caller only repaints when there
// is something new; 0 otherwise. A closed master (the shell exited)
// reports EOF, which is how the window learns to close itself.
static int pump(int *out_eof) {
    char buf[256];
    int got = 0;
    for (;;) {
        int64_t n = sys_read(g_master, buf, sizeof buf);
        if (n > 0) { vt_write(buf, (int)n); got = 1; continue; }
        if (n == 0) { *out_eof = 1; return got; } // the shell is gone
        break; // EAGAIN -- nothing more right now
    }
    return got;
}

static void size_changed(int w, int h);

// --- drawing ----------------------------------------------------------
//
// A ROW AT A TIME, IN RUNS OF ONE COLOUR PAIR. Per-cell drawing would be
// 12,000 calls a frame at this size; a run is one ugfx_draw_string() and
// there is usually one run per row. The background is painted per run
// too, which is what makes reverse video -- a status bar -- look like a
// bar rather than like coloured letters.

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
    // values rather than by looking at the screenshot, which is the
    // whole reason CLAUDE.md says to.
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
    ugfx_fill(s, VGA_RGB[VT_BG]);
    int ch = ugfx_char_h(), cw = ugfx_char_w();

    // Scrolled back: the top rows come from history, the rest from the
    // screen, and they meet without a seam because both are the same
    // grid of cells. That is the payoff of scrollback being made of
    // evicted ROWS rather than of a character stream.
    for (int r = 0; r < g_rows; r++) {
        int back = g_sb_view - r;      // >0 means this row is history
        const struct cell *row;
        if (back > 0) {
            int idx = g_sb_count - back;
            if (idx < 0) continue;     // before the oldest line we kept
            row = g_sb[idx];
        } else {
            row = g_grid[-back];
        }
        draw_row(s, row, MARGIN + r * ch);
    }

    // The caret, only while FOCUSED and only while the program wants it
    // shown (`ESC[?25l` hides it -- a full-screen program parking the
    // caret somewhere meaningless turns it off rather than moving it).
    // An unfocused window drawing a caret claims to be taking input that
    // is going somewhere else.
    if (focused && g_cursor_shown && g_sb_view == 0)
        ugfx_fill_rect(s, MARGIN + g_cc * cw, MARGIN + g_cr * ch, 2, ch,
                       VGA_RGB[VT_FG]);
}

// One line, content-relative, on stderr -- the grammar every GUI test
// tool here asserts on. The CURSOR, as a cell index, because that is the
// thing that would be wrong if a cursor sequence were mishandled and it
// is what a test can predict.
static void log_layout(void) {
    char b[64];
    int n = 0;
    const char *pre = "uterm: layout cursor ";
    while (pre[n]) { b[n] = pre[n]; n++; }
    int v = g_cr * g_cols + g_cc;
    char d[12];
    int c = 0;
    if (v <= 0) d[c++] = '0';
    while (v > 0) { d[c++] = (char)('0' + v % 10); v /= 10; }
    while (c > 0) b[n++] = d[--c];
    b[n++] = '\n';
    b[n] = '\0';
    sys_eprint(b);
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    struct ugfx_surface *s = uapp_surface(d);
    // MEASURED HERE AS WELL AS ON RESIZE, because this is the first
    // moment the real surface exists -- and because the FONT can change
    // under a running client (WIN_EV_FONT), which changes the cell size
    // without changing the window's. It early-outs when nothing moved.
    size_changed(s->w, s->h);
    draw(s, uapp_focused(a));
    log_layout();
}

// HOW BIG THE WINDOW IS, IN CELLS, AND THE CHILD IS TOLD. Only this
// process can work that out: the size is pixels and the answer is cells,
// and the conversion needs the font. SYS_TCSETWINSZ is how the shell and
// anything it runs find out -- /bin/edit asks for it before it draws a
// single row.
static void size_changed(int w, int h) {
    int cw = ugfx_char_w(), ch = ugfx_char_h();
    if (cw <= 0 || ch <= 0) return;
    int rows = (h - 2 * MARGIN) / ch;
    int cols = (w - 2 * MARGIN) / cw;
    if (rows < 2) rows = 2;
    if (cols < 8) cols = 8;
    if (rows > VT_ROWS) rows = VT_ROWS;
    if (cols > VT_COLS) cols = VT_COLS;
    if (rows == g_rows && cols == g_cols) return;

    g_rows = rows;
    g_cols = cols;
    clamp_cursor();
    if (g_master >= 0) {
        struct tty_winsize ws = { (uint16_t)rows, (uint16_t)cols };
        sys_tcsetwinsz(g_master, &ws);
    }
}

static void on_resize(struct uapp *a, int w, int h) {
    (void)a;
    size_changed(w, h);
}

// --- input ------------------------------------------------------------

// THE WHEEL SCROLLS THE SCROLLBACK, and it is the same state Page
// Up/Down moves -- not a second notion of "where the reader is". A
// terminal that scrolled differently by wheel than by key would be two
// implementations of one idea, which is the thing this file's own
// comment about the line editor already warns about.
//
// Three lines per notch, which is what every desktop terminal does and
// what the toolkit's other scroll consumers use; a notch is a detent,
// not a line. `notches` is positive AWAY from the user (win_proto.h),
// and away means BACK INTO HISTORY -- the direction the content moves
// down the screen.
#define WHEEL_LINES 3

static void on_wheel(struct uapp *a, int notches) {
    int want = g_sb_view + notches * WHEEL_LINES;
    // Clamped rather than wrapped, and clamped at BOTH ends: scrolling
    // past the oldest line must stop there, and scrolling forward past
    // the live screen must land exactly on it (0) rather than going
    // negative, which would index above the top of the buffer.
    if (want > g_sb_count) want = g_sb_count;
    if (want < 0) want = 0;
    if (want == g_sb_view) return;   // nothing moved -- do not repaint
    g_sb_view = want;
    uapp_redraw(a);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    // PAGE UP/DOWN SCROLL AND ARE NOT THE SHELL'S. Everything else --
    // including the arrows, Home, End and every Ctrl combination -- goes
    // to the shell as a byte, because THE SHELL HAS THE LINE EDITOR.
    // This window deciding what Ctrl-A means would be the second
    // implementation kernel/lib/klineedit.c exists to prevent.
    if (key == KEY_PAGE_UP) {
        g_sb_view += g_rows / 2;
        if (g_sb_view > g_sb_count) g_sb_view = g_sb_count;
        uapp_redraw(a);
        return;
    }
    if (key == KEY_PAGE_DOWN) {
        g_sb_view -= g_rows / 2;
        if (g_sb_view < 0) g_sb_view = 0;
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
    if (g_master >= 0) sys_write(g_master, &b, 1);

    // Draining right after the write is not an optimisation -- it is
    // what makes typing feel immediate. The echo comes back through the
    // discipline, and waiting for the next tick to show it would put a
    // frame of lag on every keystroke.
    int eof = 0;
    if (pump(&eof)) uapp_redraw(a);
    if (eof) uapp_quit(a, 0);
}

static int on_tick(struct uapp *a) {
    int eof = 0;
    int painted = pump(&eof);
    if (eof) {
        // The shell exited -- Ctrl-D, or `exit`. The window goes with
        // it, which is what every terminal does and what makes Ctrl-D
        // mean the same thing here as on the physical console.
        uapp_quit(a, 0);
        return 0;
    }
    return painted;
}

// --- lifecycle --------------------------------------------------------

static void on_open_cb(struct uapp *a) {
    (void)a;
    ansi_init(&g_vt, VT_FG, VT_BG);
    vt_reset_screen();

    int slave = -1;
    if (sys_openpty(&g_master, &slave) < 0) {
        vt_write("terminal: no pty available\n", 27);
        return;
    }
    // NON-BLOCKING ON THE MASTER ONLY, and never on the slave: the flag
    // lives on the DESCRIPTION, so setting it on the slave would hand
    // the shell a stdin that reports EAGAIN instead of waiting, and it
    // would spin.
    sys_set_nonblock(g_master, 1);

    // The child's 0/1/2 are the slave. dup2 around the spawn, exactly as
    // tosh's own redirection does -- SYS_SPAWN inherits the descriptor
    // table, so placing them here is placing them in the child.
    int in0 = sys_dup(0), out1 = sys_dup(1), err2 = sys_dup(2);
    sys_dup2(slave, 0);
    sys_dup2(slave, 1);
    sys_dup2(slave, 2);
    // ITS OWN GROUP, which is what makes it interruptible as a unit: the
    // shell puts each job it runs into a group of its own, and this is
    // the group that starts in front of the terminal.
    g_child = sys_spawn_group(SHELL, 0, -1, 0, PGID_NEW);
    if (in0  >= 0) { sys_dup2(in0, 0);  sys_close(in0); }
    if (out1 >= 0) { sys_dup2(out1, 1); sys_close(out1); }
    if (err2 >= 0) { sys_dup2(err2, 2); sys_close(err2); }

    // OUR copy of the slave goes now. The child holds its own through
    // the fds above, and keeping this one would mean the master never
    // sees end-of-file when the shell dies -- the window would sit there
    // with a dead shell in it.
    sys_close(slave);

    if (g_child < 0) { vt_write("terminal: could not start " SHELL "\n", 34); return; }

    // **AND NOTHING SETS THE FOREGROUND GROUP HERE, DELIBERATELY.** This
    // process opened the pty but never reads the slave, so it does not
    // own the terminal -- the shell claims it on its first read, exactly
    // as a process claims the physical console (kernel/pty.h). The shell
    // is then the one that moves the foreground group per job, which is
    // what makes Ctrl-C reach the job rather than the shell.
}

static int on_close_cb(struct uapp *a) {
    (void)a;
    // END THE SHELL, DO NOT ORPHAN IT. Closing the master alone would
    // give it end-of-file and it would exit on its own -- but only when
    // it NEXT READS, and a shell waiting on a job does not read for as
    // long as that job runs. A window that has gone must not leave a
    // process on a terminal nobody can type at.
    //
    // SIGTERM rather than SIGKILL: this is the polite one, and the shell
    // is not being force-quit -- the person closed its window. Force
    // Quit is a different action with a different signal.
    if (g_child > 0) {
        sys_kill(g_child, SIGTERM);
        int status = 0;
        sys_waitpid(g_child, &status); // reap it; init would otherwise
        g_child = 0;
    }
    if (g_master >= 0) { sys_close(g_master); g_master = -1; }
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
        // A CADENCE, because there is no poll(). The window has to
        // service the compositor AND drain its child, and cannot block
        // on either; with poll() it would wait on both at once, which is
        // the right answer and a bigger project (docs/roadmap.md).
        // 30ms is a compromise: fast enough that a command's output does
        // not appear to arrive in chunks, slow enough that an idle
        // Terminal is not a busy loop. Typing does not wait for it --
        // on_key drains straight after the write.
        .tick_ms = 30,
        .on_open = on_open_cb,
        .on_draw = on_draw,
        .on_key  = on_key,
        .on_wheel = on_wheel,
        .on_resize = on_resize,
        .on_tick = on_tick,
        .on_close = on_close_cb,
    };
    return uapp_run(&desc);
}
