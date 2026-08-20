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
// A CHAIN of handlers per IRQ, which it did not used to be. The old
// rule was one handler per line, on the measured grounds that every
// device here sat on its own line -- true until virtio-input, where
// THREE PCI functions (a keyboard, a mouse and a tablet) are routed by
// the chipset onto whichever PIRQ their slot maps to and routinely
// collide, with each other and with anything else on the bus.
//
// Sharing is what INTx is: the line is LEVEL-triggered and stays
// asserted until the device that pulled it is serviced, so every
// handler on a line runs and each one asks its own device "was that
// you?" (for virtio, by reading the ISR register, which is
// read-to-clear). A handler that does not recognise its interrupt
// simply returns -- that is the machinery the old comment said nothing
// here needed, and it is four lines of it.
//
// The hazard worth naming: a device left free to assert a line with NO
// handler reading its ISR holds that line down forever, and the PIC
// re-delivers it forever -- an interrupt storm that looks like a hang.
// That is why the polled virtio drivers (block, entropy, GPU) keep
// PCI_CMD_INTX_DISABLE set rather than "just in case" leaving it clear.
//
// EOI (end-of-interrupt) is sent HERE, automatically, after the
// handler returns -- not left to each handler to remember. A forgotten
// EOI on a real IRQ line is a classic bug (silently stops all further
// interrupts on that line), so removing the chance of it costs only a
// little flexibility (a handler that wants to EOI early, before doing
// slower work, can't) for a real safety improvement.
#include "irq.h"
#include "pic.h"

// Four per line is the PCI reality (INTA#-INTD# rotate across slots, so
// a handful of functions can land on one line) plus room for the one
// legacy owner.
#define MAX_HANDLERS_PER_IRQ 4

static irq_handler_fn handlers[16][MAX_HANDLERS_PER_IRQ];

void irq_register_handler(uint8_t irq, irq_handler_fn handler) {
    if (irq >= 16 || !handler) return;
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

void irq_dispatch(uint8_t irq, uint64_t *regs) {
    if (irq < 16) {
        // EVERY handler on the line runs, in registration order, and
        // each decides for itself whether the interrupt was its
        // device's. Stopping at the first one that claims it would
        // leave a second device on the same line asserting forever.
        for (int i = 0; i < MAX_HANDLERS_PER_IRQ && handlers[irq][i]; i++) {
            handlers[irq][i](regs);
        }
    }
    pic_send_eoi(irq);
}
