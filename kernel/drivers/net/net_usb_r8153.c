// RTL8152/RTL8153 USB Ethernet, the VENDOR protocol -- the adapter's
// first configuration, the one every other OS actually binds.
//
// WHY THIS EXISTS BESIDE net_usb_ecm.c. The TP-Link UE300 this was written
// for offers CDC-ECM as its SECOND configuration and receives nothing
// there; Linux's own cdc_ether receives nothing there either, which is
// what says the fault is the adapter's ECM and not either driver
// (docs/bugs.md). Its first configuration is Realtek's own, and that is
// the one with a receive path anybody has ever exercised.
//
// TWO THINGS DIFFER FROM A CLASS DRIVER. Registers are reached by
// VENDOR CONTROL TRANSFER -- four bytes at a time behind a byte-enable
// mask, because the chip is on the far side of USB and there is no MMIO
// window to map. And A BULK TRANSFER IS NOT A FRAME: transmit prepends
// an 8-byte descriptor, receive returns a 24-byte descriptor per frame
// padded to 8, and one transfer may carry several frames.
//
// ADAPTED FROM FreeBSD's ure(4) -- sys/dev/usb/net/if_ure.c and
// if_urereg.h, BSD-2-clause, Kevin Lo. The register map and the order
// of the init sequence are its; the notice is in LICENSE. Linux's
// r8152.c is GPL-2.0 and was deliberately not consulted.
#include "usb.h"
#include "xhci.h"
#include "xhci_regs.h"
#include "netdev.h"
#include "pmm.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"
#include "timer.h"
#include "errno.h"
#include "driver.h" // driver_bound() -- `lsdrv`

DRIVER_DECLARE("r8153", "net", "Realtek RTL8153 USB Ethernet");

// --- the register interface -------------------------------------------
//
// bRequest 5 both ways; the direction is in bmRequestType. wValue is the
// register, wIndex is the MCU block ORed with a byte-enable mask, and a
// transfer is always four bytes even for a one-byte register.
#define REQ_REGS          0x05
#define TYPE_IN_VENDOR    0xC0
#define TYPE_OUT_VENDOR   0x40

#define MCU_PLA           0x0100
#define MCU_USB           0x0000
#define BYTE_EN_BYTE      0x11
#define BYTE_EN_WORD      0x33
#define BYTE_EN_DWORD     0xFF
#define BYTE_EN_SIX       0x3F

// PLA block
#define PLA_IDR           0xC000
#define PLA_RCR           0xC010
#define PLA_RMS           0xC016
#define PLA_RXFIFO_CTRL0  0xC0A0
#define PLA_RXFIFO_CTRL1  0xC0A4
#define PLA_RXFIFO_CTRL2  0xC0A8
#define PLA_DMY_REG0      0xC0B0
#define PLA_FMC           0xC0B4
#define PLA_TEREDO_CFG    0xC0BC
#define PLA_MAR0          0xCD00
#define PLA_MAR4          0xCD04
#define PLA_BACKUP        0xD000
#define PLA_TEREDO_TIMER  0xD2CC
#define PLA_REALWOW_TIMER 0xD2E8
#define PLA_LED_FEATURE   0xDD92
#define PLA_BOOT_CTRL     0xE004
#define PLA_GPHY_INTR_IMR 0xE022
#define PLA_MAC_PWR_CTRL  0xE0C0
#define PLA_MAC_PWR_CTRL2 0xE0CA
#define PLA_MAC_PWR_CTRL3 0xE0CC
#define PLA_MAC_PWR_CTRL4 0xE0CE
#define PLA_WDT6_CTRL     0xE428
#define PLA_TCR0          0xE610
#define PLA_TCR1          0xE612
#define PLA_TXFIFO_CTRL   0xE618
#define PLA_CR            0xE813
#define PLA_CRWECR        0xE81C
#define PLA_PHY_PWR       0xE84C
#define PLA_OOB_CTRL      0xE84F
#define PLA_CPCR          0xE854
#define PLA_MISC_1        0xE85A
#define PLA_OCP_GPHY_BASE 0xE86C
#define PLA_SFF_STS_7     0xE8DE
#define PLA_PHYSTATUS     0xE908

// USB block
#define USB_USB2PHY       0xB41E
#define USB_SSPHYLINK2    0xB428
#define USB_U2P3_CTRL     0xB460
#define USB_CSR_DUMMY1    0xB464
#define USB_CSR_DUMMY2    0xB466
#define USB_CONNECT_TIMER 0xCBF8
#define USB_BURST_SIZE    0xCFC0
#define USB_USB_CTRL      0xD406
#define USB_TX_AGG        0xD40A
#define USB_RX_BUF_TH     0xD40C
#define USB_LPM_CTRL      0xD41A
#define USB_RX_EARLY_AGG  0xD42C
#define USB_RX_EARLY_SIZE 0xD42E
#define USB_PM_CTRL_STAT  0xD432
#define USB_TX_DMA        0xD434
#define USB_TOLERANCE     0xD490
#define USB_UPS_CTRL      0xD800
#define USB_POWER_CUT     0xD80A
#define USB_MISC_0        0xD81A
#define USB_AFE_CTRL2     0xD824
#define USB_WDT11_CTRL    0xE43C

