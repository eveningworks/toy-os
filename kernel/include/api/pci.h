#ifndef PCI_H
#define PCI_H

#include <stdint.h>

// A minimal PCI config-space enumerator -- the first piece of
// networking-prerequisite infrastructure (see README.md's "Ideas for
// what's next" entry on TCP/IP). Legacy port-I/O config-space access
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
    // Raw BAR values (offsets 0x10-0x24), decoded only as far as
    // "I/O or memory, and the base address" -- see pci.c's
    // pci_bar_is_io()/pci_bar_addr() for the decode. NOT size-probed
    // (the write-0xFFFFFFFF-and-read-back trick that reveals a BAR's
    // address-space size) -- deferred until an actual driver needs to
    // map one, since size-probing means temporarily disabling the
    // device's decode and isn't needed just to enumerate/identify it.
    uint32_t bar[6];
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

// The Nth recorded device (0-indexed, N < pci_device_count()). Returns
// NULL if `index` is out of range.
const struct pci_device *pci_device_at(int index);

// A short human-readable label for a class/subclass pair (e.g. "network
// controller", "IDE controller", "VGA-compatible controller") -- covers
// the class codes a QEMU machine or ordinary PC actually presents, not
// the full PCI class-code table. Falls back to "unknown (0x%02x)" for
// anything else. Used by the `lspci` shell command (apps/shell_sys.c)
// so its output reads like something, not just raw hex.
const char *pci_class_name(uint8_t class_code, uint8_t subclass);

// True if BAR `bar` (a raw value from struct pci_device.bar[]) is an
// I/O-space BAR (bit 0 set) rather than a memory-space one.
int pci_bar_is_io(uint32_t bar);

// The base address encoded in `bar`, with the low decode-type bits
// masked off (bit 0 for I/O BARs; bits 0-3 for memory BARs -- see the
// PCI spec's BAR layout). Doesn't distinguish 32-bit/64-bit/prefetchable
// memory BARs beyond that masking -- see this header's top comment on
// why full decoding (and size probing) is deferred to whichever future
// driver actually needs to map one.
uint32_t pci_bar_addr(uint32_t bar);

// Sets the "Bus Master Enable" bit (bit 2) in `dev`'s PCI Command
// register (config-space offset 0x04) -- required before a device can
// actually issue memory read/write cycles for DMA, even though its
// I/O-mapped control registers (a Bus-Master IDE controller's
// BM_CMD/BM_STATUS/BM_PRDT, say -- see ata.c, build 430) will keep
// accepting reads/writes and can still report a nominal "transfer
// complete" status without this set. Read-modify-write, so other
// Command register bits already set by firmware/QEMU are left alone.
// No-op if `dev` is NULL. Any future DMA-capable driver (a NIC,
// chiefly) needs this same call before its own DMA will move real
// data, not just this file's IDE use.
void pci_enable_bus_master(const struct pci_device *dev);

#endif
