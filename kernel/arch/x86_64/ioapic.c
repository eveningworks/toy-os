// The I/O APIC -- see kernel/include/kernel/ioapic.h.
//
// One controller is driven (the one whose GSI base is 0); a second is
// counted and left alone, since every line this kernel unmasks is
// below 24. Its registers are two memory-mapped dwords: an index at +0
// and a window at +0x10, exactly the 82093AA's.
#include "ioapic.h"
#include "irq.h"
#include "lapic.h"
#include "acpi.h"
#include "multiboot.h"
#include "string.h"
#include "klog.h"
#include "kfmt.h"
#include "errno.h"
#include "ktest.h"

#define REG_ID     0x00
#define REG_VER    0x01
#define REG_REDTBL 0x10

#define RTE_MASKED   (1u << 16)
#define RTE_LEVEL    (1u << 15)
#define RTE_LOW      (1u << 13)

static volatile uint32_t *g_base;
static uint32_t g_gsi_base;
static int g_inputs;

static uint32_t rd(uint32_t reg) { g_base[0] = reg; return g_base[4]; }
static void wr(uint32_t reg, uint32_t v) { g_base[0] = reg; g_base[4] = v; }

int ioapic_present(void) { return g_base != 0; }
int ioapic_inputs(void) { return g_inputs; }

int ioapic_gsi_for_isa(uint8_t irq, uint32_t *gsi, int *level, int *low) {
    if (irq >= 16 || irq == 2) return 0;
    const struct acpi_state *s = acpi_get_state();
    *gsi = irq; *level = 0; *low = 0;
    for (uint32_t i = 0; s && i < s->iso_count; i++) {
        const struct acpi_iso *iso = &s->isos[i];
        if (iso->bus != 0 || iso->irq != irq) continue;
        *gsi = iso->gsi;
        *level = ACPI_ISO_TRIGGER(iso->flags) == ACPI_ISO_LEVEL;
        *low = ACPI_ISO_POLARITY(iso->flags) == ACPI_ISO_ACTIVE_LOW;
        break;
    }
    return 1;
}

static int in_range(uint32_t gsi) {
    return g_base && gsi >= g_gsi_base && gsi < g_gsi_base + (uint32_t)g_inputs;
}

int ioapic_route(uint32_t gsi, uint8_t vector, int level, int low) {
    if (!in_range(gsi)) return -ERANGE;
    uint32_t idx = REG_REDTBL + 2 * (gsi - g_gsi_base);
    // High dword first: the destination must be in place before the
    // entry can fire, and it fires the moment the mask bit clears.
    wr(idx + 1, (uint32_t)lapic_id() << 24);
    wr(idx, (uint32_t)vector | (level ? RTE_LEVEL : 0) | (low ? RTE_LOW : 0) | RTE_MASKED);
    return 0;
}

void ioapic_mask(uint32_t gsi) {
    if (!in_range(gsi)) return;
    uint32_t idx = REG_REDTBL + 2 * (gsi - g_gsi_base);
    wr(idx, rd(idx) | RTE_MASKED);
}

void ioapic_unmask(uint32_t gsi) {
    if (!in_range(gsi)) return;
    uint32_t idx = REG_REDTBL + 2 * (gsi - g_gsi_base);
    wr(idx, rd(idx) & ~RTE_MASKED);
}

uint32_t ioapic_entry(uint32_t gsi) {
    if (!in_range(gsi)) return 0;
    return rd(REG_REDTBL + 2 * (gsi - g_gsi_base));
}

static int word_on_cmdline(const char *word) {
    const char *cmdline = multiboot_cmdline();
    if (!cmdline) return 0;
    uint32_t n = (uint32_t)k_strlen(word);
    for (const char *p = cmdline; (p = k_strstr(p, word)) != 0; p += n) {
        if (p != cmdline && p[-1] != ' ') continue;
        if (p[n] == 0 || p[n] == ' ') return 1;
    }
    return 0;
}

int ioapic_init(void) {
    if (g_base) return 1;
    if (!lapic_present()) return 0;   // `nomsi`, or no APIC: the PIC path, and it says so itself
    if (word_on_cmdline("noioapic")) {
        klog_write("ioapic: disabled by `noioapic` -- every line stays on the PIC\n");
        return 0;
    }
    const struct acpi_state *s = acpi_get_state();
    if (!s || s->ioapic_count == 0) {
        klog_write("ioapic: none in the MADT -- every line stays on the PIC\n");
        return 0;
    }
    const struct acpi_ioapic *io = 0;
    for (uint32_t i = 0; i < s->ioapic_count && i < ACPI_IOAPIC_MAX; i++)
        if (s->ioapics[i].gsi_base == 0) io = &s->ioapics[i];
    if (!io || !io->phys) {
        klog_write("ioapic: no controller at GSI base 0 -- every line stays on the PIC\n");
        return 0;
    }
    g_base = (volatile uint32_t *)(uintptr_t)io->phys;   // identity-mapped, below 4 GiB
    g_gsi_base = 0;
    uint32_t ver = rd(REG_VER);
    g_inputs = (int)(((ver >> 16) & 0xFF) + 1);
    if (g_inputs > IRQ_MAX) g_inputs = IRQ_MAX;   // irq.c's table bounds what can be routed
    for (int i = 0; i < g_inputs; i++) {
        wr(REG_REDTBL + 2 * (uint32_t)i, RTE_MASKED);
        wr(REG_REDTBL + 2 * (uint32_t)i + 1, 0);
    }
    klog_printf("ioapic: id %u at 0x%x, %d inputs, version 0x%x%s\n",
                (unsigned)(rd(REG_ID) >> 24) & 0xF, io->phys, g_inputs, ver & 0xFF,
                s->ioapic_count > 1 ? " (a second controller is left alone)" : "");

    // Every line the PIC was already delivering moves over, then the
    // PIC is silenced -- see irq.c.
    irq_switch_to_ioapic();
    return 1;
}

// --- KTESTs ------------------------------------------------------------

KTEST("ioapic", "an ISA line without an override keeps its number, edge and high") {
    uint32_t gsi = 99; int level = 1, low = 1;
    KTEST_ASSERT(ioapic_gsi_for_isa(4, &gsi, &level, &low));   // COM1: no PC overrides it
    KTEST_ASSERT_EQ(gsi, 4);
    KTEST_ASSERT(!level && !low);
    KTEST_ASSERT(!ioapic_gsi_for_isa(2, &gsi, &level, &low));   // the cascade is not a line
}

KTEST("ioapic", "every unmasked line has a live redirection entry with its vector") {
    if (!ioapic_present()) KTEST_SKIP("the machine is on the PIC");
    int live = 0;
    for (uint8_t irq = 0; irq < IRQ_MAX; irq++) {
        if (!irq_is_unmasked(irq)) continue;
        uint32_t gsi = irq; int level, low;
        if (irq < 16 && !ioapic_gsi_for_isa(irq, &gsi, &level, &low)) continue;
        uint32_t e = ioapic_entry(gsi);
        KTEST_ASSERT_EQ(e & 0xFF, irq_vector(irq));
        KTEST_ASSERT(!(e & RTE_MASKED));
        live++;
    }
    KTEST_ASSERT(live >= 3);   // the tick, the keyboard, the serial console at least
}
