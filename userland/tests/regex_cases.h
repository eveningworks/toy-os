#ifndef REGEX_CASES_H
#define REGEX_CASES_H

// The regex case table, shared by the ring-3 test (/tests/regex_test)
// and by a HOST harness that compiles userland/libc/regex.c directly.
// Same split kfmt_cases.h and klineedit_cases.h use, for the same
// reason: a case added once is asserted everywhere.
//
// The host build is what makes iterating on the engine bearable -- a
// full `make iso` plus a boot per edit would be minutes each. The
// ring-3 run is what proves the same source behaves the same way
// compiled for a different target with a different libc under it,
// which is exactly the gap kfmt_cases.h exists to close.
//
// WHAT TO PUT HERE. Not "a handful of interesting patterns" -- every
// construct the header claims, plus the cases where two plausible
// implementations disagree: leftmost-LONGEST vs leftmost-first, greedy
// vs lazy, an empty match, an anchor that cannot be satisfied, a class
// with ']' or '-' in an awkward place. A regex engine that is right on
// `a*b` and wrong on `[]-]` fails in the field, not in a demo.

struct rx_case {
    const char *pattern;
    const char *input;
    int         icase;     // REG_ICASE
    int         expect;    // 1 = match, 0 = no match
    int         so, eo;    // expected pmatch[0]; -1/-1 to skip checking
};

