// KTESTs for the virtio modern PCI transport.
//
// THE GATE IS THE DESIGN HERE, so read it before adding a test.
//
// On a machine with no virtio device every test in this file would
// pass by skipping, which is the shape this repo has shipped real bugs
// behind ("it responds" is not "it is drawn"). So the skip condition is
// deliberately INDEPENDENT of the code under test: it scans the PCI
// table -- pci.c's, not virtio's -- for vendor 0x1AF4.
//
// And the asymmetry that makes it worth anything: if a virtio device IS
// on the bus and the transport did NOT claim it, that is a FAILURE, not
// a skip. A broken capability walk therefore reddens the suite instead
// of quietly turning it green.
//
// tools/ktest_run.py attaches `-device virtio-rng-pci` for exactly this
// reason. rng rather than blk on purpose: it is not the device under
// test anywhere else, so it also exercises a virtio device of the WRONG
// type being declined.
#include "virtio.h"
#include "pci.h"
#include "pci_internal.h"
#include "ktest.h"

// Is there ANY virtio device on the bus? Asked through pci.c so that a
// bug in virtio_pci_find() cannot make this answer "no" and skip.
static int virtio_hw_present(void) {
    for (int i = 0; i < pci_device_count(); i++) {
        const struct pci_device *d = pci_device_at(i);
        if (d && d->vendor_id == VIRTIO_PCI_VENDOR) return 1;
    }
    return 0;
}

KTEST("virtio", "a virtio device on the bus is claimed and its windows mapped") {
    if (!virtio_hw_present()) KTEST_SKIP("no virtio device on this machine");

    struct virtio_device d = {0};
    d.name = "virtio-rng";
    // If this fails while a 1AF4 device is present, the capability walk
    // or the BAR decode is broken -- which is the whole point of the
    // gate above being independent of them.
    KTEST_ASSERT(virtio_pci_find(VIRTIO_ID_RNG, 0, &d));

    // Every required window must have been found and must be reachable.
    KTEST_ASSERT(d.common != 0);
    KTEST_ASSERT(d.notify != 0);
    KTEST_ASSERT(d.isr != 0);
    KTEST_ASSERT(d.pci != 0);
    KTEST_ASSERT_EQ(d.pci->vendor_id, VIRTIO_PCI_VENDOR);

    // The windows live below 4 GiB or map_window() would have refused
    // them -- this kernel identity-maps only that much.
    KTEST_ASSERT((uint64_t)(uintptr_t)d.common < 0x100000000ull);
    KTEST_ASSERT((uint64_t)(uintptr_t)d.notify < 0x100000000ull);
}

// The device-status handshake, end to end, checked through the device's
// own status register rather than through our return value alone.
KTEST("virtio", "feature negotiation reaches FEATURES_OK and DRIVER_OK") {
    if (!virtio_hw_present()) KTEST_SKIP("no virtio device on this machine");

    struct virtio_device d = {0};
    d.name = "virtio-rng";
    KTEST_ASSERT(virtio_pci_find(VIRTIO_ID_RNG, 0, &d));

    // Ask for nothing device-specific; VERSION_1 is added by virtio_begin.
    KTEST_ASSERT(virtio_begin(&d, 0));

    // Modern transport: this is the bit whose absence is a hard refusal.
    KTEST_ASSERT(virtio_has_feature(&d, VIRTIO_F_VERSION_1));

    // Deliberately NOT negotiated -- see barrier.h on why EVENT_IDX's
    // absence is what makes the store-load fence unreachable.
    KTEST_ASSERT_EQ(virtio_has_feature(&d, VIRTIO_F_EVENT_IDX), 0);
    KTEST_ASSERT_EQ(virtio_has_feature(&d, VIRTIO_F_INDIRECT_DESC), 0);

    // Read the status register back: the driver's own bookkeeping
    // agreeing with itself would prove nothing.
    volatile uint8_t *status = d.common + VIRTIO_COMMON_STATUS;
    KTEST_ASSERT(*status & VIRTIO_STATUS_FEATURES_OK);
    KTEST_ASSERT(*status & VIRTIO_STATUS_DRIVER);
    KTEST_ASSERT(*status & VIRTIO_STATUS_ACKNOWLEDGE);
    KTEST_ASSERT_EQ(*status & VIRTIO_STATUS_FAILED, 0);

    virtio_driver_ok(&d);
    KTEST_ASSERT(*status & VIRTIO_STATUS_DRIVER_OK);

    // Leave the device reset rather than half-owned: this test claimed
    // a real device, and a later test (or a real driver) must find it
    // in a clean state. Establishing a precondition, not tidiness.
    *(volatile uint8_t *)(d.common + VIRTIO_COMMON_STATUS) = 0;
}

// A virtio device of a type nothing here drives must be declined, and
// declined SILENTLY -- the ordinary case on every default boot.
KTEST("virtio", "a device of the wrong type is declined") {
    if (!virtio_hw_present()) KTEST_SKIP("no virtio device on this machine");

    struct virtio_device d = {0};
    // The test harness attaches an rng, never a GPU or an input device.
    KTEST_ASSERT_EQ(virtio_pci_find(VIRTIO_ID_GPU, 0, &d), 0);
    KTEST_ASSERT_EQ(virtio_pci_find(VIRTIO_ID_INPUT, 0, &d), 0);

    // ...and asking for an index past the end of what exists.
    KTEST_ASSERT_EQ(virtio_pci_find(VIRTIO_ID_RNG, 99, &d), 0);

    // A NULL out pointer is refused rather than written through.
    KTEST_ASSERT_EQ(virtio_pci_find(VIRTIO_ID_RNG, 0, 0), 0);
}

