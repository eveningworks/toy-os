// /bin/head, /bin/tail and /bin/wc, run as children with their stdout
// (and sometimes stdin) on pipes, against output worked out by hand for
// a fixture of the numbers 1..15 one per line (36 bytes) -- the same
// answers GNU coreutils gives for those inputs.
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "rt/sys.h"
#include "lib/utest.h"

static char g_fix[64];
static char g_out[4096];

// Runs /bin/<argv[0]> with `in` on its stdin (or none); returns its exit
// code, its stdout in g_out.
static int run(char *const *argv, const char *in) {
    char path[32];
    snprintf(path, sizeof path, "/bin/%s", argv[0]);
    int out[2], inp[2] = { -1, -1 };
    if (pipe(out) != 0) return -1000;
    if (in && pipe(inp) != 0) return -1000;
    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.argv = argv;
    o.stdout_fd = out[1];
    if (in) o.stdin_fd = inp[0];
    int pid = sys_spawn_opts(path, &o);
    close(out[1]);
    if (in) {
        close(inp[0]);
        if (pid > 0) write(inp[1], in, strlen(in));
        close(inp[1]);
    }
    size_t n = 0;
    for (;;) {
        ssize_t r = read(out[0], g_out + n, sizeof g_out - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    g_out[n] = '\0';
    close(out[0]);
    if (pid <= 0) return -1000;
    int code = -1;
    sys_waitpid(pid, &code);
    return code;
}

static void expect(char *const *argv, const char *in, int code, const char *want,
                   const char *what) {
    int got = run(argv, in);
    char line[256];
    snprintf(line, sizeof line, "%s", what);
    if (got == code && !strcmp(g_out, want)) { utest_check(1, line); return; }
    char detail[200];
    snprintf(detail, sizeof detail, "exit %d (want %d), output \"%.120s\"", got, code, g_out);
    utest_check_detail(0, line, detail);
}

#define A(...) ((char *const[]){ __VA_ARGS__, 0 })

int main(void) {
    utest_begin("textcmd_test", "head, tail and wc", UTEST_VERDICT_FILE);

    if (!tmppath(g_fix, sizeof g_fix, TMP_VOLATILE, "textcmd.txt")) {
        utest_check(0, "a scratch path");
        return utest_end();
    }
    FILE *f = fopen(g_fix, "w");
    for (int i = 1; i <= 15; i++) fprintf(f, "%d\n", i);
    fclose(f);

    expect(A("head", g_fix), 0, 0, "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n", "head: ten lines by default");
    expect(A("head", "-n", "3", g_fix), 0, 0, "1\n2\n3\n", "head -n 3");
    expect(A("head", "-3", g_fix), 0, 0, "1\n2\n3\n", "head -3, the old spelling");
    expect(A("head", "-c", "5", g_fix), 0, 0, "1\n2\n3", "head -c 5");
    expect(A("head", "-n", "2"), "a\nb\nc\n", 0, "a\nb\n", "head from stdin");
    {
        char want[256];
        snprintf(want, sizeof want, "==> %s <==\n1\n\n==> %s <==\n1\n", g_fix, g_fix);
        expect(A("head", "-n", "1", g_fix, g_fix), 0, 0, want, "head over two files heads each");
    }
    expect(A("head", "-n", "x", g_fix), 0, 1, "usage: head [-n <lines> | -<lines> | -c <bytes>] [<file>...]\n",
           "head -n x is refused");

    expect(A("tail", g_fix), 0, 0, "6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n", "tail: ten lines by default");
    expect(A("tail", "-n", "2", g_fix), 0, 0, "14\n15\n", "tail -n 2");
    expect(A("tail", "-n", "+14", g_fix), 0, 0, "14\n15\n", "tail -n +14 starts at line 14");
    expect(A("tail", "-c", "3", g_fix), 0, 0, "15\n", "tail -c 3");
    expect(A("tail", "-n", "1"), "a\nb\nc", 0, "c", "tail from stdin, last line unterminated");
    expect(A("tail", "-n", "5"), "a\nb\n", 0, "a\nb\n", "tail -n 5 of two lines is both");

    char want[128];
    snprintf(want, sizeof want, "     15      15      36 %s\n", g_fix);
    expect(A("wc", g_fix), 0, 0, want, "wc: lines, words, bytes");
    expect(A("wc", "-l"), "a b\nc\n", 0, "      2\n", "wc -l from stdin");
    expect(A("wc", "-w"), "  a  b\tc\n\n d", 0, "      4\n", "wc -w counts runs of non-space");
    expect(A("wc", "-c", "-m"), "\xc3\xa4\xc3\xb6\n", 0, "      3       5\n",
           "wc -m counts UTF-8 code points, -c bytes, in that column order");
    snprintf(want, sizeof want, "     15 %s\n     15 %s\n     30 total\n", g_fix, g_fix);
    expect(A("wc", "-l", g_fix, g_fix), 0, 0, want, "wc over two files adds a total");
    int code = run(A("wc", "/no/such/file"), 0);
    utest_checkf(code == 1 && !strncmp(g_out, "wc: /no/such/file: ", 19),
                 "wc of a missing file names it and exits 1");

    unlink(g_fix);
    return utest_end();
}
