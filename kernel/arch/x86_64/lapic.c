// The Local APIC. See kernel/lapic.h for what this is for and for the
// one trap in it (enabling it moves the 8259's wire).
#include "lapic.h"
#include "acpi.h"
#include "multiboot.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"
#include "cpuinfo.h"

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

#define SVR_ENABLE          (1u << 8)

// LVT delivery modes, in bits 10:8.
#define LVT_DELIVERY_EXTINT (7u << 8)
#define LVT_DELIVERY_NMI    (4u << 8)
#define LVT_MASKED          (1u << 16)

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

int lapic_summary(char *buf, uint32_t cap) {
    if (!g_base || !buf || cap == 0) return 0;
    int claimed = 0;
    for (int i = 0; i < LAPIC_VECTOR_COUNT; i++)
        if (g_vector_handlers[i]) claimed++;
    k_snprintf(buf, cap, "id %u, %d of %d MSI vector(s) claimed, %u delivered",
               g_id, claimed, LAPIC_VECTOR_COUNT, (unsigned)g_delivered);
    return 1;
}