// OCP (the PHY, reached through PLA_OCP_GPHY_BASE)
#define OCP_ALDPS_CONFIG  0x2010
#define OCP_BASE_MII      0xA400
#define OCP_PHY_STATUS    0xA420
#define OCP_POWER_CFG     0xA430
#define OCP_EEE_CFG       0xA432
#define OCP_SRAM_ADDR     0xA436
#define OCP_SRAM_DATA     0xA438
#define OCP_DOWN_SPEED    0xA442
#define OCP_ADC_CFG       0xBC06
#define SRAM_LPF_CFG      0x8012
#define SRAM_10M_AMP1     0x8080
#define SRAM_10M_AMP2     0x8082
#define SRAM_IMPEDANCE    0x8084

// bits
#define RCR_AAP           0x00000001u
#define RCR_APM           0x00000002u
#define RCR_AM            0x00000004u
#define RCR_AB            0x00000008u
#define RCR_ACPT_ALL      (RCR_AAP | RCR_APM | RCR_AM | RCR_AB)
#define RXFIFO_THR1_NORMAL 0x00080002u
#define RXFIFO_THR2_HIGH  0x00000038u
#define RXFIFO_THR2_FULL  0x00000060u
#define RXFIFO_THR3_HIGH  0x00000048u
#define RXFIFO_THR3_FULL  0x00000078u
#define TXFIFO_THR_NORMAL 0x00400008u
#define ECM_ALDPS         0x0002
#define FMC_FCR_MCU_EN    0x0001
#define WDT6_SET_MODE     0x0010
#define TCR0_AUTO_FIFO    0x0080
#define VERSION_MASK      0x7CF0
#define CR_RST            0x10
#define CR_RE             0x08
#define CR_TE             0x04
#define CRWECR_NORMAL     0x00
#define CRWECR_CONFIG     0xC0
#define NOW_IS_OOB        0x80
#define LINK_LIST_READY   0x02
#define RXDY_GATED_EN     0x0008
#define RE_INIT_LL        0x8000
#define MCU_BORW_EN       0x4000
#define CPCR_RX_VLAN      0x0040
#define TEREDO_SEL        0x8000
#define TEREDO_RS_EVENT_MASK 0x00FE
#define OOB_TEREDO_EN     0x0001
#define LED_MODE_MASK     0x0700
#define TX_10M_IDLE_EN    0x0080
#define PFM_PWM_SWITCH    0x0040
#define ALDPS_SPDWN_RATIO 0x0F87
#define EEE_SPDWN_RATIO   0x8007
#define PKT_AVAIL_SPDWN_EN 0x0100
#define SUSPEND_SPDWN_EN  0x0004
#define U1U2_SPDWN_EN     0x0002
#define L1_SPDWN_EN       0x0001
#define PWRSAVE_SPDWN_EN  0x1000
#define RXDV_SPDWN_EN     0x0800
#define TX10MIDLE_EN      0x0100
#define TP100_SPDWN_EN    0x0020
#define TP500_SPDWN_EN    0x0010
#define TP1000_SPDWN_EN   0x0008
#define EEE_SPDWN_EN      0x0001
#define AUTOLOAD_DONE     0x0002
#define PHYSTATUS_LINK    0x0002
#define PHYSTATUS_10MBPS  0x0004
#define PHYSTATUS_100MBPS 0x0008
#define PHYSTATUS_1000MBPS 0x0010
#define USB2PHY_SUSPEND   0x0001
#define USB2PHY_L1        0x0002
#define PWD_DN_SCALE_MASK 0x3FFE
#define DYNAMIC_BURST     0x0001
#define EP4_FULL_FC       0x0001
#define TIMER11_EN        0x0001
#define TX_AGG_MAX_THRESHOLD 0x03
#define RX_THR_HIGH       0x7A120180u
#define RX_THR_SUPER      0x0C350180u
#define TEST_MODE_DISABLE 0x00000001u
#define TX_SIZE_ADJUST1   0x00000100u
#define UPS_POWER_CUT     0x0100
#define RESUME_INDICATE   0x0001
#define RX_AGG_DISABLE    0x0010
#define RX_ZERO_EN        0x0080
#define U2P3_ENABLE       0x0001
#define PWR_EN            0x0001
#define PHASE2_EN         0x0008
#define PCUT_STATUS       0x0001
#define FIFO_EMPTY_1FB    0x30
#define LPM_TIMER_500US   0x0C
#define LPM_TIMER_500MS   0x04
#define ROK_EXIT_LPM      0x02
#define SEN_VAL_MASK      0xF800
#define SEN_VAL_NORMAL    0xA000
#define SEL_RXIDLE        0x0100
#define ENPWRSAVE         0x8000
#define ENPDNPS           0x0200
#define LINKENA           0x0100
#define DIS_SDSAVE        0x0010
#define EN_ALDPS          0x0004
#define EEE_CLKDIV_EN     0x8000
#define EN_10M_PLLOFF     0x0001
#define EN_10M_BGOFF      0x0080
#define CTAP_SHORT_EN     0x0040
#define CKADSEL_L         0x0100
#define ADC_EN            0x0080
#define EN_EMI_L          0x0040
#define PHY_STAT_MASK     0x0007
#define PHY_STAT_LAN_ON   3
#define PHY_STAT_PWRDN    5
#define COALESCE_SUPER    85000u
#define COALESCE_HIGH     250000u
#define COALESCE_SLOW     524280u

