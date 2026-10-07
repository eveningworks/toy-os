// The AHCI controllers and their ports, as queryable FACTS.
//
// TWO CLASSES, and the split is QUERY_PARTTABLE/QUERY_PARTITION's: the
// HBA is one record with one set of numbers, and a port is a row. One
// class carrying both would repeat the version and the capability bits
// on every row.
#include "query.h"
#include "ahci.h"
#include "block.h" // blk_ahci_device() -- a port's device name
#include "string.h"
#include <stddef.h>
#include "initcall.h"

// driver-none: a QUERY provider over what ahci.c found

// At least one record, so "no controller" reads back PRESENT-clear.
static int hba_count(void) { return ahci_hba_count() ? ahci_hba_count() : 1; }

static void copy_model(char *out, const char *model) {
    for (int i = 0; i < QUERY_AHCI_MODEL_MAX - 1 && model[i]; i++) out[i] = model[i];
}

// The HBA's first drive: the lowest-numbered drive whose `hba` is it.
static int first_drive_of(int hba, struct ahci_drive_info *out) {
    for (int d = 0; d < ahci_drive_count(); d++)
        if (ahci_drive_info(d, out) && out->hba == hba) return 1;
    return 0;
}

static int hba_fill(int index, void *out) {
    struct query_ahci *a = out;
    k_memset(a, 0, sizeof *a);
    if (index == 0 && !ahci_hba_count()) return 1;

    struct ahci_hba_info h;
    if (!ahci_hba_info(index, &h)) return 0;
    a->flags |= QUERY_AHCI_PRESENT;
    if (h.irq_driven)       a->flags |= QUERY_AHCI_IRQ;
    if (h.cap & (1u << 31)) a->flags |= QUERY_AHCI_64BIT;
    if (h.cap & (1u << 30)) a->flags |= QUERY_AHCI_NCQ;
    if (h.cap & (1u << 27)) a->flags |= QUERY_AHCI_SSS;
    a->version       = h.version;
    a->ports_impl    = (uint64_t)h.port_count;
    a->command_slots = (uint64_t)h.command_slots;
    a->irq           = h.irq;
    a->drives        = (uint64_t)h.drives;
    a->active_port   = (uint64_t)(int64_t)-1;

    struct ahci_drive_info d;
    if (!first_drive_of(index, &d)) return 1;
    a->flags |= QUERY_AHCI_DRIVE;
    if (d.lba48) a->flags |= QUERY_AHCI_LBA48;
    if (d.trim)  a->flags |= QUERY_AHCI_TRIM;
    a->active_port      = (uint64_t)d.port;
    a->sector_count     = d.sectors;
    a->cmd_sleeps       = d.cmd_sleeps;
    a->ncq_depth        = (uint64_t)d.ncq_depth;
    a->ncq_rounds       = d.ncq_rounds;
    a->ncq_cmds         = d.ncq_cmds;
    a->ncq_fallbacks    = d.ncq_fallbacks;
    a->max_sectors_xfer = (uint64_t)d.max_xfer;
    copy_model(a->model, d.model);
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
    QUERY_FIELD(struct query_ahci, drives,           QUERY_TYPE_U64),
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

// Every implemented port of every HBA, HBA by HBA.
static int port_count(void) {
    int n = 0;
    struct ahci_hba_info h;
    for (int i = 0; ahci_hba_info(i, &h); i++) n += h.port_count;
    return n;
}

static int port_fill(int index, void *out) {
    struct ahci_hba_info h;
    int hba = 0;
    for (; ahci_hba_info(hba, &h); hba++) {
        if (index < h.port_count) break;
        index -= h.port_count;
    }
    struct ahci_port_status s;
    if (!ahci_port_status(hba, index, &s)) return 0;
    struct query_ahci_port *p = out;
    k_memset(p, 0, sizeof *p);
    p->port      = s.port;
    p->det       = s.det;
    p->ipm       = s.ipm;
    p->speed     = s.speed;
    p->signature = s.signature;
    p->hba       = (uint64_t)hba;
    p->drive     = s.drive;
    if (s.det == 3)  p->flags |= QUERY_AHCI_PORT_DEVICE;
    if (s.running)   p->flags |= QUERY_AHCI_PORT_RUNNING;

    struct ahci_drive_info d;
    if (s.drive < 0 || !ahci_drive_info(s.drive, &d)) return 1;
    const struct block_device *dev = blk_ahci_device(s.drive);
    if (dev) {
        p->flags |= QUERY_AHCI_PORT_ACTIVE;
        k_strlcpy(p->device, blk_device_name(dev), sizeof p->device);
    }
    if (d.lba48)      p->flags |= QUERY_AHCI_PORT_LBA48;
    if (d.trim)       p->flags |= QUERY_AHCI_PORT_TRIM;
    if (d.irq_driven) p->flags |= QUERY_AHCI_PORT_IRQ;
    p->sector_count     = d.sectors;
    p->max_sectors_xfer = (uint64_t)d.max_xfer;
    p->cmd_sleeps       = d.cmd_sleeps;
    p->ncq_depth        = (uint64_t)d.ncq_depth;
    p->ncq_rounds       = d.ncq_rounds;
    p->ncq_cmds         = d.ncq_cmds;
    p->ncq_fallbacks    = d.ncq_fallbacks;
    copy_model(p->model, d.model);
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
