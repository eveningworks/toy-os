// KTESTs for the TRB ring arithmetic.
//
// These need no controller: xhci_ring.c is deliberately pure arithmetic
// over a caller-supplied buffer, so every one of these runs inside
// `make test` on a machine with no USB hardware at all. That is the
// whole reason the file was split out -- the rest of the driver can
// only be tested by booting with `-device qemu-xhci`, and this is where
// the bugs are.
//
// The two that earn their place are the WRAP tests. A driver that never
// flips a cycle bit works perfectly for exactly one segment and then
// goes permanently deaf, and with 256 TRBs per segment that is ~128
// keystrokes -- far more than any reasonable GUI test types, which is
// why the guest-side test has to type more than a full ring and why
// these exist to catch it long before that.
#include "xhci_ring.h"
#include "ktest.h"
#include "string.h"
#include "xhci_regs.h"   // xhci_portsc_needs_warm() -- pure, like the rings

// Small enough that a test can wrap it in a few pushes, which is the
// point: the real ring is 256 and wrapping it in a KTEST would be slow
// and would prove nothing extra.
#define TEST_TRBS 8
static struct xhci_trb g_seg[TEST_TRBS] __attribute__((aligned(64)));

// A fake physical address. Nothing here dereferences it -- the ring
// only ever stores it into a Link TRB and returns it from push() -- so
// it can be any recognisable value, and a recognisable one makes a
// wrong offset obvious in the assertion rather than plausible.
#define FAKE_PHYS 0x00ABC000ull

KTEST("xhci-ring", "a producer ring puts a Link TRB in the last slot") {
    struct xhci_ring r;
    xhci_ring_init(&r, g_seg, FAKE_PHYS, TEST_TRBS, 0);

    volatile struct xhci_trb *link = &g_seg[TEST_TRBS - 1];
    KTEST_ASSERT_EQ(XHCI_TRB_TYPE(link->control), XHCI_TRB_LINK);
    KTEST_ASSERT(link->control & XHCI_TRB_TC);   // or the cycle never flips
    KTEST_ASSERT_EQ(link->p0, (uint32_t)(FAKE_PHYS & 0xFFFFFFFFu));
    KTEST_ASSERT_EQ(link->p1, (uint32_t)(FAKE_PHYS >> 32));

    // Every other slot is zeroed, so its cycle bit reads 0 against a
    // starting producer cycle of 1 -- "not yet valid" to the controller.
    for (uint32_t i = 0; i < TEST_TRBS - 1; i++)
        KTEST_ASSERT_EQ(g_seg[i].control, 0u);
    KTEST_ASSERT_EQ(r.cycle, 1);
    KTEST_ASSERT_EQ(r.enqueue, 0u);
}

KTEST("xhci-ring", "an event ring has no Link TRB") {
    struct xhci_ring r;
    xhci_ring_init(&r, g_seg, FAKE_PHYS, TEST_TRBS, 1);

    // The last slot must stay a plain zeroed TRB. An event ring that
    // grew a Link TRB would lose one event slot per lap and misreport
    // the ERDP, and nothing about it would look wrong.
    for (uint32_t i = 0; i < TEST_TRBS; i++)
        KTEST_ASSERT_EQ(g_seg[i].control, 0u);
    KTEST_ASSERT_EQ(r.ccs, 1);
    KTEST_ASSERT_EQ(r.dequeue, 0u);
    KTEST_ASSERT_EQ(xhci_ring_erdp(&r), FAKE_PHYS);
}

KTEST("xhci-ring", "push returns where it wrote, and sets the cycle bit") {
    struct xhci_ring r;
    xhci_ring_init(&r, g_seg, FAKE_PHYS, TEST_TRBS, 0);

    uint64_t at = xhci_ring_push(&r, 0x1122334455667788ull, 0,
                                 XHCI_TRB_SET_TYPE(XHCI_TRB_NOOP_CMD));
    KTEST_ASSERT_EQ(at, FAKE_PHYS);            // the first TRB is at the base
    KTEST_ASSERT_EQ(g_seg[0].p0, 0x55667788u);
    KTEST_ASSERT_EQ(g_seg[0].p1, 0x11223344u);
    KTEST_ASSERT(g_seg[0].control & XHCI_TRB_CYCLE);
    KTEST_ASSERT_EQ(XHCI_TRB_TYPE(g_seg[0].control), XHCI_TRB_NOOP_CMD);

    uint64_t at2 = xhci_ring_push(&r, 0, 0, XHCI_TRB_SET_TYPE(XHCI_TRB_NOOP_CMD));
    KTEST_ASSERT_EQ(at2, FAKE_PHYS + sizeof(struct xhci_trb));
}

