// scanf/fscanf/sscanf -- the input half of the formatted-I/O pair.
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
// ONE SCANNER OVER A SOURCE. A string and a stream differ only in where
// the next character comes from and where a rejected one goes back to,
// so `struct src` is those two function pointers and everything else is
// shared. The alternative -- a string scanner plus a stream scanner --
// is two copies of the conversion table, which is the duplication this
// project's shared-source rule exists to prevent.
//
// The stream source needs MULTI-CHARACTER PUSHBACK: deciding that "0x"
// does not begin a number, or that "1e" has no exponent, means putting
// several characters back. C guarantees only one, so <stdio.h>'s FILE
// carries eight -- which is a permitted extension and cheaper than the
// private input buffer a scanner would otherwise need in front of the
// stream's own.
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

// Where characters come from. `get` returns EOF at the end; `unget`
// takes one back, and must accept at least eight in a row.
struct src {
    int (*get)(void *ctx);
    void (*unget)(void *ctx, int c);
    void *ctx;
    int consumed;   // characters taken, for %n
};

struct scan {
    struct src *src;
    int assigned;
    int eof;
};

static int sc_get(struct scan *sc) {
    int c = sc->src->get(sc->src->ctx);
    if (c != EOF) sc->src->consumed++;
    return c;
}

static void sc_unget(struct scan *sc, int c) {
    if (c == EOF) return;
    sc->src->unget(sc->src->ctx, c);
    sc->src->consumed--;
}

// C's two failure kinds, which the return value must tell apart: an
// INPUT failure (nothing left to read) is EOF, a MATCHING failure
// (characters are there and none of them fit) is 0. A scanner that
// reports EOF for both makes `while (scanf("%d", &n) != EOF)` spin
// forever on the first letter it meets.
static int fail(struct scan *sc) {
    if (sc->assigned) return sc->assigned;
    int c = sc_get(sc);
    sc_unget(sc, c);
    return c == EOF ? EOF : 0;
}

static void skip_ws(struct scan *sc) {
    int c;
    while ((c = sc_get(sc)) != EOF && isspace(c)) {}
    sc_unget(sc, c);
}

// --- the two sources -------------------------------------------------

struct strsrc { const char *s; const char *p; };

static int str_get(void *ctx) {
    struct strsrc *ss = (struct strsrc *)ctx;
    return *ss->p ? (unsigned char)*ss->p++ : EOF;
}
static void str_unget(void *ctx, int c) {
    struct strsrc *ss = (struct strsrc *)ctx;
    (void)c;
    // Backing up a POINTER rather than storing the character: for a
    // string the two are the same thing, and this cannot disagree with
    // what is actually there.
    if (ss->p > ss->s) ss->p--;
}

static int file_get(void *ctx) { return fgetc((FILE *)ctx); }
static void file_unget(void *ctx, int c) { ungetc(c, (FILE *)ctx); }

// Signed and unsigned share one digit loop for the same reason
// strtol/strtoul do: the base detection and the overflow behaviour are
// where a divergence would hide.
static int read_int(struct scan *sc, int base, int width,
                    unsigned long long *out, int *neg_out) {
    // Every character taken is pushed back if the conversion turns out
    // not to have one -- which is why the source needs more than one
    // byte of pushback: "0x" followed by a non-digit puts three back.
    int back[8];
    int nback = 0;
    skip_ws(sc);
    if (width == 0) width = -1;

    int neg = 0;
    int c = sc_get(sc);
    if (c == '+' || c == '-') {
        neg = (c == '-');
        back[nback++] = c;
        if (width > 0) width--;
        c = sc_get(sc);
    }

    if (base == 0 || base == 16) {
        if (c == '0') {
            int c2 = sc_get(sc);
            if ((c2 == 'x' || c2 == 'X') && width != 1) {
                base = 16;
                back[nback++] = c;
                back[nback++] = c2;
                if (width > 0) width -= 2;
                c = sc_get(sc);
            } else {
                // A leading zero IS a digit, and in base 0 it also
                // selects octal. Put the second character back and
                // carry on with the zero already in hand.
                sc_unget(sc, c2);
                if (base == 0) base = 8;
            }
        } else if (base == 0) {
            base = 10;
        }
    }

    unsigned long long v = 0;
    int digits = 0;
    while (width != 0 && c != EOF) {
        int d;
        if (isdigit(c)) d = c - '0';
        else if (isalpha(c)) d = tolower(c) - 'a' + 10;
        else break;
        if (d >= base) break;
        v = v * (unsigned long long)base + (unsigned long long)d;
        digits++;
        if (width > 0) width--;
        c = sc_get(sc);
    }
    sc_unget(sc, c);
    if (!digits) {
        // NO CONVERSION: put back everything this attempt consumed, in
        // reverse, so the next conversion sees the input untouched.
        while (nback > 0) sc_unget(sc, back[--nback]);
        return 0;
    }
    *out = v;
    *neg_out = neg;
    return 1;
}

