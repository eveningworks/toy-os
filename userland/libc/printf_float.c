// RING 3's half of kfmt's floating-point conversion -- %f %e %g and
// their uppercase forms. The kernel's half is kernel/lib/kfmt_nofloat.c
// and returns 0; see api/kfmt.h for why the split exists at all (the
// kernel is built -mno-sse, and `va_arg(ap, double)` is SSE).
//
// **ACCURACY, STATED HONESTLY.** Digits are produced by repeatedly
// scaling the value by ten and taking the integer part, which is what
// small C libraries have always done and is NOT correctly rounded in
// the last place for every input. Getting that right needs an
// exact-arithmetic algorithm over the mantissa -- Dragon4, or Grisu /
// Ryu for the fast paths -- which is several hundred lines and a table
// of powers of ten, and buys accuracy in the 17th digit that nothing
// here is doing anything with. What you get: the first ~15 significant
// digits are right, round-tripping a double through %.17g is NOT
// guaranteed, and printf("%.2f", 0.1) is "0.10".
//
// If that ever stops being good enough, this file is the whole surface
// to replace -- which is the reason the conversion is behind one
// function rather than spread through the formatter.
//
// NaN and the infinities are detected from the BITS, not by comparing
// the value against itself: -ffast-math is not on, but a comparison is
// a thing an optimiser is allowed to reason about and a bit pattern is
// not.
#include <kfmt.h>
#include <stdint.h>
#include <string.h>

#define EXP_MASK  0x7FF0000000000000ull
#define FRAC_MASK 0x000FFFFFFFFFFFFFull
#define SIGN_MASK 0x8000000000000000ull

static uint64_t bits_of(double v) {
    uint64_t b;
    k_memcpy(&b, &v, sizeof b); // not a pointer cast: that is the aliasing rule
    return b;
}

struct buf { char *p; size_t cap; size_t n; };

static void bput(struct buf *b, char c) {
    if (b->n + 1 < b->cap) b->p[b->n++] = c;
}

static void bputs(struct buf *b, const char *s) { while (*s) bput(b, *s++); }

// The integer part, which may be far larger than any integer type once
// the exponent is big. Emitted most-significant digit first by finding
// the largest power of ten that fits, so there is no reversal buffer
// and no 300-digit array.
static void put_int_part(struct buf *b, double v) {
    if (v < 1.0) { bput(b, '0'); return; }
    double scale = 1.0;
    int digits = 1;
    while (v / scale >= 10.0) { scale *= 10.0; digits++; }
    for (int i = 0; i < digits; i++) {
        int d = (int)(v / scale);
        if (d < 0) d = 0;
        if (d > 9) d = 9; // scaling error at the top digit, clamped rather than emitted as ':'
        bput(b, (char)('0' + d));
        v -= (double)d * scale;
        scale /= 10.0;
    }
}

// %f. `prec` digits after the point, rounded half-away-from-zero.
static void fixed(struct buf *b, double v, int prec) {
    // ROUND FIRST, then split. Splitting first and rounding the
    // fraction separately gets 9.999 at prec=2 wrong: the carry has to
    // reach the integer part, and by then it has been emitted.
    double round = 0.5;
    for (int i = 0; i < prec; i++) round /= 10.0;
    v += round;

    put_int_part(b, v);
    if (prec <= 0) return;
    bput(b, '.');
    double frac = v - (double)(uint64_t)v;
    // For a huge v the subtraction above is meaningless (every bit of
    // the mantissa is in the integer part), and zeros are the honest
    // answer -- they are exactly the digits the value does not have.
    if (v >= 18446744073709551615.0) frac = 0.0;
    for (int i = 0; i < prec; i++) {
        frac *= 10.0;
        int d = (int)frac;
        if (d < 0) d = 0;
        if (d > 9) d = 9;
        bput(b, (char)('0' + d));
        frac -= (double)d;
    }
}

