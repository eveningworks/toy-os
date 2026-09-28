// A path with spaces in it, handed to an app the way the desktop and the
// File Manager hand one over. Two programs in one binary, told apart by
// argc, as argv_test is:
//
//   spacepath_test <path>   the app being opened: records the argv[1]
//                           it was given, and exits
//   spacepath_test          the check: opens a spaced path through
//                           uopen_spawn() (double-click, /bin/open) and
//                           uapp_spawn() (Properties, "Open with"), and
//                           reads back what arrived
//
// Both used to pass the path as sys_spawn()'s argument STRING, which the
// kernel splits on whitespace, so "space path test.spt" arrived as
// "space" -- Properties showed a truncated name and Notepad opened
// nothing.
//
// SPAWNED, not `run`: it waits for the children it starts.
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "rt/sys.h"
#include "lib/uopen.h"
#include "lib/uconf.h"
#include "ui/uapp.h"
#include "lib/utest.h"

#define SELF   "/tests/spacepath_test"
#define TARGET "/tmp/space path test.spt"
#define SEEN   "/tmp/spacepath.seen"
#define EXT    ".spt"

static int child(const char *arg) {
    FILE *f = fopen(SEEN, "w");
    if (!f) return 1;
    fputs(arg, f);
    fclose(f);
    return 0;
}

// What the child recorded, "" if it never ran.
static void seen(char *out, int cap) {
    out[0] = '\0';
    FILE *f = fopen(SEEN, "r");
    if (!f) return;
    size_t n = fread(out, 1, (size_t)cap - 1, f);
    out[n] = '\0';
    fclose(f);
}

static void check_spawn(const char *how, int pid) {
    char got[256];
    utest_checkf(pid > 0, "%s: the spawn was accepted (pid %d)", how, pid);
    if (pid > 0) sys_waitpid(pid, 0);
    seen(got, sizeof got);
    utest_checkf(strcmp(got, TARGET) == 0,
                 "%s: the app was given the whole path (got \"%s\", wanted \"%s\")", how, got, TARGET);
    unlink(SEEN);
}

int main(int argc, char **argv) {
    if (argc > 1) return child(argv[1]);

    utest_begin("spacepath_test", "a path with spaces reaches the app whole",
                UTEST_VERDICT_FILE);
    FILE *t = fopen(TARGET, "w");
    utest_checkf(t != 0, "created \"%s\"", TARGET);
    if (t) fclose(t);
    unlink(SEEN);

    // The literal-path override, as `open -s .spt /tests/spacepath_test`
    // writes it: this binary is the app for the test's own extension.
    utest_checkf(uconf_set(UOPEN_CONF, EXT, SELF), "set the %s override", EXT);
    check_spawn("uopen_spawn", uopen_spawn(TARGET));
    uconf_unset(UOPEN_CONF, EXT);

    check_spawn("uapp_spawn", uapp_spawn(0, SELF, TARGET));

    unlink(TARGET);
    return utest_end();
}
