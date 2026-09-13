// Does /bin/dash have line editing?
//
// dash ships WITHOUT libedit on Debian, and this port used to build the
// same way (`SMALL` in userland/backends/dash/config.h) -- so it had no
// arrow keys, no history and no `fc`, a downgrade from /bin/tosh at an
// interactive prompt. It gets them from
// userland/backends/dash/histedit_shim.c, which answers libedit's names
// with this system's one editor (kernel/lib/klineedit.c).
//
// **IT HAS TO BE A pty, AND THAT IS THE POINT.** dash only builds an
// EditLine when its input is interactive and a terminal, so a test
// driving it through a pipe measures the shell with editing switched
// off and passes whether or not any of this works. The serial debug
// console cannot do it either -- `vm.py exec` is not a tty.
//
// Each failure has its own exit code, so a red run says WHICH link
// broke. Asserted on by kernel/tty/... via /tests, and by
// tools/usertest_run.py.
#include <stdint.h>
#include "rt/sys.h"
#include "keyboard.h"   // KEY_ARROW_UP -- a byte off fd 0, as the kernel sends it

#define DASH "/bin/dash"

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static int contains(const char *hay, int hay_len, const char *needle) {
    int nl = slen(needle);
    if (nl == 0) return 1;
    for (int i = 0; i + nl <= hay_len; i++) {
        int j = 0;
        while (j < nl && hay[i + j] == needle[j]) j++;
        if (j == nl) return 1;
    }
    return 0;
}

static int put(int fd, const char *s) {
    int n = slen(s);
    return (int)sys_write(fd, s, (size_t)n) == n ? 0 : -1;
}

// **ONE BUFFER THAT IS NEVER CLEARED, and a watermark.** Waits until
// `needle` appears at or after `from`, which is the accumulated length
// taken BEFORE the write that should produce it.
//
// Clearing the buffer between phases loses bytes that were already
// read: a prompt arriving in the same read() as the previous command's
// output is thrown away, and the next phase then waits for something
// that has already gone past. That is a flake when the timing shifts
// and a hard failure when it does not -- it cost a wrong diagnosis
// here, so the shape is the fix rather than a longer budget.
static char acc[4096];
static int  acc_len;

// **RETURNS THE POSITION JUST PAST THE MATCH**, which is what the next
// phase passes as its `from`. Returning the buffer LENGTH instead loses
// anything that arrived in the same read() as the match -- a prompt
// printed in the same chunk as the command's output is then searched
// for from beyond where it already sits, and the phase times out. That
// is a real failure, not a flake, and it cost two wrong diagnoses here.
static int waited(int master, const char *needle, int from) {
    int nl = slen(needle);
    for (int tries = 0; tries < 250; tries++) {
        if (acc_len < (int)sizeof acc) {
            int n = (int)sys_read(master, acc + acc_len,
                                  (size_t)((int)sizeof acc - acc_len));
            if (n > 0) acc_len += n;
        }
        for (int i = from; i + nl <= acc_len; i++) {
            int j = 0;
            while (j < nl && acc[i + j] == needle[j]) j++;
            if (j == nl) return i + nl;
        }
        sys_sleep_ms(20);
    }
    return -1;
}

