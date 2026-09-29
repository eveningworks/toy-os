// ACPI, as queryable FACTS. Three classes: what the firmware said
// (scalar), the tables it said it through (a list), and the processors
// the MADT names (a list). The provider lives here, in the subsystem
// that owns the numbers, the same rule kernel/mm/mem_query.c follows.
#include "acpi.h"
#include "acpi_internal.h"
#include "query.h"
#include "string.h"

// Copies a fixed-width, possibly unterminated ACPI field into a
// NUL-terminated one. `dst` must be at least `n + 1` bytes.
static void copy_field(char *dst, const char *src, unsigned n) {
    for (unsigned i = 0; i < n; i++) dst[i] = src[i];
    dst[n] = '\0';
}

static int acpi_fill(int index, void *out) {
    if (index != 0) return 0;
    struct query_acpi *q = out;
    k_memset(q, 0, sizeof *q);

    const struct acpi_state *s = acpi_get_state();
    q->flags         = s->flags;
    q->rsdp_source   = s->rsdp_source;
    q->rsdp_revision = s->rsdp_revision;
    q->table_count   = s->table_count;
    q->rsdt_phys     = s->rsdt_phys;
    q->dsdt_phys     = s->dsdt_phys;
    q->pm1a_cnt      = s->pm1a_cnt;
    q->pm1b_cnt      = s->pm1b_cnt;
    q->smi_cmd       = s->smi_cmd;
    q->acpi_enable   = s->acpi_enable;
    q->slp_typ_a     = s->slp_typ_a;
    q->slp_typ_b     = s->slp_typ_b;
    q->reset_space   = s->reset_reg.space_id;
    q->reset_addr    = s->reset_reg.address;
    q->reset_value   = s->reset_value;
    q->sleep_control_space = s->sleep_control.space_id;
    q->sleep_control_addr  = s->sleep_control.address;
    q->lapic_phys    = s->lapic_phys;
    q->ioapic_count  = s->ioapic_count;
    q->cpu_count     = s->cpu_count;
    return 1;
}

static int acpi_count(void) { return 1; }

static const struct query_field acpi_fields[] = {
    QUERY_FIELD(struct query_acpi, flags,         QUERY_TYPE_U64),
    QUERY_FIELD(struct query_acpi, rsdp_source,   QUERY_TYPE_U64),
    QUERY_FIELD(struct query_acpi, rsdp_revision, QUERY_TYPE_U64),
    QUERY_FIELD(struct query_acpi, table_count,   QUERY_TYPE_U64),
    QUERY_FIELD(struct query_acpi, pm1a_cnt,      QUERY_TYPE_U64),
    QUERY_FIELD(struct query_acpi, pm1b_cnt,      QUERY_TYPE_U64),
    QUERY_FIELD(struct query_acpi, smi_cmd,       QUERY_TYPE_U64),
    QUERY_FIELD(struct query_acpi, slp_typ_a,     QUERY_TYPE_U64),
    QUERY_FIELD(struct query_acpi, slp_typ_b,     QUERY_TYPE_U64),
    QUERY_FIELD(struct query_acpi, reset_addr,    QUERY_TYPE_U64),
    QUERY_FIELD(struct query_acpi, reset_value,   QUERY_TYPE_U64),
    QUERY_FIELD(struct query_acpi, lapic_phys,    QUERY_TYPE_U64),
    QUERY_FIELD(struct query_acpi, ioapic_count,  QUERY_TYPE_U64),
    QUERY_FIELD(struct query_acpi, cpu_count,     QUERY_TYPE_U64),
};

static const struct query_provider acpi_provider = {
    .cls = QUERY_ACPI,
    .name = "acpi",
    .record_size = sizeof(struct query_acpi),
    .flags = 0,
    .count = acpi_count,
    .fill = acpi_fill,
    .fields = acpi_fields,
    .field_count = sizeof acpi_fields / sizeof acpi_fields[0],
};

static int table_count(void) { return acpi_table_count(); }

static int table_fill(int index, void *out) {
    const struct acpi_sdt_header *h = acpi_table_at(index);
    if (!h) return 0;

    struct query_acpi_table *q = out;
    k_memset(q, 0, sizeof *q);
    q->address  = (uint64_t)(uintptr_t)h;
    q->length   = h->length;
    q->revision = h->revision;
    copy_field(q->signature, h->signature, 4);
    copy_field(q->oem_id, h->oem_id, 6);
    copy_field(q->oem_table_id, h->oem_table_id, 8);
    return 1;
}

