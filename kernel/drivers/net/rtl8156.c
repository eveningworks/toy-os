// RTL8156 / RTL8156B, the 2.5G part -- the chip file over rtl_usb.c.
//
// Adapted from ure(4)'s ure_rtl8153b_init() and ure_rtl8153b_nic_reset()
// (FreeBSD, BSD-2-clause, Kevin Lo -- notice in LICENSE), keeping the
// 8156 and 8156B branches and dropping the 8153B-only ones. Registers
// the reference leaves unnamed are kept as bare addresses, as it keeps
// them; a name invented here would claim knowledge nobody has.
//
// Two things differ from the 8153 beyond the sequence. The PHY reports
// 2.5G in PLA_PHYSTATUS's own bit and is advertised through an OCP
// register beside the MII ones (OCP_ADV_2500); and the MAC wants a
// power bit toggled by the negotiated speed each time the link comes
// up, which is what `link_changed` is for.
//
// RECEIVE AGGREGATION STAYS OFF here as on the 8153, where the reference
// turns it on: one frame per transfer keeps a bring-up legible.
#include "rtl_usb.h"
#include "xhci_regs.h"
#include "klog.h"
#include "driver.h"

DRIVER_DECLARE("r8156", "net", "Realtek RTL8156 2.5G USB Ethernet");

static int is_b(uint16_t v) { return v == VER_7400 || v == VER_7410; }

static int rtl8156_match(uint16_t v) {
    return v == VER_7020 || v == VER_7030 || is_b(v);
}

// ure_rtl8153b_nic_reset(), the 8156 branches: gate the receiver, take
// the chip out of OOB, reset the bulk endpoints' BMU, then the frame
// size, flow-control and FIFO parameters this part wants.
static void nic_reset(struct rtl_usb *d) {
    int b = is_b(d->version);

    rtl_clr2(USB_LPM_CONFIG, MCU_USB, LPM_U1U2_EN);
    rtl_clr2(USB_U2P3_CTRL, MCU_USB, U2P3_ENABLE);
    rtl_disable_aldps();

    rtl_set2(PLA_MISC_1, MCU_PLA, RXDY_GATED_EN);
    rtl_disable_teredo(1);
    rtl_clr4(PLA_RCR, MCU_PLA, RCR_ACPT_ALL);
    rtl_hw_reset();

    rtl_clr1(USB_BMU_RESET, MCU_USB, BMU_RESET_EP_IN | BMU_RESET_EP_OUT);
    rtl_set1(USB_BMU_RESET, MCU_USB, BMU_RESET_EP_IN | BMU_RESET_EP_OUT);
    rtl_clr1(PLA_OOB_CTRL, MCU_PLA, NOW_IS_OOB);
    rtl_clr2(PLA_SFF_STS_7, MCU_PLA, MCU_BORW_EN);

    // No VLAN stripping; the stack has no VLANs.
    rtl_write2(0xC012, MCU_PLA, (uint32_t)(rtl_read2(0xC012, MCU_PLA) & ~0x00C0));
    rtl_write2(PLA_RMS, MCU_PLA, RTL_FRAMELEN);
    rtl_write1(PLA_MTPS, MCU_PLA, MTPS_JUMBO);

    // Flow-control parameters, per part.
    if (b) { rtl_write2(0xC0A6, MCU_PLA, 0x0200); rtl_write2(0xC0AA, MCU_PLA, 0x0400); }
    else   { rtl_write2(0xC0A6, MCU_PLA, 0x0400); rtl_write2(0xC0AA, MCU_PLA, 0x0800); }

    // Receive FIFO threshold.
    rtl_write2(0xC0A2, MCU_PLA, (uint32_t)((rtl_read2(0xC0A2, MCU_PLA) & ~0x0FFF) | 0x08));
    rtl_write4(USB_RX_BUF_TH, MCU_USB, 0x00600400u);

    // Transmit FIFO threshold.
    if (b) {
        rtl_write2(PLA_TXFIFO_CTRL, MCU_PLA, 0x0008);
        rtl_write2(0xE61A, MCU_PLA, (RTL_FRAMELEN + 0x100) / 16);
    } else {
        rtl_write2(PLA_TXFIFO_CTRL, MCU_PLA, TXFIFO_THR_NORMAL2 & 0xFFFF);
        rtl_set2(0xD4B4, MCU_USB, 0x0002);
    }

    rtl_clr2(PLA_MAC_PWR_CTRL3, MCU_PLA, PLA_MCU_SPDWN_EN);
    rtl_clr2(0xD32A, MCU_USB, 0x0300);
    rtl_enable_aldps();
    rtl_set2(USB_U2P3_CTRL, MCU_USB, U2P3_ENABLE);
    if (d->speed == XHCI_SPEED_SUPER) rtl_set2(USB_LPM_CONFIG, MCU_USB, LPM_U1U2_EN);
}

