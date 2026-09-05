// MSI: asking a PCI device to signal interrupts by WRITING MEMORY
// instead of pulling a shared wire.
//
// WHAT IT REPLACES. An INTx line is level-triggered and shared: every
// handler on it runs and each asks its own device "was that you?", a
// device left asserting with nobody reading its status holds the line
// down forever, and there are sixteen lines for the whole machine. This
// kernel has already paid for all three (see irq.c's comment, and
// virtio_input's boot hang). An MSI is a posted write to 0xFEE00000
// carrying a vector number: it is edge-triggered by construction,
// belongs to exactly one device, and there is no line to share.
//
// WHAT THIS IS NOT. Not MSI-X, which is the same idea with a table in
// a BAR and one vector per queue -- worth having when a device has
// several queues to separate, which is the NVMe and multi-queue virtio
// case rather than today's. The capability id is already named
// (PCI_CAP_ID_MSIX) for whoever gets there.
//
// **THE HALF THAT IS NOT OPTIONAL: INTx MUST BE DISABLED.** A device
// with MSI enabled is forbidden by the spec from using its pin, but the
// bit that stops it is the command register's, not the capability's --
// and a device left free to assert a line whose handler has moved to a
// vector is the storm irq.c warns about, arriving on a line nobody is
// listening to.
#include "pci.h"
#include "pci_internal.h"
#include "lapic.h"
#include "klog.h"
#include "kfmt.h"

// driver-none: MSI/MSI-X setup, used by drivers

// Message Control, at capability offset 2.
#define MSI_CTL              0x02
#define MSI_CTL_ENABLE       (1u << 0)
#define MSI_CTL_MULTI_CAP(c) (((c) >> 1) & 0x7)   // log2 of vectors offered
#define MSI_CTL_MULTI_EN(n)  (((n) & 0x7) << 4)
#define MSI_CTL_64BIT        (1u << 7)

// The address register is always at +4. Where the DATA register sits
// depends on whether the device does 64-bit addressing, which is the
// one layout question in the whole capability.
#define MSI_ADDR_LO          0x04
#define MSI_ADDR_HI          0x08
#define MSI_DATA_32          0x08
#define MSI_DATA_64          0x0C

// The x86 MSI address. Bits 19:12 are the destination LAPIC id; the
// low bits select redirection and logical/physical mode, and 0 for both
// means "physical, to exactly this id", which is what a single-CPU
// machine wants and what a multi-CPU one should still start with.
#define MSI_ADDR_BASE        0xFEE00000u
#define MSI_ADDR_DEST(id)    ((uint32_t)(id) << 12)

// The data register carries the vector in its low byte. Delivery mode
// 0 (fixed) and trigger mode 0 (edge) are the rest of it, and both are
// zero -- stated here because "the data is just the vector" is true
// only by that coincidence.
#define MSI_DATA_VECTOR(v)   ((uint32_t)(v) & 0xFF)

int pci_msi_enable(const struct pci_device *dev, uint8_t vector) {
    if (!dev || !vector) return 0;
    if (!lapic_present()) return 0;   // nothing would receive the write

    uint8_t cap = pci_capability_find(dev, PCI_CAP_ID_MSI, 0);
    if (!cap) return 0;

    uint16_t ctl = pci_config_read16(dev, (uint8_t)(cap + MSI_CTL));

    uint32_t addr = MSI_ADDR_BASE | MSI_ADDR_DEST(lapic_id());
    pci_config_write32(dev, (uint8_t)(cap + MSI_ADDR_LO), addr);
    if (ctl & MSI_CTL_64BIT) {
        // The high half is zero and must be WRITTEN zero: a device that
        // powered up with rubbish there would post to an address that
        // is not the LAPIC's at all.
        pci_config_write32(dev, (uint8_t)(cap + MSI_ADDR_HI), 0);
        pci_config_write32(dev, (uint8_t)(cap + MSI_DATA_64),
                           MSI_DATA_VECTOR(vector));
    } else {
        pci_config_write32(dev, (uint8_t)(cap + MSI_DATA_32),
                           MSI_DATA_VECTOR(vector));
    }

    // ONE vector, whatever the device offered. Multiple Message Enable
    // is set to 0 (meaning 2^0) explicitly rather than left as found:
    // a device allocating several would derive them by replacing the
    // low bits of the vector, and this kernel has handed out exactly
    // one.
    ctl = (uint16_t)((ctl & ~MSI_CTL_MULTI_EN(0x7)) | MSI_CTL_ENABLE);
    pci_config_write16(dev, (uint8_t)(cap + MSI_CTL), ctl);

    // And the pin goes away -- see the header comment. Done AFTER the
    // capability is armed, so there is no window with neither path
    // able to deliver.
    pci_command_update(dev, PCI_CMD_INTX_DISABLE, 0);

    pci_note_vector(dev, vector, 0);
    klog_printf("msi: %02x:%02x.%u -> vector %u (%s addressing, cap at 0x%x)\n",
                dev->bus, dev->device, dev->function, vector,
                (ctl & MSI_CTL_64BIT) ? "64-bit" : "32-bit", cap);
    return 1;
}

// --- MSI-X ------------------------------------------------------------
//
// The same idea with the message data moved OUT of config space and
// into a table in one of the device's BARs: up to 2048 entries, each
// with its own address and data, so a device with queues can give each
// one its own vector and steer them at different CPUs. That is why
// everything modern uses MSI-X rather than MSI -- and why QEMU's
// `qemu-xhci` offers MSI-X by default and MSI not at all.
//
// ONE ENTRY IS PROGRAMMED HERE, entry 0, because this kernel hands out
// one vector per device. The rest of the table is left masked, which is
// the state it powers up in.

