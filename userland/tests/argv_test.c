// The argument vector across a spawn, and the shell's quoting on top of
// it. Two programs in one binary, told apart by argc:
//
//   argv_test <args...>   prints `argc=N` and then each argument in
//                         brackets, one per line -- the probe. Brackets
//                         because `a b` as one argument and as two look
//                         identical joined with spaces.
//   argv_test             the self-check: spawns the probe four ways
//                         through a pipe and reads back what arrived.
//
// WHAT EACH SPAWN DISCRIMINATES. The string form must still split on
// spaces, or every existing caller changed behaviour, and it must
// HONOUR QUOTES -- it is what the ring-0 shell spawns through, and that
// shell has no lexer of its own. The vector form must carry a space and
// an empty argument without any quoting at all. And /bin/tosh must turn
// quotes into one argument, which proves the lexer AND that it spawns
// with the vector -- a lexer over the string form would pass a shell
// test that only looked at the parse. The two splitters answer the SAME
// cases here on purpose (kernel/proc/elf_run.c says so too): they are
// separate implementations of one set of rules.
// The last spawn is the control the others need: a quoted operator
// that must NOT be an operator, so a lexer that unquoted after
// splitting would fail it.
//
// SPAWNED, not `run`: it blocks reading the pipe its children write.
#include <stdio.h>
#include <string.h>
#include "rt/sys.h"
#include "lib/utest.h"

#define PROBE "/tests/argv_test"

// Runs one spawn with the child's stdout on a pipe and collects it.
// Returns the byte count, or -1 if the spawn was refused.
static int collect(const char *path, const char *args, char *const *argv,
                   char *out, size_t cap) {
    int p[2];
    if (sys_pipe(p) != 0) return -1;
    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.args = args;
    o.argv = argv;
    o.env = environ;
    o.stdout_fd = p[1];
    int pid = sys_spawn_opts(path, &o);
    sys_close(p[1]);   // the parent's copy, so the child's is the last writer
    if (pid < 0) { sys_close(p[0]); return -1; }
    size_t n = 0;
    for (;;) {
        long r = sys_read(p[0], out + n, cap - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
        if (n >= cap - 1) break;
    }
    out[n] = '\0';
    sys_close(p[0]);
    sys_waitpid(pid, 0);
    return (int)n;
}

static void probe(int argc, char **argv) {
    printf("argc=%d\n", argc);
    for (int i = 1; i < argc; i++) printf("[%s]\n", argv[i]);
    fflush(stdout);
}

int main(int argc, char **argv) {
    if (argc > 1) { probe(argc, argv); return 0; }

    utest_begin("argv_test", "an argv vector across a spawn, and quoting in tosh",
                UTEST_VERDICT_FILE);
    static char out[512];

    // 1. The string form still splits on spaces.
    int n = collect(PROBE, "one two", 0, out, sizeof out);
    utest_check(n > 0, "string-form spawn ran");
    utest_check(strcmp(out, "argc=3\n[one]\n[two]\n") == 0,
                "the string form splits into two arguments");

    // 1b. AND IT HONOURS QUOTES. This is the `#` prompt's quoting:
    //     `tosh -c 'echo hi'` typed there used to arrive as two words,
    //     rejoined into one, and looked up as a program name.
    n = collect(PROBE, "'a b' \"c d\" e\\ f \"\" plain", 0, out, sizeof out);
    utest_checkf(strcmp(out, "argc=6\n[a b]\n[c d]\n[e f]\n[]\n[plain]\n") == 0,
                 "the string form: quotes and a backslash each make one argument (got %s)", out);

    // 1c. An unterminated quote is REFUSED at the edge -- the spawn
    //     fails rather than running a word nobody typed.
    n = collect(PROBE, "'oops", 0, out, sizeof out);
    utest_check(n < 0, "the string form: an unterminated quote is refused");

    // 2. The vector form carries a space and an empty argument.
    char *const vec[] = { "argv_test", "a b", "", "c", 0 };
    n = collect(PROBE, 0, vec, out, sizeof out);
    utest_check(n > 0, "vector-form spawn ran");
    utest_checkf(strcmp(out, "argc=4\n[a b]\n[]\n[c]\n") == 0,
                 "the vector arrives whole -- a space and an empty entry survive (got %s)", out);

    // 3. The empty vector is refused rather than entering main with argc 0.
    char *const none[] = { 0 };
    n = collect(PROBE, 0, none, out, sizeof out);
    utest_check(n < 0, "an empty vector is refused");

    // 4. /bin/tosh: quotes make one argument, and the shell spawns with
    //    the vector. `-c` gets the WHOLE line as one argument, which is
    //    itself the vector working.
    char *const sh1[] = { "tosh", "-c",
                          "argv_test \"a b\" 'c  d' e\\ f \"\" plain", 0 };
    n = collect("/bin/tosh", 0, sh1, out, sizeof out);
    utest_checkf(strcmp(out, "argc=6\n[a b]\n[c  d]\n[e f]\n[]\n[plain]\n") == 0,
                 "tosh: double, single and backslash quoting each make one argument (got %s)", out);

    // 5. A quoted operator is a word, and an unquoted one needs no spaces.
    char *const sh2[] = { "tosh", "-c", "argv_test 'a|b' \"c>d\" x>/tmp/argv_probe.txt", 0 };
    n = collect("/bin/tosh", 0, sh2, out, sizeof out);
    utest_check(n == 0, "tosh: `>` glued to a word still redirects (nothing on the pipe)");
    int fd = sys_open("/tmp/argv_probe.txt", 0);
    n = fd >= 0 ? (int)sys_read(fd, out, sizeof out - 1) : -1;
    if (n >= 0) out[n] = '\0';
    if (fd >= 0) sys_close(fd);
    utest_checkf(n > 0 && strcmp(out, "argc=4\n[a|b]\n[c>d]\n[x]\n") == 0,
                 "tosh: quoted `|` and `>` are words, not operators (got %s)", n > 0 ? out : "nothing");

    // 5b. A redirection in the MIDDLE of a command keeps the words after
    //     it -- a stage tracked as a word count lost them.
    char *const sh2b[] = { "tosh", "-c", "argv_test a > /tmp/argv_probe.txt b", 0 };
    n = collect("/bin/tosh", 0, sh2b, out, sizeof out);
    fd = sys_open("/tmp/argv_probe.txt", 0);
    n = fd >= 0 ? (int)sys_read(fd, out, sizeof out - 1) : -1;
    if (n >= 0) out[n] = '\0';
    if (fd >= 0) sys_close(fd);
    utest_checkf(n > 0 && strcmp(out, "argc=3\n[a]\n[b]\n") == 0,
                 "tosh: a redirection mid-command keeps the arguments after it (got %s)", n > 0 ? out : "nothing");

    // 6. An unterminated quote runs nothing -- the shell's diagnostic
    //    lands on the pipe (tosh reports through its stdout), so the
    //    check is that no probe output does.
    char *const sh3[] = { "tosh", "-c", "argv_test 'oops", 0 };
    n = collect("/bin/tosh", 0, sh3, out, sizeof out);
    utest_checkf(n > 0 && !strstr(out, "argc="),
                 "tosh: an unterminated quote runs no command (got %s)", out);

    return utest_end();
}
