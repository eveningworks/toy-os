// What the firmware programmed for the panel, read back and decoded:
// the transcoder timings, the pipe source, the DDI function, the port
// clock and the link M/N -- then compared against the EDID's preferred
// timing. Stage 2 of the modesetting plan (docs/roadmap-details.md):
// the check that the register map is understood before any of it is
// written. Nothing here writes a register.
//
// The decoders are PURE (regs in, timing out) so a KTEST can pin them
// on a machine without the hardware; the walk over the live registers
// is the only part that needs the laptop.
#include "intel_internal.h"
#include "intel_display.h"
#include "edid.h"
#include "klog.h"
#include "kfmt.h"

// driver-none: part of intel-display (intel_display.c declares it)

// Transcoder timing blocks: A/B/C at 0x60000 + n*0x1000, EDP at 0x6F000.
#define TRANS_A_BASE   0x60000
#define TRANS_EDP_BASE 0x6F000
#define TRANS_HTOTAL   0x000
#define TRANS_HBLANK   0x004
#define TRANS_HSYNC    0x008
#define TRANS_VTOTAL   0x00C
#define TRANS_VBLANK   0x010
#define TRANS_VSYNC    0x014
#define TRANS_DATA_M1  0x030
#define TRANS_DATA_N1  0x034
#define TRANS_LINK_M1  0x040
#define TRANS_LINK_N1  0x044
#define TRANS_DDI_FUNC_CTL 0x400
#define PIPEMISC(p)    (0x70030 + (p) * 0x1000)
#define PF_CTL(p)      (0x68080 + (p) * 0x800)
#define PF_WIN_POS(p)  (0x68070 + (p) * 0x800)
#define PF_WIN_SZ(p)   (0x68074 + (p) * 0x800)
#define PORT_CLK_SEL_A 0x46100
#define PCH_PP_ON_DELAYS  0xC7208
#define PCH_PP_OFF_DELAYS 0xC720C
#define PCH_PP_DIVISOR    0xC7210
#define LCPLL_CTL         0x130040

void intel_display_timing_from_regs(const struct intel_trans_regs *r, struct edid_timing *t) {
    uint32_t hactive = (r->htotal & 0xFFFF) + 1, htotal = (r->htotal >> 16) + 1;
    uint32_t hs_start = (r->hsync & 0xFFFF) + 1, hs_end = (r->hsync >> 16) + 1;
    uint32_t vactive = (r->vtotal & 0xFFFF) + 1, vtotal = (r->vtotal >> 16) + 1;
    uint32_t vs_start = (r->vsync & 0xFFFF) + 1, vs_end = (r->vsync >> 16) + 1;
    t->hactive = (uint16_t)hactive;
    t->hblank = (uint16_t)(htotal - hactive);
    t->hsync_off = (uint16_t)(hs_start - hactive);
    t->hsync_w = (uint16_t)(hs_end - hs_start);
    t->vactive = (uint16_t)vactive;
    t->vblank = (uint16_t)(vtotal - vactive);
    t->vsync_off = (uint16_t)(vs_start - vactive);
    t->vsync_w = (uint16_t)(vs_end - vs_start);
    t->pixel_khz = 0;
    t->width_mm = t->height_mm = 0;
    t->interlaced = 0;
    t->hsync_pos = t->vsync_pos = 0;
}

// The DP link's symbol clock from PORT_CLK_SEL (kHz): the LCPLL taps
// are named by the DP link rate they serve, halved.
uint32_t intel_display_port_clock_khz(uint32_t port_clk_sel) {
    switch ((port_clk_sel >> 29) & 7) {
    case 0: return 540000;   // LCPLL 2700
    case 1: return 270000;   // LCPLL 1350
    case 2: return 162000;   // LCPLL 810
    default: return 0;       // SPLL/WRPLL: not decoded (an HDMI/DVI clock)
    }
}

// dotclock = port clock * link M / link N (i915's intel_dotclock_calculate).
uint32_t intel_display_dotclock_khz(uint32_t port_khz, uint32_t link_m, uint32_t link_n) {
    link_m &= 0xFFFFFF;
    link_n &= 0xFFFFFF;
    if (!port_khz || !link_n) return 0;
    return (uint32_t)(((uint64_t)port_khz * link_m + link_n / 2) / link_n);
}

int intel_display_timing_same(const struct edid_timing *a, const struct edid_timing *b) {
    return a->hactive == b->hactive && a->hblank == b->hblank &&
           a->hsync_off == b->hsync_off && a->hsync_w == b->hsync_w &&
           a->vactive == b->vactive && a->vblank == b->vblank &&
           a->vsync_off == b->vsync_off && a->vsync_w == b->vsync_w;
}

static const char *ddi_mode_name(uint32_t mode) {
    static const char *const names[] = { "HDMI", "DVI", "DP SST", "DP MST", "FDI", "?", "?", "?" };
    return names[mode & 7];
}

