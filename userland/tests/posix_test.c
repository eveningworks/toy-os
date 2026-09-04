// The POSIX half of tolibc: <signal.h>, <sys/wait.h>, <termios.h>,
// <fcntl.h>, <strings.h> and getopt().
//
// WHAT THIS IS FOR. Every one of those headers sits on a syscall that
// already worked and already had a ring-3 test -- so "the syscall
// works" is not what is under test here. What is new is the
// TRANSLATION: POSIX's struct converted to the kernel's, a status
// decoded by macro instead of by hand, an option string parsed. A bug
// in any of those looks exactly like the underlying call being fine,
// which it is.
//
// SO EVERY CHECK COMPARES THE TWO SPELLINGS, or asserts a decode
// against a value produced deliberately. `waitpid(WNOHANG)` returning 0
// is only meaningful beside a child that really is still running; a
// termios round trip is only meaningful if something was actually
// changed and read back.
//
// MUST BE SCHEDULER-SPAWNED, same as signal_test.c: it has children, a
// process group and a terminal, none of which the legacy `run` loader
// gives a caller.
#include "rt/sys.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "lib/utest.h"

// noinline for the same reason signal_test.c states: a 192-byte line
// buffer per call, inlined into one main(), overruns the frame budget.
__attribute__((noinline))
// The call sites here read `check(what, ok, detail)`; the harness takes
// the boolean first. One adapter rather than transposing a hundred call
// sites: a transposed argument pair compiles and INVERTS the check,
// which is the failure a green suite hides.
static void check(const char *what, int ok, const char *detail) {
    utest_check_detail(ok, what, detail);
}

__attribute__((noinline))
static void checkf(const char *what, int ok, long got, long want) {
    char d[64];
    snprintf(d, sizeof d, "got %ld, want %ld", got, want);
    check(what, ok, ok ? 0 : d);
}

static volatile sig_atomic_t g_hits;
static void on_term(int sig) { (void)sig; g_hits++; }

