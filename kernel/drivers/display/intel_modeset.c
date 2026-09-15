// Stage 3 of Intel modesetting, built up one mechanism per flash: the
// pipe cycle (transcoder and pipe off and on), then the link retrain
// (the DDI buffer dropped and DP link training run again).
//
// THE INVARIANT: every step reads back what it wrote and logs it, every
// wait is bounded by an iteration count as well as by time (this runs
// inside a syscall on the laptop, and a PIT-backed clock does not
// advance there), and the exit criterion is a readback -- the pipe's
// state bit, the frame counter, the panel's lane status -- never "the
// panel looks right".
//
// THE TRAP: the panel is on the EDP transcoder, so the enable bit that
// matters is TRANSCONF_EDP's, not pipe A's PIPECONF -- which reads as
// state-only on this machine and would take a write silently.
//
// Link training follows DP 1.2 section 3.5.1.2 in i915's shape
// (intel_dp_link_training.c): clock recovery on pattern 1, channel
// equalisation on pattern 2, the sink's adjust requests honoured up to
// five tries at one level, then the idle and normal patterns. The DDI
// buffer translation table is the FIRMWARE's, never rewritten; a level
// is an index into it.
#include "intel_internal.h"
#include "intel_display.h"
#include "display.h"
#include "edid.h"
#include "barrier.h"
#include "clocksource.h"
#include "klog.h"
#include "kfmt.h"

// driver-none: part of intel-display (intel_display.c declares it)

#define STATE_SPINS 8000000

#define DDI_BUF_ENABLE        (1u << 31)
#define DDI_BUF_TRANS_SEL(n)  ((uint32_t)(n) << 24)
#define DDI_BUF_TRANS_MASK    (0xFu << 24)
#define DP_TP_ENABLE          (1u << 31)
#define DP_TP_ENHANCED        (1u << 18)
#define DP_TP_TRAIN_MASK      (7u << 8)
#define DP_TP_TRAIN_PAT1      (0u << 8)
#define DP_TP_TRAIN_PAT2      (1u << 8)
#define DP_TP_TRAIN_IDLE      (2u << 8)
#define DP_TP_TRAIN_NORMAL    (3u << 8)

// DPCD addresses (DP 1.2 table 2-83 onward).
#define DPCD_LINK_BW_SET        0x100
#define DPCD_LANE_COUNT_SET     0x101   // bit 7: enhanced framing
#define DPCD_TRAINING_PATTERN   0x102   // 1/2 | 0x20 scrambling off; 0 = done
#define DPCD_TRAINING_LANE0_SET 0x103   // swing 1:0, max-swing 2, pe 4:3, max-pe 5
#define DPCD_LANE0_1_STATUS     0x202   // per lane nibble: CR bit0, EQ bit1, SYM bit2
#define DPCD_LANE_ALIGN_STATUS  0x204   // bit 0 interlane align done
#define DPCD_ADJUST_REQUEST     0x206   // per lane nibble: swing 1:0, pe 3:2

#define EDP_MAX_SWING 2   // the eDP translation table has 9 entries: swing 0..2
static const uint8_t MAX_PE_AT[3] = { 3, 2, 1 };

// Bounded both ways: by time when the clock advances, by count when it
// does not (see the header comment).
static void udelay(uint32_t us) {
    uint64_t t0 = clocksource_now_ns(), want = (uint64_t)us * 1000;
    for (uint32_t i = 0; i < us * 4000u; i++) {
        if (clocksource_now_ns() - t0 >= want) return;
        cpu_relax();
    }
}

static int wait_state(uint32_t reg, uint32_t bit, int want) {
    uint32_t v = 0;
    for (int i = 0; i < STATE_SPINS; i++) {
        v = intel_rd(reg);
        if (((v & bit) != 0) == (want != 0)) return 1;
        cpu_relax();
    }
    klog_printf("intel-display: register %#x state never became %s (%#x)\n", reg, want ? "on" : "off", v);
    return 0;
}

