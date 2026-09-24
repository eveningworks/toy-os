// The ACPI PM timer as a clocksource -- Linux's `acpi_pm`.
//
// A 3.579545 MHz counter in the chipset, named by the FADT, that runs
// whatever the CPU is doing and needs no calibration. That makes it the
// clock for a machine with no invariant TSC -- which includes every
// plain QEMU TCG guest, where `+invtsc` cannot be had at all -- and the
// thing that lets such a machine stop its tick when idle: the PIT
// source only advances FROM the tick.
//
// THE COUNTER IS 24 BITS ON MOST CHIPSETS AND WRAPS IN 4.7 SECONDS.
// The accumulate-on-read core handles the wrap only if it is read at
// least that often, which is what clocksource_max_idle_ns() tells a
// tickless idle.
#include "clocksource.h"
#include "acpi.h"
#include "io.h"
#include "klog.h"
#include "driver.h"
#include "kfmt.h"

DRIVER_DECLARE("acpi_pm", "clock", "ACPI PM timer, 3.579545 MHz");

#define ACPI_PM_HZ 3579545u

static uint16_t g_port;

static uint64_t acpi_pm_read(void) { return inl(g_port); }

static struct clocksource g_acpi_pm_cs = {
    .name   = "acpi_pm",
    .read   = acpi_pm_read,
    .rating = CLOCKSOURCE_RATING_ACPI_PM,
    .irq_independent = 1,
};

void clocksource_init_acpi_pm(void) {
    const struct acpi_state *s = acpi_get_state();
    if (!s->pm_tmr || s->pm_tmr > 0xFFFF) {
        klog_write("clocksource: acpi_pm not offered -- the FADT names no PM timer\n");
        return;
    }
    g_port = (uint16_t)s->pm_tmr;
    g_acpi_pm_cs.mask = CLOCKSOURCE_MASK(s->pm_tmr_bits == 32 ? 32 : 24);

    // A counter that does not move is a stopped clock, and installing
    // one over the PIT would freeze time rather than refine it.
    uint32_t a = inl(g_port);
    int moved = 0;
    for (int i = 0; i < 100000 && !moved; i++) moved = inl(g_port) != a;
    if (!moved) {
        klog_printf("clocksource: acpi_pm not offered -- port 0x%x never advanced\n", g_port);
        return;
    }

    clocksource_calc_mult_shift(&g_acpi_pm_cs.mult, &g_acpi_pm_cs.shift, ACPI_PM_HZ, 5);
    clocksource_register(&g_acpi_pm_cs);
}
