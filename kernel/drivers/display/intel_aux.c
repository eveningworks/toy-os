// The eDP panel's AUX channel (DDI A, gen8): DisplayPort native AUX and
// I2C-over-AUX, enough to read the panel's EDID and its DPCD. READ-ONLY
// as far as the panel is concerned -- nothing here writes a DPCD
// register, and an AUX transaction cannot disturb the pipe.
//
// THE INVARIANT: every wait is an iteration-bounded spin. This runs
// from the display probe, before the timer, and a pit_ticks() wait
// hangs the one machine that has this hardware.
//
// THE TRAP: the transaction's bit clock divider and precharge are the
// FIRMWARE's, read back from the control register -- the GOP has
// already used this channel to read the same EDID. Deriving them from
// the CDCLK is done only as a cross-check and a fallback, because a
// wrong divider does not fail loudly: it times out on every request.
#include "intel_internal.h"
#include "barrier.h"
#include "klog.h"
#include "kfmt.h"
#include "edid.h"
#include "string.h"

// driver-none: part of intel-display (intel_display.c declares it)

#define DPA_AUX_CH_CTL   0x64010
#define DPA_AUX_CH_DATA  0x64014   // five dwords, big-endian byte order
#define LCPLL_CTL        0x130040

#define AUX_SEND_BUSY     (1u << 31)
#define AUX_DONE          (1u << 30)
#define AUX_TIMEOUT_ERR   (1u << 28)
#define AUX_TIMEOUT_600US (1u << 26)   // Broadwell's value (i915)
#define AUX_RECV_ERR      (1u << 25)
#define AUX_MSG_SIZE(n)   ((uint32_t)(n) << 20)
#define AUX_MSG_SIZE_OF(v) (((v) >> 20) & 0x1F)
#define AUX_PRECHARGE(n)  ((uint32_t)(n) << 16)
#define AUX_PRECHARGE_OF(v) (((v) >> 16) & 0xF)
#define AUX_DIVIDER_MASK  0x7FFu
#define AUX_STATUS_BITS   (AUX_DONE | AUX_TIMEOUT_ERR | AUX_RECV_ERR)

// DP AUX request codes (DP 1.2 2.7.1)
#define AUX_I2C_WRITE   0x0
#define AUX_I2C_READ    0x1
#define AUX_I2C_MOT     0x4
#define AUX_NATIVE_WRITE 0x8
#define AUX_NATIVE_READ  0x9
// Reply codes, from the header byte's high nibble.
#define AUX_REPLY_NATIVE(h) (((h) >> 4) & 3)   // 0 ack, 1 nack, 2 defer
#define AUX_REPLY_I2C(h)    (((h) >> 6) & 3)   // 0 ack, 1 nack, 2 defer

#define AUX_MAX_BYTES 20
#define AUX_I2C_CHUNK 16
#define AUX_TRIES 5
#define AUX_DEFERS 8

static int g_ready;
static uint32_t g_divider, g_precharge;
static uint8_t g_edid_raw[EDID_BLOCK];
static int g_edid_len;

static void spin(int n) { for (int i = 0; i < n; i++) cpu_relax(); }

// Broadwell's CDCLK from LCPLL_CTL, in kHz (i915's bdw_get_cdclk).
static uint32_t cdclk_khz(void) {
    uint32_t v = intel_rd(LCPLL_CTL);
    if (v & (1u << 21)) return 800000;   // CD source is FCLK
    switch ((v >> 26) & 3) {
    case 0: return 450000;
    case 1: return 540000;
    case 2: return 337500;
    default: return 675000;
    }
}

static inline uint32_t pack(const uint8_t *b, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n && i < 4; i++) v |= (uint32_t)b[i] << ((3 - i) * 8);
    return v;
}

static inline void unpack(uint32_t v, uint8_t *b, int n) {
    for (int i = 0; i < n && i < 4; i++) b[i] = (uint8_t)(v >> ((3 - i) * 8));
}

