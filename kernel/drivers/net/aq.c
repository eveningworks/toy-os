// Aquantia (now Marvell) AQtion multi-gigabit Ethernet -- both
// generations: A1 "Atlantic" (AQC100-AQC112, the first ASUS XG-C100C)
// and A2 "Antigua" (AQC113-AQC116, the XG-C100C V2 in the Kaby Lake
// desktop). One queue, one interrupt vector, no offload.
//
// THE CARD'S OWN FIRMWARE RUNS THE PHY. Nothing here touches an MDIO
// register: the driver asks the firmware for the rates it may link at
// and reads back what it got. A1 talks to it through MPI registers and
// a mailbox in the firmware's RAM; A2 through a shared-memory
// "interface buffer" (an IN half the host writes and acknowledges, an
// OUT half the firmware writes under a transaction id). The descriptor
// rings, the MAC filters and the interrupt block are the same on both,
// which is why this is one driver and not two.
//
// ADAPTED FROM OpenBSD's aq(4) -- sys/dev/pci/if_aq_pci.c, itself from
// NetBSD's if_aq.c (Ryo Shimizu), whose firmware-1.x link calls fill
// what OpenBSD left out. ISC / BSD-2 / BSD-3; the notices are in
// LICENSE. Linux's atlantic driver describes the same hardware and is
// GPL-2.0; it was deliberately not consulted, for the reason
// rtl_usb.c's note gives.
#include "netdev.h"
#include "aq.h"
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
#include "irqflags.h"    // irq_save() -- the sweep must not race the handler
#include "clocksource.h" // clocksource_delay_ms() -- the firmware takes seconds
#include "multiboot.h"   // multiboot_cmdline() -- the `noaq` flag
#include "driver.h"

DRIVER_DECLARE("aq", "net", "Aquantia AQtion multi-gigabit Ethernet (AQC100-AQC116)");

#include "aq_regs.h"

// Ring lengths are programmed in units of eight descriptors.
#define RX_DESCS  128
#define TX_DESCS  64
#define BUF_SIZE  AQ_RX_BUF

// Interrupt status bits the causes are routed to. ONE VECTOR CARRIES
// THEM ALL (MULTIVEC off), so the bit only says which work to do.
#define IRQ_RX    0
#define IRQ_LINK  31

// Link rates are the ABI's NET_RATE_* (abi/net_abi.h), what this driver
// asks the firmware for.
#define LINK_10M  NET_RATE_10M
#define LINK_100M NET_RATE_100M
#define LINK_1G   NET_RATE_1G
#define LINK_2G5  NET_RATE_2G5
#define LINK_5G   NET_RATE_5G
#define LINK_10G  NET_RATE_10G

// A1 revision features -- they change a handful of register writes.
#define FEAT_REV_B0 (1u << 0)
#define FEAT_REV_B1 (1u << 1)
#define FEAT_REV_B  (FEAT_REV_B0 | FEAT_REV_B1)
#define FEAT_TPO2   (1u << 2)
#define FEAT_RPF2   (1u << 3)

struct aq {
    volatile uint8_t *mmio;
    int a2;
    uint32_t rates;          // LINK_* this part can do
    uint32_t features;       // FEAT_* (A1 only)
    uint32_t fw_version;     // major << 24 | minor << 16 | build, both generations
    uint32_t mbox_addr;      // A1: the firmware's mailbox in its RAM
    uint32_t art_base;       // A2: where the host's ART entries start
    void *rx, *tx;           // descriptor rings, one page each
    uint8_t *rx_buf, *tx_buf;
    uint64_t rx_buf_phys, tx_buf_phys;
    uint32_t rx_cons, rx_prod;
    uint32_t tx_prod;
    int rx_in_chain;         // inside a frame that spans descriptors: drop to its EOP
    uint32_t imask;          // the interrupt bits the handler re-arms
    uint8_t irq;
    int irq_seen;
    int rx_seen;
    int drops_logged;
};

static struct aq g_aq;
static struct net_device g_dev;
static int g_present;
static int g_probed;

static inline uint32_t rd(uint32_t o) { return *(volatile uint32_t *)(g_aq.mmio + o); }
static inline void wr(uint32_t o, uint32_t v) { *(volatile uint32_t *)(g_aq.mmio + o) = v; }
static uint64_t rd64(uint32_t o) { return (uint64_t)rd(o) | ((uint64_t)rd(o + 4) << 32); }
static void wr64(uint32_t o, uint64_t v) { wr(o, (uint32_t)v); wr(o + 4, (uint32_t)(v >> 32)); }

// Read-modify-write of one field: `val` is in the field's own units and
// lands at the mask's lowest bit, the BSDs' AQ_WRITE_REG_BIT.
static void set_field(uint32_t o, uint32_t mask, uint32_t val) {
    uint32_t v = rd(o) & ~mask;
    v |= (val * (mask & -mask)) & mask;
    wr(o, v);
}

// Two bounds, because the waits come in two sizes. A microsecond-scale
// handshake is bounded by a COUNT of register reads (each one a PCIe
// round trip, so roughly a microsecond); a firmware boot that takes
// seconds is bounded in milliseconds of clocksource_delay_ms(), which
// needs no interrupt (docs/conventions/kernel.md on which bound
// belongs where).
//
// THE CONDITION IS EVALUATED ONCE PER TRY AND THAT RESULT RETURNED --
// the semaphores here are taken BY READING them, so a final re-check
// would read "held" off a semaphore the loop had just acquired.
#define WAIT_SPIN(expr, n) ({ uint32_t _n = (n); int _ok; \
    while (!(_ok = !!(expr)) && _n) { _n--; } _ok; })
#define WAIT_MS(expr, ms) ({ uint32_t _n = (ms); int _ok; \
    while (!(_ok = !!(expr)) && _n) { clocksource_delay_ms(1); _n--; } _ok; })

// --- the A1 firmware ---------------------------------------------------

static int fw1_major(void) { return (int)(g_aq.fw_version >> 24); }

// Read `cnt` words of the firmware's RAM through the mailbox, under the
// RAM semaphore the firmware also takes.
static int aq1_download(uint32_t addr, uint32_t *p, uint32_t cnt) {
    if (!WAIT_SPIN(rd(AQ_FW_SEM_RAM_REG) == 1, 10000)) {
        wr(AQ_FW_SEM_RAM_REG, 1);
        if (rd(AQ_FW_SEM_RAM_REG) == 0) return 0;
    }
    wr(AQ_FW_MBOX_ADDR_REG, addr);
    int ok = 1;
    for (; cnt > 0 && ok; cnt--) {
        set_field(AQ_FW_MBOX_CMD_REG, AQ_FW_MBOX_CMD_EXECUTE, 1);
        // B1 advances the address register when a word is done; earlier
        // revisions clear a busy bit instead.
        if (g_aq.features & FEAT_REV_B1) ok = WAIT_SPIN(rd(AQ_FW_MBOX_ADDR_REG) != addr, 1000);
        else ok = WAIT_SPIN(!(rd(AQ_FW_MBOX_CMD_REG) & AQ_FW_MBOX_CMD_BUSY), 1000);
        *p++ = rd(AQ_FW_MBOX_VAL_REG);
        addr += 4;
    }
    wr(AQ_FW_SEM_RAM_REG, 1);
    return ok;
}

static void aq1_global_reset(void) {
    set_field(RX_SYSCONTROL_REG, RX_SYSCONTROL_RESET_DIS, 0);
    set_field(TX_SYSCONTROL_REG, TX_SYSCONTROL_RESET_DIS, 0);
    set_field(FW_MPI_RESETCTRL_REG, FW_MPI_RESETCTRL_RESET_DIS, 0);
    uint32_t v = rd(AQ_FW_SOFTRESET_REG);
    wr(AQ_FW_SOFTRESET_REG, (v & ~AQ_FW_SOFTRESET_DIS) | AQ_FW_SOFTRESET_RESET);
}

