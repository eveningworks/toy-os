// Realtek USB Ethernet -- the transport core. See rtl_usb.h for the
// split (this file owns the registers, the framing, the bulk pair, the
// link poll and binding; a chip file owns its bring-up) and for the
// reference this is adapted from.
//
// TWO THINGS DIFFER FROM A CLASS DRIVER. Registers are reached by
// VENDOR CONTROL TRANSFER -- four bytes at a time behind a byte-enable
// mask, because the chip is on the far side of USB and there is no MMIO
// window to map. And A BULK TRANSFER IS NOT A FRAME: transmit prepends
// an 8-byte descriptor, receive returns a 24-byte descriptor per frame
// padded to 8, and one transfer may carry several frames.
//
// driver-none: the transport core; rtl8153.c and rtl8156.c declare the drivers
#include "rtl_usb.h"
#include "clocksource.h" // clocksource_delay_ms -- a delay that needs no interrupt
#include "xhci.h"
#include "xhci_regs.h"
#include "pmm.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"
#include "timer.h"
#include "errno.h"

static struct rtl_usb g_rtl;

// The DMA target for every register access. Static rather than on a
// stack: a ring-0 stack is 16 KiB behind a guard page with a 1 KiB frame
// budget, and this must be identity-mapped for the controller.
static uint8_t g_reg_buf[8] __attribute__((aligned(64)));

void rtl_wait_ms(uint32_t ms) { clocksource_delay_ms(ms); }

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void st32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

int rtl_reg_read_mem(uint16_t addr, uint16_t index, void *buf, uint16_t len) {
    uint8_t setup[8] = { TYPE_IN_VENDOR, REQ_REGS,
                         (uint8_t)addr, (uint8_t)(addr >> 8),
                         (uint8_t)index, (uint8_t)(index >> 8),
                         (uint8_t)len, (uint8_t)(len >> 8) };
    return xhci_control(g_rtl.slot, setup, buf, len, 1);
}

int rtl_reg_write_mem(uint16_t addr, uint16_t index, void *buf, uint16_t len) {
    uint8_t setup[8] = { TYPE_OUT_VENDOR, REQ_REGS,
                         (uint8_t)addr, (uint8_t)(addr >> 8),
                         (uint8_t)index, (uint8_t)(index >> 8),
                         (uint8_t)len, (uint8_t)(len >> 8) };
    return xhci_control(g_rtl.slot, setup, buf, len, 0);
}

uint32_t rtl_read4(uint16_t reg, uint16_t index) {
    if (rtl_reg_read_mem(reg, index, g_reg_buf, 4) < 4) return 0;
    return le32(g_reg_buf);
}

// The same read, but SAYING whether it worked. rtl_read4() cannot: it
// returns 0 for a failure and 0 is a legitimate register value. That is
// fine for setup, where a wrong value fails loudly a moment later, and
// wrong for a POLL whose whole job is to notice its device has gone.
int rtl_read2_ok(uint16_t reg, uint16_t index, uint16_t *out) {
    if (rtl_reg_read_mem((uint16_t)(reg & ~3u), index, g_reg_buf, 4) < 4) return 0;
    uint8_t shift = (uint8_t)((reg & 2) << 3);
    *out = (uint16_t)((le32(g_reg_buf) >> shift) & 0xFFFF);
    return 1;
}

uint16_t rtl_read2(uint16_t reg, uint16_t index) {
    uint8_t shift = (uint8_t)((reg & 2) << 3);
    uint32_t v = rtl_read4((uint16_t)(reg & ~3u), index);
    return (uint16_t)((v >> shift) & 0xFFFF);
}

uint8_t rtl_read1(uint16_t reg, uint16_t index) {
    uint8_t shift = (uint8_t)((reg & 3) << 3);
    uint32_t v = rtl_read4((uint16_t)(reg & ~3u), index);
    return (uint8_t)((v >> shift) & 0xFF);
}

int rtl_write4(uint16_t reg, uint16_t index, uint32_t val) {
    st32(g_reg_buf, val);
    return rtl_reg_write_mem(reg, (uint16_t)(index | BYTE_EN_DWORD), g_reg_buf, 4);
}