// What the pipe cycle saves and restores around the off/on halves.
struct pipe_state {
    int pipe;
    uint32_t func, cntr, surf, cur, curbase, frame0;
};

static int pipe_off(struct pipe_state *st) {
    int pipe = st->pipe;
    st->func = intel_rd(TRANS_DDI_FUNC_CTL_EDP);
    st->cntr = intel_rd(DSPCNTR(pipe)); st->surf = intel_rd(DSPSURF(pipe));
    st->cur = intel_rd(CURCNTR(pipe)); st->curbase = intel_rd(CURBASE(pipe));
    st->frame0 = intel_rd(PIPEFRAME(pipe));
    // Planes, then the pipe (wait for its state), then the transcoder's
    // DDI function -- i915's hsw_crtc_disable order.
    intel_wr(CURCNTR(pipe), 0);
    intel_wr(CURBASE(pipe), st->curbase);
    intel_wr(DSPCNTR(pipe), st->cntr & ~DSPCNTR_ENABLE);
    intel_wr(DSPSURF(pipe), st->surf);
    intel_wr(TRANSCONF_EDP, intel_rd(TRANSCONF_EDP) & ~PIPECONF_ENABLE);
    int off = wait_state(TRANSCONF_EDP, PIPECONF_STATE, 0);
    intel_wr(TRANS_DDI_FUNC_CTL_EDP, st->func & ~TRANS_DDI_FUNC_ENABLE);
    klog_printf("intel-display: pipe off: transconf %#x func %#x (state %s)\n",
                intel_rd(TRANSCONF_EDP), intel_rd(TRANS_DDI_FUNC_CTL_EDP), off ? "off" : "STUCK");
    return off;
}

static int pipe_on(struct pipe_state *st) {
    int pipe = st->pipe;
    intel_wr(TRANS_DDI_FUNC_CTL_EDP, st->func);
    intel_wr(TRANSCONF_EDP, intel_rd(TRANSCONF_EDP) | PIPECONF_ENABLE);
    int on = wait_state(TRANSCONF_EDP, PIPECONF_STATE, 1);
    intel_wr(DSPCNTR(pipe), st->cntr);
    intel_wr(DSPSURF(pipe), st->surf);
    intel_wr(CURCNTR(pipe), st->cur);
    intel_wr(CURBASE(pipe), st->curbase);
    for (int i = 0; i < STATE_SPINS && intel_rd(PIPEFRAME(pipe)) == st->frame0; i++) cpu_relax();
    uint32_t frame1 = intel_rd(PIPEFRAME(pipe));
    uint8_t s[3] = { 0, 0, 0 };
    int dp = intel_aux_native_read(DPCD_LANE0_1_STATUS, s, 3);
    int trained = dp == 3 && (s[0] & 0x77) == 0x77;
    klog_printf("intel-display: pipe on: %s -- transconf %#x func %#x plane %#x frame %u -> %u, lane status %02x %02x align %02x (%s)\n",
                on && trained && frame1 != st->frame0 ? "back" : "NOT BACK", intel_rd(TRANSCONF_EDP),
                intel_rd(TRANS_DDI_FUNC_CTL_EDP), intel_rd(DSPCNTR(pipe)), st->frame0, frame1,
                s[0], s[1], s[2], dp == 3 ? (trained ? "link trained" : "LINK LOST") : "aux failed");
    return on && trained && frame1 != st->frame0;
}

static int begin(struct pipe_state *st, const char *what) {
    st->pipe = intel_display_pipe();
    if (st->pipe < 0) return 0;
    if (!(intel_rd(TRANS_DDI_FUNC_CTL_EDP) & TRANS_DDI_FUNC_ENABLE)) {
        klog_printf(KLOG_ERR "intel-display: %s: the panel is not on the EDP transcoder -- refused\n", what);
        return 0;
    }
    klog_printf("intel-display: %s: transconf %#x func %#x plane %#x buf ctl %#x tp ctl %#x\n",
                what, intel_rd(TRANSCONF_EDP), intel_rd(TRANS_DDI_FUNC_CTL_EDP),
                intel_rd(DSPCNTR(st->pipe)), intel_rd(DDI_BUF_CTL_A), intel_rd(DP_TP_CTL_A));
    return 1;
}

