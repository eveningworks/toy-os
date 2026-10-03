// Realtek RTL8111/8168 PCI Express Gigabit Ethernet -- the NIC built
// into most x86 laptops and boards of the last fifteen years, and the
// one on the bare-metal test laptop's PCI bus.
//
// THE DESCRIPTOR ENGINE IS "C+ MODE": 16-byte descriptors in a ring the
// device owns a slot of at a time, an OWN bit handing each slot back and
// forth, and a doorbell (TPPOLL) rather than a tail pointer -- so unlike
// e1000.c there is no ring index in a register, and EOR on the last
// descriptor is what makes the ring a ring. r8169.h holds the two field
// rules and their traps.
//
// ADAPTED FROM FreeBSD's re(4) -- sys/dev/re/if_re.c and
// sys/dev/rl/if_rlreg.h, BSD-2-clause, Bill Paul. The register map and
// the ORDER of the bring-up are its; the notice is in LICENSE. Linux's
// r8169.c describes the same hardware and is GPL-2.0; it was
// deliberately not consulted, for the reason net_usb_r8153.c gives.
#include "netdev.h"
#include "r8169.h"
#include "pci.h"
#include "pci_internal.h"
#include "pci_driver.h"
#include "pmm.h"
#include "irq.h"
#include "pic.h"
#include "klog.h"
#include "kfmt.h"   // klog_printf
#include "string.h"
#include "errno.h"
#include "barrier.h"
#include "irqflags.h" // irq_save() -- the ring timer must not race the handler
#include "multiboot.h" // multiboot_cmdline() -- the `nor8169` flag
#include "driver.h" // driver_bound() -- `lsdrv`

DRIVER_DECLARE("r8169", "net", "Realtek RTL8169/8168 PCIe Ethernet");

#include "r8169_regs.h"

// Clause-22 PHY registers, reached through PHYAR. The MAC only ever
// talks to its own internal PHY, so no PHY address appears anywhere.
#define PHY_BMCR       0x00
#define PHY_ADVERTISE  0x04
#define PHY_CTRL1000   0x09
#define BMCR_ANRESTART 0x0200
#define BMCR_ISOLATE   0x0400
#define BMCR_PDOWN     0x0800
#define BMCR_ANENABLE  0x1000
#define ADVERTISE_ALL  0x01E1  // 10/100, half and full, CSMA selector
#define CTRL1000_FDX   0x0200

#define RX_DESCS  32
#define TX_DESCS  16
#define BUF_SIZE  R8169_RX_BUF
#define RX_MAX    (NET_FRAME_MAX + 4)   // + a VLAN tag the chip may keep

// Long enough for an MDIO cycle (tens of microseconds) and a chip
// reset (a few hundred), a COUNT rather than a deadline because probe
// runs before anything guarantees a deadline-capable clocksource --
// see docs/conventions/kernel.md on which bound belongs where.
#define POLL_LIMIT 200000

struct r8169 {
    volatile uint8_t *mmio;
    struct rl_desc *rx, *tx;
    uint8_t *rx_buf, *tx_buf;
    uint64_t rx_buf_phys, tx_buf_phys;
    uint32_t rx_cur, tx_cur;
    uint8_t irq;
};

static struct r8169 g_r;
static struct net_device g_dev;
static int g_present;
static int g_probed;

static inline uint8_t  reg_read8 (uint32_t o) { return *(volatile uint8_t  *)(g_r.mmio + o); }
static inline uint16_t reg_read16(uint32_t o) { return *(volatile uint16_t *)(g_r.mmio + o); }
static inline uint32_t reg_read32(uint32_t o) { return *(volatile uint32_t *)(g_r.mmio + o); }
static inline void reg_write8 (uint32_t o, uint8_t v)  { *(volatile uint8_t  *)(g_r.mmio + o) = v; }
static inline void reg_write16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(g_r.mmio + o) = v; }
static inline void reg_write32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(g_r.mmio + o) = v; }

static void reg_write64(uint32_t o, uint64_t v) {
    reg_write32(o, (uint32_t)v);
    reg_write32(o + 4, (uint32_t)(v >> 32));
}

// --- the descriptor rules, the half a KTEST can reach ------------------


// r8169_tx_opts1() and r8169_rx_frame_len() are inline in r8169.h.

// --- the PHY -----------------------------------------------------------

