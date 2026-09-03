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
        klog_printf("intel-display: %s: the panel is not on the EDP transcoder -- refused\n", what);
        return 0;
    }
    klog_printf("intel-display: %s: transconf %#x func %#x plane %#x buf ctl %#x tp ctl %#x\n",
                what, intel_rd(TRANSCONF_EDP), intel_rd(TRANS_DDI_FUNC_CTL_EDP),
                intel_rd(DSPCNTR(st->pipe)), intel_rd(DDI_BUF_CTL_A), intel_rd(DP_TP_CTL_A));
    return 1;
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
