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

    KTEST_ASSERT(ahci_controller_present());
    KTEST_ASSERT(ahci_port_count() > 0);
    KTEST_ASSERT(ahci_present());
    KTEST_ASSERT(ahci_sector_count() > 0);
    KTEST_ASSERT(ahci_model()[0] != 0);
    // The port carrying the drive must be one of the implemented ones,
    // not an index that happens to be in range.
    KTEST_ASSERT(ahci_active_port() >= 0);
}

KTEST("ahci", "exactly one implemented port is marked active") {
    if (!hba_on_bus()) KTEST_SKIP("no AHCI controller on this machine");
    if (!ahci_present()) KTEST_SKIP("no drive claimed");

    int active = 0, running = 0;
    struct ahci_port_status s;
    for (int i = 0; i < ahci_port_count(); i++) {
        KTEST_ASSERT(ahci_port_status(i, &s));
        if (s.active) { active++; KTEST_ASSERT_EQ(s.port, ahci_active_port()); }
        if (s.running) running++;
    }
    KTEST_ASSERT_EQ(active, 1);
    KTEST_ASSERT(running >= 1);
    // Past the end is not a port, and must not read as one.
    KTEST_ASSERT(!ahci_port_status(ahci_port_count(), &s));
}

// THE LOAD-BEARING ONE. A multi-sector read is gathered through several
// PRD entries -- one per 4 KiB page -- and a driver that filled only the
// first entry, or assembled them out of order, reads back plausible
// data for the first page and something else after it. Comparing a
// 24-sector transfer (three entries) against the same sectors read one
// at a time is what a single-entry driver cannot pass.
KTEST("ahci", "a multi-entry PRDT gathers the same bytes as single-sector reads") {
    if (!hba_on_bus()) KTEST_SKIP("no AHCI controller on this machine");
    if (!ahci_present()) KTEST_SKIP("no drive claimed");
    if (ahci_max_sectors_per_xfer() < 24) KTEST_SKIP("DMA buffer too small for a 3-page transfer");

    const int n = 24;                       // 12 KiB: three whole PRD entries
    uint8_t *bulk = kmalloc((unsigned)n * AHCI_SECTOR_SIZE);
    uint8_t *one  = kmalloc(AHCI_SECTOR_SIZE);
    KTEST_ASSERT(bulk && one);

    // The driver has one command slot and one bounce buffer, and a
    // ring-3 process is preemptible inside a syscall -- the same
    // reasoning vfs.c's FS_OP() guard exists for.
    scheduler_preempt_disable();
    int ok = ahci_read_sectors(0, n, bulk);
    int mismatch = -1;
    for (int i = 0; ok && i < n; i++) {
        if (!ahci_read_sectors((uint32_t)i, 1, one)) { ok = 0; break; }
        if (k_memcmp(bulk + (unsigned)i * AHCI_SECTOR_SIZE, one, AHCI_SECTOR_SIZE) != 0) {
            mismatch = i;
            break;
        }
    }
    scheduler_preempt_enable();

    kfree(bulk);
    kfree(one);
    KTEST_ASSERT(ok);
    KTEST_ASSERT_EQ(mismatch, -1);
}

KTEST("ahci", "a transfer past the drive or past the buffer is refused") {
    if (!hba_on_bus()) KTEST_SKIP("no AHCI controller on this machine");
    if (!ahci_present()) KTEST_SKIP("no drive claimed");

    uint8_t *buf = kmalloc(AHCI_SECTOR_SIZE);
    KTEST_ASSERT(buf);

    scheduler_preempt_disable();
    // The last sector reads; one past it does not. Refused, never short:
    // a short read reporting success is how a filesystem ends up
    // parsing whatever the bounce buffer held last.
    int last_ok  = ahci_read_sectors(ahci_sector_count() - 1, 1, buf);
    int past_ok  = ahci_read_sectors(ahci_sector_count(), 1, buf);
    int wrap_ok  = ahci_read_sectors(ahci_sector_count() - 1, 2, buf);
    int big_ok   = ahci_read_sectors(0, ahci_max_sectors_per_xfer() + 1, buf);
    int zero_ok  = ahci_read_sectors(0, 0, buf);
    int null_ok  = ahci_read_sectors(0, 1, 0);
    scheduler_preempt_enable();

    kfree(buf);
    KTEST_ASSERT(last_ok);
    KTEST_ASSERT(!past_ok);
    KTEST_ASSERT(!wrap_ok);
    KTEST_ASSERT(!big_ok);
    KTEST_ASSERT(!zero_ok);
    KTEST_ASSERT(!null_ok);
}

// TRIM IS TESTED FOR ITS REFUSALS ONLY, and deliberately: a real
// discard of a real range on the mounted root would destroy the
// filesystem this test is running from. That a TRIM actually reaches
// the drive is proved from the HOST by tools/ahci_test.py, which writes
// 40 MiB, deletes it, and requires disk.img's ALLOCATED size to come
// back to its baseline -- an oracle the guest cannot be wrong about.
KTEST("ahci", "TRIM refuses what it cannot discard") {
    if (!hba_on_bus()) KTEST_SKIP("no AHCI controller on this machine");
    if (!ahci_present()) KTEST_SKIP("no drive claimed");
    if (!ahci_trim_supported()) KTEST_SKIP("this drive does not support DSM TRIM");

    scheduler_preempt_disable();
    int zero    = ahci_trim(0, 0);
    int past    = ahci_trim(ahci_sector_count(), 1);
    int wrap    = ahci_trim(ahci_sector_count() - 1, 2);
    scheduler_preempt_enable();

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
    if (!ahci_present()) KTEST_SKIP("no drive claimed");
    const char *name = blk_name();
    if (!name || name[0] != 'a' || name[1] != 'h') KTEST_SKIP("AHCI is not the active device");

    KTEST_ASSERT_EQ(blk_trim_supported() ? 1 : 0, ahci_trim_supported() ? 1 : 0);
}

KTEST("ahci", "the drive acknowledges a flush") {
    if (!hba_on_bus()) KTEST_SKIP("no AHCI controller on this machine");
    if (!ahci_present()) KTEST_SKIP("no drive claimed");

    // BLK_CAP_FLUSH is declared unconditionally by block_ahci.c, so a
    // FLUSH CACHE EXT that the drive refuses would be a capability bit
    // that lies -- which is the corruption bug, not a slow path.
    scheduler_preempt_disable();
    int ok = ahci_flush();
    scheduler_preempt_enable();
    KTEST_ASSERT(ok);
}