// THE ROM BOOTLOADER (RBL) PATH: reset, then wait for the bootloader
// to say where the firmware came from. A card that wants the HOST to
// load its firmware is refused -- there is no firmware file to give it.
static const char *aq1_reset_rbl(void) {
    wr(AQ_FW_GLB_CTL2_REG, 0x40E1);
    wr(AQ_FW_GLB_CPU_SEM_REG(0), 1);
    wr(AQ_MBOXIF_POWER_GATING_CONTROL_REG, 0);
    wr(FW_BOOT_EXIT_CODE_REG, RBL_STATUS_DEAD);
    aq1_global_reset();
    wr(AQ_FW_GLB_CTL2_REG, 0x40E0);

    uint32_t st = 0;
    WAIT_MS(((st = rd(FW_BOOT_EXIT_CODE_REG) & 0xFFFF) != 0 && st != RBL_STATUS_DEAD), 10000);
    if (st == RBL_STATUS_SUCCESS) return 0;
    if (st == RBL_STATUS_HOST_BOOT) return "the card wants its firmware loaded by the host";
    return "the boot ROM never reported success";
}

// THE FLASH BOOTLOADER (FLB) PATH, the older parts': the delays are the
// BSDs' and are there for an SMBus or SPI transaction the reset would
// otherwise cut in half.
static const char *aq1_reset_flb(void) {
    wr(AQ_FW_GLB_CTL2_REG, 0x40E1);
    clocksource_delay_ms(50);
    wr(AQ_GLB_NVR_PROVISIONING2_REG, 0x00A0);
    wr(AQ_GLB_NVR_INTERFACE1_REG, 0x009F);
    wr(AQ_GLB_NVR_INTERFACE1_REG, 0x809F);
    clocksource_delay_ms(50);
    uint32_t v = rd(AQ_FW_SOFTRESET_REG);
    wr(AQ_FW_SOFTRESET_REG, (v & ~AQ_FW_SOFTRESET_DIS) | AQ_FW_SOFTRESET_RESET);

    wr(AQ_FW_GLB_CTL2_REG, 0x80E0);
    wr(AQ_MBOXIF_POWER_GATING_CONTROL_REG, 0);
    wr(AQ_GLB_GENERAL_PROVISIONING9_REG, 1);
    clocksource_delay_ms(50);

    wr(AQ_FW_GLB_CTL2_REG, 0x180E0);   // MAC kickstart
    if (!WAIT_MS(rd(FW_MPI_DAISY_CHAIN_STATUS_REG) & 0x10, 1000))
        return "the MAC kickstart timed out";
    wr(AQ_FW_GLB_CTL2_REG, 0x80E0);
    clocksource_delay_ms(50);

    wr(AQ_FW_GLB_CPU_SEM_REG(0), 1);
    aq1_global_reset();
    if (!WAIT_MS(rd(AQ_FW_VERSION_REG) != 0, 10000)) return "the firmware never restarted";
    return 0;
}

static const char *aq1_fw_boot(void) {
    uint32_t chain = 0, exit_code = 0;
    if (!WAIT_SPIN((chain = rd(FW_MPI_DAISY_CHAIN_STATUS_REG),
                    exit_code = rd(FW_BOOT_EXIT_CODE_REG),
                    chain != 0x06000000u || exit_code != 0), 1000))
        return "neither boot loader started";

    const char *why = exit_code ? aq1_reset_rbl() : aq1_reset_flb();
    if (why) return why;

    if (!WAIT_MS((g_aq.fw_version = rd(AQ_FW_VERSION_REG)) != 0, 10000))
        return "the firmware reports no version";
    if (fw1_major() < 1 || fw1_major() > 3) return "a firmware major version this driver does not know";

    switch (rd(AQ_HW_REVISION_REG) & 0xF) {
    case 0x1: break;                                                  // A0
    case 0x2: g_aq.features |= FEAT_REV_B0 | FEAT_TPO2 | FEAT_RPF2; break;
    case 0xA: g_aq.features |= FEAT_REV_B1 | FEAT_TPO2 | FEAT_RPF2; break;
    default:  return "a silicon revision this driver does not know";
    }

    if (fw1_major() == 1) {
        if (rd(FW1X_MPI_INIT2_REG) == 0) wr(FW1X_MPI_INIT2_REG, 0xFEFEFEFE);
        wr(FW1X_MPI_INIT1_REG, 0);
    }
    // "It usually takes about 5 seconds" (NetBSD).
    WAIT_MS((g_aq.mbox_addr = rd(FW_MPI_MBOX_ADDR_REG)) != 0, 10000);
    if (g_aq.fw_version < 0x01050006u) return "firmware older than 1.5.6";
    if (!g_aq.mbox_addr) return "the firmware published no mailbox";
    return 0;
}

// Firmware 1.x is alive once its mailbox transaction id moves.
static const char *aq1_fw_reset(void) {
    if (fw1_major() != 1) return 0;
    uint32_t hdr[3], tid0 = 0;
    for (int i = 0; i < 1000; i++) {
        if (!aq1_download(g_aq.mbox_addr, hdr, 3)) continue;
        if (i == 0) tid0 = hdr[1];
        else if (hdr[1] != tid0) return 0;
        clocksource_delay_ms(1);
    }
    return "firmware 1.x never answered its mailbox";
}

static int aq1_get_mac(uint8_t mac[6]) {
    uint32_t efuse = rd(fw1_major() >= 2 ? FW2X_MPI_EFUSEADDR_REG : FW1X_MPI_EFUSEADDR_REG);
    uint32_t w[2] = { 0, 0 };
    if (!efuse || !aq1_download(efuse + 40 * 4, w, 2)) return 0;
    if (!w[0] && !w[1]) return 0;
    aq1_mac_from_words(w, mac);
    return 1;
}

// `v` NULL takes the link down. Firmware 1.x has rates and nothing else.
static int aq1_apply(const struct net_link_values *v) {
    uint32_t rates = v ? v->rates : 0;
    if (fw1_major() == 1) {
        uint32_t s = 0;
        if (rates & LINK_10G)  s |= 1u << 0;
        if (rates & LINK_5G)   s |= (1u << 1) | (1u << 2);
        if (rates & LINK_2G5)  s |= 1u << 3;
        if (rates & LINK_1G)   s |= 1u << 4;
        if (rates & LINK_100M) s |= 1u << 5;
        wr(FW1X_MPI_CONTROL_REG, 2 /* MPI_INIT */ | (s << 16));
        return 0;
    }
    uint64_t c = rd64(FW2X_MPI_CONTROL_REG);
    c &= ~((1ull << 5) | (1ull << 8) | (1ull << 9) | (1ull << 10) | (1ull << 11));
    if (rates & LINK_10G)  c |= 1ull << 11;
    if (rates & LINK_5G)   c |= 1ull << 10;
    if (rates & LINK_2G5)  c |= 1ull << 9;
    if (rates & LINK_1G)   c |= 1ull << 8;
    if (rates & LINK_100M) c |= 1ull << 5;
    c &= ~(1ull << 54);                                       // LINK_DROP
    c &= ~((1ull << 33) | (1ull << 37) | (0xFull << 40));     // every EEE rate
    if (v && v->eee) c |= (1ull << 33) | (1ull << 37) | (0xFull << 40);
    c &= ~((1ull << 35) | (1ull << 36));                      // pause, asymmetric pause
    if (v && (v->flow & NET_FLOW_RX)) c |= 1ull << 35;
    if (v && (v->flow & NET_FLOW_TX)) c |= 1ull << 36;
    wr64(FW2X_MPI_CONTROL_REG, c);
    return 0;
}

