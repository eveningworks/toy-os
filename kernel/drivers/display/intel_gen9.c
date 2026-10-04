// Intel gen9 (Kaby Lake) -- what the firmware programmed, read and
// logged before anything here writes a register. Stage 1a of the
// desktop's driver: intel_display.c binds a gen9 device, calls this, and
// does NOT claim, so the first boot on a new machine changes nothing.
//
// Gen9's display engine is gen8's at the same offsets with different
// FIELDS, plus a display buffer (DDB) every plane must own a slice of:
// a plane enabled with no DDB blocks fetches nothing, silently. So the
// readout's job is the facts stage 1b needs -- the plane's format,
// tiling and stride units, the cursor's DDB and watermarks, which DDI
// and DPLL drive the pipe, and what GMBUS sees.
//
// Offsets and fields are from Linux's i915 (skl_universal_plane_regs.h,
// intel_cursor_regs.h, i915_reg.h, intel_gmbus_regs.h).
#include "klog.h"
#include "kfmt.h"
#include "intel_internal.h"

// driver-none: part of intel-display (intel_display.c declares it)

#define DPLL_CTRL1   0x6C058
#define DPLL_CTRL2   0x6C05C
#define DPLL_STATUS  0x6C060
#define DPLL_CFGCR1(n) (0x6C040 + ((n) - 1) * 8)   // DPLL1..3
#define DPLL_CFGCR2(n) (0x6C044 + ((n) - 1) * 8)
#define LCPLL1_CTL   0x46010
#define LCPLL2_CTL   0x46014
#define CDCLK_CTL    0x46000
#define SFUSE_STRAP  0xC2014
#define DBUF_CTL     0x45008
#define DC_STATE_EN  0x45504
#define TRANS_DDI_FUNC_CTL(t) (0x60400 + (t) * PIPE_STRIDE)
#define TRANS_HTOTAL(t)       (0x60000 + (t) * PIPE_STRIDE)
#define TRANS_HSYNC(t)        (0x60008 + (t) * PIPE_STRIDE)
#define TRANS_VTOTAL(t)       (0x6000C + (t) * PIPE_STRIDE)
#define TRANS_VSYNC(t)        (0x60014 + (t) * PIPE_STRIDE)
#define DDI_BUF_CTL(port)     (0x64000 + (port) * 0x100)
#define PS_CTRL(p, id)        (0x68180 + (p) * 0x800 + (id) * 0x100)
#define PS_WIN_POS(p, id)     (0x68170 + (p) * 0x800 + (id) * 0x100)
#define PS_WIN_SZ(p, id)      (0x68174 + (p) * 0x800 + (id) * 0x100)
#define PLANE_POS(p)          (0x7018C + (p) * PIPE_STRIDE)
#define PLANE_SIZE(p)         (0x70190 + (p) * PIPE_STRIDE)
#define PLANE_OFFSET(p)       (0x701A4 + (p) * PIPE_STRIDE)
#define PLANE_WM(p, lvl)      (0x70240 + (p) * PIPE_STRIDE + (lvl) * 4)
#define PLANE_WM_TRANS(p)     (0x70268 + (p) * PIPE_STRIDE)
#define PLANE_BUF_CFG(p)      (0x7027C + (p) * PIPE_STRIDE)
#define CUR_WM(p, lvl)        (0x70140 + (p) * PIPE_STRIDE + (lvl) * 4)
#define CUR_WM_TRANS(p)       (0x70168 + (p) * PIPE_STRIDE)
#define CUR_BUF_CFG(p)        (0x7017C + (p) * PIPE_STRIDE)
#define GMBUS0       0xC5100

static const char *const DDI_MODE[8] = { "HDMI", "DVI", "DP-SST", "DP-MST", "FDI", "?5", "?6", "?7" };

