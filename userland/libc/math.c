// The out-of-line half of <math.h>. Everything here is exact integer
// reasoning over the IEEE-754 representation -- there is no
// approximation in this file, which is why there is no accuracy note.
//
// Written against the BITS rather than with casts through integers,
// because a double can hold values no integer type can and
// `(double)(long long)x` is undefined once |x| exceeds LLONG_MAX. That
// is not hypothetical for floor(1e300).
#include <math.h>
#include <string.h>

#define EXP_MASK  0x7FF0000000000000ull
#define FRAC_MASK 0x000FFFFFFFFFFFFFull
#define SIGN_MASK 0x8000000000000000ull

static uint64_t bits(double v) { uint64_t b; k_memcpy(&b, &v, sizeof b); return b; }
static double dbl(uint64_t b) { double v; k_memcpy(&v, &b, sizeof v); return v; }

double trunc(double x) {
    uint64_t b = bits(x);
    int e = (int)((b & EXP_MASK) >> 52) - 1023;
    // e < 0: |x| < 1, so the integer part is a signed zero.
    if (e < 0) return dbl(b & SIGN_MASK);
    // e >= 52: every mantissa bit is already an integer, including the
    // infinities and NaNs, which must come back unchanged.
    if (e >= 52) return x;
    // Clear the fraction bits the exponent does not reach.
    uint64_t keep = ~(FRAC_MASK >> e);
    return dbl(b & keep);
}

double floor(double x) {
    double t = trunc(x);
    // Toward negative infinity: a negative value that lost a fraction
    // has to go one further down.
    if (x < 0.0 && t != x) return t - 1.0;
    return t;
}

double ceil(double x) {
    double t = trunc(x);
    if (x > 0.0 && t != x) return t + 1.0;
    return t;
}

double round(double x) {
    // Half away from zero, which is C's round() -- NOT the
    // round-half-to-even the hardware does, and the difference is
    // visible at exactly 0.5. Done by adding the half toward the sign
    // and truncating, so no comparison against 0.5 is needed.
    double t = trunc(x);
    double frac = x - t;
    if (frac >= 0.5) return t + 1.0;
    if (frac <= -0.5) return t - 1.0;
    return t;
}

double ldexp(double x, int e) {
    // Repeated scaling by powers of two, which is EXACT in binary
    // floating point (each step only moves the exponent) as long as
    // nothing overflows or goes subnormal. Stepping by 1023 at a time
    // keeps every intermediate representable, which a single
    // 2^e multiply would not for large |e|.
    while (e > 1023) { x *= dbl((uint64_t)(1023 + 1023) << 52); e -= 1023; }
    while (e < -1023) { x *= dbl((uint64_t)(1023 - 1023) << 52); e += 1023; }
    return x * dbl((uint64_t)(e + 1023) << 52);
}

double frexp(double x, int *e) {
    uint64_t b = bits(x);
    int ex = (int)((b & EXP_MASK) >> 52);
    if (ex == 0) { // zero or subnormal
        if ((b & ~SIGN_MASK) == 0) { *e = 0; return x; }
        // Normalise the subnormal by scaling up a known amount, then
        // correcting the exponent -- cheaper and clearer than a bit
        // search, and this path is rare.
        x *= 18014398509481984.0; // 2^54
        b = bits(x);
        ex = (int)((b & EXP_MASK) >> 52);
        *e = ex - 1022 - 54;
    } else if (ex == 0x7FF) { // inf or NaN
        *e = 0;
        return x;
    } else {
        *e = ex - 1022;
    }
    // Force the exponent to 1022, which puts the mantissa in [0.5, 1).
    return dbl((b & ~EXP_MASK) | ((uint64_t)1022 << 52));
}

double fmod(double x, double y) {
    if (y == 0.0 || isnan(x) || isnan(y) || isinf(x)) return NAN;
    if (isinf(y)) return x;
    // Repeated subtraction of a SCALED divisor rather than
    // x - trunc(x/y)*y: the second loses everything once x/y exceeds
    // the mantissa, and fmod is required to be exact.
    double ax = fabs(x), ay = fabs(y);
    if (ax < ay) return x;
    int ex, ey;
    frexp(ax, &ex);
    frexp(ay, &ey);
    for (int e = ex - ey; e >= 0; e--) {
        double scaled = ldexp(ay, e);
        if (scaled <= ax) ax -= scaled;
    }
    return copysign(ax, x);
}
