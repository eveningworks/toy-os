// Shared between the Intel display driver's files (intel_display.c,
// intel_aux.c, intel_readout.c): the MMIO accessors and the one fact
// the other files need. Not a boundary -- one driver in three files,
// split by concern (the plane and its planes; the AUX channel; the
// firmware-state readout), the userland/wm/ pattern.
#ifndef INTEL_INTERNAL_H
#define INTEL_INTERNAL_H

#include <stdint.h>

// --- display engine registers (MMIO offsets from BAR0) ----------------
#define PIPE_STRIDE 0x1000
#define PIPEDSL(p)     (0x70000 + (p) * PIPE_STRIDE) // current scanline
#define PIPECONF(p)    (0x70008 + (p) * PIPE_STRIDE) // bit31 enable, bit30 state
#define PIPEFRAME(p)   (0x70040 + (p) * PIPE_STRIDE) // frame counter
#define PIPESRC(p)     (0x6001C + (p) * PIPE_STRIDE) // (w-1)<<16 | (h-1)
#define DSPCNTR(p)     (0x70180 + (p) * PIPE_STRIDE) // bit31 enable, 29:26 format
#define DSPSTRIDE(p)   (0x70188 + (p) * PIPE_STRIDE)
#define DSPSURF(p)     (0x7019C + (p) * PIPE_STRIDE) // GGTT offset; latches at vblank
#define DSPSURFLIVE(p) (0x701AC + (p) * PIPE_STRIDE) // the offset being scanned NOW
#define CURCNTR(p)     (0x70080 + (p) * PIPE_STRIDE)
#define CURBASE(p)     (0x70084 + (p) * PIPE_STRIDE) // GGTT offset; the arming write
#define CURPOS(p)      (0x70088 + (p) * PIPE_STRIDE)
#define TRANS_DDI_FUNC_CTL_EDP 0x6F400

// --- the SPRITE (second universal) plane -------------------------------
//
// DERIVED, not remembered: the DSP* offsets above ARE gen8's universal
// plane block at plane 0 (ctl 0x70180, stride +0x08, surf +0x1C,
// surflive +0x2C), and a plane is 0x100 further on. So the sprite plane
// is the same block at +0x100 -- which is what SPR_PLANE_OFF says, so
// the relationship is visible rather than a second set of magic numbers
// that could drift from the first.
//
// UNVERIFIED AGAINST THIS SILICON UNTIL THE READOUT RUNS, which is the
// entire point of reading them before writing any: a wrong offset shows
// up as an implausible value in a log line, not as a dead panel.
#define SPR_PLANE_OFF  0x100
#define SPRCTL(p)      (DSPCNTR(p)     + SPR_PLANE_OFF)
#define SPRSTRIDE(p)   (DSPSTRIDE(p)   + SPR_PLANE_OFF)
#define SPRPOS(p)      (0x7018C + (p) * PIPE_STRIDE + SPR_PLANE_OFF) // y<<16 | x
#define SPRSIZE(p)     (0x70190 + (p) * PIPE_STRIDE + SPR_PLANE_OFF) // (h-1)<<16 | (w-1)
#define SPRSURF(p)     (DSPSURF(p)     + SPR_PLANE_OFF)
#define SPRSURFLIVE(p) (DSPSURFLIVE(p) + SPR_PLANE_OFF)

// --- watermarks --------------------------------------------------------
//
// NOTHING IN THIS DRIVER PROGRAMS THESE. It does a fastboot readout and
// reuses what the firmware left, which the firmware computed for ONE
// plane plus a cursor. Enabling a second plane changes the bandwidth the
// display engine has to sustain, and on gen8 a watermark that is too low
// for the configuration is a FIFO underrun -- flicker, or a black
// scanline band, not an error anyone is told about. i915 computes these
// in software and rejects a configuration that does not fit; a KMS
// client discovers the rejection through an atomic commit with
// TEST_ONLY. There is no such gate here, so the first thing to know is
// what the firmware actually programmed.
#define WM_PIPE(p)     (0x45100 + (p) * 4)
#define WM_LP(n)       (0x45108 + (n) * 4)   // LP1..LP3
#define WM_LINETIME(p) (0x45270 + (p) * 4)
#define WM_MISC        0x45260

#define DSPCNTR_ENABLE   (1u << 31)
#define DSPCNTR_FMT_MASK (0xFu << 26)
#define DSPCNTR_BGRX8888 (0x6u << 26)
// Gen9 (PLANE_CTL at DSPCNTR's offset): the same enable bit, these fields.
#define PLANE_CTL_FMT_MASK   (0xFu << 24)
#define PLANE_CTL_XRGB8888   (0x4u << 24)
#define PLANE_CTL_TILED_MASK (0x7u << 10)
#define PLANE_CTL_ORDER_RGBX (1u << 20)   // XBGR: red and blue swapped
#define PLANE_CTL_ROTATE_MASK 0x3u

#define CURCNTR_MODE_MASK    0x3Fu
#define CURCNTR_64_ARGB      0x27u
#define CURPOS_SIGN          0x8000u

#define HSW_PWR_WELL_CTL_BIOS   0x45400
#define HSW_PWR_WELL_CTL_DRIVER 0x45404
#define PWR_WELL_REQUEST (1u << 31)
#define PWR_WELL_STATE   (1u << 30)

