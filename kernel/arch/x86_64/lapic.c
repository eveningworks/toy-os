// The Local APIC. See kernel/lapic.h for what this is for and for the
// one trap in it (enabling it moves the 8259's wire).
#include "lapic.h"
#include "acpi.h"
#include "multiboot.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"
#include "cpuinfo.h"
#include "clockevent.h"
#include "timer.h"   // pit_ticks(), PIT_HZ -- the calibration reference
#include "barrier.h" // cpu_relax()

// The MSR that says where the LAPIC is and whether it is enabled. Its
// base field is the same address the MADT reports; they are read from
// different places on purpose (see lapic_init()).
#define IA32_APIC_BASE      0x1B
#define APIC_BASE_BSP       (1u << 8)
#define APIC_BASE_ENABLE    (1u << 11)
#define APIC_BASE_ADDR_MASK 0xFFFFF000u

// Register offsets, in bytes from the base. Every one is 32 bits wide
// and must be accessed as a single aligned dword.
#define LAPIC_REG_ID        0x020
#define LAPIC_REG_VERSION   0x030
#define LAPIC_REG_TPR       0x080
#define LAPIC_REG_EOI       0x0B0
#define LAPIC_REG_SVR       0x0F0
#define LAPIC_REG_LVT_LINT0 0x350
#define LAPIC_REG_LVT_LINT1 0x360
#define LAPIC_REG_LVT_TIMER 0x320
#define LAPIC_REG_TIMER_INIT 0x380
#define LAPIC_REG_TIMER_CUR  0x390
#define LAPIC_REG_TIMER_DIV  0x3E0

#define SVR_ENABLE          (1u << 8)

// LVT delivery modes, in bits 10:8.
#define LVT_DELIVERY_EXTINT (7u << 8)
#define LVT_DELIVERY_NMI    (4u << 8)
#define LVT_MASKED          (1u << 16)
#define LVT_TIMER_PERIODIC  (1u << 17)

// Divide-by-16. The divisor is encoded across bits 3,1,0 with bit 2
// skipped, which is why 16 is 0x3 and not 0x4. Sixteen keeps a 100 Hz
// period inside 32 bits on every bus frequency this will meet while
// leaving the counter coarse enough that calibration is not measuring
// its own overhead.
#define TIMER_DIV_16        0x3

static volatile uint8_t *g_base;
static uint8_t g_id;
static void (*g_vector_handlers[LAPIC_VECTOR_COUNT])(uint64_t *regs);
static uint32_t g_delivered;

static uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile ("wrmsr" :: "c"(msr), "a"((uint32_t)val),
                                 "d"((uint32_t)(val >> 32)));
}

static uint32_t lapic_read(uint32_t reg) {
    return *(volatile uint32_t *)(g_base + reg);
}

static void lapic_write(uint32_t reg, uint32_t value) {
    *(volatile uint32_t *)(g_base + reg) = value;
}

