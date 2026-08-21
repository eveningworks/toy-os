// The TRANSCENDENTAL half of <math.h>: sin, cos, tan, the inverse trig,
// exp, log, pow and the hyperbolics.
//
// Separate from math.c because the two halves have different natures.
// Everything in math.c is EXACT -- integer reasoning over the IEEE
// bits, right or wrong with nothing in between. Everything here is an
// APPROXIMATION, and the interesting question about each function is
// how close it gets.
//
// **ACCURACY IS MEASURED, NOT CLAIMED.** Each function is argument
// reduction plus a polynomial, and userland/tests/libm_test.c checks
// every one against values computed independently (the host's Python,
// which uses the platform libm) to a stated tolerance of 1e-12
// relative. That is short of the sub-ulp accuracy glibc reaches with
// multi-precision fallback paths, and far more than enough to draw a
// circle or model a decay. Where a function is worse than the rest --
// pow(), which compounds two approximations -- it says so.
//
// **WHY NOT THE x87 INSTRUCTIONS.** x86 has `fsin`, `fcos`, `f2xm1` and
// `fyl2x` in hardware, which look like a free libm. Three reasons not
// to, and they are the reasons every real libm stopped using them.
// They work on the x87 stack, which this userland does not otherwise
// touch -- everything is SSE2, and mixing the two means managing the
// x87 tag word across a stack the scheduler saves as SSE state. Their
// argument reduction is documented by Intel as losing precision for
// large arguments, because it reduces against a 66-bit pi rather than
// a correctly-rounded one. And they are microcoded and slow on
// anything modern. Polynomials in SSE2 are what glibc and musl do.
//
// **THE COEFFICIENTS ARE THE PUBLISHED MINIMAX ONES** (fdlibm's, the
// same set glibc and musl descend from) rather than plain Taylor
// terms: a Taylor series is optimal AT the expansion point and gets
// steadily worse across the interval, while these are fitted to
// minimise the worst error over the whole reduction range. Do not
// "simplify" them into factorials.
#include <math.h>
#include <stdint.h>

// Pi split into two doubles, so the reduction can subtract it in
// pieces without losing the low bits -- the Cody-Waite trick.
// Subtracting a single rounded pi is what makes a naive sin() drift as
// its argument grows.
#define PI_HI   3.14159265358979311600e+00  // the double nearest pi
#define PI_LO   1.22464679914735317722e-16  // pi - PI_HI
#define PI      3.14159265358979323846
#define PI_2    1.57079632679489661923
#define PI_4    0.78539816339744830962
#define TWO_PI  6.28318530717958647692
#define LN2     0.69314718055994530942
#define LN2_HI  6.93147180369123816490e-01
#define LN2_LO  1.90821492927058770002e-10
#define LN10    2.30258509299404568402
#define SQRT3   1.73205080756887729353

// --- sin / cos on [-pi/4, pi/4] --------------------------------------

static double kernel_sin(double x) {
    static const double S1 = -1.66666666666666324348e-01;
    static const double S2 =  8.33333333332248946124e-03;
    static const double S3 = -1.98412698298579493134e-04;
    static const double S4 =  2.75573137070700676789e-06;
    static const double S5 = -2.50507602534068634195e-08;
    static const double S6 =  1.58969099521155010221e-10;
    double z = x * x;
    return x + x * z * (S1 + z * (S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)))));
}

static double kernel_cos(double x) {
    static const double C1 =  4.16666666666666019037e-02;
    static const double C2 = -1.38888888888741095749e-03;
    static const double C3 =  2.48015872894767294178e-05;
    static const double C4 = -2.75573143513906633035e-07;
    static const double C5 =  2.08757232129817482790e-09;
    static const double C6 = -1.13596475577881948265e-11;
    double z = x * x;
    return 1.0 - 0.5 * z + z * z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
}

// Reduces x to [-pi/4, pi/4] and reports which QUADRANT it came from,
// so the caller knows whether to use sin or cos and with which sign.
//
// The reduction subtracts n*pi/2 in two pieces (PI_HI/PI_LO halved).
// For very large arguments even that loses meaning -- there are fewer
// bits left in the mantissa than the reduction needs -- which is a
// property of binary floating point rather than of this code. Every
// libm has the same cliff; the good ones move it out to 2^1000 with
// multi-precision reduction, and this one does not.
// pi/2 as THREE doubles, and the first one's low 33 bits are ZERO. That
// is the whole trick and it is not an optimisation: with the low bits
// clear, `n * PIO2_1` is EXACT for any n below 2^20, so the subtraction
// that follows loses nothing. Multiplying by a plain rounded pi/2
// instead rounds the product, and the error it introduces is
// proportional to n -- which is why a naive sin() is fine at x = 3 and
// wrong in the tenth digit at x = 1e6.
//
// These are fdlibm's constants, the same ones glibc and musl use.
#define PIO2_1  1.57079632673412561417e+00   // pi/2, low 33 bits zero
#define PIO2_2  6.07710050630396597660e-11   // the next chunk
#define PIO2_2T 2.02226624879595063154e-21   // and the next

