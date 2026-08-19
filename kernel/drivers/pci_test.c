// KTESTs for pci.c's DRIVER-FACING half -- the config-space accessors,
// the capability walk and the 64-bit BAR decode that pci_internal.h
// added for virtio.
//
// These deliberately assert against the machine this is running on
// rather than against baked constants: there is no fixture here, the
// PCI bus is whatever QEMU (or real hardware) presents, and a test
// demanding a particular device would fail on a legitimate topology.
// So each one checks a RELATIONSHIP that must hold whatever is on the
// bus -- the accessors agree with what pci_init() already recorded, the
// walk terminates, a decoded BAR is consistent with its own type bits.
//
// The value of that shape: it runs on every boot, including every
// existing headless test, with no new QEMU flag. A broken capability
// walk is caught by `make test` on a machine with no virtio device
// attached at all.
#include "pci.h"
#include "pci_internal.h"
#include "ktest.h"

// pci_init() read every field through the same config-space path these
// accessors use, so the two must agree. If they disagree the unpacking
// from (struct pci_device *) back to (bus, device, function) is wrong --
// which is exactly the transposition hazard pci_internal.h's comment
// says the device-taking signatures exist to prevent, so it is worth an
// assertion rather than an argument.
KTEST("pci", "config reads agree with the enumerated table") {
    int count = pci_device_count();
    KTEST_ASSERT(count > 0);  // any machine that booted this far has a bus

    for (int i = 0; i < count; i++) {
        const struct pci_device *d = pci_device_at(i);
        KTEST_ASSERT(d != 0);

        // Offset 0 is {vendor_id, device_id} as one dword.
        uint32_t id = pci_config_read32(d, 0x00);
        KTEST_ASSERT_EQ(id & 0xFFFF, d->vendor_id);
        KTEST_ASSERT_EQ((id >> 16) & 0xFFFF, d->device_id);

        // ...and the narrower reads must slice that same dword.
        KTEST_ASSERT_EQ(pci_config_read16(d, 0x00), d->vendor_id);
        KTEST_ASSERT_EQ(pci_config_read16(d, 0x02), d->device_id);
        KTEST_ASSERT_EQ(pci_config_read8(d, 0x0B), d->class_code);
        KTEST_ASSERT_EQ(pci_config_read8(d, 0x0A), d->subclass);
    }
}

// The `next` pointers come from the device, so a cycle is a hardware-
// supplied hang. This asserts the walk TERMINATES and stays in bounds.
//
// IT WALKS EVERY CAPABILITY ID THE MACHINE ACTUALLY HAS, not just the
// vendor-specific one virtio uses, and that is the whole point of how
// it is written: measured on QEMU's default pc-i440fx topology, NOTHING
// on the bus publishes a vendor-specific capability, so a test that
// asked only for PCI_CAP_ID_VNDR would run its loop body zero times and
// pass while asserting nothing. Driving the walker with the ids the
// device really publishes is what makes this cover the walk on a boot
// with no virtio device attached.
//
// The reference walk here is deliberately INDEPENDENT of the walker
// under test: it follows the {id, next} chain by hand through
// pci_config_read8(), so the two agreeing means something.
KTEST("pci", "the capability walk terminates and stays in bounds") {
    int caps_seen = 0;

    for (int i = 0; i < pci_device_count(); i++) {
        const struct pci_device *d = pci_device_at(i);
        if (!d) continue;

        // Asking for an id nothing publishes forces the walker to visit
        // every entry and fall off the end. A cycle would spin here
        // instead of returning -- the hop bound is what makes that a
        // return rather than a hung boot.
        KTEST_ASSERT_EQ(pci_capability_find(d, 0xFE, 0), 0);

        if (!(pci_config_read16(d, 0x06) & 0x0010)) {
            // No capability list. The walker must say so rather than
            // reading whatever offset 0x34 happens to hold.
            KTEST_ASSERT_EQ(pci_capability_find(d, 0x01, 0), 0);
            continue;
        }

        // Hand-walk the chain, checking the walker against each entry.
        uint8_t off = pci_config_read8(d, 0x34) & 0xFC;
        for (int hops = 0; off && hops < 48; hops++) {
            KTEST_ASSERT(off >= 0x40);        // past the standard header
            KTEST_ASSERT_EQ(off & 0x3, 0);    // dword-aligned
            uint8_t id = pci_config_read8(d, off);
            caps_seen++;

            // The walker must find SOME capability carrying this id,
            // and what it returns must actually carry it.
            uint8_t found = pci_capability_find(d, id, 0);
            KTEST_ASSERT(found >= 0x40);
            KTEST_ASSERT_EQ(pci_config_read8(d, found), id);

            off = pci_config_read8(d, (uint8_t)(off + 1)) & 0xFC;
        }
    }

    // If this fires, the machine published no capabilities at all and
    // everything above ran zero times -- which is a skip, not a pass.
    // Stated rather than assumed, because "0 of 0 checks passed" is the
    // shape this repo has shipped real bugs behind.
    if (caps_seen == 0) KTEST_SKIP("no PCI capabilities on this machine");
}

