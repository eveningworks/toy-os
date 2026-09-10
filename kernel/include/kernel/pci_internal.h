#ifndef PCI_INTERNAL_H
#define PCI_INTERNAL_H

#include <stdint.h>
#include "pci.h"

// PCI config space, for DRIVERS -- the half of pci.c that apps must not
// have.
//
// WHY THIS IS NOT IN api/pci.h
// ----------------------------
// `api/pci.h` is on userland's include path (userland/bin/lspci.c
// includes it), so anything declared there is a promise to ring 3.
// "Any app may read and write PCI config space" is not a promise this
// kernel should make: a config-space WRITE can disable a device's
// decode, move a BAR on top of another device's, or turn off bus
// mastering under a driver that is mid-DMA. `lspci` gets its data
// through SYS_PCI_COUNT/SYS_PCI_INFO, which hands over a snapshot
// struct and no mechanism.
//
// kernel/include/README.md's rule is the general form of this: a header
// starts in `kernel/` and moves to `api/` only when an app genuinely
// needs it, and that move is a decision rather than a convenience.
//
// WHY THE SIGNATURES TAKE A DEVICE RATHER THAN B/D/F
// --------------------------------------------------
// pci.c's own internals address config space as (bus, device, function,
// offset) because pci_init() reads it before a `struct pci_device`
// exists to describe what it found. Every caller OUT here already holds
// the struct, and a four-integer positional call is the shape that gets
// transposed silently -- (bus, device, function) and (device, function,
// offset) are both three small integers and the compiler cannot tell
// them apart. So the exported form takes the device and one offset.

// Config-space reads. CONFIG_DATA only ever transfers a whole 32-bit
// dword, so the narrower two pull their slice out of one -- there is no
// narrower access mechanism to ask the hardware for.
uint8_t  pci_config_read8 (const struct pci_device *dev, uint8_t offset);
uint16_t pci_config_read16(const struct pci_device *dev, uint8_t offset);
uint32_t pci_config_read32(const struct pci_device *dev, uint8_t offset);

// The only config-space WRITE width anything here needs. Read-modify-
// writes the containing dword, so neighbouring registers are undisturbed.
void pci_config_write16(const struct pci_device *dev, uint8_t offset, uint16_t value);

// The mechanism's NATIVE width, needed by MSI: a message address is a
// 32-bit register and writing it as two halves would leave the device
// briefly pointed at a spliced address.
void pci_config_write32(const struct pci_device *dev, uint8_t offset, uint32_t value);

// --- the capability list ---------------------------------------------
//
// A PCI device advertises optional features as a singly-linked list
// inside config space: Status bit 4 says a list exists, offset 0x34
// points at the first entry, and every entry's first two bytes are
// {id, next_offset} with 0 terminating. That uniform two-byte prologue
// is what makes one walker serve every capability type.
//
// This is how virtio-modern is found at all: it publishes its register
// windows as several vendor-specific (0x09) capabilities, each naming a
// BAR plus an offset and length within it.

#define PCI_CAP_ID_PM   0x01  // power management
#define PCI_CAP_ID_VNDR 0x09  // vendor-specific -- what virtio uses
#define PCI_CAP_ID_MSI  0x05
#define PCI_CAP_ID_MSIX 0x11

// Config-space OFFSET of the first capability with `cap_id` at or after
// `from`; pass 0 to start a walk, or the offset this last returned to
// continue one (it resumes through that capability's own `next`, so a
// device publishing several of the same id can be walked end to end).
//
// Returns 0 when there is none. 0 needs no separate sentinel because it
// is not a legal capability offset -- config offset 0 is the vendor ID,
// and the spec requires capabilities to live at 0x40 or above.
//
// TWO SAFETY PROPERTIES, both of which are real failure modes rather
// than defensive habit: the `next` pointers come from the DEVICE, so a
// malformed or malicious one can point a capability at itself and hang
// the walk (hence the hop bound), and firmware does not reliably zero
// the low two bits the spec reserves (hence the mask).
uint8_t pci_capability_find(const struct pci_device *dev, uint8_t cap_id, uint8_t from);

// --- BARs ------------------------------------------------------------

