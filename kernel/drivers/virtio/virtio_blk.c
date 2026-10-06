// virtio-blk: a disk that is not ATA.
//
// The whole driver is the request FORMAT plus a capacity read -- the
// mechanism (finding the device, negotiating, the ring) belongs to
// virtio_pci.c and virtqueue.c and is shared with every other virtio
// device. That split is the reason this file is short.
//
// A request is a descriptor chain of two or three buffers:
//
//     desc[0]  header  {type, reserved, sector}   16 B  device READS
//     desc[1]  data    count * 512 bytes                device WRITES (read)
//                                                       device READS  (write)
//     desc[2]  status  1 byte                            device WRITES
//
// A FLUSH has no data buffer, so it is two descriptors. Getting the
// direction of desc[1] wrong is the mistake worth watching for: a read
// whose data buffer is not marked device-writable completes normally
// and silently returns whatever the buffer already held.
#include "kmutex.h" // one request at a time -- see g_blk_lock
#include "virtio.h"
#include "virtio_blk.h"
#include "block.h"
#include "klog.h"
#include "paging.h"
#include "kfmt.h"
#include "string.h"
#include "pci_driver.h"

// driver-none: the virtio transport half; block_virtio.c declares the driver

#define VIRTIO_BLK_T_IN    0   // read
#define VIRTIO_BLK_T_OUT   1   // write
#define VIRTIO_BLK_T_FLUSH 4
#define VIRTIO_BLK_T_DISCARD 11

#define VIRTIO_BLK_S_OK     0
#define VIRTIO_BLK_S_IOERR  1
#define VIRTIO_BLK_S_UNSUPP 2

// Device-configuration offsets (spec 5.2.4).
#define VIRTIO_BLK_CFG_CAPACITY 0x00  // u64, ALWAYS in 512-byte sectors
#define VIRTIO_BLK_CFG_SIZE_MAX 0x08  // u32
#define VIRTIO_BLK_CFG_BLK_SIZE 0x14  // u32, the LOGICAL block

#define VIRTIO_BLK_F_SIZE_MAX (1ull << 1)
#define VIRTIO_BLK_F_SEG_MAX  (1ull << 2)
#define VIRTIO_BLK_F_RO       (1ull << 5)
#define VIRTIO_BLK_F_BLK_SIZE (1ull << 6)
#define VIRTIO_BLK_F_FLUSH    (1ull << 9)
#define VIRTIO_BLK_F_DISCARD  (1ull << 13)

// The discard half of the config block (spec 5.2.4). Only meaningful
// once VIRTIO_BLK_F_DISCARD is negotiated; reading them otherwise is
// reading whatever the device left there.
#define VIRTIO_BLK_CFG_MAX_DISCARD_SECTORS 0x24
#define VIRTIO_BLK_CFG_MAX_DISCARD_SEG     0x28
#define VIRTIO_BLK_CFG_DISCARD_ALIGNMENT   0x2C

// One discard SEGMENT. The device READS this, exactly like a write's
// data buffer -- a discard is a data-out request whose payload happens
// to describe ranges rather than contain them.
struct virtio_blk_discard_seg {
    uint64_t sector;
    uint32_t num_sectors;
    uint32_t flags;      // bit 0 is "unmap", and only write-zeroes uses it
};

// One transfer at a time, so one header and one status byte. Both are
// DMA targets, so they must be in memory the device can reach: a
// kernel-image static below 4 GiB is its own physical address here.
// `status` is volatile because the DEVICE writes it and the compiler
// must re-read it after the poll rather than caching the pre-request
// value.
struct virtio_blk_req_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

static struct virtio_blk_req_hdr g_hdr __attribute__((aligned(16)));
static volatile uint8_t g_status;

// One segment, because this driver issues one discard at a time and the
// block layer hands it one contiguous range. Static for the same reason
// the header is: it is a DMA target and must be at a physical address
// the device can reach.
static struct virtio_blk_discard_seg g_discard __attribute__((aligned(16)));
static uint32_t g_max_discard;    // sectors, 0 when the feature is absent

static struct virtio_device g_dev;
static struct virtqueue g_vq;
static int g_present = 0;
static int g_readonly = 0;
static uint64_t g_capacity = 0;   // 512-byte sectors, as the device reports
static uint32_t g_max_xfer = 128; // sectors per transfer
static uint32_t g_blk_size = 512;  // bytes per logical block

// ONE REQUEST AT A TIME, through the one shared header and status byte:
// two interleaving would produce silently wrong data. A LOCK, so a
// second caller WAITS -- it was a busy flag that made the second one
// fail, from before this kernel had a lock primitive (kmutex.h).
static struct kmutex g_blk_lock;