static uint64_t aq1_link_bps(void) {
    if (fw1_major() == 1) return aq_fw1x_link_bps(rd(FW1X_MPI_STATE_REG));
    return aq_fw2x_link_bps(rd64(FW2X_MPI_STATE_REG));
}

// --- the A2 firmware ---------------------------------------------------

// A CONSISTENT READ OF THE OUT BUFFER: the firmware brackets each update
// with two halves of a transaction id, so a read is good only when both
// halves agree before it AND the id has not moved after it.
static int aq2_read_out(uint32_t reg, uint32_t *data, uint32_t words) {
    for (int tries = 0; tries < 1000; tries++) {
        uint32_t t0 = rd(AQ2_FW_OUT_TRANSACTION_ID_REG);
        if ((t0 & 0xFFFF) == (t0 >> 16)) {
            for (uint32_t i = 0; i < words; i++) data[i] = rd(reg + 4 * i);
            if (rd(AQ2_FW_OUT_TRANSACTION_ID_REG) == t0) return 1;
        }
        clocksource_delay_ms(1);
    }
    return 0;
}

// The host's writes to the IN buffer take effect when the firmware
// acknowledges them -- nothing earlier.
static int aq2_commit(void) {
    wr(AQ2_MIF_HOST_FINISHED_STATUS_WRITE_REG, AQ2_MIF_HOST_FINISHED_STATUS_ACK);
    return WAIT_MS(!(rd(AQ2_MIF_HOST_FINISHED_STATUS_READ_REG) & AQ2_MIF_HOST_FINISHED_STATUS_ACK), 10000);
}

static const char *aq2_fw_boot(void) {
    wr(AQ2_MCP_HOST_REQ_INT_CLR_REG, 1);
    wr(AQ2_MIF_BOOT_REG, 1);   // reboot request
    uint32_t v = 0;
    if (!WAIT_MS(((v = rd(AQ2_MIF_BOOT_REG)) & AQ2_MIF_BOOT_BOOT_STARTED) && v != 0xFFFFFFFFu, 2000))
        return "the firmware never started rebooting";
    if (!WAIT_MS((rd(AQ2_MIF_BOOT_REG) & (AQ2_MIF_BOOT_FW_INIT_FAILED | AQ2_MIF_BOOT_FW_INIT_COMP_SUCCESS)) ||
                 (rd(AQ2_MCP_HOST_REQ_INT_REG) & AQ2_MCP_HOST_REQ_INT_READY), 20000))
        return "the firmware never finished booting";
    if (rd(AQ2_MIF_BOOT_REG) & AQ2_MIF_BOOT_FW_INIT_FAILED) return "the firmware failed to boot";
    if (rd(AQ2_MCP_HOST_REQ_INT_REG) & AQ2_MCP_HOST_REQ_INT_READY)
        return "the card wants its firmware loaded by the host";

    uint32_t bundle, caps[3];
    if (!aq2_read_out(AQ2_FW_OUT_VERSION_BUNDLE_REG, &bundle, 1) ||
        !aq2_read_out(AQ2_FW_OUT_FILTER_CAPS_REG, caps, 3))
        return "the firmware's interface buffer never settled";
    g_aq.fw_version = aq2_fw_version(bundle);
    g_aq.art_base = ((caps[2] & AQ2_FILTER_CAPS3_RESOLVER_BASE) >> 16) * 8;
    return 0;
}

static const char *aq2_fw_reset(void) {
    set_field(AQ2_FW_IN_LINK_CONTROL_REG, AQ2_FW_IN_LINK_CONTROL_MODE, AQ2_LINK_MODE_ACTIVE);
    wr(AQ2_FW_IN_MTU_REG, BUF_SIZE + 14);
    // Broadcast and multicast to queue 0. BOTH BSDs WRITE `v &= BCAST_RX_INDEX`
    // here without the `~`, which clears everything set above it; the
    // intent (every other line of the block) is what is written here.
    uint32_t v = rd(AQ2_FW_IN_REQUEST_POLICY_REG);
    v |= AQ2_POLICY_MCAST_QUEUE_OR_TC | AQ2_POLICY_MCAST_ACCEPT;
    v &= ~AQ2_POLICY_MCAST_RX_INDEX;
    v |= AQ2_POLICY_BCAST_QUEUE_OR_TC | AQ2_POLICY_BCAST_ACCEPT;
    v &= ~AQ2_POLICY_BCAST_RX_INDEX;
    v |= AQ2_POLICY_PROMISC_QUEUE_OR_TC;
    v &= ~AQ2_POLICY_PROMISC_RX_INDEX;
    wr(AQ2_FW_IN_REQUEST_POLICY_REG, v);
    return aq2_commit() ? 0 : "the firmware never acknowledged the reset";
}

static int aq2_get_mac(uint8_t mac[6]) {
    uint32_t w[2] = { rd(AQ2_FW_IN_MAC_ADDRESS_REG), rd(AQ2_FW_IN_MAC_ADDRESS_REG + 4) };
    if (!w[0] && !w[1]) return 0;
    aq2_mac_from_words(w, mac);
    return 1;
}

// `v` NULL takes the link down (SHUTDOWN mode).
static int aq2_apply(const struct net_link_values *v) {
    uint32_t rates = v ? v->rates : 0;
    uint32_t o = rd(AQ2_FW_IN_LINK_OPTIONS_REG);
    o &= ~(AQ2_LINK_OPT_RATE_MASK | AQ2_LINK_OPT_LINK_UP | AQ2_LINK_OPT_PAUSE_TX | AQ2_LINK_OPT_PAUSE_RX |
           AQ2_LINK_OPT_EEE_MASK);
    if (rates & LINK_10G)  o |= AQ2_LINK_OPT_RATE_10G;
    if (rates & LINK_5G)   o |= AQ2_LINK_OPT_RATE_N5G | AQ2_LINK_OPT_RATE_5G;
    if (rates & LINK_2G5)  o |= AQ2_LINK_OPT_RATE_N2G5 | AQ2_LINK_OPT_RATE_2G5;
    if (rates & LINK_1G)   o |= AQ2_LINK_OPT_RATE_1G | AQ2_LINK_OPT_RATE_1G_HD;
    if (rates & LINK_100M) o |= AQ2_LINK_OPT_RATE_100M | AQ2_LINK_OPT_RATE_100M_HD;
    if (rates & LINK_10M)  o |= AQ2_LINK_OPT_RATE_10M | AQ2_LINK_OPT_RATE_10M_HD;
    // EEE per rate it can be offered at; 10M has none.
    if (v && v->eee) {
        if (rates & LINK_10G)  o |= 1u << 20;
        if (rates & LINK_5G)   o |= 1u << 19;
        if (rates & LINK_2G5)  o |= 1u << 18;
        if (rates & LINK_1G)   o |= 1u << 17;
        if (rates & LINK_100M) o |= 1u << 16;
    }
    if (v && (v->flow & NET_FLOW_RX)) o |= AQ2_LINK_OPT_PAUSE_RX;
    if (v && (v->flow & NET_FLOW_TX)) o |= AQ2_LINK_OPT_PAUSE_TX;
    if (rates) o |= AQ2_LINK_OPT_LINK_UP;
    set_field(AQ2_FW_IN_LINK_CONTROL_REG, AQ2_FW_IN_LINK_CONTROL_MODE,
              rates ? AQ2_LINK_MODE_ACTIVE : AQ2_LINK_MODE_SHUTDOWN);
    wr(AQ2_FW_IN_LINK_OPTIONS_REG, o);
    if (!aq2_commit()) {
        klog_write(KLOG_WARN "aq: the firmware did not acknowledge the link request\n");
        return -EIO;
    }
    return 0;
}

