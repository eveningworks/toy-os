#ifndef RTL_USB_H
#define RTL_USB_H

#include <stdint.h>
#include "usb.h"
#include "netdev.h"

// Realtek USB Ethernet -- the family's TRANSPORT CORE (rtl_usb.c) and
// the contract a chip file fills in (rtl8153.c, rtl8156.c).
//
// What every RTL815x shares is everything but its bring-up: registers
// reached by vendor control transfer four bytes at a time, the PHY
// behind an OCP window, the same 8-byte transmit and 24-byte receive
// descriptors, the bulk pair, link polling. What differs per chip
// generation is the init and reset SEQUENCE, a handful of thresholds,
// and how autonegotiation is advertised. So the core owns the former
// and asks a `struct rtl_usb_ops` for the latter, chosen by the version
// register -- Linux's r8152.c has the same table (`rtl_ops`), FreeBSD's
// ure(4) spells it as flag branches in one file. A new chip is one more
// file and one more row in rtl_usb_chip_for(). The PCI Realtek parts
// (r8169.c) share NOTHING with this: another transport, another map.
//
// The registers are FreeBSD ure(4)'s (if_urereg.h, BSD-2-clause, Kevin
// Lo; notice in LICENSE). Linux's r8152.c is GPL-2.0 and was not
// consulted.

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

// Chip versions, from PLA_TCR1 & VERSION_MASK. Each chip file names the
// ones it drives in its `match`; a version no file claims is REFUSED
// rather than driven with a neighbour's sequence.
#define VER_4C00          0x4C00   // RTL8152
#define VER_4C10          0x4C10   // RTL8152
#define VER_5C00          0x5C00   // RTL8153 "A" steppings
#define VER_5C10          0x5C10
#define VER_5C20          0x5C20
#define VER_5C30          0x5C30
#define VER_6000          0x6000   // RTL8153B
#define VER_6010          0x6010
#define VER_7020          0x7020   // RTL8156
#define VER_7030          0x7030
#define VER_7400          0x7400   // RTL8156B
#define VER_7410          0x7410

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

#define RTL_FRAMELEN    (NET_MTU + 14 + 4 + 4)   // + VLAN, as ure sizes it
// SIXTEEN, not four. At 2.5 Gb/s a download's ACK stream refused a
// third of its transmits with four (every buffer still on the wire
// waiting for its completion), the peer read the silence as loss and
// retransmitted 40% of the data, and a long fetch wedged. Sixteen of
// each is 96 KiB of DMA32 memory, which is nothing.
#define RTL_BUFS          16
#define RTL_RX_BUF      4096
#define RTL_TX_BUF      2048

#define TIMEOUT_MS        2000

// The reference's unnamed registers, kept by address as it keeps them.
#define USB_LPM_CONFIG    0xCFD8
#define USB_MSC_TIMER     0xCBFC
#define USB_FW_CTRL       0xD334   // RTL8156B
#define USB_FC_TIMER      0xD340
#define USB_RX_EXTRA_AGG_TMR 0xD432 // the 8153B/8156 meaning of USB_PM_CTRL_STAT's address
#define USB_UPT_RXDMA_OWN 0xD437
#define USB_BMU_RESET     0xD4B0
#define USB_U1U2_TIMER    0xD4DA
#define USB_FW_TASK       0xD4E8
#define PLA_SUSPEND_FLAG  0xD38A   // USB block despite the name, as in ure(4)
#define PLA_INDICATE_FALG 0xD38C
#define PLA_EXTRA_STATUS  0xD398
#define PLA_MTPS          0xE615
#define PLA_RSTTALLY      0xE800
#define PLA_CONFIG34      0xE820
#define LPM_U1U2_EN       0x0001
#define UPS_EN            0x0010
#define USP_PREWAKE       0x0020
#define UPCOMING_RUNTIME_D3 0x01
#define LINK_CHG_EVENT    0x01
#define LINK_CHANGE_FLAG  0x0100
#define POLL_LINK_CHG     0x0001
#define CUR_LINK_OK       0x8000
#define LINK_OFF_WAKE_EN  0x0008
#define CTRL_TIMER_EN     0x8000
#define FLOW_CTRL_PATCH_OPT 0x01
#define FC_PATCH_TASK     0x0001
#define MAC_CLK_SPDWN_EN  0x8000
#define PLA_MCU_SPDWN_EN  0x4000
#define TALLY_RESET       0x0001
#define BMU_RESET_EP_IN   0x01
#define BMU_RESET_EP_OUT  0x02
#define OWN_UPDATE        0x01
#define OWN_CLEAR         0x02
#define MTPS_JUMBO        192
#define TXFIFO_THR_NORMAL2 0x01000008u
#define PHYSTATUS_FDX     0x0001
#define PHYSTATUS_2500MBPS 0x0400
#define PHY_STAT_EXT_INIT 2
#define OCP_MII_ANAR      (OCP_BASE_MII + 4 * 2)
#define OCP_MII_GTCR      (OCP_BASE_MII + 9 * 2)
#define OCP_ADV_2500      0xA5D4
#define ADV_2500TFDX      0x0080
#define BMCR_AUTOEN       0x1000
#define BMCR_PDOWN        0x0800
#define BMCR_STARTNEG     0x0200
#define ANAR_10           0x0020
#define ANAR_10_FD        0x0040
#define ANAR_TX           0x0080
#define ANAR_TX_FD        0x0100
#define ANAR_FC           0x0400
#define ANAR_PAUSE_ASYM   0x0800
#define GTCR_ADV_1000THDX 0x0100
#define GTCR_ADV_1000TFDX 0x0200

