// AHCI's KTESTs. Every one SKIPS on a machine with no SATA controller,
// which is every default boot here -- `make run DISK=ahci` and
// tools/ahci_test.py are what make them run at all.
//
// THEY ARE READ-ONLY ON PURPOSE. A KTEST runs in the live kernel with
// the root filesystem mounted off this very drive, so a write test
// would have to pick a sector nothing owns, and there is no such thing
// on a partitioned disk.
#include "ktest.h"
#include "ahci.h"
#include "pci.h"
#include "heap.h"
#include "string.h"
#include "scheduler.h"
#include "block.h"

// Is there a controller ON THE BUS, independent of whether the driver
// claimed it? That is what separates "this machine has no AHCI" (skip)
// from "the driver declined a controller that is right there" (fail).
static int hba_on_bus(void) {
    for (int i = 0; i < pci_device_count(); i++) {
        const struct pci_device *d = pci_device_at(i);
        if (d && d->class_code == 0x01 && d->subclass == 0x06) return 1;
    }
    return 0;
}

KTEST("ahci", "a controller on the bus was claimed and a drive identified") {
    if (!hba_on_bus()) KTEST_SKIP("no AHCI controller on this machine");

    KTEST_ASSERT(ahci_hba_count() > 0);
    KTEST_ASSERT(ahci_drive_count() > 0);
    for (int d = 0; d < ahci_drive_count(); d++) {
        struct ahci_drive_info info;
        KTEST_ASSERT(ahci_drive_info(d, &info));
        KTEST_ASSERT(info.sectors > 0);
        KTEST_ASSERT(info.model[0] != 0);
        KTEST_ASSERT(info.hba >= 0 && info.hba < ahci_hba_count());
    }
    KTEST_ASSERT(!ahci_drive_info(ahci_drive_count(), &(struct ahci_drive_info){0}));
}

// THE MULTI-DRIVE ONE. Every port with a SATA disk on it is a drive, and
// each drive's port says it is -- the driver stopped at the first disk
// it found once, and a second one simply did not exist.
KTEST("ahci", "every SATA disk on every port is a drive, and each port names its own") {
    if (!hba_on_bus()) KTEST_SKIP("no AHCI controller on this machine");
    if (!ahci_drive_count()) KTEST_SKIP("no drive claimed");

    int disks = 0, marked = 0;
    struct ahci_port_status s;
    for (int h = 0; h < ahci_hba_count(); h++) {
        struct ahci_hba_info hi;
        KTEST_ASSERT(ahci_hba_info(h, &hi));
        for (int i = 0; i < hi.port_count; i++) {
            KTEST_ASSERT(ahci_port_status(h, i, &s));
            if (s.det == 3 && s.signature == 0x00000101u) disks++;
            if (s.drive < 0) continue;
            marked++;
            KTEST_ASSERT(s.running);
            struct ahci_drive_info info;
            KTEST_ASSERT(ahci_drive_info(s.drive, &info));
            KTEST_ASSERT_EQ(info.hba, h);
            KTEST_ASSERT_EQ(info.port, s.port);
        }
        // Past the end is not a port, and must not read as one.
        KTEST_ASSERT(!ahci_port_status(h, hi.port_count, &s));
    }
    KTEST_ASSERT_EQ(marked, ahci_drive_count());
    KTEST_ASSERT_EQ(disks, ahci_drive_count());
}