void lapic_init(void) {
    if (g_base) return;   // idempotent, like every other *_init() here

    const char *cmdline = multiboot_cmdline();
    if (cmdline && k_strstr(cmdline, "nomsi")) {
        klog_write("lapic: disabled by `nomsi` -- every device stays on the PIC\n");
        return;
    }
    // CPUID.01H:EDX bit 9. A CPU without it has no LAPIC to enable, and
    // writing the MSR would #GP.
    struct cpu_info ci;
    cpu_info_get(&ci);
    if (!cpu_has_feature(&ci, CPU_WORD_1_EDX, 9)) {
        klog_write("lapic: no APIC on this CPU -- every device stays on the PIC\n");
        return;
    }

    // WHERE IT IS, from the MSR rather than from the MADT. The two agree
    // on every machine that has both, but the MSR is the one the CPU
    // actually decodes, and a machine with no ACPI tables still has it.
    // The MADT's value is reported beside it so a disagreement is
    // visible rather than silently resolved.
    uint64_t base_msr = rdmsr(IA32_APIC_BASE);
    uint64_t phys = base_msr & APIC_BASE_ADDR_MASK;
    const struct acpi_state *acpi = acpi_get_state();
    if (acpi && acpi->lapic_phys && acpi->lapic_phys != phys)
        klog_printf("lapic: MSR says 0x%llx, MADT says 0x%llx -- using the MSR\n",
                    (unsigned long long)phys,
                    (unsigned long long)acpi->lapic_phys);
    if (!phys || phys >= 0x100000000ull) {
        klog_printf("lapic: base 0x%llx is outside the identity map\n",
                    (unsigned long long)phys);
        return;
    }

    // The global enable bit. Clearing it again is architecturally
    // one-way until reset on some parts, which is why nothing here ever
    // turns the LAPIC back off.
    wrmsr(IA32_APIC_BASE, base_msr | APIC_BASE_ENABLE);
    g_base = (volatile uint8_t *)(uintptr_t)phys;   // identity-mapped
    g_id = (uint8_t)(lapic_read(LAPIC_REG_ID) >> 24);

    // Accept every priority. The task-priority register comes out of
    // reset at 0 on real parts and on QEMU, but a firmware that raised
    // it would silently block every vector below its class.
    lapic_write(LAPIC_REG_TPR, 0);

    // Software enable, with the spurious vector. Until this bit is set
    // the LAPIC accepts nothing at all.
    lapic_write(LAPIC_REG_SVR, SVR_ENABLE | LAPIC_SPURIOUS_VECTOR);

    // VIRTUAL WIRE MODE, and this is the line the machine lives or dies
    // by. The CPU's INTR pin is the LAPIC's now, so the 8259 reaches it
    // only through LINT0 -- programmed for ExtINT, the PIC keeps
    // delivering the timer, the keyboard and every legacy line exactly
    // as before. Masked or set to any other delivery mode, they all
    // stop, and the machine appears to hang on the first tick.
    lapic_write(LAPIC_REG_LVT_LINT0, LVT_DELIVERY_EXTINT);
    lapic_write(LAPIC_REG_LVT_LINT1, LVT_DELIVERY_NMI);

    uint32_t ver = lapic_read(LAPIC_REG_VERSION);
    klog_printf("lapic: id %u, version 0x%x, %u LVT entries, at 0x%llx%s\n",
                g_id, ver & 0xFF, ((ver >> 16) & 0xFF) + 1,
                (unsigned long long)phys,
                (base_msr & APIC_BASE_BSP) ? " (BSP)" : "");
    klog_printf("lapic: MSI vectors %u-%u available\n",
                LAPIC_VECTOR_BASE, LAPIC_VECTOR_LAST);
}

int lapic_present(void) { return g_base != 0; }

uint8_t lapic_id(void) { return g_id; }

void lapic_eoi(void) {
    if (g_base) lapic_write(LAPIC_REG_EOI, 0);
}

uint8_t lapic_alloc_vector(void (*handler)(uint64_t *regs)) {
    if (!g_base || !handler) return 0;
    for (int i = 0; i < LAPIC_VECTOR_COUNT; i++) {
        if (g_vector_handlers[i]) continue;
        g_vector_handlers[i] = handler;
        return (uint8_t)(LAPIC_VECTOR_BASE + i);
    }
    return 0;
}

void lapic_free_vector(uint8_t vector) {
    int i = vector - LAPIC_VECTOR_BASE;
    if (i < 0 || i >= LAPIC_VECTOR_COUNT) return;
    g_vector_handlers[i] = 0;
}

void lapic_dispatch_vector(uint8_t vector, uint64_t *regs) {
    int i = vector - LAPIC_VECTOR_BASE;
    if (i < 0 || i >= LAPIC_VECTOR_COUNT) return;
    g_delivered++;
    if (g_vector_handlers[i]) g_vector_handlers[i](regs);
    // THE EOI IS SENT HERE, not left to the handler, for the reason
    // irq.c gives about the PIC's: a forgotten one stops every later
    // interrupt of that class and looks nothing like a missing EOI.
    lapic_eoi();
}

// --- the LAPIC timer -------------------------------------------------
//
// A per-core tick, and the reason this file is part of docs/smp-design.md
// rather than only of the MSI work: the PIT delivers one interrupt for
// the whole machine, so an application processor started in stage 3
// would have nothing to preempt it with.

// How many times the timer counts in a second, at TIMER_DIV_16.
// Measured once; the bus frequency it derives from does not change.
static uint32_t g_timer_rate;
static uint8_t g_timer_vector;
static uint32_t g_timer_ticks;

// How long to count for. Four PIT ticks is 40ms -- long enough that the
// quantisation of a whole-tick reference is under a percent, short
// enough not to be felt at boot.
#define CALIBRATE_PIT_TICKS 4

