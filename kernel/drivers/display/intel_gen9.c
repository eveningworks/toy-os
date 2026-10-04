// Intel gen9 (Kaby Lake) -- what the firmware programmed, read and
// logged before anything here writes a register, and the one thing gen9
// needs that gen8 does not before its cursor can show: a DDB slice.
// intel_display.c claims at the firmware's mode for the cursor plane and
// the flip; the eDP modeset paths are gen8's.
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
#include "string.h"   // k_memcpy
#include "edid.h"
#include "intel_display.h"
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

// THE CURSOR'S SLICE OF THE DISPLAY BUFFER. The firmware gives it none
// (CUR_BUF_CFG 0), and a plane with no DDB blocks fetches nothing --
// enabled, armed and invisible. i915's skl_ddb allocation puts the
// cursor's 32 blocks (one active pipe) at the END of its pipe's range,
// so they are carved from the end of the primary plane's here. Both
// BUF_CFG registers and the watermarks are double-buffered: the plane's
// half latches at its next PLANE_SURF write, re-written below with the
// value it already holds; the cursor's at the CURBASE write that
// setup_cursor() makes next. An END is inclusive in the register.
#define CURSOR_DDB_BLOCKS 32
#define WM_ENABLE    (1u << 31)
#define WM_LINES(n)  ((uint32_t)(n) << 14)
#define WM_BLOCKS    0x3FFu
// Level 0 only: one line and 8 blocks covers a 64-pixel ARGB line many
// times over and leaves the levels a low-power state needs disabled.
#define CURSOR_WM0   (WM_ENABLE | WM_LINES(1) | 8)

int intel_gen9_cursor_ddb(int p) {
    uint32_t have = intel_rd(CUR_BUF_CFG(p));
    if (have) {
        klog_printf("intel-gen9: cursor already has ddb %#x\n", have);
        return 1;
    }
    uint32_t plane = intel_rd(PLANE_BUF_CFG(p));
    uint32_t start = plane & 0x3FF, end = (plane >> 16) & 0x3FF;
    if (end < start + 4 * CURSOR_DDB_BLOCKS) {
        klog_printf("intel-gen9: plane ddb %#x too small to share -- no cursor plane\n", plane);
        return 0;
    }
    uint32_t plane_end = end - CURSOR_DDB_BLOCKS;
    // Every enabled level of the plane must still fit what it keeps.
    for (int lvl = 0; lvl < 8; lvl++) {
        uint32_t wm = intel_rd(PLANE_WM(p, lvl));
        if ((wm & WM_ENABLE) && (wm & WM_BLOCKS) > plane_end - start) {
            klog_printf("intel-gen9: plane wm%d %#x needs more than %u blocks -- no cursor plane\n",
                        lvl, wm, plane_end - start);
            return 0;
        }
    }
    intel_wr(PLANE_BUF_CFG(p), (plane_end << 16) | start);
    intel_wr(DSPSURF(p), intel_rd(DSPSURF(p)));   // arm the plane's half
    intel_wr(CUR_BUF_CFG(p), (end << 16) | (plane_end + 1));
    intel_wr(CUR_WM(p, 0), CURSOR_WM0);
    klog_printf("intel-gen9: ddb plane %u-%u, cursor %u-%u, cursor wm0 %#x\n",
                start, plane_end, plane_end + 1, end, CURSOR_WM0);
    return 1;
}

