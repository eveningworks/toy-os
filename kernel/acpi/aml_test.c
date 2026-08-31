// The namespace walk, against a hand-built fixture AND against whatever
// firmware this machine actually has.
//
// THE TWO KINDS ARE BOTH NECESSARY. The fixture pins exact shapes -- and
// one shape in particular that no assertion on a real table could make
// visible: that a Method's body is NOT entered. The live table is the
// only thing that says the parser survives firmware nobody here wrote,
// which is the property that matters and the one a fixture written
// beside the parser can never test.
#include "aml.h"
#include "acpi.h"
#include "ktest.h"
#include "string.h"

// Scope(\_SB) {
//   Device(PCI0) { Name(_ADR, 7)  Method(FOO_, 0) { Device(HIDE) {} } }
//   OperationRegion(GIO_, SystemIO, 0x0402, 4)   // no PkgLength
// }
// Assembled with its lengths computed rather than guessed.
static const uint8_t FIXTURE[] = {
    0x10, 0x2e, 0x5c, 0x5f, 0x53, 0x42, 0x5f, 0x5b, 0x82, 0x1a, 0x50, 0x43,
    0x49, 0x30, 0x08, 0x5f, 0x41, 0x44, 0x52, 0x0a, 0x07, 0x14, 0x0d, 0x46,
    0x4f, 0x4f, 0x5f, 0x00, 0x5b, 0x82, 0x05, 0x48, 0x49, 0x44, 0x45, 0x5b,
    0x80, 0x47, 0x49, 0x4f, 0x5f, 0x01, 0x0b, 0x02, 0x04, 0x0a, 0x04,
};

static int find(const char *name4) {
    for (int i = 0; i < aml_node_count(); i++)
        if (k_memcmp(aml_node_at(i)->name, name4, 4) == 0) return i;
    return -1;
}

KTEST("aml", "a scope, a device and a name come out as a tree") {
    aml_reset();
    KTEST_ASSERT(aml_parse_table(FIXTURE, sizeof FIXTURE, AML_ROOT));

    int sb = find("_SB_"), pci = find("PCI0"), adr = find("_ADR");
    KTEST_ASSERT(sb > 0 && pci > 0 && adr > 0);
    KTEST_ASSERT_EQ(aml_node_at(sb)->kind, AML_KIND_SCOPE);
    KTEST_ASSERT_EQ(aml_node_at(pci)->kind, AML_KIND_DEVICE);
    KTEST_ASSERT_EQ(aml_node_at(adr)->kind, AML_KIND_NAME);
    KTEST_ASSERT_EQ(aml_node_at(pci)->parent, sb);
    KTEST_ASSERT_EQ(aml_node_at(adr)->parent, pci);

    char path[64];
    KTEST_ASSERT(aml_path(adr, path, sizeof path));
    KTEST_ASSERT_EQ(k_strcmp(path, "\\_SB_.PCI0._ADR"), 0);
}

KTEST("aml", "a METHOD's body is recorded and never entered") {
    // The load-bearing property of the whole design: `Device(HIDE)`
    // lives inside `Method(FOO_)`, so a walk that stepped into method
    // bodies would find it. Nothing else here can catch that.
    aml_reset();
    KTEST_ASSERT(aml_parse_table(FIXTURE, sizeof FIXTURE, AML_ROOT));
    KTEST_ASSERT(find("FOO_") > 0);
    KTEST_ASSERT_EQ(aml_node_at(find("FOO_"))->kind, AML_KIND_METHOD);
    KTEST_ASSERT_EQ(find("HIDE"), -1);
}

KTEST("aml", "an OperationRegion has no PkgLength and is stepped over anyway") {
    // The form that made the first draft of docs/aml-design.md wrong,
    // and the second thing QEMU's own DSDT contains. If its length is
    // mis-computed the walk loses its place and everything after it is
    // refused -- so the assertion is that the region is found AND that
    // nothing was refused.
    aml_reset();
    KTEST_ASSERT(aml_parse_table(FIXTURE, sizeof FIXTURE, AML_ROOT));
    KTEST_ASSERT(find("GIO_") > 0);
    KTEST_ASSERT_EQ(aml_refused(), 0);
}

KTEST("aml", "this machine's own firmware walks into a tree") {
    int n = aml_build();
    if (n <= 1) KTEST_SKIP("no DSDT on this machine");

    // Invariants that must hold for ANY well-formed table, so this runs
    // on the maintainer's laptop as usefully as on QEMU.
    for (int i = 1; i < n; i++) {
        const struct aml_node *node = aml_node_at(i);
        KTEST_ASSERT(node->parent < (uint16_t)i);   // a tree, and acyclic
        for (int c = 0; c < 4; c++) {
            char ch = node->name[c];
            int ok = (ch >= 'A' && ch <= 'Z') || ch == '_' ||
                     (ch >= '0' && ch <= '9') || ch == '\\';
            KTEST_ASSERT(ok);
        }
        char path[128];
        KTEST_ASSERT(aml_path(i, path, sizeof path) > 0);
        KTEST_ASSERT_EQ(path[0], '\\');
    }
}

KTEST("aml", "every truncation of a real table stops instead of running away") {
    // Firmware is untrusted input, and the failure this guards is not a
    // wrong answer but a walk that never returns. Prefixes rather than
    // random bytes, because a prefix is what a short read produces.
    const struct acpi_sdt_header *dsdt = 0;
    for (int i = 0; i < acpi_dumpable_count(); i++) {
        const struct acpi_sdt_header *h = acpi_dumpable_at(i);
        if (h && k_memcmp(h->signature, "DSDT", 4) == 0) { dsdt = h; break; }
    }
    if (!dsdt) KTEST_SKIP("no DSDT on this machine");

    const uint8_t *aml = (const uint8_t *)dsdt + sizeof *dsdt;
    uint32_t total = dsdt->length - (uint32_t)sizeof *dsdt;
    // A stride, not every prefix: this runs in the live kernel and the
    // point is coverage of the length paths, not exhaustiveness.
    for (uint32_t cut = 1; cut < total; cut += 97) {
        aml_reset();
        aml_parse_table(aml, cut, AML_ROOT);   // must simply RETURN
    }
    // And the whole table still walks afterwards, which is what says the
    // loop above proved something about the parser rather than about a
    // parser it had left broken.
    KTEST_ASSERT(aml_build() > 1);
}