static void mdio_write(uint8_t reg, uint16_t val) {
    reg_write32(REG_PHYAR, 0x80000000u | ((uint32_t)(reg & 0x1F) << 16) | val);
    for (int i = 0; i < POLL_LIMIT; i++) {
        if (!(reg_read32(REG_PHYAR) & 0x80000000u)) return;
        cpu_relax();
    }
}

static int mdio_read(uint8_t reg, uint16_t *out) {
    reg_write32(REG_PHYAR, (uint32_t)(reg & 0x1F) << 16);
    for (int i = 0; i < POLL_LIMIT; i++) {
        uint32_t v = reg_read32(REG_PHYAR);
        if (v & 0x80000000u) { *out = (uint16_t)v; return 1; }
        cpu_relax();
    }
    return 0;
}

// FIRMWARE MAY LEAVE THE PHY POWERED DOWN -- a board with PXE and
// wake-on-LAN off has no reason to have brought it up, and the symptom
// is a link that never arrives with nothing to retry. So the state is
// read first and this runs only when there is no link: it costs a boot
// nothing on a machine whose firmware did the work, and is the whole
// difference between working and dead on one that did not.
static void phy_kick(void) {
    uint16_t bmcr;
    if (!mdio_read(PHY_BMCR, &bmcr)) {
        klog_write("r8169: the PHY does not answer -- link state is the chip's\n");
        return;
    }
    bmcr &= (uint16_t)~(BMCR_PDOWN | BMCR_ISOLATE);
    mdio_write(PHY_BMCR, bmcr);
    mdio_write(PHY_ADVERTISE, ADVERTISE_ALL);
    mdio_write(PHY_CTRL1000, CTRL1000_FDX);
    mdio_write(PHY_BMCR, (uint16_t)(bmcr | BMCR_ANENABLE | BMCR_ANRESTART));
    klog_write("r8169: no link at probe -- PHY powered up, autonegotiation restarted\n");
}

static void update_link(struct net_device *dev) {
    uint8_t st = reg_read8(REG_PHYSTATUS);
    uint8_t up = (st & PHYSTATUS_LINK) ? 1 : 0;
    uint32_t bps = !up ? 0
                 : (st & PHYSTATUS_1000MBPS) ? 1000000000u
                 : (st & PHYSTATUS_100MBPS)  ? 100000000u
                 : (st & PHYSTATUS_10MBPS)   ? 10000000u : 0;

    if (!dev->link_known || dev->link_up != up || dev->link_bps != bps)
        klog_printf("r8169: %s link %s%s\n", dev->name, up ? "UP" : "down",
                    bps == 1000000000u ? " 1000M" :
                    bps == 100000000u  ? " 100M"  :
                    bps == 10000000u   ? " 10M"   : "");
    dev->link_up = up;
    dev->link_bps = bps;
    dev->link_known = 1;
}

// --- the rings ---------------------------------------------------------

static void rx_hand_back(uint32_t i) {
    struct rl_desc *d = &g_r.rx[i];
    d->opts2 = 0;
    d->addr = g_r.rx_buf_phys + (uint64_t)i * BUF_SIZE;
    kbarrier();   // the address must be visible before the device owns it
    d->opts1 = R8169_DESC_OWN | (i == RX_DESCS - 1 ? R8169_DESC_EOR : 0) | BUF_SIZE;
}

static void drain_rx(struct net_device *dev) {
    for (;;) {
        struct rl_desc *d = &g_r.rx[g_r.rx_cur];
        uint32_t opts1 = d->opts1;
        if (opts1 & R8169_DESC_OWN) break;

        uint32_t len = r8169_rx_frame_len(opts1);
        if (len) net_rx(dev, g_r.rx_buf + (uint64_t)g_r.rx_cur * BUF_SIZE, len);
        else     dev->rx_dropped++;

        rx_hand_back(g_r.rx_cur);
        g_r.rx_cur = (g_r.rx_cur + 1) % RX_DESCS;
    }
}

static uint16_t g_imr;   // the mask the handler puts back

