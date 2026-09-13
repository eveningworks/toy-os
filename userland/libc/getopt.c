// POSIX getopt(), declared in <unistd.h>.
//
// WHY IT EARNS ITS PLACE: every /bin program here parses its own
// arguments by hand, and they do it differently -- some accept `-la`,
// some only `-l -a`, some stop at the first non-option and some do not.
// That is not a style difference a user can predict. One parser makes
// the shell's programs behave alike, which is the whole argument for a
// C library over a pile of helpers.
//
// POSIX behaviour, NOT glibc's: parsing STOPS at the first non-option
// argument. glibc permutes argv so options may follow operands, which
// is convenient and is also why `ls foo -l` means different things on
// different systems. Stopping is what POSIXLY_CORRECT selects on Linux
// and what BSD does unconditionally.
#include <unistd.h>
#include <stdio.h>
#include <string.h>

char *optarg;
int optind = 1;
int opterr = 1;
int optopt;

// **THE BSD EXTENSION, and dash is why it is here.** A shell parses
// options for every builtin it runs, so it needs to restart the scanner
// rather than merely rewind optind -- which cannot express "and forget
// where you were inside a cluster". Setting it to 1 before a fresh
// getopt() loop is the documented use; it clears itself.
int optreset;

// WHERE WE ARE INSIDE A CLUSTER. `-la` is two options in one argv
// element, so optind alone cannot say what is next. Reset to 0 on
// moving to a new element, which is also what makes an optind reset by
// the caller (to restart parsing) behave.
static int g_pos;

int getopt(int argc, char *const argv[], const char *optstring) {
    optarg = 0;

    if (optreset) { optreset = 0; g_pos = 0; }
    if (optind < 1) optind = 1;         // a caller restarting parsing
    if (g_pos == 0) {
        if (optind >= argc) return -1;
        const char *a = argv[optind];
        if (!a || a[0] != '-' || a[1] == '\0') return -1;  // operand, or a bare "-"
        // "--" ENDS THE OPTIONS AND IS CONSUMED, so a following
        // argument starting with '-' is an operand. Without this a file
        // legitimately named "-n" is unreachable.
        if (a[1] == '-' && a[2] == '\0') { optind++; return -1; }
        g_pos = 1;
    }

    int c = (unsigned char)argv[optind][g_pos++];
    const char *spec = c == ':' ? 0 : strchr(optstring, c);

    // Advance past this element once its last character is consumed --
    // done HERE rather than at each return below, so no path can forget
    // it and spin on the same character.
    if (argv[optind][g_pos] == '\0') { optind++; g_pos = 0; }

    if (!spec) {
        optopt = c;
        if (opterr && optstring[0] != ':') fprintf(stderr, "invalid option -- '%c'\n", c);
        return '?';
    }

    if (spec[1] != ':') return c;       // no argument wanted

    // AN ARGUMENT: the rest of this element if there is any, else the
    // next element whole. `-ofile` and `-o file` are the same thing,
    // which is the part hand-rolled parsers usually get wrong.
    if (g_pos != 0) {
        optarg = (char *)argv[optind] + g_pos;
        optind++;
        g_pos = 0;
        return c;
    }
    if (optind >= argc) {
        optopt = c;
        // A LEADING ':' IN optstring MEANS "tell me, quietly" -- the
        // caller wants ':' rather than '?' for a missing argument and
        // no message. That distinction is the only reason a program can
        // print its own usage instead of ours.
        if (optstring[0] == ':') return ':';
        if (opterr) fprintf(stderr, "option requires an argument -- '%c'\n", c);
        return '?';
    }
    optarg = (char *)argv[optind++];
    return c;
}
