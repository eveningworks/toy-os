// echo -- print the arguments, separated by single spaces.
//
// TWO THINGS IT DELIBERATELY DOES NOT DO.
//
// It does not interpret backslash escapes. POSIX leaves that
// implementation-defined and the split between `echo -e`, bash's
// builtin and /bin/echo is a well-known mess; a parser that guesses is
// worse than one that does not exist (CLAUDE.md: a parser REJECTS
// rather than guesses). `-n` is honoured because suppressing the
// newline has no ambiguity and a script genuinely needs it.
//
// It does not re-split or collapse whitespace. argv arrives already
// split by whoever ran it, so `echo  a   b` prints "a b" -- the same
// answer every Unix echo gives, since the shell did the splitting.
#include "rt/sys.h"

int main(int argc, char **argv) {
    int first = 1;
    int newline = 1;

    // Only a leading -n, and only exactly "-n": a later argument that
    // happens to be "-n" is text, which is what a caller printing a
    // list of flags means by it.
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'n' && argv[1][2] == '\0') {
        newline = 0;
        first = 2;
    }

    for (int i = first; i < argc; i++) {
        if (i > first) sys_write(1, " ", 1);
        sys_print(argv[i]);
    }
    if (newline) sys_write(1, "\n", 1);
    return 0;
}
