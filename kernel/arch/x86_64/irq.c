// Hardware-IRQ registration and dispatch -- see kernel/include/kernel/irq.h.
//
// EOI is sent ONCE, here, after the whole chain has run -- and to the
// controller that delivered the line: the 8259 until ioapic_init(), the
// LAPIC after. A handler that sent its own would retire the interrupt
// early for every handler after it in the chain.
#include "irq.h"
#include "pic.h"
#include "lapic.h"
#include "ioapic.h"
#include "io.h"
#include "klog.h"
#include "kfmt.h"
#include "errno.h"

// Four per line is the PCI reality (INTA#-INTD# rotate across slots, so
// a handful of functions can land on one line) plus room for the one
// legacy owner.
#define MAX_HANDLERS_PER_IRQ 4

static irq_handler_fn handlers[IRQ_MAX][MAX_HANDLERS_PER_IRQ];
static uint32_t g_unmasked;          // one bit per line, whichever controller
static int g_ioapic;                 // the I/O APIC owns delivery
static uint8_t g_level[IRQ_MAX], g_low[IRQ_MAX], g_trigger_set[IRQ_MAX];

// The ELCR (ports 0x4D0/0x4D1), the chipset's edge/level control for
// the 8259 lines: a bit set means the BIOS routed a LEVEL-triggered
// (PCI) source to that ISA line. With no override for the line, this
// is how Linux decides too -- the difference between a NIC on IRQ 11
// that works and one that stalls after its first frame.
static int elcr_level(uint8_t irq) {
    uint16_t elcr = (uint16_t)(inb(0x4D0) | (inb(0x4D1) << 8));
    return (elcr >> irq) & 1;
}

void irq_register_handler(uint8_t irq, irq_handler_fn handler) {
    if (irq >= IRQ_MAX || !handler) return;
    for (int i = 0; i < MAX_HANDLERS_PER_IRQ; i++) {
        // Idempotent: registering the same handler twice is a driver
        // that re-initialised, not a request to be called twice. Being
        // called twice per interrupt would double every event it
        // reports, which is a bug that presents as duplicated input
        // rather than as a registration problem.
        if (handlers[irq][i] == handler) return;
        if (!handlers[irq][i]) {
            handlers[irq][i] = handler;
            return;
        }
    }
}

void irq_unregister_handler(uint8_t irq, irq_handler_fn handler) {
    if (irq >= IRQ_MAX || !handler) return;
    for (int i = 0; i < MAX_HANDLERS_PER_IRQ; i++) {
        if (handlers[irq][i] != handler) continue;
        // Dispatch stops at the first NULL, so the tail moves down.
        for (int j = i; j + 1 < MAX_HANDLERS_PER_IRQ; j++)
            handlers[irq][j] = handlers[irq][j + 1];
        handlers[irq][MAX_HANDLERS_PER_IRQ - 1] = 0;
        return;
    }
}

uint8_t irq_vector(uint8_t irq) {
    return irq < 16 ? (uint8_t)(32 + irq) : (uint8_t)(64 + (irq - 16));
}

int irq_is_unmasked(uint8_t irq) {
    return irq < IRQ_MAX && ((g_unmasked >> irq) & 1);
}

int irq_on_ioapic(void) { return g_ioapic; }

void irq_set_trigger(uint8_t irq, int level, int low) {
    if (irq >= IRQ_MAX) return;
    g_level[irq] = (uint8_t)!!level;
    g_low[irq] = (uint8_t)!!low;
    g_trigger_set[irq] = 1;
}

// Where and how line `irq` arrives on the I/O APIC. ISA lines come from
// the overrides, then the ELCR; a GSI is itself, level/low unless a
// `_PRT` link said otherwise.
static int route(uint8_t irq, uint32_t *gsi) {
    int level = 1, low = 1;
    if (irq < 16) {
        if (!ioapic_gsi_for_isa(irq, gsi, &level, &low)) return -EINVAL;
        if (!level && !low && elcr_level(irq)) { level = 1; low = 1; }
    } else {
        *gsi = irq;
        if (g_trigger_set[irq]) { level = g_level[irq]; low = g_low[irq]; }
    }
    int rc = ioapic_route(*gsi, irq_vector(irq), level, low);
    if (rc == 0)
        klog_printf("irq: line %u -> GSI %u, vector %u, %s %s\n", irq, *gsi, irq_vector(irq),
                    level ? "level" : "edge", low ? "low" : "high");
    return rc;
}

int irq_unmask(uint8_t irq) {
    if (irq >= IRQ_MAX) return -EINVAL;
    if (!g_ioapic) {
        if (irq >= 16) return -ENODEV;
        pic_clear_mask(irq);
    } else {
        uint32_t gsi;
        if (irq == 2) { g_unmasked |= 1u << irq; return 0; }   // the cascade: nothing to route
        int rc = route(irq, &gsi);
        if (rc) return rc;
        ioapic_unmask(gsi);
    }
    g_unmasked |= 1u << irq;
    return 0;
}

void irq_mask(uint8_t irq) {
    if (irq >= IRQ_MAX) return;
    g_unmasked &= ~(1u << irq);
    if (!g_ioapic) { if (irq < 16) pic_set_mask(irq); return; }
    uint32_t gsi = irq; int level, low;
    if (irq < 16 && !ioapic_gsi_for_isa(irq, &gsi, &level, &low)) return;
    ioapic_mask(gsi);
}

void irq_switch_to_ioapic(void) {
    if (g_ioapic) return;
    g_ioapic = 1;
    // Every line the PIC was delivering, re-routed BEFORE the PIC is
    // silenced, so no tick or keystroke falls in the gap.
    for (uint8_t irq = 0; irq < 16; irq++) {
        if (!((g_unmasked >> irq) & 1) || irq == 2) continue;
        uint32_t gsi;
        if (route(irq, &gsi) == 0) ioapic_unmask(gsi);
    }
    for (uint8_t irq = 0; irq < 16; irq++) pic_set_mask(irq);
    // The 8259 reached the CPU through LINT0 (virtual wire); with
    // nothing to deliver it is closed, or a stray line would arrive as
    // an ExtINT with no I/O APIC entry behind it.
    lapic_mask_lint0();
    klog_write("irq: the I/O APIC delivers every line now; the 8259 is masked\n");
}

void irq_dispatch(uint8_t irq, uint64_t *regs) {
    if (irq < IRQ_MAX) {
        // EVERY handler on the line runs, in registration order, and
        // each decides for itself whether the interrupt was its
        // device's. Stopping at the first one that claims it would
        // leave a second device on the same line asserting forever.
        for (int i = 0; i < MAX_HANDLERS_PER_IRQ && handlers[irq][i]; i++) {
            handlers[irq][i](regs);
        }
    }
    if (g_ioapic) lapic_eoi();
    else pic_send_eoi(irq);
}