// One ART entry, under the semaphore the firmware shares.
static void aq2_art_set(uint32_t idx, uint32_t tag, uint32_t mask, uint32_t action) {
    // READING IT IS THE ACQUIRE (1 = ours), writing 1 the release. The
    // firmware holds it for tens of milliseconds at a time after a reset.
    if (!WAIT_MS(rd(AQ2_ART_SEM_REG) == 1, 100)) {
        klog_write(KLOG_WARN "aq: the filter table's semaphore never came free\n");
        return;
    }
    idx += g_aq.art_base;
    wr(AQ2_RPF_ACT_ART_REQ_TAG_REG(idx), tag);
    wr(AQ2_RPF_ACT_ART_REQ_MASK_REG(idx), mask);
    wr(AQ2_RPF_ACT_ART_REQ_ACTION_REG(idx), action);
    wr(AQ2_ART_SEM_REG, 1);
}

static int link_apply(const struct net_link_values *v) { return g_aq.a2 ? aq2_apply(v) : aq1_apply(v); }

// How long a receive interrupt may be held back, in the hardware's 2 us
// units: the first frame waits at most `min` for company, a burst at
// most `max`. MEDIUM is the BSDs' 12-120 us.
static void moderation_apply(uint32_t m) {
    static const uint16_t lo[4] = { 0, 2, 6, 20 }, hi[4] = { 0, 20, 60, 200 };
    if (m > NET_MOD_HIGH) m = NET_MOD_MEDIUM;
    set_field(RX_INTR_MODERATION_CTL_REG(0), RX_INTR_MODERATION_CTL_EN, 0);
    if (m == NET_MOD_OFF) return;
    set_field(RX_INTR_MODERATION_CTL_REG(0), RX_INTR_MODERATION_CTL_MIN, lo[m]);
    set_field(RX_INTR_MODERATION_CTL_REG(0), RX_INTR_MODERATION_CTL_MAX, hi[m]);
    set_field(RX_INTR_MODERATION_CTL_REG(0), RX_INTR_MODERATION_CTL_EN, 1);
}

// SYS_NET_LINK, through the core (net_link.c), which has validated `v`.
static int aq_set_link(struct net_device *dev, const struct net_link_values *v) {
    int r = link_apply(v);
    if (r) return r;
    if (!v->eee) dev->eee_active = 0;   // not until the link renegotiates, otherwise
    moderation_apply(v->moderation);
    return 0;
}
static uint64_t link_bps(void) { return g_aq.a2 ? aq2_link_bps(rd(AQ2_FW_OUT_LINK_STATUS_REG)) : aq1_link_bps(); }

// --- the MAC: filters, buffers, rings ----------------------------------

static void set_own_mac(const uint8_t *m) {
    set_field(RPF_L2UC_MSW_REG(0), RPF_L2UC_MSW_EN, 0);
    wr(RPF_L2UC_LSW_REG(0), ((uint32_t)m[2] << 24) | ((uint32_t)m[3] << 16) | ((uint32_t)m[4] << 8) | m[5]);
    set_field(RPF_L2UC_MSW_REG(0), RPF_L2UC_MSW_MACADDR_HI, ((uint32_t)m[0] << 8) | m[1]);
    set_field(RPF_L2UC_MSW_REG(0), RPF_L2UC_MSW_ACTION, RPF_ACTION_HOST);
    if (g_aq.a2) set_field(RPF_L2UC_MSW_REG(0), RPF_L2UC_MSW_TAG, 1);
    set_field(RPF_L2UC_MSW_REG(0), RPF_L2UC_MSW_EN, 1);
}

static void init_tx_path(void) {
    set_field(TPB_TX_BUF_REG, TPB_TX_BUF_TC_MODE_EN, 1);
    set_field(THM_LSO_TCP_FLAG1_REG, THM_LSO_TCP_FLAG1_FIRST, 0x0FF6);
    set_field(THM_LSO_TCP_FLAG1_REG, THM_LSO_TCP_FLAG1_MID, 0x0FF6);
    set_field(THM_LSO_TCP_FLAG2_REG, THM_LSO_TCP_FLAG2_LAST, 0x0F7F);
    wr(TX_TPO2_REG, (g_aq.features & FEAT_TPO2) ? TX_TPO2_EN : 0);
    wr(TDM_DCA_REG, 0);
    set_field(TPB_TX_BUF_REG, TPB_TX_BUF_SCP_INS_EN, 1);
    if ((g_aq.features & FEAT_REV_B) || g_aq.a2) set_field(TPB_TX_BUF_REG, TPB_TX_BUF_CLK_GATE_EN, 0);
}

static void init_rx_path(void) {
    set_field(RPB_RPF_RX_REG, RPB_RPF_RX_TC_MODE, 0);
    set_field(RPB_RPF_RX_REG, RPB_RPF_RX_FC_MODE, 0);
    if (g_aq.a2) set_field(AQ2_RPF_REDIR2_REG, AQ2_RPF_REDIR2_HASHTYPE, 0x1FF);
    wr(RX_FLR_RSS_CONTROL1_REG, 0);   // one queue: no RSS

    if (!g_aq.a2)
        for (int i = 0; i < 32; i++) set_field(RPF_ETHERTYPE_FILTER_REG(i), RPF_ETHERTYPE_FILTER_EN, 0);

    for (int i = 0; i < AQ_HW_MAC_NUM; i++) {
        set_field(RPF_L2UC_MSW_REG(i), RPF_L2UC_MSW_EN, 0);
        set_field(RPF_L2UC_MSW_REG(i), RPF_L2UC_MSW_ACTION, RPF_ACTION_HOST);
    }
    wr(RPF_MCAST_FILTER_MASK_REG, 0);
    wr(RPF_MCAST_FILTER_REG(0), 0x00010FFF);

    set_field(RPF_VLAN_TPID_REG, RPF_VLAN_TPID_OUTER, 0x88A8);
    set_field(RPF_VLAN_TPID_REG, RPF_VLAN_TPID_INNER, 0x8100);
    set_field(RPF_VLAN_MODE_REG, RPF_VLAN_MODE_PROMISC, 1);
    if ((g_aq.features & FEAT_REV_B) || g_aq.a2) {
        set_field(RPF_VLAN_MODE_REG, RPF_VLAN_MODE_ACCEPT_UNTAGGED, 1);
        set_field(RPF_VLAN_MODE_REG, RPF_VLAN_MODE_UNTAGGED_ACTION, RPF_ACTION_HOST);
    }

    if (g_aq.a2) {
        // A2 DECIDES BY ACTION TABLE, not by the filter that matched: a
        // filter only TAGS a frame (our MAC tags 1, broadcast tags 1), and
        // these entries drop what carries no tag -- the hardware's
        // "promiscuous off". Every VLAN priority goes to traffic class 0.
        set_field(AQ2_RPF_REC_TAB_ENABLE_REG, AQ2_RPF_REC_TAB_ENABLE_MASK, 0xFFFF);
        set_field(RPF_L2UC_MSW_REG(0), RPF_L2UC_MSW_TAG, 1);
        set_field(AQ2_RPF_L2BC_TAG_REG, AQ2_RPF_L2BC_TAG_MASK, 1);
        aq2_art_set(AQ2_RPF_INDEX_L2_PROMISC_OFF, 0,
                    AQ2_RPF_TAG_UC_MASK | AQ2_RPF_TAG_ALLMC_MASK, AQ2_ART_ACTION_DROP);
        aq2_art_set(AQ2_RPF_INDEX_VLAN_PROMISC_OFF, 0,
                    AQ2_RPF_TAG_VLAN_MASK | AQ2_RPF_TAG_UNTAG_MASK, AQ2_ART_ACTION_DROP);
        for (uint32_t i = 0; i < 8; i++)
            aq2_art_set(AQ2_RPF_INDEX_PCP_TO_TC + i, i << AQ2_RPF_TAG_PCP_SHIFT,
                        AQ2_RPF_TAG_PCP_MASK, AQ2_ART_ACTION_ASSIGN_TC(0));
    } else {
        wr(RX_TCP_RSS_HASH_REG, (g_aq.features & FEAT_RPF2) ? RX_TCP_RSS_HASH_RPF2 : 0);
        set_field(RX_TCP_RSS_HASH_REG, RX_TCP_RSS_HASH_TYPE, 0x001E);
    }

    set_field(RPF_L2BC_REG, RPF_L2BC_EN, 1);
    set_field(RPF_L2BC_REG, RPF_L2BC_ACTION, RPF_ACTION_HOST);
    set_field(RPF_L2BC_REG, RPF_L2BC_THRESHOLD, 0xFFFF);
    wr(RX_DMA_DCA_REG, 0);
}

