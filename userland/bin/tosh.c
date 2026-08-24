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
// THERE IS NO ECHO FROM THE KERNEL, BECAUSE THIS SHELL ASKS FOR NONE.
// main() calls sys_tty_raw(0) -- ICANON and ECHO off, ISIG on -- so
// bytes arrive as typed and what the user sees is what redraw() prints.
// There IS a line discipline now (kernel/tty/ldisc.c); a program with
// its own editor turns it off, exactly as readline does on Linux. The
// call is unconditional, which is what lets this one binary run on the
// physical console and inside a Terminal window without knowing which.
#include "rt/sys.h"
#include "lib/tosh.h"
#include <string.h>
#include "lib/uhistory.h"
#include "klineedit.h"
#include "signal_abi.h" // SIGCHLD, struct k_sigaction -- see main()

// Static, not local: `struct kline_edit` is ~1.2 KiB and `struct
// uhistory` ~4 KiB, against USERLAND_CFLAGS' -Wframe-larger-than=2048
// and a 16 KiB ring-3 stack with ONE guard page below it.
// The terminal as it was before this shell put it in raw mode. Restored
// around every command, so a child gets a normal, canonical, echoing
// terminal -- which is what a program that knows nothing about terminals
// expects, and is the state its own Ctrl-D and Ctrl-C work in.
static struct tty_termios g_tio_saved;

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

// `<cwd>$ `. The `$` is what says RING 3 -- the kernel's own shell shows
// `<cwd># `, Unix's convention for the privileged one -- and the cwd is
// what this prompt was missing: it was a bare `$`, the one shell on the
// machine that could not tell you where it was standing.
//
// ASKED OF THE KERNEL EVERY TIME, not cached. The cwd belongs to the
// PROCESS (SYS_CHDIR), so a copy here would be a second record of it
// and would show as a prompt naming a directory this shell is not in.
// Cheap: one syscall per prompt, and a prompt is drawn when a human has
// just pressed a key.
//
// Static buffer because redraw() calls this several times per repaint
// and compares its LENGTH against what is on screen -- a fresh buffer
// per call would be a dangling pointer the moment it returned.
static char g_prompt[TOSH_PATH_MAX + 4];

static const char *prompt(void) {
    char here[TOSH_PATH_MAX];
    if (sys_getcwd(here, sizeof here) < 0) { here[0] = '/'; here[1] = '\0'; }
    int n = 0;
    for (const char *p = here; *p && n < (int)sizeof g_prompt - 3; p++) g_prompt[n++] = *p;
    g_prompt[n++] = '$';
    g_prompt[n++] = ' ';
    g_prompt[n] = '\0';
    return g_prompt;
}

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

// --- news from a background job ----------------------------------------
//
// **THE HANDLER DOES NOTHING BUT SAY IT HAPPENED**, and that is not
// laziness -- it is the async-signal-safety rule. A handler runs between
// two arbitrary instructions of whatever this shell was doing, so it may
// not print (sys_write into a half-written prompt), may not touch the
// job table (tosh_reap_jobs() walks it, and reap_and_repaint() below may
// be halfway through the same walk), and may not call anything that
// allocates. Setting one flag is the whole POSIX-safe repertoire, and it
// is what every real shell's SIGCHLD handler does. bash's is three
// lines of exactly this shape.
//
// `volatile` because the read loop's only other view of this variable is
// through a syscall the compiler is entitled to think cannot change it.
static volatile int g_child_news;

static void on_sigchld(int sig) { (void)sig; g_child_news = 1; }

