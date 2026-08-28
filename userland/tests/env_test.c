// The environment: getenv/setenv/unsetenv/putenv, and the thing that
// actually matters -- that a CHILD INHERITS one.
//
// **THE INHERITANCE CHECK IS THE POINT, and it cannot be done in one
// process.** Everything else here is string handling in a table; only a
// real spawn proves the blob was flattened, crossed the syscall
// boundary, was placed on a different process's initial stack, and was
// found there by that process's crt0. So this test spawns
// /tests/env_child and reads back what the child saw, through a pipe.
//
// The child is told to look for a variable this parent invented at
// RUNTIME, not one init seeded -- a child finding PATH proves only that
// init ran, while a child finding a value this process chose a
// millisecond ago proves the whole path.
//
// IT IS SPAWNED, NOT `run`. The parent has to block reading the pipe
// the child writes to, and the legacy `run` loader has no scheduler
// slot to block on -- under it the read returns immediately and the
// child's output is simply missed, which reads exactly like the
// environment not crossing. The verdict also goes to a FILE, since a
// spawned program's console output arrives while the harness is between
// commands.
//
// Prints one line per check and exits with the number of failures.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "rt/sys.h"

#define VERDICT_PATH "/tmp/env_test.out"

static FILE *g_log;
static void say(const char *s) { fputs(s, stdout); if (g_log) fputs(s, g_log); }

static int g_fail;

static void check(int ok, const char *what) {
    say(ok ? "  ok   " : "  FAIL ");
    say(what);
    say("\n");
    if (!ok) g_fail++;
}

static char putenv_buf[] = "PUTENV_VAR=static-storage";