// The load-bearing one. A ring that does not flip its cycle bit on wrap
// keeps writing TRBs the controller has already consumed and will never
// look at again -- it works for one lap and then silently stops.
KTEST("xhci-ring", "the producer cycle bit flips exactly once per lap") {
    struct xhci_ring r;
    xhci_ring_init(&r, g_seg, FAKE_PHYS, TEST_TRBS, 0);

    // TEST_TRBS - 1 usable slots per lap (the Link TRB is not usable).
    const uint32_t usable = TEST_TRBS - 1;

    for (uint32_t i = 0; i < usable; i++) {
        KTEST_ASSERT_EQ(r.cycle, 1);           // still the first lap
        xhci_ring_push(&r, i, 0, XHCI_TRB_SET_TYPE(XHCI_TRB_NORMAL));
    }
    // That last push landed on the slot before the Link TRB, so the
    // wrap has happened: cycle flipped, enqueue back at the top.
    KTEST_ASSERT_EQ(r.cycle, 0);
    KTEST_ASSERT_EQ(r.enqueue, 0u);
    // ...and the Link TRB carries the cycle the lap was written with.
    KTEST_ASSERT(g_seg[TEST_TRBS - 1].control & XHCI_TRB_CYCLE);

    // Second lap: every TRB written with cycle 0.
    for (uint32_t i = 0; i < usable; i++) {
        KTEST_ASSERT_EQ(r.cycle, 0);
        xhci_ring_push(&r, i, 0, XHCI_TRB_SET_TYPE(XHCI_TRB_NORMAL));
        KTEST_ASSERT_EQ(g_seg[i].control & XHCI_TRB_CYCLE, 0u);
    }
    KTEST_ASSERT_EQ(r.cycle, 1);               // and back again
    KTEST_ASSERT_EQ(g_seg[TEST_TRBS - 1].control & XHCI_TRB_CYCLE, 0u);
}

// The enqueue index must never come to rest on the Link TRB: a TRB
// written there would overwrite the link itself, and the controller
// would follow a pointer into whatever the payload happened to be.
KTEST("xhci-ring", "the enqueue index never rests on the Link TRB") {
    struct xhci_ring r;
    xhci_ring_init(&r, g_seg, FAKE_PHYS, TEST_TRBS, 0);

    for (uint32_t i = 0; i < TEST_TRBS * 3; i++) {
        KTEST_ASSERT(r.enqueue < TEST_TRBS - 1);
        xhci_ring_push(&r, i, 0, XHCI_TRB_SET_TYPE(XHCI_TRB_NORMAL));
    }
    // The Link TRB survived every lap.
    KTEST_ASSERT_EQ(XHCI_TRB_TYPE(g_seg[TEST_TRBS - 1].control), XHCI_TRB_LINK);
    KTEST_ASSERT(g_seg[TEST_TRBS - 1].control & XHCI_TRB_TC);
}

KTEST("xhci-ring", "an empty event ring yields nothing") {
    struct xhci_ring r;
    struct xhci_trb ev;
    xhci_ring_init(&r, g_seg, FAKE_PHYS, TEST_TRBS, 1);

    // Nothing has written a cycle-1 TRB, so there is nothing to take --
    // and asking must not advance the dequeue index, or a later real
    // event would be read from the wrong slot.
    KTEST_ASSERT_EQ(xhci_ring_event_pop(&r, &ev), 0);
    KTEST_ASSERT_EQ(r.dequeue, 0u);
    KTEST_ASSERT_EQ(r.ccs, 1);
}

