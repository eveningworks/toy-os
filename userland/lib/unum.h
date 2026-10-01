#ifndef ULIB_UNUM_H
#define ULIB_UNUM_H

// A number written the way the LC_NUMERIC locale writes it: format it
// the C way ("1234567.89", "12.5%", "1.2 MiB") and pass the string
// through unum_localize(), which swaps in the decimal mark and, with
// UNUM_GROUP, groups the whole part ("1 234 567,89" in Finland). There
// is no floating point in ring 3, so every caller already builds its
// decimals from integers; this is the one place that knows what the
// separators are.
//
// GROUPING IS THE CALLER'S CHOICE: a figure a person reads whole (the
// Calculator) wants it; a size in a column ("1007,6M") does not, since a
// separator there only widens the column -- Explorer's size column and
// coreutils' -h leave sizes ungrouped too.
//
// THE FIRST NUMBER IN THE STRING ONLY, and only its digits and point:
// a sign, a prefix and a unit suffix pass through untouched. An IPv4
// address is not a number -- never pass one.
//
// In the "C" locale it changes nothing.
#include <locale.h>
#include <string.h>

#define UNUM_GROUP 1

static inline void unum_localize(char *s, unsigned long cap, int flags) {
    const struct lconv *lc = localeconv();
    char dec = lc->decimal_point[0] ? lc->decimal_point[0] : '.';
    char sep = (flags & UNUM_GROUP) ? lc->thousands_sep[0] : 0;
    if (dec == '.' && !sep) return;

    char *p = s;
    while (*p && (*p < '0' || *p > '9')) p++;
    if (!*p) return;
    char *int_end = p;
    while (*int_end >= '0' && *int_end <= '9') int_end++;
    int digits = (int)(int_end - p);

    char out[64];
    unsigned n = 0;
    for (char *q = s; q < p && n < sizeof out - 1; q++) out[n++] = *q;
    for (int i = 0; i < digits && n < sizeof out - 1; i++) {
        // Groups of three from the right; a four-digit number groups
        // too ("1 234"), as CLDR's Finnish and English both do.
        if (sep && i > 0 && (digits - i) % 3 == 0 && n < sizeof out - 1) out[n++] = sep;
        if (n < sizeof out - 1) out[n++] = p[i];
    }
    char *rest = int_end;
    if (*rest == '.' && rest[1] >= '0' && rest[1] <= '9') {
        if (n < sizeof out - 1) out[n++] = dec;
        rest++;
    }
    while (*rest && n < sizeof out - 1) out[n++] = *rest++;
    out[n] = '\0';
    // A result that does not fit leaves the C spelling, which is wrong
    // in a known way rather than cut short.
    if (n + 1 <= cap && !*rest) memcpy(s, out, n + 1);
}

#endif // ULIB_UNUM_H
