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

// Reads until `needle` shows up or the budget runs out. A BUDGET rather
// than one read, because the shell echoes its paint in whatever chunks
// the discipline hands over and a single read can land mid-repaint.
//
// **THE MASTER MUST BE NON-BLOCKING FIRST** (main() sets it): a blocking
// read with nothing pending never returns, so the budget below never
// elapses and a broken shell HANGS this test instead of failing it --
// which is exactly what /tests/pty_test's drain() warns about.
static int wait_for(int master, const char *needle, char *acc, int cap) {
    int len = 0;
    for (int tries = 0; tries < 200; tries++) {
        if (len < cap) {
            int n = (int)sys_read(master, acc + len, (size_t)(cap - len));
            if (n > 0) len += n;
        }
        if (contains(acc, len, needle)) return len;
        sys_sleep_ms(20);
    }
    return -1;
}

int main(void) {
    int master = -1, slave = -1;
    char acc[1024];

    if (sys_openpty(&master, &slave) < 0) return 1;
    // See wait_for(): without this a shell that says nothing hangs the
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
    // id on this system answers 0 (see the roadmap's dash milestone), so
    // dash believes it is root. /bin/tosh's `$` means something else
    // entirely -- ring 3 rather than privilege -- and the two
    // conventions collide here by coincidence.
    for (int i = 0; i < (int)sizeof acc; i++) acc[i] = 0;
    if (wait_for(master, "#", acc, sizeof acc) < 0) return 3;

    // --- 2. it runs what is typed ------------------------------------
    //
    // **EDITING IS OPT-IN IN dash**, as `set -o emacs` / `set -o vi`
    // upstream: Eflag starts clear, and histedit() builds an EditLine
    // only once it is set. Debian never hits this because it ships dash
    // without libedit at all.
    if (put(master, "set -o emacs\n") < 0) return 4;
    if (put(master, "echo alpha\n") < 0) return 4;
    for (int i = 0; i < (int)sizeof acc; i++) acc[i] = 0;
    if (wait_for(master, "alpha", acc, sizeof acc) < 0) return 5;

    // --- 3. THE ACTUAL CLAIM: Up recalls the last line ---------------
    //
    // The byte is KEY_ARROW_UP as the kernel delivers it -- 0x91, which
    // IS the code klineedit switches on, so it travels through the pty
    // with no translation (keyboard.h says why). A dash with no editor
    // treats it as an ordinary character and runs a command that does
    // not exist, so the echo below would never come back.
    // **WAIT FOR THE PROMPT BACK FIRST.** The shim puts the terminal in
    // raw mode inside el_gets() and restores it around the return, so
    // the command dash runs gets an ordinary canonical terminal. An
    // arrow sent during THAT window is buffered by the discipline until
    // a newline and never reaches the editor -- the key has to arrive
    // while the shell is the thing reading.
    for (int i = 0; i < (int)sizeof acc; i++) acc[i] = 0;
    if (wait_for(master, "#", acc, sizeof acc) < 0) return 11;

    char up = (char)KEY_ARROW_UP;
    if ((int)sys_write(master, &up, 1) != 1) return 6;
    for (int i = 0; i < (int)sizeof acc; i++) acc[i] = 0;
    // The recalled line is REPAINTED, so the text comes back on its own
    // -- this is the history walk, not an echo of what was typed.
    if (wait_for(master, "echo alpha", acc, sizeof acc) < 0) return 7;

    // And it still RUNS when accepted, which a paint alone would not do.
    if (put(master, "\n") < 0) return 8;
    for (int i = 0; i < (int)sizeof acc; i++) acc[i] = 0;
    if (wait_for(master, "alpha", acc, sizeof acc) < 0) return 9;

    put(master, "exit\n");
    sys_close(master);
    sys_close(slave);
    return 0;
}