// Backlight PWM: the PCH's and the CPU's, and which one drives the pin
// is the firmware's choice (Linux's lpt_setup_backlight reads it back
// the same way).
#define BLC_PWM_CPU_CTL2  0x48250 // bit31 enable
#define BLC_PWM_CPU_CTL   0x48254 // duty in bits 15:0
#define BLC_PWM_PCH_CTL1  0xC8250 // bit31 enable, bit30 override (PCH drives), bit29 polarity
#define BLC_PWM_PCH_CTL2  0xC8254 // 31:16 period (= max duty), 15:0 duty
#define BLM_PWM_ENABLE          (1u << 31)
#define BLM_PCH_OVERRIDE_ENABLE (1u << 30)
#define BLM_PCH_POLARITY        (1u << 29)
#define PCH_PP_STATUS  0xC7200
#define PCH_PP_CONTROL 0xC7204

// The EDP transcoder: its own PIPECONF (TRANSCONF) and function control.
// PIPECONF(p) above is pipe A/B/C's; when the panel is on the EDP
// transcoder, pipe A's reads as state-only and THIS one is enabled.
#define TRANSCONF_EDP 0x7F008
#define PIPECONF_ENABLE (1u << 31)
#define PIPECONF_STATE  (1u << 30)
#define TRANS_DDI_FUNC_ENABLE (1u << 31)


// --- what differs between generations -------------------------------------
//
// ONE TABLE PER GENERATION, chosen by device id in find_gpu(); every
// generation-specific path goes through it. Scattered `if (gen == 9)`
// checks were how a gen8 register write could reach gen9 unnoticed: a
// missing slot here is a NULL the caller checks, not a wrong write.
struct intel_gen_ops {
    int gen;
    const uint16_t *ids;
    int nids;
    uint64_t (*stolen_bytes)(uint32_t gms);   // GMCH_CTRL's GMS field, decoded
    void (*readout_log)(void);                // the firmware's state, logged at probe
    int  (*plane_matches)(uint32_t cntr, uint32_t stride, uint32_t pitch);
    int  (*cursor_prepare)(int pipe);         // NULL: the common setup is enough
    uint32_t scanout_align_pages;             // a scanout's GGTT alignment
    void (*claimed)(void);                    // after the common setup; NULL for none
    int  (*read_edid)(int pipe, uint8_t *out, int cap);
    uint32_t caps;                            // DISPLAY_CAP_MODESET/SCALING it supports
    // A mode at the native timing, scaled: source w x h shown in the
    // window at x,y of ww x wh. Required with DISPLAY_CAP_MODESET.
    int  (*fit)(uint32_t w, uint32_t h, uint32_t x, uint32_t y, uint32_t ww, uint32_t wh);
    // kernel.intel_cycle's mechanisms; NULL where a generation has none.
    int  (*pipe_cycle)(void);
    int  (*link_retrain)(void);
    int  (*native)(void);
};
extern const struct intel_gen_ops intel_gen8_ops, intel_gen9_ops;

uint32_t intel_rd(uint32_t off);
void     intel_wr(uint32_t off, uint32_t v);
int      intel_display_pipe(void);   // the pipe scanning the framebuffer, or -1

// intel_aux.c -- DDI A's AUX channel, the eDP panel's.
void intel_aux_init(void);
int  intel_aux_read_edid(uint8_t *out, int cap);   // display_driver.read_edid
int  intel_aux_native_read(uint32_t addr, uint8_t *buf, int len); // DPCD; bytes or -1
int  intel_aux_native_write(uint32_t addr, const uint8_t *buf, int len);

// intel_gen9.c -- Kaby Lake: intel_gen9_ops and what it points at.

// intel_gmbus.c -- the display engine's I2C, for an HDMI/DVI EDID.
int  intel_gmbus_pin_for_port(int port);   // 0 when the DDI has none
int  intel_gmbus_read_edid(int pin, uint8_t *out, int len);   // bytes, 0 on failure

// intel_readout.c -- what the firmware programmed, decoded and compared.
struct display_edid;
void intel_readout_log(const struct display_edid *edid);
// Stage 0 of the sprite plane: the second universal plane's registers
// and the firmware's watermarks, read and logged. Writes nothing.
void intel_readout_planes_log(void);

// intel_modeset.c -- stage 3. pipe_cycle() turns the transcoder and
// pipe off and back on with the link and panel power untouched; 1 when
// the pipe came back and the link is still trained.
int  intel_modeset_pipe_cycle(void);
// link_retrain() also drops the DDI buffer and retrains the DP link
// (patterns 1 and 2, the swing loop) before bringing the pipe back.
int  intel_modeset_link_retrain(void);
// native() runs the whole sequence -- panel power, port clock, the link,
// the timings from the EDID -- for the mode already on screen.
int  intel_modeset_native(void);
// fit() re-places a mode: pipe off, PIPESRC = w x h, the fitter window
// at x,y of ww x wh, pipe on. The link and panel power are untouched.
int  intel_modeset_fit(uint32_t w, uint32_t h, uint32_t x, uint32_t y, uint32_t ww, uint32_t wh);
#define PF_CTL(p)      (0x68080 + (p) * 0x800)
#define PF_WIN_POS(p)  (0x68070 + (p) * 0x800)
#define PF_WIN_SZ(p)   (0x68074 + (p) * 0x800)
#define PF_ENABLE      (1u << 31)

// The DDI A port registers, shared with the readout.
#define DDI_BUF_CTL_A   0x64000
#define DP_TP_CTL_A     0x64040
#define DDI_BUF_TRANS_A 0x64E00   // 9 eDP entries (10 DP), two dwords each

#endif
