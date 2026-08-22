// /bin/tosh -- the standalone toy-os shell.
//
// The thin main() userland/lib/tosh.h has promised since it was
// written: an editor plus a read loop. Everything a shell DOES lives in
// the library, which the GUI Terminal drives from its own event loop --
// so this file is only the half a terminal already owns, the part that
// turns keystrokes into a line.
//
// What made it possible is a blocking stdin. Until fd 0 could be read
// (kernel/proc/syscall_fd.c's sys_do_read_console), a ring-3 program
// had only the non-blocking SYS_READ_KEY and would have had to
// spin-poll the keyboard for its entire idle life.
//
// LINE EDITING IS THE KERNEL'S OWN EDITOR, compiled a second time.
// kernel/lib/klineedit.c is the readline-style buffer/cursor/kill-ring
// the physical shell and the GUI Terminal already share, and it turned
// out to be freestanding already -- it includes klineedit.h, string.h
// and keyboard.h and touches no kernel state -- so this is the
// shared-source rule (geom.c, etc_config.c, calc_engine.c), not a port.
// There is ONE definition of what Ctrl-A means on this machine.
//
// THERE IS NO ECHO FROM THE KERNEL. fd 0 is raw -- bytes as typed, no
// line discipline -- so what the user sees is what redraw() prints.
// That is the shape a program with its own editor wants anyway, and it
// is why a real TTY layer (docs/roadmap.md) belongs under both of us
// rather than inside either.
#include "rt/sys.h"
#include "lib/tosh.h"
#include <string.h>
#include "lib/uhistory.h"
#include "klineedit.h"

// Static, not local: `struct kline_edit` is ~1.2 KiB and `struct
// uhistory` ~4 KiB, against USERLAND_CFLAGS' -Wframe-larger-than=2048
// and a 16 KiB ring-3 stack with ONE guard page below it.
static struct kline_edit g_ed;
static struct uhistory   g_hist;
static struct tosh       g_sh;

// How much of the line the screen currently shows. Redrawing needs it
// because erasing is done by overwriting with spaces, so the painter
// has to know how far the last paint reached.
static int g_shown;

static void out_fd1(void *ctx, const char *text, int len) {
    (void)ctx;
    sys_write(1, text, (size_t)len);
}

static void put(const char *s) { sys_write(1, s, (size_t)strlen(s)); }

static const char *prompt(void) { return "$ "; }

// Repaints prompt + line and leaves the caret at ed.cursor.
//
// TWO PASSES, using only '\r'. The kernel front end moves the caret
// with vga_cursor_move(), which is a non-destructive seek a ring-3
// process cannot reach -- there is no drawing syscall and there should
// not be one. What fd 1 does carry is '\r' (vga.c's fb_putc sets col to
// 0), so: return to column 0, paint the whole line plus enough spaces
// to cover whatever the previous paint left behind, then return to
// column 0 again and paint only the prefix. The caret ends up exactly
// where the cursor is, having moved only by writing characters.
//
// THE LIMIT, stated because it is real: '\r' returns to the start of
// the CURRENT ROW, so a prompt plus line longer than the console is
// wide repaints wrongly. A terminal solves this with escape sequences
// it parses itself, which is the TTY layer's job (docs/roadmap.md) --
// not something to bolt onto vga_putc for one caller.
static void redraw(void) {
    put("\r");
    put(prompt());
    sys_write(1, g_ed.buf, (size_t)g_ed.len);

    for (int i = g_ed.len; i < g_shown; i++) sys_write(1, " ", 1);

    put("\r");
    put(prompt());
    sys_write(1, g_ed.buf, (size_t)g_ed.cursor);

    g_shown = g_ed.len;
}

// Ends the current line on screen and starts a fresh one. Used before
// anything that prints (a command's output, ^C) so it does not land on
// top of the line being edited.
static void end_line(void) {
    put("\r");
    put(prompt());
    sys_write(1, g_ed.buf, (size_t)g_ed.len);
    put("\n");
    g_shown = 0;
}

static void fresh_prompt(void) {
    kline_init(&g_ed);
    uhist_reset(&g_hist);
    g_shown = 0;
    redraw();
}

