// RTL8152/RTL8153 "A" -- the chip file. The transport is rtl_usb.c;
// this is ure(4)'s ure_rtl8153_init() and ure_rtl8152_nic_reset(),
// stepping-conditionals included, plus the thresholds ure_init() sets
// for this part. Most of the init is power management; what it buys is
// a chip that stays out of the low-power states its firmware would
// otherwise enter behind us.
#include "rtl_usb.h"
#include "xhci_regs.h"
#include "klog.h"
#include "driver.h"

DRIVER_DECLARE("r8153", "net", "Realtek RTL8152/8153 USB Ethernet");

static int rtl8153_match(uint16_t v) {
    return v == VER_5C00 || v == VER_5C10 || v == VER_5C20 || v == VER_5C30;
}

// receiver, take the chip out of OOB (where the firmware, not the host,
// owns the link), rebuild the FIFO link list, then set the thresholds.
static void nic_reset(struct rtl_usb *d) {
    rtl_set2(PLA_MISC_1, MCU_PLA, RXDY_GATED_EN);
    rtl_disable_teredo(0);
    rtl_clr4(PLA_RCR, MCU_PLA, RCR_ACPT_ALL);
    rtl_hw_reset();
    rtl_write1(PLA_CR, MCU_PLA, 0);

    rtl_clr1(PLA_OOB_CTRL, MCU_PLA, NOW_IS_OOB);
    rtl_clr2(PLA_SFF_STS_7, MCU_PLA, MCU_BORW_EN);
    for (int i = 0; i < TIMEOUT_MS / 10; i++) {
        if (rtl_read1(PLA_OOB_CTRL, MCU_PLA) & LINK_LIST_READY) break;
        rtl_wait_ms(10);
    }
    rtl_set2(PLA_SFF_STS_7, MCU_PLA, RE_INIT_LL);
    for (int i = 0; i < TIMEOUT_MS / 10; i++) {
        if (rtl_read1(PLA_OOB_CTRL, MCU_PLA) & LINK_LIST_READY) break;
        rtl_wait_ms(10);
    }

    rtl_clr2(PLA_CPCR, MCU_PLA, CPCR_RX_VLAN);
    rtl_set2(PLA_TCR0, MCU_PLA, TCR0_AUTO_FIFO);

    rtl_write4(PLA_RXFIFO_CTRL0, MCU_PLA, RXFIFO_THR1_NORMAL);
    int full = (d->speed == XHCI_SPEED_FULL);
    rtl_write4(PLA_RXFIFO_CTRL1, MCU_PLA, full ? RXFIFO_THR2_FULL : RXFIFO_THR2_HIGH);
    rtl_write4(PLA_RXFIFO_CTRL2, MCU_PLA, full ? RXFIFO_THR3_FULL : RXFIFO_THR3_HIGH);
    rtl_write4(PLA_TXFIFO_CTRL, MCU_PLA, TXFIFO_THR_NORMAL);
}