// THE LOAD-BEARING ONE. A multi-sector read is gathered through several
// PRD entries -- one per 4 KiB page -- and a driver that filled only the
// first entry, or assembled them out of order, reads back plausible
// data for the first page and something else after it. Comparing a
// 24-sector transfer (three entries) against the same sectors read one
// at a time is what a single-entry driver cannot pass.
KTEST("ahci", "a multi-entry PRDT gathers the same bytes as single-sector reads") {
    if (!hba_on_bus()) KTEST_SKIP("no AHCI controller on this machine");
    if (!ahci_drive_count()) KTEST_SKIP("no drive claimed");

    const int n = 24;                       // 12 KiB: three whole PRD entries
    uint8_t *bulk = kmalloc((unsigned)n * AHCI_SECTOR_SIZE);
    uint8_t *one  = kmalloc(AHCI_SECTOR_SIZE);
    KTEST_ASSERT(bulk && one);

    // EVERY drive: each has its own tables and bounce buffer, and a
    // second drive's PRDT pointing at the first one's buffer reads back
    // the first drive's bytes. No preemption guard: the driver
    // serialises its own commands (each drive's lock), and a lock taken
    // under the guard is what kmutex.c's atomic-take check reports.
    int ok = 1, mismatch = -1;
    for (int d = 0; ok && mismatch < 0 && d < ahci_drive_count(); d++) {
        if (ahci_max_sectors_per_xfer(d) < n) continue;
        ok = ahci_read_sectors(d, 0, n, bulk);
        for (int i = 0; ok && i < n; i++) {
            if (!ahci_read_sectors(d, (uint32_t)i, 1, one)) { ok = 0; break; }
            if (k_memcmp(bulk + (unsigned)i * AHCI_SECTOR_SIZE, one, AHCI_SECTOR_SIZE) != 0) {
                mismatch = i;
                break;
            }
        }
    }

    kfree(bulk);
    kfree(one);
    KTEST_ASSERT(ok);
    KTEST_ASSERT_EQ(mismatch, -1);
}

KTEST("ahci", "a transfer past the drive or past the buffer is refused") {
    if (!hba_on_bus()) KTEST_SKIP("no AHCI controller on this machine");
    if (!ahci_drive_count()) KTEST_SKIP("no drive claimed");

    uint8_t *buf = kmalloc(AHCI_SECTOR_SIZE);
    KTEST_ASSERT(buf);

    // The last sector reads; one past it does not. Refused, never short:
    // a short read reporting success is how a filesystem ends up
    // parsing whatever the bounce buffer held last. The LAST drive, so
    // a second drive's bounds come from its own IDENTIFY.
    int d = ahci_drive_count() - 1;
    uint64_t end = ahci_sector_count(d);
    int last_ok  = ahci_read_sectors(d, end - 1, 1, buf);
    int past_ok  = ahci_read_sectors(d, end, 1, buf);
    int wrap_ok  = ahci_read_sectors(d, end - 1, 2, buf);
    int big_ok   = ahci_read_sectors(d, 0, ahci_max_sectors_per_xfer(d) + 1, buf);
    int zero_ok  = ahci_read_sectors(d, 0, 0, buf);
    int null_ok  = ahci_read_sectors(d, 0, 1, 0);
    int none_ok  = ahci_read_sectors(ahci_drive_count(), 0, 1, buf);

    kfree(buf);
    KTEST_ASSERT(last_ok);
    KTEST_ASSERT(!past_ok);
    KTEST_ASSERT(!wrap_ok);
    KTEST_ASSERT(!big_ok);
    KTEST_ASSERT(!zero_ok);
    KTEST_ASSERT(!null_ok);
    KTEST_ASSERT(!none_ok);
}

// TRIM IS TESTED FOR ITS REFUSALS ONLY, and deliberately: a real
// discard of a real range on the mounted root would destroy the
// filesystem this test is running from. That a TRIM actually reaches
// the drive is proved from the HOST by tools/ahci_test.py, which writes
// 40 MiB, deletes it, and requires disk.img's ALLOCATED size to come
// back to its baseline -- an oracle the guest cannot be wrong about.
KTEST("ahci", "TRIM refuses what it cannot discard") {
    if (!hba_on_bus()) KTEST_SKIP("no AHCI controller on this machine");
    if (!ahci_drive_count()) KTEST_SKIP("no drive claimed");
    if (!ahci_trim_supported(0)) KTEST_SKIP("this drive does not support DSM TRIM");

    int zero    = ahci_trim(0, 0, 0);
    int past    = ahci_trim(0, ahci_sector_count(0), 1);
    int wrap    = ahci_trim(0, ahci_sector_count(0) - 1, 2);

    KTEST_ASSERT(!zero);
    KTEST_ASSERT(!past);
    KTEST_ASSERT(!wrap);
}

