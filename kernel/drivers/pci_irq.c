// Which line a PCI device's INTx pin arrives on -- see pci_internal.h.
// driver-none: routing, drives nothing itself
#include "pci_internal.h"
#include "acpi_prt.h"
#include "irq.h"
#include "klog.h"
#include "kfmt.h"

uint8_t pci_irq_line(const struct pci_device *dev) {
    if (!dev || !dev->interrupt_pin) return IRQ_NONE;

    // With the I/O APIC delivering, the firmware's routing table is the
    // truth: a GSI, and how the wire behaves. Refused tables (every
    // PIC-mode link, all of i440fx) fall through to the BIOS's line.
    if (irq_on_ioapic()) {
        uint32_t gsi; int level, low;
        int rc = acpi_prt_lookup(dev->bus, dev->device, (uint8_t)(dev->interrupt_pin - 1),
                                 &gsi, &level, &low);
        if (rc == 0 && gsi < IRQ_MAX) {
            if (gsi >= 16) irq_set_trigger((uint8_t)gsi, level, low);
            klog_printf("pci: %02x:%02x.%u INT%c -> GSI %u by _PRT\n", dev->bus, dev->device,
                        dev->function, 'A' + dev->interrupt_pin - 1, gsi);
            return (uint8_t)gsi;
        }
        if (rc == 0)
            klog_printf("pci: %02x:%02x.%u: _PRT names GSI %u, beyond the routed range\n",
                        dev->bus, dev->device, dev->function, gsi);
    }

    // The BIOS's routing for the 8259: an ISA-range line. On the I/O
    // APIC the same input number carries it on every PC chipset QEMU
    // models (the GSI handler fans a PIRQ out to both), which is what
    // makes this the fallback when `_PRT` has nothing decodable.
    uint8_t line = dev->interrupt_line;
    if (line == 0xFF || line == 0 || line >= 16) return IRQ_NONE;
    return line;
}
