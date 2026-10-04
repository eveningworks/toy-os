// Gen9 HDMI modeset: the transcoder, the DDI port and its PLL turned off
// and back on at a timing of our choosing -- stage 4 of the Kaby Lake
// desktop's plan (docs/roadmap-details.md). Three nested levels, each a
// kernel.intel_cycle mechanism verified on the hardware before the next
// was trusted, the way the laptops' eDP modeset was built:
//
//   pipe   -- planes, transcoder and its DDI function off and on;
//   port   -- the DDI buffer too (HDMI has no link training);
//   native -- the PLL too, its dividers recomputed from the timing's
//             pixel clock (intel_display_gen9_wrpll()).
//
// A REAL lower mode is the native level with another timing (a DMT one
// the monitor lists). The order is i915's for an HSW+ DDI in HDMI mode:
// disable planes, pipe, transcoder function, buffer, clock, PLL; enable
// PLL, clock select, timings, transcoder function, pipe, THEN the buffer
// (intel_enable_ddi_hdmi() turns the buffer on after the transcoder).
//
// What is NOT reprogrammed, and why that is safe: the DDI buffer
// translations (the firmware's HDMI entry stays in the registers),
// TRANS_CLK_SEL (the same port), the watermarks and DDB (a lower mode
// fetches less) and CDCLK (675 MHz on the desktop, far above any clock
// this sets). A mode above the firmware's would need all four.
#include "intel_internal.h"
#include "intel_display.h"
#include "edid.h"
#include "clocksource.h"
#include "barrier.h"
#include "klog.h"
#include "kfmt.h"

// driver-none: part of intel-display (intel_display.c declares it)

#define TRANS_DDI_FUNC_CTL(t)  (0x60400 + (t) * PIPE_STRIDE)
#define TRANS_HTOTAL(t)        (0x60000 + (t) * PIPE_STRIDE)
#define TRANS_HBLANK(t)        (0x60004 + (t) * PIPE_STRIDE)
#define TRANS_HSYNC(t)         (0x60008 + (t) * PIPE_STRIDE)
#define TRANS_VTOTAL(t)        (0x6000C + (t) * PIPE_STRIDE)
#define TRANS_VBLANK(t)        (0x60010 + (t) * PIPE_STRIDE)
#define TRANS_VSYNC(t)         (0x60014 + (t) * PIPE_STRIDE)
#define DDI_BUF_CTL(port)      (0x64000 + (port) * 0x100)
#define DDI_BUF_ENABLE         (1u << 31)
#define DDI_BUF_IDLE           (1u << 7)
#define DPLL_CTRL1             0x6C058
#define DPLL_CTRL2             0x6C05C
#define DPLL_STATUS            0x6C060
#define DPLL_CFGCR1(n)         (0x6C040 + ((n) - 1) * 8)
#define DPLL_CFGCR2(n)         (0x6C044 + ((n) - 1) * 8)
#define PLANE_SIZE(p)          (0x70190 + (p) * PIPE_STRIDE)
#define WM_LINETIME_REG(p)     (0x45270 + (p) * 4)
#define FUNC_PORT_MASK         (7u << 28)
#define FUNC_MODE_MASK         (7u << 24)
#define FUNC_HSYNC_HIGH        (1u << 16)
#define FUNC_VSYNC_HIGH        (1u << 17)
#define STATE_SPINS            4000000

// DPLL1..3's enable registers (DPLL0 is the DP/CDCLK one, never ours).
static uint32_t pll_ctl(int n) { return n == 1 ? 0x46014u : n == 2 ? 0x46040u : 0x46060u; }

static void udelay(uint32_t us) {
    uint64_t t0 = clocksource_now_ns(), want = (uint64_t)us * 1000;
    for (uint32_t i = 0; i < us * 4000u; i++) {
        if (clocksource_now_ns() - t0 >= want) return;
        cpu_relax();
    }
}

static int wait_bits(uint32_t reg, uint32_t bits, int want) {
    uint32_t v = 0;
    for (int i = 0; i < STATE_SPINS; i++) {
        v = intel_rd(reg);
        if (((v & bits) == bits) == (want != 0)) return 1;
        cpu_relax();
    }
    klog_printf("intel-gen9: register %#x bits %#x never became %s (%#x)\n",
                reg, bits, want ? "set" : "clear", v);
    return 0;
}

struct gen9_pipe_state {
    int pipe, port, pll;
    uint32_t func, ctl, surf, cur, curbase, buf, frame0;
};