int main(void) {
    int master = -1, slave = -1;

    if (sys_openpty(&master, &slave) < 0) return 1;
    // See waited(): without this a shell that says nothing hangs the
    // test rather than failing it.
    if (sys_set_nonblock(master, 1) < 0) return 10;

    int saved_in = sys_dup(0), saved_out = sys_dup(1), saved_err = sys_dup(2);
    sys_dup2(slave, 0);
    sys_dup2(slave, 1);
    sys_dup2(slave, 2);
    int pid = sys_spawn_group(DASH, "-i", -1, 0, PGID_NEW);
    if (saved_in  >= 0) { sys_dup2(saved_in, 0);  sys_close(saved_in); }
    if (saved_out >= 0) { sys_dup2(saved_out, 1); sys_close(saved_out); }
    if (saved_err >= 0) { sys_dup2(saved_err, 2); sys_close(saved_err); }
    if (pid < 0) return 2;
    sys_tcsetpgrp(master, pid);

    // --- 1. it prompts -----------------------------------------------
    //
    // `#`, NOT `$`: dash picks its default PS1 from geteuid(), and every
    // id on this system answers 0, so dash believes it is root.
    // /bin/tosh's `$` means something else entirely -- ring 3 rather
    // than privilege -- and the two conventions collide by coincidence.
    int mark = waited(master, "#", 0);
    if (mark < 0) return 3;

    // --- 2. it runs what is typed ------------------------------------
    //
    // **NO `set -o emacs`, DELIBERATELY.** Editing is opt-in upstream,
    // and /bin/dash's entry turns it on by default because this
    // system's terminals send specials as raw 0x91-0xA6 bytes -- with
    // no editor an arrow key types blanks into the line, which is how
    // that was found. Sending `set -o emacs` here would test the shim
    // and leave the DEFAULT untested, and the default is what a person
    // meets.
    if (put(master, "echo alpha\n") < 0) return 4;
    if ((mark = waited(master, "alpha", mark)) < 0) return 5;

    // The prompt has to come BACK before a key means anything: the shim
    // owns raw mode only inside el_gets(), so a key sent while the
    // command is running is held by the discipline until a newline.
    if ((mark = waited(master, "#", mark)) < 0) return 11;

    // --- 3. THE CLAIM: Up recalls the last line ----------------------
    //
    // KEY_ARROW_UP as the kernel delivers it -- 0x91, which IS the code
    // klineedit switches on, and exactly what the GUI Terminal writes
    // to its pty for that key. A dash with no editor puts the byte in
    // the line instead, and the recall below never comes.
    char up = (char)KEY_ARROW_UP;
    if ((int)sys_write(master, &up, 1) != 1) return 6;
    // The recalled line is REPAINTED, so the text arrives on its own --
    // this is the history walk, not an echo of what was typed.
    if ((mark = waited(master, "echo alpha", mark)) < 0) return 7;

    // And it still RUNS when accepted, which a paint alone would not do.
    if (put(master, "\n") < 0) return 8;
    if (waited(master, "alpha", mark) < 0) return 9;

    // --- 4. the four keys that used to do nothing --------------------
    //
    // Each is recognised by the editor and acted on by the front end, so
    // a regression here means the shim stopped handling an action rather
    // than the editor forgetting a key.

    // Ctrl-L clears: ESC[2J is the erase-display sequence, and it is in
    // the stream only because something acted on KLINE_CLEAR_SCREEN.
    if ((int)sys_write(master, "\x0c", 1) != 1) return 12;
    if ((mark = waited(master, "\x1b[2J", mark)) < 0) return 13;

    // Alt-. inserts the last WORD of the previous line, which is
    // "alpha" -- an ESC prefix then '.', the way Meta arrives here.
    if ((int)sys_write(master, "\x1b", 1) != 1) return 14;
    if ((int)sys_write(master, ".", 1) != 1) return 14;
    if ((mark = waited(master, "alpha", mark)) < 0) return 15;
    // Ctrl-U clears the line again so the next phase starts clean.
    if ((int)sys_write(master, "\x15", 1) != 1) return 16;

    // Tab completes against the SHARED engine: `ech` has exactly one
    // completion, so the line becomes `echo `.
    if (put(master, "ech") < 0) return 17;
    if ((int)sys_write(master, "\t", 1) != 1) return 18;
    if ((mark = waited(master, "echo", mark)) < 0) return 19;
    if ((int)sys_write(master, "\x15", 1) != 1) return 20;

    // Ctrl-R opens the shared reverse search, which announces itself.
    if ((int)sys_write(master, "\x12", 1) != 1) return 21;
    if ((mark = waited(master, "reverse-i-search", mark)) < 0) return 22;
    if ((int)sys_write(master, "\x07", 1) != 1) return 23;   // Ctrl-G cancels

    put(master, "exit\n");
    sys_close(master);
    sys_close(slave);
    return 0;
}
