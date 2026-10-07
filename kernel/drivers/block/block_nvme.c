// NVMe namespaces, as block devices -- one per namespace, the same thin
// adapter shape as block_ahci.c. Every op is handed its device, and the
// namespace is that device's index in g_nvme_dev.
#include "block.h"
#include "nvme.h"
#include "multiboot.h"
#include "string.h"
#include "klog.h"
#include "kfmt.h"

// driver-none: the block_device shim; nvme.c is the driver

static struct block_device g_nvme_dev[NVME_MAX_NS];

static int ns_of(const struct block_device *self) { return (int)(self - g_nvme_dev); }

static uint64_t ns_count(const struct block_device *self) {
    return nvme_ns_sector_count(ns_of(self));
}
static int ns_read(const struct block_device *self, uint64_t lba, int count, void *buf) {
    return nvme_read(ns_of(self), lba, count, buf);
}
static int ns_write(const struct block_device *self, uint64_t lba, int count, const void *buf) {
    return nvme_write(ns_of(self), lba, count, buf);
}
static int ns_flush(const struct block_device *self) { return nvme_flush(ns_of(self)); }
static int ns_trim(const struct block_device *self, uint64_t lba, uint32_t count) {
    return nvme_trim(ns_of(self), lba, count);
}
static int ns_trim_ranges(const struct block_device *self, const struct blk_range *r, int n) {
    return nvme_trim_ranges(ns_of(self), r, n);
}
static int ns_batch(const struct block_device *self, struct blk_io *io, int n) {
    return nvme_submit_batch(ns_of(self), io, n);
}
static int max_xfer(const struct block_device *self) { (void)self; return nvme_max_sectors_per_xfer(); }

// `nonvme` on the boot line leaves every namespace unregistered, matched
// as a whole word -- the same reachability switch as `novirtio`/`noahci`.
static int nvme_disabled(void) {
    const char *cmdline = multiboot_cmdline();
    if (!cmdline) return 0;
    for (const char *p = cmdline; (p = k_strstr(p, "nonvme")) != 0; p += 6) {
        if (p != cmdline && p[-1] != ' ') continue;
        if (p[6] == 0 || p[6] == ' ') return 1;
    }
    return 0;
}

int blk_nvme_init(void) {
    int n = nvme_ns_count();
    if (!n) return 0;
    if (nvme_disabled()) {
        klog_write("nvme: nonvme -- no namespace registered\n");
        return 0;
    }
    // Namespace 0 is REGISTERED (it may carry the root, by precedence);
    // the rest are only TRACKED, so they are named -- nvme1, nvme2 --
    // without each taking the root from the one before.
    for (int i = 0; i < n; i++) {
        struct block_device *d = &g_nvme_dev[i];
        d->name = "nvme";
        d->model = nvme_model();
        d->pci = nvme_pci();
        d->driver = "nvme";
        d->sector_count = ns_count;
        d->read_sectors = ns_read;
        d->write_sectors = ns_write;
        d->max_sectors_per_xfer = max_xfer;
        d->persistent = 1;
        d->block_size = nvme_ns_block_size(i);
        d->submit_batch = ns_batch;
        if (nvme_has_flush()) { d->caps |= BLK_CAP_FLUSH; d->flush = ns_flush; }
        if (nvme_has_trim()) {
            d->caps |= BLK_CAP_TRIM;
            d->trim = ns_trim;
            d->trim_ranges = ns_trim_ranges;
        }
    }
    if (!blk_register(&g_nvme_dev[0])) return 0;
    for (int i = 1; i < n; i++) {
        blk_track(&g_nvme_dev[i], &g_nvme_dev[i], 0);
        klog_printf("block: %s is namespace %u (%llu sectors, %u-byte blocks)\n",
                    blk_device_name(&g_nvme_dev[i]), nvme_ns_id(i),
                    (unsigned long long)nvme_ns_sector_count(i), nvme_ns_block_size(i));
    }
    return 1;
}