#define MSIX_CTL             0x02
#define MSIX_CTL_TABLE_SIZE(c) (((c) & 0x7FF) + 1)   // stored as N-1
#define MSIX_CTL_FUNC_MASK   (1u << 14)
#define MSIX_CTL_ENABLE      (1u << 15)
#define MSIX_TABLE_OFFSET    0x04
#define MSIX_BIR_MASK        0x7                     // which BAR holds it

// One table entry, 16 bytes.
#define MSIX_ENTRY_ADDR_LO   0x0
#define MSIX_ENTRY_ADDR_HI   0x4
#define MSIX_ENTRY_DATA      0x8
#define MSIX_ENTRY_VECCTL    0xC
#define MSIX_VECCTL_MASK     (1u << 0)

int pci_msix_enable(const struct pci_device *dev, uint8_t vector) {
    if (!dev || !vector || !lapic_present()) return 0;

    uint8_t cap = pci_capability_find(dev, PCI_CAP_ID_MSIX, 0);
    if (!cap) return 0;

    uint16_t ctl = pci_config_read16(dev, (uint8_t)(cap + MSIX_CTL));
    uint32_t tbl = pci_config_read32(dev, (uint8_t)(cap + MSIX_TABLE_OFFSET));
    uint8_t  bir = (uint8_t)(tbl & MSIX_BIR_MASK);
    uint32_t off = tbl & ~(uint32_t)MSIX_BIR_MASK;

    uint64_t bar = pci_bar_mem_addr(dev, bir);
    uint64_t bar_len = pci_bar_mem_size(dev, bir);   // 0 if the probe could not size it
    if (!bar || bar + off + 16 > 0x100000000ull || (bar_len && off + 16 > bar_len)) {
        klog_printf("msix: %02x:%02x.%u table in BAR%u is unreachable\n",
                    dev->bus, dev->device, dev->function, bir);
        return 0;
    }
    // Identity-mapped, like every other MMIO window this kernel touches
    // (the xHCI's own registers are reached the same way).
    volatile uint32_t *entry = (volatile uint32_t *)(uintptr_t)(bar + off);

    // MASKED WHILE WRITING, unmasked at the end. A half-written entry
    // that the device sampled would post to a spliced address, and the
    // per-entry mask bit exists precisely so this window is closable --
    // MSI, with no such bit, has to rely on its enable bit instead.
    entry[MSIX_ENTRY_VECCTL / 4] |= MSIX_VECCTL_MASK;
    entry[MSIX_ENTRY_ADDR_LO / 4] = MSI_ADDR_BASE | MSI_ADDR_DEST(lapic_id());
    entry[MSIX_ENTRY_ADDR_HI / 4] = 0;
    entry[MSIX_ENTRY_DATA / 4] = MSI_DATA_VECTOR(vector);
    entry[MSIX_ENTRY_VECCTL / 4] &= ~(uint32_t)MSIX_VECCTL_MASK;

    // Enabled, and the function-wide mask cleared with it -- a device
    // that powered up with Function Mask set delivers nothing however
    // its entries are programmed.
    ctl = (uint16_t)((ctl | MSIX_CTL_ENABLE) & ~MSIX_CTL_FUNC_MASK);
    pci_config_write16(dev, (uint8_t)(cap + MSIX_CTL), ctl);
    pci_command_update(dev, PCI_CMD_INTX_DISABLE, 0);

    pci_note_vector(dev, vector, 1);
    klog_printf("msix: %02x:%02x.%u -> vector %u (entry 0 of %u, BAR%u+0x%x)\n",
                dev->bus, dev->device, dev->function, vector,
                MSIX_CTL_TABLE_SIZE(ctl), bir, off);
    return 1;
}


// Puts a device back on its pin: MSI-X off, INTx re-enabled. For the
// one case that needs it -- a device that took the capability and then
// refused to be programmed further, leaving a caller about to fall back
// to a line the device would never assert, because MSI-X outranks INTx
// for as long as it is enabled.
void pci_msix_disable(const struct pci_device *dev) {
    if (!dev) return;
    uint8_t cap = pci_capability_find(dev, PCI_CAP_ID_MSIX, 0);
    if (!cap) return;
    uint16_t ctl = pci_config_read16(dev, (uint8_t)(cap + MSIX_CTL));
    pci_config_write16(dev, (uint8_t)(cap + MSIX_CTL),
                       (uint16_t)(ctl & ~MSIX_CTL_ENABLE));
    pci_command_update(dev, 0, PCI_CMD_INTX_DISABLE);
    pci_note_vector(dev, 0, 0);
}

// --- the ladder every driver climbs -----------------------------------
//
// MSI-X, then MSI, then 0 meaning "use your pin" -- the order and the
// give-up shape of Linux's pci_alloc_irq_vectors(dev, 1, 1,
// PCI_IRQ_MSIX | PCI_IRQ_MSI), and for its reason: MSI-X is what a PCIe
// device actually offers, and several devices here have one and not the
// other. The INTx half is deliberately NOT in here, because it is the
// part that genuinely differs -- AHCI stays polled, e1000 switches to a
// poll, AC'97 gives up, xHCI polls anyway.
//
// A vector is claimed BEFORE the device is programmed, so the handler is
// installed for a message that can arrive the instant the capability is
// armed; a device with neither capability hands it straight back.
uint8_t pci_msi_request(const struct pci_device *dev, void (*handler)(uint64_t *regs)) {
    if (!dev || !handler) return 0;

    uint8_t vector = lapic_alloc_vector(handler);
    if (!vector) return 0;
    if (pci_msix_enable(dev, vector) || pci_msi_enable(dev, vector)) return vector;

    lapic_free_vector(vector);
    return 0;
}