// Alt-. -- the last whitespace-delimited word of the previous command.
static void insert_last_arg(void) {
    const char *last = uhist_last(&g_hist);
    if (!last) return;
    int n = (int)strlen(last);
    int start = kline_ws_word_start(last, n, n);
    kline_insert_str(&g_ed, last + start);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    tosh_init(&g_sh, out_fd1, 0);
    // THIS shell owns the physical console -- reading fd 0 is its whole
    // existence -- so a child it spawns should read the same keyboard.
    // The GUI Terminal leaves this 0 and its children get an empty
    // stdin instead; see struct tosh's `stdin_ok` for why that matters.
    g_sh.stdin_ok = 1;
    uhist_init(&g_hist);

    // --- job control, in two lines -----------------------------------
    //
    // A SHELL LEADS ITS OWN GROUP. Started by init, this shell would
    // otherwise be in init's, and putting init's group in front of the
    // console would point every Ctrl-C at pid 1.
    sys_setpgid(PGID_SELF, PGID_SELF);
    // AND IGNORES SIGINT, which every real shell does. Whenever no job
    // is running, this shell's own group is the console's foreground
    // group -- so a SIGINT sent by hand (`kill -INT`) would end the
    // shell. The INTR KEY does not send one in that state (kernel/tty.h
    // delivers the byte instead, so Ctrl-C still abandons the line
    // below), but a signal that arrives some other way must not be
    // fatal either. Belt and braces, cheaply.
    sys_sigaction(SIGINT, SIG_IGN);

    // The console is CLAIMED BY READING IT, and the foreground group is
    // set at the same moment (kernel/tty.h) -- so nothing here has to
    // call tcsetpgrp: the first sys_read(0, ...) below does it, and
    // job_foreground()/job_done() in the tosh library move it per job.

    put("tosh -- toy-os shell in ring 3. Ctrl-D to exit.\n");
    fresh_prompt();

    for (;;) {
        char buf[32];
        // Blocks. The kernel parks this process on SCHED_WAIT_KEY and
        // the keyboard IRQ releases it, so an idle shell costs nothing
        // -- `ps` shows it blocked, not ready.
        int64_t n = sys_read(0, buf, sizeof buf);
        if (n <= 0) break; // a console has no EOF; this is an error

        for (int64_t i = 0; i < n; i++) {
            // Specials arrive as the same 0x91-0xA6 codes keyboard.h
            // defines and klineedit already switches on, so a byte off
            // fd 0 is fed in as-is. That identity is what makes the
            // shared editor work with no translation layer.
            int key = (unsigned char)buf[i];

            switch (kline_key(&g_ed, key)) {
            case KLINE_REDRAW:
                redraw();
                break;

            case KLINE_ACCEPT: {
                end_line();
                if (g_ed.len > 0) {
                    uhist_add(&g_hist, g_ed.buf);
                    tosh_run_line(&g_sh, g_ed.buf);
                }
                fresh_prompt();
                break;
            }

            case KLINE_CANCEL:
                end_line();
                put("^C\n");
                fresh_prompt();
                break;

            case KLINE_EOF:
                put("\n");
                return 0;

            case KLINE_HISTORY_PREV: {
                const char *e = uhist_prev(&g_hist, g_ed.buf);
                if (e) { kline_set(&g_ed, e); redraw(); }
                break;
            }

            case KLINE_HISTORY_NEXT: {
                const char *e = uhist_next(&g_hist);
                if (e) { kline_set(&g_ed, e); redraw(); }
                break;
            }

            case KLINE_LAST_ARG:
                insert_last_arg();
                redraw();
                break;

            case KLINE_CLEAR_SCREEN:
                // No clear-screen control code reaches ring 3 (that is
                // a terminal's job, and there is no terminal under this
                // yet), so the honest thing is a fresh line rather than
                // a screen that half-clears.
                put("\n");
                g_shown = 0;
                redraw();
                break;

            case KLINE_COMPLETE:
                // Tab does nothing here YET. Completion lives in
                // apps/completion.c, which is kernel-side and reaches
                // the filesystem through kernel APIs; a ring-3 version
                // is its own change (docs/roadmap.md) and a second
                // half-built one would be the drift this whole file is
                // avoiding.
                break;

            case KLINE_SEARCH:
                // Ctrl-R needs a second prompt line to type the query
                // into, which needs the caret control redraw() does not
                // have. Also a roadmap item; ignored rather than faked.
                break;

            case KLINE_IGNORED:
                break;
            }
        }
    }
    return 0;
}
