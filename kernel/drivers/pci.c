// See pci.h's top comment for the enumeration strategy (brute-force,
// legacy CONFIG_ADDRESS/CONFIG_DATA port I/O) and why it was chosen.
#include "pci.h"
#include "pci_internal.h"
#include "io.h"
#include "klog.h"
#include "bootstage.h"
#include "knum.h"

// driver-none: the bus a driver scans, not a driver

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static struct pci_device g_devices[PCI_MAX_DEVICES];
static int g_count;

// Builds the CONFIG_ADDRESS value for a given bus/device/function/
// register offset -- see the PCI spec's config-address layout: bit 31
// enables the access, bits 23-16 are the bus, 15-11 the device, 10-8
// the function, 7-2 the register (offset is masked to a 4-byte
// boundary since CONFIG_DATA always reads/writes a whole 32-bit dword
// at a time regardless of what size the caller actually wants).
static uint32_t config_address(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    return 0x80000000u
         | ((uint32_t)bus << 16)
         | ((uint32_t)device << 11)
         | ((uint32_t)function << 8)
         | ((uint32_t)offset & 0xFC);
}

static uint32_t config_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, config_address(bus, device, function, offset));
    return inl(PCI_CONFIG_DATA);
}

// 16-/8-bit reads just pull the right slice out of the 32-bit dword
// CONFIG_DATA always hands back -- there's no narrower config-space
// access mechanism to ask for less.
static uint16_t config_read16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t v = config_read32(bus, device, function, offset);
    return (uint16_t)((v >> ((offset & 2) * 8)) & 0xFFFF);
}

static uint8_t config_read8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t v = config_read32(bus, device, function, offset);
    return (uint8_t)((v >> ((offset & 3) * 8)) & 0xFF);
}

static void config_write16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint16_t value) {
    // CONFIG_DATA only ever accepts a full 32-bit dword write, so a
    // narrower write (this is the only width any caller needs so far --
    // see pci_enable_bus_master() below) has to read-modify-write the
    // dword the target 16 bits live in, same shape config_read16()
    // already uses to pull a narrower READ back out of one.
    uint32_t old = config_read32(bus, device, function, offset);
    uint32_t shift = (offset & 2) * 8;
    uint32_t mask = 0xFFFFu << shift;
    uint32_t updated = (old & ~mask) | ((uint32_t)value << shift);
    outl(PCI_CONFIG_ADDRESS, config_address(bus, device, function, offset));
    outl(PCI_CONFIG_DATA, updated);
}

// Fixed-width hex, no "0x" prefix, no leading-zero trim -- so
// vendor:device/bus:device.function columns line up the same way
// `lspci`'s own print_hex_digits() (apps/shell_sys.c) formats them.
// klog_write_hex() trims leading zeros instead, which is right for a
// one-off value but wrong for a fixed-width field -- which is exactly
// the split k_htoa()'s min_digits argument exists for (see knum.h,
// where this call site is the worked example).
static void klog_hex_digits(uint32_t v, int digits) {
    char buf[17];
    k_htoa(v, buf, sizeof buf, (unsigned)digits);
    klog_write(buf);
}

