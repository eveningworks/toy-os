#ifndef NVME_H
#define NVME_H

#include <stdint.h>
#include "block.h"

struct pci_device;

// An NVM Express controller -- see kernel/drivers/nvme.c.
//
// ONE controller, ONE I/O queue pair, and up to NVME_MAX_NS namespaces,
// each of which block_nvme.c registers as its own disk (nvme0, nvme1, ...).
// Namespaces are addressed by INDEX here, 0 .. nvme_ns_count() - 1, not
// by their NSID, which need not be dense.
//
// Every LBA and count below is in 512-byte sectors, the block layer's
// unit (block.h); a namespace formatted with 4096-byte blocks reports it
// through nvme_ns_block_size() and the driver divides by eight.
#define NVME_MAX_NS 4

int nvme_ns_count(void);
uint32_t nvme_ns_id(int ns);
uint32_t nvme_ns_sector_count(int ns);
uint32_t nvme_ns_block_size(int ns);

int nvme_read(int ns, uint32_t lba, int count, void *buf);
int nvme_write(int ns, uint32_t lba, int count, const void *buf);
// Several transfers in flight on the I/O queue at once, each answering
// in io[i].ok (block.h's submit_batch contract).
int nvme_submit_batch(int ns, struct blk_io *io, int n);
int nvme_flush(int ns);
int nvme_trim_ranges(int ns, const struct blk_range *r, int n);
int nvme_trim(int ns, uint32_t lba, uint32_t count);
int nvme_max_sectors_per_xfer(void);

// Capabilities, from IDENTIFY CONTROLLER. FLUSH only when the controller
// has a volatile write cache (VWC): without one a completed write is
// already durable and there is nothing for a flush to do.
int nvme_has_flush(void);
int nvme_has_trim(void);

// Diagnostics, for the KTESTs and the boot log.
int nvme_irq_driven(void);
uint64_t nvme_irq_count(void);
uint64_t nvme_sleeps(void);
const char *nvme_model(void);
const struct pci_device *nvme_pci(void); // the controller driven, or NULL

#endif