// THE MASK IS DROPPED WHILE THE HANDLER RUNS, re(4)'s re_intr_msi(): an
// MSI is raised on a status bit going 0 -> 1, so an event landing while
// an earlier one is still set raises nothing, and the device can sit
// with work pending and no interrupt coming. Re-enabling the mask after
// the drain makes a still-set bit raise a fresh one.
static void r8169_irq(uint64_t *regs) {
    (void)regs;
    if (!g_present) return;
    uint16_t status = reg_read16(REG_ISR);
    if (!status || status == 0xFFFF) return;   // a shared INTx line, or a gone device
    reg_write16(REG_IMR, 0);
    reg_write16(REG_ISR, status);      // write-one-to-clear

    if (status & (ISR_ROK | ISR_RER | ISR_RDU | ISR_FOVW)) drain_rx(&g_dev);
    if (status & ISR_LINKCHG) update_link(&g_dev);
    if (status & ISR_SYSERR) klog_write(KLOG_ERR "r8169: the chip reports a system error\n");
    reg_write16(REG_IMR, g_imr);
}

static int r8169_transmit(struct net_device *dev, const void *frame, uint32_t len) {
    (void)dev;
    struct rl_desc *d = &g_r.tx[g_r.tx_cur];
    if (d->opts1 & R8169_DESC_OWN) return -ENOSPC;   // the device has not sent it yet

    uint32_t padded = r8169_tx_pad(len);
    uint8_t *buf = g_r.tx_buf + (uint64_t)g_r.tx_cur * BUF_SIZE;
    k_memcpy(buf, frame, len);
    if (padded > len) k_memset(buf + len, 0, padded - len);

    d->opts2 = 0;
    d->addr = g_r.tx_buf_phys + (uint64_t)g_r.tx_cur * BUF_SIZE;
    kbarrier();
    d->opts1 = r8169_tx_opts1(padded, g_r.tx_cur == TX_DESCS - 1);
    kbarrier();

    g_r.tx_cur = (g_r.tx_cur + 1) % TX_DESCS;
    reg_write8(REG_TPPOLL, TPPOLL_NPQ);
    return 0;
}

// THE INTERRUPT CAN COME BEFORE THE FRAME. An RTL8168G raises its receive
// status a moment before it writes the descriptor back, so the handler
// can drain an empty ring and return -- and that frame then waits for
// the NEXT interrupt. With TFTP the next one only came after the server
// timed out, 2 s later, several times a transfer. (Linux's NAPI reads
// the ring later, from a softirq.) A 10 ms sweep finishes the job, with
// interrupts off so it cannot meet the handler inside drain_rx(); with
// no interrupt at all it is the whole receive path.
static void r8169_rx_sweep(struct net_device *dev) {
    uint64_t f = irq_save();
    if (g_present && !(g_r.rx[g_r.rx_cur].opts1 & R8169_DESC_OWN)) drain_rx(dev);
    irq_restore(f);
}

// --- bring-up ----------------------------------------------------------

static int chip_reset(void) {
    reg_write8(REG_CR, CR_RESET);
    for (int i = 0; i < POLL_LIMIT; i++) {
        if (!(reg_read8(REG_CR) & CR_RESET)) return 1;
        cpu_relax();
    }
    return 0;
}

static int alloc_rings(void) {
    uint64_t rx_ring = pmm_alloc_contiguous(1, PMM_ZONE_DMA32);
    uint64_t tx_ring = pmm_alloc_contiguous(1, PMM_ZONE_DMA32);
    uint64_t rx_bufs = pmm_alloc_contiguous((RX_DESCS * BUF_SIZE) / 4096, PMM_ZONE_DMA32);
    uint64_t tx_bufs = pmm_alloc_contiguous((TX_DESCS * BUF_SIZE) / 4096, PMM_ZONE_DMA32);
    if (!rx_ring || !tx_ring || !rx_bufs || !tx_bufs) return 0;

    // A page is 256-byte aligned, which is what both ring registers
    // require of their base address.
    g_r.rx = (struct rl_desc *)(uintptr_t)rx_ring;
    g_r.tx = (struct rl_desc *)(uintptr_t)tx_ring;
    g_r.rx_buf = (uint8_t *)(uintptr_t)rx_bufs;
    g_r.tx_buf = (uint8_t *)(uintptr_t)tx_bufs;
    g_r.rx_buf_phys = rx_bufs;
    g_r.tx_buf_phys = tx_bufs;
    k_memset(g_r.rx, 0, 4096);
    k_memset(g_r.tx, 0, 4096);
    for (uint32_t i = 0; i < RX_DESCS; i++) rx_hand_back(i);
    // EOR on the last transmit slot before the device is running: it
    // may prefetch past a descriptor it does not own, and nothing else
    // would tell it where the ring wraps.
    g_r.tx[TX_DESCS - 1].opts1 = R8169_DESC_EOR;
    return 1;
}

