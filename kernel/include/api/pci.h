#ifndef PCI_H
#define PCI_H

#include <stdint.h>

// A minimal PCI config-space enumerator -- the bus every other device
// driver here finds its hardware on (see docs/roadmap.md's hardware
// track). Legacy port-I/O config-space access
// (CONFIG_ADDRESS/CONFIG_DATA, ports 0xCF8/0xCFC), not the newer
// memory-mapped ECAM mechanism -- CF8/CFC is universally supported
// (including by QEMU's emulated chipset) and needs no ACPI/MCFG table
// parsing to locate, unlike ECAM. Good enough for finding a NIC; a
// future session reaching for PCIe features ECAM-only devices need
// (extended config space past 256 bytes) would have to add that
// separately.
//
// Enumeration is a brute-force scan of every possible bus/device/
// function (256 x 32 x 8 = 65536 config-space reads, each cheap) --
// deliberately not the bridge-aware recursive walk real OSes use
// (scan bus 0, recurse into any PCI-to-PCI bridge's secondary bus).
// This is simpler (no bridge detection, no recursion, no cycle
// safety needed) and finds the same devices on any topology that
// doesn't hide a device behind a bridge whose own upstream path was
// never brute-force-visited -- which can't happen here, since every
// bus number 0-255 is tried directly regardless of whether anything
// upstream claims to lead there. The tradeoff is purely how many
// probe reads happen (mostly wasted on non-existent buses), not
// correctness.
struct pci_device {
    uint8_t bus, device, function;
    uint16_t vendor_id, device_id;
    uint8_t class_code, subclass, prog_if, revision;
    uint8_t header_type;
    uint8_t interrupt_line; // 0xFF conventionally means "not connected"
    uint8_t interrupt_pin;  // 1-4 for INTA-INTD, 0 for no pin
    uint8_t secondary_bus;  // a type-1 bridge: the bus behind it; 0 otherwise
    // Raw BAR values (offsets 0x10-0x24), decoded only as far as
    // "I/O or memory, and the base address" -- see pci.c's
    // pci_bar_is_io()/pci_bar_addr() for the decode.
    uint32_t bar[6];
    // Bytes each MEMORY BAR decodes, size-probed once at enumeration
    // (write all-ones, read back, restore -- the mask's low set bit).
    // 0 for an I/O BAR, an unimplemented one, or the upper-half slot of
    // a 64-bit BAR. Drivers ask pci_bar_mem_size() rather than reading
    // this, so the probe has ONE owner and no driver ever toggles a
    // device's decode.
    uint64_t bar_size[6];
    // Config offset of the MSI / MSI-X capability, 0 when the device
    // has none, and MSI-X's table size in entries. Recorded at
    // enumeration so a reporting tool need not walk config space --
    // which ring 3 has no mechanism for (see pci_internal.h).
    uint8_t  msi_cap, msix_cap;
    uint16_t msix_entries;
    // The LAPIC vector this device was programmed with, or 0 for a
    // device still on its INTx pin, and which of the two capabilities
    // carries it. Written by pci_msi.c when a driver takes one; 0 is
    // safe as "none" because vector 0 is a CPU exception and is never
    // allocatable. `irq_msix` is not derivable from `msix_cap`: a
    // device can advertise MSI-X, fail to have its table programmed,
    // and end up on MSI.
    uint8_t  irq_vector, irq_msix;
};

#define PCI_MAX_DEVICES 32

// Scans every bus/device/function and records what it finds (see this
// header's top comment). Safe to call exactly once, from kernel_main()
// -- idempotent in the sense that calling it again just re-scans and
// overwrites the recorded list, but nothing in this kernel does that
// today. Devices beyond PCI_MAX_DEVICES are silently not recorded
// (plenty for any real machine or QEMU's default topology, which has a
// handful).
void pci_init(void);

// Number of devices pci_init() found (and could fit within
// PCI_MAX_DEVICES).
int pci_device_count(void);

// The PCI-to-PCI bridge whose secondary bus is `bus`, or NULL -- what a
// device's interrupt routing climbs through (kernel/acpi/acpi_prt.c).
const struct pci_device *pci_bridge_for_bus(uint8_t bus);

// The Nth recorded device (0-indexed, N < pci_device_count()). Returns
// NULL if `index` is out of range.
const struct pci_device *pci_device_at(int index);

// A short human-readable label for a class/subclass pair, spelled as
// pci.ids spells it ("Ethernet controller", "VGA compatible controller")
// -- the classes a QEMU machine or an ordinary PC presents, not the full
// table. "Unknown device" for anything else. For the boot log and the
// debug console's lsdev, which cannot read the disk, and as /bin/lspci's
// fallback when pci.ids has no name (kernel/lib/pci_class.c compiles
// into both rings).
const char *pci_class_name(uint8_t class_code, uint8_t subclass);

// True if BAR `bar` (a raw value from struct pci_device.bar[]) is an
// I/O-space BAR (bit 0 set) rather than a memory-space one.
int pci_bar_is_io(uint32_t bar);

// The base address encoded in `bar`, with the low decode-type bits
// masked off (bit 0 for I/O BARs; bits 0-3 for memory BARs -- see the
// PCI spec's BAR layout). Takes a raw dword, so it CANNOT see the
// neighbouring BAR slot and therefore cannot decode a 64-bit memory BAR
// (whose upper half lives in the next one) -- it returns the low half.
//
// That is fine for every caller here, which pass I/O BARs (vmsvga.c's
// bar[0], ata.c's bar4), and it is why the 64-bit decode landed as a
// SEPARATE function rather than as a fix to this one: see
// pci_bar_mem_addr() in kernel/include/kernel/pci_internal.h, added
// when virtio-modern -- whose register windows live in a 64-bit BAR --
// became the first driver that needed one. `lspci` still prints the low
// half of such a BAR; harmless for identification, which is all it does.
uint32_t pci_bar_addr(uint32_t bar);

// Sets the "Bus Master Enable" bit (bit 2) in `dev`'s PCI Command
// register (config-space offset 0x04) -- required before a device can
// actually issue memory read/write cycles for DMA, even though its
// I/O-mapped control registers (a Bus-Master IDE controller's
// BM_CMD/BM_STATUS/BM_PRDT, say -- see ata.c) will keep
// accepting reads/writes and can still report a nominal "transfer
// complete" status without this set. Read-modify-write, so other
// Command register bits already set by firmware/QEMU are left alone.
// No-op if `dev` is NULL. Any future DMA-capable driver (a NIC,
// chiefly) needs this same call before its own DMA will move real
// data, not just this file's IDE use.
void pci_enable_bus_master(const struct pci_device *dev);

#endif