struct rtl_usb;

// What a chip file supplies. Every hook runs with the device's registers
// reachable and `d->version` known; `init` brings the chip out of its
// firmware's hands, `start` sets the receive thresholds before traffic
// is enabled, `advertise` restarts autonegotiation, and `link_changed`
// hears every transition the poll reports (2.5G parts want the MAC told).
struct rtl_usb_ops {
    const char *name;                    // the DRIVER_DECLARE name
    int  (*match)(uint16_t version);
    void (*init)(struct rtl_usb *d);
    void (*start)(struct rtl_usb *d);
    void (*advertise)(struct rtl_usb *d);
    void (*link_changed)(struct rtl_usb *d, int up, uint32_t bps);
};

struct rtl_usb {
    uint8_t in_use;
    uint8_t slot;
    uint8_t ep_in, ep_out;
    uint16_t mps;
    uint16_t version;
    uint8_t speed;                 // XHCI_SPEED_*
    const struct rtl_usb_ops *ops;

    uint64_t mem_phys;
    uint32_t mem_pages;
    uint8_t *tx[RTL_BUFS];
    uint64_t rx_phys[RTL_BUFS], tx_phys[RTL_BUFS];
    // Written by the completion callback (the event drain) and read by
    // transmit(), so volatile -- a stale busy flag either drops a frame
    // that could have gone or overwrites one still on the wire.
    volatile uint8_t tx_busy[RTL_BUFS];
    uint8_t tx_next;

    uint64_t link_checked;         // coarse_ticks() of the last PHY read
    // Consecutive failed link reads, and the latch that stops them. A
    // device that has gone away without a detach event is polled
    // forever otherwise -- see rtl_poll().
    uint8_t  link_fails;
    uint8_t  link_gone;

    struct net_device dev;
};

// The chip whose `match` accepts `version`, or NULL: the gate that keeps
// an unknown part from being driven with the wrong sequence.
const struct rtl_usb_ops *rtl_usb_chip_for(uint16_t version);
extern const struct rtl_usb_ops rtl8153_ops;
extern const struct rtl_usb_ops rtl8156_ops;

// --- the register layer, for the chip files ---------------------------
//
// A register narrower than 32 bits is read as the aligned dword it sits
// in and shifted out, and written behind a byte-enable mask naming the
// bytes to keep; get the shift and the mask out of step and the right
// value lands in the wrong half, which reads back perfectly and does
// nothing. `index` is MCU_PLA or MCU_USB.
int      rtl_reg_read_mem(uint16_t addr, uint16_t index, void *buf, uint16_t len);
int      rtl_reg_write_mem(uint16_t addr, uint16_t index, void *buf, uint16_t len);
uint32_t rtl_read4(uint16_t reg, uint16_t index);
uint16_t rtl_read2(uint16_t reg, uint16_t index);
uint8_t  rtl_read1(uint16_t reg, uint16_t index);
int      rtl_read2_ok(uint16_t reg, uint16_t index, uint16_t *out);
int      rtl_write4(uint16_t reg, uint16_t index, uint32_t val);
int      rtl_write2(uint16_t reg, uint16_t index, uint32_t val);
int      rtl_write1(uint16_t reg, uint16_t index, uint32_t val);
void     rtl_set2(uint16_t reg, uint16_t index, uint16_t bits);
void     rtl_clr2(uint16_t reg, uint16_t index, uint16_t bits);
void     rtl_set1(uint16_t reg, uint16_t index, uint8_t bits);
void     rtl_clr1(uint16_t reg, uint16_t index, uint8_t bits);
void     rtl_clr4(uint16_t reg, uint16_t index, uint32_t bits);
uint16_t rtl_ocp_read(uint16_t addr);
void     rtl_ocp_write(uint16_t addr, uint16_t data);
void     rtl_sram_write(uint16_t addr, uint16_t data);
void     rtl_wait_ms(uint32_t ms);

// Sequences more than one chip shares, as ure(4) names them.
void     rtl_hw_reset(void);                    // PLA_CR reset and wait
void     rtl_disable_teredo(int b_family);      // the B parts write the whole byte
void     rtl_disable_aldps(void);
void     rtl_enable_aldps(void);
uint16_t rtl_phy_status(uint16_t desired);      // waits; 0 = any settled state
void     rtl_wait_autoload(void);
void     rtl_tolerance(uint8_t fill);
int      rtl_link_up_now(void);                 // PLA_PHYSTATUS's link bit

#endif