// %e.
static void sci(struct buf *b, double v, int prec, char e_char) {
    int exp10 = 0;
    if (v != 0.0) {
        // Scaled by repeated multiply rather than by a table of powers
        // of ten: the table would be the larger half of this file, and
        // the loop runs at most ~308 times for the most extreme double
        // there is.
        while (v >= 10.0) { v /= 10.0; exp10++; }
        while (v < 1.0)   { v *= 10.0; exp10--; }
    }
    // Rounding the mantissa can push it to 10.0, which has to become
    // 1.0 with one more in the exponent -- the case that makes
    // printf("%.0e", 9.9) read "1e+01" and not "10e+00".
    double round = 0.5;
    for (int i = 0; i < prec; i++) round /= 10.0;
    if (v + round >= 10.0) { v /= 10.0; exp10++; }

    fixed(b, v, prec);
    bput(b, e_char);
    bput(b, exp10 < 0 ? '-' : '+');
    unsigned a = (unsigned)(exp10 < 0 ? -exp10 : exp10);
    // At least two exponent digits, as C requires.
    if (a >= 100) bput(b, (char)('0' + a / 100));
    bput(b, (char)('0' + (a / 10) % 10));
    bput(b, (char)('0' + a % 10));
}

size_t k_fmt_float(char *out, size_t cap, va_list ap, char conv, int prec) {
    double v = va_arg(ap, double);
    struct buf b = { out, cap, 0 };
    int upper = (conv == 'F' || conv == 'E' || conv == 'G');

    uint64_t bi = bits_of(v);
    if (bi & SIGN_MASK) { bput(&b, '-'); v = -v; }
    if ((bi & EXP_MASK) == EXP_MASK) {
        // Infinity and NaN print as C says, and a NaN keeps no sign
        // beyond the one already emitted -- there is nothing else true
        // to say about it.
        int is_nan = (bi & FRAC_MASK) != 0;
        bputs(&b, is_nan ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf"));
        b.p[b.n] = '\0';
        return b.n;
    }

    if (prec < 0) prec = 6; // C's default for all three

    if (conv == 'f' || conv == 'F') {
        fixed(&b, v, prec);
    } else if (conv == 'e' || conv == 'E') {
        sci(&b, v, prec, upper ? 'E' : 'e');
    } else {
        // %g: C's rule. P is the precision (0 means 1); use %e when the
        // decimal exponent is < -4 or >= P, otherwise %f with the
        // precision adjusted so the DIGIT COUNT is P rather than the
        // digits after the point.
        int p10 = prec == 0 ? 1 : prec;
        int exp10 = 0;
        double t = v;
        if (t != 0.0) {
            while (t >= 10.0) { t /= 10.0; exp10++; }
            while (t < 1.0)   { t *= 10.0; exp10--; }
        }
        size_t start = b.n;
        if (exp10 < -4 || exp10 >= p10) sci(&b, v, p10 - 1, upper ? 'E' : 'e');
        else                            fixed(&b, v, p10 - 1 - exp10);
        // %g strips trailing fraction zeros, and the point with them --
        // IN BOTH FORMS. Stripping only the fixed form is the easy
        // mistake and it prints 1e-05 as "1.00000e-05"; the mantissa of
        // the exponent form has to be trimmed too, and then the
        // exponent suffix moved down over the gap.
        size_t epos = b.n;
        for (size_t i = start; i < b.n; i++)
            if (b.p[i] == 'e' || b.p[i] == 'E') { epos = i; break; }

        int has_point = 0;
        for (size_t i = start; i < epos; i++) if (b.p[i] == '.') has_point = 1;
        if (has_point) {
            size_t end = epos;
            while (end > start && b.p[end - 1] == '0') end--;
            if (end > start && b.p[end - 1] == '.') end--;
            if (end < epos) {
                for (size_t i = epos; i < b.n; i++) b.p[end + (i - epos)] = b.p[i];
                b.n -= (epos - end);
            }
        }
    }
    b.p[b.n] = '\0';
    return b.n;
}