// Reads a floating-point literal into a buffer and hands it to strtod,
// so text -> double means the same thing however a program asks for it.
static int read_double(struct scan *sc, int width, double *out) {
    char buf[64];
    int n = 0;
    skip_ws(sc);
    if (width <= 0 || width > (int)sizeof buf - 1) width = (int)sizeof buf - 1;
    // Accepts the SHAPE of a number and lets strtod judge it: taking
    // every plausible character and then backing up to strtod's endptr
    // is what keeps the two agreeing about what a number is.
    int c;
    while (n < width && (c = sc_get(sc)) != EOF) {
        if (isdigit(c) || c == '+' || c == '-' || c == '.' ||
            c == 'e' || c == 'E' || c == 'x' || c == 'X' ||
            (isalpha(c) && n < 3)) {   // inf / nan
            buf[n++] = (char)c;
        } else {
            sc_unget(sc, c);
            break;
        }
    }
    buf[n] = '\0';
    char *end;
    double d = strtod(buf, &end);
    if (end == buf) {
        while (n > 0) sc_unget(sc, (unsigned char)buf[--n]);
        return 0;
    }
    // Give back whatever strtod did not use.
    for (int i = n - 1; i >= (int)(end - buf); i--) sc_unget(sc, (unsigned char)buf[i]);
    *out = d;
    return 1;
}

// Stores through the pointer at the width the length modifier asked
// for. One place, so a missing size is a missing CASE rather than a
// silently truncated store somewhere.
//
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

// The scanner itself. Knows nothing about where the characters are
// coming from.
static int vscan(struct scan *sc, const char *fmt, va_list ap) {
    for (const char *f = fmt; *f; f++) {
        if (isspace((unsigned char)*f)) {
            // WHITESPACE IN THE FORMAT MATCHES ANY RUN OF WHITESPACE,
            // including none -- it is not "match one space".
            skip_ws(sc);
            continue;
        }
        if (*f != '%') {
            // An ordinary character must match exactly; a mismatch ends
            // the whole call, it does not skip the conversion.
            int c = sc_get(sc);
            if (c != (unsigned char)*f) { sc_unget(sc, c); return sc->assigned; }
            continue;
        }

        f++;
        if (*f == '%') {
            skip_ws(sc);
            int c = sc_get(sc);
            if (c != '%') { sc_unget(sc, c); return sc->assigned; }
            continue;
        }

        int suppress = 0;
        if (*f == '*') { suppress = 1; f++; }

        int width = 0;
        while (isdigit((unsigned char)*f)) { width = width * 10 + (*f - '0'); f++; }

        int len = 0; // -2 hh, -1 h, 0 none, 1 l, 2 ll
        if (*f == 'h') { len = -1; f++; if (*f == 'h') { len = -2; f++; } }
        else if (*f == 'l') { len = 1; f++; if (*f == 'l') { len = 2; f++; } }
        else if (*f == 'z' || *f == 'j' || *f == 't' || *f == 'L') { len = 1; f++; }

        unsigned long long v = 0;
        int neg = 0;

        switch (*f) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p': {
            int base = (*f == 'd' || *f == 'u') ? 10 :
                       (*f == 'o') ? 8 :
                       (*f == 'i') ? 0 : 16;
            if (!read_int(sc, base, width, &v, &neg)) return fail(sc);
            if (!suppress) { store_int(ap, len, v, neg, *f != 'u'); sc->assigned++; }
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': {
            double d;
            if (!read_double(sc, width, &d)) return fail(sc);
            if (!suppress) {
                // `%f` is a float and `%lf` is a double -- the one
                // length modifier that changes the TYPE rather than its
                // width, and getting it wrong writes four bytes where
                // eight belong.
                if (len >= 1) *va_arg(ap, double *) = d;
                else          *va_arg(ap, float *) = (float)d;
                sc->assigned++;
            }
            break;
        }
        case 's': {
            skip_ws(sc);
            char *dst = suppress ? 0 : va_arg(ap, char *);
            int n = 0, c;
            while ((c = sc_get(sc)) != EOF && !isspace(c) && (width == 0 || n < width)) {
                if (dst) dst[n] = (char)c;
                n++;
            }
            sc_unget(sc, c);
            if (!n) return fail(sc);
            if (dst) { dst[n] = '\0'; sc->assigned++; }
            break;
        }
        case 'c': {
            // NO leading-whitespace skip, and NO NUL terminator: %c
            // reads exactly `width` characters (1 by default) as a
            // block, which is the one conversion that does neither.
            int n = width ? width : 1;
            char *dst = suppress ? 0 : va_arg(ap, char *);
            for (int i = 0; i < n; i++) {
                int c = sc_get(sc);
                if (c == EOF) return fail(sc);
                if (dst) dst[i] = (char)c;
            }
            if (dst) sc->assigned++;
            break;
        }
        case 'n':
            // Assigns the count so far and does NOT count as an
            // assignment, which is C's rule and the reason a caller
            // cannot detect %n by the return value.
            if (!suppress) *va_arg(ap, int *) = sc->src->consumed;
            break;
        default:
            // An unknown conversion ends the call rather than guessing
            // how many arguments it would have taken -- the same
            // refuse-rather-than-guess rule the parsers here follow.
            return sc->assigned;
        }
    }
    return sc->assigned;
}

// --- the three public entry points ------------------------------------

int vsscanf(const char *s, const char *fmt, va_list ap) {
    struct strsrc ss = { s, s };
    struct src src = { str_get, str_unget, &ss, 0 };
    struct scan sc = { &src, 0, 0 };
    return vscan(&sc, fmt, ap);
}

int sscanf(const char *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsscanf(s, fmt, ap);
    va_end(ap);
    return n;
}

int vfscanf(FILE *f, const char *fmt, va_list ap) {
    struct src src = { file_get, file_unget, f, 0 };
    struct scan sc = { &src, 0, 0 };
    return vscan(&sc, fmt, ap);
}

int fscanf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfscanf(f, fmt, ap);
    va_end(ap);
    return n;
}

int vscanf(const char *fmt, va_list ap) { return vfscanf(stdin, fmt, ap); }

int scanf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfscanf(stdin, fmt, ap);
    va_end(ap);
    return n;
}
