#ifndef INTEL_DISPLAY_H
#define INTEL_DISPLAY_H
#include <stdint.h>
#include "display.h"

// Intel integrated graphics, gen8 (Broadwell) and gen9 (Kaby Lake) -- the DISPLAY ENGINE
// only, as a display_driver. What Linux's i915 calls fastboot: the
// firmware's GOP has already lit the panel and programmed a pipe, so
// the driver READS OUT that state and adopts it rather than setting a
// mode. On top of the inherited scanout it adds what vesafb cannot:
// the cursor plane, the page flip, the backlight PWM (gen8), and the
// display power well. The eDP modeset (intel_modeset.c) is gen8 only.
//
// Register it after virtio-gpu/vmsvga and before bochs/vesafb. It
// claims only a gen8/gen9 device whose live plane matches GRUB's framebuffer
// exactly, and declines otherwise -- vesafb then takes the same pixels.
void intel_display_register(void);

// The display power well (pipes B/C, DDI B-D and display AUDIO). The
// HDMI codec behind 8086:160c answers nothing while it is off. Returns
// 1 when the well reports the requested state.
int intel_display_power_well(int on);

// For the KTEST: the pure halves of the register encodings. CURPOS is
// sign-magnitude, 13 bits per axis; a duty is the PWM period scaled by
// a percent and never 0 for a non-zero percent.
uint32_t intel_display_curpos_field(int v);
uint32_t intel_display_duty(uint32_t max, int percent);
int      intel_display_percent(uint32_t max, uint32_t duty);
// Whether a plane's control and stride registers describe a linear
// XRGB8888 surface of `pitch` bytes, in generation `gen`'s encoding.
int      intel_display_plane_matches(int gen, uint32_t cntr, uint32_t stride, uint32_t pitch);
// 1 while this driver owns the screen (never under QEMU).
int intel_display_active(void);
// The generation it claimed (8 or 9), 0 when inactive.
int intel_display_gen(void);
// How many scanouts it set up: 3 with the flip, 1 without, 0 inactive.
int intel_display_scanout_count(void);

// intel_readout.c's pure decoders, exposed for the KTESTs.
struct edid_timing;
struct intel_trans_regs { uint32_t htotal, hblank, hsync, vtotal, vblank, vsync; };
void     intel_display_timing_from_regs(const struct intel_trans_regs *r, struct edid_timing *out);
uint32_t intel_display_port_clock_khz(uint32_t port_clk_sel);
uint32_t intel_display_dotclock_khz(uint32_t port_khz, uint32_t link_m, uint32_t link_n);
int      intel_display_timing_same(const struct edid_timing *a, const struct edid_timing *b);
// Gen9: an HDMI-mode DPLL's CFGCR1/CFGCR2 to the pixel clock in kHz, 0
// for a disabled or unencodable one.
uint32_t intel_display_gen9_hdmi_khz(uint32_t cfgcr1, uint32_t cfgcr2);

// The panel fitter's window for a mode of w x h on a panel of pw x ph
// under `scaling` (enum display_scaling); pure, for the KTESTs.
void intel_display_fit_window(int scaling, uint32_t w, uint32_t h, uint32_t pw, uint32_t ph,
                              uint32_t *x, uint32_t *y, uint32_t *ww, uint32_t *wh);

// Stage 3, one mechanism at a time: the pipe off and on with the link
// kept. 1 when it came back trained; 0 (and a log line naming the
// register) otherwise. Refused without the hardware.
int intel_display_pipe_cycle(void);
int intel_display_link_retrain(void);
int intel_display_native(void);

#endif