int rtl_write2(uint16_t reg, uint16_t index, uint32_t val) {
    uint16_t byen = BYTE_EN_WORD;
    uint8_t shift = (uint8_t)(reg & 2);
    val &= 0xFFFF;
    if (shift) { byen = (uint16_t)(byen << shift); val <<= (shift << 3); reg &= (uint16_t)~3u; }
    st32(g_reg_buf, val);
    return rtl_reg_write_mem(reg, (uint16_t)(index | byen), g_reg_buf, 4);
}

int rtl_write1(uint16_t reg, uint16_t index, uint32_t val) {
    uint16_t byen = BYTE_EN_BYTE;
    uint8_t shift = (uint8_t)(reg & 3);
    val &= 0xFF;
    if (shift) { byen = (uint16_t)(byen << shift); val <<= (shift << 3); reg &= (uint16_t)~3u; }
    st32(g_reg_buf, val);
    return rtl_reg_write_mem(reg, (uint16_t)(index | byen), g_reg_buf, 4);
}

void rtl_set2(uint16_t reg, uint16_t index, uint16_t bits) {
    rtl_write2(reg, index, (uint32_t)(rtl_read2(reg, index) | bits));
}
void rtl_clr2(uint16_t reg, uint16_t index, uint16_t bits) {
    rtl_write2(reg, index, (uint32_t)(rtl_read2(reg, index) & ~bits));
}
void rtl_set1(uint16_t reg, uint16_t index, uint8_t bits) {
    rtl_write1(reg, index, (uint32_t)(rtl_read1(reg, index) | bits));
}
void rtl_clr1(uint16_t reg, uint16_t index, uint8_t bits) {
    rtl_write1(reg, index, (uint32_t)(rtl_read1(reg, index) & ~bits));
}
void rtl_clr4(uint16_t reg, uint16_t index, uint32_t bits) {
    rtl_write4(reg, index, rtl_read4(reg, index) & ~bits);
}

// The PHY lives behind a window: the high nibble of the address goes in
// PLA_OCP_GPHY_BASE and the rest becomes a PLA register at 0xb000.
uint16_t rtl_ocp_read(uint16_t addr) {
    rtl_write2(PLA_OCP_GPHY_BASE, MCU_PLA, addr & 0xF000);
    return rtl_read2((uint16_t)((addr & 0x0FFF) | 0xB000), MCU_PLA);
}

void rtl_ocp_write(uint16_t addr, uint16_t data) {
    rtl_write2(PLA_OCP_GPHY_BASE, MCU_PLA, addr & 0xF000);
    rtl_write2((uint16_t)((addr & 0x0FFF) | 0xB000), MCU_PLA, data);
}

void rtl_sram_write(uint16_t addr, uint16_t data) {
    rtl_ocp_write(OCP_SRAM_ADDR, addr);
    rtl_ocp_write(OCP_SRAM_DATA, data);
}

// --- framing, exported for the KTESTs ---------------------------------

void usb_r8153_tx_desc(uint8_t out[TXD_LEN], uint32_t len) {
    st32(out, (len & TXPKT_LEN_MASK) | TXPKT_FS | TXPKT_LS);
    st32(out + 4, 0);
}

uint32_t usb_r8153_rx_step(const uint8_t *buf, uint32_t avail,
                           uint32_t *out_off, uint32_t *out_len) {
    *out_off = 0;
    *out_len = 0;
    if (avail < RXD_LEN) return 0;

    uint32_t len = le32(buf) & RXPKT_LEN_MASK;
    // A length at the mask's ceiling says nothing about where the NEXT
    // descriptor begins, so the rest of the transfer is unwalkable and
    // is dropped rather than guessed at -- continuing would let one
    // frame's payload be delivered as a frame of its own.
    if (len >= RXPKT_LEN_MASK) return 0;
    if (len > avail - RXD_LEN) return 0;             // truncated

    *out_off = RXD_LEN;
    if (len >= 14 + ETH_CRC_LEN && len - ETH_CRC_LEN <= NET_FRAME_MAX)
        *out_len = len - ETH_CRC_LEN;                // the CRC is not frame

    uint32_t stride = RXD_LEN + ((len + RXD_ALIGN - 1) & ~(uint32_t)(RXD_ALIGN - 1));
    return stride > avail ? avail : stride;          // the last one may be short
}

