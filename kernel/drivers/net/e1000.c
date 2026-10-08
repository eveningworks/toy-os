// Intel 82540EM (8086:100E) -- the NIC QEMU's default machine already
// has. Every toy-os boot before this driver existed had one sitting
// unclaimed on the PCI bus, which is why this is the first card: it
// needs no flag, so every existing test guest gets a network.
//
// LEGACY DESCRIPTORS ONLY. The 8254x family also has an extended
// (offload) descriptor format for checksum and TSO work; nothing here
// offloads anything, so the legacy layout is not a shortcut but the
// whole feature set being used. Linux's e1000 driver does the same for
// this chip.
//
// THE RINGS ARE FIXED AND PRE-FILLED. Every receive descriptor points
// at a 2 KiB buffer allocated once at init and never freed -- the
// device writes into it, net_rx() copies out of it, and the descriptor
// goes straight back to the tail. There is no per-packet allocation
// anywhere in the receive path, which is what lets the whole path run
// from an interrupt handler.
#include "netdev.h"
#include "net.h"
#include "pci.h"
#include "pci_internal.h"
#include "pmm.h"
#include "irq.h"
#include "pic.h"
#include "klog.h"
#include "kfmt.h"   // klog_printf
#include "string.h"
#include "errno.h"
#include "driver.h" // driver_bound() -- `lsdrv`
#include "pci_driver.h"

DRIVER_DECLARE("e1000", "net", "Intel 8254x gigabit Ethernet");

#include "e1000_regs.h"

#define RX_DESCS   64
#define TX_DESCS   16
#define BUF_SIZE   2048

struct e1000 {
    volatile uint8_t *mmio;
    struct rx_desc *rx;
    struct tx_desc *tx;
    uint8_t *rx_buf;       // RX_DESCS * BUF_SIZE, contiguous
    uint8_t *tx_buf;
    uint64_t rx_buf_phys, tx_buf_phys;
    uint32_t rx_cur;
    uint32_t tx_cur;
    uint8_t irq;
};

static struct e1000 g_e1000;
static struct net_device g_dev;
static int g_present;
static uint8_t g_msi_vector;   // LAPIC vector, 0 when on the INTx pin

static inline uint32_t reg_read(uint32_t off) {
    return *(volatile uint32_t *)(g_e1000.mmio + off);
}
static inline void reg_write(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(g_e1000.mmio + off) = v;
}

// The MAC comes from the receive-address registers, which the hardware
// loads from EEPROM at reset (and which QEMU fills from -netdev's
// configured address). EEPROM is read directly only when RAH says the
// register pair is not valid -- one path that works on both, with the
// fallback for the machine where it does not.
static int read_mac(uint8_t mac[NET_MAC_LEN]) {
    uint32_t low = reg_read(REG_RAL);
    uint32_t high = reg_read(REG_RAH);
    if (high & RAH_AV) {
        mac[0] = (uint8_t)(low);        mac[1] = (uint8_t)(low >> 8);
        mac[2] = (uint8_t)(low >> 16);  mac[3] = (uint8_t)(low >> 24);
        mac[4] = (uint8_t)(high);       mac[5] = (uint8_t)(high >> 8);
        return 1;
    }
    for (int word = 0; word < 3; word++) {
        reg_write(REG_EERD, ((uint32_t)word << 8) | 1u);
        uint32_t v = 0;
        for (int spin = 0; spin < 100000; spin++) {
            v = reg_read(REG_EERD);
            if (v & (1u << 4)) break;   // DONE
            __asm__ volatile("pause");
        }
        if (!(v & (1u << 4))) return 0;
        mac[word * 2]     = (uint8_t)(v >> 16);
        mac[word * 2 + 1] = (uint8_t)(v >> 24);
    }
    return 1;
}

static void drain_rx(struct net_device *dev) {
    struct e1000 *e = &g_e1000;
    for (;;) {
        struct rx_desc *d = &e->rx[e->rx_cur];
        if (!(d->status & RX_STATUS_DD)) break;

        // A frame spanning descriptors means the MTU and the buffer
        // size disagree; with 2 KiB buffers and a 1500-byte MTU that
        // cannot happen, so a non-EOP descriptor is a bug rather than a
        // case to reassemble -- it is dropped and counted.
        if ((d->status & RX_STATUS_EOP) && !d->errors)
            net_rx(dev, e->rx_buf + (uint64_t)e->rx_cur * BUF_SIZE, d->length);
        else
            dev->rx_dropped++;

        d->status = 0;
        uint32_t prev = e->rx_cur;
        e->rx_cur = (e->rx_cur + 1) % RX_DESCS;
        reg_write(REG_RDT, prev);   // hand this descriptor back to the device
    }
}

