// TRB ring arithmetic. See kernel/include/kernel/xhci_ring.h for the
// two ring shapes and why this is a separate, controller-free file.
#include "xhci_ring.h"
#include "string.h"
#include "barrier.h"

// driver-none: the TRB ring inside the xHCI driver

void xhci_ring_init(struct xhci_ring *r, void *base, uint64_t phys,
                    uint32_t count, int is_event) {
    if (!r || !base || count < 1) return;

    r->trb      = (volatile struct xhci_trb *)base;
    r->phys     = phys;
    r->count    = count;
    r->enqueue  = 0;
    r->dequeue  = 0;
    r->is_event = (uint8_t)(is_event ? 1 : 0);

    // Both cycle states start at 1, and the ring starts zeroed -- so
    // every slot reads cycle 0, which is "not yet valid" to whichever
    // side is consuming. Skipping the zeroing makes the first poll
    // report a phantom completion off whatever the frame held before,
    // which is the same trap virtqueue_setup() documents.
    r->cycle = 1;
    r->ccs   = 1;
    k_memset(base, 0, (uint64_t)count * sizeof(struct xhci_trb));

    if (!is_event && count >= 2) {
        // The Link TRB occupies the last slot permanently and points
        // back at the segment base. TC (Toggle Cycle) is what makes the
        // producer's cycle bit flip on wrap; without it the controller
        // would keep consuming stale TRBs on the second lap.
        volatile struct xhci_trb *link = &r->trb[count - 1];
        link->p0      = (uint32_t)(phys & 0xFFFFFFFFu);
        link->p1      = (uint32_t)(phys >> 32);
        link->status  = 0;
        link->control = XHCI_TRB_SET_TYPE(XHCI_TRB_LINK) | XHCI_TRB_TC;
        // Its cycle bit is set as part of the wrap, in push().
    }
}

uint64_t xhci_ring_enq_phys(const struct xhci_ring *r) {
    if (!r) return 0;
    return r->phys + (uint64_t)r->enqueue * sizeof(struct xhci_trb);
}

uint64_t xhci_ring_push(struct xhci_ring *r, uint64_t param,
                        uint32_t status, uint32_t control) {
    if (!r || r->is_event || r->count < 2) return 0;

    volatile struct xhci_trb *t = &r->trb[r->enqueue];
    uint64_t at = r->phys + (uint64_t)r->enqueue * sizeof(struct xhci_trb);

    t->p0     = (uint32_t)(param & 0xFFFFFFFFu);
    t->p1     = (uint32_t)(param >> 32);
    t->status = status;

    // The cycle bit goes on LAST and behind a barrier: it is what hands
    // the TRB to the controller, so every other field must already be
    // visible. On x86 the store-store ordering is free, but the
    // compiler still has to be told (see barrier.h).
    kbarrier();
    t->control = (control & ~(uint32_t)XHCI_TRB_CYCLE) |
                 (r->cycle ? XHCI_TRB_CYCLE : 0);

    r->enqueue++;

    // Reaching the Link TRB is the wrap. Publish its cycle bit -- same
    // ordering rule -- then flip ours and return to the top. This is a
    // `while` rather than an `if` only in shape; one Link TRB per
    // segment means it runs at most once.
    if (r->enqueue == r->count - 1) {
        volatile struct xhci_trb *link = &r->trb[r->count - 1];
        kbarrier();
        link->control = XHCI_TRB_SET_TYPE(XHCI_TRB_LINK) | XHCI_TRB_TC |
                        (r->cycle ? XHCI_TRB_CYCLE : 0);
        r->cycle  = (uint8_t)!r->cycle;
        r->enqueue = 0;
    }
    return at;
}

int xhci_ring_event_pop(struct xhci_ring *r, struct xhci_trb *out) {
    if (!r || !r->is_event || !out) return 0;

    volatile struct xhci_trb *t = &r->trb[r->dequeue];

    // `volatile` is the load-bearing part here, not the barrier: without
    // it GCC hoists this read out of a polling loop and spins forever on
    // a stale register. chain_done() in virtqueue.c documents the same.
    uint32_t control = t->control;
    if ((control & XHCI_TRB_CYCLE) != (r->ccs ? XHCI_TRB_CYCLE : 0))
        return 0;   // not yet written by the controller

    // Read the payload only after the cycle bit said it is valid.
    kbarrier();
    out->p0      = t->p0;
    out->p1      = t->p1;
    out->status  = t->status;
    out->control = control;

    r->dequeue++;
    if (r->dequeue == r->count) {     // no Link TRB on an event ring
        r->dequeue = 0;
        r->ccs = (uint8_t)!r->ccs;
    }
    return 1;
}

uint64_t xhci_ring_erdp(const struct xhci_ring *r) {
    if (!r) return 0;
    return r->phys + (uint64_t)r->dequeue * sizeof(struct xhci_trb);
}

uint32_t xhci_ring_room(const struct xhci_ring *r) {
    if (!r || r->is_event || r->count < 2) return 0;
    uint32_t usable = r->count - 1;               // the Link TRB is not ours
    uint32_t used = (r->enqueue + usable - r->dequeue) % usable;
    return usable - 1 - used;
}

void xhci_ring_consumed(struct xhci_ring *r, uint64_t trb_phys) {
    if (!r || r->is_event || r->count < 2 || trb_phys < r->phys) return;
    uint64_t idx = (trb_phys - r->phys) / sizeof(struct xhci_trb);
    if (idx >= r->count - 1) return;              // outside, or the Link TRB
    r->dequeue = idx + 1 == r->count - 1 ? 0 : (uint32_t)idx + 1;
}