// Chip versions, from PLA_TCR1. Only the RTL8153 "A" steppings are
// driven: the B/8156 parts want a different init and none is here to
// test it against, so an unrecognised version is REFUSED rather than
// driven with the wrong sequence.
#define VER_4C00          0x4C00   // RTL8152
#define VER_4C10          0x4C10   // RTL8152
#define VER_5C00          0x5C00
#define VER_5C10          0x5C10
#define VER_5C20          0x5C20
#define VER_5C30          0x5C30

// Framing. The transmit descriptor is two little-endian words, the
// receive one six; a received length INCLUDES the Ethernet CRC.
#define TXD_LEN           8
#define RXD_LEN           24
#define RXD_ALIGN         8
#define TXPKT_FS          (1u << 31)
#define TXPKT_LS          (1u << 30)
#define TXPKT_LEN_MASK    0xFFFFu
#define RXPKT_LEN_MASK    0x7FFFu
#define ETH_CRC_LEN       4

#define R8153_FRAMELEN    (NET_MTU + 14 + 4 + 4)   // + VLAN, as ure sizes it
#define R8153_BUFS        4
#define R8153_RX_BUF      4096
#define R8153_TX_BUF      2048

#define TIMEOUT_MS        2000

struct r8153_dev {
    uint8_t in_use;
    uint8_t slot;
    uint8_t ep_in, ep_out;
    uint16_t mps;
    uint16_t version;
    uint8_t speed;                 // XHCI_SPEED_*

    uint64_t mem_phys;
    uint32_t mem_pages;
    uint8_t *tx[R8153_BUFS];
    uint64_t rx_phys[R8153_BUFS], tx_phys[R8153_BUFS];
    // Written by the completion callback (the event drain) and read by
    // transmit(), so volatile -- a stale busy flag either drops a frame
    // that could have gone or overwrites one still on the wire.
    volatile uint8_t tx_busy[R8153_BUFS];
    uint8_t tx_next;

    uint64_t link_checked;         // pit_ticks() of the last PHY read

    struct net_device dev;
};

static struct r8153_dev g_r8153;

// The DMA target for every register access. Static rather than on a
// stack: a ring-0 stack is 16 KiB behind a guard page with a 1 KiB frame
// budget, and this must be identity-mapped for the controller.
static uint8_t g_reg_buf[8] __attribute__((aligned(64)));

