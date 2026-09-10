#ifndef IRQ_H
#define IRQ_H

#include <stdint.h>

// Generic hardware-IRQ registration -- see irq.c's top comment for why
// this exists and the design choices behind it (one handler per IRQ,
// no chaining; automatic EOI). idt.c's isr_dispatch() is the only
// caller of irq_dispatch(); idt_init() is the only caller of
// irq_register_handler() today, but any future driver needing its own
// IRQ line (a NIC, chiefly -- see README.md's TCP/IP entry) calls it
// directly, the same way kernel/drivers/*.c already includes other
// kernel/include headers directly (this is kernel-internal wiring, not
// part of the apps/ boundary kapi.h enforces).

// Signature every hardware-IRQ handler must have. `regs` is the same
// saved-register-block pointer isr_dispatch() receives (see idt.c's
// top comment on isr_dispatch) -- most handlers never need it (a
// device poll function just wants to know "service me"), but the
// timer's handler needs it to hand off to scheduler_tick(), which can
// redirect where the CPU resumes after this ISR returns. Passing it
// uniformly to every handler, even ones that ignore it, is what keeps
// this a single generic mechanism instead of a special case for the
// timer.
typedef void (*irq_handler_fn)(uint64_t *regs);

// Registers `handler` to run whenever IRQ `irq` (0-15, a PIC line
// number, NOT a raw interrupt vector -- vector 32+irq is what actually
// reaches isr_dispatch(), see pic.h) fires. Only one handler per IRQ --
// see irq.c's top comment on why this kernel doesn't need IRQ-line
// sharing/chaining. Registering a second handler for the same IRQ
// replaces the first (last call wins); pass NULL to unregister.
// Out-of-range `irq` values are silently ignored.
// Adds a handler to `irq`'s chain. Several devices may share a line
// (that is what PCI INTx does), so every registered handler runs on
// every interrupt and each must decide whether its own device was the
// source -- typically by reading a status register. Registering the
// same function twice is a no-op rather than a second call per
// interrupt. EOI is sent here once, after the chain, so no handler
// sends its own.
void irq_register_handler(uint8_t irq, irq_handler_fn handler);

// --- which controller a line is on is NOT the driver's business --------
//
// A line is 0-15 for an ISA IRQ (translated to its I/O APIC input by
// the MADT's overrides when that controller is live) or 16-23 for a
// GSI a PCI device's `_PRT` entry named (pci_irq_line()). irq_unmask()
// programs whichever controller owns the machine; on the 8259 a GSI
// above 15 does not exist and is refused with -ENODEV, which
// pci_irq_line() already accounts for. Linux's irq_chip, in one call.
#define IRQ_MAX 24
#define IRQ_NONE 0xFF
int  irq_unmask(uint8_t irq);
void irq_mask(uint8_t irq);
int  irq_is_unmasked(uint8_t irq);
uint8_t irq_vector(uint8_t irq);   // 32+irq below 16, 64+(irq-16) above

// For a GSI above 15: how the wire behaves, from the `_PRT` link's
// resource descriptor. The default -- and the rule for a bare GSI -- is
// PCI's level-triggered, active-low. Set BEFORE irq_unmask().
void irq_set_trigger(uint8_t irq, int level, int low);

// Called once by ioapic_init(): re-routes every line the PIC was
// delivering, silences the PIC, and makes every later unmask an I/O
// APIC entry. Not for drivers.
void irq_switch_to_ioapic(void);
int  irq_on_ioapic(void);

// The inverse, for a driver that is going away (a module unloading).
// Closes the gap so the chain stays contiguous; unknown is a no-op.
void irq_unregister_handler(uint8_t irq, irq_handler_fn handler);

// Called by idt.c's isr_dispatch() for every hardware-IRQ vector
// (32-47, i.e. `irq` in 0-15): looks up and calls whatever's
// registered for `irq`, then unconditionally sends the PIC
// end-of-interrupt for it -- see irq.c's top comment on why EOI lives
// here, not in each handler. A no-op aside from the EOI if nothing is
// registered for `irq`, same observable behavior an unhandled IRQ had
// before this existed.
void irq_dispatch(uint8_t irq, uint64_t *regs);

#endif
