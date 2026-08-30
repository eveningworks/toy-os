// The `_S5_` scan is the only thing in kernel/acpi/ that PARSES rather
// than reads a fixed offset, so it is the only thing that can be wrong
// in a way a boot does not notice: a machine whose sleep type is
// decoded as 0 when it is 5 shuts down perfectly under QEMU (whose _S5_
// really is 0) and does nothing at all on the hardware that motivated
// this. These build the encodings by hand so both answers are checked
// without needing a machine that has them.
#include "ktest.h"
#include "acpi.h"

// `\_S5_` as QEMU's DSDT encodes it: NameOp, root prefix, the name,
// a Package of four ZeroOps.
static const uint8_t s5_qemu[] = {
    0x08, 0x5C, '_', 'S', '5', '_',
    0x12, 0x06, 0x04, 0x00, 0x00, 0x00, 0x00,
};

// The same object with byte constants, which is how firmware that uses
// a non-zero sleep type writes it.
static const uint8_t s5_bytes[] = {
    0x08, '_', 'S', '5', '_',
    0x12, 0x0A, 0x04, 0x0A, 0x05, 0x0A, 0x05, 0x00, 0x00,
};

KTEST("acpi", "_S5_ decodes ZeroOp elements") {
    uint8_t a = 0xFF, b = 0xFF;
    KTEST_ASSERT(acpi_scan_s5(s5_qemu, sizeof s5_qemu, &a, &b) == 1);
    KTEST_ASSERT(a == 0);
    KTEST_ASSERT(b == 0);
}

KTEST("acpi", "_S5_ decodes byte-constant elements") {
    uint8_t a = 0, b = 0;
    KTEST_ASSERT(acpi_scan_s5(s5_bytes, sizeof s5_bytes, &a, &b) == 1);
    KTEST_ASSERT(a == 5);
    KTEST_ASSERT(b == 5);
}

KTEST("acpi", "_S5_ not introduced by NameOp is refused") {
    // The same four letters inside a string. Accepting this is how a
    // scanner ends up writing a sleep request built out of whatever
    // bytes happened to follow a piece of unrelated AML.
    const uint8_t noise[] = {
        0x0D, '_', 'S', '5', '_', 0x00,
        0x12, 0x06, 0x04, 0x0A, 0x07, 0x0A, 0x07,
    };
    uint8_t a = 9, b = 9;
    KTEST_ASSERT(acpi_scan_s5(noise, sizeof noise, &a, &b) == 0);
    KTEST_ASSERT(a == 9); // untouched -- a refusal writes nothing
}

KTEST("acpi", "a _S5_ package that runs off the end is refused") {
    // Truncated after the element count. A parser that read on would
    // take its sleep types from whatever follows the table.
    const uint8_t cut[] = { 0x08, 0x5C, '_', 'S', '5', '_', 0x12, 0x06, 0x04 };
    uint8_t a = 9, b = 9;
    KTEST_ASSERT(acpi_scan_s5(cut, sizeof cut, &a, &b) == 0);
}

KTEST("acpi", "an element encoding this does not know is refused") {
    // 0x0C is a DWordPrefix -- legal AML, and not something a sleep
    // type is ever written as. Guessing here would mean reading four
    // bytes as one.
    const uint8_t dword[] = {
        0x08, '_', 'S', '5', '_',
        0x12, 0x0C, 0x04, 0x0C, 0x05, 0x00, 0x00, 0x00,
    };
    uint8_t a = 9, b = 9;
    KTEST_ASSERT(acpi_scan_s5(dword, sizeof dword, &a, &b) == 0);
}

KTEST("acpi", "an ACPI structure sums to zero") {
    // The rule every table and the RSDP are validated by. A structure
    // whose bytes sum to something else is dropped, so getting this
    // backwards would silently drop every table on the machine.
    const uint8_t good[] = { 0x10, 0x20, 0xD0 };
    const uint8_t bad[]  = { 0x10, 0x20, 0xD1 };
    KTEST_ASSERT(acpi_checksum(good, sizeof good) == 0);
    KTEST_ASSERT(acpi_checksum(bad, sizeof bad) != 0);
}

KTEST("acpi", "the tables found this boot all checksum") {
    // Not a parser test -- a check that what acpi_init() kept is what
    // it claimed to keep. Every recorded table passed its checksum at
    // discovery; re-summing here catches a walk that recorded a header
    // it had not validated. A machine with no ACPI has zero tables,
    // which passes trivially and is a real supported state.
    for (int i = 0; i < acpi_table_count(); i++) {
        const struct acpi_sdt_header *h = acpi_table_at(i);
        KTEST_ASSERT(h != 0);
        KTEST_ASSERT(h->length >= sizeof *h);
        KTEST_ASSERT(acpi_checksum(h, h->length) == 0);
    }
}

KTEST("acpi", "a poweroff path exists, or the state says why not") {
    // The one assertion that would have caught this whole feature being
    // absent: either _S5_ decoded and a register was named, or the
    // flags say which half is missing. It must not be possible to have
    // the flag and no port.
    const struct acpi_state *s = acpi_get_state();
    if (s->flags & ACPI_F_S5) {
        KTEST_ASSERT(s->pm1a_cnt != 0 ||
                     ((s->flags & ACPI_F_HW_REDUCED) && s->sleep_control.address != 0));
    }
}