// The devices this claims. Both are 8168-generation PCIe parts with the
// same registers and the same bring-up; only 0x8168 has been on a real
// machine here. The older PCI RTL8169 (0x8169) and the 2.5G RTL8125
// (0x8125) are deliberately NOT listed -- each differs in the parts
// this file would have to get right, and neither can be tested here.
static const struct pci_match r8169_matches[] = {
    PCI_MATCH_ID(REALTEK_VENDOR, 0x8168),   // RTL8111/8168/8211/8411
    PCI_MATCH_ID(REALTEK_VENDOR, 0x8136),   // RTL8101/8102/8106, 10/100
};

// `nor8169` on the boot line leaves the card alone, matched as a whole
// word -- the same shape as `nousb` and `noahci`, and here for the same
// reason: this is the first driver in the tree whose hardware nothing
// emulates, so a machine it hangs has no way to reach a prompt and say
// so. The rescue entry in grub.cfg is the other half of that.
static int r8169_disabled(void) {
    const char *cmdline = multiboot_cmdline();
    if (!cmdline) return 0;
    for (const char *p = cmdline; (p = k_strstr(p, "nor8169")) != 0; p += 7) {
        if (p != cmdline && p[-1] != ' ') continue;
        char after = p[7];
        if (after == 0 || after == ' ') return 1;
    }
    return 0;
}

static void r8169_probe(const struct pci_device *pci) {
    if (r8169_disabled()) {
        klog_write("r8169: disabled by `nor8169` on the boot line\n");
        return;
    }
    if (g_probed) {
        klog_printf("r8169: a second card at %02x:%02x.%u -- one is driven\n",
                    pci->bus, pci->device, pci->function);
        return;
    }
    g_probed = 1;

    // The registers are in the first MEMORY BAR, which is BAR2 on every
    // part this matches: BAR0 is the I/O alias of the same window.
    uint64_t bar = pci_bar_mem_addr(pci, 2);
    if (!bar) {
        klog_write("r8169: BAR2 is not usable memory space\n");
        return;
    }
    g_r.mmio = (volatile uint8_t *)(uintptr_t)bar;   // identity-mapped below 4 GiB

    if (!alloc_rings()) {
        klog_write("r8169: not enough contiguous memory for the rings\n");
        return;
    }

    pci_command_update(pci, PCI_CMD_MEMORY | PCI_CMD_BUS_MASTER, 0);

    reg_write16(REG_IMR, 0);
    if (!chip_reset()) {
        klog_write("r8169: the chip never came out of reset\n");
        return;
    }

    uint32_t xid = reg_read32(REG_TCR) & TCR_HWREV;
    for (int i = 0; i < NET_MAC_LEN; i++) g_dev.mac[i] = reg_read8(REG_IDR0 + i);

    reg_write8(REG_9346CR, CFG_UNLOCK);

    // Neither checksum offload nor VLAN stripping is implemented above,
    // so both are turned OFF rather than left as the firmware had them.
    uint16_t cpcr = reg_read16(REG_CPCR);
    cpcr |= CPCR_PCI_MRW;
    cpcr &= (uint16_t)~(CPCR_RXCSUM | CPCR_VLANSTRIP);
    reg_write16(REG_CPCR, cpcr);

    reg_write16(REG_RMS, RX_MAX);   // longer than this is dropped, not split
    reg_write8(REG_MTPS, 0x3F);

    reg_write64(REG_RDSAR, (uint64_t)(uintptr_t)g_r.rx);
    reg_write64(REG_TNPDS, (uint64_t)(uintptr_t)g_r.tx);
    reg_write8(REG_9346CR, CFG_LOCK);

    reg_write32(REG_TCR, TCR_IFG_STD | TCR_DMA_UNLIM);

    // THE ORDER DEPENDS ON THE GENERATION, as in re(4)'s re_init_locked().
    // Older revisions latch RCR only while the receive engine runs, so
    // they are enabled first and configured after. The 8168G family is
    // configured FIRST and enabled after, with its RXDV gate opened and
    // EARLYOFF_V2 set. Done the old way, an RTL8168G received exactly one
    // lap of the ring (32 frames) and then nothing, every bring-up, while
    // the 8168GU worked. Its gate read open (MISC 0x3f), so the order,
    // EARLYOFF_V2 or the handler's IMR re-arm cured it -- not isolated.
    uint32_t fam = xid & TCR_FAMILY;
    int g_family = fam == HWREV_8168G || fam == HWREV_8168GU ||
                   fam == HWREV_8168H || fam == HWREV_8411B;
    uint32_t rcr = RCR_FIFO_NONE | RCR_DMA_UNLIM | RCR_BROAD | RCR_MULTI | RCR_INDIV;
    if (g_family) {
        reg_write32(REG_MISC, reg_read32(REG_MISC) & ~MISC_RXDV_GATED);
        reg_write32(REG_RCR, rcr | RCR_EARLYOFF_V2);
        reg_write32(REG_MAR0, 0xFFFFFFFFu);
        reg_write32(REG_MAR0 + 4, 0xFFFFFFFFu);
        reg_write8(REG_CR, CR_RX_ENB | CR_TX_ENB);
    } else {
        reg_write8(REG_CR, CR_RX_ENB | CR_TX_ENB);
        reg_write32(REG_RCR, rcr);
        reg_write32(REG_MAR0, 0xFFFFFFFFu);
        reg_write32(REG_MAR0 + 4, 0xFFFFFFFFu);
    }

    net_location_pci(&g_dev, pci->bus, pci->device, pci->function);
    g_dev.driver = "r8169";
    g_dev.transmit = r8169_transmit;
    g_dev.drv = &g_r;

    // Everything the handler reads is built before the commit point, so
    // the first interrupt cannot land on a half-set-up device.
    g_present = 1;

    reg_write16(REG_ISR, 0xFFFF);
    // NO TRANSMIT INTERRUPT: a completion is discovered by the OWN bit
    // when the slot comes round again, so asking for one per frame
    // would be an interrupt whose handler has nothing to do.
    uint16_t mask = ISR_ROK | ISR_RER | ISR_RDU |
                    ISR_LINKCHG | ISR_FOVW | ISR_SYSERR;
    g_imr = mask;

    uint8_t line = pci_irq_line(pci);
    uint8_t vector = pci_msi_request(pci, r8169_irq);
    if (vector) {
        reg_write16(REG_IMR, mask);
        klog_printf("r8169: xid %x, on %s vector %u\n", xid,
                    pci->irq_msix ? "MSI-X" : "MSI", vector);
    } else if (line != IRQ_NONE) {
        g_r.irq = line;
        irq_register_handler(line, r8169_irq);
        irq_unmask(line);
        reg_write16(REG_IMR, mask);
        klog_printf("r8169: xid %x, on IRQ %u\n", xid, line);
    } else {
        klog_printf("r8169: xid %x, no usable interrupt -- receiving by poll\n", xid);
    }

    g_dev.poll = r8169_rx_sweep;
    g_dev.poll_ms = 10;
    if (!net_register(&g_dev)) { g_present = 0; return; }

    update_link(&g_dev);
    if (!g_dev.link_up) phy_kick();
}

