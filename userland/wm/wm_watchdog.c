// The WM's slow-frame watchdog: times one wm_run() iteration, splits it
// by phase, logs any frame that took longer than a threshold, and keeps
// a distribution of frame work and of how late the wait returned.
//
// WHY THIS EXISTS
// ---------------
// "The desktop freezes for a few seconds" is a report nothing here could
// answer. The GUI test tools measure the WM from OUTSIDE, over the
// serial console -- and a WM that is busy cannot answer a `gui` command,
// so from the host side a guest that is genuinely working and a guest
// the HOST has descheduled look identical: both are silence on a wire.
//
// The one measurement that separates them has to be taken inside the
// loop. Frame WORK times only what happens after the wait, so:
//
//   a slow frame IS logged  -> the WM really did that much work, and the
//                              worst phase names which part
//   nothing is logged, yet
//   the user saw a freeze   -> the loop was not running at all, i.e. the
//                              stall is below us (host scheduling, the
//                              display backend, an emulator's fsync)
//
// A negative result is therefore just as informative as a positive one,
// which is the property worth preserving if this is ever changed.
//
// WAKE OVERSHOOT IS THE OTHER HALF, AND FRAME WORK CANNOT SEE IT. When
// another process holds the CPU -- every syscall here runs with
// interrupts off, so a disk write stops the machine until it completes
// -- the WM does no work at all and every frame it eventually runs looks
// fast. What moves is the gap between the wait it ASKED for and the one
// it got. That is cyclictest's measurement, and it is why the park is
// bracketed rather than merely excluded.
//
// RESOLUTION: sys_monotonic_ns(), because the TSC clocksource resolves
// to nanoseconds (clocksource.h: rating 300 against the PIT's 110) and a
// desktop frame is invisible to sys_ticks()'s 10 ms. It costs no extra
// syscalls -- sys_ticks() was one too.
//
// THE TRAP: wmwd_phase() names the phase that is ABOUT TO START, so the
// interval it measures belongs to the PREVIOUS one. Attributing it
// forward instead silently blames each slow phase on its neighbour.
#include "wm_internal.h"
#include "kapi.h" // wm_logf
#include "rt/sys.h"
#include "wm/wm_log.h"

// 150 ms: comfortably above any ordinary frame (a full 1280x720 repaint
// measures well under a millisecond) and far below the multi-second
// stall this was written to catch, so an ordinary desktop logs nothing.
#define WMWD_DEFAULT_MS 150

static uint32_t g_threshold_ms = WMWD_DEFAULT_MS;

static uint64_t g_frame_start;   // ns at the top of this frame
static uint64_t g_phase_start;   // ns the current phase began at
static const char *g_phase;      // what is running now
static const char *g_worst;      // the slowest phase seen this frame
static uint64_t g_worst_ns;

static uint64_t g_park_start;    // ns the wait began, 0 when not parked
static uint32_t g_park_asked_ms;

// Counters, so `gui watchdog` can say whether it has ever fired rather
// than leaving "no output" ambiguous between "nothing slow happened" and
// "this is switched off".
static uint32_t g_slow_frames;
static uint32_t g_peak_ms;

static struct wmwd_dist g_work, g_wake;

void wmwd_dist_add(struct wmwd_dist *d, uint64_t us) {
    d->n++;
    d->sum_us += us;
    if (us > d->max_us) d->max_us = us;
    int b = us ? 63 - __builtin_clzll(us) : 0;
    if (b >= WMWD_BUCKETS) b = WMWD_BUCKETS - 1;
    d->bucket[b]++;
}

void wmwd_set_threshold_ms(uint32_t ms) { g_threshold_ms = ms; }
uint32_t wmwd_threshold_ms(void) { return g_threshold_ms; }
uint32_t wmwd_slow_frames(void) { return g_slow_frames; }
uint32_t wmwd_peak_ms(void) { return g_peak_ms; }
const struct wmwd_dist *wmwd_work(void) { return &g_work; }
const struct wmwd_dist *wmwd_wake(void) { return &g_wake; }

void wmwd_reset(void) {
    struct wmwd_dist zero = {0};
    g_work = g_wake = zero;
    g_slow_frames = 0;
    g_peak_ms = 0;
}

void wmwd_park_begin(uint32_t asked_ms) {
    g_park_asked_ms = asked_ms;
    g_park_start = sys_monotonic_ns();
}

void wmwd_frame_begin(void) {
    uint64_t now = sys_monotonic_ns();
    if (g_park_start) {
        // A wait cut short by an event overshoots by nothing, which is
        // the healthy case and must not read as negative.
        uint64_t slept_us = (now - g_park_start) / 1000;
        uint64_t asked_us = (uint64_t)g_park_asked_ms * 1000;
        wmwd_dist_add(&g_wake, slept_us > asked_us ? slept_us - asked_us : 0);
        g_park_start = 0;
    }
    g_frame_start = g_phase_start = now;
    g_phase = "start";
    g_worst = "start";
    g_worst_ns = 0;
}

void wmwd_phase(const char *name) {
    uint64_t now = sys_monotonic_ns();
    uint64_t d = now - g_phase_start;
    // >= rather than >, so a frame whose phases all measure 0 still
    // names one rather than reporting "start" for everything.
    if (d >= g_worst_ns) { g_worst_ns = d; g_worst = g_phase; }
    g_phase_start = now;
    g_phase = name;
}

void wmwd_frame_end(void) {
    wmwd_phase("end");             // closes the last phase, same rule as above
    uint64_t us = (sys_monotonic_ns() - g_frame_start) / 1000;
    wmwd_dist_add(&g_work, us);

    uint32_t ms = (uint32_t)(us / 1000);
    if (ms > g_peak_ms) g_peak_ms = ms;
    if (!g_threshold_ms || ms < g_threshold_ms) return;   // 0 silences the log
    g_slow_frames++;
    // Not rate-limited on purpose: during a real stall every frame is
    // evidence, and the threshold is what keeps a healthy desktop silent.
    wm_logf("wm: SLOW FRAME %u ms -- worst phase '%s' %u ms\n",
                ms, g_worst, (uint32_t)(g_worst_ns / 1000000));
}