// One AUX transaction: `tx` bytes out, up to `rxcap` back including
// the reply header. Returns the bytes received, or -1.
static int aux_xfer(const uint8_t *tx, int txn, uint8_t *rx, int rxcap) {
    if (!g_ready || txn < 3 || txn > AUX_MAX_BYTES) return -1;
    for (int attempt = 0; attempt < AUX_TRIES; attempt++) {
        int i;
        for (i = 0; i < 100000 && (intel_rd(DPA_AUX_CH_CTL) & AUX_SEND_BUSY); i++) cpu_relax();
        if (intel_rd(DPA_AUX_CH_CTL) & AUX_SEND_BUSY) return -1;
        for (int d = 0; d < 5; d++)
            intel_wr(DPA_AUX_CH_DATA + 4 * d, d * 4 < txn ? pack(tx + d * 4, txn - d * 4) : 0);
        // Status bits are write-1-to-clear, so the send word clears them.
        intel_wr(DPA_AUX_CH_CTL, AUX_SEND_BUSY | AUX_STATUS_BITS | AUX_TIMEOUT_600US |
                                 AUX_MSG_SIZE(txn) | AUX_PRECHARGE(g_precharge) | g_divider);
        uint32_t st = 0;
        for (i = 0; i < 1000000; i++) {
            st = intel_rd(DPA_AUX_CH_CTL);
            if (!(st & AUX_SEND_BUSY)) break;
            cpu_relax();
        }
        intel_wr(DPA_AUX_CH_CTL, st | AUX_STATUS_BITS);
        if (st & AUX_SEND_BUSY) return -1;   // the engine never finished
        if (st & (AUX_TIMEOUT_ERR | AUX_RECV_ERR)) { spin(20000); continue; }
        int n = (int)AUX_MSG_SIZE_OF(st);
        if (n < 1 || n > AUX_MAX_BYTES) { spin(20000); continue; }
        if (n > rxcap) n = rxcap;
        for (int d = 0; d * 4 < n; d++)
            unpack(intel_rd(DPA_AUX_CH_DATA + 4 * d), rx + d * 4, n - d * 4);
        return n;
    }
    return -1;
}

// One request with its DEFER retries. `read` selects the direction,
// `native` the DPCD space (else I2C at `addr`), `mot` keeps an I2C
// transaction open. Returns the DATA bytes moved (0 for an address-
// only request), or -1 on a NACK or a dead channel.
static int aux_request(int native, int read, int mot, uint32_t addr, uint8_t *buf, int len) {
    uint8_t tx[AUX_MAX_BYTES], rx[AUX_MAX_BYTES];
    if (len > AUX_I2C_CHUNK || len < 0) return -1;
    uint8_t req = native ? (read ? AUX_NATIVE_READ : AUX_NATIVE_WRITE)
                         : (uint8_t)((read ? AUX_I2C_READ : AUX_I2C_WRITE) | (mot ? AUX_I2C_MOT : 0));
    tx[0] = (uint8_t)((req << 4) | ((addr >> 16) & 0xF));
    tx[1] = (uint8_t)(addr >> 8);
    tx[2] = (uint8_t)addr;
    int txn = 3;
    if (len) {
        tx[3] = (uint8_t)(len - 1);
        txn = 4;
        if (!read) { k_memcpy(tx + 4, buf, (size_t)len); txn += len; }
    }
    for (int defer = 0; defer < AUX_DEFERS; defer++) {
        int n = aux_xfer(tx, txn, rx, sizeof rx);
        if (n < 1) return -1;
        int nat = AUX_REPLY_NATIVE(rx[0]);
        if (nat == 2) { spin(50000); continue; }        // native DEFER
        if (nat != 0) return -1;                          // native NACK
        if (!native) {
            int i2c = AUX_REPLY_I2C(rx[0]);
            if (i2c == 2) { spin(50000); continue; }    // I2C DEFER
            if (i2c != 0) return -1;                      // I2C NACK
        }
        if (!read) return len;
        int got = n - 1;
        if (got > len) got = len;
        if (got > 0) k_memcpy(buf, rx + 1, (size_t)got);
        return got;
    }
    return -1;
}