// The sink's link configuration block (DPCD 0x100..0x10F) and its power
// state (0x600), logged around a power cycle (this panel keeps them).
static int read_link_config(uint8_t *cfg, uint8_t *power, const char *when) {
    int n = intel_aux_native_read(0x100, cfg, 16);
    int m = intel_aux_native_read(0x600, power, 1);
    klog_printf("intel-display: dpcd %s: 100-10f %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x, 600 %02x (%d/%d)\n",
                when, cfg[0], cfg[1], cfg[2], cfg[3], cfg[4], cfg[5], cfg[6], cfg[7], cfg[8], cfg[9],
                cfg[10], cfg[11], cfg[12], cfg[13], cfg[14], cfg[15], *power, n, m);
    return n == 16 && m == 1;
}

int intel_modeset_pipe_cycle(void) {
    struct pipe_state st;
    if (!begin(&st, "pipe cycle")) return 0;
    int off = pipe_off(&st);
    int on = pipe_on(&st);
    return off && on;
}

// --- link training -----------------------------------------------------

static uint8_t level_of(int swing, int pe) {
    static const uint8_t base[3] = { 0, 4, 7 };
    return (uint8_t)(base[swing] + pe);
}

static void set_signal(uint32_t buf, int lanes, int swing, int pe) {
    intel_wr(DDI_BUF_CTL_A, (buf & ~DDI_BUF_TRANS_MASK) | DDI_BUF_TRANS_SEL(level_of(swing, pe)));
    uint8_t v = (uint8_t)(swing | (pe << 3) | (swing == EDP_MAX_SWING ? 0x04 : 0) |
                          (pe == MAX_PE_AT[swing] ? 0x20 : 0));
    uint8_t lane[4] = { v, v, v, v };
    intel_aux_native_write(DPCD_TRAINING_LANE0_SET, lane, lanes);
}

// The sink's requests, folded to one level for every lane (i915 does
// the same: the highest asked for), clamped to the table.
static void adjust(int lanes, int *swing, int *pe) {
    uint8_t adj[2] = { 0, 0 };
    if (intel_aux_native_read(DPCD_ADJUST_REQUEST, adj, (lanes + 1) / 2) <= 0) return;
    int s = 0, p = 0;
    for (int l = 0; l < lanes; l++) {
        uint8_t n = (uint8_t)((adj[l / 2] >> ((l & 1) * 4)) & 0xF);
        if ((n & 3) > s) s = n & 3;
        if (((n >> 2) & 3) > p) p = (n >> 2) & 3;
    }
    if (s > EDP_MAX_SWING) s = EDP_MAX_SWING;
    if (p > MAX_PE_AT[s]) p = MAX_PE_AT[s];
    *swing = s;
    *pe = p;
}

static int cr_done(const uint8_t *s, int lanes) {
    for (int l = 0; l < lanes; l++)
        if (!((s[l / 2] >> ((l & 1) * 4)) & 0x1)) return 0;
    return 1;
}

static int eq_done(const uint8_t *s, int lanes) {
    for (int l = 0; l < lanes; l++)
        if (((s[l / 2] >> ((l & 1) * 4)) & 0x7) != 0x7) return 0;
    return (s[2] & 0x1) != 0;
}

