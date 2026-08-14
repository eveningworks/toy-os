// Tests for the ATA driver's PIO fallback path.
//
// The PIO path is why this file exists. It is the driver toy-os falls
// back to when Bus-Master DMA can't be brought up -- and DMA comes up on
// every machine this OS actually boots, so until `ata nodma` existed
// those ~100 lines never executed at all. Code that only ever runs in an
// emergency, and has never been observed running, is not a fallback;
// it's a guess. These tests drive the same switch the shell command
// does, so the path executes on every `make test` and CI run.
//
// The DMA path is deliberately NOT retested here: it's what every other
// test in the suite, `make test` itself, and every boot already exercise
// end to end.
#include "ktest.h"
#include "ata.h"
#include "fs.h"
#include "string.h"

#define PIO_TEST_PATH "/tmp/.ata_pio_test"

// Runs `body` with DMA forced off, then restores the previous mode --
// even if an assertion inside `body` would have returned early, which
// is why the assertions live AFTER this rather than inside it. A test
// that leaks driver state poisons every test after it, the same cascade
// the runner's fault_any_armed() check exists to stop (see CHANGELOG.md
// on the flaky CI failure that produced it).
#define WITH_PIO_FORCED(body)                       \
    do {                                            \
        int _had_dma = ata_dma_hardware_available(); \
        int _applied = _had_dma ? ata_set_dma_forced_off(1) : 1; \
        if (_applied) { body }                      \
        if (_had_dma) ata_set_dma_forced_off(0);    \
    } while (0)

KTEST("ata", "PIO path round-trips a file through the filesystem") {
    // Establish the precondition rather than inherit it -- FRESH()'s
    // lesson from fs_test.c, applied here: a leftover file from an
    // earlier run would make fs_write() take a different branch.
    fs_delete(PIO_TEST_PATH);
    KTEST_ASSERT(!fs_exists(PIO_TEST_PATH));

    static const char payload[] =
        "PIO round-trip: the CPU moved every one of these bytes through "
        "REG_DATA itself, one 16-bit word at a time, with no bus-master "
        "engine involved anywhere.";
    char readback[sizeof payload];
    int wrote = 0, on_pio = 0;
    uint32_t got = 0;

    WITH_PIO_FORCED({
        on_pio = !ata_dma_active();
        wrote = fs_write(PIO_TEST_PATH, payload, 0);
        got = fs_read_range(PIO_TEST_PATH, 0, readback, sizeof readback);
    });

    KTEST_ASSERT(on_pio);                       // the switch actually took effect
    KTEST_ASSERT(wrote);                        // write went through the PIO path
    KTEST_ASSERT_EQ(got, (uint32_t)sizeof payload - 1);
    KTEST_ASSERT_EQ(k_memcmp(readback, payload, sizeof payload - 1), 0);

    fs_delete(PIO_TEST_PATH);
}

KTEST("ata", "the per-transfer sector cap follows the active path") {
    // The wrinkle that makes a single dma_in_use() helper necessary
    // rather than the flags being checked ad hoc at each site: PIO
    // caps a transfer lower than DMA does, so a dispatch site and the
    // cap disagreeing would let a caller batch more sectors into a
    // transfer than the path it lands on can carry.
    if (!ata_dma_hardware_available()) {
        // Nothing to compare on a PIO-only machine; the cap is already
        // the PIO one and that's all this test could assert.
        KTEST_ASSERT(ata_max_sectors_per_xfer() > 0);
        return;
    }

    int dma_cap = ata_max_sectors_per_xfer();
    int pio_cap = 0;
    WITH_PIO_FORCED({ pio_cap = ata_max_sectors_per_xfer(); });

    KTEST_ASSERT(pio_cap > 0);
    KTEST_ASSERT(pio_cap <= dma_cap);
    KTEST_ASSERT_EQ(ata_max_sectors_per_xfer(), dma_cap); // restored
}

KTEST("ata", "forcing PIO is refused mid-transfer, and reported") {
    // ata_set_dma_forced_off() returns 0 rather than switching while a
    // non-blocking transfer is in flight. Nothing here starts one, so
    // this asserts the ordinary case succeeds -- the refusal branch
    // itself needs a stepped write in progress, which a test can't
    // hold open across a KTEST body today. Recorded as a known gap
    // rather than left looking covered.
    KTEST_ASSERT(ata_set_dma_forced_off(0) == 1);
    KTEST_ASSERT(ata_dma_active() == ata_dma_hardware_available());
}

KTEST("ata", "TRIM refuses what it cannot safely discard") {
    // Deliberately only the REFUSAL paths. A test that actually trimmed
    // a range would be discarding real blocks of the live filesystem
    // this kernel is running from -- the tests run inside the booted
    // system (see CLAUDE.md), so there is no scratch region to aim at.
    // What can be asserted safely is that ata_trim() rejects the inputs
    // that would do damage, which is the part with branches in it.
    KTEST_ASSERT_EQ(ata_trim(0, 0), 0);           // empty range: nothing to do

    uint32_t sectors = ata_sector_count();
    if (sectors) {
        // Past the end of the drive, and straddling the end. Both must
        // be refused rather than clamped: a clamped discard would silently
        // trim a different range than the caller asked for.
        KTEST_ASSERT_EQ(ata_trim(sectors, 8), 0);
        KTEST_ASSERT_EQ(ata_trim(sectors - 4, 8), 0);
    }

    // And the capability answer is consistent with the drive being there
    // at all -- ata_trim_supported() must never claim support with no
    // drive, since every caller uses it to decide whether to bother.
    if (!ata_present()) KTEST_ASSERT_EQ(ata_trim_supported(), 0);
}
