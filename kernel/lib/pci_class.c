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
//
// A TABLE, not a switch: it is data, and a switch over it outgrew
// tools/check_dispatch.py's limit the day it gained real hardware's
// classes. ANY_SUB is the class's own name, used when no row names the
// subclass.
#define ANY_SUB -1
static const struct {
    uint8_t cls;
    int16_t sub;
    const char *name;
} classes[] = {
    { 0x00, ANY_SUB, "Unclassified device" },
    { 0x01, 0x00,    "SCSI storage controller" },
    { 0x01, 0x01,    "IDE interface" },
    { 0x01, 0x06,    "SATA controller" },
    { 0x01, 0x07,    "Serial Attached SCSI controller" },
    { 0x01, 0x08,    "Non-Volatile memory controller" },
    { 0x01, ANY_SUB, "Mass storage controller" },
    { 0x02, 0x00,    "Ethernet controller" },
    { 0x02, ANY_SUB, "Network controller" },
    { 0x03, 0x00,    "VGA compatible controller" },
    { 0x03, 0x02,    "3D controller" },
    { 0x03, ANY_SUB, "Display controller" },
    { 0x04, 0x01,    "Multimedia audio controller" },
    { 0x04, 0x03,    "Audio device" },
    { 0x04, ANY_SUB, "Multimedia controller" },
    { 0x05, ANY_SUB, "Memory controller" },
    { 0x06, 0x00,    "Host bridge" },
    { 0x06, 0x01,    "ISA bridge" },
    { 0x06, 0x04,    "PCI bridge" },
    { 0x06, ANY_SUB, "Bridge" },
    { 0x07, ANY_SUB, "Communication controller" },
    { 0x08, ANY_SUB, "Generic system peripheral" },
    { 0x09, ANY_SUB, "Input device controller" },
    { 0x0C, 0x03,    "USB controller" },
    { 0x0C, 0x05,    "SMBus" },
    { 0x0C, ANY_SUB, "Serial bus controller" },
    { 0x0D, ANY_SUB, "Wireless controller" },
    { 0x11, ANY_SUB, "Signal processing controller" },
};

const char *pci_class_name(uint8_t class_code, uint8_t subclass) {
    const char *class_only = "Unknown device";
    for (unsigned i = 0; i < sizeof classes / sizeof classes[0]; i++) {
        if (classes[i].cls != class_code) continue;
        if (classes[i].sub == subclass) return classes[i].name;
        if (classes[i].sub == ANY_SUB) class_only = classes[i].name;
    }
    return class_only;
}