int intel_aux_native_read(uint32_t addr, uint8_t *buf, int len) {
    int pos = 0;
    while (pos < len) {
        int want = len - pos > AUX_I2C_CHUNK ? AUX_I2C_CHUNK : len - pos;
        int n = aux_request(1, 1, 0, addr + (uint32_t)pos, buf + pos, want);
        if (n <= 0) return pos ? pos : -1;
        pos += n;
    }
    return pos;
}

// I2C-over-AUX, the DDC read of the base block (drm_dp_i2c_xfer's
// shape): a bare address to open, the offset, 16-byte chunks with MOT
// held, then a bare address with MOT clear to close.
static int read_edid_block(uint8_t *out) {
    if (aux_request(0, 0, 1, 0x50, 0, 0) < 0) return 0;
    uint8_t off = 0;
    if (aux_request(0, 0, 1, 0x50, &off, 1) < 0) return 0;
    int pos = 0, empty = 0;
    while (pos < EDID_BLOCK) {
        int want = EDID_BLOCK - pos > AUX_I2C_CHUNK ? AUX_I2C_CHUNK : EDID_BLOCK - pos;
        int n = aux_request(0, 1, 1, 0x50, out + pos, want);
        if (n < 0) break;
        if (n == 0 && ++empty > AUX_DEFERS) break;
        pos += n;
    }
    aux_request(0, 1, 0, 0x50, 0, 0);   // close, whatever happened
    return pos;
}

static void log_dpcd(void) {
    uint8_t d[16];
    int n = intel_aux_native_read(0x000, d, 16);
    if (n < 16) { klog_printf("intel-display: DPCD read failed (%d)\n", n); return; }
    klog_printf("intel-display: DPCD rev %d.%d, max link %d.%02d Gbps, max %d lanes%s, edp cap %#x, aux interval %#x\n",
                d[0] >> 4, d[0] & 0xF, (d[1] * 27) / 100, (d[1] * 27) % 100,
                d[2] & 0x1F, (d[2] & 0x80) ? " (enhanced framing)" : "", d[0xD], d[0xE]);
    uint8_t l[4];
    if (intel_aux_native_read(0x100, l, 4) == 4)
        klog_printf("intel-display: DPCD link set: bw %#x lanes %#x pattern %#x\n", l[0], l[1] & 0x1F, l[2]);
    uint8_t st[4];
    if (intel_aux_native_read(0x202, st, 3) == 3)
        klog_printf("intel-display: DPCD lane status %02x %02x align %02x\n", st[0], st[1], st[2]);
}

void intel_aux_init(void) {
    g_ready = 0;
    g_edid_len = 0;
    uint32_t ctl = intel_rd(DPA_AUX_CH_CTL);
    uint32_t derived = (cdclk_khz() + 1000) / 2000;   // i915's hsw divider for port A
    g_divider = ctl & AUX_DIVIDER_MASK;
    g_precharge = AUX_PRECHARGE_OF(ctl);
    if (!g_divider) g_divider = derived;
    if (!g_precharge) g_precharge = 3;
    klog_printf("intel-display: aux ctl %#x -- divider %u (cdclk %u kHz says %u), precharge %u\n",
                ctl, g_divider, cdclk_khz(), derived, g_precharge);
    g_ready = 1;
}

int intel_aux_read_edid(uint8_t *out, int cap) {
    if (!g_ready || !out || cap <= 0) return 0;
    if (!g_edid_len) {
        g_edid_len = read_edid_block(g_edid_raw);
        klog_printf("intel-display: EDID over AUX: %d bytes\n", g_edid_len);
        log_dpcd();
        // Stage 2 of the modesetting plan: decode what the firmware
        // programmed and say whether it is this timing.
        struct display_edid e;
        intel_readout_log(g_edid_len == EDID_BLOCK && edid_parse(g_edid_raw, g_edid_len, &e) ? &e : 0);
    }
    int n = g_edid_len < cap ? g_edid_len : cap;
    k_memcpy(out, g_edid_raw, (size_t)n);
    return n;
}