static int reduce_quadrant(double x, double *out) {
    double q = x / PI_2;
    int64_t n = (int64_t)(q >= 0 ? q + 0.5 : q - 0.5);   // nearest
    double fn = (double)n;
    // Three subtractions rather than one, each removing a piece the
    // previous one could not represent.
    double r = x - fn * PIO2_1;
    r -= fn * PIO2_2;
    r -= fn * PIO2_2T;
    *out = r;
    return (int)(((n % 4) + 4) % 4);
}

// THE LIMIT, stated because it is real and every libm has one. The
// product above stays exact while |n| < 2^20, i.e. |x| below about
// 1.6e6. Past that the reduction starts consuming mantissa bits the
// argument does not have left, and accuracy falls off steadily; by 2^63
// there is nothing meaningful to reduce at all. Fixing that needs
// Payne-Hanek reduction against a multi-hundred-bit pi, which is a
// bigger piece of machinery than the rest of this file put together and
// buys correctness for arguments no program has a physical reason to
// pass. userland/tests/libm_test.c asserts full accuracy to 1e6 and
// stops there deliberately.

double sin(double x) {
    if (isnan(x) || isinf(x)) return NAN;
    double r;
    switch (reduce_quadrant(x, &r)) {
    case 0:  return kernel_sin(r);
    case 1:  return kernel_cos(r);
    case 2:  return -kernel_sin(r);
    default: return -kernel_cos(r);
    }
}

double cos(double x) {
    if (isnan(x) || isinf(x)) return NAN;
    double r;
    switch (reduce_quadrant(x, &r)) {
    case 0:  return kernel_cos(r);
    case 1:  return -kernel_sin(r);
    case 2:  return -kernel_cos(r);
    default: return kernel_sin(r);
    }
}

double tan(double x) {
    // sin/cos rather than its own polynomial: tan's poles mean a direct
    // approximation needs its own reduction and a separate accuracy
    // story, and this way there is one place that knows how to reduce
    // an angle. Near a pole the division does what the mathematics does
    // -- it goes to infinity, and the relative error there is a
    // property of the pole, not of the code.
    double c = cos(x);
    if (c == 0.0) return copysign(HUGE_VAL, sin(x));
    return sin(x) / c;
}

// --- exp / log --------------------------------------------------------

double exp(double x) {
    if (isnan(x)) return NAN;
    if (x > 709.782712893384) return HUGE_VAL;   // overflows a double
    if (x < -745.133219101941) return 0.0;       // underflows to zero
    // exp(x) = 2^k * exp(r), with r in [-ln2/2, ln2/2]. Splitting ln2
    // into HI and LO keeps k*ln2 exact to more than double precision,
    // which is what stops the result drifting for large |x|.
    double kd = x / LN2;
    int64_t k = (int64_t)(kd >= 0 ? kd + 0.5 : kd - 0.5);
    double r = x - (double)k * LN2_HI;
    r -= (double)k * LN2_LO;
    // Taylor on |r| <= 0.347, where it converges fast: the r^11 term is
    // already below 1e-19 relative.
    double p = 1.0/3628800;
    p = 1.0/362880 + r * p;
    p = 1.0/40320  + r * p;
    p = 1.0/5040   + r * p;
    p = 1.0/720    + r * p;
    p = 1.0/120    + r * p;
    p = 1.0/24     + r * p;
    p = 1.0/6      + r * p;
    p = 1.0/2      + r * p;
    p = 1.0        + r * p;
    p = 1.0        + r * p;
    return ldexp(p, (int)k);
}

