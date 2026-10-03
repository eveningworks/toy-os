// head -- the first lines (or bytes) of files, or of stdin.
//
// IT STOPS READING AT THE COUNT, which is the point of `dmesg | head`:
// once it exits, the writer's next write fails and it dies of SIGPIPE
// quietly, instead of the whole input being read to throw it away.
#include "lib/cmd.h"
#include <stdio.h>
#include <string.h>

#define USAGE "head [-n <lines> | -<lines> | -c <bytes>] [<file>...]"

static int head_stream(FILE *f, unsigned long long count, int bytes) {
    int c;
    while (count > 0 && (c = getc(f)) != EOF) {
        putchar(c);
        if (bytes || c == '\n') count--;
    }
    return ferror(f) ? 1 : 0;
}

int main(int argc, char **argv) {
    unsigned long long count = 10;
    int bytes = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) { i++; break; }
        if ((!strcmp(a, "-n") || !strcmp(a, "-c")) && i + 1 < argc &&
            cmd_parse_count(argv[i + 1], &count)) {
            bytes = a[1] == 'c';
            i++;
        } else if (cmd_parse_count(a + 1, &count)) {
            bytes = 0;                       // `head -40`, the old spelling
        } else {
            cmd_usage(USAGE);
            return 1;
        }
    }

    if (i >= argc) {
        int r = head_stream(stdin, count, bytes);
        fflush(stdout);
        return r;
    }
    int failed = 0, many = argc - i > 1;
    for (int first = i; i < argc; i++) {
        FILE *f = strcmp(argv[i], "-") ? fopen(argv[i], "r") : stdin;
        if (!f) {
            fflush(stdout);
            cmd_fail("head", argv[i]);
            failed = 1;
            continue;
        }
        if (many) printf("%s==> %s <==\n", i > first ? "\n" : "", argv[i]);
        if (head_stream(f, count, bytes)) failed = 1;
        if (f != stdin) fclose(f);
    }
    fflush(stdout);
    return failed;
}