// --- sequences the chips share -----------------------------------------

void rtl_hw_reset(void) {
    rtl_write1(PLA_CR, MCU_PLA, CR_RST);
    for (int i = 0; i < TIMEOUT_MS / 10; i++) {
        if (!(rtl_read1(PLA_CR, MCU_PLA) & CR_RST)) return;
        rtl_wait_ms(10);
    }
    klog_write("rtl-usb: reset never completed\n");
}

void rtl_disable_teredo(int b_family) {
    if (b_family) rtl_write1(PLA_TEREDO_CFG, MCU_PLA, 0xFF);
    else rtl_clr2(PLA_TEREDO_CFG, MCU_PLA, TEREDO_SEL | TEREDO_RS_EVENT_MASK | OOB_TEREDO_EN);
    rtl_write2(PLA_WDT6_CTRL, MCU_PLA, WDT6_SET_MODE);
    rtl_write2(PLA_REALWOW_TIMER, MCU_PLA, 0);
    rtl_write4(PLA_TEREDO_TIMER, MCU_PLA, 0);
}

void rtl_disable_aldps(void) {
    rtl_ocp_write(OCP_ALDPS_CONFIG, ENPDNPS | LINKENA | DIS_SDSAVE);
    for (int i = 0; i < 20; i++) {
        rtl_wait_ms(1);
        if (rtl_ocp_read(0xE000) & 0x0100) break;
    }
}

void rtl_enable_aldps(void) {
    rtl_ocp_write(OCP_POWER_CFG, (uint16_t)(rtl_ocp_read(OCP_POWER_CFG) | EN_ALDPS));
}

uint16_t rtl_phy_status(uint16_t desired) {
    uint16_t st = 0;
    for (int i = 0; i < TIMEOUT_MS / 10; i++) {
        st = rtl_ocp_read(OCP_PHY_STATUS) & PHY_STAT_MASK;
        if (desired ? st == desired
                    : (st == PHY_STAT_LAN_ON || st == PHY_STAT_PWRDN || st == PHY_STAT_EXT_INIT))
            return st;
        rtl_wait_ms(10);
    }
    klog_write("rtl-usb: the PHY never settled\n");
    return st;
}

void rtl_wait_autoload(void) {
    for (int i = 0; i < TIMEOUT_MS / 10; i++) {
        if (rtl_read2(PLA_BOOT_CTRL, MCU_PLA) & AUTOLOAD_DONE) return;
        rtl_wait_ms(10);
    }
}

void rtl_tolerance(uint8_t fill) {
    for (int i = 0; i < 8; i++) g_reg_buf[i] = fill;
    rtl_reg_write_mem(USB_TOLERANCE, MCU_USB | BYTE_EN_SIX, g_reg_buf, 8);
}

int rtl_link_up_now(void) {
    return (rtl_read2(PLA_PHYSTATUS, MCU_PLA) & PHYSTATUS_LINK) ? 1 : 0;
}

// ure(4)'s ure_init(): what the interface needs to actually pass
// traffic, as opposed to the chip bring-up the ops did. The MAC, then
// the chip's thresholds, then receive and transmit on, then the
// advertisement.
static void mac_start(struct rtl_usb *d, const uint8_t mac[NET_MAC_LEN]) {
    rtl_hw_reset();

    rtl_write1(PLA_CRWECR, MCU_PLA, CRWECR_CONFIG);
    for (int i = 0; i < NET_MAC_LEN; i++) g_reg_buf[i] = mac[i];
    g_reg_buf[6] = g_reg_buf[7] = 0;
    rtl_reg_write_mem(PLA_IDR, MCU_PLA | BYTE_EN_SIX, g_reg_buf, 8);
    rtl_write1(PLA_CRWECR, MCU_PLA, CRWECR_NORMAL);

    d->ops->start(d);

    // ure(4) leaves this at the reset default on an 8153A; set it, since
    // an adapter that came out of OOB with a smaller one silently drops
    // full-size frames.
    rtl_write2(PLA_RMS, MCU_PLA, RTL_FRAMELEN);

    rtl_clr2(PLA_FMC, MCU_PLA, FMC_FCR_MCU_EN);
    rtl_set2(PLA_FMC, MCU_PLA, FMC_FCR_MCU_EN);
    rtl_clr2(PLA_CPCR, MCU_PLA, CPCR_RX_VLAN);

    rtl_set1(PLA_CR, MCU_PLA, CR_RE | CR_TE);
    rtl_clr2(PLA_MISC_1, MCU_PLA, RXDY_GATED_EN);

    // Unicast to us plus broadcast, and every multicast group -- there
    // is no multicast membership list in this stack to filter against.
    rtl_write4(PLA_MAR0, MCU_PLA, 0xFFFFFFFFu);
    rtl_write4(PLA_MAR4, MCU_PLA, 0xFFFFFFFFu);
    rtl_write4(PLA_RCR, MCU_PLA, RCR_APM | RCR_AB | RCR_AM);

    d->ops->advertise(d);
}