// The consumer half of the same hazard, and the one a real controller
// exposes: the driver must flip its OWN cycle state on wrap, because
// the controller reuses the segment with the opposite bit.
KTEST("xhci-ring", "the consumer cycle state flips on wrap, with no Link TRB") {
    struct xhci_ring r;
    struct xhci_trb ev;
    xhci_ring_init(&r, g_seg, FAKE_PHYS, TEST_TRBS, 1);

    // Play the controller: fill the whole segment with cycle-1 events.
    for (uint32_t i = 0; i < TEST_TRBS; i++) {
        g_seg[i].p0      = 0xE0000000u + i;
        g_seg[i].status  = 0;
        g_seg[i].control = XHCI_TRB_SET_TYPE(XHCI_TRB_CMD_COMPLETION) | XHCI_TRB_CYCLE;
    }
    for (uint32_t i = 0; i < TEST_TRBS; i++) {
        KTEST_ASSERT_EQ(xhci_ring_event_pop(&r, &ev), 1);
        KTEST_ASSERT_EQ(ev.p0, 0xE0000000u + i);   // in order, every slot
        KTEST_ASSERT_EQ(xhci_ring_erdp(&r),
                        FAKE_PHYS + (uint64_t)((i + 1) % TEST_TRBS) * sizeof(struct xhci_trb));
    }
    // Wrapped: CCS is now 0, and the cycle-1 entries still sitting in
    // the segment must read as STALE rather than be handed out twice.
    KTEST_ASSERT_EQ(r.ccs, 0);
    KTEST_ASSERT_EQ(r.dequeue, 0u);
    KTEST_ASSERT_EQ(xhci_ring_event_pop(&r, &ev), 0);

    // The controller writes the second lap with cycle 0.
    g_seg[0].p0      = 0xF00Du;
    g_seg[0].control = XHCI_TRB_SET_TYPE(XHCI_TRB_CMD_COMPLETION);  // cycle 0
    KTEST_ASSERT_EQ(xhci_ring_event_pop(&r, &ev), 1);
    KTEST_ASSERT_EQ(ev.p0, 0xF00Du);
}

// THE RING-FULL GUARD. An isochronous stream's producer is a process
// that can over-post, and a lapped ring goes silent for good.
KTEST("xhci-ring", "room counts down to zero and never lets a push lap the consumer") {
    struct xhci_ring r;
    xhci_ring_init(&r, g_seg, FAKE_PHYS, TEST_TRBS, 0);
    const uint32_t usable = TEST_TRBS - 1;

    KTEST_ASSERT_EQ(xhci_ring_room(&r), usable - 1);   // one slot kept empty
    for (uint32_t i = 0; i < usable - 1; i++) {
        KTEST_ASSERT_EQ(xhci_ring_room(&r), usable - 1 - i);
        xhci_ring_push(&r, i, 0, XHCI_TRB_SET_TYPE(XHCI_TRB_ISOCH));
    }
    KTEST_ASSERT_EQ(xhci_ring_room(&r), 0u);
    // Full is NOT empty: the next push would land on `dequeue`.
    KTEST_ASSERT_EQ((r.enqueue + 1) % usable, r.dequeue);
}

KTEST("xhci-ring", "an event naming a TRB frees everything up to and including it") {
    struct xhci_ring r;
    xhci_ring_init(&r, g_seg, FAKE_PHYS, TEST_TRBS, 0);
    const uint32_t usable = TEST_TRBS - 1;
    uint64_t third = 0;
    for (uint32_t i = 0; i < usable - 1; i++) {
        uint64_t at = xhci_ring_push(&r, i, 0, XHCI_TRB_SET_TYPE(XHCI_TRB_ISOCH));
        if (i == 2) third = at;
    }
    xhci_ring_consumed(&r, third);
    KTEST_ASSERT_EQ(xhci_ring_room(&r), 3u);

    // Across the wrap: consuming the slot before the Link TRB puts the
    // consumer back at the top, not ON the Link.
    xhci_ring_consumed(&r, FAKE_PHYS + (usable - 1) * sizeof(struct xhci_trb));
    KTEST_ASSERT_EQ(r.dequeue, 0u);
    // The Link TRB itself, and anything outside the segment, are ignored.
    xhci_ring_consumed(&r, FAKE_PHYS + usable * sizeof(struct xhci_trb));
    xhci_ring_consumed(&r, FAKE_PHYS - sizeof(struct xhci_trb));
    xhci_ring_consumed(&r, FAKE_PHYS + 64 * sizeof(struct xhci_trb));
    KTEST_ASSERT_EQ(r.dequeue, 0u);
}

// The link-failure rule, on the Lenovo's own readings (2026-09-28): port
// 12 went dark with an RTL8156B still plugged in, reading 0x2c0 (SS.Inactive),
// while every empty port read 0x2a0 (RxDetect). Only the first wants a warm
// reset -- an unplugged port must not be reset on every event.
KTEST("xhci", "a failed SuperSpeed link, and only that, wants a warm reset") {
    KTEST_ASSERT(xhci_portsc_needs_warm(0x000002c0));    // SS.Inactive, the Lenovo
    KTEST_ASSERT(xhci_portsc_needs_warm(0x00000340));    // Compliance Mode
    KTEST_ASSERT(!xhci_portsc_needs_warm(0x000002a0));   // RxDetect: nothing there
    KTEST_ASSERT(!xhci_portsc_needs_warm(0x00001203));   // U0, a working SS device
    KTEST_ASSERT(!xhci_portsc_needs_warm(0x00000a03));   // a USB2 low-speed device
}