// Packet-buffer sizes and arbitration for traffic class 0, the only one
// used. Units are KiB for the sizes and 32 bytes for the thresholds.
static void init_qos(void) {
    set_field(TPS_DESC_RATE_REG, TPS_DESC_RATE_TA_RST, 0);
    set_field(TPS_DESC_RATE_REG, TPS_DESC_RATE_LIM, 0xA);
    set_field(TPS_DESC_VM_ARB_MODE_REG, TPS_DESC_VM_ARB_MODE, 0);
    set_field(TPS_DESC_TC_ARB_MODE_REG, TPS_DESC_TC_ARB_MODE, 0);
    set_field(TPS_DATA_TC_ARB_MODE_REG, TPS_DATA_TC_ARB_MODE, 0);
    if (g_aq.a2) {
        set_field(TPS_DATA_TCT_REG(0), TPS2_DATA_TCT_CREDIT_MAX, 0xFFF0);
        set_field(TPS_DATA_TCT_REG(0), TPS2_DATA_TCT_WEIGHT, 0x640);
    } else {
        set_field(TPS_DATA_TCT_REG(0), TPS_DATA_TCT_CREDIT_MAX, 0xFFF);
        set_field(TPS_DATA_TCT_REG(0), TPS_DATA_TCT_WEIGHT, 0x64);
    }
    set_field(TPS_DESC_TCT_REG(0), TPS_DESC_TCT_CREDIT_MAX, 0x50);
    set_field(TPS_DESC_TCT_REG(0), TPS_DESC_TCT_WEIGHT, 0x1E);

    uint32_t txb = g_aq.a2 ? 128 : 160, rxb = g_aq.a2 ? 192 : 320;
    set_field(TPB_TXB_BUFSIZE_REG(0), TPB_TXB_BUFSIZE, txb);
    set_field(TPB_TXB_THRESH_REG(0), TPB_TXB_THRESH_HI, txb * 32 * 66 / 100);
    set_field(TPB_TXB_THRESH_REG(0), TPB_TXB_THRESH_LO, txb * 32 * 50 / 100);
    set_field(RPB_RXB_BUFSIZE_REG(0), RPB_RXB_BUFSIZE, rxb);
    set_field(RPB_RXB_XOFF_REG(0), RPB_RXB_XOFF_EN, 0);
    set_field(RPB_RXB_XOFF_REG(0), RPB_RXB_XOFF_THRESH_HI, rxb * 32 * 66 / 100);
    set_field(RPB_RXB_XOFF_REG(0), RPB_RXB_XOFF_THRESH_LO, rxb * 32 * 50 / 100);

    for (int p = 0; p < 8; p++) set_field(RPF_RPB_RX_TC_UPT_REG, 7u << (p * 4), 0);

    if (g_aq.a2) {
        set_field(TPB_TX_BUF_REG, TPB_TX_BUF_TC_Q_RAND_MAP_EN, 1);
        static const uint32_t txmap[8] = { 0, 0, 0x01010101, 0x01010101, 0x02020202, 0x02020202,
                                           0x03030303, 0x03030303 };
        for (int i = 0; i < 8; i++) wr(AQ2_TX_Q_TC_MAP_REG(i), txmap[i]);
        for (uint32_t i = 0; i < 4; i++) wr(AQ2_RX_Q_TC_MAP_REG(i), 0x11111111u * i);
    }
}

static void rx_arm(uint32_t i) {
    struct aq_rx_desc_read *d = (struct aq_rx_desc_read *)g_aq.rx + i;
    d->buf_addr = g_aq.rx_buf_phys + (uint64_t)i * BUF_SIZE;
    d->hdr_addr = 0;
}

static void rings_start(void) {
    // Invalidate whatever descriptors the engine cached from before --
    // the bit TOGGLES, it does not set.
    uint32_t c = rd(RX_DMA_DESC_CACHE_INIT_REG);
    set_field(RX_DMA_DESC_CACHE_INIT_REG, RX_DMA_DESC_CACHE_INIT, (c & 1) ^ 1);

    uint64_t tx = (uint64_t)(uintptr_t)g_aq.tx, rx = (uint64_t)(uintptr_t)g_aq.rx;

    set_field(TX_DMA_DESC_REG(0), TX_DMA_DESC_EN, 0);
    wr(TX_DMA_DESC_BASE_ADDRLSW_REG(0), (uint32_t)tx);
    wr(TX_DMA_DESC_BASE_ADDRMSW_REG(0), (uint32_t)(tx >> 32));
    set_field(TX_DMA_DESC_REG(0), TX_DMA_DESC_LEN, TX_DESCS / 8);
    g_aq.tx_prod = rd(TX_DMA_DESC_TAIL_PTR_REG(0)) % TX_DESCS;
    wr(TX_DMA_DESC_WRWB_THRESH_REG(0), 0);
    set_field(TX_DMA_DESC_REG(0), TX_DMA_DESC_EN, 1);
    wr(TDM_DCAD_REG(0), 0);

    set_field(RX_DMA_DESC_REG(0), RX_DMA_DESC_EN, 0);
    wr(RX_DMA_DESC_BASE_ADDRLSW_REG(0), (uint32_t)rx);
    wr(RX_DMA_DESC_BASE_ADDRMSW_REG(0), (uint32_t)(rx >> 32));
    set_field(RX_DMA_DESC_REG(0), RX_DMA_DESC_LEN, RX_DESCS / 8);
    set_field(RX_DMA_DESC_BUFSIZE_REG(0), RX_DMA_DESC_BUFSIZE_DATA, BUF_SIZE / 1024);
    set_field(RX_DMA_DESC_BUFSIZE_REG(0), RX_DMA_DESC_BUFSIZE_HDR, 0);
    set_field(RX_DMA_DESC_REG(0), RX_DMA_DESC_HEADER_SPLIT, 0);
    set_field(RX_DMA_DESC_REG(0), RX_DMA_DESC_VLAN_STRIP, 0);
    g_aq.rx_cons = (rd(RX_DMA_DESC_HEAD_PTR_REG(0)) & RX_DMA_DESC_HEAD_PTR) % RX_DESCS;
    wr(RX_DMA_DESC_TAIL_PTR_REG(0), g_aq.rx_cons);
    set_field(AQ_INTR_IRQ_MAP_TXRX_REG(0), AQ_INTR_IRQ_MAP_RX_IRQMAP(0), IRQ_RX);
    set_field(AQ_INTR_IRQ_MAP_TXRX_REG(0), AQ_INTR_IRQ_MAP_RX_EN(0), 1);
    wr(RX_DMA_DCAD_REG(0), 0);
    set_field(RX_DMA_DESC_REG(0), RX_DMA_DESC_EN, 1);

    // ONE SLOT STAYS EMPTY: tail == head means the device owns nothing,
    // so a ring handed over whole would read as empty. The armed slots
    // are [cons, prod).
    uint32_t p = g_aq.rx_cons;
    for (uint32_t n = 0; n < RX_DESCS - 1; n++) { rx_arm(p); p = (p + 1) % RX_DESCS; }
    g_aq.rx_prod = p;
    kbarrier();
    wr(RX_DMA_DESC_TAIL_PTR_REG(0), g_aq.rx_prod);
}

