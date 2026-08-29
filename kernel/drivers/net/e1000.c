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

#define E1000_VENDOR 0x8086
#define E1000_DEV_82540EM 0x100E

// Register offsets (8254x manual section 13).
#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_EERD   0x0014
#define REG_ICR    0x00C0
#define REG_IMS    0x00D0
#define REG_IMC    0x00D8
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_TIPG   0x0410
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_MTA    0x5200
#define REG_RAL    0x5400
#define REG_RAH    0x5404

#define CTRL_SLU   (1u << 6)   // set link up
#define CTRL_ASDE  (1u << 5)   // auto-speed detection

#define RCTL_EN    (1u << 1)
#define RCTL_BAM   (1u << 15)  // accept broadcast
#define RCTL_SECRC (1u << 26)  // strip the Ethernet CRC
// BSIZE 00 with no BSEX is 2048 bytes, which is why neither appears here.

#define TCTL_EN    (1u << 1)
#define TCTL_PSP   (1u << 3)   // pad short packets

#define ICR_RXT0   (1u << 7)   // receive timer -- "there are packets"
#define ICR_RXDMT0 (1u << 4)   // ring is running low
#define ICR_RXO    (1u << 6)   // overrun

#define RAH_AV     (1u << 31)  // the address register holds a valid address

#define RX_DESCS   32
#define TX_DESCS   16
#define BUF_SIZE   2048

#define RX_STATUS_DD  (1u << 0)   // descriptor done
#define RX_STATUS_EOP (1u << 1)

#define TX_CMD_EOP  (1u << 0)
#define TX_CMD_IFCS (1u << 1)
#define TX_CMD_RS   (1u << 3)
#define TX_STATUS_DD (1u << 0)

struct rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

struct tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

_Static_assert(sizeof(struct rx_desc) == 16, "rx descriptor is 16 bytes");
_Static_assert(sizeof(struct tx_desc) == 16, "tx descriptor is 16 bytes");

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

    // The ring is full when the descriptor we are about to reuse has
    // not been written back. Report it rather than overwriting a frame
    // the device may still be reading.
    if (d->cmd && !(d->status & TX_STATUS_DD)) return -ENOSPC;

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

void e1000_init(void) {
    const struct pci_device *pci = 0;
    for (int i = 0; i < pci_device_count(); i++) {
        const struct pci_device *d = pci_device_at(i);
        if (d && d->vendor_id == E1000_VENDOR && d->device_id == E1000_DEV_82540EM) {
            pci = d;
            break;
        }
    }
    if (!pci) return;   // the ordinary case on a machine without one

    uint64_t bar = pci_bar_mem_addr(pci, 0);
    if (!bar || pci_bar_is_io(pci->bar[0])) {
        klog_write("e1000: BAR0 is not usable memory space\n");
        return;
    }
    g_e1000.mmio = (volatile uint8_t *)(uintptr_t)bar;   // identity-mapped below 4 GiB

    // Descriptors and buffers in one contiguous block each: the device
    // DMAs to physical addresses and the kernel reaches the same memory
    // through the identity map.
    uint64_t rx_ring = pmm_alloc_contiguous(1);
    uint64_t tx_ring = pmm_alloc_contiguous(1);
    uint64_t rx_bufs = pmm_alloc_contiguous((RX_DESCS * BUF_SIZE) / 4096);
    uint64_t tx_bufs = pmm_alloc_contiguous((TX_DESCS * BUF_SIZE) / 4096);
    if (!rx_ring || !tx_ring || !rx_bufs || !tx_bufs) {
        klog_write("e1000: not enough contiguous memory for the rings\n");
        return;
    }
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

    if (!read_mac(g_dev.mac)) {
        klog_write("e1000: could not read the MAC address\n");
        return;
    }

    g_dev.driver = "e1000";
    g_dev.transmit = e1000_transmit;
    g_dev.drv = &g_e1000;

    // The handler must be able to see a fully built device before the
    // first interrupt can arrive, so everything above happens first and
    // `g_present` is the commit point -- the ordering virtio_input.c
    // documents for the same reason.
    g_present = 1;

    uint8_t line = pci->interrupt_line;
    if (line != 0xFF && line < 16) {
        g_e1000.irq = line;
        irq_register_handler(line, e1000_irq);
        pic_clear_mask(line);
        reg_write(REG_IMS, ICR_RXT0 | ICR_RXDMT0 | ICR_RXO);
    } else {
        g_dev.poll = e1000_poll;
        klog_write("e1000: no usable interrupt line -- receiving by poll\n");
    }

    if (!net_register(&g_dev)) g_present = 0;
}