int virtio_blk_present(void) { return g_present; }
const struct pci_device *virtio_blk_pci(void) { return g_present ? g_dev.pci : 0; }
int virtio_blk_max_sectors_per_xfer(void) { return (int)g_max_xfer; }
uint32_t virtio_blk_block_size(void) { return g_blk_size; }
int virtio_blk_flush_supported(void) {
    return g_present && virtio_has_feature(&g_dev, VIRTIO_BLK_F_FLUSH);
}

uint32_t virtio_blk_sector_count(void) {
    // The block layer indexes sectors with a uint32_t, so a device
    // larger than 2 TiB is exposed truncated rather than wrapped. ATA
    // has the same ceiling (LBA28), so this is not a new limitation --
    // but it is one worth saying out loud rather than silently.
    if (g_capacity > 0xFFFFFFFFull) return 0xFFFFFFFFu;
    return (uint32_t)g_capacity;
}

// The one request path. `data` may be NULL for a FLUSH.
static int request_locked(uint32_t type, uint64_t sector, void *data, uint32_t len, int device_writes);

static int do_request(uint32_t type, uint64_t sector, void *data, uint32_t len, int device_writes) {
    if (!g_present) return 0;
    kmutex_lock(&g_blk_lock);
    int r = request_locked(type, sector, data, len, device_writes);
    kmutex_unlock(&g_blk_lock);
    return r;
}

static int request_locked(uint32_t type, uint64_t sector, void *data, uint32_t len, int device_writes) {

    // The device DMAs straight into the caller's buffer -- no bounce
    // buffer, because kernel memory is identity-mapped and therefore a
    // virtually-contiguous kernel buffer is physically contiguous by
    // construction. Virtio addresses are 64-bit, so the only bound is
    // the map itself (a kmalloc buffer may be above 4 GiB now).
    if (data && ((uint64_t)(uintptr_t)data + len) > paging_identity_limit()) {
        klog_write(KLOG_ERR "virtio-blk: buffer outside the identity map refused\n");
        return 0;
    }

    g_hdr.type = type;
    g_hdr.reserved = 0;
    g_hdr.sector = sector;
    g_status = 0xFF;   // not a legal status, so "untouched" is visible

    struct virtio_sg out[2];
    struct virtio_sg in[2];
    int n_out = 0, n_in = 0;

    out[n_out].phys = (uint64_t)(uintptr_t)&g_hdr;
    out[n_out].len = sizeof g_hdr;
    n_out++;

    if (data) {
        // Direction is from the DEVICE's point of view: a disk READ is
        // a buffer the device WRITES.
        if (device_writes) { in[n_in].phys = (uint64_t)(uintptr_t)data; in[n_in].len = len; n_in++; }
        else               { out[n_out].phys = (uint64_t)(uintptr_t)data; out[n_out].len = len; n_out++; }
    }

    in[n_in].phys = (uint64_t)(uintptr_t)&g_status;
    in[n_in].len = 1;
    n_in++;

    int head = virtqueue_submit(&g_vq, out, n_out, in, n_in);
    if (head < 0) {
        klog_write("virtio-blk: no free descriptors\n");
        return 0;
    }
    virtqueue_kick(&g_vq);

    uint32_t used_len = 0;
    int done = virtqueue_poll(&g_vq, head, &used_len);

    if (!done) {
        // WHICH REQUEST, by name. virtqueue.c cannot say -- it is
        // generic -- and its message reports a chain number and a
        // descriptor count, from which a reader has to infer that a
        // 2-descriptor chain is a FLUSH. Saying so here turns a bug
        // report into a diagnosis.
        klog_printf("virtio-blk: %s at sector %llu never completed\n",
                    type == VIRTIO_BLK_T_IN ? "read" :
                    type == VIRTIO_BLK_T_OUT ? "write" :
                    type == VIRTIO_BLK_T_FLUSH ? "FLUSH (a journal barrier)" :
                    type == VIRTIO_BLK_T_DISCARD ? "discard" : "request",
                    (unsigned long long)sector);
        return 0;   // virtqueue_poll() logged, and leaked the chain
    }
    if (g_status != VIRTIO_BLK_S_OK) {
        klog_printf(KLOG_ERR "virtio-blk: request type %u at sector %llu failed, status %u\n",
                    type, (unsigned long long)sector, (unsigned)g_status);
        return 0;
    }
    return 1;
}

int virtio_blk_read_sectors(uint32_t lba, int count, void *buf) {
    if (!buf || count <= 0 || (uint32_t)count > g_max_xfer) return 0;
    return do_request(VIRTIO_BLK_T_IN, lba, buf, (uint32_t)count * 512u, 1);
}

int virtio_blk_write_sectors(uint32_t lba, int count, const void *buf) {
    if (!buf || count <= 0 || (uint32_t)count > g_max_xfer) return 0;
    if (g_readonly) {
        klog_write(KLOG_ERR "virtio-blk: device is read-only, write refused\n");
        return 0;
    }
    return do_request(VIRTIO_BLK_T_OUT, lba, (void *)buf, (uint32_t)count * 512u, 0);
}

