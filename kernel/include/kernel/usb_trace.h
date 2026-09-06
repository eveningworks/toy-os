#ifndef USB_TRACE_H
#define USB_TRACE_H

#include <stdint.h>

// A SMALL RING OF USB EVENTS, PRINTED ONLY WHEN SOMETHING FAILS.
//
// The enumeration fault this exists for is intermittent, so the useful
// detail has to be recorded on EVERY boot -- and printing it on every
// boot is exactly the trap CLAUDE.md names: the klog ring holds a few
// hundred lines, and a probe that outruns it destroys the evidence it
// was gathering. That is not hypothetical here; a 1 Hz timeout storm
// flushed a laptop's entire boot log on 2026-09-06 and left the failure
// undiagnosable.
//
// So: record always, print never -- until a port gives up, at which
// point the events for THAT port are dumped and nothing else is. A
// healthy boot is byte-for-byte as quiet as before, and a failing one
// carries what a person needs without a switch that had to be armed
// before a boot nobody could predict.
//
// It is a diagnostic, not an interface: the format is for reading, and
// nothing parses it.

enum usb_trace_kind {
    USB_TR_CMD = 0,   // a command: a=TRB type, b=completion code
    USB_TR_CTRL,      // a control transfer: a=setup[0..3], b=result
    USB_TR_PORT,      // a port state read: a=PORTSC
    USB_TR_NOTE,      // a bare marker: a=code
};

// One event. Called from the enumeration and transfer paths; cheap
// enough to sit on them (a timestamp and four stores).
void usb_trace(uint8_t kind, uint8_t port, uint8_t slot,
               uint32_t a, uint32_t b);

// Where the ring is now. Take one before an attempt, hand it back to
// usb_trace_dump() to print only what that attempt produced -- which is
// what keeps the dump bounded by the work rather than by the ring.
uint32_t usb_trace_mark(void);

// Print events recorded since `mark`, newest last, capped. Also prints
// the CONTROLLER's own state, which is the half a per-port trace cannot
// show: a halted controller or a host system error would explain every
// port after the first failing at once, and nothing here was looking.
void usb_trace_dump(uint32_t mark, uint8_t port);

// The controller's USBSTS, so the dump can name a halted or errored
// controller. 0 when there is no controller.
uint32_t xhci_usbsts(void);

#endif