// --- the data path ----------------------------------------------------

static void rx_done(void *ctx, uint64_t phys, uint32_t bytes, int ok) {
    struct rtl_usb *d = ctx;
    if (!d->in_use) return;

    if (ok && bytes) {
        const uint8_t *p = (const uint8_t *)(uintptr_t)phys;
        uint32_t off = 0;
        while (off < bytes) {
            uint32_t fo = 0, fl = 0;
            uint32_t step = usb_r8153_rx_step(p + off, bytes - off, &fo, &fl);
            if (!step) { d->dev.rx_dropped++; break; }
            if (fl) net_rx(&d->dev, p + off + fo, fl);
            else    d->dev.rx_dropped++;
            off += step;
        }
    }
    xhci_bulk_post(d->slot, d->ep_in, phys, RTL_RX_BUF);
}

static void tx_done(void *ctx, uint64_t phys, uint32_t bytes, int ok) {
    struct rtl_usb *d = ctx;
    (void)bytes; (void)ok;
    for (int i = 0; i < RTL_BUFS; i++)
        if (d->tx_phys[i] == phys) { d->tx_busy[i] = 0; return; }
}

static int rtl_tx_slot(struct rtl_usb *d) {
    for (int i = 0; i < RTL_BUFS; i++) {
        int at = (d->tx_next + i) % RTL_BUFS;
        if (!d->tx_busy[at]) return at;
    }
    return -1;
}

static int rtl_transmit(struct net_device *dev, const void *frame, uint32_t len) {
    struct rtl_usb *d = dev->drv;
    if (!d->in_use) return -ENODEV;
    if (len > NET_FRAME_MAX) return -EINVAL;

    int slot = rtl_tx_slot(d);
    // Full: reap completions before saying so. tx_done() runs from the
    // event ring, and a caller waiting on a full ring (net_tx()) may be
    // where no interrupt reaches -- a fragment burst outruns the ring.
    if (slot < 0) { xhci_service(); slot = rtl_tx_slot(d); }
    if (slot < 0) return -ENOSPC;

    usb_r8153_tx_desc(d->tx[slot], len);
    k_memcpy(d->tx[slot] + TXD_LEN, frame, len);
    d->tx_busy[slot] = 1;
    d->tx_next = (uint8_t)((slot + 1) % RTL_BUFS);

    uint32_t total = TXD_LEN + len;
    if (xhci_bulk_post(d->slot, d->ep_out, d->tx_phys[slot], total) < 0) {
        d->tx_busy[slot] = 0;
        return -EIO;
    }
    // A transfer that is an exact multiple of the endpoint's packet size
    // does not END until a short packet follows it, so the device sits
    // waiting for more of a frame it already has. FreeBSD spells this
    // force_short_xfer; here it is a second, zero-length TRB.
    if (d->mps && total % d->mps == 0)
        xhci_bulk_post(d->slot, d->ep_out, d->tx_phys[slot], 0);
    return 0;
}

