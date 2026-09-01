// The ATA disk's transfer path, as a queryable FACT.
//
// It answers the question `ata` at the shell existed for: are transfers
// going through DMA or the PIO fallback, and is that a choice or the
// only option this machine offers. Three states, not two -- "no
// Bus-Master DMA on this controller" and "DMA is available but forced
// off" look identical from a throughput number and mean completely
// different things.
//
// The FORCING is not here: it is kernel.ata_nodma, a tunable
// (kernel/lib/tunables.c). This class reports; that writes. Keeping the
// two halves in one command is exactly why the tunable exists.
#include "query.h"
#include "ata.h"
#include "string.h"
#include <stddef.h>

// driver-none: a QUERY provider over what ata.c found

static int ata_count(void) { return 1; }

static int ata_fill(int index, void *out) {
    if (index != 0) return 0;
    struct query_ata *a = out;
    k_memset(a, 0, sizeof *a);
    if (ata_present())                 a->flags |= QUERY_ATA_PRESENT;
    if (ata_dma_hardware_available())  a->flags |= QUERY_ATA_DMA_HW;
    if (ata_dma_active())              a->flags |= QUERY_ATA_DMA_ON;
    if (ata_trim_supported())          a->flags |= QUERY_ATA_TRIM;
    a->max_sectors_xfer = (uint64_t)ata_max_sectors_per_xfer();
    a->sector_count     = (uint64_t)ata_sector_count();
    return 1;
}

static const struct query_field ata_fields[] = {
    QUERY_FIELD(struct query_ata, flags,            QUERY_TYPE_U64),
    QUERY_FIELD(struct query_ata, max_sectors_xfer, QUERY_TYPE_U64),
    QUERY_FIELD(struct query_ata, sector_count,     QUERY_TYPE_U64),
};

static const struct query_provider ata_provider = {
    .cls = QUERY_ATA,
    .name = "ata",
    .record_size = sizeof(struct query_ata),
    .flags = 0,
    .count = ata_count,
    .fill = ata_fill,
    .fields = ata_fields,
    .field_count = sizeof ata_fields / sizeof ata_fields[0],
};

void ata_query_init(void) {
    query_register(&ata_provider);
}
