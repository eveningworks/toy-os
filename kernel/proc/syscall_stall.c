// How long each syscall held the CPU, as a per-syscall histogram.
//
// THE MEASUREMENT IS THE HANDLER'S DURATION, and that is the stall it
// imposed rather than the latency the caller saw. Interrupts are off for
// the whole handler, so while it runs no other process is scheduled, no
// timer tick lands and a compositor parked on a frame deadline simply
// wakes late -- which is what `gui latency`'s wake distribution counts
// from the other end. A syscall that PARKS its caller still returns here
// promptly (scheduler_block_current() marks the slot and returns; the
// switch happens on the way out of the trap), so a blocking call
// contributes the work it did, not the time its caller slept.
//
// **IT TIMES WITH rdtsc, NOT WITH clocksource_now_ns(), AND THAT IS THE
// WHOLE REASON THIS FILE HAS A CLOCK OF ITS OWN.** The default
// clocksource is the PIT, whose read is pit_ticks() -- a counter the
// timer INTERRUPT increments. Interrupts are off for exactly the stretch
// being measured, so that counter stands still and every duration comes
// out as zero: not coarse, structurally blind, and reported as a machine
// with no stalls at all. The TSC keeps counting with IF clear, costs a
// couple of dozen cycles and no I/O port, which is why ftrace's default
// trace clock is the same register.
//
// The cost is that a NON-INVARIANT TSC changes rate as the CPU throttles,
// so a duration here is accurate to whatever the CPU was doing. That is
// accepted rather than fixed: this measures millisecond-scale stalls, and
// refusing to run on every machine without an invariant TSC would leave
// the default QEMU boot -- where the work actually gets debugged --
// with no instrument.
#include "syscall_stall.h"
#include "random_hw.h" // arch_rdtsc()
#include "cpuinfo.h"
#include "klog.h"
#include "string.h"
#include <stddef.h>

static int g_on;
static uint32_t g_mhz;  // TSC ticks per microsecond; 0 until armed
static struct syscall_stall_info g_stall[SYSCALL_STALL_MAX];

uint64_t syscall_stall_begin(void) {
    if (!g_on) return 0;
    uint64_t t = arch_rdtsc();
    return t ? t : 1; // 0 is the "not armed" sentinel
}

void syscall_stall_record_us(int nr, uint64_t us) {
    if (nr < 0 || nr >= SYSCALL_STALL_MAX) return;
    struct syscall_stall_info *s = &g_stall[nr];
    s->n++;
    s->sum_us += us;
    if (us > s->max_us) s->max_us = us;
    int b = us ? 63 - __builtin_clzll(us) : 0;
    if (b >= QUERY_SYSCALL_STALL_BUCKETS) b = QUERY_SYSCALL_STALL_BUCKETS - 1;
    s->bucket[b]++;
}

void syscall_stall_end(int nr, uint64_t t0) {
    if (!t0) return;
    uint64_t now = arch_rdtsc();
    // A backwards delta is not a measurement. It should not happen on
    // one core, and treating a wrapped or migrated read as an enormous
    // stall would put a bogus row at the top of the report -- which is
    // the one row anybody reads.
    if (now <= t0) return;
    syscall_stall_record_us(nr, (now - t0) / g_mhz);
}

void syscall_stall_reset(void) {
    k_memset(g_stall, 0, sizeof g_stall);
}

int syscall_stall_set(int on) {
    on = on ? 1 : 0;
    if (on && !g_on) {
        // The frequency is READ AT ARM rather than at every call, and
        // its absence is a REFUSAL rather than a divide by zero or a
        // silent 1 MHz. cpu_info's calibration is RDTSC against the
        // PIT at boot; a machine where it did not run cannot convert
        // ticks to time and must say so instead of reporting numbers.
        struct cpu_info info;
        cpu_info_get(&info);
        if (info.mhz == 0 || info.mhz_source == CPU_MHZ_UNKNOWN) {
            klog_write(KLOG_ERR "syscall_stall: refused -- no calibrated TSC frequency\n");
            return 0;
        }
        g_mhz = info.mhz;
        syscall_stall_reset(); // arming from off zeroes; see /bin/stalls' `reset`
    }
    g_on = on;
    return 1;
}

int syscall_stall_get(void) { return g_on; }

uint32_t syscall_stall_mhz(void) { return g_mhz; }

int syscall_stall_info(int nr, struct syscall_stall_info *out) {
    if (nr < 0 || nr >= SYSCALL_STALL_MAX || !g_stall[nr].n) return 0;
    *out = g_stall[nr];
    return 1;
}
