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

// Enable Slot, then Address Device, for a device on root port
// `root_port` (1-based) at `speed`, reached through `route` (the xHCI
// route string; 0 for a root-port device). For a low/full-speed device
// behind a HIGH-speed hub, `tt_slot`/`tt_port` name the hub doing the
// split transactions; zero otherwise. Allocates the slot's contexts and
// its endpoint-0 transfer ring. Returns the slot id, or a negative
// completion code.
int xhci_address_device(uint8_t root_port, uint32_t route, uint8_t speed,
                        uint8_t tt_slot, uint8_t tt_port);

// Disable Slot plus teardown: frees the slot's contexts, its ep0 ring
// and every interrupt endpoint configured on it. The one door out, used
// by detach and by enumeration's own failure path -- a failed
// enumeration used to leak its slot, which is what made a retry
// impossible.
void xhci_disable_slot(uint8_t slot);

// Marks the slot a HUB in its input slot context (hub flag, port count,
// and TT think time for a high-speed hub). Takes effect with the next
// Configure Endpoint command, which is xhci_add_interrupt_in()'s -- so
// the hub driver calls this before configuring the status-change
// endpoint. The controller refuses to route through a slot not marked
// this way.
void xhci_slot_set_hub(uint8_t slot, uint8_t n_ports, uint8_t ttt);

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

// Adds one isochronous OUT endpoint to a configured device and starts
// its transfer ring, via a Configure Endpoint command. `mps` is the
// endpoint's wMaxPacketSize -- for an isochronous endpoint that is the
// bytes it carries EVERY service interval, not a ceiling.
//
// `done` is called once per completed TD group, FROM THE EVENT DRAIN
// (so from interrupt context), with the bytes the controller reported.
// A driver's callback may call xhci_isoch_post() and nothing else here.
// Returns 0, or a negative completion code.
// --- bulk endpoints ---------------------------------------------------
//
// THE DRIVER OWNS THE BUFFERS, as with isochronous and unlike interrupt:
// an Ethernet frame is 1514 bytes and the interrupt path's slices are
// 64, so there is no shared geometry worth borrowing. Post a buffer,
// get it back through the callback with the byte count.
//
// `done` runs FROM THE EVENT DRAIN. It may post again -- that touches
// only its own transfer ring and a doorbell -- and must not do anything
// that issues a COMMAND, which the drain cannot re-enter.
int xhci_add_bulk(uint8_t slot, uint8_t ep_addr, uint16_t mps,
                  void (*done)(void *ctx, uint64_t phys, uint32_t bytes, int ok),
                  void *ctx);

// Queue one buffer on a bulk endpoint. Direction is the endpoint's.
int xhci_bulk_post(uint8_t slot, uint8_t ep_addr, uint64_t buf_phys,
                   uint32_t len);

// A bulk endpoint HALTS on a stall, unlike an isochronous one, and
// xhci_deferred_work() recovers it. A driver whose completions stopped
// should ask this rather than assume the device went away.
int xhci_bulk_halted(uint8_t slot, uint8_t ep_addr);

int xhci_add_isoch_out(uint8_t slot, uint8_t ep_addr, uint16_t mps,
                       uint8_t interval, void (*done)(void *ctx, uint32_t bytes),
                       void *ctx);

// Queues ONE isochronous TD -- one packet, one service interval -- from
// `buf_phys`. `ioc` asks for a completion event; a group of TDs
// normally carries it on the last one only, since an isochronous
// endpoint completes them in order and an error reports itself
// regardless. Returns 0, or -1 when there is no such endpoint.
//
// The buffer must not cross a 64 KiB boundary: xHCI splits a TRB there
// and this posts one TRB per TD. A 192-byte packet inside one frame
// cannot, which is why the audio driver keeps its packets in a frame of
// its own rather than pointing into the sound core's 64 KiB ring.
int xhci_isoch_post(uint8_t slot, uint8_t ep_addr, uint64_t buf_phys,
                    uint32_t len, int ioc);

// Ring underruns seen on that endpoint -- the stream ran dry. A
// diagnostic counter, so a test can tell "it never played" from "it
// played and stuttered".
uint32_t xhci_isoch_underruns(uint8_t slot, uint8_t ep_addr);

// Takes the next completed report from that endpoint, if one has
// arrived, copying at most `cap` bytes. Returns the byte count, or 0
// when nothing is pending. Re-posts the TRB, so no caller has to think
// about ring depth.
int xhci_take_report(uint8_t slot, uint8_t ep_addr, void *buf, uint32_t cap);

// Drains the event ring. Called from the interrupt handler and from
// the input core's poll -- BOTH are always live now (see usb_init()'s
// comment on why the poll is a backup rather than an either/or), and
// the single-consumer guard inside is what makes that safe.
void xhci_service(void);

// The work an event marks but must not do in interrupt context: root
// port attach/detach (enumeration is synchronous control transfers) and
// halted-endpoint recovery. Called from the input core's poll.
void xhci_deferred_work(void);

// A completion code's name, for logs. Never NULL.
const char *xhci_completion_name(uint32_t code);

#endif