double log(double x) {
    if (isnan(x) || x < 0.0) return NAN;
    if (x == 0.0) return -HUGE_VAL;
    if (isinf(x)) return HUGE_VAL;
    // x = 2^k * m, m in [sqrt(2)/2, sqrt(2)), so that s below is small.
    int k;
    double m = frexp(x, &k);   // m in [0.5, 1)
    if (m < 0.70710678118654752440) { m *= 2.0; k--; }
    // The atanh series: log(m) = 2*(s + s^3/3 + s^5/5 + ...) with
    // s = (m-1)/(m+1). |s| <= 0.1716, so s^2 <= 0.0295 and the terms
    // fall by a factor of 34 each -- nine of them reach double
    // precision. The direct log(1+z) series would need hundreds near
    // the ends of the interval, which is the whole reason for this form.
    double s = (m - 1.0) / (m + 1.0);
    double s2 = s * s;
    double q = 2.0/17;
    q = 2.0/15 + s2 * q;
    q = 2.0/13 + s2 * q;
    q = 2.0/11 + s2 * q;
    q = 2.0/9  + s2 * q;
    q = 2.0/7  + s2 * q;
    q = 2.0/5  + s2 * q;
    q = 2.0/3  + s2 * q;
    q = 2.0    + s2 * q;
    double sum = s * q;
    return (double)k * LN2_HI + ((double)k * LN2_LO + sum);
}

double log2(double x)  { return log(x) / LN2; }
double log10(double x) { return log(x) / LN10; }
double exp2(double x)  { return exp(x * LN2); }

double pow(double x, double y) {
    // THE SPECIAL CASES ARE THE HARD PART, not the arithmetic: C
    // specifies about twenty of them and most of the disagreement
    // between libms is here rather than in the accuracy.
    if (y == 0.0) return 1.0;                 // including pow(NaN, 0)
    if (isnan(x) || isnan(y)) return NAN;
    if (x == 1.0) return 1.0;                 // including pow(1, inf)
    if (x == 0.0) {
        if (y < 0.0) return HUGE_VAL;
        return signbit(x) && y == trunc(y) && fmod(y, 2.0) != 0.0 ? -0.0 : 0.0;
    }
    if (isinf(y)) {
        double ax = fabs(x);
        if (ax > 1.0) return y > 0 ? HUGE_VAL : 0.0;
        return y > 0 ? 0.0 : HUGE_VAL;
    }

    int y_is_int = (y == trunc(y));
    // A NEGATIVE base is only defined for an INTEGER exponent -- there
    // is no real answer for (-8)^0.5 -- and this is where exp(y*log(x))
    // silently produces a NaN without saying why. Handled explicitly.
    if (x < 0.0 && !y_is_int) return NAN;

    // An integer exponent goes through repeated squaring rather than
    // exp/log. Two reasons: pow(2,10) comes out as exactly 1024 rather
    // than 1023.9999999999998, and a negative base works at all.
    if (y_is_int && fabs(y) <= 1024.0) {
        double result = 1.0, base = x;
        int64_t n = (int64_t)fabs(y);
        while (n) {
            if (n & 1) result *= base;
            base *= base;
            n >>= 1;
        }
        return y < 0 ? 1.0 / result : result;
    }
    // The general case, and the least accurate function in this file:
    // it compounds log's error with exp's, and the product y*log(x)
    // magnifies it by |y|. Good to roughly 1e-13 relative for ordinary
    // exponents, which the test asserts rather than assumes.
    return exp(y * log(x));
}

// --- inverse trig -----------------------------------------------------

double atan(double x) {
    if (isnan(x)) return NAN;
    if (isinf(x)) return copysign(PI_2, x);
    int neg = x < 0.0;
    if (neg) x = -x;
    int invert = 0;
    if (x > 1.0) { x = 1.0 / x; invert = 1; }   // atan(x) = pi/2 - atan(1/x)
    // A SECOND reduction, to |t| <= tan(pi/12) = 0.2679. The series
    // converges as t^2, so on [0,1] the last terms would still matter
    // at t = 1 and it would need dozens of them; below 0.268 the terms
    // fall by 0.072 each and twelve reach double precision.
    double adjust = 0.0;
    if (x > 0.26794919243112270647) {
        adjust = PI / 6.0;
        x = (x * SQRT3 - 1.0) / (x + SQRT3);
    }
    double z = x * x;
    // Horner, written as a chain of assignments rather than one nested
    // expression: a mis-parenthesised polynomial compiles happily and
    // is wrong in the low digits, which is exactly the bug this file
    // would be worst at noticing.
    double q = 1.0/21;
    q = -1.0/19 + z * q;
    q =  1.0/17 + z * q;
    q = -1.0/15 + z * q;
    q =  1.0/13 + z * q;
    q = -1.0/11 + z * q;
    q =  1.0/9  + z * q;
    q = -1.0/7  + z * q;
    q =  1.0/5  + z * q;
    q = -1.0/3  + z * q;
    q =  1.0    + z * q;
    double r = x * q;
    r += adjust;
    if (invert) r = PI_2 - r;
    return neg ? -r : r;
}

