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
#define PS_VPHASE(p, id)      (0x68188 + (p) * 0x800 + (id) * 0x100)
#define PS_HPHASE(p, id)      (0x68194 + (p) * 0x800 + (id) * 0x100)
#define PS_SCALER_EN          (1u << 31)
#define PS_MODE_HQ            (1u << 28)   // one scaler on the pipe: both halves
#define PS_PHASE_TRIP         1u
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

static void gen9_readout_log(void) {
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

static int gen9_cursor_ddb(int p) {
    uint32_t plane = intel_rd(PLANE_BUF_CFG(p));
    uint32_t start = plane & 0x3FF, end = (plane >> 16) & 0x3FF;
    // A slice already there is trusted only when it is clear of the
    // plane's and has a watermark: a warm reboot can hand the plane back
    // its whole range and leave our old cursor slice inside it.
    uint32_t have = intel_rd(CUR_BUF_CFG(p));
    uint32_t hs = have & 0x3FF, he = (have >> 16) & 0x3FF;
    if (have && (he < start || hs > end) && (intel_rd(CUR_WM(p, 0)) & WM_ENABLE)) {
        klog_printf("intel-gen9: cursor already has ddb %#x\n", have);
        return 1;
    }
    if (end < start + 4 * CURSOR_DDB_BLOCKS) {
        klog_printf("intel-gen9: plane ddb %#x too small to share -- no cursor plane\n", plane);
        return 0;
    }
    uint32_t plane_end = end - CURSOR_DDB_BLOCKS;
    // Every enabled level of the plane -- and its transition watermark,
    // level 8 here -- must still fit what it keeps.
    for (int lvl = 0; lvl < 9; lvl++) {
        uint32_t wm = intel_rd(lvl < 8 ? PLANE_WM(p, lvl) : PLANE_WM_TRANS(p));
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

// The inverse: CFGCR1/CFGCR2 for an HDMI pixel clock, i915's
// skl_ddi_calculate_wrpll() in integers. The AFE clock is 5 x pixel;
// the DCO must sit within +1%/-6% of one of three central frequencies,
// divided down by P x Q x K, an even divider preferred and the smallest
// deviation winning. 0 when no divider fits.
int intel_display_gen9_wrpll(uint32_t pixel_khz, uint32_t *cfgcr1, uint32_t *cfgcr2) {
    static const uint64_t CENTRAL[3] = { 8400000000ull, 9000000000ull, 9600000000ull };
    static const uint8_t EVEN[] = { 4, 6, 8, 10, 12, 14, 16, 18, 20, 24, 28, 30, 32, 36, 40, 42,
                                    44, 48, 52, 54, 56, 60, 64, 66, 68, 70, 72, 76, 78, 80, 84,
                                    88, 90, 92, 96, 98 };
    static const uint8_t ODD[] = { 3, 5, 7, 9, 15, 21, 35 };
    const struct { const uint8_t *d; int n; } LISTS[2] = {
        { EVEN, (int)sizeof EVEN }, { ODD, (int)sizeof ODD } };
    uint64_t afe = (uint64_t)pixel_khz * 1000 * 5;   // Hz
    uint64_t best_dev = ~0ull, best_central = 0, best_dco = 0;
    unsigned best_p = 0;
    for (int l = 0; l < 2 && !best_p; l++) {
        for (int c = 0; c < 3 && best_dev; c++) {
            for (int i = 0; i < LISTS[l].n && best_dev; i++) {
                uint64_t dco = LISTS[l].d[i] * afe;
                uint64_t diff = dco > CENTRAL[c] ? dco - CENTRAL[c] : CENTRAL[c] - dco;
                uint64_t dev = 10000 * diff / CENTRAL[c];
                if (dev < (dco >= CENTRAL[c] ? 100u : 600u) && dev < best_dev) {
                    best_dev = dev; best_central = CENTRAL[c]; best_dco = dco; best_p = LISTS[l].d[i];
                }
            }
        }
    }
    if (!best_p) return 0;
    // P x Q x K from the divider (skl_wrpll_get_multipliers()).
    unsigned p0 = 0, p1 = 0, p2 = 0, p = best_p;
    if (p % 2 == 0) {
        unsigned half = p / 2;
        if (half == 1 || half == 2 || half == 3 || half == 5) { p0 = 2; p1 = 1; p2 = half; }
        else if (half % 2 == 0) { p0 = 2; p1 = half / 2; p2 = 2; }
        else if (half % 3 == 0) { p0 = 3; p1 = half / 3; p2 = 2; }
        else if (half % 7 == 0) { p0 = 7; p1 = half / 7; p2 = 2; }
    } else if (p == 3 || p == 9) { p0 = 3; p1 = 1; p2 = p / 3; }
    else if (p == 5 || p == 7) { p0 = p; p1 = 1; p2 = 1; }
    else if (p == 15) { p0 = 3; p1 = 1; p2 = 5; }
    else if (p == 21) { p0 = 7; p1 = 1; p2 = 3; }
    else if (p == 35) { p0 = 7; p1 = 1; p2 = 5; }
    uint32_t pdiv = p0 == 1 ? 0 : p0 == 2 ? 1 : p0 == 3 ? 2 : p0 == 7 ? 4 : 99;
    uint32_t kdiv = p2 == 5 ? 0 : p2 == 2 ? 1 : p2 == 3 ? 2 : p2 == 1 ? 3 : 99;
    if (pdiv == 99 || kdiv == 99) return 0;
    uint32_t central = best_central == 9600000000ull ? 0 : best_central == 9000000000ull ? 1 : 3;
    (void)best_dco;
    uint64_t dco = (uint64_t)p0 * p1 * p2 * afe;
    const uint64_t REF = 24000;   // kHz, the non-SSC reference
    uint64_t integer = dco / (REF * 1000);
    uint64_t fraction = (dco / (REF / 1000) - integer * 1000000) * 0x8000 / 1000000;
    *cfgcr1 = (1u << 31) | ((uint32_t)fraction << 9) | (uint32_t)integer;
    *cfgcr2 = ((uint32_t)p1 << 8) | (p1 != 1 ? 1u << 7 : 0) | (kdiv << 5) | (pdiv << 2) | central;
    return 1;
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
    klog_printf("intel-gen9: firmware timing %s EDID timing 0, pixel clock %s (%u vs %u kHz)\n",
                same ? "MATCHES" : "DIFFERS FROM",
                intel_display_clock_agrees(t.pixel_khz, want) ? "agrees" : "DISAGREES", t.pixel_khz, want);
}

static uint8_t g_edid[EDID_MAX];
static int g_edid_len = -1;   // -1 until read once

static int gen9_read_edid(int p, uint8_t *out, int cap) {
    if (g_edid_len < 0) {
        int port = ddi_of_pipe(p);
        int pin = intel_gmbus_pin_for_port(port);
        g_edid_len = pin ? intel_gmbus_read_edid(pin, g_edid, EDID_BLOCK) : 0;
        // The CEA extension (where an HDMI monitor lists 1080p), when the
        // base block says there is one: one read of both, from offset 0.
        if (g_edid_len == EDID_BLOCK && g_edid[126])
            g_edid_len = intel_gmbus_read_edid(pin, g_edid, EDID_MAX) == EDID_MAX ? EDID_MAX : EDID_BLOCK;
        klog_printf("intel-gen9: EDID over GMBUS pin %d (DDI %c): %d bytes\n",
                    pin, port >= 0 ? 'A' + port : '-', g_edid_len);
        struct display_edid e;
        timing_log(p, g_edid_len >= EDID_BLOCK && edid_parse(g_edid, g_edid_len, &e) ? &e : 0);
    }
    int n = g_edid_len < cap ? g_edid_len : cap;
    if (n > 0) k_memcpy(out, g_edid, (size_t)n);
    return n > 0 ? n : 0;
}

// --- stage 3: a smaller mode through the pipe scaler ----------------------
//
// THE NATIVE TIMING NEVER CHANGES: a smaller mode is the pipe's SOURCE
// (PIPESRC, and the plane's size) shown through scaler 0 into a window
// of the native active area, as gen8's panel fitter does -- but live,
// with no pipe cycle, the way i915's fastset updates PIPESRC and the
// pfit (intel_pipe_fastset()). The firmware leaves the scaler on at 1:1
// with ITS coefficients loaded; the native mode puts that state back
// exactly, and a scaled one switches to the built-in medium filter.
struct gen9_fw_scaler { uint32_t ctrl, vphase, hphase, pos, sz; int saved; };
static struct gen9_fw_scaler g_fw;

static void gen9_claimed(void) {
    int p = intel_display_pipe();
    g_fw = (struct gen9_fw_scaler){ intel_rd(PS_CTRL(p, 0)), intel_rd(PS_VPHASE(p, 0)),
                                    intel_rd(PS_HPHASE(p, 0)), intel_rd(PS_WIN_POS(p, 0)),
                                    intel_rd(PS_WIN_SZ(p, 0)), 1 };
}

// i915's skl_scaler_calc_phase() for RGB (one sample, not cosited):
// the initial phase is half a source step back from -0.5, u2.13 with a
// trip bit when it is not negative. `scale` is src/dst in 16.16.
uint32_t intel_display_gen9_phase(uint32_t scale) {
    int phase = -0x8000 + (int)(scale / 2);
    uint32_t trip = 0;
    if (phase < 0) phase += 0x10000;
    else trip = PS_PHASE_TRIP;
    return (((uint32_t)phase >> 2) & (0x7FFFu << 1)) | trip;
}

int intel_gen9_fit(uint32_t w, uint32_t h, uint32_t x, uint32_t y, uint32_t ww, uint32_t wh) {
    int p = intel_display_pipe();
    if (p < 0 || w < 8 || h < 8 || ww < 8 || wh < 8 || w > 4096 || h > 4096) return 0;
    uint32_t src = ((w - 1) << 16) | (h - 1);
    uint32_t ctrl, vph, hph, pos = (x << 16) | y, sz = (ww << 16) | wh;
    if (g_fw.saved && sz == g_fw.sz && pos == g_fw.pos &&
        src == ((((g_fw.sz >> 16) - 1) << 16) | ((g_fw.sz & 0xFFFF) - 1))) {
        ctrl = g_fw.ctrl; vph = g_fw.vphase; hph = g_fw.hphase;   // the firmware's own
    } else {
        ctrl = PS_SCALER_EN | PS_MODE_HQ;   // pipe binding 0, the medium filter
        hph = intel_display_gen9_phase((w << 16) / ww);
        vph = intel_display_gen9_phase((h << 16) / wh);
    }
    // PIPESRC is single-buffered and lands now; the plane and the scaler
    // latch at the next vblank, armed by PLANE_SURF and PS_WIN_SZ. At
    // worst one frame shows the new source through the old window.
    intel_wr(PIPESRC(p), src);
    intel_wr(PLANE_SIZE(p), ((h - 1) << 16) | (w - 1));
    intel_wr(PS_CTRL(p, 0), ctrl);
    intel_wr(PS_VPHASE(p, 0), vph);
    intel_wr(PS_HPHASE(p, 0), hph);
    intel_wr(PS_WIN_POS(p, 0), pos);
    intel_wr(PS_WIN_SZ(p, 0), sz);
    intel_wr(DSPSURF(p), intel_rd(DSPSURF(p)));
    int ok = intel_rd(PIPESRC(p)) == src && intel_rd(PS_WIN_SZ(p, 0)) == sz &&
             intel_rd(PS_CTRL(p, 0)) == ctrl;
    klog_printf("intel-gen9: fit %ux%u into %ux%u at %u,%u -- ctrl %#x phase %#x/%#x %s\n",
                w, h, ww, wh, x, y, ctrl, hph, vph, ok ? "ok" : "DID NOT READ BACK");
    return ok;
}

// --- stage 4: the cycles and a real timing --------------------------------

static int gen9_pipe_cycle(void) { return intel_gen9_cycle(0, 0); }
static int gen9_port_cycle(void) { return intel_gen9_cycle(1, 0); }

// The PLL level needs a clock: the monitor's preferred timing, which the
// readout showed is what the firmware lit.
static int gen9_native(void) {
    const struct display_edid *e = display_edid();
    if (!e || !e->timing_count) {
        klog_write("intel-gen9: pll cycle: no EDID timing -- refused\n");
        return 0;
    }
    return intel_gen9_cycle(2, &e->timing[0]);
}

static int gen9_set_timing(const struct edid_timing *t) {
    return intel_gen9_cycle(2, t);
}

// Kaby Lake, i915's INTEL_KBL_IDS.
static const uint16_t KBL_IDS[] = {
    0x5902, 0x5906, 0x5908, 0x590A, 0x590B, 0x590E,
    0x5912, 0x5913, 0x5915, 0x5916, 0x5917, 0x591A, 0x591B, 0x591C,
    0x591D, 0x591E, 0x5921, 0x5923, 0x5926, 0x5927, 0x593B,
};

// GMS in 32 MiB units, plus 4 MiB steps from 0xF0 (Linux's gen9_stolen_size).
static uint64_t gen9_stolen_bytes(uint32_t gms) {
    return gms >= 0xF0 ? (uint64_t)(gms - 0xF0 + 1) << 22 : (uint64_t)gms << 25;
}

static int gen9_plane_matches(uint32_t cntr, uint32_t stride, uint32_t pitch) {
    return intel_display_plane_matches(9, cntr, stride, pitch);
}

const struct intel_gen_ops intel_gen9_ops = {
    .gen = 9,
    .ids = KBL_IDS,
    .nids = (int)(sizeof KBL_IDS / sizeof KBL_IDS[0]),
    .stolen_bytes = gen9_stolen_bytes,
    .readout_log = gen9_readout_log,
    .plane_matches = gen9_plane_matches,
    .cursor_prepare = gen9_cursor_ddb,
    // A linear surface scans only from 256 KiB (i915's skl_plane_min_alignment).
    .scanout_align_pages = 64,
    .claimed = gen9_claimed,
    .read_edid = gen9_read_edid,
    .caps = DISPLAY_CAP_MODESET | DISPLAY_CAP_SCALING,
    .fit = intel_gen9_fit,
    .set_timing = gen9_set_timing,
    .pipe_cycle = gen9_pipe_cycle,
    .link_retrain = gen9_port_cycle,
    .native = gen9_native,
};
