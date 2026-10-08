// A Realtek RTL8111/8168 OWNED BY THE KERNEL DEBUGGER -- the polled copy
// of r8169.c that kdebug_nic.h describes, and the Lenovo's way in: the
// onboard NIC goes to the debugger and the OS networks over a USB
// adapter. The bring-up is r8169.c's order (FreeBSD re(4)'s), minus the
// interrupt and the PHY kick. Small rings: this carries GDB's packets.
#include "kdebug_nic.h"
#include "r8169_regs.h"
#include "r8169.h"          // the descriptor rules r8169_*() -- shared, KTESTed
#include "pci_internal.h"   // pci_bar_mem_addr(), pci_command_update()
#include "pmm.h"
#include "string.h"
#include "barrier.h"
#include "driver.h"

DRIVER_DECLARE("r8169-kdb", "net", "an RTL8111/8168 owned by the kernel debugger (kdebug=net)");

#define KRX 8
#define KTX 8
#define KBUF R8169_RX_BUF

static volatile uint8_t *g_mmio;
static struct rl_desc *g_rx, *g_tx;
static uint8_t *g_rxbuf, *g_txbuf;
static uint64_t g_rxbuf_phys, g_txbuf_phys;
static uint32_t g_rx_cur, g_tx_cur;

static uint8_t  rd8 (uint32_t o) { return *(volatile uint8_t  *)(g_mmio + o); }
static uint32_t rd32(uint32_t o) { return *(volatile uint32_t *)(g_mmio + o); }
static uint16_t rd16(uint32_t o) { return *(volatile uint16_t *)(g_mmio + o); }
static void wr8 (uint32_t o, uint8_t v)  { *(volatile uint8_t  *)(g_mmio + o) = v; }
static void wr16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(g_mmio + o) = v; }
static void wr32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(g_mmio + o) = v; }
static void wr64(uint32_t o, uint64_t v) { wr32(o, (uint32_t)v); wr32(o + 4, (uint32_t)(v >> 32)); }

static int rtl_match(const struct pci_device *pci) {
    return pci->vendor_id == REALTEK_VENDOR &&
           (pci->device_id == 0x8168 || pci->device_id == 0x8136);
}

static void rx_hand_back(uint32_t i) {
    g_rx[i].opts2 = 0;
    g_rx[i].addr = g_rxbuf_phys + (uint64_t)i * KBUF;
    kbarrier();   // the address before the ownership
    g_rx[i].opts1 = R8169_DESC_OWN | (i == KRX - 1 ? R8169_DESC_EOR : 0) | KBUF;
}

