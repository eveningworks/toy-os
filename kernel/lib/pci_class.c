// pci_class_name(), COMPILED TWICE -- into the kernel and into
// libuapp for /bin/lspci -- so the shell's `lspci` and the ring-3 one
// name a device the same way from one table. Freestanding: it may name
// nothing kernel-only (see the Makefile's shared-source rule).
#include <stdint.h>
#include "pci.h"

// The class/subclass pairs a QEMU machine, or the machines toy-os is
// tested on, present -- not the full class-code table, which /bin/lspci
// reads from pci.ids. **SPELLED AS pci.ids SPELLS THEM**, so wherever
// both have a name they agree, and the database only ever adds names.
const char *pci_class_name(uint8_t class_code, uint8_t subclass) {
    switch (class_code) {
        case 0x00: return "Unclassified device";
        case 0x01:
            switch (subclass) {
                case 0x00: return "SCSI storage controller";
                case 0x01: return "IDE interface";
                case 0x06: return "SATA controller";
                case 0x07: return "Serial Attached SCSI controller";
                case 0x08: return "Non-Volatile memory controller";
                default:   return "Mass storage controller";
            }
        case 0x02:
            switch (subclass) {
                case 0x00: return "Ethernet controller";
                default:   return "Network controller";
            }
        case 0x03:
            switch (subclass) {
                case 0x00: return "VGA compatible controller";
                case 0x02: return "3D controller";
                default:   return "Display controller";
            }
        case 0x04:
            switch (subclass) {
                case 0x01: return "Multimedia audio controller";
                case 0x03: return "Audio device";
                default:   return "Multimedia controller";
            }
        case 0x05: return "Memory controller";
        case 0x06:
            switch (subclass) {
                case 0x00: return "Host bridge";
                case 0x01: return "ISA bridge";
                case 0x04: return "PCI bridge";
                default:   return "Bridge";
            }
        case 0x07: return "Communication controller";
        case 0x08: return "Generic system peripheral";
        case 0x09: return "Input device controller";
        case 0x0C:
            switch (subclass) {
                case 0x03: return "USB controller";
                case 0x05: return "SMBus";
                default:   return "Serial bus controller";
            }
        case 0x0D: return "Wireless controller";
        case 0x11: return "Signal processing controller";
        default: return "Unknown device";
    }
}