// ure(4)'s ure_rtl8153_init(), stepping-conditionals included. Most of
// it is power management; what it buys is a chip that stays out of the
// low-power states its firmware would otherwise enter behind us.
static void rtl8153_init(struct rtl_usb *d) {
    uint16_t v = d->version;

    rtl_disable_aldps();
    rtl_tolerance(0x00);

    for (int i = 0; i < TIMEOUT_MS / 10; i++) {
        if (rtl_read2(PLA_BOOT_CTRL, MCU_PLA) & AUTOLOAD_DONE) break;
        rtl_wait_ms(10);
    }
    for (int i = 0; i < TIMEOUT_MS / 10; i++) {
        uint16_t st = rtl_ocp_read(OCP_PHY_STATUS) & PHY_STAT_MASK;
        if (st == PHY_STAT_LAN_ON || st == PHY_STAT_PWRDN) break;
        rtl_wait_ms(10);
    }

    rtl_clr2(USB_U2P3_CTRL, MCU_USB, U2P3_ENABLE);

    if (v == VER_5C10) {
        uint16_t val = rtl_read2(USB_SSPHYLINK2, MCU_USB);
        val = (uint16_t)((val & ~PWD_DN_SCALE_MASK) | (96 << 1));
        rtl_write2(USB_SSPHYLINK2, MCU_USB, val);
        rtl_set1(USB_USB2PHY, MCU_USB, USB2PHY_L1 | USB2PHY_SUSPEND);
    } else if (v == VER_5C20) {
        rtl_clr1(PLA_DMY_REG0, MCU_PLA, ECM_ALDPS);
    }

    if (v == VER_5C20 || v == VER_5C30) {
        uint8_t val = rtl_read1(USB_CSR_DUMMY1, MCU_USB);
        if (rtl_read2(USB_BURST_SIZE, MCU_USB) == 0) val &= (uint8_t)~DYNAMIC_BURST;
        else                                         val |= DYNAMIC_BURST;
        rtl_write1(USB_CSR_DUMMY1, MCU_USB, val);
    }

    rtl_set1(USB_CSR_DUMMY2, MCU_USB, EP4_FULL_FC);
    rtl_clr2(USB_WDT11_CTRL, MCU_USB, TIMER11_EN);
    rtl_clr2(PLA_LED_FEATURE, MCU_PLA, LED_MODE_MASK);

    uint16_t lpm = (v == VER_5C10 && d->speed != XHCI_SPEED_SUPER)
                       ? LPM_TIMER_500MS : LPM_TIMER_500US;
    rtl_write1(USB_LPM_CTRL, MCU_USB, lpm | FIFO_EMPTY_1FB | ROK_EXIT_LPM);

    uint16_t afe = rtl_read2(USB_AFE_CTRL2, MCU_USB);
    afe = (uint16_t)((afe & ~SEN_VAL_MASK) | SEN_VAL_NORMAL | SEL_RXIDLE);
    rtl_write2(USB_AFE_CTRL2, MCU_USB, afe);

    rtl_write2(USB_CONNECT_TIMER, MCU_USB, 0x0001);
    rtl_clr2(USB_POWER_CUT, MCU_USB, PWR_EN | PHASE2_EN);
    rtl_clr2(USB_MISC_0, MCU_USB, PCUT_STATUS);
    rtl_tolerance(0xFF);

    rtl_write2(PLA_MAC_PWR_CTRL,  MCU_PLA, ALDPS_SPDWN_RATIO);
    rtl_write2(PLA_MAC_PWR_CTRL2, MCU_PLA, EEE_SPDWN_RATIO);
    rtl_write2(PLA_MAC_PWR_CTRL3, MCU_PLA,
               PKT_AVAIL_SPDWN_EN | SUSPEND_SPDWN_EN | U1U2_SPDWN_EN | L1_SPDWN_EN);
    rtl_write2(PLA_MAC_PWR_CTRL4, MCU_PLA,
               PWRSAVE_SPDWN_EN | RXDV_SPDWN_EN | TX10MIDLE_EN | TP100_SPDWN_EN |
               TP500_SPDWN_EN | TP1000_SPDWN_EN | EEE_SPDWN_EN);

    if (v == VER_5C00 || v == VER_5C10) rtl_clr2(USB_U2P3_CTRL, MCU_USB, U2P3_ENABLE);
    else                                rtl_set2(USB_U2P3_CTRL, MCU_USB, U2P3_ENABLE);

    rtl_tolerance(0x00);
    rtl_disable_aldps();

    if (v == VER_5C00 || v == VER_5C10 || v == VER_5C20)
        rtl_ocp_write(OCP_ADC_CFG, CKADSEL_L | ADC_EN | EN_EMI_L);
    if (v == VER_5C00)
        rtl_ocp_write(OCP_EEE_CFG, (uint16_t)(rtl_ocp_read(OCP_EEE_CFG) & ~CTAP_SHORT_EN));

    rtl_ocp_write(OCP_POWER_CFG, (uint16_t)(rtl_ocp_read(OCP_POWER_CFG) | EEE_CLKDIV_EN));
    rtl_ocp_write(OCP_DOWN_SPEED, (uint16_t)(rtl_ocp_read(OCP_DOWN_SPEED) | EN_10M_BGOFF));
    rtl_ocp_write(OCP_POWER_CFG, (uint16_t)(rtl_ocp_read(OCP_POWER_CFG) | EN_10M_PLLOFF));
    rtl_sram_write(SRAM_IMPEDANCE, 0x0B13);
    rtl_set2(PLA_PHY_PWR, MCU_PLA, PFM_PWM_SWITCH);
    rtl_sram_write(SRAM_LPF_CFG, 0xF70F);
    rtl_sram_write(SRAM_10M_AMP1, 0x00AF);
    rtl_sram_write(SRAM_10M_AMP2, 0x0208);

    nic_reset(d);

    // RECEIVE AGGREGATION STAYS OFF, where ure(4) turns it on. One frame
    // per transfer is what makes a first bring-up legible: with it on, a
    // bug in the descriptor walk and a dead receive path look the same.
    // The walk handles a packed transfer anyway, so this costs only
    // throughput.
    rtl_set2(USB_USB_CTRL, MCU_USB, RX_AGG_DISABLE);
    rtl_clr2(USB_USB_CTRL, MCU_USB, RX_ZERO_EN);

    if (v == VER_5C00 || v == VER_5C10) rtl_clr2(USB_U2P3_CTRL, MCU_USB, U2P3_ENABLE);
    else                                rtl_set2(USB_U2P3_CTRL, MCU_USB, U2P3_ENABLE);
    rtl_tolerance(0xFF);
}

