#ifndef FIXED_H
#define FIXED_H

#include <stdint.h>

// Q16.16 fixed-point arithmetic and trigonometry.
//
// Exists because this kernel has no floating point available to it at
// all -- `CFLAGS` builds with `-mno-sse`, deliberately (see fpu.h: an
// FP-enabled kernel would need an FXSAVE on every interrupt vector,
// not just where the scheduler swaps processes). Ring-3 code CAN use
// SSE, but geometry that is shared between both sides has to work in
// the kernel too, so it is integer-only.
//
// ANGLES ARE MEASURED IN TURNS, not radians: FX_ONE is a full circle.
// That is not an affectation -- it removes pi from the API entirely.
// A quarter turn is FX_ONE/4 exactly, wrapping is a bitwise AND rather
// than a range reduction with rounding error, and the sine table index
// is just the high bits of the angle. Radians would have introduced an
// irrational constant into every call for no gain.

typedef int32_t fx_t;

#define FX_SHIFT 16
#define FX_ONE   (1 << FX_SHIFT)
#define FX_HALF  (FX_ONE / 2)

// A MULTIPLY, not `v << FX_SHIFT`: shifting a negative value left is
// undefined in C (GCC defines it; UBSAN=1 and other compilers do not).
// Same result for every |v| < 32768; past that the shift wrapped
// silently, where the multiply's overflow is UB that UBSAN=1 reports.
static inline fx_t fx_from_int(int v)  { return (fx_t)(v * FX_ONE); }
static inline int   fx_to_int(fx_t v)  { return (int)(v >> FX_SHIFT); }

// Rounds to nearest rather than truncating -- truncation biases every
// coordinate half a pixel toward the origin, which is visible as a
// rotating shape drifting off-centre.
static inline int fx_round(fx_t v) { return (int)((v + FX_HALF) >> FX_SHIFT); }

// The fractional part, always in [0, FX_ONE) even for negatives --
// which is what anti-aliasing coverage needs.
static inline fx_t fx_frac(fx_t v) { return v & (FX_ONE - 1); }

static inline fx_t fx_mul(fx_t a, fx_t b) {
    return (fx_t)(((int64_t)a * (int64_t)b) >> FX_SHIFT);
}

static inline fx_t fx_div(fx_t a, fx_t b) {
    if (b == 0) return 0; // no traps here; a caller dividing by zero gets 0
    return (fx_t)(((int64_t)a * FX_ONE) / b);   // not <<: see fx_from_int()
}

// sin/cos of an angle in TURNS. Any input is valid -- the angle wraps,
// so an ever-increasing rotation counter never needs clamping.
fx_t fx_sin(fx_t turns);
fx_t fx_cos(fx_t turns);

#endif