static void wait_ms(uint32_t ms) {
    uint64_t start = pit_ticks();
    uint64_t ticks = (ms + 9) / 10 + 1;
    while (pit_ticks() - start < ticks) { }
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void st32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static int reg_read_mem(uint16_t addr, uint16_t index, void *buf, uint16_t len) {
    uint8_t setup[8] = { TYPE_IN_VENDOR, REQ_REGS,
                         (uint8_t)addr, (uint8_t)(addr >> 8),
                         (uint8_t)index, (uint8_t)(index >> 8),
                         (uint8_t)len, (uint8_t)(len >> 8) };
    return xhci_control(g_r8153.slot, setup, buf, len, 1);
}

static int reg_write_mem(uint16_t addr, uint16_t index, void *buf, uint16_t len) {
    uint8_t setup[8] = { TYPE_OUT_VENDOR, REQ_REGS,
                         (uint8_t)addr, (uint8_t)(addr >> 8),
                         (uint8_t)index, (uint8_t)(index >> 8),
                         (uint8_t)len, (uint8_t)(len >> 8) };
    return xhci_control(g_r8153.slot, setup, buf, len, 0);
}

// A register narrower than 32 bits is read as the aligned dword it sits
// in and shifted out; written with a byte-enable mask that names the
// bytes to keep. Getting the shift and the mask out of step writes the
// right value into the wrong half of the register, which reads back
// perfectly and does nothing.
static uint32_t reg_read4(uint16_t reg, uint16_t index) {
    if (reg_read_mem(reg, index, g_reg_buf, 4) < 4) return 0;
    return le32(g_reg_buf);
}

static uint16_t reg_read2(uint16_t reg, uint16_t index) {
    uint8_t shift = (uint8_t)((reg & 2) << 3);
    uint32_t v = reg_read4((uint16_t)(reg & ~3u), index);
    return (uint16_t)((v >> shift) & 0xFFFF);
}

static uint8_t reg_read1(uint16_t reg, uint16_t index) {
    uint8_t shift = (uint8_t)((reg & 3) << 3);
    uint32_t v = reg_read4((uint16_t)(reg & ~3u), index);
    return (uint8_t)((v >> shift) & 0xFF);
}

static int reg_write4(uint16_t reg, uint16_t index, uint32_t val) {
    st32(g_reg_buf, val);
    return reg_write_mem(reg, (uint16_t)(index | BYTE_EN_DWORD), g_reg_buf, 4);
}

static int reg_write2(uint16_t reg, uint16_t index, uint32_t val) {
    uint16_t byen = BYTE_EN_WORD;
    uint8_t shift = (uint8_t)(reg & 2);
    val &= 0xFFFF;
    if (shift) { byen = (uint16_t)(byen << shift); val <<= (shift << 3); reg &= (uint16_t)~3u; }
    st32(g_reg_buf, val);
    return reg_write_mem(reg, (uint16_t)(index | byen), g_reg_buf, 4);
}

static int reg_write1(uint16_t reg, uint16_t index, uint32_t val) {
    uint16_t byen = BYTE_EN_BYTE;
    uint8_t shift = (uint8_t)(reg & 3);
    val &= 0xFF;
    if (shift) { byen = (uint16_t)(byen << shift); val <<= (shift << 3); reg &= (uint16_t)~3u; }
    st32(g_reg_buf, val);
    return reg_write_mem(reg, (uint16_t)(index | byen), g_reg_buf, 4);
}

static void set2(uint16_t reg, uint16_t index, uint16_t bits) {
    reg_write2(reg, index, (uint32_t)(reg_read2(reg, index) | bits));
}
static void clr2(uint16_t reg, uint16_t index, uint16_t bits) {
    reg_write2(reg, index, (uint32_t)(reg_read2(reg, index) & ~bits));
}
static void set1(uint16_t reg, uint16_t index, uint8_t bits) {
    reg_write1(reg, index, (uint32_t)(reg_read1(reg, index) | bits));
}
static void clr1(uint16_t reg, uint16_t index, uint8_t bits) {
    reg_write1(reg, index, (uint32_t)(reg_read1(reg, index) & ~bits));
}
static void clr4(uint16_t reg, uint16_t index, uint32_t bits) {
    reg_write4(reg, index, reg_read4(reg, index) & ~bits);
}

// The PHY lives behind a window: the high nibble of the address goes in
// PLA_OCP_GPHY_BASE and the rest becomes a PLA register at 0xb000.
static uint16_t ocp_read(uint16_t addr) {
    reg_write2(PLA_OCP_GPHY_BASE, MCU_PLA, addr & 0xF000);
    return reg_read2((uint16_t)((addr & 0x0FFF) | 0xB000), MCU_PLA);
}

static void ocp_write(uint16_t addr, uint16_t data) {
    reg_write2(PLA_OCP_GPHY_BASE, MCU_PLA, addr & 0xF000);
    reg_write2((uint16_t)((addr & 0x0FFF) | 0xB000), MCU_PLA, data);
}

static void sram_write(uint16_t addr, uint16_t data) {
    ocp_write(OCP_SRAM_ADDR, addr);
    ocp_write(OCP_SRAM_DATA, data);
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

// --- init -------------------------------------------------------------

static void hw_reset(void) {
    reg_write1(PLA_CR, MCU_PLA, CR_RST);
    for (int i = 0; i < TIMEOUT_MS / 10; i++) {
        if (!(reg_read1(PLA_CR, MCU_PLA) & CR_RST)) return;
        wait_ms(10);
    }
    klog_write("r8153: reset never completed\n");
}

static void disable_teredo(void) {
    clr2(PLA_TEREDO_CFG, MCU_PLA, TEREDO_SEL | TEREDO_RS_EVENT_MASK | OOB_TEREDO_EN);
    reg_write2(PLA_WDT6_CTRL, MCU_PLA, WDT6_SET_MODE);
    reg_write2(PLA_REALWOW_TIMER, MCU_PLA, 0);
    reg_write4(PLA_TEREDO_TIMER, MCU_PLA, 0);
}

static void disable_aldps(void) {
    ocp_write(OCP_ALDPS_CONFIG, ENPDNPS | LINKENA | DIS_SDSAVE);
    for (int i = 0; i < 20; i++) {
        wait_ms(1);
        if (ocp_read(0xE000) & 0x0100) break;
    }
}

// The MAC-level reset ure(4) calls ure_rtl8152_nic_reset(): gate the
// receiver, take the chip out of OOB (where the firmware, not the host,
// owns the link), rebuild the FIFO link list, then set the thresholds.
static void nic_reset(void) {
    set2(PLA_MISC_1, MCU_PLA, RXDY_GATED_EN);
    disable_teredo();
    clr4(PLA_RCR, MCU_PLA, RCR_ACPT_ALL);
    hw_reset();
    reg_write1(PLA_CR, MCU_PLA, 0);

    clr1(PLA_OOB_CTRL, MCU_PLA, NOW_IS_OOB);
    clr2(PLA_SFF_STS_7, MCU_PLA, MCU_BORW_EN);
    for (int i = 0; i < TIMEOUT_MS / 10; i++) {
        if (reg_read1(PLA_OOB_CTRL, MCU_PLA) & LINK_LIST_READY) break;
        wait_ms(10);
    }
    set2(PLA_SFF_STS_7, MCU_PLA, RE_INIT_LL);
    for (int i = 0; i < TIMEOUT_MS / 10; i++) {
        if (reg_read1(PLA_OOB_CTRL, MCU_PLA) & LINK_LIST_READY) break;
        wait_ms(10);
    }

    clr2(PLA_CPCR, MCU_PLA, CPCR_RX_VLAN);
    set2(PLA_TCR0, MCU_PLA, TCR0_AUTO_FIFO);

    reg_write4(PLA_RXFIFO_CTRL0, MCU_PLA, RXFIFO_THR1_NORMAL);
    int full = (g_r8153.speed == XHCI_SPEED_FULL);
    reg_write4(PLA_RXFIFO_CTRL1, MCU_PLA, full ? RXFIFO_THR2_FULL : RXFIFO_THR2_HIGH);
    reg_write4(PLA_RXFIFO_CTRL2, MCU_PLA, full ? RXFIFO_THR3_FULL : RXFIFO_THR3_HIGH);
    reg_write4(PLA_TXFIFO_CTRL, MCU_PLA, TXFIFO_THR_NORMAL);
}

static void tolerance(uint8_t fill) {
    for (int i = 0; i < 8; i++) g_reg_buf[i] = fill;
    reg_write_mem(USB_TOLERANCE, MCU_USB | BYTE_EN_SIX, g_reg_buf, 8);
}

// ure(4)'s ure_rtl8153_init(), stepping-conditionals included. Most of
// it is power management; what it buys is a chip that stays out of the
// low-power states its firmware would otherwise enter behind us.
static void rtl8153_init(void) {
    uint16_t v = g_r8153.version;

    disable_aldps();
    tolerance(0x00);

    for (int i = 0; i < TIMEOUT_MS / 10; i++) {
        if (reg_read2(PLA_BOOT_CTRL, MCU_PLA) & AUTOLOAD_DONE) break;
        wait_ms(10);
    }
    for (int i = 0; i < TIMEOUT_MS / 10; i++) {
        uint16_t st = ocp_read(OCP_PHY_STATUS) & PHY_STAT_MASK;
        if (st == PHY_STAT_LAN_ON || st == PHY_STAT_PWRDN) break;
        wait_ms(10);
    }

    clr2(USB_U2P3_CTRL, MCU_USB, U2P3_ENABLE);

    if (v == VER_5C10) {
        uint16_t val = reg_read2(USB_SSPHYLINK2, MCU_USB);
        val = (uint16_t)((val & ~PWD_DN_SCALE_MASK) | (96 << 1));
        reg_write2(USB_SSPHYLINK2, MCU_USB, val);
        set1(USB_USB2PHY, MCU_USB, USB2PHY_L1 | USB2PHY_SUSPEND);
    } else if (v == VER_5C20) {
        clr1(PLA_DMY_REG0, MCU_PLA, ECM_ALDPS);
    }

    if (v == VER_5C20 || v == VER_5C30) {
        uint8_t val = reg_read1(USB_CSR_DUMMY1, MCU_USB);
        if (reg_read2(USB_BURST_SIZE, MCU_USB) == 0) val &= (uint8_t)~DYNAMIC_BURST;
        else                                         val |= DYNAMIC_BURST;
        reg_write1(USB_CSR_DUMMY1, MCU_USB, val);
    }

    set1(USB_CSR_DUMMY2, MCU_USB, EP4_FULL_FC);
    clr2(USB_WDT11_CTRL, MCU_USB, TIMER11_EN);
    clr2(PLA_LED_FEATURE, MCU_PLA, LED_MODE_MASK);

    uint16_t lpm = (v == VER_5C10 && g_r8153.speed != XHCI_SPEED_SUPER)
                       ? LPM_TIMER_500MS : LPM_TIMER_500US;
    reg_write1(USB_LPM_CTRL, MCU_USB, lpm | FIFO_EMPTY_1FB | ROK_EXIT_LPM);

    uint16_t afe = reg_read2(USB_AFE_CTRL2, MCU_USB);
    afe = (uint16_t)((afe & ~SEN_VAL_MASK) | SEN_VAL_NORMAL | SEL_RXIDLE);
    reg_write2(USB_AFE_CTRL2, MCU_USB, afe);

    reg_write2(USB_CONNECT_TIMER, MCU_USB, 0x0001);
    clr2(USB_POWER_CUT, MCU_USB, PWR_EN | PHASE2_EN);
    clr2(USB_MISC_0, MCU_USB, PCUT_STATUS);
    tolerance(0xFF);

    reg_write2(PLA_MAC_PWR_CTRL,  MCU_PLA, ALDPS_SPDWN_RATIO);
    reg_write2(PLA_MAC_PWR_CTRL2, MCU_PLA, EEE_SPDWN_RATIO);
    reg_write2(PLA_MAC_PWR_CTRL3, MCU_PLA,
               PKT_AVAIL_SPDWN_EN | SUSPEND_SPDWN_EN | U1U2_SPDWN_EN | L1_SPDWN_EN);
    reg_write2(PLA_MAC_PWR_CTRL4, MCU_PLA,
               PWRSAVE_SPDWN_EN | RXDV_SPDWN_EN | TX10MIDLE_EN | TP100_SPDWN_EN |
               TP500_SPDWN_EN | TP1000_SPDWN_EN | EEE_SPDWN_EN);

    if (v == VER_5C00 || v == VER_5C10) clr2(USB_U2P3_CTRL, MCU_USB, U2P3_ENABLE);
    else                                set2(USB_U2P3_CTRL, MCU_USB, U2P3_ENABLE);

    tolerance(0x00);
    disable_aldps();

    if (v == VER_5C00 || v == VER_5C10 || v == VER_5C20)
        ocp_write(OCP_ADC_CFG, CKADSEL_L | ADC_EN | EN_EMI_L);
    if (v == VER_5C00)
        ocp_write(OCP_EEE_CFG, (uint16_t)(ocp_read(OCP_EEE_CFG) & ~CTAP_SHORT_EN));

    ocp_write(OCP_POWER_CFG, (uint16_t)(ocp_read(OCP_POWER_CFG) | EEE_CLKDIV_EN));
    ocp_write(OCP_DOWN_SPEED, (uint16_t)(ocp_read(OCP_DOWN_SPEED) | EN_10M_BGOFF));
    ocp_write(OCP_POWER_CFG, (uint16_t)(ocp_read(OCP_POWER_CFG) | EN_10M_PLLOFF));
    sram_write(SRAM_IMPEDANCE, 0x0B13);
    set2(PLA_PHY_PWR, MCU_PLA, PFM_PWM_SWITCH);
    sram_write(SRAM_LPF_CFG, 0xF70F);
    sram_write(SRAM_10M_AMP1, 0x00AF);
    sram_write(SRAM_10M_AMP2, 0x0208);

    nic_reset();

    // RECEIVE AGGREGATION STAYS OFF, where ure(4) turns it on. One frame
    // per transfer is what makes a first bring-up legible: with it on, a
    // bug in the descriptor walk and a dead receive path look the same.
    // The walk handles a packed transfer anyway, so this costs only
    // throughput.
    set2(USB_USB_CTRL, MCU_USB, RX_AGG_DISABLE);
    clr2(USB_USB_CTRL, MCU_USB, RX_ZERO_EN);

    if (v == VER_5C00 || v == VER_5C10) clr2(USB_U2P3_CTRL, MCU_USB, U2P3_ENABLE);
    else                                set2(USB_U2P3_CTRL, MCU_USB, U2P3_ENABLE);
    tolerance(0xFF);
}

// ure(4)'s ure_init(): what the interface needs to actually pass
// traffic, as opposed to the chip bring-up above.
static void mac_start(const uint8_t mac[NET_MAC_LEN]) {
    hw_reset();

    reg_write1(PLA_CRWECR, MCU_PLA, CRWECR_CONFIG);
    for (int i = 0; i < NET_MAC_LEN; i++) g_reg_buf[i] = mac[i];
    g_reg_buf[6] = g_reg_buf[7] = 0;
    reg_write_mem(PLA_IDR, MCU_PLA | BYTE_EN_SIX, g_reg_buf, 8);
    reg_write1(PLA_CRWECR, MCU_PLA, CRWECR_NORMAL);

    // How much room the host buffer has left before the device should
    // stop filling it. Sized from OUR buffer, not ure(4)'s 32 KiB, or
    // the device would happily overrun a 4 KiB one.
    uint32_t coalesce = g_r8153.speed == XHCI_SPEED_SUPER ? COALESCE_SUPER
                      : g_r8153.speed == XHCI_SPEED_HIGH  ? COALESCE_HIGH
                                                          : COALESCE_SLOW;
    reg_write2(USB_RX_EARLY_AGG, MCU_USB, coalesce / 8);
    reg_write2(USB_RX_EARLY_SIZE, MCU_USB,
               (R8153_RX_BUF - (R8153_FRAMELEN + RXD_LEN + RXD_ALIGN)) / 4);

    reg_write1(USB_TX_AGG, MCU_USB, TX_AGG_MAX_THRESHOLD);
    reg_write4(USB_RX_BUF_TH, MCU_USB,
               g_r8153.speed == XHCI_SPEED_SUPER ? RX_THR_SUPER : RX_THR_HIGH);
    reg_write4(USB_TX_DMA, MCU_USB, TEST_MODE_DISABLE | TX_SIZE_ADJUST1);

    clr2(USB_UPS_CTRL, MCU_USB, UPS_POWER_CUT);
    clr2(USB_PM_CTRL_STAT, MCU_USB, RESUME_INDICATE);

    // ure(4) leaves this at the reset default on an 8153A; set it, since
    // an adapter that came out of OOB with a smaller one silently drops
    // full-size frames.
    reg_write2(PLA_RMS, MCU_PLA, R8153_FRAMELEN);

    clr2(PLA_FMC, MCU_PLA, FMC_FCR_MCU_EN);
    set2(PLA_FMC, MCU_PLA, FMC_FCR_MCU_EN);
    clr2(PLA_CPCR, MCU_PLA, CPCR_RX_VLAN);

    set1(PLA_CR, MCU_PLA, CR_RE | CR_TE);
    clr2(PLA_MISC_1, MCU_PLA, RXDY_GATED_EN);

    // Unicast to us plus broadcast, and every multicast group -- there
    // is no multicast membership list in this stack to filter against.
    reg_write4(PLA_MAR0, MCU_PLA, 0xFFFFFFFFu);
    reg_write4(PLA_MAR4, MCU_PLA, 0xFFFFFFFFu);
    reg_write4(PLA_RCR, MCU_PLA, RCR_APM | RCR_AB | RCR_AM);

    // Restart autonegotiation without touching the advertisement, which
    // the chip's own autoload has already set to everything it can do.
    ocp_write(OCP_BASE_MII, 0x1200);   // BMCR: autoneg enable + restart
}

// --- the data path ----------------------------------------------------

static void rx_done(void *ctx, uint64_t phys, uint32_t bytes, int ok) {
    struct r8153_dev *d = ctx;
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
    xhci_bulk_post(d->slot, d->ep_in, phys, R8153_RX_BUF);
}

static void tx_done(void *ctx, uint64_t phys, uint32_t bytes, int ok) {
    struct r8153_dev *d = ctx;
    (void)bytes; (void)ok;
    for (int i = 0; i < R8153_BUFS; i++)
        if (d->tx_phys[i] == phys) { d->tx_busy[i] = 0; return; }
}

static int r8153_transmit(struct net_device *dev, const void *frame, uint32_t len) {
    struct r8153_dev *d = dev->drv;
    if (!d->in_use) return -ENODEV;
    if (len > NET_FRAME_MAX) return -EINVAL;

    int slot = -1;
    for (int i = 0; i < R8153_BUFS; i++) {
        int at = (d->tx_next + i) % R8153_BUFS;
        if (!d->tx_busy[at]) { slot = at; break; }
    }
    if (slot < 0) return -ENOSPC;

    usb_r8153_tx_desc(d->tx[slot], len);
    k_memcpy(d->tx[slot] + TXD_LEN, frame, len);
    d->tx_busy[slot] = 1;
    d->tx_next = (uint8_t)((slot + 1) % R8153_BUFS);

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
static void r8153_poll(struct net_device *dev) {
    struct r8153_dev *d = dev->drv;
    if (!d->in_use) return;
    uint64_t now = pit_ticks();
    if (d->link_checked && now - d->link_checked < 100) return;
    d->link_checked = now;

    uint16_t st = reg_read2(PLA_PHYSTATUS, MCU_PLA);
    uint8_t up = (st & PHYSTATUS_LINK) ? 1 : 0;
    uint32_t bps = !up ? 0
                 : (st & PHYSTATUS_1000MBPS) ? 1000000000u
                 : (st & PHYSTATUS_100MBPS)  ? 100000000u
                 : (st & PHYSTATUS_10MBPS)   ? 10000000u : 0;

    if (!dev->link_known || dev->link_up != up || dev->link_bps != bps)
        klog_printf("usb-net: %s link %s%s\n", dev->name, up ? "UP" : "down",
                    bps == 1000000000u ? " 1000M" :
                    bps == 100000000u  ? " 100M"  :
                    bps == 10000000u   ? " 10M"   : "");
    dev->link_up = up;
    dev->link_bps = bps;
    dev->link_known = 1;
}

// --- binding ----------------------------------------------------------

// The devices this drives, by id. A vendor-specific configuration says
// nothing about what is behind it, so an unlisted device is left alone
// rather than probed -- the cost of being wrong is claiming somebody
// else's hardware and writing to its registers.
struct r8153_id { uint16_t vid, pid; };
static const struct r8153_id g_ids[] = {
    { 0x0BDA, 0x8152 },   // Realtek RTL8152
    { 0x0BDA, 0x8153 },   // Realtek RTL8153
    { 0x2357, 0x0601 },   // TP-Link UE300
};

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
    klog_printf("usb-net: r8153 %s %02x:%02x:%02x:%02x:%02x:%02x\n", what,
                m[0], m[1], m[2], m[3], m[4], m[5]);
}

int usb_r8153_bind(struct usb_device_info *info, const uint8_t *cfg,
                   uint32_t total) {
    if (!info || g_r8153.in_use) return 0;
    if (!usb_r8153_claims(info->vendor_id, info->product_id)) return 0;

    uint8_t ep_in, ep_out;
    uint16_t mps;
    if (!parse_endpoints(cfg, total, &ep_in, &ep_out, &mps)) {
        klog_printf("usb: slot %u: r8153 has no vendor bulk pair -- not bound\n",
                    info->slot);
        return 0;
    }

    struct r8153_dev *d = &g_r8153;
    k_memset(d, 0, sizeof *d);
    d->slot = info->slot;
    d->ep_in = ep_in;
    d->ep_out = ep_out;
    d->mps = mps;
    d->speed = info->speed;

    // THE FIRST CHECKPOINT, and the one that says whether any of the
    // rest is honest: a register layer that is wrong here reads back
    // zeroes and every write after it is silently discarded.
    d->version = (uint16_t)(reg_read2(PLA_TCR1, MCU_PLA) & VERSION_MASK);
    uint8_t idr[NET_MAC_LEN];
    if (reg_read_mem(PLA_IDR, MCU_PLA, g_reg_buf, 8) < 8) {
        klog_printf("usb: slot %u: r8153 register read failed -- not bound\n",
                    info->slot);
        return 0;
    }
    for (int i = 0; i < NET_MAC_LEN; i++) idr[i] = g_reg_buf[i];
    klog_printf("usb-net: r8153 version 0x%04x\n", d->version);
    log_mac("PLA_IDR", idr);

    switch (d->version) {
    case VER_5C00: case VER_5C10: case VER_5C20: case VER_5C30:
        break;
    default:
        // An RTL8153B, an RTL8156 or a chip whose registers did not read
        // wants a different init sequence, and driving it with this one
        // is worse than leaving the device unbound.
        klog_printf("usb: slot %u: r8153 version 0x%04x not driven "
                    "-- not bound\n", info->slot, d->version);
        return 0;
    }

    uint64_t need = (uint64_t)R8153_BUFS * (R8153_RX_BUF + R8153_TX_BUF);
    d->mem_pages = (uint32_t)((need + 4095) / 4096);
    d->mem_phys = pmm_alloc_contiguous(d->mem_pages);
    if (!d->mem_phys) {
        klog_write("usb: no contiguous frames for the r8153 buffers\n");
        return 0;
    }
    uint8_t *base = (uint8_t *)(uintptr_t)d->mem_phys;   // identity-mapped
    for (int i = 0; i < R8153_BUFS; i++) {
        d->rx_phys[i] = d->mem_phys + (uint64_t)i * R8153_RX_BUF;
        uint32_t tx_at = R8153_BUFS * R8153_RX_BUF + (uint32_t)i * R8153_TX_BUF;
        d->tx[i] = base + tx_at;
        d->tx_phys[i] = d->mem_phys + tx_at;
    }

    rtl8153_init();

    // The address the chip loaded from its own EEPROM. ure(4) takes it
    // from PLA_BACKUP on an 8153 and from PLA_IDR only on an 8152.
    uint8_t mac[NET_MAC_LEN];
    if (reg_read_mem(PLA_BACKUP, MCU_PLA, g_reg_buf, 8) < 8) {
        klog_printf("usb: slot %u: r8153 MAC unreadable -- not bound\n", info->slot);
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
        klog_printf("usb: slot %u: r8153 MAC is not a unicast address "
                    "-- not bound\n", info->slot);
        pmm_free_contiguous(d->mem_phys, d->mem_pages);
        return 0;
    }
    for (int i = 0; i < NET_MAC_LEN; i++) d->dev.mac[i] = mac[i];

    mac_start(mac);

    if (xhci_add_bulk(info->slot, ep_in, mps, rx_done, d) < 0 ||
        xhci_add_bulk(info->slot, ep_out, mps, tx_done, d) < 0) {
        klog_printf("usb: slot %u: r8153 bulk endpoints 0x%02x/0x%02x not "
                    "configured\n", info->slot, ep_in, ep_out);
        pmm_free_contiguous(d->mem_phys, d->mem_pages);
        return 0;
    }

    d->in_use = 1;                 // published before a completion can arrive
    d->dev.driver = "r8153";
    d->dev.mtu = NET_MTU;
    d->dev.transmit = r8153_transmit;
    d->dev.poll = r8153_poll;      // link state only; receive is pushed
    d->dev.drv = d;

    if (!net_register(&d->dev)) {
        d->in_use = 0;
        pmm_free_contiguous(d->mem_phys, d->mem_pages);
        return 0;
    }

    for (int i = 0; i < R8153_BUFS; i++)
        xhci_bulk_post(info->slot, ep_in, d->rx_phys[i], R8153_RX_BUF);

    info->bound = 1;
    klog_printf("usb: slot %u: bound as r8153, ep in 0x%02x out 0x%02x, "
                "%u B/packet\n", info->slot, ep_in, ep_out, mps);
    return 1;
}

void usb_r8153_unbind(uint8_t slot) {
    struct r8153_dev *d = &g_r8153;
    if (!d->in_use || d->slot != slot) return;
    // As with cdc-ecm: netdev.h has no way to unregister, so the device
    // stays listed and its transmit refuses. A card that cannot send is
    // a better answer than a dangling pointer.
    d->in_use = 0;
    klog_printf("usb-net: %s removed -- the interface stays listed and "
                "cannot send\n", d->dev.name);
}
