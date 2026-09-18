#ifndef ULIB_UTWEEN_H
#define ULIB_UTWEEN_H

// An integer that moves from one value to another over a fixed time,
// eased -- the one interpolator the desktop has, shared by the toolkit
// (a scroll that glides) and the window manager (an effect that
// animates). Q16.16 over fixed.h; no floating point exists here.
//
// TIME IS THE CALLER'S. Every call takes `now_ns` (sys_monotonic_ns()
// in the guest) rather than reading a clock, so the same code runs
// under a host compiler for tools/utween_hostcheck.py, and so a caller
// that has already read the clock this frame does not read it again
// per tween. Pace by TIME, never by frame count: a frame-paced motion
// changes speed with the load on the machine.
//
// EASE-OUT, not linear: starts at full speed and settles, which is what
// every desktop uses for a scroll or a window (Qt's OutCubic, CSS
// ease-out). A linear motion of the same length reads as mechanical.

#include <stdint.h>
#include "fixed.h"

struct utween {
    unsigned long long t0_ns;   // when it started
    unsigned long long dur_ns;  // how long it takes; 0 means already there
    int from, to;
    int active;                 // 1 until utween_value() has returned `to` past the end
};

// Start moving from `from` to `to` over `dur_ms`, beginning at `now_ns`.
void utween_start(struct utween *tw, int from, int to, unsigned dur_ms,
                  unsigned long long now_ns);

// Change the destination of a tween in flight WITHOUT a jump: the motion
// continues from wherever it is now, over a fresh `dur_ms`. On an idle
// tween this is utween_start() from its resting value. What a wheel
// does when a second notch arrives mid-glide.
void utween_retarget(struct utween *tw, int to, unsigned dur_ms,
                     unsigned long long now_ns);

// The value at `now_ns`. Exactly `to` from the end onwards, and the
// tween goes inactive the first time it answers that -- so a caller
// polling until !active always draws the final value once.
int utween_value(struct utween *tw, unsigned long long now_ns);

static inline int utween_active(const struct utween *tw) { return tw->active; }

// The curve itself, exposed for a caller that eases something that is
// not an int: `t` in [0, FX_ONE] -> [0, FX_ONE], cubic ease-out.
fx_t utween_ease_out(fx_t t);

#endif // ULIB_UTWEEN_H