// --- the data path -----------------------------------------------------

static void update_link(struct net_device *dev) {
    uint64_t bps = link_bps();
    uint8_t up = bps ? 1 : 0;
    // Whether the link NEGOTIATED EEE -- the request alone does not
    // decide it. A2 says so in its link status; A1 does not say.
    dev->eee_active = up && g_aq.a2 && (rd(AQ2_FW_OUT_LINK_STATUS_REG) & (1u << 10)) ? 1 : 0;
    if (!dev->link_known || dev->link_up != up || dev->link_bps != bps) {
        // EEE is named because a link that negotiated it is a different
        // link: the PHY drops into low-power idle between frames.
        const char *eee = dev->eee_active ? ", EEE" : "";
        if (!up) klog_printf("aq: %s link down\n", dev->name);
        else if (bps >= 1000000000ull && bps % 1000000000ull)
            klog_printf("aq: %s link UP %u.%uG%s\n", dev->name, (unsigned)(bps / 1000000000ull),
                        (unsigned)(bps % 1000000000ull / 100000000ull), eee);
        else if (bps >= 1000000000ull)
            klog_printf("aq: %s link UP %uG%s\n", dev->name, (unsigned)(bps / 1000000000ull), eee);
        else klog_printf("aq: %s link UP %uM%s\n", dev->name, (unsigned)(bps / 1000000ull), eee);
    }
    dev->link_up = up;
    dev->link_bps = bps;
    dev->link_known = 1;
}

// THE HEAD REGISTER, NOT THE DD BIT, says how far the device got -- DD
// is checked too, because the head can move a moment before the
// write-back lands. Each consumed slot re-arms the EMPTY one behind the
// armed run (rx_prod), so the gap travels round the ring.
static void drain_rx(struct net_device *dev) {
    uint32_t head = (rd(RX_DMA_DESC_HEAD_PTR_REG(0)) & RX_DMA_DESC_HEAD_PTR) % RX_DESCS;
    int moved = 0;
    while (g_aq.rx_cons != head) {
        volatile struct aq_rx_desc_wb *d = (volatile struct aq_rx_desc_wb *)g_aq.rx + g_aq.rx_cons;
        uint16_t status = d->status;
        if (!(status & AQ_RXD_DD)) break;
        uint32_t len = aq_rx_frame_len(status, d->type, d->pkt_len);
        // The first few refusals are logged whole: a drop counter says
        // something is wrong, the descriptor says what.
        if ((!len || g_aq.rx_in_chain) && g_aq.drops_logged < 4) {
            g_aq.drops_logged++;
            klog_printf(KLOG_WARN "aq: dropped rx slot %u: status %x type %x len %u%s\n", g_aq.rx_cons,
                        status, d->type, d->pkt_len, g_aq.rx_in_chain ? " (inside a chain)" : "");
        }
        if (!(status & AQ_RXD_EOP)) {
            g_aq.rx_in_chain = 1;
        } else {
            if (len && !g_aq.rx_in_chain) {
                if (!g_aq.rx_seen) {
                    g_aq.rx_seen = 1;
                    klog_printf("aq: first frame received, %u bytes\n", len);
                }
                net_rx(dev, g_aq.rx_buf + (uint64_t)g_aq.rx_cons * BUF_SIZE, len);
            } else {
                dev->rx_dropped++;
            }
            g_aq.rx_in_chain = 0;
        }
        rx_arm(g_aq.rx_prod);
        g_aq.rx_prod = (g_aq.rx_prod + 1) % RX_DESCS;
        g_aq.rx_cons = (g_aq.rx_cons + 1) % RX_DESCS;
        moved = 1;
    }
    if (moved) {
        kbarrier();
        wr(RX_DMA_DESC_TAIL_PTR_REG(0), g_aq.rx_prod);
    }
}

// Every cause shares one vector and the status says which. AUTOMASK
// masks a bit as it fires, so the mask is written back at the end --
// an event that landed meanwhile then raises a fresh interrupt.
static void aq_irq(uint64_t *regs) {
    (void)regs;
    if (!g_present) return;
    uint32_t status = rd(AQ_INTR_STATUS_REG);
    if (!status || status == 0xFFFFFFFFu) return;   // a shared INTx line, or a gone device
    wr(AQ_INTR_STATUS_CLR_REG, status);
    if (!g_aq.irq_seen) {
        g_aq.irq_seen = 1;
        klog_printf("aq: first interrupt, status %x\n", status);
    }
    if (status & (1u << IRQ_RX)) drain_rx(&g_dev);
    if (status & (1u << IRQ_LINK)) update_link(&g_dev);
    wr(AQ_INTR_MASK_REG, g_aq.imask);
}

// The completion is the hardware's head pointer: everything before it
// has gone, so there is no transmit interrupt and no reap pass.
static int aq_transmit(struct net_device *dev, const void *frame, uint32_t len) {
    (void)dev;
    if (len > BUF_SIZE) return -EINVAL;
    uint64_t f = irq_save();   // net_rx() can answer an ARP from inside the handler
    uint32_t head = (rd(TX_DMA_DESC_HEAD_PTR_REG(0)) & TX_DMA_DESC_HEAD_PTR) % TX_DESCS;
    uint32_t next = (g_aq.tx_prod + 1) % TX_DESCS;
    if (next == head) { irq_restore(f); return -ENOSPC; }

    uint32_t padded = aq_tx_pad(len);
    uint8_t *buf = g_aq.tx_buf + (uint64_t)g_aq.tx_prod * BUF_SIZE;
    k_memcpy(buf, frame, len);
    if (padded > len) k_memset(buf + len, 0, padded - len);

    struct aq_tx_desc *d = (struct aq_tx_desc *)g_aq.tx + g_aq.tx_prod;
    d->addr = g_aq.tx_buf_phys + (uint64_t)g_aq.tx_prod * BUF_SIZE;
    d->ctl1 = aq_tx_ctl1(padded);
    d->ctl2 = aq_tx_ctl2(padded);
    kbarrier();
    g_aq.tx_prod = next;
    wr(TX_DMA_DESC_TAIL_PTR_REG(0), next);
    irq_restore(f);
    return 0;
}

// A 10 ms sweep beside the interrupt, as r8169.c has: it finishes a
// receive the interrupt raced, and with no interrupt at all it IS the
// receive path. The link is read here too -- one register on A2.
static void aq_sweep(struct net_device *dev) {
    uint64_t f = irq_save();
    if (g_present) {
        drain_rx(dev);
        update_link(dev);
    }
    irq_restore(f);
}

// --- bring-up ----------------------------------------------------------

// Which generation each part is, and the rates it can link at (from the
// BSDs' product table: the cheaper parts top out lower).
struct aq_part { uint16_t device; uint8_t a2; uint8_t rates; };
#define R_ALL (LINK_100M | LINK_1G | LINK_2G5 | LINK_5G | LINK_10G)
#define R_5G  (LINK_100M | LINK_1G | LINK_2G5 | LINK_5G)
#define R_2G5 (LINK_100M | LINK_1G | LINK_2G5)
static const struct aq_part aq_parts[] = {
    { 0x00B1, 0, R_ALL }, { 0x07B1, 0, R_ALL }, { 0x08B1, 0, R_5G },  { 0x09B1, 0, R_2G5 },
    { 0x11B1, 0, R_5G },  { 0x12B1, 0, R_2G5 }, { 0x80B1, 0, R_ALL }, { 0x87B1, 0, R_ALL },
    { 0x88B1, 0, R_5G },  { 0x89B1, 0, R_2G5 }, { 0x91B1, 0, R_5G },  { 0x92B1, 0, R_2G5 },
    { 0xD100, 0, R_ALL }, { 0xD107, 0, R_ALL }, { 0xD108, 0, R_5G },  { 0xD109, 0, R_2G5 },
    { 0x04C0, 1, R_ALL | LINK_10M }, { 0x14C0, 1, R_ALL | LINK_10M },
    { 0x34C0, 1, R_ALL | LINK_10M }, { 0x94C0, 1, R_ALL | LINK_10M },
    { 0x93C0, 1, LINK_10M | R_5G },  { 0x12C0, 1, LINK_10M | R_2G5 },
    { 0x11C0, 1, LINK_10M | LINK_100M | LINK_1G },
};

