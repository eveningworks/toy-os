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
// THE EMULATION IS DELIBERATELY TINY: `\n`, `\r`, `\b` and overwrite.
// That is exactly what `/bin/tosh` emits -- its repaint uses '\r' and
// spaces and nothing else, because a ring-3 program has no cursor-move
// syscall -- so it is what a terminal here has to honour. ANSI escapes
// (colour, cursor addressing) are a named roadmap item, not a gap: the
// kernel console already parses them (kernel/lib/ansi.c) and this
// window should end up sharing that parser rather than growing a second.
#include <stdint.h>
#include "rt/sys.h"
#include "ui/ugfx.h"
#include "ui/uui.h"
#include "ui/uapp.h"
#include "ui/utext.h"
#include "ui/utheme.h"
#include "keyboard.h"

#define WIN_W 640
#define WIN_H 400
#define MARGIN 6

#define SHELL "/bin/tosh"

static struct utext g_out;   // the screen: transcript and current line alike
static int g_master = -1;    // our end of the pty
static int g_child;          // the shell's pid, for reaping and for `ps`

// --- the emulator ----------------------------------------------------
//
// A terminal is a cursor over a screen. `g_line` is the logical index
// where the current line starts and `g_col` how far along it the cursor
// is -- which is all the state '\n', '\r' and '\b' need, and all
// /bin/tosh's repaint asks for.
//
// **WHY A COLUMN AND NOT JUST AN APPEND POINT.** tosh repaints an edited
// line in two passes: return to column 0, paint the whole line plus
// spaces to cover what was there before, return to column 0 again, paint
// only the part before the caret. A terminal that treated '\r' as "start
// a new line" would show the line twice; one that treated it as "discard
// this line" would lose everything after the caret on the second pass.
// Overwrite-in-place is the only reading that produces what the shell
// means, and it is what a real terminal does.
static int g_line;
static int g_col;

static void vt_putc(char c) {
    if (c == '\n') {
        // Move to the end before appending: the cursor may be mid-line,
        // and a newline ends the line rather than splitting it.
        g_col = g_out.count - g_line;
        utext_putc(&g_out, '\n');
        g_line = g_out.count;
        g_col = 0;
        return;
    }
    if (c == '\r') { g_col = 0; return; }
    if (c == '\b') { if (g_col > 0) g_col--; return; }

    int at = g_line + g_col;
    if (at < g_out.count) {
        utext_set(&g_out, at, c);
    } else {
        int before = g_out.count;
        utext_putc(&g_out, c);
        // THE RING EVICTS ITS OLDEST CHARACTER WHEN FULL, which shifts
        // every logical index down by one -- including the one that says
        // where this line starts. Without this the cursor drifts
        // backwards through the transcript once the scrollback fills,
        // and only then, which is the kind of bug that shows up after
        // ten minutes of use and never in a test that types three lines.
        if (g_out.count == before && g_line > 0) g_line--;
    }
    g_col++;
}

static void vt_write(const char *buf, int len) {
    for (int i = 0; i < len; i++) vt_putc(buf[i]);
    // The caret IS the cursor: utext_draw() draws it at ed.cursor, so
    // keeping that in step is the whole of caret handling here.
    g_out.ed.cursor = g_line + g_col;
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

// --- drawing ----------------------------------------------------------

static void draw(struct ugfx_surface *s, int focused) {
    ugfx_fill(s, ugfx_rgb(0, 0, 0));
    // ONE TEXT AREA, not a transcript plus a separately-drawn prompt.
    // The prompt is not this program's any more -- it is bytes the shell
    // printed, in the same stream as everything else, which is what a
    // terminal is. That deleted the "where does the prompt go" clamping
    // this file used to carry.
    utext_draw(&g_out, s, MARGIN, MARGIN,
                s->w - 2 * MARGIN, s->h - 2 * MARGIN,
                ugfx_rgb(220, 220, 220), ugfx_rgb(0, 0, 0),
                ugfx_rgb(60, 80, 120), focused);
}

// One line, content-relative, on stderr -- the grammar every GUI test
// tool here asserts on. The CURSOR rather than a prompt row: there is no
// prompt widget to report the position of any more, and the cursor is
// the thing a test can predict.
static void log_layout(void) {
    char b[64];
    int n = 0;
    const char *pre = "uterm: layout cursor ";
    while (pre[n]) { b[n] = pre[n]; n++; }
    int v = g_out.ed.cursor;
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
    draw(uapp_surface(d), uapp_focused(a));
    log_layout();
}

// --- input ------------------------------------------------------------

static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    // PAGE UP/DOWN SCROLL AND ARE NOT THE SHELL'S. Everything else --
    // including the arrows, Home, End and every Ctrl combination -- goes
    // to the shell as a byte, because THE SHELL HAS THE LINE EDITOR.
    // This window deciding what Ctrl-A means would be the second
    // implementation kernel/lib/klineedit.c exists to prevent.
    if (key == KEY_PAGE_UP)   { utext_scroll(&g_out, 5);  uapp_redraw(a); return; }
    if (key == KEY_PAGE_DOWN) { utext_scroll(&g_out, -5); uapp_redraw(a); return; }

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
    utext_init(&g_out);
    g_line = 0;
    g_col = 0;

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
        .on_tick = on_tick,
        .on_close = on_close_cb,
    };
    return uapp_run(&desc);
}
