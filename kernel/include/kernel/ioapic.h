#ifndef IOAPIC_H
#define IOAPIC_H

#include <stdint.h>

// THE I/O APIC: the interrupt controller that replaces the 8259 pair
// once the LAPIC is up. Every legacy line and every PCI INTx pin lands
// on one of its inputs (a GLOBAL SYSTEM INTERRUPT number, GSI), and a
// redirection entry per input says which vector to raise and with what
// trigger and polarity. Linux's io_apic.c; here one controller, one
// CPU.
//
// Nothing outside irq.c calls the routing half. Drivers say
// irq_unmask(line) (kernel/irq.h) and never learn which controller is
// live -- which is what lets `noioapic` keep the whole machine on the
// 8259 for a bisect.
//
// A GSI is NOT an ISA IRQ NUMBER. The MADT's interrupt source overrides
// say where each ISA line actually arrives (IRQ 0 is GSI 2 on every
// PC), and ioapic_gsi_for_isa() is the only place that translation
// lives.

int  ioapic_init(void);     // 1 when it took over, 0 when the machine stays on the PIC
int  ioapic_present(void);
int  ioapic_inputs(void);   // redirection entries on the first controller

// The I/O APIC input an ISA IRQ arrives on, with its trigger and
// polarity, from the overrides (default: the same number, edge, high).
// Returns 0 for IRQ 2 -- the cascade, which is not a line.
int  ioapic_gsi_for_isa(uint8_t irq, uint32_t *gsi, int *level, int *low);

// Programs input `gsi` to raise `vector` on this CPU, MASKED. Returns
// 0, or -ERANGE when the input is not this controller's.
int  ioapic_route(uint32_t gsi, uint8_t vector, int level, int low);
void ioapic_mask(uint32_t gsi);
void ioapic_unmask(uint32_t gsi);

// The redirection entry as programmed, for the KTEST and `lsirq`-style
// reporting: low dword (vector, delivery, trigger, mask) or 0.
uint32_t ioapic_entry(uint32_t gsi);

#endif
