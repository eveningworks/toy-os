// Tab completion, in RING 3.
//
// WHY THIS EXISTS RATHER THAN ONLY A KTEST -- the same reason
// klineedit_test.c does. kernel/lib/completion.c has KTESTs
// (kernel/test/completion_test.c) covering its logic through the KERNEL
// shell's environment, and they would go on passing whether or not a
// single line of it were reachable from a ring-3 program. What is new is
// the LINK: the engine is compiled a second time into libuapp.a so
// `/bin/tosh` shares one definition of what completion means, and a
// KTEST runs inside the kernel and cannot see that build at all.
//
// So this asserts the RING-3 environment (userland/lib/ucomplete.c):
// that its filesystem hooks reach real directories through sys_listdir()
// and sys_stat(), that its builtin list is offered in first position,
// and that `cd` filters to directories -- the one command-specific rule
// tosh has.
//
// It builds its own fixture rather than assuming the disk: a test that
// completes against whatever happens to be in / passes or fails on
// unrelated files. Prints one line per case and exits with the number of
// failures.
#include <stdint.h>
#include "rt/sys.h"
#include <string.h>
#include <stdio.h>
#include "lib/ucomplete.h"

#include "lib/utest.h"

static struct completion_result g_r;

static int run(const char *line) {
    return completion_run_env(ucomplete_env(), line, (int)strlen(line), &g_r);
}

static int has(const char *want) {
    for (int i = 0; i < g_r.count; i++)
        if (strcmp(g_r.candidates[i], want) == 0) return 1;
    return 0;
}

// The call sites here read `check(what, ok, detail)`; the harness takes
// the boolean first. One adapter rather than transposing a hundred call
// sites: a transposed argument pair compiles and INVERTS the check,
// which is the failure a green suite hides.
static void check(const char *what, int ok, const char *detail) {
    utest_check_detail(ok, what, detail);
}

// The fixture: a directory and a FILE sharing a prefix, which is what
// makes the `cd` check discriminating -- with files included there are
// two candidates and completion can only reach the common prefix, so a
// broken filter is visible as a shorter insert rather than as nothing.
#define DIR_A  "/ct_probe_dir"
#define FILE_A "/ct_probe_file"

static void fixture_make(void) {
    sys_mkdir(DIR_A);
    int fd = sys_open(FILE_A, SYS_O_CREAT | SYS_O_WRITE | SYS_O_TRUNC);
    if (fd >= 0) { sys_write(fd, "x", 1); sys_close(fd); }
}

static void fixture_remove(void) {
    sys_unlink(FILE_A);
    sys_unlink(DIR_A);   // sys_unlink removes an empty directory too
}

int main(void) {
    utest_begin("complete_test", "the shared completion engine, built for ring 3", 0);
    fixture_remove();   // a previous run's leftovers would change the counts
    fixture_make();

    char detail[160];

    // 1. FIRST WORD: a builtin tosh dispatches itself.
    run("jo");
    check("a builtin completes in first position",
          g_r.count == 1 && has("jobs"), g_r.count == 1 ? "" : "count != 1");

    // 2. FIRST WORD: a real program, found by walking PATH through the
    // env's list_dir hook. This is the half a KTEST cannot reach.
    run("gre");
    snprintf(detail, sizeof detail, "count %d", g_r.count);
    check("a PATH program completes in first position", has("grep"), detail);

    // 3. A DIRECTORY IS NOT A COMMAND. /bin/wm is a directory, so `wm/`
    // must not be offered as a first word -- it could never run.
    run("w");
    check("a directory in PATH is not offered as a command", !has("wm/"), "");

    // 4. PATH COMPLETION on an argument, through sys_listdir().
    run("cat /ct_probe_f");
    check("an argument completes as a path",
          g_r.count == 1 && has("/ct_probe_file"), detail);

    // 5. THE DISCRIMINATING ONE: `cd` sees the directory and NOT the
    // file beside it, so a single candidate remains and completion can
    // finish the word. Without the filter both match and `insert` stops
    // at the shared prefix.
    run("cd /ct_probe_");
    snprintf(detail, sizeof detail, "count %d, insert '%s'", g_r.count, g_r.insert);
    check("cd offers directories only", g_r.count == 1 && has(DIR_A "/"), detail);

    // 6. ...and the control beside it: the same word after a command
    // whose arguments are ordinary paths sees BOTH.
    run("cat /ct_probe_");
    snprintf(detail, sizeof detail, "count %d", g_r.count);
    check("...and an ordinary command still sees both", g_r.count == 2, detail);

    // 7. A directory candidate does NOT get a trailing space: the user
    // is probably continuing the path.
    //
    // **A UNIQUE prefix, not the shared one.** Asking this of
    // "/ct_probe_" made the check pass for the wrong reason -- two
    // candidates never set add_space at all, so it stayed green under a
    // positive control that broke the filter above it.
    run("cd /ct_probe_d");
    snprintf(detail, sizeof detail, "count %d, add_space %d",
             g_r.count, g_r.add_space);
    check("a lone directory candidate is not followed by a space",
          g_r.count == 1 && g_r.add_space == 0, detail);

    // 8. Nothing matches -> nothing offered, and nothing inserted.
    run("zzzznosuchthing");
    check("an unmatched word offers nothing",
          g_r.count == 0 && g_r.insert[0] == '\0', "");

    fixture_remove();

    return utest_end();
}
