// pci_class_name(), COMPILED TWICE -- into the kernel and into
// libuapp for /bin/lspci -- so the shell's `lspci` and the ring-3 one
// name a device the same way from one table. Freestanding: it may name
// nothing kernel-only (see the Makefile's shared-source rule).
#include <stdint.h>
#include "pci.h"

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
