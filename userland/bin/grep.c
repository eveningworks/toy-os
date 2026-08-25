// grep -- print lines matching a pattern.
//
// A thin front end over tolibc's <regex.h> (userland/libc/regex.c).
// Everything interesting about the matching is there; what is here is
// line handling, the flags, and the decision below about which dialect
// to speak.
//
// **IT SPEAKS ERE, NOT BRE, AND THAT IS A DELIBERATE DIVERGENCE FROM
// POSIX.** POSIX grep is BRE: `+ ? |` are literals and grouping is
// `\( \)`. Those rules exist because grep predates the extended syntax
// and could not break the scripts already written against it -- decades
// of compatibility baggage this OS has no reason to inherit
// (CLAUDE.md's "copy the SHAPE, not the size"). There is no `-E`,
// because there is nothing to switch to. If a POSIX script ever needs
// to run here, `regcomp()` already implements BRE and adding the flag
// is a line.
//
// Reads stdin when given no files, the same shape /bin/cat uses, which
// is what makes `dmesg | grep partition` work.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>
#include <unistd.h>
#include "rt/sys.h"
#include "lib/cmd.h"

// A line longer than this is matched in full but reported truncated.
// Sized against the widest thing this OS produces (a `dmesg` line, a
// long path listing) with room over, and it is a stack buffer in main()
// rather than per-call because the ring-3 frame budget is 2 KiB.
#define LINE_MAX 1024

static const char *USAGE =
    "grep [-i] [-n] [-v] [-c] <pattern> [file ...]\n"
    "  -i  ignore case\n"
    "  -n  print the line number before each line\n"
    "  -v  print the lines that do NOT match\n"
    "  -c  print only a count of matching lines\n"
    "  with no file, reads standard input";

static regex_t g_re;
static int opt_n, opt_v, opt_c;
static long g_total;      // -c across every file, printed per file
static int g_any;         // did anything match anywhere (exit status)

// One file, already open. `name` is NULL for stdin, which is what
// decides whether a "name:" prefix is printed.
static void grep_fd(int fd, const char *name, int show_name) {
    char line[LINE_MAX];
    char buf[512];
    size_t len = 0;
    long lineno = 0, count = 0;
    int truncated = 0;

    for (;;) {
        long n = sys_read(fd, buf, sizeof buf);
        // A SHORT READ IS NOT EOF -- it is how a pipe delivers whatever
        // is ready, and treating it as the end would silently drop the
        // rest of `dmesg | grep`. Only 0 ends the stream.
        if (n <= 0) break;

        for (long i = 0; i < n; i++) {
            char c = buf[i];
            if (c != '\n') {
                if (len < sizeof line - 1) line[len++] = c;
                else truncated = 1;   // matched in full is impossible; say so
                continue;
            }
            line[len] = '\0';
            lineno++;
            int hit = regexec(&g_re, line, 0, NULL, 0) == 0;
            if (opt_v) hit = !hit;
            if (hit) {
                count++;
                g_any = 1;
                if (!opt_c) {
                    if (show_name) printf("%s:", name);
                    if (opt_n) printf("%ld:", lineno);
                    printf("%s%s\n", line, truncated ? " [truncated]" : "");
                }
            }
            len = 0;
            truncated = 0;
        }
    }

    // A final line with no trailing newline still counts. Forgetting
    // this drops the last line of any file that does not end in one,
    // which is most files written by an editor that does not add it.
    if (len > 0) {
        line[len] = '\0';
        lineno++;
        int hit = regexec(&g_re, line, 0, NULL, 0) == 0;
        if (opt_v) hit = !hit;
        if (hit) {
            count++;
            g_any = 1;
            if (!opt_c) {
                if (show_name) printf("%s:", name);
                if (opt_n) printf("%ld:", lineno);
                printf("%s\n", line);
            }
        }
    }

    if (opt_c) {
        if (show_name) printf("%s:", name);
        printf("%ld\n", count);
    }
    g_total += count;
}

int main(int argc, char **argv) {
    int opt_i = 0;
    int argi = 1;

    // Hand-rolled rather than getopt(), for one reason: a pattern may
    // legitimately begin with '-' ("-v" as a literal string), and the
    // `--` terminator is how that is said. getopt() would work too;
    // this keeps the argument walk visible beside the `--` handling.
    for (; argi < argc; argi++) {
        const char *a = argv[argi];
        if (a[0] != '-' || a[1] == '\0') break;
        if (strcmp(a, "--") == 0) { argi++; break; }
        for (const char *f = a + 1; *f; f++) {
            switch (*f) {
            case 'i': opt_i = 1; break;
            case 'n': opt_n = 1; break;
            case 'v': opt_v = 1; break;
            case 'c': opt_c = 1; break;
            default:
                cmd_usage(USAGE);
                return 2;
            }
        }
    }

    if (argi >= argc) {
        cmd_usage(USAGE);
        return 2;
    }
    const char *pattern = argv[argi++];

    int rc = regcomp(&g_re, pattern, REG_EXTENDED | (opt_i ? REG_ICASE : 0));
    if (rc != 0) {
        // The REASON, not just "bad pattern". regcomp() distinguishes
        // an unmatched bracket from a bad repetition count, and a user
        // staring at their own quoting needs to know which.
        char eb[96];
        regerror(rc, &g_re, eb, sizeof eb);
        fprintf(stderr, "grep: %s: %s\n", pattern, eb);
        return 2;
    }

    // Exit status is grep's real interface in a script: 0 if anything
    // matched, 1 if nothing did, 2 on an error. That is why a missing
    // file is 2 and an empty result is 1, and why they are not the same.
    int had_error = 0;
    if (argi >= argc) {
        grep_fd(0, NULL, 0);
    } else {
        int show_name = (argc - argi) > 1;
        for (; argi < argc; argi++) {
            int fd = sys_open(argv[argi], 0);
            if (fd < 0) {
                cmd_fail("grep", argv[argi]);
                had_error = 1;
                continue;
            }
            grep_fd(fd, argv[argi], show_name);
            sys_close(fd);
        }
    }

    regfree(&g_re);
    if (had_error) return 2;
    return g_any ? 0 : 1;
}
