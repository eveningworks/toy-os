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
void irq_register_handler(uint8_t irq, irq_handler_fn handler);

// Called by idt.c's isr_dispatch() for every hardware-IRQ vector
// (32-47, i.e. `irq` in 0-15): looks up and calls whatever's
// registered for `irq`, then unconditionally sends the PIC
// end-of-interrupt for it -- see irq.c's top comment on why EOI lives
// here, not in each handler. A no-op aside from the EOI if nothing is
// registered for `irq`, same observable behavior an unhandled IRQ had
// before this existed.
void irq_dispatch(uint8_t irq, uint64_t *regs);

#endif