void intel_gen9_readout_log(void) {
    for (int p = 0; p < 3; p++) {
        uint32_t ctl = intel_rd(DSPCNTR(p));
        klog_printf("intel-gen9: pipe %c conf %#x src %#x | plane ctl %#x (fmt %u tiling %u) stride %u surf %#x live %#x pos %#x size %#x offset %#x\n",
                    'A' + p, intel_rd(PIPECONF(p)), intel_rd(PIPESRC(p)), ctl,
                    (ctl >> 24) & 0xF, (ctl >> 10) & 7, intel_rd(DSPSTRIDE(p)),
                    intel_rd(DSPSURF(p)), intel_rd(DSPSURFLIVE(p)),
                    intel_rd(PLANE_POS(p)), intel_rd(PLANE_SIZE(p)), intel_rd(PLANE_OFFSET(p)));
        klog_printf("intel-gen9: pipe %c ddb plane %#x cursor %#x | wm plane %#x %#x %#x trans %#x | wm cursor %#x %#x trans %#x | cursor ctl %#x base %#x\n",
                    'A' + p, intel_rd(PLANE_BUF_CFG(p)), intel_rd(CUR_BUF_CFG(p)),
                    intel_rd(PLANE_WM(p, 0)), intel_rd(PLANE_WM(p, 1)), intel_rd(PLANE_WM(p, 7)),
                    intel_rd(PLANE_WM_TRANS(p)),
                    intel_rd(CUR_WM(p, 0)), intel_rd(CUR_WM(p, 1)), intel_rd(CUR_WM_TRANS(p)),
                    intel_rd(CURCNTR(p)), intel_rd(CURBASE(p)));
        uint32_t ddi = intel_rd(TRANS_DDI_FUNC_CTL(p));
        klog_printf("intel-gen9: trans %c ddi func %#x (%s port %c %s) htotal %#x hsync %#x vtotal %#x vsync %#x | scaler %#x/%#x win %#x %#x\n",
                    'A' + p, ddi, (ddi >> 31) ? "on" : "off", 'A' + (int)((ddi >> 28) & 7),
                    DDI_MODE[(ddi >> 24) & 7],
                    intel_rd(TRANS_HTOTAL(p)), intel_rd(TRANS_HSYNC(p)),
                    intel_rd(TRANS_VTOTAL(p)), intel_rd(TRANS_VSYNC(p)),
                    intel_rd(PS_CTRL(p, 0)), intel_rd(PS_CTRL(p, 1)),
                    intel_rd(PS_WIN_POS(p, 0)), intel_rd(PS_WIN_SZ(p, 0)));
    }
    klog_printf("intel-gen9: ddi buf A %#x B %#x C %#x D %#x E %#x | fuse strap %#x\n",
                intel_rd(DDI_BUF_CTL(0)), intel_rd(DDI_BUF_CTL(1)), intel_rd(DDI_BUF_CTL(2)),
                intel_rd(DDI_BUF_CTL(3)), intel_rd(DDI_BUF_CTL(4)), intel_rd(SFUSE_STRAP));
    // Each line under KFMT_LINE_MAX: an overlong one logs NOTHING.
    klog_printf("intel-gen9: dpll ctrl1 %#x ctrl2 %#x status %#x | lcpll1 %#x lcpll2 %#x cdclk %#x\n",
                intel_rd(DPLL_CTRL1), intel_rd(DPLL_CTRL2), intel_rd(DPLL_STATUS),
                intel_rd(LCPLL1_CTL), intel_rd(LCPLL2_CTL), intel_rd(CDCLK_CTL));
    klog_printf("intel-gen9: dpll1 cfgcr %#x/%#x dpll2 %#x/%#x dpll3 %#x/%#x\n",
                intel_rd(DPLL_CFGCR1(1)), intel_rd(DPLL_CFGCR2(1)),
                intel_rd(DPLL_CFGCR1(2)), intel_rd(DPLL_CFGCR2(2)),
                intel_rd(DPLL_CFGCR1(3)), intel_rd(DPLL_CFGCR2(3)));
    klog_printf("intel-gen9: power wells bios %#x driver %#x | dbuf %#x dc state %#x | gmbus0-5 %#x %#x %#x %#x %#x %#x\n",
                intel_rd(HSW_PWR_WELL_CTL_BIOS), intel_rd(HSW_PWR_WELL_CTL_DRIVER),
                intel_rd(DBUF_CTL), intel_rd(DC_STATE_EN),
                intel_rd(GMBUS0), intel_rd(GMBUS0 + 4), intel_rd(GMBUS0 + 8),
                intel_rd(GMBUS0 + 12), intel_rd(GMBUS0 + 16), intel_rd(GMBUS0 + 0x20));
}
