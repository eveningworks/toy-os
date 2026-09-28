// An Intel 8254x OWNED BY THE KERNEL DEBUGGER -- the polled copy of
// e1000.c that kdebug_nic.h describes. The card is claimed before PCI
// binding, so e1000.c never probes it and nothing here shares a ring
// with the OS. Small rings: this carries GDB's packets, not traffic.
#include "kdebug_nic.h"
#include "e1000_regs.h"
#include "pci_internal.h"   // pci_bar_mem_addr()
#include "pmm.h"
#include "string.h"
#include "barrier.h"        // cpu_relax()
#include "driver.h"

DRIVER_DECLARE("e1000-kdb", "net", "an 8254x owned by the kernel debugger (kdebug=net)");

// EIGHT EACH, THE MINIMUM: RDLEN and TDLEN must be multiples of 128
// bytes. A 4-descriptor TX ring (64) made the card resend a stale buffer
// instead of the frame just queued.
#define KRX 8
#define KTX 8
#define KBUF 2048

static volatile uint8_t *g_mmio;
static struct rx_desc *g_rx;
static struct tx_desc *g_tx;
static uint8_t *g_rxbuf, *g_txbuf;
static uint64_t g_rxbuf_phys, g_txbuf_phys;
static uint32_t g_rx_cur, g_tx_cur;

static uint32_t rd(uint32_t off) { return *(volatile uint32_t *)(g_mmio + off); }
static void wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(g_mmio + off) = v; }

static int e1k_match(const struct pci_device *pci) {
    return pci->vendor_id == E1000_VENDOR && pci->device_id == E1000_DEV_82540EM;
}

static int e1k_claim(const struct pci_device *pci, uint8_t mac[6]) {
    uint64_t bar = pci_bar_mem_addr(pci, 0);
    if (!bar || pci_bar_is_io(pci->bar[0])) return 0;
    g_mmio = (volatile uint8_t *)(uintptr_t)bar;   // identity-mapped below 4 GiB

    uint64_t rings = pmm_alloc_contiguous(1, PMM_ZONE_DMA32);
    uint64_t rxb = pmm_alloc_contiguous(KRX * KBUF / 4096, PMM_ZONE_DMA32);
    uint64_t txb = pmm_alloc_contiguous(KTX * KBUF / 4096, PMM_ZONE_DMA32);
    if (!rings || !rxb || !txb) return 0;
    k_memset((void *)(uintptr_t)rings, 0, 4096);
    g_rx = (struct rx_desc *)(uintptr_t)rings;
    g_tx = (struct tx_desc *)(uintptr_t)(rings + 2048);
    g_rxbuf = (uint8_t *)(uintptr_t)rxb;
    g_txbuf = (uint8_t *)(uintptr_t)txb;
    g_rxbuf_phys = rxb;
    g_txbuf_phys = txb;

    pci_enable_bus_master(pci);
    // A GLOBAL RESET FIRST: firmware (a PXE or UEFI network stack) may
    // have left the card running, DMAing into rings that are not ours.
    // Receive and transmit off, then CTRL.RST, which clears itself; the
    // EEPROM reload that follows is what sets RAH.AV again. Both waits
    // are bounded -- a card that never finishes is refused, not hung on.
    wr(REG_IMC, 0xFFFFFFFFu);
    wr(REG_RCTL, 0);
    wr(REG_TCTL, 0);
    rd(REG_STATUS);
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_RST);
    for (int i = 0; i < 1000; i++) cpu_relax();   // no register access for ~1 us
    for (int spin = 0; rd(REG_CTRL) & CTRL_RST; spin++) {
        if (spin > 1000000) return 0;
        cpu_relax();
    }
    for (int spin = 0; !(rd(REG_RAH) & RAH_AV); spin++) {
        if (spin > 1000000) return 0;
        cpu_relax();
    }
    wr(REG_IMC, 0xFFFFFFFFu);   // polled: no interrupt, ever
    rd(REG_ICR);
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_SLU | CTRL_ASDE);
    for (int i = 0; i < 128; i++) wr(REG_MTA + i * 4, 0);

    for (int i = 0; i < KRX; i++) g_rx[i].addr = rxb + (uint64_t)i * KBUF;
    wr(REG_RDBAL, (uint32_t)rings);
    wr(REG_RDBAH, (uint32_t)(rings >> 32));
    wr(REG_RDLEN, KRX * (uint32_t)sizeof(struct rx_desc));
    wr(REG_RDH, 0);
    wr(REG_RDT, KRX - 1);
    wr(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);

    uint64_t txr = rings + 2048;
    wr(REG_TDBAL, (uint32_t)txr);
    wr(REG_TDBAH, (uint32_t)(txr >> 32));
    wr(REG_TDLEN, KTX * (uint32_t)sizeof(struct tx_desc));
    wr(REG_TDH, 0);
    wr(REG_TDT, 0);
    wr(REG_TIPG, 0x0060200Au);
    wr(REG_TCTL, TCTL_EN | TCTL_PSP | (0x0Fu << 4) | (0x40u << 12));

    // The receive-address pair only: QEMU and every 8254x with a
    // programmed EEPROM load it at reset. No EEPROM fallback here.
    uint32_t lo = rd(REG_RAL), hi = rd(REG_RAH);
    if (!(hi & RAH_AV)) return 0;
    for (int i = 0; i < 4; i++) mac[i] = (uint8_t)(lo >> (i * 8));
    mac[4] = (uint8_t)hi;
    mac[5] = (uint8_t)(hi >> 8);
    return 1;
}

static int e1k_recv(uint8_t *buf, int cap) {
    for (;;) {
        struct rx_desc *d = &g_rx[g_rx_cur];
        if (!(d->status & RX_STATUS_DD)) return 0;
        int len = d->length;
        int ok = (d->status & RX_STATUS_EOP) && !d->errors && len <= cap;
        if (ok) k_memcpy(buf, g_rxbuf + (uint64_t)g_rx_cur * KBUF, (size_t)len);
        d->status = 0;
        wr(REG_RDT, g_rx_cur);   // hand it back
        g_rx_cur = (g_rx_cur + 1) % KRX;
        if (ok) return len;
    }
}

static int e1k_send(const void *frame, int len) {
    if (len > KBUF) return 0;
    struct tx_desc *d = &g_tx[g_tx_cur];
    // Bounded: a card that never completes must not hang the debugger.
    for (int spin = 0; e1000_tx_full(g_tx, KTX, g_tx_cur); spin++) {
        if (spin > 1000000) return 0;
        cpu_relax();
    }
    k_memcpy(g_txbuf + (uint64_t)g_tx_cur * KBUF, frame, (size_t)len);
    d->addr = g_txbuf_phys + (uint64_t)g_tx_cur * KBUF;
    d->length = (uint16_t)len;
    d->cso = 0;
    d->cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    d->status = 0;
    d->css = 0;
    d->special = 0;
    g_tx_cur = (g_tx_cur + 1) % KTX;
    wr(REG_TDT, g_tx_cur);
    return 1;
}

const struct kdb_nic kdb_nic_e1000 = { "e1000", e1k_match, e1k_claim, e1k_recv, e1k_send };
