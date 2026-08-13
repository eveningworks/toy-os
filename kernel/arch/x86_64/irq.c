// Generic hardware-IRQ registration + dispatch -- the single table
// every PIC-routed interrupt (vectors 32-47, IRQ 0-15) now goes
// through. Replaces what used to be a hardcoded if/else chain in
// idt.c's isr_dispatch() -- three special cases (timer, keyboard,
// mouse) and every other IRQ silently EOI'd and ignored, with no way
// for a new driver to plug in without adding yet another one-off
// branch. Added specifically because a NIC needs its own promptly-
// serviced IRQ line -- see README.md's TCP/IP entry and
// docs/decisions.md for the full reasoning behind adding this now.
//
// One handler per IRQ, not a chain: every IRQ source this kernel
// currently has -- or is about to add (a NIC) -- lives on its own
// dedicated line in QEMU's default topology (confirmed by build 390's
// `lspci`), so real IRQ-line sharing (two devices interrupting on the
// same line, which does happen on busier real hardware) isn't a case
// that comes up here. Registering a second handler for an IRQ that
// already has one replaces it outright rather than chaining, which
// would need deciding what happens when a handler doesn't recognize
// "its" interrupt -- machinery nothing here needs yet.
//
// EOI (end-of-interrupt) is sent HERE, automatically, after the
// handler returns -- not left to each handler to remember. A forgotten
// EOI on a real IRQ line is a classic bug (silently stops all further
// interrupts on that line), so removing the chance of it costs only a
// little flexibility (a handler that wants to EOI early, before doing
// slower work, can't) for a real safety improvement.
#include "irq.h"
#include "pic.h"

static irq_handler_fn handlers[16];

void irq_register_handler(uint8_t irq, irq_handler_fn handler) {
    if (irq >= 16) return;
    handlers[irq] = handler;
}

void irq_dispatch(uint8_t irq, uint64_t *regs) {
    if (irq < 16 && handlers[irq]) handlers[irq](regs);
    pic_send_eoi(irq);
}
