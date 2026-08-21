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
// WHAT IS DELIBERATELY ABSENT: sin, cos, tan, exp, log, pow and the
// rest of the transcendentals. Each needs argument reduction and a
// polynomial approximation with an accuracy claim attached, which is a
// real numerical-library project and not a corner of a hobby OS's libc.
// A ported program that calls sin() gets a LINK ERROR naming it, which
// says "this OS does not have this yet" -- the same choice <unistd.h>
// makes about fork(). A stub returning 0 would be worse in every way.
//
// **Not to be confused with kernel/include/api/fixed.h**, which is the
// kernel's Q16.16 fixed-point maths and exists precisely BECAUSE the
// kernel has no floating point. Its angles are in TURNS, not radians.
// The two never meet: fixed.h is ring 0 and this is ring 3.
#include <stdint.h>

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
