// sscanf/vsscanf -- the input half of the formatted-I/O pair.
//
// NOT built on kfmt. The output side is one formatter shared with the
// kernel because the kernel formats constantly; nothing in ring 0 has
// ever PARSED a format string, so there is no second caller to share
// with and no reason to put a scanner in the shared toolkit. This is
// the C library's, and it lives here.
//
// The conversions are the ones a real program uses: %d %i %u %o %x %c
// %s %f/%e/%g %n %%, a field width, `*` to suppress the assignment, and
// the h/hh/l/ll/z length modifiers. Character classes (`%[a-z]`) are
// absent -- they are a parser of their own and nothing has needed one.
//
// THE RETURN VALUE IS THE NUMBER ASSIGNED, and EOF only when the input
// ended before the FIRST assignment. That distinction is the whole
// error protocol: a caller checking `== 2` after "%d %d" learns which
// half failed, and one checking `!= EOF` learns nothing useful.
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <stdarg.h>

struct scan {
    const char *p;
    int assigned;
    int eof;      // input ran out
};

static void skip_ws(struct scan *sc) {
    while (isspace((unsigned char)*sc->p)) sc->p++;
}

// Signed and unsigned share one digit loop for the same reason
// strtol/strtoul do: the base detection and the overflow behaviour are
// where a divergence would hide.
static int read_int(struct scan *sc, int base, int is_signed, int width,
                    unsigned long long *out, int *neg_out) {
    const char *start = sc->p;
    skip_ws(sc);
    int neg = 0;
    int used = 0;
    if (width == 0) width = -1; // no limit
    if ((*sc->p == '+' || *sc->p == '-') && width != 0) {
        neg = (*sc->p == '-');
        sc->p++;
        if (width > 0) { width--; used++; }
    }
    if (base == 0) {
        if (sc->p[0] == '0' && (sc->p[1] == 'x' || sc->p[1] == 'X') && width != 1) {
            base = 16; sc->p += 2; if (width > 0) width -= 2;
        } else if (sc->p[0] == '0') {
            base = 8;
        } else {
            base = 10;
        }
    } else if (base == 16 && sc->p[0] == '0' && (sc->p[1] == 'x' || sc->p[1] == 'X')) {
        sc->p += 2; if (width > 0) width -= 2;
    }

    unsigned long long v = 0;
    int digits = 0;
    while (width != 0) {
        int c = (unsigned char)*sc->p, d;
        if (isdigit(c)) d = c - '0';
        else if (isalpha(c)) d = tolower(c) - 'a' + 10;
        else break;
        if (d >= base) break;
        v = v * (unsigned long long)base + (unsigned long long)d;
        sc->p++;
        digits++;
        if (width > 0) width--;
    }
    if (!digits) { sc->p = start; return 0; } // no conversion: put it all back
    (void)is_signed;
    (void)used;
    *out = v;
    *neg_out = neg;
    return 1;
}

// Stores through the pointer at the width the length modifier asked
// for. One place, so a missing size is a missing CASE rather than a
// silently truncated store somewhere.
// Takes the va_list BY VALUE and consumes from it, which reads wrong
// and is right on this ABI: a `va_list` is an array type, so a va_list
// parameter is already a pointer to the caller's state. Same mechanism
// k_fmt_float() uses (api/kfmt.h), and the reason `&ap` does not
// compile -- that is a pointer to the pointer.
static void store_int(va_list ap, int len, unsigned long long v, int neg, int is_signed) {
    long long sv = neg ? -(long long)v : (long long)v;
    switch (len) {
    case -2: *va_arg(ap, signed char *) = (signed char)sv; break;   // hh
    case -1: *va_arg(ap, short *) = (short)sv; break;               // h
    case 1:  *va_arg(ap, long *) = (long)sv; break;                 // l
    case 2:  *va_arg(ap, long long *) = (long long)sv; break;       // ll
    default:
        if (is_signed) *va_arg(ap, int *) = (int)sv;
        else           *va_arg(ap, unsigned *) = (unsigned)v;
        break;
    }
}