// Link state, from the MAC's own view of the PHY. Rate-limited to 1 Hz
// because this is a synchronous CONTROL TRANSFER and net_poll() runs
// from scheduler_idle() -- net_poll()'s own re-entrancy guard is what
// keeps two of them off endpoint 0 at once.
// A DEVICE THAT HAS GONE MUST STOP BEING POLLED, and three seconds of
// silence is enough to say so. Without this a vanished adapter is asked
// once a second forever, and each ask is a CONTROL TRANSFER that burns
// its full 1000 ms timeout inside scheduler_idle() and logs a line --
// so the machine spends about a second of every second on a doomed
// transfer, and the klog ring loses its boot history in a few minutes.
// Measured on the ASUS: 72 timeouts, and the boot log gone with them,
// which is precisely the "a probe that outruns the log destroys the
// evidence" trap CLAUDE.md warns about, arrived at from the driver side.
//
// Nothing has to un-latch this: a replug re-enumerates the adapter and
// binds a fresh rtl_usb, which is where the state lives.
#define RTL_LINK_FAILS_MAX 3

static void rtl_poll(struct net_device *dev) {
    struct rtl_usb *d = dev->drv;
    if (!d->in_use || d->link_gone) return;
    uint64_t now = coarse_ticks();
    if (d->link_checked && now - d->link_checked < 100) return;
    d->link_checked = now;

    uint16_t st;
    if (!rtl_read2_ok(PLA_PHYSTATUS, MCU_PLA, &st)) {
        if (++d->link_fails < RTL_LINK_FAILS_MAX) return;
        d->link_gone = 1;
        // ONCE, and it says what stopped as well as why -- a link that
        // simply goes down is a different event from one nobody is
        // watching any more.
        klog_printf("usb-net: %s not answering after %u tries -- link "
                    "polling stopped; replug to recover\n",
                    dev->name, (unsigned)RTL_LINK_FAILS_MAX);
        dev->link_up = 0;
        dev->link_bps = 0;
        dev->link_known = 1;
        return;
    }
    d->link_fails = 0;
    uint8_t up = (st & PHYSTATUS_LINK) ? 1 : 0;
    uint32_t bps = !up ? 0
                 : (st & PHYSTATUS_2500MBPS) ? 2500000000u
                 : (st & PHYSTATUS_1000MBPS) ? 1000000000u
                 : (st & PHYSTATUS_100MBPS)  ? 100000000u
                 : (st & PHYSTATUS_10MBPS)   ? 10000000u : 0;

    if (!dev->link_known || dev->link_up != up || dev->link_bps != bps) {
        klog_printf("usb-net: %s link %s%s\n", dev->name, up ? "UP" : "down",
                    bps == 2500000000u ? " 2500M" :
                    bps == 1000000000u ? " 1000M" :
                    bps == 100000000u  ? " 100M"  :
                    bps == 10000000u   ? " 10M"   : "");
        if (d->ops->link_changed) d->ops->link_changed(d, up, bps);
    }
    dev->link_up = up;
    dev->link_bps = bps;
    dev->link_known = 1;
}

// --- binding ----------------------------------------------------------

// The devices this drives, by id. A vendor-specific configuration says
// nothing about what is behind it, so an unlisted device is left alone
// rather than probed -- the cost of being wrong is claiming somebody
// else's hardware and writing to its registers.
struct rtl_id { uint16_t vid, pid; };
static const struct rtl_id g_ids[] = {
    { 0x0BDA, 0x8152 },   // Realtek RTL8152
    { 0x0BDA, 0x8153 },   // Realtek RTL8153
    { 0x0BDA, 0x8156 },   // Realtek RTL8156 / RTL8156B, 2.5G
    { 0x2357, 0x0601 },   // TP-Link UE300
};

const struct rtl_usb_ops *rtl_usb_chip_for(uint16_t version) {
    static const struct rtl_usb_ops *const chips[] = { &rtl8153_ops, &rtl8156_ops };
    for (unsigned i = 0; i < sizeof chips / sizeof chips[0]; i++)
        if (chips[i]->match(version)) return chips[i];
    return 0;
}

int usb_r8153_claims(uint16_t vid, uint16_t pid) {
    for (unsigned i = 0; i < sizeof g_ids / sizeof g_ids[0]; i++)
        if (g_ids[i].vid == vid && g_ids[i].pid == pid) return 1;
    return 0;
}

#define DESC_INTERFACE_T  0x04
#define DESC_ENDPOINT_T   0x05

