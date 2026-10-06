// The AHCI drive, as a block device. A thin adapter, the same shape as
// block_ata.c and block_virtio.c: the driver keeps its behaviour and
// this only states which of it the block layer may use.
#include "block.h"
#include "ahci.h"
#include "multiboot.h"
#include "string.h"

// driver-none: the block_device shim; ahci.c is the driver

static uint32_t ahci_blk_sector_count(void) { return ahci_sector_count(); }
static int ahci_blk_read(uint32_t lba, int count, void *buf) { return ahci_read_sectors(lba, count, buf); }
static int ahci_blk_write(uint32_t lba, int count, const void *buf) { return ahci_write_sectors(lba, count, buf); }
static int ahci_blk_max_xfer(void) { return ahci_max_sectors_per_xfer(); }
static int ahci_blk_flush(void) { return ahci_flush(); }
static int ahci_blk_trim(uint32_t lba, uint32_t count) { return ahci_trim(lba, count); }
static int ahci_blk_trim_ranges(const struct blk_range *r, int n) { return ahci_trim_ranges(r, n); }

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
static struct block_device AHCI_DEV = {
    .name = "ahci",
    .driver = "ahci",
    .sector_count = ahci_blk_sector_count,
    .read_sectors = ahci_blk_read,
    .write_sectors = ahci_blk_write,
    .max_sectors_per_xfer = ahci_blk_max_xfer,
    .persistent = 1,
    .caps = BLK_CAP_FLUSH,
    .flush = ahci_blk_flush,
    .trim = 0,
};

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

int blk_ahci_init(void) {
    if (!ahci_present()) return 0;
    if (ahci_disabled()) return 0;

    if (ahci_trim_supported()) {
        AHCI_DEV.caps |= BLK_CAP_TRIM;
        AHCI_DEV.trim = ahci_blk_trim;
        AHCI_DEV.trim_ranges = ahci_blk_trim_ranges;
    }

    // Several commands in flight, when the driver queues any -- and
    // `noncq` takes that away without touching anything else, so one
    // build can be measured both ways (Linux's libata.force=noncq).
    if (ahci_ncq_depth() && !word_on_cmdline("noncq"))
        AHCI_DEV.submit_batch = ahci_submit_batch;

    AHCI_DEV.model = ahci_model();
    AHCI_DEV.pci = ahci_pci();
    // No announcement: blk_register() already logs the device it accepts.
    return blk_register(&AHCI_DEV);
}