void intel_readout_log(const struct display_edid *edid) {
    int pipe = intel_display_pipe();
    if (pipe < 0) return;
    uint32_t edp_func = intel_rd(TRANS_EDP_BASE + TRANS_DDI_FUNC_CTL);
    // The EDP transcoder drives the panel when it is enabled and its
    // input select names our pipe (0 = A, 4/5/6 = A/B/C on-off).
    uint32_t edp_in = (edp_func >> 12) & 7;
    int edp_pipe = edp_in == 0 ? 0 : (int)edp_in - 4;
    int on_edp = (edp_func & (1u << 31)) && edp_pipe == pipe;
    uint32_t base = on_edp ? TRANS_EDP_BASE : TRANS_A_BASE + (uint32_t)pipe * 0x1000;

    struct intel_trans_regs r = {
        .htotal = intel_rd(base + TRANS_HTOTAL), .hblank = intel_rd(base + TRANS_HBLANK),
        .hsync = intel_rd(base + TRANS_HSYNC),   .vtotal = intel_rd(base + TRANS_VTOTAL),
        .vblank = intel_rd(base + TRANS_VBLANK), .vsync = intel_rd(base + TRANS_VSYNC),
    };
    uint32_t func = intel_rd(base + TRANS_DDI_FUNC_CTL);
    uint32_t src = intel_rd(PIPESRC(pipe));
    uint32_t data_m = intel_rd(base + TRANS_DATA_M1), data_n = intel_rd(base + TRANS_DATA_N1);
    uint32_t link_m = intel_rd(base + TRANS_LINK_M1), link_n = intel_rd(base + TRANS_LINK_N1);
    uint32_t clk_sel = intel_rd(PORT_CLK_SEL_A);
    static const uint8_t BPC[] = { 8, 10, 6, 12 };

    klog_printf("intel-display: transcoder %s: htotal %#x hblank %#x hsync %#x vtotal %#x vblank %#x vsync %#x src %ux%u\n",
                on_edp ? "EDP" : "A+pipe", r.htotal, r.hblank, r.hsync, r.vtotal, r.vblank, r.vsync,
                (src >> 16) + 1, (src & 0xFFFF) + 1);
    klog_printf("intel-display: ddi func %#x: port %u %s %u bpc %u lane(s)%s%s; port clk sel %#x buf ctl %#x tp ctl %#x pipemisc %#x\n",
                func, (func >> 28) & 7, ddi_mode_name((func >> 24) & 7), BPC[(func >> 20) & 3],
                ((func >> 1) & 7) + 1, (func & (1u << 16)) ? " +hsync" : " -hsync",
                (func & (1u << 17)) ? " +vsync" : " -vsync",
                clk_sel, intel_rd(DDI_BUF_CTL_A), intel_rd(DP_TP_CTL_A), intel_rd(PIPEMISC(pipe)));
    klog_printf("intel-display: data m/n %#x/%#x (tu %u) link m/n %#x/%#x; pf ctl %#x pos %#x size %#x; pp on %#x off %#x div %#x lcpll %#x\n",
                data_m, data_n, ((data_m >> 25) & 0x3F) + 1, link_m, link_n,
                intel_rd(PF_CTL(pipe)), intel_rd(PF_WIN_POS(pipe)), intel_rd(PF_WIN_SZ(pipe)),
                intel_rd(PCH_PP_ON_DELAYS), intel_rd(PCH_PP_OFF_DELAYS), intel_rd(PCH_PP_DIVISOR),
                intel_rd(LCPLL_CTL));

    // The port's buffer translation table (swing/pre-emphasis levels),
    // as the firmware programmed it; link training selects an entry.
    uint32_t bt[18];
    for (int i = 0; i < 18; i++) bt[i] = intel_rd(DDI_BUF_TRANS_A + 4u * (uint32_t)i);
    klog_printf("intel-display: ddi a buf trans: %#x/%#x %#x/%#x %#x/%#x %#x/%#x %#x/%#x %#x/%#x %#x/%#x %#x/%#x %#x/%#x\n",
                bt[0], bt[1], bt[2], bt[3], bt[4], bt[5], bt[6], bt[7], bt[8], bt[9],
                bt[10], bt[11], bt[12], bt[13], bt[14], bt[15], bt[16], bt[17]);

    struct edid_timing hw;
    intel_display_timing_from_regs(&r, &hw);
    uint32_t port_khz = intel_display_port_clock_khz(clk_sel);
    hw.pixel_khz = intel_display_dotclock_khz(port_khz, link_m, link_n);
    uint32_t mhz = edid_refresh_mhz(&hw);
    klog_printf("intel-display: firmware timing: %ux%u h %u %u %u %u v %u %u %u %u, %u.%02u MHz (link %u.%02u Gbps), %u.%02u Hz\n",
                hw.hactive, hw.vactive, hw.hactive, hw.hsync_off, hw.hsync_w, hw.hblank,
                hw.vactive, hw.vsync_off, hw.vsync_w, hw.vblank,
                hw.pixel_khz / 1000, (hw.pixel_khz % 1000) / 10,
                port_khz / 100000, (port_khz / 1000) % 100,
                mhz / 1000, (mhz % 1000) / 10);
    if (!edid || edid->timing_count < 1) {
        klog_write("intel-display: no EDID timing to compare against\n");
        return;
    }
    const struct edid_timing *e = &edid->timing[0];
    int same = intel_display_timing_same(&hw, e);
    uint32_t dk = hw.pixel_khz > e->pixel_khz ? hw.pixel_khz - e->pixel_khz : e->pixel_khz - hw.pixel_khz;
    int clock_ok = e->pixel_khz && dk * 100 <= e->pixel_khz;   // within 1%
    klog_printf("intel-display: firmware timing %s EDID timing 0, pixel clock %s (%u vs %u kHz)\n",
                same ? "MATCHES" : "DIFFERS FROM", clock_ok ? "agrees" : "DISAGREES",
                hw.pixel_khz, e->pixel_khz);
}
