// lib/ulaunch.h: what a file is to a double-click -- programs, an app,
// a library, scripts with and without their interpreter or an execute
// bit, a document -- then the argv per act, the execute bit set, and the
// policy's round trip through /etc/mimeapps.conf.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "lib/utest.h"
#include "lib/ulaunch.h"
#include "lib/uconf.h"
#include "lib/uopen.h"
#include "rt/sys.h"

#define DIR "/home/ulaunch_test"

// A check whose detail -- what was got -- shows only when it fails.
__attribute__((format(printf, 3, 4)))
static void chk(int ok, const char *what, const char *fmt, ...) {
    char detail[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    utest_check_detail(ok, what, detail);
}

static void put(const char *path, const char *text, unsigned mode) {
    FILE *f = fopen(path, "w");
    if (f) { fputs(text, f); fclose(f); }
    sys_chmod(path, mode);
}

static void kind_of(const char *path, int want, const char *what) {
    struct ulaunch_info li;
    int ok = ulaunch_classify(path, &li);
    chk(ok && li.kind == want, what, "%s: ok %d kind %d, want %d", path, ok, li.kind, want);
}

int main(void) {
    utest_begin("ulaunch_test", "what a double-click runs", UTEST_VERDICT_FILE);
    sys_mkdir(DIR);

    struct ulaunch_info li;
    kind_of("/bin/uptime", ULAUNCH_PROGRAM, "an ELF program is a PROGRAM");
    ulaunch_classify("/bin/uptime", &li);
    utest_check(li.runnable && li.interp_found, "...runnable, with nothing missing");
    kind_of("/bin/wm/apps/calculator", ULAUNCH_APP, "a program a desktop entry runs is an APP");
    ulaunch_classify("/bin/wm/apps/calculator", &li);
    chk(!strcmp(li.app, "Calculator"), "...named by its entry", "app \"%s\"", li.app);
    kind_of("/lib/libuapp.so", ULAUNCH_NONE, "a shared library is not a program");

    put(DIR "/s.sh", "#!/bin/dash -e\n# second\tline\necho third\n", 0644);
    kind_of(DIR "/s.sh", ULAUNCH_SCRIPT, "#! makes a SCRIPT");
    ulaunch_classify(DIR "/s.sh", &li);
    chk(!strcmp(li.interp, "/bin/dash") && li.interp_found, "its interpreter is the first word, and exists",
                 "interp \"%s\" found %d", li.interp, li.interp_found);
    utest_check(!li.runnable, "0644 is not runnable");
    chk(li.head_lines == 3 && !strcmp(li.head[0], "#!/bin/dash -e") && !strcmp(li.head[1], "# second line"),
                 "its first lines, a tab as a space", "%d lines: \"%s\" \"%s\"", li.head_lines, li.head[0], li.head[1]);

    put(DIR "/p.py", "#!/usr/bin/nope\nprint(1)\n", 0755);
    ulaunch_classify(DIR "/p.py", &li);
    utest_check(li.kind == ULAUNCH_SCRIPT && !li.interp_found && li.runnable,
                "a script whose interpreter is missing says so");
    put(DIR "/plain.txt", "hello\n", 0755);
    kind_of(DIR "/plain.txt", ULAUNCH_NONE, "an executable text file without #! is a document");
    int ok = ulaunch_classify(DIR, &li);
    chk(!ok && li.kind == ULAUNCH_NONE, "a folder is not run (and not classified)", "ok %d kind %d", ok, li.kind);

    utest_check(ulaunch_allow(DIR "/s.sh"), "allowing it succeeds");
    struct sys_stat st;
    sys_stat(DIR "/s.sh", &st);
    chk((st.mode & 0777) == 0755, "...and gives x where there is r", "mode %04o", st.mode & 0777);

    char *argv[5];
    utest_check(ulaunch_argv(DIR "/s.sh", ULAUNCH_SCRIPT, ULAUNCH_TERMINAL, argv) &&
                !strcmp(argv[0], ULAUNCH_TERMINAL_EXEC) && !strcmp(argv[1], "-e") &&
                !strcmp(argv[2], DIR "/s.sh") && !argv[3], "in a Terminal: uterm -e PATH");
    utest_check(ulaunch_argv(DIR "/s.sh", ULAUNCH_SCRIPT, ULAUNCH_RUN, argv) &&
                !strcmp(argv[0], DIR "/s.sh") && !argv[1], "run: the file itself");
    utest_check(ulaunch_argv(DIR "/s.sh", ULAUNCH_SCRIPT, ULAUNCH_EDIT, argv) &&
                !strcmp(argv[0], ULAUNCH_EDIT_EXEC), "a script edits in Notepad");
    utest_check(!ulaunch_argv("/bin/uptime", ULAUNCH_PROGRAM, ULAUNCH_EDIT, argv), "a program does not");
    utest_check(!ulaunch_argv("/bin/uptime", ULAUNCH_PROGRAM, ULAUNCH_ASK, argv), "ASK is never an argv");

    // The policy, kept around whatever the machine had.
    char keep_s[16] = "", keep_p[16] = "";
    int had_s = uconf_get(UOPEN_CONF, ULAUNCH_MIME_SCRIPT, keep_s, sizeof keep_s);
    int had_p = uconf_get(UOPEN_CONF, ULAUNCH_MIME_PROGRAM, keep_p, sizeof keep_p);
    utest_check(ulaunch_policy(ULAUNCH_APP) == ULAUNCH_RUN, "an app is never asked about");
    ulaunch_set_policy(ULAUNCH_SCRIPT, ULAUNCH_EDIT);
    utest_check(ulaunch_policy(ULAUNCH_SCRIPT) == ULAUNCH_EDIT, "a script's choice is kept");
    ulaunch_set_policy(ULAUNCH_PROGRAM, ULAUNCH_EDIT);
    utest_check(ulaunch_policy(ULAUNCH_PROGRAM) == ULAUNCH_ASK, "'edit' for a program reads as ask");
    ulaunch_set_policy(ULAUNCH_SCRIPT, ULAUNCH_ASK);
    char v[16];
    utest_check(ulaunch_policy(ULAUNCH_SCRIPT) == ULAUNCH_ASK &&
                !uconf_get(UOPEN_CONF, ULAUNCH_MIME_SCRIPT, v, sizeof v), "ask forgets the key");
    if (had_s) uconf_set(UOPEN_CONF, ULAUNCH_MIME_SCRIPT, keep_s);
    if (had_p) uconf_set(UOPEN_CONF, ULAUNCH_MIME_PROGRAM, keep_p);
    else uconf_unset(UOPEN_CONF, ULAUNCH_MIME_PROGRAM);

    sys_unlink(DIR "/s.sh");
    sys_unlink(DIR "/p.py");
    sys_unlink(DIR "/plain.txt");
    sys_unlink(DIR);
    return utest_end();
}