static void e1000_irq(uint64_t *regs) {
    (void)regs;
    if (!g_present) return;
    uint32_t cause = reg_read(REG_ICR);   // read-to-clear
    if (!cause) return;                   // a shared INTx line: not ours
    if (cause & (ICR_RXT0 | ICR_RXDMT0 | ICR_RXO)) drain_rx(&g_dev);
}

static int e1000_transmit(struct net_device *dev, const void *frame, uint32_t len) {
    (void)dev;
    struct e1000 *e = &g_e1000;
    struct tx_desc *d = &e->tx[e->tx_cur];

    // Full (e1000_tx_full(), which keeps one slot free): report it rather
    // than overwriting a frame the device may still be reading.
    if (e1000_tx_full(e->tx, TX_DESCS, e->tx_cur)) return -ENOSPC;

    k_memcpy(e->tx_buf + (uint64_t)e->tx_cur * BUF_SIZE, frame, len);
    d->addr = e->tx_buf_phys + (uint64_t)e->tx_cur * BUF_SIZE;
    d->length = (uint16_t)len;
    d->cso = 0;
    d->cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    d->status = 0;
    d->css = 0;
    d->special = 0;

    e->tx_cur = (e->tx_cur + 1) % TX_DESCS;
    reg_write(REG_TDT, e->tx_cur);
    return 0;
}

// No interrupt line means the frames still have to be collected, so
// the core polls. Registered as net_device.poll only in that case.
static void e1000_poll(struct net_device *dev) { drain_rx(dev); }

static int g_probed;
static const struct pci_match e1000_matches[] = { PCI_MATCH_ID(E1000_VENDOR, E1000_DEV_82540EM) };

// A decline AFTER an interrupt is registered must take it back first:
// this is a module, and an unbound device does not stop it unloading.
// The minimum gap between interrupts, in the 256 ns units ITR counts:
// 20000, 8000 and 4000 a second -- Linux e1000's InterruptThrottleRate
// default is the middle one.
static int e1000_set_link(struct net_device *dev, const struct net_link_values *v) {
    (void)dev;
    static const uint32_t itr[4] = { 0, 195, 488, 976 };
    reg_write(REG_ITR, itr[v->moderation <= NET_MOD_HIGH ? v->moderation : NET_MOD_OFF]);
    return 0;
}

