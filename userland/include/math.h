#ifndef ULIB_MATH_H
#define ULIB_MATH_H

// The ALGEBRAIC half of <math.h>, and deliberately no more.
//
// WHAT IS HERE: functions that are one machine instruction, a bit
// twiddle, or three lines of exact arithmetic -- sqrt, fabs, floor,
// ceil, trunc, round, fmod, copysign, ldexp, frexp, and the
// classification macros. Every one of them is exact (or, for sqrt,
// correctly rounded by the hardware), so there is nothing to get subtly
// wrong and nothing to document about accuracy.
//
// WHAT IS APPROXIMATE: sin, cos, tan, the inverse trig, exp, log, pow
// and the hyperbolics, in userland/libc/math_trig.c. Each is argument
// reduction plus a published minimax polynomial, and each is CHECKED
// AGAINST AN INDEPENDENT IMPLEMENTATION to 1e-12 relative
// (userland/tests/libm_test.c). That is short of glibc's sub-ulp
// accuracy and far more than enough to draw a circle. pow() is the
// weakest, since it compounds log's error with exp's; the test says so
// and asserts a looser bound for it.
//
// Still absent, and these are omissions of the ordinary kind rather
// than decisions: the long-double forms, the float forms (sinf and
// friends), lgamma/tgamma/erf, and the C99 rounding-mode functions.
//
// **Not to be confused with kernel/include/api/fixed.h**, which is the
// kernel's Q16.16 fixed-point maths and exists precisely BECAUSE the
// kernel has no floating point. Its angles are in TURNS, not radians.
// The two never meet: fixed.h is ring 0 and this is ring 3.
#include <stdint.h>

// The X/Open constants. Not in C99 -- glibc, musl and the BSDs all
// provide them anyway, and tolibc's bar is COMPLETENESS rather than
// this project's usual second-real-caller rule (docs/libc-design.md),
// so the whole conventional set is here rather than the one entry that
// happened to be wanted first.
#define M_E        2.7182818284590452354
#define M_LOG2E    1.4426950408889634074
#define M_LOG10E   0.43429448190325182765
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_1_PI     0.31830988618379067154
#define M_2_PI     0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.70710678118654752440

#define HUGE_VAL (__builtin_huge_val())
#define INFINITY (__builtin_inff())
#define NAN      (__builtin_nanf(""))

// Bit tests, not comparisons: a comparison is something an optimiser is
// entitled to reason about, and these have to keep working regardless.
#define isnan(x)      __builtin_isnan(x)
#define isinf(x)      __builtin_isinf(x)
#define isfinite(x)   __builtin_isfinite(x)
#define signbit(x)    __builtin_signbit(x)

// sqrtsd is a single SSE instruction and is correctly rounded by the
// hardware, so this is not an approximation of anything.
static inline double sqrt(double x)  { return __builtin_sqrt(x); }
static inline double fabs(double x)  { return __builtin_fabs(x); }
static inline double copysign(double x, double y) { return __builtin_copysign(x, y); }

// --- the transcendentals (userland/libc/math_trig.c) -----------------

double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
// The quadrant-aware inverse tangent: atan(y/x) cannot tell (1,1) from
// (-1,-1) and this can, which is why every polar conversion uses it.
double atan2(double y, double x);
double exp(double x);
double exp2(double x);
double log(double x);
double log2(double x);
double log10(double x);
// An INTEGER exponent goes through repeated squaring, so pow(2,10) is
// exactly 1024 and a negative base works. Everything else is
// exp(y*log(x)) and is the least accurate function here.
double pow(double x, double y);
double sinh(double x);
double cosh(double x);
double tanh(double x);
// sqrt(x*x + y*y) computed SCALED, so it does not overflow for values
// whose result is perfectly representable.
double hypot(double x, double y);
double cbrt(double x);
double fmin(double a, double b);
double fmax(double a, double b);
double fdim(double a, double b);
// Splits into integer and fractional parts; the fraction keeps the
// sign, so modf(-0.5) gives -0.5 and -0.0.
double modf(double x, double *iptr);

// --- the exact ones (userland/libc/math.c) ---------------------------

double floor(double x);
double ceil(double x);
double trunc(double x);
double round(double x);
double fmod(double x, double y);
// x * 2^e, and the reverse: frexp writes the exponent and returns a
// mantissa in [0.5, 1).
double ldexp(double x, int e);
double frexp(double x, int *e);

#endif
