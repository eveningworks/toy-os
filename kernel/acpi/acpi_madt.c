// The MADT: which processors exist, and where the interrupt
// controllers are. NOTHING IS STARTED -- this is a list of facts, and
// `lscpu` printing "online: no" against every core is the honest
// report until docs/smp-design.md's later stages exist.
#include "acpi.h"
#include "acpi_internal.h"
#include "klog.h"
#include "kfmt.h" // klog_printf
#include "string.h"

// Bounded by what `procs[]` could ever schedule, not by what a machine
// could report. A host with more cores than this has the rest ignored,
// which is a truthful "this kernel counted 64" rather than a walk off
// the end of the array.
#define ACPI_CPU_MAX 64

static struct acpi_cpu g_cpus[ACPI_CPU_MAX];

#define MADT_LAPIC        0
#define MADT_IOAPIC       1
#define MADT_LAPIC_OVERRIDE 5
#define MADT_X2APIC       9

int acpi_cpu_count(void) { return (int)acpi_get_state()->cpu_count; }

const struct acpi_cpu *acpi_cpu_at(int index) {
    if (index < 0 || index >= (int)acpi_get_state()->cpu_count) return 0;
    return &g_cpus[index];
}

static void add_cpu(uint32_t acpi_id, uint32_t apic_id, uint32_t entry_flags, int x2) {
    struct acpi_state *s = acpi_state_mut();
    if (s->cpu_count >= ACPI_CPU_MAX) return;

    struct acpi_cpu *c = &g_cpus[s->cpu_count++];
    c->acpi_id = acpi_id;
    c->apic_id = apic_id;
    c->flags = 0;
    // Bit 0 is "enabled"; bit 1 means the firmware may bring it up
    // later. A core with NEITHER set is listed anyway -- it exists, and
    // hiding it would make the count disagree with the firmware's.
    if (entry_flags & 1u) c->flags |= ACPI_CPU_ENABLED;
    if (entry_flags & 2u) c->flags |= ACPI_CPU_ONLINE_CAPABLE;
    if (x2) c->flags |= ACPI_CPU_X2APIC;
}

void acpi_madt_init(void) {
    struct acpi_state *s = acpi_state_mut();

    const struct acpi_sdt_header *madt = 0;
    for (int i = 0; i < acpi_table_count(); i++)
        if (k_memcmp(acpi_table_at(i)->signature, "APIC", 4) == 0) {
            madt = acpi_table_at(i);
            break;
        }
    if (!madt || madt->length < sizeof *madt + 8) return;

    const uint8_t *b = (const uint8_t *)madt;
    uint32_t lapic = 0;
    for (int i = 3; i >= 0; i--) lapic = (lapic << 8) | b[36 + i];
    s->lapic_phys = lapic;

    // Entries are a type/length pair each. A zero length would loop
    // forever on a malformed table, so it ends the walk.
    uint32_t off = 44;
    while (off + 2 <= madt->length) {
        uint8_t type = b[off];
        uint8_t len = b[off + 1];
        if (len < 2 || off + len > madt->length) break;

        if (type == MADT_LAPIC && len >= 8) {
            uint32_t f = 0;
            for (int i = 3; i >= 0; i--) f = (f << 8) | b[off + 4 + i];
            add_cpu(b[off + 2], b[off + 3], f, 0);
        } else if (type == MADT_X2APIC && len >= 16) {
            uint32_t apic = 0, f = 0, uid = 0;
            for (int i = 3; i >= 0; i--) apic = (apic << 8) | b[off + 4 + i];
            for (int i = 3; i >= 0; i--) f = (f << 8) | b[off + 8 + i];
            for (int i = 3; i >= 0; i--) uid = (uid << 8) | b[off + 12 + i];
            add_cpu(uid, apic, f, 1);
        } else if (type == MADT_IOAPIC && len >= 12) {
            s->ioapic_count++;
        } else if (type == MADT_LAPIC_OVERRIDE && len >= 12) {
            // A 64-bit override of the address at offset 36. It exists
            // because that field is 32 bits and the register block can
            // sit above 4 GiB; taking it is not optional.
            uint64_t addr = 0;
            for (int i = 7; i >= 0; i--) addr = (addr << 8) | b[off + 4 + i];
            s->lapic_phys = addr;
        }

        off += len;
    }

    klog_printf("acpi: MADT lists %d processor(s), %d I/O APIC(s), LAPIC at 0x%x\n",
                (int)s->cpu_count, (int)s->ioapic_count, (uint32_t)s->lapic_phys);
}