// The capability bit and the function pointer must agree, in BOTH
// directions -- block.h refuses a device whose bits and pointers
// disagree, so this is really asking whether block_ahci.c consulted the
// drive at registration rather than advertising blind.
KTEST("ahci", "the block layer's TRIM capability matches the drive's answer") {
    if (!hba_on_bus()) KTEST_SKIP("no AHCI controller on this machine");
    if (!ahci_drive_count()) KTEST_SKIP("no drive claimed");
    int checked = 0;
    for (int d = 0; d < ahci_drive_count(); d++) {
        const struct block_device *dev = blk_ahci_device(d);
        if (!dev) continue;
        KTEST_ASSERT_EQ(blkdev_trim_supported(dev) ? 1 : 0, ahci_trim_supported(d) ? 1 : 0);
        checked++;
    }
    if (!checked) KTEST_SKIP("noahci -- no drive reached the block layer");
}

KTEST("ahci", "the drive acknowledges a flush") {
    if (!hba_on_bus()) KTEST_SKIP("no AHCI controller on this machine");
    if (!ahci_drive_count()) KTEST_SKIP("no drive claimed");

    // BLK_CAP_FLUSH is declared unconditionally by block_ahci.c, so a
    // FLUSH CACHE EXT that the drive refuses would be a capability bit
    // that lies -- which is the corruption bug, not a slow path.
    for (int d = 0; d < ahci_drive_count(); d++) KTEST_ASSERT(ahci_flush(d));
}

// **A QUEUED BATCH READS THE SAME BYTES AS ONE COMMAND AT A TIME, AND IT
// REALLY WAS QUEUED.** Scattered LBAs, so each tag is its own command
// and a tag/LBA mix-up shows as a mismatch rather than as neighbouring
// data. The NCQ round counter is the half that stops a silent fallback
// to one-at-a-time from passing: that path reads the same bytes too.
KTEST("ahci", "a queued batch reads the same bytes as single commands, and was queued") {
    if (!hba_on_bus()) KTEST_SKIP("no AHCI controller on this machine");
    if (!ahci_drive_count()) KTEST_SKIP("no drive claimed");
    if (ahci_ncq_depth(0) < 2) KTEST_SKIP("no NCQ on this drive or HBA");

    enum { N = 12, SECT = 8 };              // 12 transfers of 4 KiB
    uint8_t *buf = kmalloc(N * SECT * AHCI_SECTOR_SIZE);
    uint8_t *one = kmalloc(SECT * AHCI_SECTOR_SIZE);
    KTEST_ASSERT(buf && one);
    struct blk_io io[N];
    uint32_t span = (uint32_t)(ahci_sector_count(0) / (N + 1));
    for (int i = 0; i < N; i++)
        io[i] = (struct blk_io){ .lba = (uint32_t)i * span + (uint32_t)i * 7u, .count = SECT,
                                 .write = 0, .buf = buf + i * SECT * AHCI_SECTOR_SIZE };

    struct ahci_drive_info before, after;
    ahci_drive_info(0, &before);
    int all = ahci_submit_batch(0, io, N);
    ahci_drive_info(0, &after);
    uint64_t queued = after.ncq_rounds - before.ncq_rounds;
    int mismatch = -1, each_ok = 1;
    for (int i = 0; i < N; i++) {
        if (io[i].ok != 1) each_ok = 0;
        if (!ahci_read_sectors(0, io[i].lba, SECT, one)) { mismatch = 100 + i; break; }
        if (k_memcmp(io[i].buf, one, SECT * AHCI_SECTOR_SIZE) != 0) { mismatch = i; break; }
    }
    kfree(buf);
    kfree(one);
    KTEST_ASSERT(all);
    KTEST_ASSERT(each_ok);
    KTEST_ASSERT_EQ(mismatch, -1);
    KTEST_ASSERT(queued >= 1);              // <- NOT the fallback
}
