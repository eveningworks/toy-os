// virtio-rng: entropy the host supplies.
//
// The smallest complete virtio device there is. There is no request
// header, no status byte and no device-configuration space: the driver
// puts a device-WRITABLE buffer in the one queue, and the used ring's
// length field says how many bytes of entropy the device wrote into it
// (spec 5.4). Everything else -- finding the device, negotiating,
// the ring itself -- belongs to virtio_pci.c and virtqueue.c.
//
// WHY THIS EXISTS, given krandom already works
// --------------------------------------------
// Under QEMU's default CPU model the guest has neither RDSEED nor
// RDRAND, so krandom falls back to TSC jitter -- and the timestamp
// counter it measures against is itself software, which makes that
// fallback weak in exactly the environment this OS usually runs in
// (see kernel/lib/krandom.c). virtio-rng is real entropy from the host
// in that environment, one `-device virtio-rng-pci` away.
//
// IT SEEDS, IT DOES NOT SERVE
// ---------------------------
// A draw here is a device round trip plus a spin-poll, so this is not
// something krandom_u64() can call per value. It is registered as a
// krandom SOURCE instead: mixed in once at registration and every few
// hundred draws after that. Linux splits it the same way -- virtio-rng
// is an hwrng that reseeds the CRNG, never the per-call generator --
// and Windows' viorng feeds CNG rather than answering each request.
//
// THE BOUNCE BUFFER IS NOT AN OPTIMISATION
// ----------------------------------------
// The device DMAs to a PHYSICAL address, and the obvious "just hand it
// the caller's buffer" (which is what virtio_blk.c does, for buffers it
// checks) is wrong here: a caller may pass a kernel stack address, and
// kernel stacks are mapped with a guard page rather than identity
// mapped, so their virtual address is not their physical one. Filling
// through a static below 4 GiB removes the question entirely, and at
// 64 bytes a copy costs nothing.
#include "virtio.h"
#include "virtio_rng.h"
#include "krandom.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"
#include "driver.h" // DRIVER_DECLARE -- `lsdrv`
#include "pci_driver.h"

DRIVER_DECLARE("virtio-rng", "rng", "virtio entropy source");

static struct virtio_device g_dev;
static struct virtqueue g_vq;
static int g_present = 0;
static unsigned g_fills = 0;

// The DMA target. A kernel-image static below 4 GiB is its own physical
// address here, since this kernel identity-maps that range.
static uint8_t g_buf[64] __attribute__((aligned(16)));

// Not a lock -- there is no lock primitive in this kernel. It makes a
// re-entrant call FAIL rather than corrupt the ring: krandom reseeds
// from any context, including inside a syscall, and two requests
// interleaving through one shared buffer would hand out one draw twice.
static int g_busy = 0;

int virtio_rng_present(void) { return g_present; }
unsigned virtio_rng_fills(void) { return g_fills; }

// One round trip: up to sizeof g_buf bytes into g_buf. Returns how many
// the device actually wrote, which the spec allows to be fewer than
// asked for.
static uint32_t fill_once(uint32_t want) {
    if (want > sizeof g_buf) want = sizeof g_buf;

    struct virtio_sg in = { .phys = (uint64_t)(uintptr_t)g_buf, .len = want };
    int head = virtqueue_submit(&g_vq, 0, 0, &in, 1);
    if (head < 0) {
        klog_write("virtio-rng: no free descriptors\n");
        return 0;
    }
    virtqueue_kick(&g_vq);

    uint32_t used_len = 0;
    if (!virtqueue_poll(&g_vq, head, &used_len)) return 0;  // poll logged it
    if (used_len > want) return 0;   // a device claiming more than it was given
    return used_len;
}

int virtio_rng_read(void *buf, size_t n) {
    if (!g_present || !buf) return 0;
    if (!n) return 1;
    if (g_busy) return 0;

    g_busy = 1;
    uint8_t *out = (uint8_t *)buf;
    size_t done = 0;

    while (done < n) {
        uint32_t want = (uint32_t)(n - done);
        uint32_t got = fill_once(want);
        if (!got) {   // the device stopped answering; a short fill is a failure
            g_busy = 0;
            return 0;
        }
        k_memcpy(out + done, g_buf, got);
        done += got;
    }

    g_fills++;
    g_busy = 0;
    return 1;
}

// The shape krandom wants. Separate from virtio_rng_read() only so the
// size type matches the callback signature exactly.
static int rng_source(void *buf, size_t n) { return virtio_rng_read(buf, n); }

static const struct pci_match virtio_rng_matches[] = {
    VIRTIO_PCI_MATCH_MODERN(VIRTIO_ID_RNG), PCI_MATCH_ID(VIRTIO_PCI_VENDOR, 0x1005),
};

static void virtio_rng_probe(const struct pci_device *pci) {
    if (g_dev.pci) return; // virtio_test.c keeps a spare one on purpose
    g_dev.name = "virtio-rng";
    if (!virtio_pci_attach(pci, VIRTIO_ID_RNG, &g_dev)) return;

    // virtio-rng defines no device feature bits at all, so the only
    // thing negotiated is VIRTIO_F_VERSION_1, which virtio_begin() adds.
    if (!virtio_begin(&g_dev, 0)) return;   // logged its own reason

    if (!virtqueue_setup(&g_dev, 0, &g_vq)) {
        klog_write("virtio-rng: could not set up its request queue\n");
        virtio_fail(&g_dev);
        return;
    }

    virtio_driver_ok(&g_dev);
    g_present = 1;

    // Prove it answers BEFORE claiming it as an entropy source: a
    // device that is present but wedged would otherwise raise the
    // reported quality to virtio-rng while contributing nothing, which
    // is the one lie this tier must not tell.
    uint8_t probe[8];
    if (!virtio_rng_read(probe, sizeof probe)) {
        klog_write("virtio-rng: present but the first request did not complete"
                   " -- not registered as an entropy source\n");
        return;
    }

    krandom_register_source("virtio-rng", rng_source, KRANDOM_VIRTIO);
    klog_printf("virtio-rng: entropy source registered (krandom is now %s)\n",
                krandom_quality_name(krandom_quality()));
}
PCI_DRIVER("virtio-rng", virtio_rng_matches, virtio_rng_probe);
