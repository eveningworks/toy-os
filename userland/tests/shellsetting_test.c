// `system.shell` is HONOURED, not merely stored.
//
// **THE SHAPE MATTERS HERE.** A test that set the value and read it back
// would pass while every consumer still spawned /bin/tosh, which is
// exactly the bug worth catching. So this points the setting at a
// SCRIPT that leaves a marker file and then calls system(): the marker
// can only appear if system() spawned what the setting named. A build
// with the old hardcoded path produces no marker and fails.
//
// The script is a `#!` file, which is also why this could not have been
// written before the loader learned about them.
//
// It puts the setting back before returning, including on every failure
// path -- a test that leaves `system.shell` pointing at /var/tmp
// changes the shell every later tool gets, which is the hazard
// CLAUDE.md records for settings_test's mouse values.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>   // chmod -- the script must be executable
#include "rt/sys.h"
#include "lib/usetting.h"
#include "lib/utest.h"

#define FAKESH "/var/tmp/shellsetting_sh"
#define MARKER "/var/tmp/shellsetting_ran"

static int write_script(void) {
    FILE *f = fopen(FAKESH, "w");
    if (!f) return 0;
    // Writes the marker whatever it is asked to run, then exits. It
    // never has to behave like a shell: what is under test is WHICH
    // binary system() reached.
    fputs("#!/bin/dash\n", f);
    fputs("echo ran > " MARKER "\n", f);
    fclose(f);
    return chmod(FAKESH, 0755) == 0;
}

int main(void) {
    utest_begin("shellsetting_test", "system.shell is honoured, not just stored",
                UTEST_VERDICT_FILE);

    char was[128];
    int have = usetting_get("system.shell", was, sizeof was) && was[0];
    utest_check(have, "the setting exists");
    if (!have) return utest_end();
    utest_check(was[0] == '/', "and its default is a path");

    if (access("/bin/dash", F_OK) != 0) {
        utest_notef("note: no /bin/dash -- nothing to point the setting at");
        return utest_end();
    }
    int wrote = write_script();
    utest_check(wrote, "a `#!` script to stand in for a shell");
    if (!wrote) return utest_end();

    int rc = usetting_set("system.shell", FAKESH);
    int took = (rc == SETTING_SAVED || rc == SETTING_UNSAVED);
    utest_check(took, "the setting accepts it");
    if (!took) {
        usetting_set("system.shell", was);
        unlink(FAKESH);
        return utest_end();
    }

    unlink(MARKER);
    system("true");

    // **POLLED, NOT CHECKED ONCE.** system() waits for its child, but
    // `usertest_run.py` starts a test through the LEGACY `run` loader,
    // which has no scheduler slot of its own -- so the wait is not the
    // same wait, and a single check raced it. Waiting on the artifact
    // is right either way.
    int honoured = 0;
    for (int i = 0; i < 100 && !honoured; i++) {
        honoured = access(MARKER, F_OK) == 0;
        if (!honoured) usleep(20000);
    }

    // ALWAYS, and before the verdict: a test that leaves system.shell
    // pointing at /var/tmp changes the shell every later tool gets.
    usetting_set("system.shell", was);
    unlink(MARKER);
    unlink(FAKESH);

    // THE ONE THAT MATTERS. A build still hardcoding /bin/tosh gets no
    // marker: only the script the setting names writes one.
    utest_check(honoured, "system() spawned what system.shell named");

    char back[128];
    utest_check(usetting_get("system.shell", back, sizeof back) &&
                strcmp(back, was) == 0, "and the setting was put back");
    return utest_end();
}