static int train(uint32_t buf, uint32_t tp, int lanes, uint8_t bw, int enhanced) {
    uint8_t set[2] = { bw, (uint8_t)(lanes | (enhanced ? 0x80 : 0)) };
    if (intel_aux_native_write(DPCD_LINK_BW_SET, set, 2) != 2) {
        klog_write("intel-display: link retrain: DPCD link settings not written\n");
        return 0;
    }
    // Pattern 1 on the port, the buffer on at level 0, 600 us to settle.
    uint32_t tp_base = (tp & ~(DP_TP_TRAIN_MASK | DP_TP_ENHANCED)) | DP_TP_ENABLE | (enhanced ? DP_TP_ENHANCED : 0);
    intel_wr(DP_TP_CTL_A, tp_base | DP_TP_TRAIN_PAT1);
    int swing = 0, pe = 0;
    set_signal(buf | DDI_BUF_ENABLE, lanes, swing, pe);
    udelay(600);
    uint8_t pat = 0x21;
    intel_aux_native_write(DPCD_TRAINING_PATTERN, &pat, 1);
    set_signal(buf | DDI_BUF_ENABLE, lanes, swing, pe);

    uint8_t st[3];
    int same = 0, tries = 0, ok = 0;
    for (tries = 0; tries < 10; tries++) {
        udelay(100);
        if (intel_aux_native_read(DPCD_LANE0_1_STATUS, st, 2) != 2) break;
        if (cr_done(st, lanes)) { ok = 1; break; }
        int ns = swing, np = pe;
        adjust(lanes, &ns, &np);
        if (ns == swing && np == pe) { if (++same >= 5) break; } else same = 0;
        if (swing == EDP_MAX_SWING && ns == swing) break;
        swing = ns; pe = np;
        set_signal(buf | DDI_BUF_ENABLE, lanes, swing, pe);
    }
    klog_printf("intel-display: link retrain: clock recovery %s after %d tries at swing %d pre-emphasis %d (status %02x %02x)\n",
                ok ? "done" : "FAILED", tries + 1, swing, pe, st[0], st[1]);
    if (!ok) return 0;

    intel_wr(DP_TP_CTL_A, tp_base | DP_TP_TRAIN_PAT2);
    pat = 0x22;
    intel_aux_native_write(DPCD_TRAINING_PATTERN, &pat, 1);
    ok = 0;
    for (tries = 0; tries < 5; tries++) {
        udelay(400);
        if (intel_aux_native_read(DPCD_LANE0_1_STATUS, st, 3) != 3) break;
        if (!cr_done(st, lanes)) break;   // lost clock recovery: i915 restarts; we report
        if (eq_done(st, lanes)) { ok = 1; break; }
        adjust(lanes, &swing, &pe);
        set_signal(buf | DDI_BUF_ENABLE, lanes, swing, pe);
    }
    klog_printf("intel-display: link retrain: channel equalisation %s after %d tries at swing %d pre-emphasis %d (status %02x %02x align %02x)\n",
                ok ? "done" : "FAILED", tries + 1, swing, pe, st[0], st[1], st[2]);

    // Training off at the sink first, then idle, then the normal pattern
    // (port A has no DP_TP_STATUS to poll for idle: 1 ms, as i915 does).
    pat = 0;
    intel_aux_native_write(DPCD_TRAINING_PATTERN, &pat, 1);
    intel_wr(DP_TP_CTL_A, tp_base | DP_TP_TRAIN_IDLE);
    udelay(1000);
    intel_wr(DP_TP_CTL_A, tp_base | DP_TP_TRAIN_NORMAL);
    return ok;
}