// HDMI's pixel clock from a DPLL in HDMI mode: DCO = (integer +
// fraction / 2^15) x 24 MHz, divided by P x Q x K into the AFE clock,
// which is five times the pixel clock (TMDS's 10 bits per character
// over a DDR clock). The inverse of i915's skl_ddi_calculate_wrpll.
uint32_t intel_display_gen9_hdmi_khz(uint32_t cfgcr1, uint32_t cfgcr2) {
    static const uint8_t P[8] = { 1, 2, 3, 0, 7, 0, 0, 0 };
    static const uint8_t K[4] = { 5, 2, 3, 1 };
    uint32_t p = P[(cfgcr2 >> 2) & 7], k = K[(cfgcr2 >> 5) & 3];
    uint32_t q = (cfgcr2 & (1u << 7)) ? (cfgcr2 >> 8) & 0xFF : 1;
    if (!(cfgcr1 & (1u << 31)) || !p || !q) return 0;
    uint64_t dco_khz = (uint64_t)(cfgcr1 & 0x1FF) * 24000 +
                       (uint64_t)((cfgcr1 >> 9) & 0x7FFF) * 24000 / 0x8000;
    return (uint32_t)(dco_khz / (p * q * k) / 5);
}

// The DDI a transcoder drives (1 = B .. 4 = E), or -1 when it is off.
static int ddi_of_pipe(int p) {
    uint32_t f = intel_rd(TRANS_DDI_FUNC_CTL(p));
    return (f >> 31) ? (int)((f >> 28) & 7) : -1;
}

// What the firmware programmed, against the EDID's preferred timing --
// the check stage 4's modeset leans on, as intel_readout.c's is for gen8.
static void timing_log(int p, const struct display_edid *e) {
    struct intel_trans_regs r = {
        intel_rd(TRANS_HTOTAL(p)), intel_rd(TRANS_HTOTAL(p) + 4), intel_rd(TRANS_HSYNC(p)),
        intel_rd(TRANS_VTOTAL(p)), intel_rd(TRANS_VTOTAL(p) + 4), intel_rd(TRANS_VSYNC(p)),
    };
    struct edid_timing t;
    intel_display_timing_from_regs(&r, &t);
    int port = ddi_of_pipe(p);
    uint32_t sel = port > 0 ? (intel_rd(DPLL_CTRL2) >> (port * 3 + 1)) & 3 : 0;
    if (sel) t.pixel_khz = intel_display_gen9_hdmi_khz(intel_rd(DPLL_CFGCR1(sel)),
                                                       intel_rd(DPLL_CFGCR2(sel)));
    klog_printf("intel-gen9: firmware timing %ux%u h %u %u %u v %u %u %u, DPLL%u %u kHz\n",
                t.hactive, t.vactive, t.hsync_off, t.hsync_w, t.hblank,
                t.vsync_off, t.vsync_w, t.vblank, sel, t.pixel_khz);
    if (!e || !e->timing_count) return;
    int same = intel_display_timing_same(&t, &e->timing[0]);
    uint32_t want = e->timing[0].pixel_khz;
    int clk = t.pixel_khz + 10 >= want && t.pixel_khz <= want + 10;
    klog_printf("intel-gen9: firmware timing %s EDID timing 0, pixel clock %s (%u vs %u kHz)\n",
                same ? "MATCHES" : "DIFFERS FROM", clk ? "agrees" : "DIFFERS", t.pixel_khz, want);
}

static uint8_t g_edid[EDID_BLOCK];
static int g_edid_len = -1;   // -1 until read once

int intel_gen9_read_edid(int p, uint8_t *out, int cap) {
    if (g_edid_len < 0) {
        int port = ddi_of_pipe(p);
        int pin = intel_gmbus_pin_for_port(port);
        g_edid_len = pin ? intel_gmbus_read_edid(pin, g_edid, EDID_BLOCK) : 0;
        klog_printf("intel-gen9: EDID over GMBUS pin %d (DDI %c): %d bytes\n",
                    pin, port >= 0 ? 'A' + port : '-', g_edid_len);
        struct display_edid e;
        timing_log(p, g_edid_len == EDID_BLOCK && edid_parse(g_edid, g_edid_len, &e) ? &e : 0);
    }
    int n = g_edid_len < cap ? g_edid_len : cap;
    if (n > 0) k_memcpy(out, g_edid, (size_t)n);
    return n > 0 ? n : 0;
}
