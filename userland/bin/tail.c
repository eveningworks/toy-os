// tail -- the last lines (or bytes) of files, or of stdin; `-n +N`
// starts at line N instead.
//
// ONE PASS, NO SEEKING: the last N lines are kept in a ring as they go
// by, so a pipe works exactly as a file does. The cost is reading all
// of a large file to print its end; there is no `-f` either -- `log -f`
// and `dmesg -w` follow the logs that are worth following.
#include "lib/cmd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define USAGE "tail [-n <lines> | -n +<from> | -<lines> | -c <bytes>] [<file>...]"

// Past this many lines a count is refused rather than allocated for.
#define TAIL_LINES_MAX 100000

static int tail_from(FILE *f, unsigned long long from) {
    unsigned long long line = 1;
    int c;
    while ((c = getc(f)) != EOF) {
        if (line >= from) putchar(c);
        if (c == '\n') line++;
    }
    return ferror(f) ? 1 : 0;
}

static int tail_bytes(FILE *f, unsigned long long n) {
    if (n == 0) { while (getc(f) != EOF) {} return ferror(f) ? 1 : 0; }
    unsigned char *ring = malloc((size_t)n);
    if (!ring) { cmd_fail_msg("tail", 0, "out of memory"); return 1; }
    unsigned long long seen = 0;
    int c;
    while ((c = getc(f)) != EOF) ring[seen++ % n] = (unsigned char)c;
    unsigned long long keep = seen < n ? seen : n;
    for (unsigned long long k = seen - keep; k < seen; k++) putchar(ring[k % n]);
    free(ring);
    return ferror(f) ? 1 : 0;
}

static int tail_lines(FILE *f, unsigned long long n) {
    if (n == 0) { while (getc(f) != EOF) {} return ferror(f) ? 1 : 0; }
    char **ring = calloc((size_t)n, sizeof *ring);
    if (!ring) { cmd_fail_msg("tail", 0, "out of memory"); return 1; }
    unsigned long long seen = 0;
    char *line = 0;
    size_t cap = 0;
    while (getline(&line, &cap, f) >= 0) {
        free(ring[seen % n]);
        ring[seen % n] = line;   // kept: getline() allocates the next one
        line = 0;
        cap = 0;
        seen++;
    }
    free(line);
    unsigned long long keep = seen < n ? seen : n;
    for (unsigned long long k = seen - keep; k < seen; k++) fputs(ring[k % n], stdout);
    for (unsigned long long k = 0; k < n; k++) free(ring[k]);
    free(ring);
    return ferror(f) ? 1 : 0;
}

int main(int argc, char **argv) {
    unsigned long long count = 10;
    int bytes = 0, from = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) { i++; break; }
        if ((!strcmp(a, "-n") || !strcmp(a, "-c")) && i + 1 < argc) {
            const char *v = argv[++i];
            bytes = a[1] == 'c';
            from = !bytes && v[0] == '+';
            if (!cmd_parse_count(from ? v + 1 : v, &count)) { cmd_usage(USAGE); return 1; }
        } else if (cmd_parse_count(a + 1, &count)) {
            bytes = from = 0;                // `tail -40`, the old spelling
        } else {
            cmd_usage(USAGE);
            return 1;
        }
    }
    if (!bytes && !from && count > TAIL_LINES_MAX) {
        cmd_fail_msg("tail", 0, "more lines than it will keep (100000)");
        return 1;
    }

    static const char *const just_stdin[] = { "-" };
    const char *const *names = i < argc ? (const char *const *)argv + i : just_stdin;
    int n = i < argc ? argc - i : 1, failed = 0, many = n > 1;
    for (i = 0; i < n; i++) {
        FILE *f = strcmp(names[i], "-") ? fopen(names[i], "r") : stdin;
        if (!f) {
            fflush(stdout);
            cmd_fail("tail", names[i]);
            failed = 1;
            continue;
        }
        if (many) printf("%s==> %s <==\n", i ? "\n" : "", names[i]);
        int r = from ? tail_from(f, count) : bytes ? tail_bytes(f, count)
                                                   : tail_lines(f, count);
        if (r) failed = 1;
        if (f != stdin) fclose(f);
    }
    fflush(stdout);
    return failed;
}