static const struct rx_case RX_CASES[] = {
    // --- literals -----------------------------------------------
    { "abc",          "abc",           0, 1,  0, 3 },
    { "abc",          "xxabcxx",       0, 1,  2, 5 },
    { "abc",          "ab",            0, 0, -1, -1 },
    { "",             "anything",      0, 1,  0, 0 },  // empty pattern matches empty at 0

    // --- . ------------------------------------------------------
    { "a.c",          "abc",           0, 1,  0, 3 },
    { "a.c",          "a c",           0, 1,  0, 3 },
    { "a.c",          "ac",            0, 0, -1, -1 },

    // --- * + ? --------------------------------------------------
    { "ab*c",         "ac",            0, 1,  0, 2 },
    { "ab*c",         "abbbc",         0, 1,  0, 5 },
    { "ab+c",         "ac",            0, 0, -1, -1 },
    { "ab+c",         "abbbc",         0, 1,  0, 5 },
    { "ab?c",         "ac",            0, 1,  0, 2 },
    { "ab?c",         "abc",           0, 1,  0, 3 },
    { "ab?c",         "abbc",          0, 0, -1, -1 },

    // GREEDINESS. `a*` must take all four, not stop early -- the
    // difference between leftmost-longest and a first-match engine.
    { "a*",           "aaaa",          0, 1,  0, 4 },
    { "a*",           "bbbb",          0, 1,  0, 0 },  // empty match at 0

    // --- anchors ------------------------------------------------
    { "^abc",         "abcdef",        0, 1,  0, 3 },
    { "^abc",         "xabcdef",       0, 0, -1, -1 },
    { "abc$",         "xxabc",         0, 1,  2, 5 },
    { "abc$",         "abcx",          0, 0, -1, -1 },
    { "^abc$",        "abc",           0, 1,  0, 3 },
    { "^abc$",        "abcd",          0, 0, -1, -1 },
    { "^$",           "",              0, 1,  0, 0 },

    // --- alternation --------------------------------------------
    { "cat|dog",      "hotdog",        0, 1,  3, 6 },
    { "cat|dog",      "bird",          0, 0, -1, -1 },
    { "^(ata|virtio)","virtio-blk",    0, 1,  0, 6 },
    { "a(b|c)d",      "acd",           0, 1,  0, 3 },
    { "a(b|c)d",      "aed",           0, 0, -1, -1 },

    // LEFTMOST-LONGEST, the case that separates POSIX from Perl: with
    // leftmost-first `a|ab` would stop at "a".
    { "a|ab",         "ab",            0, 1,  0, 2 },

    // --- groups + repetition ------------------------------------
    { "(ab)*",        "ababab",        0, 1,  0, 6 },
    { "(ab)+c",       "ababc",         0, 1,  0, 5 },
    { "(a|b)+",       "abba",          0, 1,  0, 4 },

    // The classic catastrophic-backtracking pattern. A backtracking
    // engine takes exponential time here; this must simply answer.
    { "(a*)*b",       "aaaaaaaaaaaaaaaaaaaaaaaaac", 0, 0, -1, -1 },

    // --- classes ------------------------------------------------
    { "[abc]",        "xbx",           0, 1,  1, 2 },
    { "[a-z]+",       "..hello..",     0, 1,  2, 7 },
    { "[^a-z]+",      "abc123def",     0, 1,  3, 6 },
    { "[0-9]+",       "part 42 here",  0, 1,  5, 7 },
    { "partition [0-9]", "partition 1 active", 0, 1, 0, 11 },

    // ']' first is a LITERAL; '-' last is a LITERAL. Two corners that
    // a naive scanner gets wrong in opposite directions.
    { "[]]",          "]",             0, 1,  0, 1 },
    { "[a-]",         "-",             0, 1,  0, 1 },
    { "[a-]",         "a",             0, 1,  0, 1 },

    // --- named classes ------------------------------------------
    { "[[:digit:]]+", "abc987",        0, 1,  3, 6 },
    { "[[:alpha:]]+", "12abc34",       0, 1,  2, 5 },
    { "[[:space:]]",  "a b",           0, 1,  1, 2 },
    { "[[:upper:]]+", "abcDEFghi",     0, 1,  3, 6 },

    // --- escapes ------------------------------------------------
    { "a\\.c",        "a.c",           0, 1,  0, 3 },
    { "a\\.c",        "abc",           0, 0, -1, -1 },
    { "\\.qoi$",      "icon.qoi",      0, 1,  4, 8 },
    { "a\\*b",        "a*b",           0, 1,  0, 3 },
    { "\\(x\\)",      "(x)",           0, 1,  0, 3 },
    { "a\\tb",        "a\tb",          0, 1,  0, 3 },

    // --- intervals ----------------------------------------------
    { "a{3}",         "aaaa",          0, 1,  0, 3 },
    { "a{3}",         "aa",            0, 0, -1, -1 },
    { "a{2,3}",       "aaaa",          0, 1,  0, 3 },
    { "a{2,}",        "aaaaa",         0, 1,  0, 5 },
    { "(ab){2}",      "ababab",        0, 1,  0, 4 },

    // --- case folding -------------------------------------------
    { "hello",        "HELLO",         1, 1,  0, 5 },
    { "HeLLo",        "hello",         1, 1,  0, 5 },
    { "[a-z]+",       "ABC",           1, 1,  0, 3 },
    { "hello",        "HELLO",         0, 0, -1, -1 },
    { "\\.QOI$",      "icon.qoi",      1, 1,  4, 8 },

    // --- real ones, from this OS's own output --------------------
    { "^fs:",         "fs: mounting tfs3 from partition 1", 0, 1, 0, 3 },
    { "partition",    "fs: GPT partition table, 1 entries", 0, 1, 8, 17 },
    { "tfs[23]",      "active backend: tfs3",               0, 1, 16, 20 },
    { "LBA [0-9]+",   "active (18872287 sectors at LBA 2048 of ata)", 0, 1, 28, 36 },
};

#define RX_CASE_COUNT ((int)(sizeof RX_CASES / sizeof RX_CASES[0]))

// Patterns regcomp() must REJECT. A parser rejects rather than guesses
// -- and each of these is something a backtracking engine would either
// accept and mis-handle or loop on.
struct rx_bad {
    const char *pattern;
    const char *why;
};

static const struct rx_bad RX_BAD[] = {
    { "[abc",     "unterminated bracket" },
    { "(abc",     "unterminated group" },
    { "abc)",     "unbalanced close paren" },
    { "a\\",      "trailing backslash" },
    { "*abc",     "repeat with nothing to repeat" },
    { "a{2,1}",   "reversed interval" },
    { "[[:bogus:]]", "unknown character class" },
    { "[z-a]",    "reversed range" },
    { "(a)\\1",   "back-reference -- an NFA cannot do these" },
};

#define RX_BAD_COUNT ((int)(sizeof RX_BAD / sizeof RX_BAD[0]))

#endif
