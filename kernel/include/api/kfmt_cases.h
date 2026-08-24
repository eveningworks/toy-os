#ifndef KFMT_CASES_H
#define KFMT_CASES_H

// A table of (format, argument, expected output) cases for kfmt.c,
// COMPILED INTO BOTH RINGS and asserted from both -- the same shape
// klineedit_cases.h uses, and for the same two reasons plus a third.
//
// One: kfmt.c is built twice, once for the kernel image and once into
// libc.a, where it IS `printf` and `snprintf`. "The same source" is not
// "the same behaviour" across two code models and two warning sets.
//
// Two: the header is one file and the implementation is TWO
// (kfmt.c/kfmt_print.c), so a kernel include creeping into the shared
// half silently takes snprintf away from ring 3.
//
// Three, and this is why the table is EXHAUSTIVE over conversions and
// flags rather than a handful of interesting cases. An unrecognised
// conversion in this formatter does not print a wrong value -- it emits
// its letters literally and CONSUMES NO ARGUMENT, so every later
// conversion in the same call reads the wrong slot. A missing feature
// therefore corrupts output that has nothing to do with it, arbitrarily
// far away. That has now shipped three times:
//
//   %.3d  precision ignored on integers -- Doom asked its WAD for lump
//         "STCFN33" instead of "STCFN033" and died at startup.
//   %X    missing entirely -- /bin/font printed a codepoint where a
//         slot number belonged, because the %X ate nothing and the %d
//         after it read the wrong argument.
//   %p %o %+d %hd  found by auditing the switch after the second one,
//         all the same class, none of them yet in anyone's way.
//
// So the rule this table encodes: **every conversion and every flag C
// defines has a case here, including the ones that do nothing on this
// implementation** -- because "does nothing" and "is not parsed" look
// identical until the argument after it moves.
//
// ARGUMENT-CONSUMPTION IS ASSERTED SEPARATELY FROM RENDERING. A case
// with `KFMT_ARG_INT_INT` puts a second `%d` after the conversion under
// test and pins ITS value, which is the only way to catch the failure
// above -- a case that checks the rendering alone passes while the rest
// of the line is wrong.

// What the case's format string wants passed to it. Varargs cannot be
// table-driven, so the runner switches on this and makes the call with
// the right static types -- which is also what keeps the test honest
// about default argument promotion.
enum kfmt_arg {
    KFMT_ARG_NONE = 0,  // no argument at all ("%%", literals)
    KFMT_ARG_INT,       // one int
    KFMT_ARG_UINT,      // one unsigned int
    KFMT_ARG_LONG,      // one long
    KFMT_ARG_ULONG,     // one unsigned long
    KFMT_ARG_STR,       // one const char *
    KFMT_ARG_CHAR,      // one int, passed to %c
    KFMT_ARG_PTR,       // one void *, built from `a`
    KFMT_ARG_INT_INT,   // TWO ints -- the consumption check
};

struct kfmt_case {
    const char   *fmt;
    enum kfmt_arg kind;
    long long     a;     // the first argument, whatever its type
    long long     b;     // the second, for KFMT_ARG_INT_INT
    const char   *s;     // for KFMT_ARG_STR
    const char   *want;
};

extern const struct kfmt_case kfmt_cases[];
extern const int kfmt_case_count;

// Formats one case into `out` (NUL-terminated, capped). Returns 1 when
// the result matched `want`, 0 otherwise -- `out` is filled either way
// so a failure can print what it got.
int kfmt_case_run(const struct kfmt_case *c, char *out, int cap);

#endif