int intel_modeset_link_retrain(void) {
    struct pipe_state st;
    if (!begin(&st, "link retrain")) return 0;
    uint8_t cfg[16] = { 0 }, power = 0;
    read_link_config(cfg, &power, "before");
    uint32_t buf = intel_rd(DDI_BUF_CTL_A), tp = intel_rd(DP_TP_CTL_A);
    int lanes = (int)((intel_rd(TRANS_DDI_FUNC_CTL_EDP) >> 1) & 7) + 1;
    uint8_t cur[2] = { 0, 0 };
    intel_aux_native_read(DPCD_LINK_BW_SET, cur, 2);
    uint8_t bw = cur[0] ? cur[0] : 0x0A;
    int enhanced = (tp & DP_TP_ENHANCED) != 0;
    klog_printf("intel-display: link retrain: %d lane(s) at bw %#x%s, buf ctl %#x tp ctl %#x\n",
                lanes, bw, enhanced ? " enhanced" : "", buf, tp);

    int off = pipe_off(&st);
    // The port down: buffer off, then DP_TP to pattern 1 and disabled
    // (intel_disable_ddi_buf); port A has no idle bit, so 100 us.
    intel_wr(DDI_BUF_CTL_A, buf & ~DDI_BUF_ENABLE);
    intel_wr(DP_TP_CTL_A, (tp & ~(DP_TP_ENABLE | DP_TP_TRAIN_MASK)) | DP_TP_TRAIN_PAT1);
    udelay(100);
    klog_printf("intel-display: link retrain: port down (buf ctl %#x tp ctl %#x)\n",
                intel_rd(DDI_BUF_CTL_A), intel_rd(DP_TP_CTL_A));

    int trained = train(buf & ~DDI_BUF_ENABLE, tp, lanes, bw, enhanced);
    klog_printf("intel-display: link retrain: port up (buf ctl %#x tp ctl %#x)\n",
                intel_rd(DDI_BUF_CTL_A), intel_rd(DP_TP_CTL_A));
    int on = pipe_on(&st);
    return off && trained && on;
}

// --- the native mode, end to end --------------------------------------
// Panel power off, port clock off, the sequencer's cycle delay, port
// clock on, panel power on, link training, the transcoder timings and
// M/N written from the EDID, pipe on, backlight on: the sequence a
// set_mode will run, exercised first on the mode already on screen so
// the readout can say MATCHES afterwards.

#define PCH_PP_STATUS_R  0xC7200
#define PCH_PP_CONTROL_R 0xC7204
#define PCH_PP_DIVISOR_R 0xC7210
#define PP_ON            (1u << 0)
#define PP_RESET         (1u << 1)
#define PP_BLC_ENABLE    (1u << 2)
#define PP_FORCE_VDD     (1u << 3)
#define PP_UNLOCK        0xABCD0000u   // LPT's PANEL_UNLOCK_REGS, bits 31:16
#define PP_STATUS_ON     (1u << 31)
#define PP_STATUS_SEQ    (3u << 28)
#define PP_STATUS_CYCLE  (1u << 27)   // the power-cycle (T12) delay is running
#define PORT_CLK_SEL_A_R 0x46100
#define PORT_CLK_NONE    (7u << 29)
#define TRANS_EDP_BASE_R 0x6F000
#define DATA_LINK_N_MAX  0x800000u
#define M_N_MASK         0xFFFFFFu

// Waits on a PP_STATUS condition for up to ~2 s (the sequencer's T12
// is 500 ms on this panel), bounded by count as well.
static int wait_pp(uint32_t mask, uint32_t want) {
    uint32_t v = 0;
    for (int i = 0; i < 2000; i++) {
        v = intel_rd(PCH_PP_STATUS_R);
        if ((v & mask) == want) return 1;
        udelay(1000);
    }
    klog_printf("intel-display: panel power status %#x never reached %#x/%#x\n", v, mask, want);
    return 0;
}

static uint32_t pp_control(void) { return (intel_rd(PCH_PP_CONTROL_R) & 0xFFFFu) | PP_UNLOCK; }

// i915's compute_m_n: n is the power of two at or above the divisor,
// capped; m scaled to it; both shifted down together if either overflows.
static void compute_m_n(uint32_t m_in, uint32_t n_in, uint32_t *m, uint32_t *n) {
    uint32_t nn = 1;
    while (nn < n_in && nn < DATA_LINK_N_MAX) nn <<= 1;
    uint32_t mm = (uint32_t)(((uint64_t)m_in * nn) / n_in);
    while (mm > M_N_MASK || nn > M_N_MASK) { mm >>= 1; nn >>= 1; }
    *m = mm; *n = nn;
}

