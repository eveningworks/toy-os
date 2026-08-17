// The WM's slow-frame watchdog: times one wm_run() iteration, splits it
// by phase, and logs any frame that took longer than a threshold.
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
// loop. This times only the work AFTER the `hlt`, so:
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
// RESOLUTION: pit_ticks() is 100 Hz, so a frame is measured to 10 ms and
// anything faster reads as 0. That is deliberate -- the question is
// "hundreds of milliseconds or not", and a coarse counter that is always
// available beats plumbing a finer one through a header for it (see
// clocksource.h if this ever needs sub-tick precision).
//
// THE TRAP: wmwd_phase() names the phase that is ABOUT TO START, so the
// interval it measures belongs to the PREVIOUS one. Attributing it
// forward instead silently blames each slow phase on its neighbour.
#include "wm_internal.h"
#include "kapi.h" // pit_ticks, PIT_HZ, klog_printf

#define MS_PER_TICK (1000 / PIT_HZ)

// 150 ms: comfortably above any ordinary frame (a full 1280x720 repaint
// measures 0-1 ticks here) and far below the multi-second stall this was
// written to catch, so an ordinary desktop logs nothing at all.
#define WMWD_DEFAULT_MS 150

static uint32_t g_threshold_ms = WMWD_DEFAULT_MS;

static uint64_t g_frame_start;   // tick at the top of this frame
static uint64_t g_phase_start;   // tick the current phase began at
static const char *g_phase;      // what is running now
static const char *g_worst;      // the slowest phase seen this frame
static uint64_t g_worst_ticks;

// Counters, so `gui watchdog` can say whether it has ever fired rather
// than leaving "no output" ambiguous between "nothing slow happened" and
// "this is switched off".
static uint32_t g_slow_frames;
static uint32_t g_peak_ms;

void wmwd_set_threshold_ms(uint32_t ms) { g_threshold_ms = ms; }
uint32_t wmwd_threshold_ms(void) { return g_threshold_ms; }
uint32_t wmwd_slow_frames(void) { return g_slow_frames; }
uint32_t wmwd_peak_ms(void) { return g_peak_ms; }

void wmwd_frame_begin(void) {
    g_frame_start = g_phase_start = pit_ticks();
    g_phase = "start";
    g_worst = "start";
    g_worst_ticks = 0;
}

void wmwd_phase(const char *name) {
    uint64_t now = pit_ticks();
    uint64_t d = now - g_phase_start;
    // >= rather than >, so a frame whose phases all measure 0 still
    // names one rather than reporting "start" for everything.
    if (d >= g_worst_ticks) { g_worst_ticks = d; g_worst = g_phase; }
    g_phase_start = now;
    g_phase = name;
}

void wmwd_frame_end(void) {
    if (!g_threshold_ms) return;   // off
    wmwd_phase("end");             // closes the last phase, same rule as above
    uint64_t total = pit_ticks() - g_frame_start;
    uint32_t ms = (uint32_t)(total * MS_PER_TICK);
    if (ms > g_peak_ms) g_peak_ms = ms;
    if (ms < g_threshold_ms) return;
    g_slow_frames++;
    // Not rate-limited on purpose: during a real stall every frame is
    // evidence, and the threshold is what keeps a healthy desktop silent.
    klog_printf("wm: SLOW FRAME %u ms -- worst phase '%s' %u ms\n",
                ms, g_worst, (uint32_t)(g_worst_ticks * MS_PER_TICK));
}
