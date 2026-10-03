// lib/uargs.h's parser against fixture tables: what is a flag, a value,
// a positional and an error.
//
// WHAT A BROKEN VERSION WOULD STILL PASS: a parser that took every
// "-x" word as options would pass a table without commands; the command
// table's case is bootcfg's own, "words 0 -nokaslr", where -nokaslr is
// an ARGUMENT. And a flag recorded as plain 1 would pass every "was it
// given" check while ls's "-1 -C" vs "-C -1" came out the same.
#include <string.h>
#include "lib/uargs.h"
#include "lib/utest.h"

static int f_one, f_cols, f_force, f_h;
static const char *v_file, *v_port;

static const struct uargs_opt OPTS[] = {
    { 0,       '1', 0,      "one", &f_one, 0 },
    { 0,       'C', 0,      "cols", &f_cols, 0 },
    { "force", 'f', 0,      "force", &f_force, 0 },
    { "file",  0,   "CFG",  "file", 0, &v_file },
    { "port",  'p', "PORT", "port", 0, &v_port },
    { 0 },
};
static const struct uargs_prog PLAIN = { .name = "t", .usage = "[OPTION]... [X]", .opts = OPTS };

static const struct uargs_opt HOPTS[] = { { 0, 'h', 0, "human", &f_h, 0 }, { 0 } };
static const struct uargs_prog HUMAN = { .name = "t", .usage = "[-h]", .opts = HOPTS };

static const struct uargs_cmd CMDS[] = {
    { "words", "ENTRY WORD...", "w" }, { "list", 0, "l" }, { 0 },
};
static const struct uargs_prog WITHCMDS = { .name = "t", .usage = "[CMD]", .opts = OPTS, .cmds = CMDS };

static struct uargs a;

static int run(const struct uargs_prog *p, int n, const char **words) {
    static char *argv[16];
    f_one = f_cols = f_force = f_h = 0;
    v_file = v_port = 0;
    argv[0] = "t";
    for (int i = 0; i < n; i++) argv[i + 1] = (char *)words[i];
    return uargs_parse(&a, p, n + 1, argv);
}

#define RUN(p, ...) ({ const char *w_[] = { __VA_ARGS__ }; run(p, (int)(sizeof w_ / sizeof w_[0]), w_); })

int main(void) {
    utest_begin("uargs_test", "the declared-table argument parser", 0);

    utest_check(RUN(&PLAIN, "-1", "x") == 0 && f_one && a.argc == 1 && !strcmp(a.argv[0], "x"),
                "a flag and a positional");
    utest_check(RUN(&PLAIN, "-1C") == 0 && f_one && f_cols && f_cols == f_one,
                "a bundle sets both, at the same position");
    utest_check(RUN(&PLAIN, "-1", "-C") == 0 && f_cols > f_one && RUN(&PLAIN, "-C", "-1") == 0 && f_one > f_cols,
                "a flag holds the position of its last occurrence, so the last of a pair wins");
    utest_check(RUN(&PLAIN, "--file=a.cfg") == 0 && v_file && !strcmp(v_file, "a.cfg") &&
                RUN(&PLAIN, "--file", "b.cfg") == 0 && !strcmp(v_file, "b.cfg"),
                "a long value as --name=V and --name V");
    utest_check(RUN(&PLAIN, "-p80") == 0 && !strcmp(v_port, "80") && RUN(&PLAIN, "-1p", "81") == 0 &&
                f_one && !strcmp(v_port, "81"), "a short value glued, or the next word after a bundle");
    utest_check(RUN(&PLAIN, "x", "--force") == 0 && f_force && a.argc == 1, "options after a positional (no commands)");
    utest_check(RUN(&PLAIN, "--", "-1") == 0 && !f_one && a.argc == 1 && !strcmp(a.argv[0], "-1"),
                "-- ends the options");
    utest_check(RUN(&PLAIN, "-") == 0 && a.argc == 1, "a lone - is a positional");

    utest_check(RUN(&PLAIN, "--frob") == 1 && a.status == UARGS_USAGE, "an unknown long option is a usage error");
    utest_check(RUN(&PLAIN, "-z") == 1 && a.status == UARGS_USAGE, "an unknown short option is a usage error");
    utest_check(RUN(&PLAIN, "--file") == 1 && a.status == UARGS_USAGE, "a missing value is a usage error");
    utest_check(RUN(&PLAIN, "--force=1") == 1 && a.status == UARGS_USAGE, "a value on a flag is a usage error");
    utest_check(RUN(&PLAIN, "--help") == 1 && a.status == 0, "--help stops with status 0");
    utest_check(RUN(&PLAIN, "-1h") == 1 && a.status == 0, "-h is help inside a bundle too");
    utest_check(RUN(&HUMAN, "-h") == 0 && f_h, "a program that claims -h gets it");
    utest_check(RUN(&HUMAN, "--help") == 1 && a.status == 0, "...and keeps --help");

    utest_check(RUN(&WITHCMDS, "words", "0", "-nokaslr", "+video=1") == 0 && a.argc == 4 &&
                !strcmp(a.argv[2], "-nokaslr"), "after the command a -word is an argument, not options");
    utest_check(RUN(&WITHCMDS, "-f", "words", "0", "--force") == 0 && f_force && a.argc == 2,
                "short options before the command, long ones anywhere");
    utest_check(RUN(&WITHCMDS, "wrods", "0") == 1 && a.status == UARGS_USAGE, "an unknown command is a usage error");
    utest_check(RUN(&WITHCMDS) == 0 && a.argc == 0, "no command is the program's to default");
    return utest_end();
}
