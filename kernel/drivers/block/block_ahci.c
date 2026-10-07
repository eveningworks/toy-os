// Every AHCI drive, as a block device -- one per drive, the same thin
// adapter shape as block_nvme.c: the driver keeps its behaviour and this
// only states which of it the block layer may use. Every op is handed
// its device, and the drive is that device's index in g_ahci_dev.
#include "block.h"
#include "ahci.h"
#include "multiboot.h"
#include "string.h"
#include "klog.h"
#include "kfmt.h"

// driver-none: the block_device shim; ahci.c is the driver

static struct block_device g_ahci_dev[AHCI_MAX_DRIVES];

static int drive_of(const struct block_device *self) { return (int)(self - g_ahci_dev); }

static uint64_t ahci_blk_sector_count(const struct block_device *self) {
    return ahci_sector_count(drive_of(self));
}
static int ahci_blk_read(const struct block_device *self, uint64_t lba, int count, void *buf) {
    return ahci_read_sectors(drive_of(self), lba, count, buf);
}
static int ahci_blk_write(const struct block_device *self, uint64_t lba, int count, const void *buf) {
    return ahci_write_sectors(drive_of(self), lba, count, buf);
}
static int ahci_blk_max_xfer(const struct block_device *self) {
    return ahci_max_sectors_per_xfer(drive_of(self));
}
static int ahci_blk_flush(const struct block_device *self) { return ahci_flush(drive_of(self)); }
static int ahci_blk_trim(const struct block_device *self, uint64_t lba, uint32_t count) {
    return ahci_trim(drive_of(self), lba, count);
}
static int ahci_blk_trim_ranges(const struct block_device *self, const struct blk_range *r, int n) {
    return ahci_trim_ranges(drive_of(self), r, n);
}
static int ahci_blk_batch(const struct block_device *self, struct blk_io *io, int n) {
    return ahci_submit_batch(drive_of(self), io, n);
}

// FLUSH is unconditional and means a real FLUSH CACHE EXT reaching the
// drive: there is no write-back cache above this one (ahci.h says why),
// so the bit and the function agree in the way blk_register() checks.
//
// TRIM is decided at REGISTRATION from the drive's own IDENTIFY answer,
// which is block_virtio.c's rule rather than block_ata.c's: IDENTIFY has
// completed by the time this runs, so the bit can mean exactly "a TRIM
// issued now goes out" instead of being advertised blind.
//
// Static and MUTABLE: blk_register() keeps the POINTER, so a stack copy
// would leave the block layer reading a dead frame.
static void fill(struct block_device *d, int drive) {
    d->name = "ahci";
    d->driver = "ahci";
    d->sector_count = ahci_blk_sector_count;
    d->read_sectors = ahci_blk_read;
    d->write_sectors = ahci_blk_write;
    d->max_sectors_per_xfer = ahci_blk_max_xfer;
    d->persistent = 1;
    d->caps = BLK_CAP_FLUSH;
    d->flush = ahci_blk_flush;
    if (ahci_trim_supported(drive)) {
        d->caps |= BLK_CAP_TRIM;
        d->trim = ahci_blk_trim;
        d->trim_ranges = ahci_blk_trim_ranges;
    }
    d->model = ahci_model(drive);
    d->pci = ahci_drive_pci(drive);
}

// `noahci` on the boot line forces the ATA fallback, matched as a whole
// word. Same shape and same purpose as `novirtio`: a fallback nothing
// can reach is a guess.
static int word_on_cmdline(const char *word) {
    const char *cmdline = multiboot_cmdline();
    if (!cmdline) return 0;
    uint32_t n = (uint32_t)k_strlen(word);
    for (const char *p = cmdline; (p = k_strstr(p, word)) != 0; p += n) {
        if (p != cmdline && p[-1] != ' ') continue;
        char after = p[n];
        if (after == 0 || after == ' ') return 1;
    }
    return 0;
}

static int ahci_disabled(void) { return word_on_cmdline("noahci"); }

static int g_registered;   // how many of g_ahci_dev reached the table

int blk_ahci_init(void) {
    int n = ahci_drive_count();
    if (!n || ahci_disabled()) return 0;

    // Several commands in flight, when the driver queues any -- and
    // `noncq` takes that away without touching anything else, so one
    // build can be measured both ways (Linux's libata.force=noncq).
    int ncq_ok = !word_on_cmdline("noncq");
    for (int i = 0; i < n; i++) {
        fill(&g_ahci_dev[i], i);
        if (ahci_ncq_depth(i) && ncq_ok) g_ahci_dev[i].submit_batch = ahci_blk_batch;
    }
    // Drive 0 is REGISTERED (it may carry the root, by precedence); the
    // rest are only TRACKED, so they are named -- ahci1, ahci2 -- without
    // each taking the root from the one before. blk_nvme_init()'s shape.
    // No announcement for drive 0: blk_register() already logs it.
    if (!blk_register(&g_ahci_dev[0])) return 0;
    g_registered = 1;
    for (int i = 1; i < n; i++) {
        if (!blk_track(&g_ahci_dev[i], &g_ahci_dev[i], 0)) break;
        g_registered = i + 1;
        struct ahci_drive_info info;
        ahci_drive_info(i, &info);
        klog_printf("block: %s is SATA port %d (%llu sectors)\n",
                    blk_device_name(&g_ahci_dev[i]), info.port,
                    (unsigned long long)info.sectors);
    }
    return 1;
}

const struct block_device *blk_ahci_device(int drive) {
    return (drive >= 0 && drive < g_registered) ? &g_ahci_dev[drive] : NULL;
}