// ure_rtl8153b_init(), the 8156 branches.
static void rtl8156_init(struct rtl_usb *d) {
    int b = is_b(d->version);

    rtl_clr1(0xD26B, MCU_USB, 0x01);
    rtl_write2(0xD32A, MCU_USB, 0);
    rtl_set2(0xCFEE, MCU_USB, 0x0020);
    if (b) rtl_set2(USB_U2P3_CTRL, MCU_USB, 0x0008);

    rtl_disable_aldps();
    rtl_clr2(USB_LPM_CONFIG, MCU_USB, LPM_U1U2_EN);

    // The 7410 stepping may still be loading its flash: wait for it.
    if (d->version == VER_7410 &&
        (rtl_read2(0xD3AE, MCU_PLA) & 0x0002) && !(rtl_read2(0xD284, MCU_USB) & 0x0020)) {
        for (int i = 0; i < 100; i++) {
            if (rtl_read2(0xD284, MCU_USB) & 0x0004) break;
            rtl_wait_ms(1);
        }
    }
    rtl_wait_autoload();

    // A PHY still in its external-init state is nudged out of it.
    if (rtl_phy_status(0) == PHY_STAT_EXT_INIT) {
        rtl_ocp_write(0xA468, (uint16_t)(rtl_ocp_read(0xA468) & ~0x0A));
        if (b) rtl_ocp_write(0xA466, (uint16_t)(rtl_ocp_read(0xA466) & ~0x01));
    }
    uint16_t bmcr = rtl_ocp_read(OCP_BASE_MII);
    if (bmcr & BMCR_PDOWN) rtl_ocp_write(OCP_BASE_MII, (uint16_t)(bmcr & ~BMCR_PDOWN));
    rtl_phy_status(PHY_STAT_LAN_ON);

    rtl_clr2(USB_U2P3_CTRL, MCU_USB, U2P3_ENABLE);
    rtl_write2(USB_MSC_TIMER, MCU_USB, 0x0FFF);     // MSC timer, 32760 ms
    rtl_write2(USB_U1U2_TIMER, MCU_USB, 500);       // U1/U2/L1 idle, 500 us

    // No power cut, no UPS, no queue wake, no runtime suspend: every
    // state the firmware could put the link into behind the host's
    // back stays off.
    rtl_clr2(USB_POWER_CUT, MCU_USB, PWR_EN);
    rtl_clr2(USB_MISC_0, MCU_USB, PCUT_STATUS);
    rtl_clr1(USB_POWER_CUT, MCU_USB, UPS_EN | USP_PREWAKE);
    rtl_clr1(0xCFFF, MCU_USB, 0x01);
    rtl_clr1(PLA_INDICATE_FALG, MCU_USB, UPCOMING_RUNTIME_D3);
    rtl_clr1(PLA_SUSPEND_FLAG, MCU_USB, LINK_CHG_EVENT);
    rtl_clr2(PLA_EXTRA_STATUS, MCU_USB, LINK_CHANGE_FLAG);
    rtl_write1(PLA_CRWECR, MCU_PLA, CRWECR_CONFIG);
    rtl_clr2(PLA_CONFIG34, MCU_USB, LINK_OFF_WAKE_EN);
    rtl_write1(PLA_CRWECR, MCU_PLA, CRWECR_NORMAL);

    if (d->speed == XHCI_SPEED_SUPER) rtl_set2(USB_LPM_CONFIG, MCU_USB, LPM_U1U2_EN);

    if (b) {
        rtl_clr2(PLA_RCR, MCU_PLA, 0x0800);
        rtl_set2(PLA_CPCR, MCU_PLA, 0x0001);
        // The flow-control timer, 600 ms, and the firmware's patch task.
        rtl_write2(USB_FC_TIMER, MCU_USB, CTRL_TIMER_EN | (600 / 8));
        if (!(rtl_read1(0xDC6B, MCU_PLA) & 0x80)) {
            uint16_t v = rtl_read2(USB_FW_CTRL, MCU_USB);
            v = (uint16_t)((v | FLOW_CTRL_PATCH_OPT | 0x0100) & ~0x08);
            rtl_write2(USB_FW_CTRL, MCU_USB, v);
        }
        rtl_set2(USB_FW_TASK, MCU_USB, FC_PATCH_TASK);
    }

    uint16_t es = rtl_read2(PLA_EXTRA_STATUS, MCU_PLA);
    es = (uint16_t)(rtl_link_up_now() ? (es | CUR_LINK_OK) : (es & ~CUR_LINK_OK));
    rtl_write2(PLA_EXTRA_STATUS, MCU_PLA, es | POLL_LINK_CHG);

    // MAC clock speed-down.
    rtl_write2(PLA_MAC_PWR_CTRL, MCU_PLA, 0x0403);
    uint16_t pc2 = rtl_read2(PLA_MAC_PWR_CTRL2, MCU_PLA);
    rtl_write2(PLA_MAC_PWR_CTRL2, MCU_PLA, (uint32_t)((pc2 & ~0xFF) | MAC_CLK_SPDWN_EN | 0x03));
    rtl_clr2(PLA_MAC_PWR_CTRL3, MCU_PLA, PLA_MCU_SPDWN_EN);

    // Aggregation OFF (the reference clears RX_AGG_DISABLE here).
    rtl_set2(USB_USB_CTRL, MCU_USB, RX_AGG_DISABLE);
    rtl_clr2(USB_USB_CTRL, MCU_USB, RX_ZERO_EN);
    if (!b) rtl_set1(0xD4B4, MCU_USB, 0x02);

    rtl_set2(PLA_RSTTALLY, MCU_USB, TALLY_RESET);

    nic_reset(d);
}

