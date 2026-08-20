// KTESTs for virtio-rng, exercised through the LIVE driver.
//
// Deliberately not the shape of virtio_test.c beside it. Those tests
// claim a spare device and drive the transport by hand; these ask
// whether the device the kernel actually brought up at boot is working,
// which is the only way to catch "the driver initialised, reported
// itself, and answers nothing".
//
// THE GATE, and it is the same rule as virtio_test.c's: the skip
// condition is INDEPENDENT of the code under test. It scans pci.c's own
// table for an rng, so a driver that failed to claim its device cannot
// make these tests skip -- it makes them FAIL, which is the point.
#include "virtio.h"
#include "virtio_rng.h"
#include "krandom.h"
#include "pci.h"
#include "pci_internal.h"
#include "scheduler.h"
#include "ktest.h"

// The spec's PCI device ids for an entropy device: modern, and the
// transitional/legacy id QEMU's default virtio-rng-pci presents.
// Duplicated from virtio_test.c on purpose -- a test file's gate must
// not depend on another test file being correct.
#define PCI_ID_RNG_MODERN 0x1044
#define PCI_ID_RNG_LEGACY 0x1005

static int rng_on_bus(void) {
    for (int i = 0; i < pci_device_count(); i++) {
        const struct pci_device *d = pci_device_at(i);
        if (!d || d->vendor_id != VIRTIO_PCI_VENDOR) continue;
        if (d->device_id == PCI_ID_RNG_MODERN || d->device_id == PCI_ID_RNG_LEGACY) return 1;
    }
    return 0;
}

// The device DMAs into the driver's own bounce buffer, not into these,
// so a stack array would be safe -- but the ring-0 frame budget is
// 1 KiB, so anything of size lives here regardless.
static uint8_t g_a[200];
static uint8_t g_b[200];

KTEST("virtio-rng", "the device on the bus was claimed and raised krandom's quality") {
    if (!rng_on_bus()) KTEST_SKIP("no virtio-rng on this machine");

    // Present, because a device IS on the bus. A driver that declined
    // it fails here rather than skipping.
    KTEST_ASSERT(virtio_rng_present());

    // And it must have been registered: virtio_rng_init() only calls
    // krandom_register_source() after a probe request completes, so
    // this asserts the device answered at BOOT, not merely that it was
    // found. The comparison is >= because the enum is ordered by trust
    // and a CPU with RDSEED keeps the higher tier (--cpu max).
    KTEST_ASSERT(krandom_quality() >= KRANDOM_VIRTIO);
}

KTEST("virtio-rng", "a fill writes exactly the requested bytes") {
    if (!rng_on_bus()) KTEST_SKIP("no virtio-rng on this machine");
    KTEST_ASSERT(virtio_rng_present());

    for (unsigned i = 0; i < sizeof g_a; i++) g_a[i] = 0xA5;

    // 100 bytes is deliberately larger than the driver's 64-byte bounce
    // buffer AND not a multiple of it: this is the only test that
    // reaches the chunking loop, and a driver that filled one chunk and
    // returned success would leave bytes 64..99 untouched below.
    const unsigned n = 100;
    unsigned fills_before = virtio_rng_fills();

    // The driver refuses a re-entrant request (one shared bounce
    // buffer), and krandom reseeds from any context every few hundred
    // draws -- so this establishes the precondition rather than
    // tolerating a spurious 0.
    scheduler_preempt_disable();
    int ok = virtio_rng_read(g_a, n);
    scheduler_preempt_enable();
    KTEST_ASSERT(ok);
    KTEST_ASSERT_EQ(virtio_rng_fills(), fills_before + 1);

    // Every requested byte was written. Entropy can legitimately
    // produce an 0xA5, so this asks whether ANY byte in each 8-byte
    // window changed -- a whole window surviving means that window was
    // never written, which is what a short fill looks like.
    for (unsigned w = 0; w < n; w += 8) {
        int changed = 0;
        for (unsigned i = w; i < w + 8 && i < n; i++) if (g_a[i] != 0xA5) changed = 1;
        KTEST_ASSERT(changed); // an 8-byte window of the fill was left untouched
    }

    // ...and nothing past the request. The bounce buffer is 64 bytes,
    // so an off-by-a-chunk would land here.
    for (unsigned i = n; i < sizeof g_a; i++) {
        KTEST_ASSERT_EQ(g_a[i], 0xA5); // virtio-rng wrote past the requested length
    }
}

KTEST("virtio-rng", "two fills differ") {
    if (!rng_on_bus()) KTEST_SKIP("no virtio-rng on this machine");
    KTEST_ASSERT(virtio_rng_present());

    scheduler_preempt_disable();
    int ok_a = virtio_rng_read(g_a, 32);
    int ok_b = virtio_rng_read(g_b, 32);
    scheduler_preempt_enable();
    KTEST_ASSERT(ok_a);
    KTEST_ASSERT(ok_b);

    // What this catches is not weak entropy -- 32 bytes cannot judge
    // that -- but the failure modes that DO happen: a driver handing
    // back its stale bounce buffer, or a device that answers once and
    // then replays. Two independent 32-byte draws colliding by chance
    // is not an event that occurs.
    int differ = 0;
    for (int i = 0; i < 32; i++) if (g_a[i] != g_b[i]) differ = 1;
    KTEST_ASSERT(differ); // two virtio-rng draws returned identical bytes
}

KTEST("virtio-rng", "a zero-length fill succeeds and a null buffer is refused") {
    if (!rng_on_bus()) KTEST_SKIP("no virtio-rng on this machine");
    KTEST_ASSERT(virtio_rng_present());

    unsigned fills_before = virtio_rng_fills();
    KTEST_ASSERT(virtio_rng_read(g_a, 0));
    // Nothing was asked of the device, so nothing should have been.
    KTEST_ASSERT_EQ(virtio_rng_fills(), fills_before);

    KTEST_ASSERT_EQ(virtio_rng_read(0, 8), 0);
}