// --- QUERY_ACPIDUMP: the raw bytes, in slices ------------------------
//
// One record per QUERY_ACPIDUMP_DATA bytes of one table, walked table by
// table. The index is a position in the CONCATENATION, because a list
// provider's index has nowhere to carry a second selector -- so a reader
// groups by the `signature` each record carries.

static uint32_t slices_of(uint32_t len) {
    return (len + QUERY_ACPIDUMP_DATA - 1) / QUERY_ACPIDUMP_DATA;
}

// Resolves a flat record index to (table, slice), or -1. Shared by count
// and fill so the two cannot disagree about the ordering.
static int locate(int index, int *out_tbl, uint32_t *out_slice) {
    int n = acpi_dumpable_count();
    for (int i = 0; i < n; i++) {
        const struct acpi_sdt_header *h = acpi_dumpable_at(i);
        if (!h || h->length == 0) continue;
        uint32_t slices = slices_of(h->length);
        if ((uint32_t)index < slices) {
            *out_tbl = i;
            *out_slice = (uint32_t)index;
            return 0;
        }
        index -= (int)slices;
    }
    return -1;
}

static int dump_count(void) {
    int total = 0, n = acpi_dumpable_count();
    for (int i = 0; i < n; i++) {
        const struct acpi_sdt_header *h = acpi_dumpable_at(i);
        if (h && h->length) total += (int)slices_of(h->length);
    }
    return total;
}

static int dump_fill(int index, void *out) {
    int tbl = 0;
    uint32_t slice = 0;
    if (index < 0 || locate(index, &tbl, &slice) < 0) return 0;
    const struct acpi_sdt_header *h = acpi_dumpable_at(tbl);
    if (!h) return 0;

    struct query_acpidump *q = out;
    k_memset(q, 0, sizeof *q);
    q->table  = (uint64_t)tbl;
    q->total  = h->length;
    q->offset = slice * QUERY_ACPIDUMP_DATA;
    q->len    = h->length - q->offset;
    if (q->len > QUERY_ACPIDUMP_DATA) q->len = QUERY_ACPIDUMP_DATA;
    copy_field(q->signature, h->signature, 4);
    k_memcpy(q->data, (const uint8_t *)h + q->offset, q->len);
    return 1;
}

static const struct query_provider dump_provider = {
    .cls = QUERY_ACPIDUMP,
    .name = "acpidump",
    .record_size = sizeof(struct query_acpidump),
    .flags = QUERY_F_LIST,
    .count = dump_count,
    .fill = dump_fill,
    .fields = 0,
    .field_count = 0,
};

static const struct query_provider table_provider = {
    .cls = QUERY_ACPI_TABLE,
    .name = "acpitable",
    .record_size = sizeof(struct query_acpi_table),
    .flags = QUERY_F_LIST,
    .count = table_count,
    .fill = table_fill,
    .fields = 0,
    .field_count = 0,
};

_Static_assert(ACPI_CPU_ENABLED == QUERY_CPU_ENABLED &&
               ACPI_CPU_ONLINE_CAPABLE == QUERY_CPU_ONLINE_CAPABLE &&
               ACPI_CPU_X2APIC == QUERY_CPU_X2APIC,
               "QUERY_CPUS hands the kernel's flags through unchanged");

static int cpu_count_fn(void) { return acpi_cpu_count(); }

static int cpu_fill(int index, void *out) {
    const struct acpi_cpu *c = acpi_cpu_at(index);
    if (!c) return 0;

    struct query_cpu *q = out;
    q->acpi_id = c->acpi_id;
    q->apic_id = c->apic_id;
    q->flags   = c->flags;
    return 1;
}

static const struct query_provider cpu_provider = {
    .cls = QUERY_CPUS,
    .name = "cpus",
    .record_size = sizeof(struct query_cpu),
    .flags = QUERY_F_LIST,
    .count = cpu_count_fn,
    .fill = cpu_fill,
    .fields = 0,
    .field_count = 0,
};

void acpi_query_init(void) {
    query_register(&acpi_provider);
    query_register(&table_provider);
    query_register(&dump_provider);
    query_register(&cpu_provider);
}
