// The AHCI controller and its ports, as queryable FACTS.
//
// TWO CLASSES, and the split is QUERY_PARTTABLE/QUERY_PARTITION's: the
// HBA is one thing with one set of numbers, and a port is a row. One
// class carrying both would repeat the version and the capability bits
// on every row.
#include "query.h"
#include "ahci.h"
#include "string.h"
#include <stddef.h>
#include "initcall.h"

// driver-none: a QUERY provider over what ahci.c found

static int hba_count(void) { return 1; }

static int hba_fill(int index, void *out) {
    if (index != 0) return 0;
    struct query_ahci *a = out;
    k_memset(a, 0, sizeof *a);

    uint32_t cap = ahci_capabilities();
    if (ahci_controller_present()) a->flags |= QUERY_AHCI_PRESENT;
    if (ahci_present())            a->flags |= QUERY_AHCI_DRIVE;
    if (ahci_irq_driven())         a->flags |= QUERY_AHCI_IRQ;
    if (cap & (1u << 31))          a->flags |= QUERY_AHCI_64BIT;
    if (cap & (1u << 30))          a->flags |= QUERY_AHCI_NCQ;
    if (cap & (1u << 27))          a->flags |= QUERY_AHCI_SSS;
    if (ahci_lba48())              a->flags |= QUERY_AHCI_LBA48;
    if (ahci_trim_supported())     a->flags |= QUERY_AHCI_TRIM;

    a->version          = ahci_version();
    a->ports_impl       = (uint64_t)ahci_port_count();
    a->command_slots    = (uint64_t)ahci_command_slots();
    a->active_port      = (uint64_t)(int64_t)ahci_active_port();
    a->irq              = ahci_irq_line();
    a->sector_count     = ahci_sector_count();
    a->cmd_sleeps       = ahci_cmd_sleeps();
    a->ncq_depth        = (uint64_t)ahci_ncq_depth();
    a->ncq_rounds       = ahci_ncq_rounds();
    a->ncq_cmds         = ahci_ncq_cmds();
    a->ncq_fallbacks    = ahci_ncq_fallbacks();
    a->max_sectors_xfer = ahci_present() ? (uint64_t)ahci_max_sectors_per_xfer() : 0;

    const char *model = ahci_model();
    for (int i = 0; i < QUERY_AHCI_MODEL_MAX - 1 && model[i]; i++) a->model[i] = model[i];
    return 1;
}

static const struct query_field hba_fields[] = {
    QUERY_FIELD(struct query_ahci, flags,            QUERY_TYPE_U64),
    QUERY_FIELD(struct query_ahci, version,          QUERY_TYPE_U64),
    QUERY_FIELD(struct query_ahci, ports_impl,       QUERY_TYPE_U64),
    QUERY_FIELD(struct query_ahci, command_slots,    QUERY_TYPE_U64),
    QUERY_FIELD(struct query_ahci, active_port,      QUERY_TYPE_U64),
    QUERY_FIELD(struct query_ahci, irq,              QUERY_TYPE_U64),
    QUERY_FIELD(struct query_ahci, sector_count,     QUERY_TYPE_U64),
    QUERY_FIELD(struct query_ahci, cmd_sleeps,       QUERY_TYPE_U64),
    QUERY_FIELD(struct query_ahci, ncq_depth,        QUERY_TYPE_U64),
    QUERY_FIELD(struct query_ahci, ncq_rounds,       QUERY_TYPE_U64),
    QUERY_FIELD(struct query_ahci, ncq_fallbacks,    QUERY_TYPE_U64),
    QUERY_FIELD(struct query_ahci, max_sectors_xfer, QUERY_TYPE_U64),
};

static const struct query_provider hba_provider = {
    .cls = QUERY_AHCI,
    .name = "ahci",
    .record_size = sizeof(struct query_ahci),
    .flags = 0,
    .count = hba_count,
    .fill = hba_fill,
    .fields = hba_fields,
    .field_count = sizeof hba_fields / sizeof hba_fields[0],
};

static int port_count(void) { return ahci_port_count(); }

static int port_fill(int index, void *out) {
    struct ahci_port_status s;
    if (!ahci_port_status(index, &s)) return 0;
    struct query_ahci_port *p = out;
    k_memset(p, 0, sizeof *p);
    p->port      = s.port;
    p->det       = s.det;
    p->ipm       = s.ipm;
    p->speed     = s.speed;
    p->signature = s.signature;
    if (s.det == 3)  p->flags |= QUERY_AHCI_PORT_DEVICE;
    if (s.running)   p->flags |= QUERY_AHCI_PORT_RUNNING;
    if (s.active)    p->flags |= QUERY_AHCI_PORT_ACTIVE;
    return 1;
}

static const struct query_provider port_provider = {
    .cls = QUERY_AHCI_PORT,
    .name = "ahciport",
    .record_size = sizeof(struct query_ahci_port),
    .flags = QUERY_F_LIST,
    .count = port_count,
    .fill = port_fill,
    // No named fields: a list is read whole, since an index baked into
    // a name means a different record a second later (api/query.h).
    .fields = 0,
    .field_count = 0,
};

void ahci_query_init(void) {
    query_register(&hba_provider);
    query_register(&port_provider);
}
INITCALL(ahci_query_init, INIT_QUERY);
