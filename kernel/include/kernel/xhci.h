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

// --- what usb_enum.c and usb_hid.c call ------------------------------

// Enable Slot, then Address Device, for a device sitting on `port`
// (1-based) at `speed`. Allocates the slot's contexts and its endpoint-0
// transfer ring. Returns the slot id, or a negative completion code.
int xhci_address_device(uint8_t port, uint8_t speed);

// Corrects endpoint 0's max packet size once the device descriptor has
// said what it really is, via an Evaluate Context command.
//
// A full-speed device may legitimately report 8, 16, 32 or 64, and the
// driver has to guess 8 before it can ask. Guessing and never
// correcting hangs on the first device that says 64 -- and QEMU's
// usb-kbd is full-speed, so this path is reachable in test rather than
// theoretical. Returns 0, or a negative completion code.
int xhci_set_ep0_mps(uint8_t slot, uint16_t mps);

// A control transfer on endpoint 0. `setup` is the 8-byte SETUP packet
// as the wire carries it; `buf`/`len` is the optional data stage and
// `in` says which way it goes. Returns bytes transferred, or a negative
// completion code.
//
// SYNCHRONOUS: it enqueues, rings the doorbell and drains the event ring
// until the Transfer Event naming its own TRB arrives. That is honest
// for enumeration -- which happens once, at boot, with nothing else
// running -- and is deliberately NOT how interrupt endpoints are
// serviced.
int xhci_control(uint8_t slot, const uint8_t setup[8],
                 void *buf, uint16_t len, int in);

// Adds one interrupt-IN endpoint to a configured device and starts its
// transfer ring, via a Configure Endpoint command. `ep_addr`, `mps` and
// `interval` come from the endpoint descriptor. Returns 0, or a
// negative completion code.
//
// It pre-posts a DEPTH of TRBs rather than one, and re-posts each on
// completion. A ring with a single outstanding TRB drops every report
// that arrives between completion and re-post -- the same reason
// virtio_input.c keeps 64 buffers posted.
int xhci_add_interrupt_in(uint8_t slot, uint8_t ep_addr, uint16_t mps,
                          uint8_t interval);

// Takes the next completed report from that endpoint, if one has
// arrived, copying at most `cap` bytes. Returns the byte count, or 0
// when nothing is pending. Re-posts the TRB, so no caller has to think
// about ring depth.
int xhci_take_report(uint8_t slot, uint8_t ep_addr, void *buf, uint32_t cap);

// Drains the event ring. Called from the interrupt handler and, when
// the controller has no usable IRQ line, from the input core's poll.
void xhci_service(void);

// A completion code's name, for logs. Never NULL.
const char *xhci_completion_name(uint32_t code);

#endif
