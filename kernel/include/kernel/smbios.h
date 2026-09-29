#ifndef KERNEL_SMBIOS_H
#define KERNEL_SMBIOS_H

#include <stdint.h>
#include "query_abi.h"

// SMBIOS / DMI: the firmware's description of the machine -- its maker,
// its model, the firmware's own version. Read once at boot, served as
// QUERY_SMBIOS. Found by scanning the BIOS area for the entry point,
// which is where a BIOS (or UEFI CSM) boot through GRUB i386-pc leaves
// it; an EFI boot would need Multiboot2's EFI system-table tag, which
// nothing here reads yet, and then reports `found` 0.

// Parses a structure TABLE (not the entry point) of `len` bytes into
// `out`, zeroing it first. UNTRUSTED INPUT: every read is bounded by
// `len`, a structure that does not fit ends the walk, and a string with
// a byte outside printable ASCII is dropped rather than repaired.
// Returns the number of structures read.
int smbios_parse(const uint8_t *tbl, uint32_t len, struct query_smbios *out);

// What boot found; `found` is 0 when there was nothing.
const struct query_smbios *smbios_info(void);

#endif
