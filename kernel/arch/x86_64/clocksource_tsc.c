// The TSC as a clocksource. Lives in arch/ because it reads a
// control-adjacent CPU register and asks CPUID a question -- neither
// belongs outside this directory (kernel/README.md).
//
// The TSC is the reason this whole interface is worth having: it is
// sub-nanosecond, costs a couple of dozen cycles to read, and needs no
// I/O port. The PIT source it displaces cannot resolve anything shorter
// than 10ms, which is why CPU accounting could not see a client drawing
// one frame.
#include "clocksource.h"
#include "cpuinfo.h"
#include "random_hw.h" // arch_rdtsc()
#include "klog.h"
#include "kfmt.h"
#include "multiboot.h" // the `notsc` boot flag
#include "string.h"   // k_strstr()
#include "driver.h" // DRIVER_REGISTER -- `lsdrv`

// CPUID 8000_0007H, EDX bit 8: INVARIANT TSC -- the counter runs at a
// constant rate regardless of power state and does not stop in deep C
// states.
//
// This gate is the whole reason a rating is not enough. Without an
// invariant TSC the counter's RATE changes underneath you as the CPU
// throttles, so a calibration taken at boot silently stops being true
// and every duration measured with it is wrong by whatever the CPU
// felt like doing -- a failure with no symptom except numbers that do
// not add up. An old machine keeps the PIT, which is coarse and
// correct, and correct beats precise here.
static int has_invariant_tsc(void) {
    uint32_t eax, ebx, ecx, edx;

    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                              : "a"(0x80000000U));
    if (eax < 0x80000007U) return 0; // leaf not implemented at all

    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                              : "a"(0x80000007U));
    return (edx & (1U << 8)) != 0;
}

static uint64_t tsc_cs_read(void) { return arch_rdtsc(); }

static struct clocksource g_tsc_cs = {
    .name   = "tsc",
    .read   = tsc_cs_read,
    .mask   = CLOCKSOURCE_MASK(64),
    .rating = CLOCKSOURCE_RATING_TSC,
};

// `notsc` on the GRUB command line keeps the PIT source, so the coarse
// path stays REACHABLE on a machine where the TSC would otherwise win.
// Same rule as `ata nodma`, `nokaslr` and `nopat`: a fallback nobody can
// execute is a guess, and here the fallback is what every CPU without an
// invariant TSC actually runs. Documented in docs/boot-flags.md.
static int notsc_requested(void) {
    const char *cmdline = multiboot_cmdline();
    return cmdline && k_strstr(cmdline, "notsc") ? 1 : 0;
}

void clocksource_init_tsc(void) {
    // DECLARED BEFORE THE HARDWARE IS LOOKED FOR, so a driver
    // that finds nothing still appears in `lsdrv` -- "compiled
    // in but idle" is the answer somebody is looking for.
    DRIVER_REGISTER("tsc", "clock");
    if (notsc_requested()) {
        klog_write("clocksource: tsc not offered -- notsc on the command line\n");
        return;
    }

    if (!has_invariant_tsc()) {
        klog_write("clocksource: tsc not offered -- no invariant TSC on this CPU\n");
        return;
    }

    struct cpu_info info;
    cpu_info_get(&info);
    if (info.mhz == 0 || info.mhz_source == CPU_MHZ_UNKNOWN) {
        // Calibration never ran or produced nothing. Registering anyway
        // would install a clock whose conversion factor is a guess,
        // which is worse than a coarse clock that is right.
        klog_write("clocksource: tsc not offered -- no calibrated frequency\n");
        return;
    }

    // A one-hour worst case for the mult/shift bound. Nothing waits
    // anywhere near that between reads (the scheduler reads on every
    // context switch), but the bound is what keeps the multiply inside
    // 64 bits, and picking it generously costs only a little precision.
    uint64_t hz = (uint64_t)info.mhz * 1000000ULL;
    clocksource_calc_mult_shift(&g_tsc_cs.mult, &g_tsc_cs.shift, hz, 3600);
    clocksource_register(&g_tsc_cs);
}