static uint32_t timing_reg(uint32_t start, uint32_t end) { return ((end - 1) << 16) | (start - 1); }

int intel_modeset_native(void) {
    struct pipe_state st;
    if (!begin(&st, "native modeset")) return 0;
    const struct display_edid *e = display_edid();
    if (!e || e->timing_count < 1) {
        klog_write(KLOG_ERR "intel-display: native modeset: no EDID timing to program -- refused\n");
        return 0;
    }
    const struct edid_timing *t = &e->timing[0];
    uint8_t cfg[16] = { 0 }, power = 0;
    int have_cfg = read_link_config(cfg, &power, "before");
    uint32_t buf = intel_rd(DDI_BUF_CTL_A), tp = intel_rd(DP_TP_CTL_A);
    uint32_t clk = intel_rd(PORT_CLK_SEL_A_R);
    uint32_t func = intel_rd(TRANS_DDI_FUNC_CTL_EDP);
    int lanes = (int)((func >> 1) & 7) + 1;
    static const uint8_t BPC[] = { 8, 10, 6, 12 };
    uint32_t bpp = 3u * BPC[(func >> 20) & 3];
    uint8_t cur[2] = { 0, 0 };
    intel_aux_native_read(DPCD_LINK_BW_SET, cur, 2);
    uint8_t bw = cur[0] ? cur[0] : 0x0A;
    int enhanced = (tp & DP_TP_ENHANCED) != 0;
    uint32_t link_khz = intel_display_port_clock_khz(clk);
    if (!link_khz) {
        klog_printf(KLOG_ERR "intel-display: native modeset: port clock %#x is not an LCPLL tap -- refused\n", clk);
        return 0;
    }

    // What the EDID says the transcoder should hold, beside what it does.
    uint32_t htotal = edid_htotal(t), vtotal = edid_vtotal(t);
    uint32_t regs[6] = {
        timing_reg(t->hactive, htotal), timing_reg(t->hactive, htotal),
        timing_reg(t->hactive + t->hsync_off, t->hactive + t->hsync_off + t->hsync_w),
        timing_reg(t->vactive, vtotal), timing_reg(t->vactive, vtotal),
        timing_reg(t->vactive + t->vsync_off, t->vactive + t->vsync_off + t->vsync_w),
    };
    uint32_t data_m, data_n, link_m, link_n;
    compute_m_n(bpp * t->pixel_khz, link_khz * (uint32_t)lanes * 8, &data_m, &data_n);
    compute_m_n(t->pixel_khz, link_khz, &link_m, &link_n);
    uint32_t tu = 64;
    klog_printf("intel-display: native modeset: %ux%u %u kHz %u bpp %d lane(s) on %u kHz -> timings %#x %#x %#x %#x %#x %#x, data m/n %#x/%#x link m/n %#x/%#x (firmware %#x %#x %#x %#x %#x %#x, %#x/%#x %#x/%#x)\n",
                t->hactive, t->vactive, t->pixel_khz, bpp, lanes, link_khz,
                regs[0], regs[1], regs[2], regs[3], regs[4], regs[5],
                data_m, data_n, link_m, link_n,
                intel_rd(TRANS_EDP_BASE_R + 0x00), intel_rd(TRANS_EDP_BASE_R + 0x04),
                intel_rd(TRANS_EDP_BASE_R + 0x08), intel_rd(TRANS_EDP_BASE_R + 0x0C),
                intel_rd(TRANS_EDP_BASE_R + 0x10), intel_rd(TRANS_EDP_BASE_R + 0x14),
                intel_rd(TRANS_EDP_BASE_R + 0x30) & M_N_MASK, intel_rd(TRANS_EDP_BASE_R + 0x34),
                intel_rd(TRANS_EDP_BASE_R + 0x40), intel_rd(TRANS_EDP_BASE_R + 0x44));

    // Down: backlight, pipe, port, panel power, port clock.
    intel_wr(PCH_PP_CONTROL_R, pp_control() & ~PP_BLC_ENABLE);
    int off = pipe_off(&st);
    intel_wr(DDI_BUF_CTL_A, buf & ~DDI_BUF_ENABLE);
    intel_wr(DP_TP_CTL_A, (tp & ~(DP_TP_ENABLE | DP_TP_TRAIN_MASK)) | DP_TP_TRAIN_PAT1);
    udelay(100);
    intel_wr(PCH_PP_CONTROL_R, pp_control() & ~(PP_ON | PP_RESET | PP_FORCE_VDD | PP_BLC_ENABLE));
    int pp_off = wait_pp(PP_STATUS_ON | PP_STATUS_SEQ, 0);
    intel_wr(PORT_CLK_SEL_A_R, PORT_CLK_NONE);
    klog_printf("intel-display: native modeset: down -- pp status %#x control %#x port clk %#x\n",
                intel_rd(PCH_PP_STATUS_R), intel_rd(PCH_PP_CONTROL_R), intel_rd(PORT_CLK_SEL_A_R));

    // The sequencer's own power-cycle delay before the panel may come
    // back (T12); the status bit says while it runs.
    int cycled = wait_pp(PP_STATUS_CYCLE, 0);
    udelay(20000);

    // Up: port clock, panel power (VDD comes with it), the link, the
    // transcoder's registers, the pipe, the backlight.
    intel_wr(PORT_CLK_SEL_A_R, clk);
    udelay(20);
    intel_wr(PCH_PP_CONTROL_R, pp_control() | PP_ON | PP_RESET);
    int pp_on = wait_pp(PP_STATUS_ON | PP_STATUS_SEQ, PP_STATUS_ON);
    // T3: the firmware left PP_ON_DELAYS at zero, so the sequencer says
    // ON before the panel can answer AUX (it did, and training failed
    // on its first DPCD write). The spec's T1+T3 ceiling is 210 ms; the
    // observable is the DPCD answering, polled up to twice that.
    int aux_ready = 0, aux_ms = 0;
    for (; aux_ms < 420 && !aux_ready; aux_ms += 10) {
        udelay(10000);
        uint8_t rev = 0;
        aux_ready = intel_aux_native_read(0x000, &rev, 1) == 1;
    }
    klog_printf("intel-display: native modeset: panel %s -- pp status %#x control %#x port clk %#x, aux %s after %d ms\n",
                pp_on ? "on" : "NOT ON", intel_rd(PCH_PP_STATUS_R), intel_rd(PCH_PP_CONTROL_R),
                intel_rd(PORT_CLK_SEL_A_R), aux_ready ? "ready" : "SILENT", aux_ms);
    // THE TRAP: AUX answers ~30 ms after power-on, and training started
    // then fails with no adjust request -- the panel needs the rest of
    // T1+T3 (210 ms by the spec) before it will train. Measured: the
    // DPCD block below survives the reset unchanged; rewriting it is
    // what i915 does and costs nothing, but the wait is the fix.
    if (aux_ms < 210) udelay((uint32_t)(210 - aux_ms) * 1000);
    if (aux_ready) {
        uint8_t after[16] = { 0 }, pw = 0;
        read_link_config(after, &pw, "after power on");
        uint8_t d0 = 1;
        for (int i = 0; i < 3 && intel_aux_native_write(0x600, &d0, 1) != 1; i++) udelay(1000);
        if (have_cfg) {
            uint8_t restore[16];
            for (int i = 0; i < 16; i++) restore[i] = cfg[i];
            restore[2] = 0;   // TRAINING_PATTERN_SET: train() owns it
            intel_aux_native_write(0x103, restore + 3, 13);
            intel_aux_native_write(0x100, restore, 2);
        }
        read_link_config(after, &pw, "restored");
    }
    int trained = aux_ready && train(buf & ~DDI_BUF_ENABLE, tp, lanes, bw, enhanced);

    intel_wr(TRANS_EDP_BASE_R + 0x00, regs[0]);
    intel_wr(TRANS_EDP_BASE_R + 0x04, regs[1]);
    intel_wr(TRANS_EDP_BASE_R + 0x08, regs[2]);
    intel_wr(TRANS_EDP_BASE_R + 0x0C, regs[3]);
    intel_wr(TRANS_EDP_BASE_R + 0x10, regs[4]);
    intel_wr(TRANS_EDP_BASE_R + 0x14, regs[5]);
    intel_wr(TRANS_EDP_BASE_R + 0x30, ((tu - 1) << 25) | data_m);
    intel_wr(TRANS_EDP_BASE_R + 0x34, data_n);
    intel_wr(TRANS_EDP_BASE_R + 0x40, link_m);
    intel_wr(TRANS_EDP_BASE_R + 0x44, link_n);
    // PIPESRC and the fitter window are left as they are: they describe
    // the mode on screen, which may be a fitted one (intel_modeset_fit).
    int on = pipe_on(&st);
    udelay(50000);   // T8, the backlight's own delay, also zero in PP_ON_DELAYS here
    intel_wr(PCH_PP_CONTROL_R, pp_control() | PP_BLC_ENABLE);
    klog_printf("intel-display: native modeset: %s (pipe off %d, panel off %d, cycled %d, panel on %d, trained %d, pipe on %d)\n",
                off && pp_off && pp_on && trained && on ? "done" : "INCOMPLETE",
                off, pp_off, cycled, pp_on, trained, on);
    return off && pp_off && pp_on && trained && on;
}