static int e1000_probe(const struct pci_device *pci) {
    if (g_probed) return pci_probe_decline(pci, "a second card; one is driven");
    g_probed = 1;

    uint64_t bar = pci_bar_mem_addr(pci, 0);
    if (!bar || pci_bar_is_io(pci->bar[0]))
        return pci_probe_decline(pci, "BAR0 is not usable memory space");
    g_e1000.mmio = (volatile uint8_t *)(uintptr_t)bar;   // identity-mapped below 4 GiB

    // Descriptors and buffers in one contiguous block each: the device
    // DMAs to physical addresses and the kernel reaches the same memory
    // through the identity map.
    uint64_t rx_ring = pmm_alloc_contiguous(1, PMM_ZONE_DMA32);
    uint64_t tx_ring = pmm_alloc_contiguous(1, PMM_ZONE_DMA32);
    uint64_t rx_bufs = pmm_alloc_contiguous((RX_DESCS * BUF_SIZE) / 4096, PMM_ZONE_DMA32);
    uint64_t tx_bufs = pmm_alloc_contiguous((TX_DESCS * BUF_SIZE) / 4096, PMM_ZONE_DMA32);
    if (!rx_ring || !tx_ring || !rx_bufs || !tx_bufs)
        return pci_probe_decline(pci, "not enough contiguous memory for the rings");
    g_e1000.rx = (struct rx_desc *)(uintptr_t)rx_ring;
    g_e1000.tx = (struct tx_desc *)(uintptr_t)tx_ring;
    g_e1000.rx_buf = (uint8_t *)(uintptr_t)rx_bufs;
    g_e1000.tx_buf = (uint8_t *)(uintptr_t)tx_bufs;
    g_e1000.rx_buf_phys = rx_bufs;
    g_e1000.tx_buf_phys = tx_bufs;

    pci_enable_bus_master(pci);

    reg_write(REG_IMC, 0xFFFFFFFFu);   // mask everything before touching the rings
    reg_read(REG_ICR);
    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_SLU | CTRL_ASDE);
    for (int i = 0; i < 128; i++) reg_write(REG_MTA + i * 4, 0);

    k_memset(g_e1000.rx, 0, 4096);
    k_memset(g_e1000.tx, 0, 4096);
    for (int i = 0; i < RX_DESCS; i++) {
        g_e1000.rx[i].addr = rx_bufs + (uint64_t)i * BUF_SIZE;
        g_e1000.rx[i].status = 0;
    }
    reg_write(REG_RDBAL, (uint32_t)rx_ring);
    reg_write(REG_RDBAH, (uint32_t)(rx_ring >> 32));
    reg_write(REG_RDLEN, RX_DESCS * (uint32_t)sizeof(struct rx_desc));
    reg_write(REG_RDH, 0);
    reg_write(REG_RDT, RX_DESCS - 1);   // every descriptor but one is the device's
    reg_write(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);

    reg_write(REG_TDBAL, (uint32_t)tx_ring);
    reg_write(REG_TDBAH, (uint32_t)(tx_ring >> 32));
    reg_write(REG_TDLEN, TX_DESCS * (uint32_t)sizeof(struct tx_desc));
    reg_write(REG_TDH, 0);
    reg_write(REG_TDT, 0);
    reg_write(REG_TIPG, 0x0060200Au);   // the manual's IEEE 802.3 default
    reg_write(REG_TCTL, TCTL_EN | TCTL_PSP | (0x0Fu << 4) | (0x40u << 12));

    if (!read_mac(g_dev.mac)) return pci_probe_decline(pci, "could not read the MAC address");

    // Interrupt moderation is the one adapter setting this card offers
    // (SYS_NET_LINK). Off by default, as the chip resets -- the
    // behaviour this driver always had.
    reg_write(REG_ITR, 0);
    g_dev.link_caps = NET_LINK_MODERATION;
    g_dev.link.moderation = NET_MOD_OFF;
    g_dev.set_link = e1000_set_link;

    net_location_pci(&g_dev, pci->bus, pci->device, pci->function);
    g_dev.driver = "e1000";
    g_dev.transmit = e1000_transmit;
    g_dev.drv = &g_e1000;

    // The handler must be able to see a fully built device before the
    // first interrupt can arrive, so everything above happens first and
    // `g_present` is the commit point -- the ordering virtio_input.c
    // documents for the same reason.
    g_present = 1;

    // A vector if the card offers one, else the pin, else a poll.
    // QEMU's 82540EM advertises neither capability -- so does Linux's
    // `e1000` driver, which is INTx-only for the same parts; MSI-X
    // arrived with e1000e, which this driver does not match.
    uint8_t line = pci_irq_line(pci);
    g_msi_vector = pci_msi_request(pci, e1000_irq);
    if (g_msi_vector) {
        reg_write(REG_IMS, ICR_RXT0 | ICR_RXDMT0 | ICR_RXO);
        klog_printf("e1000: on %s vector %u\n",
                    pci->irq_msix ? "MSI-X" : "MSI", g_msi_vector);
    } else if (line != IRQ_NONE) {
        g_e1000.irq = line;
        irq_register_handler(line, e1000_irq);
        irq_unmask(line);
        reg_write(REG_IMS, ICR_RXT0 | ICR_RXDMT0 | ICR_RXO);
    } else {
        g_dev.poll = e1000_poll;
        g_dev.poll_ms = 10;
        klog_write("e1000: no usable interrupt line -- receiving by poll\n");
    }

    if (!net_register(&g_dev)) {
        g_present = 0;
        reg_write(REG_IMC, 0xFFFFFFFFu);
        if (g_msi_vector) pci_msi_release(pci, g_msi_vector);
        else if (g_e1000.irq) { irq_mask(g_e1000.irq); irq_unregister_handler(g_e1000.irq, e1000_irq); }
        g_msi_vector = 0;
        g_e1000.irq = 0;
        return pci_probe_decline(pci, "the network core has no room for another card");
    }
    return 0;
}

// The inverse: interrupts masked FIRST, so the handler can be
// unregistered with nothing in flight; then the rings stopped, the
// device taken out of the stack, and the frames given back. The
// static state is reset so a re-probe starts from nothing -- this is
// what lets the module be unloaded and loaded again.
static void e1000_remove(const struct pci_device *pci) {
    if (!g_probed) return;
    struct e1000 *e = &g_e1000;
    if (e->mmio) {
        reg_write(REG_IMC, 0xFFFFFFFFu);
        reg_read(REG_ICR);
        reg_write(REG_RCTL, 0);
        reg_write(REG_TCTL, 0);
    }
    g_present = 0;
    if (g_msi_vector) { pci_msi_release(pci, g_msi_vector); g_msi_vector = 0; }
    else if (e->irq) irq_unregister_handler(e->irq, e1000_irq);
    net_unregister(&g_dev);
    if (e->rx) pmm_free_contiguous((uint64_t)(uintptr_t)e->rx, 1);
    if (e->tx) pmm_free_contiguous((uint64_t)(uintptr_t)e->tx, 1);
    if (e->rx_buf) pmm_free_contiguous(e->rx_buf_phys, (RX_DESCS * BUF_SIZE) / 4096);
    if (e->tx_buf) pmm_free_contiguous(e->tx_buf_phys, (TX_DESCS * BUF_SIZE) / 4096);
    k_memset(e, 0, sizeof *e);
    k_memset(&g_dev, 0, sizeof g_dev);
    g_probed = 0;
}
PCI_DRIVER_REMOVABLE("e1000", e1000_matches, e1000_probe, e1000_remove);