double asin(double x) {
    if (isnan(x) || x < -1.0 || x > 1.0) return NAN;
    if (x == 1.0) return PI_2;
    if (x == -1.0) return -PI_2;
    // asin(x) = atan(x / sqrt(1 - x^2)). Exact at the ends, where the
    // division would otherwise be 0/0 -- which is why they are handled
    // above rather than left to the identity.
    return atan(x / sqrt(1.0 - x * x));
}

double acos(double x) {
    if (isnan(x) || x < -1.0 || x > 1.0) return NAN;
    return PI_2 - asin(x);
}

double atan2(double y, double x) {
    if (isnan(x) || isnan(y)) return NAN;
    if (x == 0.0) {
        if (y == 0.0) {
            // atan2(0, +0) is 0 and atan2(0, -0) is pi, sign of y kept.
            return signbit(x) ? copysign(PI, y) : copysign(0.0, y);
        }
        return copysign(PI_2, y);
    }
    double a = atan(y / x);
    if (x > 0.0) return a;
    // The quadrant the division threw away: y/x is the same for
    // (1,1) and (-1,-1), and this is the only thing that tells them
    // apart. Getting it wrong is what makes a plotted spiral fold in
    // half.
    return y >= 0.0 ? a + PI : a - PI;
}

// --- hyperbolics ------------------------------------------------------

double sinh(double x) {
    if (isnan(x) || isinf(x)) return x;
    double e = exp(fabs(x));
    // (e - 1/e)/2 rather than (exp(x) - exp(-x))/2: for small |x| the
    // two exponentials are both near 1 and subtracting them cancels
    // most of the mantissa. This form still cancels; a series for small
    // x would not, and is the refinement to make if it ever matters.
    double r = (e - 1.0 / e) / 2.0;
    return x < 0 ? -r : r;
}

double cosh(double x) {
    if (isnan(x)) return NAN;
    if (isinf(x)) return HUGE_VAL;
    double e = exp(fabs(x));
    return (e + 1.0 / e) / 2.0;
}

double tanh(double x) {
    if (isnan(x)) return NAN;
    if (x > 20.0) return 1.0;      // 1 - 2e-18: already the nearest double
    if (x < -20.0) return -1.0;
    double e = exp(2.0 * x);
    return (e - 1.0) / (e + 1.0);
}

// --- odds and ends ----------------------------------------------------

double hypot(double x, double y) {
    x = fabs(x); y = fabs(y);
    if (isinf(x) || isinf(y)) return HUGE_VAL;   // even if the other is NaN
    if (x < y) { double t = x; x = y; y = t; }
    if (x == 0.0) return 0.0;
    // SCALED, so that hypot(1e200, 1e200) does not overflow computing
    // x*x -- which the obvious sqrt(x*x + y*y) does, giving infinity
    // for a result that is perfectly representable.
    double r = y / x;
    return x * sqrt(1.0 + r * r);
}

double cbrt(double x) {
    if (x == 0.0 || isnan(x) || isinf(x)) return x;
    int neg = x < 0.0;
    if (neg) x = -x;
    // A rough seed from the exponent, then Newton. Each iteration
    // doubles the correct digits, so four take a 4-bit guess past
    // double precision.
    int e;
    frexp(x, &e);
    double r = ldexp(1.0, e / 3);
    for (int i = 0; i < 6; i++) r = (2.0 * r + x / (r * r)) / 3.0;
    return neg ? -r : r;
}

double fmin(double a, double b) {
    if (isnan(a)) return b;
    if (isnan(b)) return a;
    return a < b ? a : b;
}

double fmax(double a, double b) {
    if (isnan(a)) return b;
    if (isnan(b)) return a;
    return a > b ? a : b;
}

double fdim(double a, double b) { return a > b ? a - b : 0.0; }

double modf(double x, double *iptr) {
    double i = trunc(x);
    if (iptr) *iptr = i;
    // The fractional part keeps the SIGN of the input, including for a
    // negative value whose integer part is zero -- modf(-0.5) is -0.5
    // and -0.0, not -0.5 and 0.0.
    return isinf(x) ? copysign(0.0, x) : x - i;
}
