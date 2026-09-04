#ifndef ULIB_HUMAN_H
#define ULIB_HUMAN_H

// "1.2K", "4.0M" -- a byte count a person reads, for `-h`-style output.
//
// A header rather than a third copy. /bin/df and /bin/meminfo had this
// written out by hand, byte for byte identical -- two REAL callers,
// which is the bar (CLAUDE.md: a second real caller, not a plausible
// one).
//
// /bin/ls deliberately does NOT use it, and that is worth knowing
// before "unifying" the third one. Its `-h` is a different function
// wearing a similar shape: no `B` suffix on a plain byte count, the
// decimal dropped once the whole part reaches 10 ("12K", not "12.3K"),
// and the result right-aligned in a fixed column. That is coreutils'
// `ls -h`, it is what its callers expect, and folding it in here would
// change ls's output to fix a duplication that is not one.
//
// INTEGER ONLY, and the tenth comes out of the REMAINDER: there is no
// floating point in ring 3 (-mno-sse), so `(rem * 10) / 1024` is the
// one digit after the point. Multiplying before dividing is what keeps
// it exact -- the other order truncates to zero.
//
// It stops at G deliberately. A T would need the unit table and the
// loop bound to agree, and this OS's disk is measured in gigabytes; a
// value past the table's end formats in the largest unit it has rather
// than falling off the end of it.
//
// Inline rather than a .c file for the same reason cmd.h is: it is a
// dozen lines on the far side of --gc-sections, and a program that
// includes it and never calls it links nothing.
#include <stdio.h>

static inline void human_size(char *out, unsigned long cap, unsigned long long n) {
    static const char unit[] = { 'B', 'K', 'M', 'G' };
    int u = 0;
    unsigned long long whole = n, rem = 0;
    while (whole >= 1024 && u < 3) {
        rem = whole % 1024;
        whole /= 1024;
        u++;
    }
    // Bytes get no decimal: "512B" is exact and "512.0B" implies a
    // precision that is not there.
    if (u == 0) snprintf(out, cap, "%lluB", whole);
    else snprintf(out, cap, "%llu.%llu%c", whole, (rem * 10) / 1024, unit[u]);
}

// "1.2 MiB", "3.4 GiB" -- the same number with the unit spelled out and
// a space before it, for a window somebody reads (a gauge, an About
// box) rather than a column. Task Manager and About each had one.
static inline void human_size_iec(char *out, unsigned long cap, unsigned long long n) {
    static const char *const unit[] = { "B", "KiB", "MiB", "GiB" };
    int u = 0;
    unsigned long long whole = n, rem = 0;
    while (whole >= 1024 && u < 3) {
        rem = whole % 1024;
        whole /= 1024;
        u++;
    }
    if (u == 0) snprintf(out, cap, "%llu %s", whole, unit[0]);
    else snprintf(out, cap, "%llu.%llu %s", whole, (rem * 10) / 1024, unit[u]);
}

#endif // ULIB_HUMAN_H
