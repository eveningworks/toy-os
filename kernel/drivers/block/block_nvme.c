// NVMe namespaces, as block devices -- one per namespace, the same thin
// adapter shape as block_ahci.c. The ops take no context (block.h), so
// each namespace slot gets its own thunks, as block_part.c's windows do.
#include "block.h"
#include "nvme.h"
#include "multiboot.h"
#include "string.h"
#include "klog.h"
#include "kfmt.h"

// driver-none: the block_device shim; nvme.c is the driver

static struct block_device g_nvme_dev[NVME_MAX_NS];

#define NS_THUNKS(i)                                                                   \
    static uint32_t n##i##_count(void) { return nvme_ns_sector_count(i); }             \
    static int n##i##_read(uint32_t l, int c, void *b) { return nvme_read(i, l, c, b); } \
    static int n##i##_write(uint32_t l, int c, const void *b) { return nvme_write(i, l, c, b); } \
    static int n##i##_flush(void) { return nvme_flush(i); }                            \
    static int n##i##_trim(uint32_t l, uint32_t c) { return nvme_trim(i, l, c); }      \
    static int n##i##_trim_ranges(const struct blk_range *r, int n) { return nvme_trim_ranges(i, r, n); } \
    static int n##i##_batch(struct blk_io *io, int n) { return nvme_submit_batch(i, io, n); }

NS_THUNKS(0) NS_THUNKS(1) NS_THUNKS(2) NS_THUNKS(3)
_Static_assert(NVME_MAX_NS == 4, "one NS_THUNKS line per namespace slot");

static int max_xfer(void) { return nvme_max_sectors_per_xfer(); }

#define NS_OPS(i) { n##i##_count, n##i##_read, n##i##_write, n##i##_flush, \
                    n##i##_trim, n##i##_trim_ranges, n##i##_batch }

static const struct {
    uint32_t (*count)(void);
    int (*read)(uint32_t, int, void *);
    int (*write)(uint32_t, int, const void *);
    int (*flush)(void);
    int (*trim)(uint32_t, uint32_t);
    int (*trim_ranges)(const struct blk_range *, int);
    int (*batch)(struct blk_io *, int);
} g_ops[NVME_MAX_NS] = { NS_OPS(0), NS_OPS(1), NS_OPS(2), NS_OPS(3) };

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
        d->sector_count = g_ops[i].count;
        d->read_sectors = g_ops[i].read;
        d->write_sectors = g_ops[i].write;
        d->max_sectors_per_xfer = max_xfer;
        d->persistent = 1;
        d->block_size = nvme_ns_block_size(i);
        d->submit_batch = g_ops[i].batch;
        if (nvme_has_flush()) { d->caps |= BLK_CAP_FLUSH; d->flush = g_ops[i].flush; }
        if (nvme_has_trim()) {
            d->caps |= BLK_CAP_TRIM;
            d->trim = g_ops[i].trim;
            d->trim_ranges = g_ops[i].trim_ranges;
        }
    }
    if (!blk_register(&g_nvme_dev[0])) return 0;
    for (int i = 1; i < n; i++) {
        blk_track(&g_nvme_dev[i], &g_nvme_dev[i], 0);
        klog_printf("block: %s is namespace %u (%u sectors, %u-byte blocks)\n",
                    blk_device_name(&g_nvme_dev[i]), nvme_ns_id(i),
                    nvme_ns_sector_count(i), nvme_ns_block_size(i));
    }
    return 1;
}
