#ifndef XHCI_RING_H
#define XHCI_RING_H

#include <stdint.h>
#include "xhci_regs.h"

// TRB ring arithmetic, with no controller anywhere in it.
//
// WHY THIS IS ITS OWN FILE. Everything else in the xHCI driver needs a
// live controller to say anything at all, so it can only be tested by
// booting a machine with one attached. This half is pure arithmetic
// over a buffer -- the cycle bit, the Link TRB, the wrap -- and it is
// where the bugs actually are, so it is separated in order to be
// KTESTable on every boot including a machine with no USB at all.
// Same argument geom.c makes for drawing through a callback.
//
// TWO RING SHAPES, and the difference is not cosmetic:
//
//  - A COMMAND or TRANSFER ring is PRODUCER-side. The driver enqueues,
//    the controller consumes. Its last slot is a LINK TRB with the
//    Toggle Cycle bit, so the ring is circular and the producer's cycle
//    bit flips on every wrap. The enqueue index must never come to rest
//    on the Link TRB.
//
//  - An EVENT ring is CONSUMER-side. The controller enqueues, the
//    driver consumes. It has NO Link TRB at all: the segment size comes
//    from the ERST, and the consumer knows an entry is fresh by
//    comparing the TRB's cycle bit against its own Consumer Cycle
//    State, flipping CCS when the dequeue index wraps.
//
// Getting the second one wrong is the failure that works for exactly
// one segment and then goes permanently deaf -- 256 TRBs, so a test
// typing a handful of characters cannot see it.

struct xhci_ring {
    volatile struct xhci_trb *trb;  // segment base (identity-mapped)
    uint64_t phys;                  // its physical address
    uint32_t count;                 // TRBs in the segment, Link TRB included
    uint32_t enqueue;               // producer index
    uint32_t dequeue;               // consumer index (event rings)
    uint8_t  cycle;                 // producer cycle state
    uint8_t  ccs;                   // consumer cycle state (event rings)
    uint8_t  is_event;
};

// Zeroes the segment and, for a producer ring, installs the Link TRB in
// the last slot. `count` must be at least 2 for a producer ring (one
// usable TRB plus the Link) and at least 1 for an event ring.
void xhci_ring_init(struct xhci_ring *r, void *base, uint64_t phys,
                    uint32_t count, int is_event);

// Enqueues one TRB on a producer ring, handling the Link TRB and the
// cycle flip. `control` must NOT carry the cycle bit -- this owns it.
// Returns the PHYSICAL address the TRB was written at, which is what a
// Command Completion event names, or 0 if the ring is an event ring.
uint64_t xhci_ring_push(struct xhci_ring *r, uint64_t param,
                        uint32_t status, uint32_t control);

// Consumes one event if a fresh one is there. Copies it to `out` and
// returns 1; returns 0 when the ring is empty (the next TRB's cycle bit
// does not match CCS). Advances the dequeue index and flips CCS on wrap.
int xhci_ring_event_pop(struct xhci_ring *r, struct xhci_trb *out);

// The physical address the ERDP should be written with -- the segment
// base plus the current dequeue index.
uint64_t xhci_ring_erdp(const struct xhci_ring *r);

// Where the NEXT push will land. A caller that must be ready for the
// completion BEFORE the TRB is visible to the controller needs this:
// xhci_ring_push() sets the cycle bit last, which hands the TRB over,
// so anything armed after it has already lost the race.
uint64_t xhci_ring_enq_phys(const struct xhci_ring *r);

#endif
