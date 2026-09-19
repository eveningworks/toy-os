// See utween.h. No clock, no allocation, no toolkit: this file must
// compile with a host gcc for tools/utween_hostcheck.py.
#include "utween.h"

fx_t utween_ease_out(fx_t t) {
    if (t <= 0) return 0;
    if (t >= FX_ONE) return FX_ONE;
    fx_t u = FX_ONE - t;
    fx_t u3 = fx_mul(fx_mul(u, u), u);
    return FX_ONE - u3;
}

// Cubic in-out. The halves are mirrored, so f(1/2) = 1/2 exactly and
// the two pieces meet with the same slope -- a seam there is visible
// as a hitch at the midpoint of every animation using it.
fx_t utween_ease_in_out(fx_t t) {
    if (t <= 0) return 0;
    if (t >= FX_ONE) return FX_ONE;
    if (t < FX_HALF) {
        fx_t t3 = fx_mul(fx_mul(t, t), t);
        return 4 * t3;                       // 4t^3
    }
    fx_t u = FX_ONE - t;
    fx_t u3 = fx_mul(fx_mul(u, u), u);
    return FX_ONE - 4 * u3;                  // 1 - 4(1-t)^3
}

static fx_t curve_of(const struct utween *tw, fx_t t) {
    return tw->curve == UTWEEN_EASE_IN_OUT ? utween_ease_in_out(t)
                                           : utween_ease_out(t);
}

void utween_start_curve(struct utween *tw, int from, int to, unsigned dur_ms,
                        unsigned long long now_ns, enum utween_curve curve) {
    tw->t0_ns = now_ns;
    tw->dur_ns = (unsigned long long)dur_ms * 1000000ull;
    tw->from = from;
    tw->to = to;
    tw->active = 1;
    tw->curve = (unsigned char)curve;
}

void utween_start(struct utween *tw, int from, int to, unsigned dur_ms,
                  unsigned long long now_ns) {
    utween_start_curve(tw, from, to, dur_ms, now_ns, UTWEEN_EASE_OUT);
}

// Where the motion is at `now_ns`, with the tween left as it is.
static int value_at(const struct utween *tw, unsigned long long now_ns) {
    if (!tw->active) return tw->to;
    if (tw->dur_ns == 0 || now_ns <= tw->t0_ns) return tw->dur_ns == 0 ? tw->to : tw->from;
    unsigned long long el = now_ns - tw->t0_ns;
    if (el >= tw->dur_ns) return tw->to;
    // Progress in Q16.16. `el` and `dur_ns` fit comfortably: a duration
    // is milliseconds, and the shift happens before the divide.
    fx_t t = (fx_t)((el << FX_SHIFT) / tw->dur_ns);
    fx_t e = curve_of(tw, t);
    // 64-bit product: a screen-sized distance times FX_ONE overflows 32.
    long long d = (long long)(tw->to - tw->from) * (long long)e;
    // Round to nearest with a DIVIDE, not a shift: an arithmetic shift
    // floors, which on a negative distance overshoots `to` by one for a
    // frame (100 -> 0 visited -1). |d| <= |to - from| * FX_ONE, so the
    // quotient never passes the destination.
    long long q = (d + (d >= 0 ? FX_HALF : -FX_HALF)) / FX_ONE;
    return tw->from + (int)q;
}

void utween_retarget(struct utween *tw, int to, unsigned dur_ms,
                     unsigned long long now_ns) {
    int here = value_at(tw, now_ns);
    // KEEPS THE CURVE: a retarget continues the same motion, and
    // switching it to ease-out halfway would be visible as the travel
    // suddenly starting fast again.
    utween_start_curve(tw, here, to, dur_ms, now_ns,
                       (enum utween_curve)tw->curve);
}

int utween_value(struct utween *tw, unsigned long long now_ns) {
    if (!tw->active) return tw->to;
    int v = value_at(tw, now_ns);
    if (tw->dur_ns == 0 || now_ns >= tw->t0_ns + tw->dur_ns) tw->active = 0;
    return v;
}
