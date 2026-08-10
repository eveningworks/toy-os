// Legacy PIO ATA/IDE, primary bus, master drive, 28-bit LBA. See ata.h
// for why this was chosen over AHCI. The detection sequence in
// ata_init() follows the standard "identify, bail out early at every
// step that doesn't look like a plain ATA master" algorithm (floating
// bus -> no drive at all -> non-ATA device -> success), same shape
// widely documented for this hardware, since getting any one of those
// checks wrong tends to mean hanging forever polling a status register
// that will never change, rather than a clean failure.
#include "ata.h"
#include "io.h"

#define ATA_PRIMARY_IO 0x1F0

#define REG_DATA        (ATA_PRIMARY_IO + 0)
#define REG_SECCOUNT    (ATA_PRIMARY_IO + 2)
#define REG_LBA_LOW     (ATA_PRIMARY_IO + 3)
#define REG_LBA_MID     (ATA_PRIMARY_IO + 4)
#define REG_LBA_HIGH    (ATA_PRIMARY_IO + 5)
#define REG_DRIVE_HEAD  (ATA_PRIMARY_IO + 6)
#define REG_STATUS      (ATA_PRIMARY_IO + 7)
#define REG_COMMAND     (ATA_PRIMARY_IO + 7)

#define STATUS_BSY 0x80
#define STATUS_DRQ 0x08
#define STATUS_ERR 0x01

#define CMD_READ_SECTORS  0x20
#define CMD_WRITE_SECTORS 0x30
#define CMD_CACHE_FLUSH   0xE7
#define CMD_IDENTIFY      0xEC

// Bounded retry counts, not infinite loops -- if there's genuinely no
// drive attached (very possible: this is a hobby OS, most runs won't
// have a `-drive` argument), every wait_*() here needs to give up and
// report failure rather than hang the boot forever.
#define ATA_POLL_LIMIT 100000

static int g_present = 0;

static int wait_not_busy(void) {
    for (int i = 0; i < ATA_POLL_LIMIT; i++) {
        if (!(inb(REG_STATUS) & STATUS_BSY)) return 1;
        io_wait();
    }
    return 0;
}

static int wait_drq(void) {
    for (int i = 0; i < ATA_POLL_LIMIT; i++) {
        uint8_t status = inb(REG_STATUS);
        if (status & STATUS_ERR) return 0;
        if (status & STATUS_DRQ) return 1;
        io_wait();
    }
    return 0;
}

void ata_init(void) {
    g_present = 0;

    // Select primary master (0xA0; slave would be 0xB0 -- this driver
    // never touches the slave, only ever the one drive most setups
    // (including this project's QEMU invocation) actually attach).
    outb(REG_DRIVE_HEAD, 0xA0);
    io_wait(); io_wait(); io_wait(); io_wait(); // ~400ns select settle time

    if (inb(REG_STATUS) == 0xFF) return; // floating bus: no controller here at all

    outb(REG_SECCOUNT, 0);
    outb(REG_LBA_LOW, 0);
    outb(REG_LBA_MID, 0);
    outb(REG_LBA_HIGH, 0);
    outb(REG_COMMAND, CMD_IDENTIFY);

    if (inb(REG_STATUS) == 0) return; // controller present, but no drive on it

    if (!wait_not_busy()) return;

    // A real ATA drive leaves these two at 0 throughout IDENTIFY; an
    // ATAPI device (like this project's boot CD, were it ever on this
    // bus instead of the secondary one -- see ata.h) reports its packet
    // signature here instead. Bail rather than try to read a "sector"
    // from something that isn't a plain block device.
    if (inb(REG_LBA_MID) != 0 || inb(REG_LBA_HIGH) != 0) return;

    if (!wait_drq()) return;

    // Must still read all 256 words even though nothing here is
    // inspected yet (no capacity/feature checks -- see the header) --
    // leaving them unread would desync the controller for the next
    // command.
    for (int i = 0; i < 256; i++) (void)inw(REG_DATA);

    g_present = 1;
}

int ata_present(void) {
    return g_present;
}

static void select_lba(uint32_t lba) {
    outb(REG_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F)); // 0xE0: LBA mode, master
    outb(REG_SECCOUNT, 1);
    outb(REG_LBA_LOW,  (uint8_t)(lba & 0xFF));
    outb(REG_LBA_MID,  (uint8_t)((lba >> 8) & 0xFF));
    outb(REG_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
}

int ata_read_sector(uint32_t lba, void *buf) {
    if (!g_present) return 0;
    if (!wait_not_busy()) return 0;

    select_lba(lba);
    outb(REG_COMMAND, CMD_READ_SECTORS);

    if (!wait_drq()) return 0;

    uint16_t *p = (uint16_t *)buf;
    for (int i = 0; i < ATA_SECTOR_SIZE / 2; i++) p[i] = inw(REG_DATA);
    return 1;
}

int ata_write_sector(uint32_t lba, const void *buf) {
    if (!g_present) return 0;
    if (!wait_not_busy()) return 0;

    select_lba(lba);
    outb(REG_COMMAND, CMD_WRITE_SECTORS);

    if (!wait_drq()) return 0;

    const uint16_t *p = (const uint16_t *)buf;
    for (int i = 0; i < ATA_SECTOR_SIZE / 2; i++) outw(REG_DATA, p[i]);

    // Best-effort: ask the drive to flush its write cache so the data
    // is actually durable, not just handed off. Failure here doesn't
    // undo the write above (PIO WRITE SECTORS has already transferred
    // the data by this point) -- it just means we can't be as sure it
    // survives a real power loss, which for QEMU's emulated disk backed
    // by a host file is already close to moot.
    if (wait_not_busy()) {
        outb(REG_COMMAND, CMD_CACHE_FLUSH);
        wait_not_busy();
    }

    return 1;
}