int vsscanf(const char *s, const char *fmt, va_list ap) {
    struct scan sc = { s, 0, 0 };

    for (const char *f = fmt; *f; f++) {
        if (isspace((unsigned char)*f)) {
            // WHITESPACE IN THE FORMAT MATCHES ANY RUN OF WHITESPACE,
            // including none -- it is not "match one space".
            skip_ws(&sc);
            continue;
        }
        if (*f != '%') {
            // An ordinary character must match exactly; a mismatch ends
            // the whole call, it does not skip the conversion.
            if (*sc.p != *f) return sc.assigned;
            sc.p++;
            continue;
        }

        f++;
        if (*f == '%') {
            skip_ws(&sc);
            if (*sc.p != '%') return sc.assigned;
            sc.p++;
            continue;
        }

        int suppress = 0;
        if (*f == '*') { suppress = 1; f++; }

        int width = 0;
        while (isdigit((unsigned char)*f)) { width = width * 10 + (*f - '0'); f++; }

        int len = 0; // -2 hh, -1 h, 0 none, 1 l, 2 ll
        if (*f == 'h') { len = -1; f++; if (*f == 'h') { len = -2; f++; } }
        else if (*f == 'l') { len = 1; f++; if (*f == 'l') { len = 2; f++; } }
        else if (*f == 'z' || *f == 'j' || *f == 't') { len = 1; f++; }
        else if (*f == 'L') { len = 1; f++; }

        unsigned long long v = 0;
        int neg = 0;

        switch (*f) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
            int base = (*f == 'd' || *f == 'u') ? 10 :
                       (*f == 'o') ? 8 :
                       (*f == 'i') ? 0 : 16;
            if (!read_int(&sc, base, *f != 'u', width, &v, &neg)) {
                if (!*sc.p) sc.eof = 1;
                return sc.assigned ? sc.assigned : (sc.eof ? EOF : 0);
            }
            if (!suppress) { store_int(ap, len, v, neg, *f != 'u'); sc.assigned++; }
            break;
        }
        case 'f': case 'e': case 'E': case 'g': case 'G': case 'a': {
            skip_ws(&sc);
            char *end;
            // strtod does the parsing, so text -> double means the same
            // thing whichever way a program asks for it.
            double d = strtod(sc.p, &end);
            if (end == sc.p) {
                if (!*sc.p) sc.eof = 1;
                return sc.assigned ? sc.assigned : (sc.eof ? EOF : 0);
            }
            sc.p = end;
            if (!suppress) {
                // `%f` is a float and `%lf` is a double -- the one
                // length modifier that changes the TYPE rather than its
                // width, and getting it wrong writes four bytes where
                // eight belong.
                if (len >= 1) *va_arg(ap, double *) = d;
                else          *va_arg(ap, float *) = (float)d;
                sc.assigned++;
            }
            break;
        }
        case 's': {
            skip_ws(&sc);
            if (!*sc.p) { sc.eof = 1; return sc.assigned ? sc.assigned : EOF; }
            char *dst = suppress ? 0 : va_arg(ap, char *);
            int n = 0;
            while (*sc.p && !isspace((unsigned char)*sc.p) && (width == 0 || n < width)) {
                if (dst) dst[n] = *sc.p;
                sc.p++;
                n++;
            }
            if (dst) { dst[n] = '\0'; sc.assigned++; }
            break;
        }
        case 'c': {
            // NO leading-whitespace skip, and NO NUL terminator: %c
            // reads exactly `width` characters (1 by default) as a
            // block, which is the one conversion that does neither.
            int n = width ? width : 1;
            char *dst = suppress ? 0 : va_arg(ap, char *);
            for (int i = 0; i < n; i++) {
                if (!*sc.p) { sc.eof = 1; return sc.assigned ? sc.assigned : EOF; }
                if (dst) dst[i] = *sc.p;
                sc.p++;
            }
            if (dst) sc.assigned++;
            break;
        }
        case 'n':
            // Assigns the count so far and does NOT count as an
            // assignment, which is C's rule and the reason a caller
            // cannot detect %n by the return value.
            if (!suppress) *va_arg(ap, int *) = (int)(sc.p - s);
            break;
        default:
            // An unknown conversion ends the call rather than guessing
            // how many arguments it would have taken -- the same
            // refuse-rather-than-guess rule the parsers here follow.
            return sc.assigned;
        }
    }
    return sc.assigned;
}

int sscanf(const char *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsscanf(s, fmt, ap);
    va_end(ap);
    return n;
}