// ure_init()'s thresholds for this part. How much room the host buffer
// has left before the device should stop filling it is sized from OUR
// buffer, not ure(4)'s 32 KiB, or the device would happily overrun a
// 4 KiB one.
static void rtl8153_start(struct rtl_usb *d) {
    // How much room the host buffer has left before the device should
    // stop filling it. Sized from OUR buffer, not ure(4)'s 32 KiB, or
    // the device would happily overrun a 4 KiB one.
    uint32_t coalesce = d->speed == XHCI_SPEED_SUPER ? COALESCE_SUPER
                      : d->speed == XHCI_SPEED_HIGH  ? COALESCE_HIGH
                                                          : COALESCE_SLOW;
    rtl_write2(USB_RX_EARLY_AGG, MCU_USB, coalesce / 8);
    rtl_write2(USB_RX_EARLY_SIZE, MCU_USB,
               (RTL_RX_BUF - (RTL_FRAMELEN + RXD_LEN + RXD_ALIGN)) / 4);

    rtl_write1(USB_TX_AGG, MCU_USB, TX_AGG_MAX_THRESHOLD);
    rtl_write4(USB_RX_BUF_TH, MCU_USB,
               d->speed == XHCI_SPEED_SUPER ? RX_THR_SUPER : RX_THR_HIGH);
    rtl_write4(USB_TX_DMA, MCU_USB, TEST_MODE_DISABLE | TX_SIZE_ADJUST1);

    rtl_clr2(USB_UPS_CTRL, MCU_USB, UPS_POWER_CUT);
    rtl_clr2(USB_PM_CTRL_STAT, MCU_USB, RESUME_INDICATE);
}

// Restart autonegotiation without touching the advertisement, which
// the chip's own autoload has already set to everything it can do.
static void rtl8153_advertise(struct rtl_usb *d) {
    (void)d;
    rtl_ocp_write(OCP_BASE_MII, BMCR_AUTOEN | BMCR_STARTNEG);
}

const struct rtl_usb_ops rtl8153_ops = {
    .name      = "r8153",
    .match     = rtl8153_match,
    .init      = rtl8153_init,
    .start     = rtl8153_start,
    .advertise = rtl8153_advertise,
    .link_changed = 0,
};