// ure_init()'s thresholds for this part: an early-aggregation timer and
// size (from OUR buffer, not the reference's 48 KiB), the receive DMA
// ownership handshake, and on the B part a bounce of the firmware's
// flow-control task.
static void rtl8156_start(struct rtl_usb *d) {
    rtl_write2(USB_RX_EARLY_AGG, MCU_USB, 80);
    rtl_write2(USB_RX_EXTRA_AGG_TMR, MCU_USB, 1875);
    rtl_write2(USB_RX_EARLY_SIZE, MCU_USB,
               (RTL_RX_BUF - (RTL_FRAMELEN + RXD_LEN + RXD_ALIGN)) / 8);
    rtl_write1(USB_UPT_RXDMA_OWN, MCU_USB, OWN_UPDATE | OWN_CLEAR);
    if (is_b(d->version)) {
        rtl_clr2(USB_FW_TASK, MCU_USB, FC_PATCH_TASK);
        rtl_wait_ms(2);
        rtl_set2(USB_FW_TASK, MCU_USB, FC_PATCH_TASK);
    }
}

// Autonegotiate everything up to 2.5G full duplex, with flow control.
// The 2.5G bit is not an MII register: it lives beside them in OCP
// space, which is why the 8153's "leave the advertisement to autoload"
// is not enough here.
static void rtl8156_advertise(struct rtl_usb *d) {
    (void)d;
    rtl_ocp_write(OCP_MII_ANAR, ANAR_TX_FD | ANAR_TX | ANAR_10_FD | ANAR_10 |
                                ANAR_PAUSE_ASYM | ANAR_FC);
    rtl_ocp_write(OCP_MII_GTCR, GTCR_ADV_1000TFDX | GTCR_ADV_1000THDX);
    rtl_ocp_write(OCP_ADV_2500, (uint16_t)(rtl_ocp_read(OCP_ADV_2500) | ADV_2500TFDX));
    rtl_ocp_write(OCP_BASE_MII, BMCR_AUTOEN | BMCR_STARTNEG);
}

// ure_link_state(): on every link-up, receive and transmit are enabled
// again and a MAC power bit follows the negotiated speed.
static void rtl8156_link_changed(struct rtl_usb *d, int up, uint32_t bps) {
    (void)d;
    if (!up) return;
    rtl_set1(PLA_CR, MCU_PLA, CR_RE | CR_TE);
    if (bps == 2500000000u) rtl_clr2(PLA_MAC_PWR_CTRL4, MCU_PLA, 0x0040);
    else                    rtl_set2(PLA_MAC_PWR_CTRL4, MCU_PLA, 0x0040);
}

const struct rtl_usb_ops rtl8156_ops = {
    .name      = "r8156",
    .match     = rtl8156_match,
    .init      = rtl8156_init,
    .start     = rtl8156_start,
    .advertise = rtl8156_advertise,
    .link_changed = rtl8156_link_changed,
};