// Size every memory BAR by the standard probe: decode off, write
// all-ones, read the mask back, put the value back, decode on. Runs
// before interrupts and before the console reaches a framebuffer, and
// the cli guards that ordering rather than anything happening today.
//
// TWO TRAPS. A type-1 (bridge) header has TWO BARs and bus numbers at
// 0x18 -- writing all-ones there renumbers the bus behind it. And a
// host bridge keeps decoding throughout (Linux's mmio_always_on):
// some chipsets hang the machine when it is switched off.
static void probe_bar_sizes(struct pci_device *d) {
    uint8_t type = d->header_type & 0x7F;
    int nbars = type == 0 ? 6 : type == 1 ? 2 : 0;
    if (!nbars) return;

    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");

    int host_bridge = d->class_code == 0x06 && d->subclass == 0x00;
    uint16_t cmd = pci_config_read16(d, 0x04);
    if (!host_bridge && (cmd & (PCI_CMD_IO | PCI_CMD_MEMORY)))
        pci_config_write16(d, 0x04, (uint16_t)(cmd & ~(PCI_CMD_IO | PCI_CMD_MEMORY)));

    for (int i = 0; i < nbars; i++) {
        uint32_t orig = d->bar[i];
        uint8_t  off  = (uint8_t)(0x10 + i * 4);
        if (pci_bar_is_io(orig)) continue;   // memory BARs only

        int wide = pci_bar_is_64(orig) && i + 1 < nbars;
        pci_config_write32(d, off, 0xFFFFFFFFu);
        if (wide) pci_config_write32(d, (uint8_t)(off + 4), 0xFFFFFFFFu);
        uint64_t mask = pci_config_read32(d, off) & 0xFFFFFFF0u;
        if (wide) mask |= (uint64_t)pci_config_read32(d, (uint8_t)(off + 4)) << 32;
        else if (mask) mask |= 0xFFFFFFFF00000000ull;  // a 32-bit BAR's mask stops at bit 31
        if (wide) pci_config_write32(d, (uint8_t)(off + 4), d->bar[i + 1]);
        pci_config_write32(d, off, orig);

        // Mask 0 is a BAR that decodes nothing; the low set bit is the size.
        d->bar_size[i] = mask ? (~mask + 1) : 0;
        if (wide) i++;   // the upper half is not a BAR of its own
    }

    if (!host_bridge) pci_config_write16(d, 0x04, cmd);
    __asm__ volatile ("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}

void pci_init(void) {
    g_count = 0;

    for (uint32_t bus = 0; bus <= 0xFF; bus++) {
        for (uint32_t device = 0; device < 32; device++) {
            for (uint32_t function = 0; function < 8; function++) {
                uint16_t vendor = config_read16((uint8_t)bus, (uint8_t)device, (uint8_t)function, 0x00);
                if (vendor == 0xFFFF) continue; // no device at this bus/device/function

                if (g_count >= PCI_MAX_DEVICES) continue; // full -- keep scanning, just stop recording

                struct pci_device *d = &g_devices[g_count++];
                d->bus = (uint8_t)bus;
                d->device = (uint8_t)device;
                d->function = (uint8_t)function;
                d->vendor_id = vendor;
                d->device_id = config_read16((uint8_t)bus, (uint8_t)device, (uint8_t)function, 0x02);
                d->revision = config_read8((uint8_t)bus, (uint8_t)device, (uint8_t)function, 0x08);
                d->prog_if = config_read8((uint8_t)bus, (uint8_t)device, (uint8_t)function, 0x09);
                d->subclass = config_read8((uint8_t)bus, (uint8_t)device, (uint8_t)function, 0x0A);
                d->class_code = config_read8((uint8_t)bus, (uint8_t)device, (uint8_t)function, 0x0B);
                d->header_type = config_read8((uint8_t)bus, (uint8_t)device, (uint8_t)function, 0x0E);
                d->interrupt_line = config_read8((uint8_t)bus, (uint8_t)device, (uint8_t)function, 0x3C);
                for (int i = 0; i < 6; i++) {
                    d->bar[i] = config_read32((uint8_t)bus, (uint8_t)device, (uint8_t)function, (uint8_t)(0x10 + i * 4));
                    d->bar_size[i] = 0;
                }
                probe_bar_sizes(d);

                // The two interrupt capabilities, recorded once rather
                // than re-walked per query. MSI-X's Message Control is
                // at cap+2 and its low 11 bits are the table size minus
                // one.
                d->msi_cap  = pci_capability_find(d, PCI_CAP_ID_MSI, 0);
                d->msix_cap = pci_capability_find(d, PCI_CAP_ID_MSIX, 0);
                d->msix_entries = d->msix_cap
                    ? (uint16_t)((pci_config_read16(d, (uint8_t)(d->msix_cap + 2)) & 0x7FF) + 1)
                    : 0;
                d->irq_vector = 0;
                d->irq_msix = 0;

                klog_write("pci: ");
                klog_hex_digits(d->bus, 2);
                klog_write(":");
                klog_hex_digits(d->device, 2);
                klog_write(".");
                klog_hex_digits(d->function, 1);
                klog_write("  ");
                klog_hex_digits(d->vendor_id, 4);
                klog_write(":");
                klog_hex_digits(d->device_id, 4);
                klog_write("  ");
                klog_write(pci_class_name(d->class_code, d->subclass));
                klog_write("\n");
            }
        }
    }

    klog_write("pci: ");
    klog_write_dec((uint32_t)g_count);
    klog_write(" device(s) found\n");

    // Marked HERE rather than from kernel_main(), so the flag cannot
    // drift from the thing it claims. See bootstage.h.
    boot_subsystem_up(BOOT_SUB_PCI);
}

int pci_device_count(void) {
    // A scan before pci_init() finds nothing, which reads as "there is
    // no such hardware" rather than as a boot-order bug -- the exact
    // misdirection bootstage.h exists for.
    BOOT_REQUIRE(BOOT_SUB_PCI);
    return g_count;
}

const struct pci_device *pci_device_at(int index) {
    BOOT_REQUIRE(BOOT_SUB_PCI);
    if (index < 0 || index >= g_count) return 0;
    return &g_devices[index];
}

// Covers the class/subclass pairs a QEMU machine or an ordinary PC
// actually presents -- not the full PCI class-code table (dozens of
// entries most of which nothing in this kernel will ever see). See
// pci.h's own comment on this function.
const char *pci_class_name(uint8_t class_code, uint8_t subclass) {
    switch (class_code) {
        case 0x00: return "unclassified device";
        case 0x01:
            switch (subclass) {
                case 0x01: return "IDE controller";
                case 0x06: return "SATA controller";
                default:   return "mass storage controller";
            }
        case 0x02:
            switch (subclass) {
                case 0x00: return "ethernet controller";
                default:   return "network controller";
            }
        case 0x03:
            switch (subclass) {
                case 0x00: return "VGA-compatible controller";
                default:   return "display controller";
            }
        case 0x04: return "multimedia controller";
        case 0x05: return "memory controller";
        case 0x06:
            switch (subclass) {
                case 0x00: return "host bridge";
                case 0x01: return "ISA bridge";
                case 0x04: return "PCI-to-PCI bridge";
                default:   return "bridge device";
            }
        case 0x07: return "communication controller";
        case 0x08: return "system peripheral";
        case 0x09: return "input device controller";
        case 0x0C:
            switch (subclass) {
                case 0x03: return "USB controller";
                default:   return "serial bus controller";
            }
        default: return "unknown device";
    }
}

int pci_bar_is_io(uint32_t bar) {
    return (bar & 0x1) != 0;
}

uint32_t pci_bar_addr(uint32_t bar) {
    if (pci_bar_is_io(bar)) return bar & 0xFFFFFFFCu;  // low 2 bits are decode-type/reserved
    return bar & 0xFFFFFFF0u;                          // low 4 bits are decode-type/prefetchable
}

void pci_enable_bus_master(const struct pci_device *dev) {
    // PCI Command register, offset 0x04, bit 2 ("Bus Master Enable") --
    // without this set, the device won't actually issue memory
    // read/write cycles for DMA at all, even though its I/O-mapped
    // control registers (a Bus-Master IDE controller's BM_CMD/BM_STATUS/
    // BM_PRDT, say) keep accepting reads/writes and can still report a
    // nominal "transfer complete" status -- the classic, easy-to-miss
    // reason a DMA engine that looks fully programmed correctly still
    // silently moves no real data.
    //
    // Kept as its own name because that comment is the value here and
    // ata.c calls it; the read-modify-write it used to do inline is now
    // pci_command_update()'s, so there is one implementation of "touch
    // the Command register" rather than one per bit somebody wanted.
    pci_command_update(dev, PCI_CMD_BUS_MASTER, 0);
}

// --- config space, for drivers (see pci_internal.h) ------------------
//
// Thin unpackers over the file-local B/D/F forms above. The narrowing
// and the read-modify-write live there; these exist so a caller holding
// a `struct pci_device` never has to spread it back out into three
// arguments.

uint8_t pci_config_read8(const struct pci_device *dev, uint8_t offset) {
    if (!dev) return 0xFF;
    return config_read8(dev->bus, dev->device, dev->function, offset);
}

uint16_t pci_config_read16(const struct pci_device *dev, uint8_t offset) {
    if (!dev) return 0xFFFF;
    return config_read16(dev->bus, dev->device, dev->function, offset);
}

uint32_t pci_config_read32(const struct pci_device *dev, uint8_t offset) {
    if (!dev) return 0xFFFFFFFFu;
    return config_read32(dev->bus, dev->device, dev->function, offset);
}

void pci_config_write16(const struct pci_device *dev, uint8_t offset, uint16_t value) {
    if (!dev) return;
    config_write16(dev->bus, dev->device, dev->function, offset, value);
}

void pci_config_write32(const struct pci_device *dev, uint8_t offset, uint32_t value) {
    if (!dev) return;
    // The native width of the mechanism -- no read-modify-write needed,
    // unlike the 16-bit form above.
    outl(PCI_CONFIG_ADDRESS,
         config_address(dev->bus, dev->device, dev->function, offset));
    outl(PCI_CONFIG_DATA, value);
}

// A device with no capability list reports so in Status (offset 0x06)
// bit 4. Walking one anyway would read whatever offset 0x34 happens to
// hold, which on such a device is not a pointer to anything.
#define PCI_STATUS          0x06
#define PCI_STATUS_CAP_LIST 0x0010
#define PCI_CAP_PTR         0x34

// The smallest legal capability offset: everything below 0x40 is the
// standard header, so a `next` pointing there is a malformed device
// rather than a capability.
#define PCI_CAP_MIN_OFFSET  0x40

// 256 bytes of config space with a 4-byte minimum capability gives 64 as
// the true ceiling on chain length. 48 is comfortably past any real
// device (QEMU's present three or four) and still terminates promptly on
// a cycle -- the point is to bound it, not to admit the maximum.
#define PCI_CAP_MAX_HOPS    48

uint8_t pci_capability_find(const struct pci_device *dev, uint8_t cap_id, uint8_t from) {
    if (!dev) return 0;
    if (!(pci_config_read16(dev, PCI_STATUS) & PCI_STATUS_CAP_LIST)) return 0;

    // `from` == 0 starts a walk at the list head; anything else resumes
    // through THAT capability's own next pointer, so a caller can ask
    // for the same id repeatedly and step through every instance of it
    // (which is exactly what virtio needs -- it publishes four or five
    // vendor-specific capabilities and they are told apart by a field
    // inside the payload, not by the id).
    uint8_t offset = from ? pci_config_read8(dev, (uint8_t)(from + 1))
                          : pci_config_read8(dev, PCI_CAP_PTR);

    for (int hops = 0; hops < PCI_CAP_MAX_HOPS; hops++) {
        offset &= 0xFC;                        // spec: dword-aligned
        if (offset < PCI_CAP_MIN_OFFSET) return 0;  // 0 (end) or malformed
        if (pci_config_read8(dev, offset) == cap_id) return offset;
        offset = pci_config_read8(dev, (uint8_t)(offset + 1));
    }
    return 0;  // cycle, or a chain longer than any real device has
}

// Records which LAPIC vector a device ended up on, so `lspci` can say
// so. Looked up by bus/device/function rather than taking the caller's
// pointer as writable: every driver holds a `const struct pci_device *`
// into g_devices, and keeping the ONE mutable path inside this file is
// what stops a driver editing the enumerator's record directly.
void pci_note_vector(const struct pci_device *dev, uint8_t vector, int msix) {
    if (!dev) return;
    for (int i = 0; i < g_count; i++) {
        struct pci_device *d = &g_devices[i];
        if (d->bus == dev->bus && d->device == dev->device &&
            d->function == dev->function) {
            d->irq_vector = vector;
            d->irq_msix = (uint8_t)(msix ? 1 : 0);
            return;
        }
    }
}

int pci_bar_is_64(uint32_t bar) {
    if (pci_bar_is_io(bar)) return 0;
    return ((bar >> 1) & 0x3) == 0x2;
}

uint64_t pci_bar_mem_addr(const struct pci_device *dev, int index) {
    if (!dev || index < 0 || index > 5) return 0;

    uint32_t low = dev->bar[index];
    if (low == 0) return 0;            // unimplemented
    if (pci_bar_is_io(low)) return 0;  // caller wanted memory

    if (!pci_bar_is_64(low)) return (uint64_t)(low & 0xFFFFFFF0u);

    // A 64-bit BAR consumes the NEXT slot as its upper half, so one in
    // slot 5 has nowhere to put those bits. That is a malformed device,
    // not a 32-bit BAR to fall back on -- reading bar[6] would be off
    // the end of the array.
    if (index == 5) return 0;

    return ((uint64_t)dev->bar[index + 1] << 32) | (uint64_t)(low & 0xFFFFFFF0u);
}

uint64_t pci_bar_mem_size(const struct pci_device *dev, int index) {
    if (!dev || index < 0 || index > 5) return 0;
    return dev->bar_size[index];
}

uint16_t pci_command_update(const struct pci_device *dev, uint16_t set, uint16_t clear) {
    if (!dev) return 0;
    uint16_t cmd = pci_config_read16(dev, 0x04);
    uint16_t updated = (uint16_t)((cmd & ~clear) | set);
    pci_config_write16(dev, 0x04, updated);
    return updated;
}