// --- the virtqueue, end to end ---------------------------------------
//
// virtio-rng is the smallest possible consumer of a virtqueue: one
// queue, put a device-WRITABLE buffer in, get entropy back. That makes
// it the right thing to prove the ring with -- if this passes, then
// descriptor chaining, the available ring, the doorbell address
// arithmetic, DMA into guest memory and the used ring all work. A disk
// driver on top is then payload rather than mechanism.

// The device DMAs into this, so it must live at a physical address --
// which for a kernel-image static below 4 GiB is its own address, since
// this kernel identity-maps that range. Static rather than on the stack
// because the kernel stack has a 1 KiB frame budget and the device may
// still be writing after a timeout.
static uint8_t g_entropy[64];

KTEST("virtio", "a buffer round-trips through a virtqueue") {
    if (!virtio_hw_present()) KTEST_SKIP("no virtio device on this machine");

    struct virtio_device d = {0};
    d.name = "virtio-rng";
    KTEST_ASSERT(virtio_pci_find(VIRTIO_ID_RNG, 0, &d));
    KTEST_ASSERT(virtio_begin(&d, 0));

    struct virtqueue vq;
    KTEST_ASSERT(virtqueue_setup(&d, 0, &vq));
    KTEST_ASSERT(vq.size > 0);
    KTEST_ASSERT(vq.notify != 0);

    // The spec forbids using a queue before DRIVER_OK.
    virtio_driver_ok(&d);

    // A fresh pool: every descriptor free.
    KTEST_ASSERT_EQ(virtqueue_free_count(&vq), vq.size);

    // Fill with a known pattern so "the device wrote here" is
    // distinguishable from "this memory happened to be zero". A device
    // that writes nothing at all leaves the pattern intact.
    for (unsigned i = 0; i < sizeof g_entropy; i++) g_entropy[i] = 0xA5;

    struct virtio_sg in = { .phys = (uint64_t)(uintptr_t)g_entropy, .len = sizeof g_entropy };
    int head = virtqueue_submit(&vq, 0, 0, &in, 1);
    KTEST_ASSERT(head >= 0);
    KTEST_ASSERT_EQ(virtqueue_free_count(&vq), vq.size - 1);

    virtqueue_kick(&vq);

    uint32_t len = 0;
    KTEST_ASSERT(virtqueue_poll(&vq, head, &len));
    KTEST_ASSERT(len > 0);
    KTEST_ASSERT(len <= sizeof g_entropy);

    // The device really wrote: the pattern is gone. (Entropy could in
    // principle produce 0xA5 bytes, but not all of them -- so this asks
    // whether ANY byte in what it claims to have written changed.)
    int changed = 0;
    for (uint32_t i = 0; i < len; i++) if (g_entropy[i] != 0xA5) changed = 1;
    KTEST_ASSERT(changed);

    // The chain came back to the pool. A chain that is never returned
    // leaks silently and only kills the driver much later.
    KTEST_ASSERT_EQ(virtqueue_free_count(&vq), vq.size);

    virtqueue_teardown(&vq);
    *(volatile uint8_t *)(d.common + VIRTIO_COMMON_STATUS) = 0;  // leave it reset
}

// The free-descriptor pool must BALANCE across many requests, not just
// one. This is the check that catches a chain never returned -- which
// otherwise shows up as the driver dying after a few hundred requests,
// hours into a session, with nothing pointing at the cause.
KTEST("virtio", "the descriptor pool balances across many transfers") {
    if (!virtio_hw_present()) KTEST_SKIP("no virtio device on this machine");

    struct virtio_device d = {0};
    d.name = "virtio-rng";
    KTEST_ASSERT(virtio_pci_find(VIRTIO_ID_RNG, 0, &d));
    KTEST_ASSERT(virtio_begin(&d, 0));

    struct virtqueue vq;
    KTEST_ASSERT(virtqueue_setup(&d, 0, &vq));
    virtio_driver_ok(&d);

    uint16_t start_free = virtqueue_free_count(&vq);
    KTEST_ASSERT_EQ(start_free, vq.size);

    // More iterations than the queue is deep would be better still, but
    // the pool is 256 and each request takes one descriptor -- 16 is
    // enough to catch a leak of one per request while keeping the test
    // quick, and the balance assertion is exact rather than approximate.
    for (int i = 0; i < 16; i++) {
        struct virtio_sg in = { .phys = (uint64_t)(uintptr_t)g_entropy, .len = 8 };
        int head = virtqueue_submit(&vq, 0, 0, &in, 1);
        KTEST_ASSERT(head >= 0);
        virtqueue_kick(&vq);
        uint32_t len = 0;
        KTEST_ASSERT(virtqueue_poll(&vq, head, &len));
    }

    KTEST_ASSERT_EQ(virtqueue_free_count(&vq), start_free);
    KTEST_ASSERT_EQ(virtqueue_lost_chains(), 0);

    virtqueue_teardown(&vq);
    *(volatile uint8_t *)(d.common + VIRTIO_COMMON_STATUS) = 0;
}