int virtio_blk_flush(void) {
    if (!virtio_blk_flush_supported()) return 1;  // nothing to do, not a failure
    return do_request(VIRTIO_BLK_T_FLUSH, 0, 0, 0, 0);
}

int virtio_blk_discard_supported(void) {
    return g_present && g_max_discard > 0;
}

// A discard is refused rather than split when it is larger than the
// device will take in one segment: a partial discard reporting success
// would leave the caller believing blocks were released that were not,
// and the block layer's contract is refused-never-short.
int virtio_blk_discard(uint32_t lba, uint32_t count) {
    if (!virtio_blk_discard_supported() || count == 0) return 0;
    if (g_readonly) return 0;
    if (lba >= g_capacity || count > g_capacity - lba) return 0;
    if (count > g_max_discard) return 0;

    g_discard.sector = lba;
    g_discard.num_sectors = count;
    g_discard.flags = 0;
    return do_request(VIRTIO_BLK_T_DISCARD, 0, &g_discard, sizeof g_discard, 0);
}

static const struct pci_match virtio_blk_matches[] = {
    VIRTIO_PCI_MATCH_MODERN(VIRTIO_ID_BLK), PCI_MATCH_ID(VIRTIO_PCI_VENDOR, 0x1001),
};

static int virtio_blk_probe(const struct pci_device *pci) {
    if (g_dev.pci) return pci_probe_decline(pci, "a second virtio disk; one is driven");
    g_dev.name = "virtio-blk";
    if (!virtio_pci_attach(pci, VIRTIO_ID_BLK, &g_dev))
        return pci_probe_decline(pci, "the virtio transport did not attach");

    uint64_t wanted = VIRTIO_BLK_F_FLUSH | VIRTIO_BLK_F_SIZE_MAX
                    | VIRTIO_BLK_F_SEG_MAX | VIRTIO_BLK_F_RO
                    | VIRTIO_BLK_F_DISCARD | VIRTIO_BLK_F_BLK_SIZE;
    if (!virtio_begin(&g_dev, wanted)) return pci_probe_decline(pci, "feature negotiation failed");

    if (!virtqueue_setup(&g_dev, 0, &g_vq)) {
        virtio_fail(&g_dev);
        return pci_probe_decline(pci, "could not set up its request queue");
    }

    // Capacity is readable after FEATURES_OK; only USING a queue has to
    // wait for DRIVER_OK.
    g_capacity = virtio_cfg_read64(&g_dev, VIRTIO_BLK_CFG_CAPACITY);
    g_readonly = virtio_has_feature(&g_dev, VIRTIO_BLK_F_RO);
    // Every sector field in the protocol stays in 512-byte units whatever
    // this says -- the block layer's own rule -- so only the block layer
    // needs to know it, to refuse a transfer QEMU would fail with IOERR.
    if (virtio_has_feature(&g_dev, VIRTIO_BLK_F_BLK_SIZE))
        g_blk_size = virtio_cfg_read32(&g_dev, VIRTIO_BLK_CFG_BLK_SIZE);

    if (virtio_has_feature(&g_dev, VIRTIO_BLK_F_SIZE_MAX)) {
        uint32_t size_max = virtio_cfg_read32(&g_dev, VIRTIO_BLK_CFG_SIZE_MAX);
        uint32_t sectors = size_max / 512u;
        if (sectors > 0 && sectors < g_max_xfer) g_max_xfer = sectors;
    }

    // A device may negotiate DISCARD and still advertise a zero maximum,
    // which means it cannot actually take one -- so the CAPABILITY is
    // the maximum, not the feature bit. QEMU reports zero here unless
    // the drive was given `discard=unmap`.
    if (virtio_has_feature(&g_dev, VIRTIO_BLK_F_DISCARD)) {
        g_max_discard = virtio_cfg_read32(&g_dev, VIRTIO_BLK_CFG_MAX_DISCARD_SECTORS);
    }

    virtio_driver_ok(&g_dev);
    g_present = 1;

    if (g_capacity > 0xFFFFFFFFull) {
        klog_printf("virtio-blk: capacity %llu sectors exceeds the block layer's 32-bit"
                    " sector index -- exposing 4294967295\n", (unsigned long long)g_capacity);
    }
    klog_printf("virtio-blk: %llu sectors, %u-byte blocks, max %u per transfer, flush %s, discard %s%s\n",
                (unsigned long long)g_capacity, g_blk_size, g_max_xfer,
                virtio_blk_flush_supported() ? "yes" : "no",
                g_max_discard ? "yes" : "no",
                g_readonly ? ", READ-ONLY" : "");
    return 0;
}
PCI_DRIVER("virtio-blk", virtio_blk_matches, virtio_blk_probe);