static int rtl_claim(const struct pci_device *pci, uint8_t mac[6]) {
    uint64_t bar = pci_bar_mem_addr(pci, 2);   // BAR0 is the I/O alias
    if (!bar) return 0;
    g_mmio = (volatile uint8_t *)(uintptr_t)bar;

    // One page for both rings: each base must be 256-byte aligned.
    uint64_t rings = pmm_alloc_contiguous(1, PMM_ZONE_DMA32);
    uint64_t rxb = pmm_alloc_contiguous(KRX * KBUF / 4096, PMM_ZONE_DMA32);
    uint64_t txb = pmm_alloc_contiguous(KTX * KBUF / 4096, PMM_ZONE_DMA32);
    if (!rings || !rxb || !txb) return 0;
    k_memset((void *)(uintptr_t)rings, 0, 4096);
    g_rx = (struct rl_desc *)(uintptr_t)rings;
    g_tx = (struct rl_desc *)(uintptr_t)(rings + 2048);
    g_rxbuf = (uint8_t *)(uintptr_t)rxb;
    g_txbuf = (uint8_t *)(uintptr_t)txb;
    g_rxbuf_phys = rxb;
    g_txbuf_phys = txb;

    pci_command_update(pci, PCI_CMD_MEMORY | PCI_CMD_BUS_MASTER, 0);
    wr16(REG_IMR, 0);   // polled: no interrupt, ever
    wr8(REG_CR, CR_RESET);
    int spin = 0;
    while (rd8(REG_CR) & CR_RESET) {
        if (++spin > 200000) return 0;
        cpu_relax();
    }
    for (int i = 0; i < 6; i++) mac[i] = rd8(REG_IDR0 + i);

    for (uint32_t i = 0; i < KRX; i++) rx_hand_back(i);
    g_tx[KTX - 1].opts1 = R8169_DESC_EOR;   // the rest are zero: ours, idle

    wr8(REG_9346CR, CFG_UNLOCK);
    uint16_t cpcr = rd16(REG_CPCR);
    cpcr |= CPCR_PCI_MRW;
    cpcr &= (uint16_t)~(CPCR_RXCSUM | CPCR_VLANSTRIP);
    wr16(REG_CPCR, cpcr);
    wr16(REG_RMS, KBUF);
    wr8(REG_MTPS, 0x3F);
    wr64(REG_RDSAR, rings);
    wr64(REG_TNPDS, rings + 2048);
    wr8(REG_9346CR, CFG_LOCK);
    uint32_t xid = rd32(REG_TCR) & TCR_HWREV;
    wr32(REG_TCR, TCR_IFG_STD | TCR_DMA_UNLIM);
    // The receiver's order depends on the generation -- r8169.c says why.
    // The old order on the desktop's 8168G received one lap of this ring
    // (8 frames of LAN broadcast) and then nothing, so the stub went deaf.
    uint32_t rcr = RCR_FIFO_NONE | RCR_DMA_UNLIM | RCR_BROAD | RCR_MULTI | RCR_INDIV;
    if (r8169_g_family(xid)) {
        wr32(REG_MISC, rd32(REG_MISC) & ~MISC_RXDV_GATED);
        wr32(REG_RCR, rcr | RCR_EARLYOFF_V2);
        wr32(REG_MAR0, 0xFFFFFFFFu);
        wr32(REG_MAR0 + 4, 0xFFFFFFFFu);
        wr8(REG_CR, CR_RX_ENB | CR_TX_ENB);
    } else {
        wr8(REG_CR, CR_RX_ENB | CR_TX_ENB);
        wr32(REG_RCR, rcr);
        wr32(REG_MAR0, 0xFFFFFFFFu);
        wr32(REG_MAR0 + 4, 0xFFFFFFFFu);
    }
    wr16(REG_ISR, 0xFFFF);
    return 1;
}

static int rtl_recv(uint8_t *buf, int cap) {
    // Status still latches with the mask off; clear it so a ring that
    // ran dry (RDU) is not left flagged.
    uint16_t st = rd16(REG_ISR);
    if (st) wr16(REG_ISR, st);
    for (;;) {
        struct rl_desc *d = &g_rx[g_rx_cur];
        uint32_t opts1 = d->opts1;
        if (opts1 & R8169_DESC_OWN) return 0;
        int len = (int)r8169_rx_frame_len(opts1);
        int ok = len > 0 && len <= cap;
        if (ok) k_memcpy(buf, g_rxbuf + (uint64_t)g_rx_cur * KBUF, (size_t)len);
        rx_hand_back(g_rx_cur);
        g_rx_cur = (g_rx_cur + 1) % KRX;
        if (ok) return len;
    }
}

static int rtl_send(const void *frame, int len) {
    if (len > KBUF) return 0;
    struct rl_desc *d = &g_tx[g_tx_cur];
    for (int spin = 0; d->opts1 & R8169_DESC_OWN; spin++) {   // not sent yet
        if (spin > 1000000) return 0;
        cpu_relax();
    }
    uint32_t padded = r8169_tx_pad((uint32_t)len);
    uint8_t *b = g_txbuf + (uint64_t)g_tx_cur * KBUF;
    k_memcpy(b, frame, (size_t)len);
    if (padded > (uint32_t)len) k_memset(b + len, 0, padded - (uint32_t)len);
    d->opts2 = 0;
    d->addr = g_txbuf_phys + (uint64_t)g_tx_cur * KBUF;
    kbarrier();
    d->opts1 = r8169_tx_opts1(padded, g_tx_cur == KTX - 1);
    kbarrier();
    g_tx_cur = (g_tx_cur + 1) % KTX;
    wr8(REG_TPPOLL, TPPOLL_NPQ);
    return 1;
}

const struct kdb_nic kdb_nic_r8169 = { "r8169", rtl_match, rtl_claim, rtl_recv, rtl_send };