// The vendor interface's bulk pair. Alt 0 carries both, unlike CDC's
// data interface, so there is no SET_INTERFACE here.
static int parse_endpoints(const uint8_t *cfg, uint32_t total,
                           uint8_t *ep_in, uint8_t *ep_out, uint16_t *mps) {
    int in_vendor = 0;
    *ep_in = *ep_out = 0;
    *mps = 0;
    for (uint32_t o = 0; o + 2 <= total; ) {
        uint32_t blen = cfg[o];
        if (blen < 2 || o + blen > total) return 0;
        if (cfg[o + 1] == DESC_INTERFACE_T && blen >= 9) {
            in_vendor = (cfg[o + 5] == 0xFF && cfg[o + 3] == 0);
        } else if (in_vendor && cfg[o + 1] == DESC_ENDPOINT_T && blen >= 7 &&
                   (cfg[o + 3] & 0x03) == 2) {
            uint16_t w = (uint16_t)((cfg[o + 4] | ((uint16_t)cfg[o + 5] << 8)) & 0x7FF);
            if (cfg[o + 2] & 0x80) *ep_in = cfg[o + 2]; else *ep_out = cfg[o + 2];
            *mps = w;
        }
        o += blen;
    }
    return *ep_in && *ep_out && *mps;
}

static void log_mac(const char *what, const uint8_t *m) {
    klog_printf("usb-net: %s %s %02x:%02x:%02x:%02x:%02x:%02x\n", g_rtl.ops->name, what,
                m[0], m[1], m[2], m[3], m[4], m[5]);
}

