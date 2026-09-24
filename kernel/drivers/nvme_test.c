// Tests for the NVMe driver (kernel/drivers/nvme.c), against whatever
// controller the machine has. READ-ONLY on purpose: the root filesystem
// may be on namespace 1. tools/nvme_test.py is what writes, reboots and
// reads back.
//
// Every test first asks the PCI bus, independently of the driver,
// whether a controller is there. That splits "no hardware" (skip) from
// "hardware the driver declined" (fail) -- ahci_test.c's rule, because
// a suite that skips on a broken driver is green for the wrong reason.
#include "ktest.h"
#include "nvme.h"
#include "block.h"
#include "pci.h"
#include "heap.h"
#include "string.h"

static int controller_on_bus(void) {
    for (int i = 0; i < pci_device_count(); i++) {
        const struct pci_device *d = pci_device_at(i);
        if (d && d->class_code == 0x01 && d->subclass == 0x08 && d->prog_if == 0x02) return 1;
    }
    return 0;
}

KTEST("nvme", "a controller on the bus is claimed and its namespaces identified") {
    if (!controller_on_bus()) KTEST_SKIP("no NVMe controller on the PCI bus");
    KTEST_ASSERT(nvme_ns_count() >= 1);
    KTEST_ASSERT(nvme_model()[0] != 0);
    for (int i = 0; i < nvme_ns_count(); i++) {
        KTEST_ASSERT(nvme_ns_id(i) != 0);
        KTEST_ASSERT(nvme_ns_sector_count(i) > 0);
        uint32_t bs = nvme_ns_block_size(i);
        KTEST_ASSERT(bs == 512 || bs == 4096);
        KTEST_ASSERT_EQ((int)(nvme_ns_sector_count(i) % (bs / 512)), 0);
    }
    KTEST_ASSERT(nvme_max_sectors_per_xfer() >= 8);
}

// THE LOAD-BEARING ONE. A 64-sector read touches nine pages from an
// offset buffer, so it needs a PRP LIST, where PRP1 carries an offset
// and every later entry a whole page -- and it is compared against the
// same bytes read one block at a time, which needs no list at all. A
// list built one page off returns a plausible block from the wrong place.
KTEST("nvme", "a PRP-list read from an offset buffer equals block-at-a-time reads") {
    if (!controller_on_bus()) KTEST_SKIP("no NVMe controller on the PCI bus");
    KTEST_ASSERT(nvme_ns_count() >= 1);
    uint32_t spb = nvme_ns_block_size(0) / 512;
    uint32_t n = 64;
    if (n > (uint32_t)nvme_max_sectors_per_xfer()) n = (uint32_t)nvme_max_sectors_per_xfer();
    n -= n % spb;
    uint8_t *big = kmalloc(n * 512 + 4096 + 512);
    uint8_t *one = kmalloc(4096);
    if (!big || !one) { kfree(big); kfree(one); KTEST_SKIP("no memory"); }
    uint8_t *at = big + 512;                 // not page-aligned: PRP1 has an offset
    int ok = nvme_read(0, 0, (int)n, at);
    int same = ok;
    int nonzero = 0;
    for (uint32_t s = 0; same && s < n; s += spb) {
        same = nvme_read(0, s, (int)spb, one) && k_memcmp(one, at + s * 512, spb * 512) == 0;
        for (uint32_t k = 0; k < spb * 512; k++) nonzero |= one[k];
    }
    kfree(big);
    kfree(one);
    KTEST_ASSERT(ok);
    KTEST_ASSERT(same);
    KTEST_ASSERT(nonzero);                   // a partition table, not a blank disk
}

// A buffer that is not dword-aligned cannot be a PRP; it goes through
// the bounce buffer, and must read the same bytes.
KTEST("nvme", "an unaligned buffer is bounced and reads the same bytes") {
    if (!controller_on_bus()) KTEST_SKIP("no NVMe controller on the PCI bus");
    KTEST_ASSERT(nvme_ns_count() >= 1);
    uint32_t spb = nvme_ns_block_size(0) / 512;
    uint8_t *a = kmalloc(spb * 512 + 8);
    uint8_t *b = kmalloc(spb * 512);
    if (!a || !b) { kfree(a); kfree(b); KTEST_SKIP("no memory"); }
    int ok = nvme_read(0, 0, (int)spb, a + 1) && nvme_read(0, 0, (int)spb, b);
    int same = ok && k_memcmp(a + 1, b, spb * 512) == 0;
    kfree(a);
    kfree(b);
    KTEST_ASSERT(ok);
    KTEST_ASSERT(same);
}