// Every part above, by name, so modules.alias loads this for each. Only
// the AQC113CS (0x94C0) has been on a real machine here.
static const struct pci_match aq_matches[] = {
    PCI_MATCH_ID(AQUANTIA_VENDOR, 0x00B1),   // AQC100 (SFP+)
    PCI_MATCH_ID(AQUANTIA_VENDOR, 0x07B1),   // AQC107
    PCI_MATCH_ID(AQUANTIA_VENDOR, 0x08B1),   // AQC108
    PCI_MATCH_ID(AQUANTIA_VENDOR, 0x09B1),   // AQC109
    PCI_MATCH_ID(AQUANTIA_VENDOR, 0x11B1),   // AQC111
    PCI_MATCH_ID(AQUANTIA_VENDOR, 0x12B1),   // AQC112
    PCI_MATCH_ID(AQUANTIA_VENDOR, 0x80B1),   // AQC100S
    PCI_MATCH_ID(AQUANTIA_VENDOR, 0x87B1),   // AQC107S
    PCI_MATCH_ID(AQUANTIA_VENDOR, 0x88B1),   // AQC108S
    PCI_MATCH_ID(AQUANTIA_VENDOR, 0x89B1),   // AQC109S
    PCI_MATCH_ID(AQUANTIA_VENDOR, 0x91B1),   // AQC111S
    PCI_MATCH_ID(AQUANTIA_VENDOR, 0x92B1),   // AQC112S
    PCI_MATCH_ID(AQUANTIA_VENDOR, 0xD100),   // D100
    PCI_MATCH_ID(AQUANTIA_VENDOR, 0xD107),   // D107
    PCI_MATCH_ID(AQUANTIA_VENDOR, 0xD108),   // D108
    PCI_MATCH_ID(AQUANTIA_VENDOR, 0xD109),   // D109
    PCI_MATCH_ID(AQUANTIA_VENDOR, 0x04C0),   // AQC113
    PCI_MATCH_ID(AQUANTIA_VENDOR, 0x14C0),   // AQC113C
    PCI_MATCH_ID(AQUANTIA_VENDOR, 0x34C0),   // AQC113CA
    PCI_MATCH_ID(AQUANTIA_VENDOR, 0x94C0),   // AQC113CS -- the XG-C100C V2
    PCI_MATCH_ID(AQUANTIA_VENDOR, 0x93C0),   // AQC114CS
    PCI_MATCH_ID(AQUANTIA_VENDOR, 0x12C0),   // AQC115C
    PCI_MATCH_ID(AQUANTIA_VENDOR, 0x11C0),   // AQC116C
};

// `noaq` on the boot line leaves the card alone -- r8169.c's `nor8169`,
// for the same reason: nothing emulates this chip, so a probe that hangs
// a machine must still be avoidable from the GRUB line.
static int aq_disabled(void) {
    const char *cmdline = multiboot_cmdline();
    if (!cmdline) return 0;
    for (const char *p = cmdline; (p = k_strstr(p, "noaq")) != 0; p += 4) {
        if (p != cmdline && p[-1] != ' ') continue;
        if (p[4] == 0 || p[4] == ' ') return 1;
    }
    return 0;
}

static int alloc_rings(void) {
    uint64_t rx_ring = pmm_alloc_contiguous(1, PMM_ZONE_DMA32);
    uint64_t tx_ring = pmm_alloc_contiguous(1, PMM_ZONE_DMA32);
    uint64_t rx_bufs = pmm_alloc_contiguous((RX_DESCS * BUF_SIZE) / 4096, PMM_ZONE_DMA32);
    uint64_t tx_bufs = pmm_alloc_contiguous((TX_DESCS * BUF_SIZE) / 4096, PMM_ZONE_DMA32);
    g_aq.rx = (void *)(uintptr_t)rx_ring;
    g_aq.tx = (void *)(uintptr_t)tx_ring;
    g_aq.rx_buf = (uint8_t *)(uintptr_t)rx_bufs;
    g_aq.tx_buf = (uint8_t *)(uintptr_t)tx_bufs;
    g_aq.rx_buf_phys = rx_bufs;
    g_aq.tx_buf_phys = tx_bufs;
    if (!rx_ring || !tx_ring || !rx_bufs || !tx_bufs) return 0;
    k_memset(g_aq.rx, 0, 4096);
    k_memset(g_aq.tx, 0, 4096);
    return 1;
}

static void aq_remove(const struct pci_device *pci);

// pci_probe_decline() logs the reason, so a decline is one line.
static int aq_decline(const struct pci_device *pci, const char *why) {
    aq_remove(pci);
    return pci_probe_decline(pci, "%s", why);
}