// The inverse, in the order that keeps the handler safe: the chip's
// interrupt mask off first, then the handler unhooked, then the rings
// stopped by a reset, the device out of the stack, the frames back.
// Statics are reset so a re-probe starts from nothing -- what lets the
// module be unloaded and loaded again on the machine.
static void r8169_remove(const struct pci_device *pci) {
    if (!g_probed) return;
    if (g_r.mmio) {
        reg_write16(REG_IMR, 0);
        reg_write16(REG_ISR, 0xFFFF);
    }
    g_present = 0;
    if (pci->irq_vector) pci_msi_release(pci, pci->irq_vector);
    else if (g_r.irq) irq_unregister_handler(g_r.irq, r8169_irq);
    if (g_r.mmio) chip_reset();
    net_unregister(&g_dev);
    if (g_r.rx) pmm_free_contiguous((uint64_t)(uintptr_t)g_r.rx, 1);
    if (g_r.tx) pmm_free_contiguous((uint64_t)(uintptr_t)g_r.tx, 1);
    if (g_r.rx_buf) pmm_free_contiguous(g_r.rx_buf_phys, (RX_DESCS * BUF_SIZE) / 4096);
    if (g_r.tx_buf) pmm_free_contiguous(g_r.tx_buf_phys, (TX_DESCS * BUF_SIZE) / 4096);
    k_memset(&g_r, 0, sizeof g_r);
    k_memset(&g_dev, 0, sizeof g_dev);
    g_probed = 0;
}
PCI_DRIVER_REMOVABLE("r8169", r8169_matches, r8169_probe, r8169_remove);