int main(void) {
    g_log = fopen(VERDICT_PATH, "w");
    say("env_test: getenv/setenv, and a child inheriting\n");

    // --- what init seeded, IF this process descends from init --------
    //
    // It may not. The ring-0 shell's `run` starts a program through the
    // legacy loader, which has no parent with an environment to pass --
    // so a program run that way correctly has none at all. That is a
    // property of the ring-0 shell rather than a gap in the mechanism,
    // and asserting PATH unconditionally would fail for a reason that
    // has nothing to do with the environment working.
    //
    // (An earlier version did assert it, then dereferenced the NULL it
    // got back two checks later and took a page fault -- which is why
    // `path &&` guards are not decoration here.)
    const char *path = getenv("PATH");
    int from_init = (path != 0);
    if (from_init) {
        check(strcmp(path, "/bin") == 0, "PATH was inherited from init");
        const char *home = getenv("HOME");
        check(home && strcmp(home, "/") == 0, "and HOME");
    } else {
        say("  note   no inherited environment -- this process's parent had\n"
            "         none to pass. The child below must MIRROR that, which\n"
            "         is the property being tested either way.\n");
    }
    check(getenv("DEFINITELY_NOT_SET") == 0, "an unset variable answers NULL");
    check(getenv("") == 0, "and so does an empty name");

    // --- setenv -------------------------------------------------------
    check(setenv("TOYOS_A", "one", 1) == 0, "setenv adds");
    check(strcmp(getenv("TOYOS_A"), "one") == 0, "and the value reads back");
    check(setenv("TOYOS_A", "two", 0) == 0, "setenv with overwrite=0 succeeds");
    check(strcmp(getenv("TOYOS_A"), "one") == 0, "...and leaves the old value");
    check(setenv("TOYOS_A", "two", 1) == 0, "setenv with overwrite=1");
    check(strcmp(getenv("TOYOS_A"), "two") == 0, "...replaces it");
    check(setenv("TOYOS_EQ=BAD", "x", 1) == -1, "a name containing '=' is refused");

    // The FIRST setenv moved the whole environment off the initial
    // stack onto the heap, and the casualties of a wrong copy would be
    // exactly the INHERITED entries. This process may have none (see
    // above), so the real check for that lives in the child, which
    // always has one.
    if (from_init)
        check(getenv("PATH") && strcmp(getenv("PATH"), "/bin") == 0,
              "PATH survived the move to the heap");

    // --- many entries, to force the array to grow ---------------------
    char name[32];
    for (int i = 0; i < 40; i++) {
        snprintf(name, sizeof name, "TOYOS_N%d", i);
        if (setenv(name, "v", 1) != 0) { check(0, "setenv failed while growing"); break; }
    }
    int found = 1;
    for (int i = 0; i < 40; i++) {
        snprintf(name, sizeof name, "TOYOS_N%d", i);
        const char *v = getenv(name);
        if (!v || strcmp(v, "v") != 0) { found = 0; break; }
    }
    check(found, "40 additions all survive the array growing");
    if (from_init)
        check(getenv("PATH") && strcmp(getenv("PATH"), "/bin") == 0,
              "and PATH still does too");

    // --- unsetenv -----------------------------------------------------
    check(unsetenv("TOYOS_A") == 0, "unsetenv removes");
    check(getenv("TOYOS_A") == 0, "and it is gone");
    check(unsetenv("NEVER_EXISTED") == 0, "removing something absent is not an error");
    // unsetenv moves the LAST entry into the gap, so the entry that was
    // last is the one that would vanish if that swap were wrong.
    check(getenv("TOYOS_N39") != 0, "the entry moved into the gap is still there");

    // --- putenv -------------------------------------------------------
    check(putenv(putenv_buf) == 0, "putenv accepts an entry");
    check(strcmp(getenv("PUTENV_VAR"), "static-storage") == 0, "which reads back");
    check(putenv((char *)"NO_EQUALS_SIGN") == -1, "and refuses one with no '='");

    // --- THE REAL CHECK: a child inherits -----------------------------
    check(setenv("TOYOS_SECRET", "passed-down", 1) == 0, "set a variable for the child");

    int p[2];
    if (sys_pipe(p) != 0) {
        check(0, "could not make a pipe");
    } else {
        int pid = sys_spawn("/tests/env_child", "TOYOS_SECRET", p[1]);
        check(pid > 0, "spawned /tests/env_child");
        sys_close(p[1]);   // the parent's copy, so the child's is the last writer

        static char buf[256];
        int n = 0;
        for (;;) {
            int64_t r = sys_read(p[0], buf + n, (uint64_t)(sizeof buf - 1 - n));
            if (r <= 0) break;
            n += (int)r;
            if (n >= (int)sizeof buf - 1) break;
        }
        buf[n] = '\0';
        sys_close(p[0]);
        int code = 0;
        sys_waitpid(pid, &code);

        // The child reports THREE things in one line, and each is a
        // link in the chain that only a real spawn can test:
        //   <value>   -- what this process set crossed the syscall
        //                boundary and was found on the child's stack
        //   kept      -- and survived the child's own first setenv,
        //                which moves an inherited environment off that
        //                stack and onto the heap
        //   /bin      -- init's PATH reached a grandchild, so the whole
        //                inheritance chain is intact
        // The child must MIRROR this process: the variable set a
        // moment ago, and whatever PATH this process itself has --
        // which may be init's /bin or may be nothing, depending on who
        // started us. Asserting a literal "/bin" would test the
        // ancestry rather than the mechanism, and would fail whenever
        // the chain is short.
        const char *mypath = getenv("PATH");
        static char want[128];
        snprintf(want, sizeof want, "passed-down|kept|%s\n",
                 mypath ? mypath : "(unset)");
        check(strcmp(buf, want) == 0,
              "the CHILD inherited it, kept it across a heap move, and mirrors PATH");
        if (strcmp(buf, want) != 0) {
            static char msg[320];
            snprintf(msg, sizeof msg, "       child said \"%s\", wanted \"%s\"\n",
                     buf, want);
            say(msg);
        }
    }

    static char verdict[64];
    if (g_fail) snprintf(verdict, sizeof verdict, "env_test: %d FAILURES\n", g_fail);
    else        snprintf(verdict, sizeof verdict, "env_test: all checks passed\n");
    say(verdict);
    if (g_log) fclose(g_log);
    return g_fail;
}