// --- a fitted mode -----------------------------------------------------
// The transcoder keeps the panel's native timing; PIPESRC is the mode
// and the panel fitter scales it into a window. The fitter is already
// enabled in pass-through by the firmware on this panel, so this is a
// window change under a pipe cycle, and the eye sees a blink.

int intel_modeset_fit(uint32_t w, uint32_t h, uint32_t x, uint32_t y, uint32_t ww, uint32_t wh) {
    struct pipe_state st;
    if (!begin(&st, "fit")) return 0;
    int pipe = st.pipe;
    uint32_t pf = intel_rd(PF_CTL(pipe));
    klog_printf("intel-display: fit: %ux%u into %ux%u at %u,%u (pf ctl %#x, pipesrc %#x)\n",
                w, h, ww, wh, x, y, pf, intel_rd(PIPESRC(pipe)));
    int off = pipe_off(&st);
    // THE TRAP: PF_WIN_SZ is the ARMING write -- the fitter takes its
    // control and position with the size, DSPSURF-style -- so the order
    // is CTL, POS, SZ (i915's ilk_pfit_enable). Written CTL-last, a
    // 1600x900 source sat unscaled at the top-left with the registers
    // reading back exactly as asked. Disabled first, as i915 does.
    intel_wr(PF_CTL(pipe), 0);
    intel_wr(PF_WIN_POS(pipe), 0);
    intel_wr(PF_WIN_SZ(pipe), 0);
    intel_wr(PIPESRC(pipe), ((w - 1) << 16) | (h - 1));
    intel_wr(PF_CTL(pipe), pf | PF_ENABLE);
    intel_wr(PF_WIN_POS(pipe), (x << 16) | y);
    intel_wr(PF_WIN_SZ(pipe), (ww << 16) | wh);
    int on = pipe_on(&st);
    klog_printf("intel-display: fit: %s -- pipesrc %#x pf ctl %#x pos %#x size %#x\n",
                off && on ? "done" : "INCOMPLETE", intel_rd(PIPESRC(pipe)),
                intel_rd(PF_CTL(pipe)), intel_rd(PF_WIN_POS(pipe)), intel_rd(PF_WIN_SZ(pipe)));
    return off && on;
}
