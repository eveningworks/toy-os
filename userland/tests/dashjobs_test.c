// Does /bin/dash actually DO job control?
//
// dash's docs page called this "unproven" for a while, and it was: the
// harness that drives the other 22 cases (tools/dash_test.py) runs
// scripts, and a script has no terminal, so `fg`/`bg`/`jobs` and the
// signals around them were never exercised at all.
//
// **THE CLAIM WORTH TESTING IS THE LAST ONE HERE**: after `fg`, a
// Ctrl-C must kill THE JOB and leave the shell alive. That can only
// happen if `fg` moved the terminal's foreground group to the job --
// which is the tcsetpgrp() that used to fail outright with "Cannot set
// tty process group" before terminals belonged to sessions. A test that
// stopped at "jobs lists something" would pass with job control
// half-broken.
//
// A pty, for the same reason /tests/dashedit_test uses one: dash turns
// job control on only when it is interactive on a terminal. Each
// failure has its own exit code; kernel/tty/tty_test.c drives this.
#include <stdint.h>
#include "rt/sys.h"

#define DASH "/bin/dash"
#define JOB  "/tests/spin_test 900000\n"

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

#define CLEAR(a) do { for (int i_ = 0; i_ < (int)sizeof (a); i_++) (a)[i_] = 0; } while (0)

int main(void) {
    int master = -1, slave = -1;

    if (sys_openpty(&master, &slave) < 0) return 1;
    if (sys_set_nonblock(master, 1) < 0) return 2;

    int saved_in = sys_dup(0), saved_out = sys_dup(1), saved_err = sys_dup(2);
    sys_dup2(slave, 0);
    sys_dup2(slave, 1);
    sys_dup2(slave, 2);
    // **SPAWN_SETSID, because that is what a terminal emulator does.**
    // The shell leads its own session and the pty becomes that session's
    // controlling terminal, which is what makes tcgetpgrp() answer
    // during dash's startup and job control stay ON.
    int sh = sys_spawn_flags(DASH, "-i", -1, 0, PGID_NEW, SPAWN_SETSID);
    if (saved_in  >= 0) { sys_dup2(saved_in, 0);  sys_close(saved_in); }
    if (saved_out >= 0) { sys_dup2(saved_out, 1); sys_close(saved_out); }
    if (saved_err >= 0) { sys_dup2(saved_err, 2); sys_close(saved_err); }
    if (sh < 0) return 3;

    int mark = waited(master, "#", 0);
    if (mark < 0) return 4;

    // --- 1. `&` returns the prompt instead of waiting ----------------
    //
    // **dash PRINTS NOTHING when it backgrounds a job** -- the `[1] pid`
    // line is bash's and ksh's, and dash's own `[%d]` appears only in
    // `jobs` and `bg`. So the evidence here is the PROMPT coming back
    // while a 900000-round spin is still going: a shell that waited
    // would not give it back for many seconds.
    if (put(master, "/tests/spin_test 900000 &\n") < 0) return 5;
    if ((mark = waited(master, "#", mark)) < 0) return 6;

    // --- 2. it is in the job table, numbered, and RUNNING ------------
    if (put(master, "jobs\n") < 0) return 7;
    if ((mark = waited(master, "[1]", mark)) < 0) return 8;
    if ((mark = waited(master, "Running", mark)) < 0) return 17;

    // --- 3. `fg` MOVES THE TERMINAL, which is the real claim ---------
    //
    // Then Ctrl-C. It reaches whatever group is in FRONT of the
    // terminal: the job if fg did its work, and the shell itself if it
    // did not. So the shell surviving AND prompting again is the
    // evidence -- the job was the one that died.
    if (put(master, "fg\n") < 0) return 9;
    if ((mark = waited(master, "spin_test", mark)) < 0) return 10;

    if ((int)sys_write(master, "\x03", 1) != 1) return 11;
    if ((mark = waited(master, "#", mark)) < 0) return 12;

    // --- 4. the shell is ALIVE, and the job is gone ------------------
    int before_jobs = mark;
    if (put(master, "jobs\n") < 0) return 13;
    if ((mark = waited(master, "#", mark)) < 0) return 14;
    // Nothing may still be Running between the `jobs` and the prompt it
    // printed: the Ctrl-C above went to the JOB, not to this shell.
    if (contains(acc + before_jobs, mark - before_jobs, "Running")) return 15;

    int code = -1;
    // SYS_RETRY means "still running", which is the answer this wants --
    // sys_waitpid() would block here and hide it (rt/sys.h says so).
    if (sys_waitpid_nohang(sh, &code) != SYS_RETRY) return 16;

    put(master, "exit\n");
    sys_close(master);
    sys_close(slave);
    return 0;
}
