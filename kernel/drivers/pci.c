// See pci.h's top comment for the enumeration strategy (brute-force,
// legacy CONFIG_ADDRESS/CONFIG_DATA port I/O) and why it was chosen.
#include "pci.h"
#include "io.h"

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
                }
            }
        }
    }
}

int pci_device_count(void) {
    return g_count;
}

const struct pci_device *pci_device_at(int index) {
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