// True if `bar` is a 64-bit memory BAR -- bits 2:1 == 0b10. Such a BAR
// is TWO dwords: the next slot holds its upper 32 bits and is not a BAR
// in its own right.
int pci_bar_is_64(uint32_t bar);

// The full base address of BAR `index`, combining bar[index] with
// bar[index + 1] when the former says 64-bit.
//
// Returns 0 for an unimplemented BAR, an I/O BAR, or a 64-bit BAR in
// slot 5 (which has no upper half to combine with and is a malformed
// device). This is deliberately a SEPARATE function from api/pci.h's
// pci_bar_addr() rather than a fix to it: that one takes a raw dword
// with no way to reach the neighbouring slot, and its callers
// (vmsvga.c, ata.c) pass I/O BARs where the 64-bit rule does not apply.
uint64_t pci_bar_mem_addr(const struct pci_device *dev, int index);

// Bytes BAR `index` decodes, or 0 for the cases pci_bar_mem_addr()
// returns 0 for. Probed ONCE in pci_init(), before any driver and before
// the console reaches a framebuffer -- the probe turns the device's
// decode off, so a driver-time probe would run under whoever is using
// the device (Linux sizes at enumeration for the same reason).
uint64_t pci_bar_mem_size(const struct pci_device *dev, int index);

// --- the Command register --------------------------------------------

#define PCI_CMD_IO           0x0001  // respond to I/O-space accesses
#define PCI_CMD_MEMORY       0x0002  // respond to memory-space accesses
#define PCI_CMD_BUS_MASTER   0x0004  // may issue DMA cycles
#define PCI_CMD_INTX_DISABLE 0x0400  // do not assert the legacy INTx line

// Read-modify-writes the Command register (offset 0x04): `set` bits go
// on, `clear` bits come off, `set` wins a conflict. Returns the value
// written, or 0 if `dev` is NULL.
//
// A polled driver wants PCI_CMD_INTX_DISABLE: INTx is LEVEL-triggered
// and shared, so a device left free to assert it with nothing installed
// to acknowledge it holds the line down for every other device on it.
uint16_t pci_command_update(const struct pci_device *dev, uint16_t set, uint16_t clear);

// --- MSI (kernel/drivers/pci_msi.c) -----------------------------------
//
// Points the device's MSI capability at `vector` on this CPU's LAPIC
// and disables its INTx pin. Returns 1 when the device took it, 0 when
// it has no MSI capability or there is no LAPIC to deliver to -- both
// of which are ordinary answers, and the caller's cue to stay on its
// line. `vector` comes from lapic_alloc_vector().
int pci_msi_enable(const struct pci_device *dev, uint8_t vector);

// MSI-X: the same, with the message table in a BAR instead of in config
// space. PREFER THIS ONE -- it is what a PCIe device actually offers
// (QEMU's own xHCI has MSI-X and no MSI), and it is the only form that
// could later give a multi-queue device a vector per queue. One entry
// is programmed; the rest of the table stays masked.
int pci_msix_enable(const struct pci_device *dev, uint8_t vector);

// Undoes that: MSI-X off, INTx back on. Needed because MSI-X OUTRANKS
// the pin while it is enabled, so a caller that gave up half way and
// fell back to its line would find the line never asserts.
void pci_msix_disable(const struct pci_device *dev);

// WHAT A DRIVER SHOULD ACTUALLY CALL: claim a vector for `handler` and
// try MSI-X then MSI. Returns the vector, or 0 meaning "this device has
// neither -- use your pin", which is an ordinary answer. Whether a pin
// is usable, and what to do when it is not, stays with the driver.
uint8_t pci_msi_request(const struct pci_device *dev, void (*handler)(uint64_t *regs));

// The inverse of pci_msi_request(): the message capability off (MSI-X
// or MSI, whichever took the vector), INTx back on, the vector freed.
// A driver leaving -- a module unloading -- calls it after masking the
// device's own interrupt sources, so no message is in flight.
void pci_msi_release(const struct pci_device *dev, uint8_t vector);

// Records the vector a device was given, for `lspci`. Called by
// pci_msi.c only; a driver reads it back as pci_device.irq_vector.
void pci_note_vector(const struct pci_device *dev, uint8_t vector, int msix);

#endif