// Several reads in flight in one doorbell, each to a different place,
// each checked against the same block read on its own.
KTEST("nvme", "a batch of reads in flight at once matches one at a time") {
    if (!controller_on_bus()) KTEST_SKIP("no NVMe controller on the PCI bus");
    KTEST_ASSERT(nvme_ns_count() >= 1);
    uint32_t spb = nvme_ns_block_size(0) / 512;
    enum { N = 8 };
    uint8_t *buf = kmalloc(N * 2 * spb * 512);
    if (!buf) KTEST_SKIP("no memory");
    struct blk_io io[N];
    for (int i = 0; i < N; i++)
        io[i] = (struct blk_io){ .lba = (uint32_t)i * 37 * spb, .count = (uint16_t)spb,
                                 .write = 0, .buf = buf + i * spb * 512 };
    int all = nvme_submit_batch(0, io, N);
    int same = all;
    for (int i = 0; same && i < N; i++) {
        uint8_t *one = buf + (N + i) * spb * 512;
        same = io[i].ok && nvme_read(0, io[i].lba, (int)spb, one) &&
               k_memcmp(one, io[i].buf, spb * 512) == 0;
    }
    kfree(buf);
    KTEST_ASSERT(all);
    KTEST_ASSERT(same);
}

KTEST("nvme", "a transfer past the end, empty, too big or not whole blocks is refused") {
    if (!controller_on_bus()) KTEST_SKIP("no NVMe controller on the PCI bus");
    KTEST_ASSERT(nvme_ns_count() >= 1);
    uint32_t spb = nvme_ns_block_size(0) / 512;
    uint32_t end = nvme_ns_sector_count(0);
    uint8_t *b = kmalloc(8192);
    if (!b) KTEST_SKIP("no memory");
    KTEST_ASSERT(!nvme_read(0, end, (int)spb, b));
    KTEST_ASSERT(!nvme_read(0, end - spb, (int)(2 * spb), b));
    KTEST_ASSERT(!nvme_read(0, 0, 0, b));
    KTEST_ASSERT(!nvme_read(0, 0, nvme_max_sectors_per_xfer() + (int)spb, b));
    KTEST_ASSERT(!nvme_read(0, 0, (int)spb, 0));
    KTEST_ASSERT(!nvme_read(nvme_ns_count(), 0, (int)spb, b));
    if (spb > 1) KTEST_ASSERT(!nvme_read(0, 1, (int)spb, b));
    kfree(b);
}

KTEST("nvme", "a flush completes") {
    if (!controller_on_bus()) KTEST_SKIP("no NVMe controller on the PCI bus");
    KTEST_ASSERT(nvme_ns_count() >= 1);
    KTEST_ASSERT(nvme_flush(0));
}

// THE INTERRUPT IS DELIVERED, not merely configured. A dead vector would
// not fail any read: a sleeping waiter wakes at its deadline and reaps
// anyway, so everything works ten seconds at a time. The count is what
// tells the two apart. Needs interrupts on in the test's own context,
// since that is when a delivery can happen at all.
KTEST("nvme", "a completion raises the interrupt") {
    if (!controller_on_bus()) KTEST_SKIP("no NVMe controller on the PCI bus");
    KTEST_ASSERT(nvme_ns_count() >= 1);
    if (!nvme_irq_driven()) KTEST_SKIP("the controller is polled on this machine");
    uint64_t rflags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(rflags));
    if (!(rflags & (1u << 9))) KTEST_SKIP("interrupts are off in this context");
    uint32_t spb = nvme_ns_block_size(0) / 512;
    uint8_t *b = kmalloc(spb * 512);
    if (!b) KTEST_SKIP("no memory");
    uint64_t before = nvme_irq_count();
    int ok = 1;
    for (int i = 0; i < 4 && ok; i++) ok = nvme_read(0, (uint32_t)i * spb, (int)spb, b);
    for (int spin = 0; spin < 1000000 && nvme_irq_count() == before; spin++)
        __asm__ volatile ("pause");
    kfree(b);
    KTEST_ASSERT(ok);
    KTEST_ASSERT(nvme_irq_count() > before);
}