int main(void) {
    utest_begin("posix_test", "the POSIX headers over the syscalls", UTEST_KLOG);

    // --- <strings.h> ---------------------------------------------------
    check("strncasecmp matches ignoring case", strncasecmp("HeLLo", "hello", 5) == 0, 0);
    check("strncasecmp respects its bound",
          strncasecmp("hello", "help!", 3) == 0 && strncasecmp("hello", "help!", 4) != 0, 0);
    {
        char buf[8];
        memset(buf, 0x5a, sizeof buf);
        bzero(buf, 4);
        check("bzero clears exactly n bytes",
              buf[0] == 0 && buf[3] == 0 && buf[4] == 0x5a, 0);
        // bcopy's ARGUMENTS ARE REVERSED from memmove's, which is the
        // one thing about it that can be wrong -- so the check has to
        // be asymmetric or it passes either way.
        char src[4] = { 1, 2, 3, 4 }, dst[4] = { 9, 9, 9, 9 };
        bcopy(src, dst, 4);
        check("bcopy copies src into dst, not the reverse",
              dst[0] == 1 && dst[3] == 4 && src[0] == 1, 0);
    }

    // --- <signal.h> ----------------------------------------------------
    check("strsignal names a signal", strcmp(strsignal(SIGSEGV), "Segmentation fault") == 0, 0);
    check("and an unknown number is not a crash",
          strcmp(strsignal(999), "Unknown signal") == 0, 0);

    {
        sigset_t s;
        sigemptyset(&s);
        check("an empty set contains nothing", sigismember(&s, SIGINT) == 0, 0);
        sigaddset(&s, SIGINT);
        check("and contains what was added",
              sigismember(&s, SIGINT) == 1 && sigismember(&s, SIGTERM) == 0, 0);
        sigdelset(&s, SIGINT);
        check("and not what was removed", sigismember(&s, SIGINT) == 0, 0);
        sigfillset(&s);
        check("a full set contains a valid signal", sigismember(&s, SIGTERM) == 1, 0);
        check("and a number that is not a signal is refused, not answered",
              sigaddset(&s, 99) == -1 && sigismember(&s, 99) == -1, 0);
    }

    // THE ROUND TRIP: install through POSIX's sigaction, read back
    // through it, and require the handler to come back unchanged. That
    // is what proves the struct conversion is symmetric -- a one-way
    // bug (writing the handler but reading back garbage) is invisible
    // to a test that only installs.
    {
        struct sigaction act, old;
        memset(&act, 0, sizeof act);
        act.sa_handler = on_term;
        act.sa_flags = SA_RESTART;
        check("sigaction installs a handler", sigaction(SIGTERM, &act, 0) == 0, 0);

        memset(&old, 0xff, sizeof old);
        check("and reads the same one back",
              sigaction(SIGTERM, 0, &old) == 0 && old.sa_handler == on_term, 0);
        check("with its flags", (old.sa_flags & SA_RESTART) != 0, 0);

        // AND IT ACTUALLY DELIVERS. Everything above would pass against
        // a sigaction() that stored a handler somewhere and never told
        // the kernel.
        g_hits = 0;
        raise(SIGTERM);
        checkf("raise() reaches the handler installed through sigaction", g_hits == 1, g_hits, 1);

        // A NON-EMPTY MASK IS REFUSED RATHER THAN IGNORED. This is the
        // check that would fail if someone later "helpfully" dropped
        // the mask on the floor to make more code compile.
        memset(&act, 0, sizeof act);
        act.sa_handler = on_term;
        sigaddset(&act.sa_mask, SIGINT);
        check("a non-empty sa_mask is refused, not silently dropped",
              sigaction(SIGTERM, &act, 0) == -1 && errno == EINVAL, 0);

        signal(SIGTERM, SIG_DFL);
    }

    check("signal() refuses SIGKILL", signal(SIGKILL, on_term) == SIG_ERR, 0);
    check("kill() refuses a non-positive pid", kill(0, SIGTERM) == -1 && errno == EINVAL, 0);

    // --- <sys/wait.h> --------------------------------------------------
    //
    // THE MACROS DECODED AGAINST DELIBERATE VALUES FIRST, so a wrong
    // boundary is named here rather than showing up as a confusing
    // child-exit result below.
    check("WIFEXITED/WEXITSTATUS decode a normal exit",
          WIFEXITED(7) && WEXITSTATUS(7) == 7 && !WIFSIGNALED(7) && !WIFSTOPPED(7), 0);
    check("WIFSIGNALED/WTERMSIG decode a death by signal",
          WIFSIGNALED(SIGNAL_EXIT_BASE + SIGTERM) &&
          WTERMSIG(SIGNAL_EXIT_BASE + SIGTERM) == SIGTERM &&
          !WIFEXITED(SIGNAL_EXIT_BASE + SIGTERM), 0);
    check("WIFSTOPPED/WSTOPSIG decode a stop",
          WIFSTOPPED(SIGNAL_STOP_BASE + SIGTSTP) &&
          WSTOPSIG(SIGNAL_STOP_BASE + SIGTSTP) == SIGTSTP &&
          !WIFSIGNALED(SIGNAL_STOP_BASE + SIGTSTP), 0);
    check("the three ranges do not overlap at their boundaries",
          WIFEXITED(SIGNAL_EXIT_BASE - 1) && WIFSIGNALED(SIGNAL_EXIT_BASE) &&
          WIFSIGNALED(SIGNAL_STOP_BASE - 1) && WIFSTOPPED(SIGNAL_STOP_BASE), 0);

    // AND AGAINST A REAL CHILD. /bin/hello exits 0 promptly.
    {
        int pid = sys_spawn_group("/bin/hello", 0, -1, 0, PGID_NEW);
        if (pid > 0) {
            int st = -1;
            int r = waitpid(pid, &st, 0);
            checkf("waitpid reaps a real child", r == pid, r, pid);
            check("and its status decodes as a normal exit",
                  WIFEXITED(st) && WEXITSTATUS(st) == 0, 0);
        } else {
            check("spawning /bin/hello", 0, "spawn failed");
        }
    }

    // WNOHANG RETURNS 0, NOT -1, while a child is still running -- the
    // one place the wrapper has to translate SYS_RETRY, and a
    // pass-through would look like an error to every caller.
    {
        // /tests/spin_test spins across many timer slices and prints
        // NOTHING, which is what makes it usable here -- a chatty child
        // would interleave into the very stream this report is read
        // from. Same child signal_test.c uses, and the same reason.
        int pid = sys_spawn_group("/tests/spin_test", "100000", -1, 0, PGID_NEW);
        if (pid > 0) {
            int st = -1;
            int r = waitpid(pid, &st, WNOHANG);
            checkf("waitpid(WNOHANG) reports 0 for a child still running", r == 0, r, 0);
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            check("and that child's status decodes as a death by signal",
                  WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, 0);
        }
    }

    // --- <termios.h> ---------------------------------------------------
    //
    // A ROUND TRIP THROUGH THE TERMINAL, not through our own struct:
    // change a flag, write it, read it back from the kernel. A
    // conversion that dropped c_lflag entirely would pass a check that
    // only looked at the caller's copy.
    {
        struct termios t, back;
        if (tcgetattr(0, &t) == 0) {
            tcflag_t was = t.c_lflag;

            // "A TERMINAL STARTS IN THE POSIX DEFAULT" WAS THE FIRST
            // VERSION OF THIS CHECK AND IT WAS A FIXTURE ASSUMPTION,
            // not a property of this library: TTY_LFLAG_DEFAULT is what
            // a terminal is CREATED with, and every shell here turns
            // ICANON and ECHO off at startup (abi/tty_abi.h says so) --
            // so a test spawned from one never sees it. It failed
            // against a correct build.
            //
            // What replaced it tests the conversion in BOTH DIRECTIONS,
            // which is more than the original did: set the bits ON,
            // read them back, then clear them and read back again. A
            // tcsetattr that masked everything to zero would pass the
            // clearing half alone.
            t.c_lflag |= ICANON | ECHO;
            check("tcsetattr can turn ICANON and ECHO on",
                  tcsetattr(0, TCSANOW, &t) == 0 && tcgetattr(0, &back) == 0 &&
                  (back.c_lflag & ICANON) && (back.c_lflag & ECHO), 0);

            t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
            check("and off again", tcsetattr(0, TCSANOW, &t) == 0, 0);
            check("with the terminal reporting each way round",
                  tcgetattr(0, &back) == 0 &&
                  !(back.c_lflag & ICANON) && !(back.c_lflag & ECHO), 0);

            // cfmakeraw CLEARS ISIG TOO, which is the difference between
            // it and the two lines above -- and the reason a full-screen
            // program calls it rather than writing its own.
            struct termios raw;
            tcgetattr(0, &raw);
            raw.c_lflag |= ISIG;
            cfmakeraw(&raw);
            check("cfmakeraw clears ISIG as well as ICANON and ECHO",
                  !(raw.c_lflag & (ICANON | ECHO | ISIG)), 0);

            // AN INERT FLAG MUST NOT REACH THE KERNEL. <termios.h>
            // defines IEXTEN and friends outside tty_abi.h's flag
            // space; if tcsetattr passed c_lflag through unmasked they
            // would be stored, and a future TTY_* bit landing there
            // would be set by every program that ever cleared one.
            struct termios inert = back;
            inert.c_lflag |= IEXTEN | ECHOE;
            tcsetattr(0, TCSANOW, &inert);
            check("an inert c_lflag bit is masked off, not stored",
                  tcgetattr(0, &back) == 0 && !(back.c_lflag & (IEXTEN | ECHOE)), 0);

            t.c_lflag = was;
            tcsetattr(0, TCSANOW, &t);   // leave the terminal as found
        } else {
            check("tcgetattr on fd 0", 0, "no terminal on fd 0");
        }

        struct winsize ws;
        check("tcgetwinsize answers in cells",
              tcgetwinsize(0, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0, 0);
    }

    // --- <fcntl.h> -----------------------------------------------------
    //
    // The point is that the NAMES map to the right bits. O_WRONLY|
    // O_CREAT|O_TRUNC has to actually create and truncate, and the mode
    // argument has to be accepted and ignored.
    {
        const char *path = "/tmp/posix_test.txt";
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        check("open(O_WRONLY|O_CREAT|O_TRUNC) with a mode argument", fd >= 0, 0);
        if (fd >= 0) {
            sys_write(fd, "abcd", 4);
            sys_close(fd);

            fd = open(path, O_RDONLY);
            char buf[8] = { 0 };
            long n = fd >= 0 ? sys_read(fd, buf, sizeof buf) : -1;
            if (fd >= 0) sys_close(fd);
            check("O_RDONLY reads back what was written",
                  n == 4 && memcmp(buf, "abcd", 4) == 0, 0);

            // O_TRUNC IS THE ONE THAT CAN BE SILENTLY WRONG: if it were
            // mapped to the wrong bit the file would still open and
            // still be written, just with the old tail left behind.
            fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) { sys_write(fd, "z", 1); sys_close(fd); }
            fd = open(path, O_RDONLY);
            memset(buf, 0, sizeof buf);
            n = fd >= 0 ? sys_read(fd, buf, sizeof buf) : -1;
            if (fd >= 0) sys_close(fd);
            checkf("O_TRUNC really truncated", n == 1, n, 1);
            sys_unlink(path);
        }
    }

    // --- getopt() ------------------------------------------------------
    //
    // Driven over a fixed argv, so the checks are about the PARSER and
    // not about however this program was invoked.
    {
        char *av[] = { (char *)"prog", (char *)"-la", (char *)"-o", (char *)"out",
                       (char *)"-v", (char *)"file", (char *)"-x", 0 };
        int ac = 7;
        optind = 1;
        opterr = 0;
        int c, seen_l = 0, seen_a = 0, seen_v = 0;
        char *arg = 0;
        while ((c = getopt(ac, av, "lao:v")) != -1) {
            if (c == 'l') seen_l = 1;
            else if (c == 'a') seen_a = 1;
            else if (c == 'v') seen_v = 1;
            else if (c == 'o') arg = optarg;
        }
        // A CLUSTER IS THE HALF HAND-ROLLED PARSERS GET WRONG: "-la"
        // must be two options, not one unknown one.
        check("getopt splits a cluster into separate options", seen_l && seen_a, 0);
        check("and takes a separated option argument",
              arg && strcmp(arg, "out") == 0, 0);
        check("and sees an option before the first operand", seen_v, 0);
        // AND STOPS AT THE OPERAND. The `-x` after "file" must NOT be
        // parsed -- that is the POSIX behaviour this deliberately has
        // rather than glibc's argv permutation, and without this check
        // either would pass.
        checkf("and STOPS at the first non-option, leaving it at optind",
               optind == 5, optind, 5);
        check("with the operand itself still there",
              strcmp(av[optind], "file") == 0, 0);

        // An attached argument: -oout is the same as -o out.
        char *av2[] = { (char *)"prog", (char *)"-oout", 0 };
        optind = 1;
        c = getopt(2, av2, "o:");
        check("an attached option argument works too",
              c == 'o' && optarg && strcmp(optarg, "out") == 0, 0);

        // "--" ends the options and is consumed, so a following "-n" is
        // an operand rather than an unknown option.
        char *av3[] = { (char *)"prog", (char *)"--", (char *)"-n", 0 };
        optind = 1;
        check("-- ends the options and is consumed",
              getopt(3, av3, "n") == -1 && optind == 2, 0);

        // A MISSING ARGUMENT IS REPORTED, not invented.
        char *av4[] = { (char *)"prog", (char *)"-o", 0 };
        optind = 1;
        check("a missing option argument is reported",
              getopt(2, av4, ":o:") == ':' && optopt == 'o', 0);

        char *av5[] = { (char *)"prog", (char *)"-q", 0 };
        optind = 1;
        check("an unknown option is '?' with optopt set",
              getopt(2, av5, "o:") == '?' && optopt == 'q', 0);
    }

    // --- <unistd.h> additions -------------------------------------------
    check("getpid agrees with the syscall", getpid() == sys_getpid(), 0);
    check("getpgrp answers this process's group", getpgrp() == sys_getpgid(0), 0);

    return utest_end();
}
