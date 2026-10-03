// wc -- count lines, words, characters and bytes.
//
// A WORD is a run of non-space bytes, as POSIX defines it. A CHARACTER
// (-m) is a UTF-8 code point -- every byte that is not a continuation
// byte -- so it differs from -c only for text outside ASCII. The columns
// are always in the order lines, words, chars, bytes, whatever order the
// flags were given in, as every wc prints them.
#include "lib/cmd.h"
#include <stdio.h>
#include <string.h>

#define USAGE "wc [-l] [-w] [-m] [-c] [<file>...]   (no flag: -l -w -c)"

struct counts { unsigned long long lines, words, chars, bytes; };

static int is_space(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

static int count(FILE *f, struct counts *k) {
    int c, in_word = 0;
    while ((c = getc(f)) != EOF) {
        k->bytes++;
        if ((c & 0xC0) != 0x80) k->chars++;
        if (c == '\n') k->lines++;
        if (is_space(c)) in_word = 0;
        else if (!in_word) { in_word = 1; k->words++; }
    }
    return ferror(f) ? 1 : 0;
}

static int g_l, g_w, g_m, g_c;

// Seven columns a count, BSD's layout, so a column of files lines up.
static void show(const struct counts *k, const char *name) {
    const char *sep = "";
    if (g_l) { printf("%s%7llu", sep, k->lines); sep = " "; }
    if (g_w) { printf("%s%7llu", sep, k->words); sep = " "; }
    if (g_m) { printf("%s%7llu", sep, k->chars); sep = " "; }
    if (g_c) { printf("%s%7llu", sep, k->bytes); }
    if (name) printf(" %s", name);
    putchar('\n');
}

int main(int argc, char **argv) {
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'l') g_l = 1;
            else if (*p == 'w') g_w = 1;
            else if (*p == 'm') g_m = 1;
            else if (*p == 'c') g_c = 1;
            else { cmd_usage(USAGE); return 1; }
        }
    }
    if (!g_l && !g_w && !g_m && !g_c) g_l = g_w = g_c = 1;

    if (i >= argc) {
        struct counts k = { 0, 0, 0, 0 };
        int r = count(stdin, &k);
        show(&k, 0);
        fflush(stdout);
        return r;
    }
    struct counts total = { 0, 0, 0, 0 };
    int failed = 0, files = argc - i;
    for (; i < argc; i++) {
        FILE *f = strcmp(argv[i], "-") ? fopen(argv[i], "r") : stdin;
        if (!f) {
            fflush(stdout);
            cmd_fail("wc", argv[i]);
            failed = 1;
            continue;
        }
        struct counts k = { 0, 0, 0, 0 };
        if (count(f, &k)) failed = 1;
        if (f != stdin) fclose(f);
        show(&k, argv[i]);
        total.lines += k.lines; total.words += k.words;
        total.chars += k.chars; total.bytes += k.bytes;
    }
    if (files > 1) show(&total, "total");
    fflush(stdout);
    return failed;
}