// The decode must agree with the BAR's own type bits, and must never
// hand back a half-decoded 64-bit address.
//
// COVERAGE NOTE, so nobody reads more into a green run than is there:
// QEMU's default pc-i440fx topology presents no 64-bit BAR at all
// (measured -- every BAR on it is 32-bit memory or I/O), so the 64-bit
// branch below is NOT exercised on an ordinary boot. It becomes
// reachable when a virtio device is attached, whose register windows
// live in one. What this test covers unconditionally is the negative
// space: I/O BARs, unimplemented BARs, and the promise that a 32-bit
// BAR never grows an upper half.
KTEST("pci", "BAR decoding agrees with the BAR's type bits") {
    for (int i = 0; i < pci_device_count(); i++) {
        const struct pci_device *d = pci_device_at(i);
        if (!d) continue;

        for (int b = 0; b < 6; b++) {
            uint32_t raw = d->bar[b];
            uint64_t addr = pci_bar_mem_addr(d, b);

            // An I/O BAR is not a memory BAR, whatever else is true.
            if (pci_bar_is_io(raw)) {
                KTEST_ASSERT_EQ(addr, 0);
                KTEST_ASSERT_EQ(pci_bar_is_64(raw), 0);
                continue;
            }
            if (raw == 0) { KTEST_ASSERT_EQ(addr, 0); continue; }

            if (pci_bar_is_64(raw)) {
                // A 64-bit BAR in slot 5 has no upper half: refused,
                // not silently treated as 32-bit.
                if (b == 5) { KTEST_ASSERT_EQ(addr, 0); continue; }
                KTEST_ASSERT_EQ((uint32_t)(addr >> 32), d->bar[b + 1]);
            } else {
                KTEST_ASSERT_EQ(addr >> 32, 0);  // no upper half invented
            }
            // The low decode-type bits are masked off in both cases.
            KTEST_ASSERT_EQ((uint32_t)addr & 0xF, 0);
            KTEST_ASSERT_EQ((uint32_t)addr, raw & 0xFFFFFFF0u);
        }
    }
}

// The 64-bit combine, against a SYNTHETIC device rather than the bus.
//
// THIS EXISTS BECAUSE THE HARDWARE TEST ABOVE CANNOT CATCH THE BUG.
// Measured with a positive control: deleting the upper-half combine
// outright ("return the low half") left every check above GREEN. The
// reason is that QEMU places BARs below 4 GiB, so a real 64-bit BAR's
// upper half is ZERO -- combining it and dropping it produce the same
// answer, and the assertion cannot tell them apart. The branch runs;
// the values make correct and broken indistinguishable. A real decode
// bug would therefore be invisible here and would surface only on
// hardware, or on a machine that puts a BAR high.
//
// A hand-built struct pci_device fixes that: pci_bar_mem_addr() reads
// only the bar[] array, so it can be handed values no QEMU topology
// here will produce. This is the case whose input actually crosses the
// boundary being tested.
KTEST("pci", "a 64-bit BAR combines both halves") {
    struct pci_device d = {0};

    // bits 2:1 == 0b10 marks 64-bit; bit 0 clear marks memory.
    d.bar[4] = 0xFE000000u | 0x4u;   // low half + 64-bit type
    d.bar[5] = 0x00000012u;          // upper half -- NONZERO, unlike QEMU's
    KTEST_ASSERT(pci_bar_is_64(d.bar[4]));
    KTEST_ASSERT_EQ(pci_bar_mem_addr(&d, 4), 0x12FE000000ull);

    // A 32-bit BAR must never grow an upper half from the next slot,
    // even when that slot holds something.
    d.bar[0] = 0xFEBC0000u;          // 32-bit memory BAR
    d.bar[1] = 0x0000C001u;          // an I/O BAR, not an upper half
    KTEST_ASSERT_EQ(pci_bar_is_64(d.bar[0]), 0);
    KTEST_ASSERT_EQ(pci_bar_mem_addr(&d, 0), 0xFEBC0000ull);

    // A 64-bit BAR in slot 5 has no upper half to combine with: refused
    // rather than silently treated as 32-bit (and never read off the
    // end of bar[6]).
    d.bar[5] = 0xFE000000u | 0x4u;
    KTEST_ASSERT_EQ(pci_bar_mem_addr(&d, 5), 0);

    // An I/O BAR is not a memory BAR however its other bits look.
    d.bar[2] = 0x0000C041u;
    KTEST_ASSERT_EQ(pci_bar_mem_addr(&d, 2), 0);
}

// Out-of-range and NULL must be refused rather than read off the end of
// bar[6] -- the 64-bit path indexes index+1, so this is the bound that
// keeps it inside the array.
KTEST("pci", "BAR decoding refuses an out-of-range index") {
    const struct pci_device *d = pci_device_at(0);
    KTEST_ASSERT(d != 0);
    KTEST_ASSERT_EQ(pci_bar_mem_addr(d, -1), 0);
    KTEST_ASSERT_EQ(pci_bar_mem_addr(d, 6), 0);
    KTEST_ASSERT_EQ(pci_bar_mem_addr(0, 0), 0);
    KTEST_ASSERT_EQ(pci_capability_find(0, PCI_CAP_ID_VNDR, 0), 0);
}