static int interrupts_enabled(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags));
    return (flags & (1u << 9)) != 0;
}

// Counts down from the maximum across a whole number of PIT ticks. The
// PIT is the only reference that exists this early, and it is exact by
// construction rather than measured.
static uint32_t lapic_timer_calibrate(void) {
    // WITHOUT INTERRUPTS THIS NEVER RETURNS. pit_ticks() advances only
    // from the timer interrupt, so the loops below would spin forever --
    // the deadlock cpuinfo.h describes for the TSC, refused here rather
    // than hit.
    if (!interrupts_enabled()) return 0;

    lapic_write(LAPIC_REG_TIMER_DIV, TIMER_DIV_16);
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED); // count, deliver nothing

    uint64_t edge = pit_ticks();
    while (pit_ticks() == edge) cpu_relax(); // start on a tick boundary

    lapic_write(LAPIC_REG_TIMER_INIT, 0xFFFFFFFFu);
    uint64_t t0 = pit_ticks();
    while (pit_ticks() - t0 < CALIBRATE_PIT_TICKS) cpu_relax();
    uint32_t remaining = lapic_read(LAPIC_REG_TIMER_CUR);
    uint64_t elapsed = pit_ticks() - t0;
    lapic_write(LAPIC_REG_TIMER_INIT, 0);

    // A counter that reached ZERO ran out mid-window, so the count is a
    // floor rather than a measurement -- and believing it would set a
    // silently wrong tick rate, which makes every timeout in the kernel
    // wrong rather than failing anywhere visible.
    uint32_t counted = 0xFFFFFFFFu - remaining;
    if (!elapsed || !counted || !remaining) return 0;
    return (uint32_t)(((uint64_t)counted * PIT_HZ) / elapsed);
}

static void lapic_timer_isr(uint64_t *regs) {
    g_timer_ticks++;
    clockevent_tick(regs);
}

static int lapic_ce_start(uint32_t hz) {
    if (!g_base || !hz) return 0;

    if (!g_timer_rate) g_timer_rate = lapic_timer_calibrate();
    if (!g_timer_rate) return 0;

    uint32_t count = g_timer_rate / hz;
    if (!count) return 0; // asked for a rate faster than the timer counts

    if (!g_timer_vector) {
        g_timer_vector = lapic_alloc_vector(lapic_timer_isr);
        if (!g_timer_vector) return 0;
    }

    lapic_write(LAPIC_REG_TIMER_DIV, TIMER_DIV_16);
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_TIMER_PERIODIC | g_timer_vector);
    // THE INITIAL COUNT IS THE ARMING WRITE, and it must be last: the
    // timer starts the instant it is written, so an LVT still holding a
    // masked or stale vector delivers the first tick somewhere wrong.
    lapic_write(LAPIC_REG_TIMER_INIT, count);

    klog_printf("lapic: timer at %u Hz -- %u counts, %u/s calibrated, vector %u\n",
                hz, count, g_timer_rate, g_timer_vector);
    return 1;
}

static void lapic_ce_stop(void) {
    if (!g_base) return;
    lapic_write(LAPIC_REG_TIMER_INIT, 0);
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED);
}

static const struct clockevent g_lapic_ce = {
    .name    = "lapic-timer",
    .start   = lapic_ce_start,
    .stop    = lapic_ce_stop,
    .rating  = CLOCKEVENT_RATING_LAPIC,
    .per_cpu = 1,
};

void clockevent_init_lapic(void) {
    if (!g_base) return; // no LAPIC, or `nomsi` -- the PIT keeps the tick
    clockevent_register(&g_lapic_ce);
}

uint32_t lapic_timer_rate(void) { return g_timer_rate; }
uint32_t lapic_timer_ticks(void) { return g_timer_ticks; }

int lapic_summary(char *buf, uint32_t cap) {
    if (!g_base || !buf || cap == 0) return 0;
    int claimed = 0;
    for (int i = 0; i < LAPIC_VECTOR_COUNT; i++)
        if (g_vector_handlers[i]) claimed++;
    k_snprintf(buf, cap, "id %u, %d of %d MSI vector(s) claimed, %u delivered",
               g_id, claimed, LAPIC_VECTOR_COUNT, (unsigned)g_delivered);
    return 1;
}
