#ifndef KERNEL_DEVEVENT_H
#define KERNEL_DEVEVENT_H

#include "query_abi.h"   // QUERY_DEVEV_* -- the kinds are ABI

struct pci_device;

// THE DEVICES' LIFECYCLE, this boot -- Windows' Kernel-PnP log, Linux's
// uevents, KEPT rather than broadcast: nothing listens for a device
// event yet, so a reader asks afterwards (QUERY_DEVEVENT). A fixed ring
// of the latest events; the oldest are overwritten.
//
// LIFECYCLE ONLY -- bound, declined, released, claimed, plugged in. A
// driver's own chatter stays in the kernel log; this is what Device
// Manager can show without parsing it, and what survives the log
// wrapping.
//
// `device_id` is userland/lib/udevice.c's naming: "pci:00:1f.2",
// "usb:14:2357:0601", "blk:ahci0". Callable from any context, an
// interrupt handler included.
void devevent_add(int kind, const char *device_id, const char *driver,
                  const char *fmt, ...) __attribute__((format(printf, 4, 5)));

// The same for a PCI device, named from its address.
void devevent_pci(int kind, const struct pci_device *d, const char *driver,
                  const char *fmt, ...) __attribute__((format(printf, 4, 5)));

// The newest event, for a test asserting that something recorded one.
// 0 when the ring is empty.
int devevent_latest(struct query_devevent *out);

// "pci:00:1f.2" into `out`; the form every PCI device_id field uses.
void devevent_pci_id(const struct pci_device *d, char *out, unsigned cap);

#endif