static int aq_probe(const struct pci_device *pci) {
    if (aq_disabled()) return pci_probe_decline(pci, "off: `noaq` on the boot line");
    if (g_probed) return pci_probe_decline(pci, "a second card; one is driven");

    const struct aq_part *part = 0;
    for (unsigned i = 0; i < sizeof aq_parts / sizeof aq_parts[0]; i++)
        if (aq_parts[i].device == pci->device_id) part = &aq_parts[i];
    if (!part) return pci_probe_decline(pci, "not a part in this driver's table");
    g_probed = 1;
    g_aq.a2 = part->a2;
    g_aq.rates = part->rates;

    uint64_t bar = pci_bar_mem_addr(pci, 0);
    if (!bar || bar >= 0x100000000ull) return aq_decline(pci, "BAR0 is not memory below 4 GiB");
    g_aq.mmio = (volatile uint8_t *)(uintptr_t)bar;   // identity-mapped below 4 GiB

    if (!alloc_rings()) return aq_decline(pci, "not enough contiguous memory for the rings");
    pci_command_update(pci, PCI_CMD_MEMORY | PCI_CMD_BUS_MASTER, 0);

    const char *why = g_aq.a2 ? aq2_fw_boot() : aq1_fw_boot();
    if (why) return aq_decline(pci, why);

    // Interrupt block reset, then the firmware's own reset step.
    set_field(AQ_INTR_CTRL_REG, AQ_INTR_CTRL_RESET_DIS, 0);
    set_field(AQ_INTR_CTRL_REG, AQ_INTR_CTRL_RESET_IRQ, 1);
    if (!WAIT_MS(!(rd(AQ_INTR_CTRL_REG) & AQ_INTR_CTRL_RESET_IRQ), 10))
        return aq_decline(pci, "the interrupt block never came out of reset");
    why = g_aq.a2 ? aq2_fw_reset() : aq1_fw_reset();
    if (why) return aq_decline(pci, why);

    if (!(g_aq.a2 ? aq2_get_mac(g_dev.mac) : aq1_get_mac(g_dev.mac)))
        return aq_decline(pci, "the firmware gave no MAC address");

    // The vector first: the interrupt block's MODE follows what we got.
    wr(AQ_INTR_MASK_CLR_REG, 0xFFFFFFFFu);
    uint32_t mode = AQ_INTR_CTRL_IRQMODE_LEGACY;
    uint8_t line = pci_irq_line(pci);
    uint8_t vector = pci_msi_request(pci, aq_irq);
    if (vector) {
        mode = pci->irq_msix ? AQ_INTR_CTRL_IRQMODE_MSIX : AQ_INTR_CTRL_IRQMODE_MSI;
    } else if (line != IRQ_NONE) {
        g_aq.irq = line;
        irq_register_handler(line, aq_irq);
    }

    // --- hardware init (the BSDs' aq_hw_init) ---
    if (!g_aq.a2) {
        uint32_t v = rd(AQ_PCI_REG_CONTROL_6_REG);   // read requests capped at 2 KiB
        wr(AQ_PCI_REG_CONTROL_6_REG, (v & ~0x0707u) | 0x0404u);
        wr(AQ_HW_TX_DMA_TOTAL_REQ_LIMIT_REG, 24);
    } else {
        uint32_t fpga = rd(AQ2_HW_FPGA_VERSION_REG);
        set_field(AQ2_LAUNCHTIME_CTRL_REG, AQ2_LAUNCHTIME_CTRL_RATIO,
                  fpga < 0x01000000u ? 1 : fpga >= 0x01008502u ? 2 : 4);
    }
    init_tx_path();
    init_rx_path();
    set_own_mac(g_dev.mac);
    link_apply(0);
    init_qos();
    if (g_aq.a2) set_field(AQ2_RPF_NEW_CTRL_REG, AQ2_RPF_NEW_CTRL_ENABLE, 1);

    wr(AQ_INTR_CTRL_REG, AQ_INTR_CTRL_RESET_DIS);
    set_field(AQ_INTR_CTRL_REG, AQ_INTR_CTRL_MULTIVEC, 0);
    set_field(AQ_INTR_CTRL_REG, AQ_INTR_CTRL_IRQMODE, mode);
    wr(AQ_INTR_AUTOMASK_REG, 0xFFFFFFFFu);
    wr(AQ_GEN_INTR_MAP_REG(0), (AQ_B0_ERR_INT << 24) | (1u << 31) | (AQ_B0_ERR_INT << 16) | (1u << 23));
    wr(AQ_GEN_INTR_MAP_REG(3), (1u << 7) | IRQ_LINK);

    // Moderation: at most one receive interrupt per 12-120 us (units of 2 us).
    moderation_apply(NET_MOD_MEDIUM);
    set_field(TX_DMA_INT_DESC_WRWB_EN_REG, TX_DMA_INT_DESC_WRWB_EN, 0);
    set_field(TX_DMA_INT_DESC_WRWB_EN_REG, TX_DMA_INT_DESC_MODERATE_EN, 1);
    set_field(RX_DMA_INT_DESC_WRWB_EN_REG, RX_DMA_INT_DESC_WRWB_EN, 0);
    set_field(RX_DMA_INT_DESC_WRWB_EN_REG, RX_DMA_INT_DESC_MODERATE_EN, 1);

    // THE DEFAULTS the core keeps for "Restore defaults": every rate the
    // part has, no pause frames, MEDIUM moderation -- and EEE OFF, not the
    // firmware's on: on the desktop's switch a link that negotiated it came
    // up 2 boots in 12 receiving truncated frames with MACERR until the
    // cable was replugged, and 0 in 20 with it off.
    g_dev.link = (struct net_link_values){ g_aq.rates, 0, 0, NET_MOD_MEDIUM };
    g_dev.rates_supported = g_aq.rates;
    g_dev.link_caps = (g_aq.a2 || fw1_major() >= 2) ? NET_LINK_ALL : NET_LINK_RATES | NET_LINK_MODERATION;
    g_dev.set_link = aq_set_link;
    link_apply(&g_dev.link);

    // --- up (the BSDs' aq_up) ---
    rings_start();
    set_own_mac(g_dev.mac);
    wr(TPO_HWCSUM_REG, 0);   // no checksum offload above, so none here
    wr(RPO_HWCSUM_REG, 0);

    net_location_pci(&g_dev, pci->bus, pci->device, pci->function);
    g_dev.driver = "aq";
    g_dev.transmit = aq_transmit;
    g_dev.drv = &g_aq;
    g_dev.poll = aq_sweep;
    g_dev.poll_ms = 10;

    // Everything the handler reads is built before the commit point.
    g_present = 1;
    g_aq.imask = (1u << IRQ_RX) | (1u << IRQ_LINK);
    wr(AQ_INTR_STATUS_CLR_REG, 0xFFFFFFFFu);
    if (vector || g_aq.irq) wr(AQ_INTR_MASK_REG, g_aq.imask);
    if (g_aq.irq) irq_unmask(g_aq.irq);
    set_field(TPB_TX_BUF_REG, TPB_TX_BUF_EN, 1);
    set_field(RPB_RPF_RX_REG, RPB_RPF_RX_BUF_EN, 1);

    uint32_t fw = g_aq.fw_version;
    const char *irq = vector ? (pci->irq_msix ? "MSI-X" : "MSI") : g_aq.irq ? "IRQ" : "no interrupt";
    klog_printf("aq: %s, firmware %u.%u.%u, %s %u, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                g_aq.a2 ? "A2 (AQC113 family)" : "A1 (AQC107 family)",
                fw >> 24, (fw >> 16) & 0xFF, fw & 0xFFFF, irq,
                vector ? vector : g_aq.irq,
                g_dev.mac[0], g_dev.mac[1], g_dev.mac[2], g_dev.mac[3], g_dev.mac[4], g_dev.mac[5]);

    if (!net_register(&g_dev)) return aq_decline(pci, "the network core has no room for another card");
    update_link(&g_dev);
    return 0;
}

// The inverse, in the order that keeps the handler safe: the card's
// mask off, the handler unhooked, the engines stopped, the link handed
// back to the firmware as down, the device out of the stack, the memory
// back. Statics reset so a re-probe (`modload -r aq`) starts from nothing.
static void aq_remove(const struct pci_device *pci) {
    if (!g_probed) return;
    if (g_aq.mmio && g_present) {
        wr(AQ_INTR_MASK_CLR_REG, 0xFFFFFFFFu);
        wr(AQ_INTR_STATUS_CLR_REG, 0xFFFFFFFFu);
    }
    g_present = 0;
    if (pci->irq_vector) pci_msi_release(pci, pci->irq_vector);
    else if (g_aq.irq) irq_unregister_handler(g_aq.irq, aq_irq);
    if (g_aq.mmio && g_dev.transmit) {
        set_field(RPB_RPF_RX_REG, RPB_RPF_RX_BUF_EN, 0);
        set_field(TPB_TX_BUF_REG, TPB_TX_BUF_EN, 0);
        set_field(RX_DMA_DESC_REG(0), RX_DMA_DESC_EN, 0);
        set_field(TX_DMA_DESC_REG(0), TX_DMA_DESC_EN, 0);
        link_apply(0);
    }
    net_unregister(&g_dev);
    if (g_aq.rx) pmm_free_contiguous((uint64_t)(uintptr_t)g_aq.rx, 1);
    if (g_aq.tx) pmm_free_contiguous((uint64_t)(uintptr_t)g_aq.tx, 1);
    if (g_aq.rx_buf) pmm_free_contiguous(g_aq.rx_buf_phys, (RX_DESCS * BUF_SIZE) / 4096);
    if (g_aq.tx_buf) pmm_free_contiguous(g_aq.tx_buf_phys, (TX_DESCS * BUF_SIZE) / 4096);
    k_memset(&g_aq, 0, sizeof g_aq);
    k_memset(&g_dev, 0, sizeof g_dev);
    g_probed = 0;
}
PCI_DRIVER_REMOVABLE("aq", aq_matches, aq_probe, aq_remove);
