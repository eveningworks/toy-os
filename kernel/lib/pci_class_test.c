// pci_class_name(): the named pairs, a subclass the table does not know
// falling back to its class, and a class it does not know at all.
#include "ktest.h"
#include "pci.h"
#include "string.h"

KTEST("pci", "a class code is named, a subclass it lacks falls back to the class") {
    KTEST_ASSERT(k_strcmp(pci_class_name(0x01, 0x06), "SATA controller") == 0);
    KTEST_ASSERT(k_strcmp(pci_class_name(0x01, 0x08), "Non-Volatile memory controller") == 0);
    KTEST_ASSERT(k_strcmp(pci_class_name(0x01, 0x7f), "Mass storage controller") == 0);
    KTEST_ASSERT(k_strcmp(pci_class_name(0x04, 0x03), "Audio device") == 0);
    KTEST_ASSERT(k_strcmp(pci_class_name(0x0C, 0x03), "USB controller") == 0);
    KTEST_ASSERT(k_strcmp(pci_class_name(0x11, 0x80), "Signal processing controller") == 0);
    KTEST_ASSERT(k_strcmp(pci_class_name(0xFE, 0x00), "Unknown device") == 0);
}