// The pipe drives an HDMI or DVI DDI from DPLL1..3, or nothing here
// touches it -- the same refusal the gen8 path makes for a non-eDP pipe.
static int begin(struct gen9_pipe_state *st, const char *what) {
    st->pipe = intel_display_pipe();
    if (st->pipe < 0) return 0;
    st->func = intel_rd(TRANS_DDI_FUNC_CTL(st->pipe));
    st->port = (int)((st->func >> 28) & 7);
    uint32_t mode = (st->func >> 24) & 7;
    st->pll = (int)((intel_rd(DPLL_CTRL2) >> (st->port * 3 + 1)) & 3);
    if (!(st->func & TRANS_DDI_FUNC_ENABLE) || mode > 1 || st->port < 1 || st->pll < 1) {
        klog_printf(KLOG_ERR "intel-gen9: %s: pipe %c is not HDMI/DVI on DDI B-E from DPLL1-3 "
                    "(func %#x, pll %d) -- refused\n", what, 'A' + st->pipe, st->func, st->pll);
        return 0;
    }
    st->buf = intel_rd(DDI_BUF_CTL(st->port));
    klog_printf("intel-gen9: %s: pipe %c DDI %c DPLL%d -- transconf %#x func %#x buf %#x cfgcr %#x/%#x\n",
                what, 'A' + st->pipe, 'A' + st->port, st->pll, intel_rd(PIPECONF(st->pipe)),
                st->func, st->buf, intel_rd(DPLL_CFGCR1(st->pll)), intel_rd(DPLL_CFGCR2(st->pll)));
    return 1;
}

static int off(struct gen9_pipe_state *st, int level) {
    int p = st->pipe;
    st->ctl = intel_rd(DSPCNTR(p)); st->surf = intel_rd(DSPSURF(p));
    st->cur = intel_rd(CURCNTR(p)); st->curbase = intel_rd(CURBASE(p));
    st->frame0 = intel_rd(PIPEFRAME(p));
    intel_wr(CURCNTR(p), 0);
    intel_wr(CURBASE(p), st->curbase);
    intel_wr(DSPCNTR(p), st->ctl & ~DSPCNTR_ENABLE);
    intel_wr(DSPSURF(p), st->surf);
    intel_wr(PIPECONF(p), intel_rd(PIPECONF(p)) & ~PIPECONF_ENABLE);
    int ok = wait_bits(PIPECONF(p), PIPECONF_STATE, 0);
    // i915 clears the port select with the enable (intel_ddi_disable_transcoder_func()).
    intel_wr(TRANS_DDI_FUNC_CTL(p), st->func & ~(TRANS_DDI_FUNC_ENABLE | FUNC_PORT_MASK));
    if (level >= 1) {
        intel_wr(DDI_BUF_CTL(st->port), st->buf & ~DDI_BUF_ENABLE);
        ok &= wait_bits(DDI_BUF_CTL(st->port), DDI_BUF_IDLE, 1);
    }
    if (level >= 2) {
        intel_wr(DPLL_CTRL2, intel_rd(DPLL_CTRL2) | (1u << (st->port + 15)));   // the port's clock off
        intel_wr(pll_ctl(st->pll), intel_rd(pll_ctl(st->pll)) & ~(1u << 31));
        ok &= wait_bits(DPLL_STATUS, 1u << (st->pll * 8), 0);
    }
    klog_printf("intel-gen9: off (level %d): transconf %#x func %#x buf %#x pll %#x status %#x -- %s\n",
                level, intel_rd(PIPECONF(p)), intel_rd(TRANS_DDI_FUNC_CTL(p)),
                intel_rd(DDI_BUF_CTL(st->port)), intel_rd(pll_ctl(st->pll)), intel_rd(DPLL_STATUS),
                ok ? "off" : "STUCK");
    return ok;
}

static void write_timing(int p, const struct edid_timing *t) {
    uint32_t ht = edid_htotal(t), vt = edid_vtotal(t);
    uint32_t hss = t->hactive + t->hsync_off, hse = hss + t->hsync_w;
    uint32_t vss = t->vactive + t->vsync_off, vse = vss + t->vsync_w;
    intel_wr(TRANS_HTOTAL(p), ((ht - 1) << 16) | (t->hactive - 1u));
    intel_wr(TRANS_HBLANK(p), ((ht - 1) << 16) | (t->hactive - 1u));
    intel_wr(TRANS_HSYNC(p), ((hse - 1) << 16) | (hss - 1));
    intel_wr(TRANS_VTOTAL(p), ((vt - 1) << 16) | (t->vactive - 1u));
    intel_wr(TRANS_VBLANK(p), ((vt - 1) << 16) | (t->vactive - 1u));
    intel_wr(TRANS_VSYNC(p), ((vse - 1) << 16) | (vss - 1));
    intel_wr(PIPESRC(p), ((t->hactive - 1u) << 16) | (t->vactive - 1u));
    intel_wr(PLANE_SIZE(p), ((t->vactive - 1u) << 16) | (t->hactive - 1u));
    // The line time in 1/8 us (skl_wm_linetime), read by the watermarks.
    uint32_t lt = (uint32_t)(((uint64_t)ht * 8000 + t->pixel_khz / 2) / t->pixel_khz);
    intel_wr(WM_LINETIME_REG(p), (intel_rd(WM_LINETIME_REG(p)) & ~0x1FFu) | (lt & 0x1FF));
}