// What the flag means when the shell gets round to it: erase the line
// being typed, print whatever the jobs did, and put the line back.
//
// **THE LINE HAS TO BE ERASED, NOT JUST SCROLLED PAST.** A report
// printed on top of a half-typed command leaves the old characters on
// screen with nothing to say they are stale, and redraw()'s two passes
// only overwrite as far as g_shown -- they cannot know something else
// wrote to the row in between. So the row is blanked first, by the same
// '\r'-and-spaces means redraw() uses (this shell has no cursor
// addressing; see redraw()'s comment on why).
static void reap_and_repaint(void) {
    g_child_news = 0;

    put("\r");
    int width = (int)strlen(prompt()) + g_shown;
    for (int i = 0; i < width; i++) put(" ");
    put("\r");

    tosh_reap_jobs(&g_sh);
    redraw();
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
    // A PROMPT IS WHERE BACKGROUND NEWS GOES. Every path that draws one
    // comes through here -- a finished command, a bare Enter, a
    // cancelled line -- so this is the one place `[1]+ Done` can be
    // printed without landing in the middle of something else. Before
    // the repaint, so the report scrolls above the prompt rather than
    // over it.
    tosh_reap_jobs(&g_sh);

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
    tosh_init(&g_sh, out_fd1, 0);

    // --- `-c <command>`: run ONE line and exit -----------------------
    //
    // The non-interactive shell, which is what `system()` needs and what
    // every real shell has. It returns BEFORE any of the interactive
    // setup below -- no history, no raw mode, no job control, no signal
    // handlers, and no prompt -- because none of that belongs to a shell
    // that is about to run one command and leave.
    //
    // **IT MUST NOT TOUCH THE TERMINAL**, and that is the reason for the
    // early return rather than a flag threaded through main(). A
    // `system()` call from a GUI app has no terminal of its own; putting
    // fd 0 in raw mode there would reconfigure whatever terminal it did
    // inherit and leave it that way for the parent, which is the bug
    // this shell already documents at the other end (it saves and
    // restores the termios around every command it runs).
    //
    // Everything after the flag is joined back with spaces rather than
    // requiring one quoted argument, so `tosh -c ls /bin` works as
    // typed. A real shell would take argv[2] alone and treat the rest as
    // $0/$1..., but there are no positional parameters here to give them
    // to, and silently dropping them would be worse than joining them.
    if (argc >= 3 && argv[1] && argv[1][0] == '-' && argv[1][1] == 'c' &&
        argv[1][2] == '\0') {
        char line[512];
        unsigned n = 0;
        for (int i = 2; i < argc && n < sizeof line - 1; i++) {
            if (i > 2 && n < sizeof line - 1) line[n++] = ' ';
            for (const char *p = argv[i]; *p && n < sizeof line - 1; p++)
                line[n++] = *p;
        }
        line[n] = '\0';
        return tosh_run_line(&g_sh, line);
    }

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
    sys_signal(SIGINT, (sighandler_t)SIG_IGN);

    // AND THE TERMINAL IS PUT IN RAW MODE, because this shell edits for
    // itself (klineedit.c above) and would otherwise be fighting the
    // kernel's line discipline: canonical mode would hold every line
    // until Enter -- so redraw() would paint nothing as you type -- and
    // ECHO would double every character.
    //
    // **UNCONDITIONAL, AND THAT IS WHAT MAKES ONE BINARY WORK ON BOTH.**
    // tty0 already starts raw, so this changes nothing on the physical
    // console; a pty starts POSIX (abi/tty_abi.h), so in a Terminal
    // window this is what makes the shell usable. A shell that had to
    // know which kind of terminal it was on would mean the tty layer had
    // failed. Failure is ignored on purpose: a shell reading a pipe has
    // no terminal to configure and should still run.
    // ...AND SAVES WHAT IT WAS FIRST, because raw mode is this shell's
    // preference and not the terminal's state. Every command it runs
    // gets the terminal back the way it found it -- see run_line()
    // below. That is exactly what readline does around a command, and
    // without it `cat` with no arguments could never end: the shell
    // would have left the terminal raw, where there is no VEOF and
    // Ctrl-D is just a byte.
    sys_tcgetattr(0, &g_tio_saved);
    sys_tty_raw(0);

    // **NO SA_RESTART, AND THAT IS THE ENTIRE POINT OF INSTALLING IT.**
    // With the flag, the kernel rewinds the interrupted read and this
    // shell never learns anything happened -- which is what
    // sys_signal() would give (rt/sys.c sets SA_RESTART for every
    // handler it installs, the right default for code that does not
    // want to grow an EINTR loop). Here the interruption IS the message:
    // it is what turns a shell parked in sys_read() into one that can
    // report a finished background job while nobody is typing.
    //
    // The blocking call that must NOT be interrupted -- waiting on a
    // foreground job -- is protected in libsys instead, where
    // sys_waitpid() retries on EINTR (rt/sys.c). That is the right place
    // for it: a wait for a named child means the same thing whether or
    // not a signal arrived, and every caller wants that, not just this
    // one.
    struct k_sigaction chld = {
        .handler  = (uint64_t)(uintptr_t)on_sigchld,
        .restorer = (uint64_t)(uintptr_t)__sigrestore,
        .flags    = 0,
    };
    sys_sigaction(SIGCHLD, &chld, 0);

    // The console is CLAIMED BY READING IT, and the foreground group is
    // set at the same moment (kernel/tty.h) -- so nothing here has to
    // call tcsetpgrp: the first sys_read(0, ...) below does it, and
    // job_foreground()/job_done() in the tosh library move it per job.

    put("tosh -- the toy-os shell, in ring 3. Ctrl-D to exit.\n");
    fresh_prompt();

    // A SHELL IS USABLE WHEN THERE IS A PROMPT ON THE SCREEN, which is
    // here and not at main(): everything above -- the signal handlers,
    // the job table, the terminal -- has to be in place before a typed
    // Ctrl-C means anything. init reads this for a service whose
    // descriptor says `Ready=notify` (data/etc/services.d/README.md);
    // it is a no-op for the far more common case of a tosh started from
    // another shell or by a pty, so this needs no test for how it was
    // started. Deliberately AFTER the interactive setup and never
    // reached by `tosh -c`, which returns long before this point.
    sys_notify_ready();

    for (;;) {
        char buf[32];

        // **THE FLAG IS TESTED BEFORE THE READ, NOT ONLY AFTER IT**, and
        // without this line a job that finishes in the gap between one
        // read returning and the next one starting is reported at the
        // NEXT keystroke instead of now -- which is the whole of what
        // this was built to avoid, failing in exactly the case it was
        // built for (a `&` job that exits almost immediately).
        //
        // On Unix this test-then-block is the classic race that pselect()
        // and sigsuspend() exist to close: the signal can arrive after
        // the test and before the sleep, and be missed. It is NOT a race
        // here, and the reason is where this kernel delivers -- a signal
        // that becomes pending in that window is acted on at SYSCALL
        // ENTRY, before the read runs at all (kernel/signal.h), so it
        // comes straight back as EINTR rather than being swallowed by a
        // read that already parked. What closes the window is the
        // kernel's delivery point, not anything this loop does.
        if (g_child_news) reap_and_repaint();

        // Blocks. The kernel parks this process on SCHED_WAIT_KEY and
        // the keyboard IRQ releases it, so an idle shell costs nothing
        // -- `ps` shows it blocked, not ready.
        int64_t n = sys_read(0, buf, sizeof buf);

        // **INTERRUPTED, NOT BROKEN.** A background job finishing is the
        // one thing that reaches this shell while it is parked here with
        // nothing to do, and SIGCHLD is deliberately installed WITHOUT
        // SA_RESTART (see main()) so that it does: the read fails with
        // EINTR, the news gets printed, and the read is entered again.
        // Without this branch the -1 would fall into the `n <= 0` exit
        // below and a finished background job would close the shell.
        if (n < 0 && sys_errno() == EINTR) {
            if (g_child_news) reap_and_repaint();
            continue;
        }
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
                    // THE TERMINAL GOES BACK TO HOW IT WAS FOR THE
                    // CHILD, and raw again afterwards. A child inherits
                    // this terminal, and a program that knows nothing
                    // about terminals -- `cat` with no arguments is the
                    // one that proves it -- needs canonical mode with
                    // echo: it is where Ctrl-D means end of input and
                    // where the person can see what they are typing.
                    // readline brackets a command exactly this way.
                    sys_tcsetattr(0, &g_tio_saved);
                    tosh_run_line(&g_sh, g_ed.buf);
                    sys_tty_raw(0);
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