int usb_r8153_bind(struct usb_device_info *info, const uint8_t *cfg,
                   uint32_t total) {
    if (!info || g_rtl.in_use) return 0;
    if (!usb_r8153_claims(info->vendor_id, info->product_id)) return 0;

    uint8_t ep_in, ep_out;
    uint16_t mps;
    if (!parse_endpoints(cfg, total, &ep_in, &ep_out, &mps)) {
        klog_printf("usb: slot %u: r8153 has no vendor bulk pair -- not bound\n",
                    info->slot);
        return 0;
    }

    struct rtl_usb *d = &g_rtl;
    k_memset(d, 0, sizeof *d);
    d->slot = info->slot;
    d->ep_in = ep_in;
    d->ep_out = ep_out;
    d->mps = mps;
    d->speed = info->speed;

    // WAIT FOR THE CHIP'S AUTOLOAD BEFORE READING ANYTHING. A cold boot
    // probes this adapter a third of a second in, before its firmware
    // has loaded the registers, and PLA_TCR1 then answers 0 and PLA_IDR
    // half a MAC -- which read exactly like an unsupported chip and left
    // the laptop with no network one boot in three. A chip's init waits
    // for the same bit, but the version check below comes first.
    rtl_wait_autoload();

    // THE FIRST CHECKPOINT, and the one that says whether any of the
    // rest is honest: a register layer that is wrong here reads back
    // zeroes and every write after it is silently discarded. A zero is
    // retried a few times rather than believed, for the same reason.
    d->version = 0;
    for (int i = 0; i < 5 && !d->version; i++) {
        if (i) rtl_wait_ms(20);
        d->version = (uint16_t)(rtl_read2(PLA_TCR1, MCU_PLA) & VERSION_MASK);
    }
    uint8_t idr[NET_MAC_LEN];
    if (rtl_reg_read_mem(PLA_IDR, MCU_PLA, g_reg_buf, 8) < 8) {
        klog_printf(KLOG_ERR "usb: slot %u: rtl-usb register read failed -- not bound\n",
                    info->slot);
        return 0;
    }
    for (int i = 0; i < NET_MAC_LEN; i++) idr[i] = g_reg_buf[i];

    // THE VERSION GATE. A chip no file claims -- an RTL8153B, an
    // RTL8152, or registers that did not read -- wants a sequence nobody
    // here has tested, and driving it with a neighbour's is worse than
    // leaving the device unbound.
    d->ops = rtl_usb_chip_for(d->version);
    if (!d->ops) {
        klog_printf("usb: slot %u: rtl-usb version 0x%04x not driven "
                    "-- not bound\n", info->slot, d->version);
        return 0;
    }
    klog_printf("usb-net: %s version 0x%04x\n", d->ops->name, d->version);
    log_mac("PLA_IDR", idr);

    uint64_t need = (uint64_t)RTL_BUFS * (RTL_RX_BUF + RTL_TX_BUF);
    d->mem_pages = (uint32_t)((need + 4095) / 4096);
    d->mem_phys = pmm_alloc_contiguous(d->mem_pages, PMM_ZONE_DMA32);
    if (!d->mem_phys) {
        klog_write("usb: no contiguous frames for the rtl-usb buffers\n");
        return 0;
    }
    uint8_t *base = (uint8_t *)(uintptr_t)d->mem_phys;   // identity-mapped
    for (int i = 0; i < RTL_BUFS; i++) {
        d->rx_phys[i] = d->mem_phys + (uint64_t)i * RTL_RX_BUF;
        uint32_t tx_at = RTL_BUFS * RTL_RX_BUF + (uint32_t)i * RTL_TX_BUF;
        d->tx[i] = base + tx_at;
        d->tx_phys[i] = d->mem_phys + tx_at;
    }

    d->ops->init(d);

    // The address the chip loaded from its own EEPROM. ure(4) takes it
    // from PLA_BACKUP on an 8153 and from PLA_IDR only on an 8152.
    uint8_t mac[NET_MAC_LEN];
    if (rtl_reg_read_mem(PLA_BACKUP, MCU_PLA, g_reg_buf, 8) < 8) {
        klog_printf("usb: slot %u: %s MAC unreadable -- not bound\n", info->slot, d->ops->name);
        pmm_free_contiguous(d->mem_phys, d->mem_pages);
        return 0;
    }
    for (int i = 0; i < NET_MAC_LEN; i++) mac[i] = g_reg_buf[i];
    log_mac("PLA_BACKUP", mac);

    int zero = 1;
    for (int i = 0; i < NET_MAC_LEN; i++) if (mac[i]) zero = 0;
    if (zero || (mac[0] & 0x01)) {
        // Refused rather than invented: a stack given an address the
        // hardware does not answer to is worse than no adapter.
        klog_printf("usb: slot %u: %s MAC is not a unicast address "
                    "-- not bound\n", info->slot, d->ops->name);
        pmm_free_contiguous(d->mem_phys, d->mem_pages);
        return 0;
    }
    for (int i = 0; i < NET_MAC_LEN; i++) d->dev.mac[i] = mac[i];

    mac_start(d, mac);

    if (xhci_add_bulk(info->slot, ep_in, mps, rx_done, d) < 0 ||
        xhci_add_bulk(info->slot, ep_out, mps, tx_done, d) < 0) {
        klog_printf("usb: slot %u: %s bulk endpoints 0x%02x/0x%02x not "
                    "configured\n", info->slot, d->ops->name, ep_in, ep_out);
        pmm_free_contiguous(d->mem_phys, d->mem_pages);
        return 0;
    }

    d->in_use = 1;                 // published before a completion can arrive
    d->dev.driver = d->ops->name;
    d->dev.mtu = NET_MTU;
    d->dev.transmit = rtl_transmit;
    d->dev.poll = rtl_poll;        // link state only; receive is pushed
    d->dev.poll_ms = 1000;         // rtl_poll()'s own cadence
    d->dev.drv = d;

    net_location_usb(&d->dev, info->root_port, info->port);
    if (!net_register(&d->dev)) {
        d->in_use = 0;
        pmm_free_contiguous(d->mem_phys, d->mem_pages);
        return 0;
    }

    for (int i = 0; i < RTL_BUFS; i++)
        xhci_bulk_post(info->slot, ep_in, d->rx_phys[i], RTL_RX_BUF);

    usb_mark_bound(info, d->ops->name);
    klog_printf("usb: slot %u: bound as %s, ep in 0x%02x out 0x%02x, "
                "%u B/packet\n", info->slot, d->ops->name, ep_in, ep_out, mps);
    return 1;
}

void usb_r8153_unbind(uint8_t slot) {
    struct rtl_usb *d = &g_rtl;
    if (!d->in_use || d->slot != slot) return;
    d->in_use = 0;   // unpublished before the core can call transmit()
    net_unregister(&d->dev);
}