static int on(struct gen9_pipe_state *st, const struct edid_timing *t, int level) {
    int p = st->pipe, ok = 1;
    if (level >= 2) {
        uint32_t c1, c2;
        if (!t || !intel_display_gen9_wrpll(t->pixel_khz, &c1, &c2)) {
            klog_printf(KLOG_ERR "intel-gen9: no PLL dividers for %u kHz\n", t ? t->pixel_khz : 0);
            return 0;
        }
        // The PLL's DPLL_CTRL1 field: override, HDMI mode; the link
        // rate bits mean nothing in HDMI mode.
        uint32_t f = (1u << (st->pll * 6)) | (1u << (st->pll * 6 + 5));
        intel_wr(DPLL_CTRL1, (intel_rd(DPLL_CTRL1) & ~(0x3Fu << (st->pll * 6))) | f);
        (void)intel_rd(DPLL_CTRL1);
        intel_wr(DPLL_CFGCR1(st->pll), c1);
        intel_wr(DPLL_CFGCR2(st->pll), c2);
        (void)intel_rd(DPLL_CFGCR2(st->pll));
        intel_wr(pll_ctl(st->pll), intel_rd(pll_ctl(st->pll)) | (1u << 31));
        ok &= wait_bits(DPLL_STATUS, 1u << (st->pll * 8), 1);
        uint32_t c2r = intel_rd(DPLL_CTRL2);
        c2r &= ~((1u << (st->port + 15)) | (3u << (st->port * 3 + 1)));
        c2r |= ((uint32_t)st->pll << (st->port * 3 + 1)) | (1u << (st->port * 3));
        intel_wr(DPLL_CTRL2, c2r);
    }
    uint32_t func = st->func;
    if (t) {
        write_timing(p, t);
        intel_gen9_fit(t->hactive, t->vactive, 0, 0, t->hactive, t->vactive);   // 1:1
        func &= ~(FUNC_HSYNC_HIGH | FUNC_VSYNC_HIGH);
        if (t->hsync_pos) func |= FUNC_HSYNC_HIGH;
        if (t->vsync_pos) func |= FUNC_VSYNC_HIGH;
    }
    intel_wr(TRANS_DDI_FUNC_CTL(p), func);
    intel_wr(PIPECONF(p), intel_rd(PIPECONF(p)) | PIPECONF_ENABLE);
    ok &= wait_bits(PIPECONF(p), PIPECONF_STATE, 1);
    if (level >= 1) {
        intel_wr(DDI_BUF_CTL(st->port), st->buf | DDI_BUF_ENABLE);
        udelay(600);
        ok &= wait_bits(DDI_BUF_CTL(st->port), DDI_BUF_IDLE, 0);
    }
    intel_wr(DSPCNTR(p), st->ctl);
    intel_wr(DSPSURF(p), st->surf);
    intel_wr(CURCNTR(p), st->cur);
    intel_wr(CURBASE(p), st->curbase);
    for (int i = 0; i < STATE_SPINS && intel_rd(PIPEFRAME(p)) == st->frame0; i++) cpu_relax();
    uint32_t frame1 = intel_rd(PIPEFRAME(p));
    ok &= frame1 != st->frame0;
    klog_printf("intel-gen9: on (level %d): %s -- transconf %#x func %#x buf %#x cfgcr %#x/%#x "
                "status %#x frame %u -> %u\n", level, ok ? "back" : "NOT BACK",
                intel_rd(PIPECONF(p)), intel_rd(TRANS_DDI_FUNC_CTL(p)), intel_rd(DDI_BUF_CTL(st->port)),
                intel_rd(DPLL_CFGCR1(st->pll)), intel_rd(DPLL_CFGCR2(st->pll)), intel_rd(DPLL_STATUS),
                st->frame0, frame1);
    return ok;
}

// One cycle at `level`, at timing `t` (NULL keeps the timing on the
// transcoder; the PLL level needs one, for its clock).
int intel_gen9_cycle(int level, const struct edid_timing *t) {
    static const char *const NAME[3] = { "pipe cycle", "port cycle", "pll cycle" };
    struct gen9_pipe_state st;
    if (level < 0 || level > 2 || !begin(&st, NAME[level])) return 0;
    if (!off(&st, level)) {
        // Put back whatever went off, rather than leave the screen dark.
        on(&st, 0, level >= 1 ? 1 : 0);
        return 0;
    }
    return on(&st, t, level);
}
