#ifndef XHCI_H
#define XHCI_H

#include <stdint.h>
#include "xhci_regs.h"
#include "xhci_ring.h"

// The seam between xHCI MECHANICS and USB SEMANTICS.
//
// This is a plain header with one implementation behind it, NOT an ops
// table -- see usb.h for why there is no HCD abstraction. What makes
// this seam worth having anyway is that it already has two real callers
// (the keyboard and the mouse in usb_hid.c, both reached through
// usb_enum.c) rather than one plausible future one.
//
// Below this line: rings, doorbells, contexts, completion codes.
// Above it: descriptors, standard requests, HID reports.

#define XHCI_MAX_PORTS   16
#define XHCI_MAX_SLOTS   8

// Every wait in this driver is bounded by a POLL COUNT, never by a
// deadline. That is not a style choice: clocksource_now_ns() may be
// PIT-backed, the PIT does not advance while interrupts are off, and CI
// once reported a virtqueue timeout of "927725008 us and 454 polls" as
// a result. Elapsed time is printed for diagnosis and decides nothing.
// virtqueue.c's poll loop carries the full argument.
#define XHCI_POLL_BACKSTOP 2000000u

// Drains the event ring. Called from the interrupt handler and, when
// the controller has no usable IRQ line, from the input core's poll.
void xhci_service(void);

// A completion code's name, for logs. Never NULL.
const char *xhci_completion_name(uint32_t code);

#endif
