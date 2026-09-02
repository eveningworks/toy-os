#ifndef INTEL_DISPLAY_H
#define INTEL_DISPLAY_H
#include <stdint.h>
#include "display.h"

// Intel integrated graphics, gen8 (Broadwell) -- the DISPLAY ENGINE
// only, as a display_driver. What Linux's i915 calls fastboot: the
// firmware's GOP has already lit the panel and programmed a pipe, so
// the driver READS OUT that state and adopts it rather than setting a
// mode. On top of the inherited scanout it adds what vesafb cannot:
// the cursor plane, the backlight PWM, and the display power well.
//
// Register it after virtio-gpu/vmsvga and before bochs/vesafb. It
// claims only a gen8 device whose live plane matches GRUB's framebuffer
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
// 1 while this driver owns the screen (never under QEMU).
int intel_display_active(void);
// How many scanouts it set up: 3 with the flip, 1 without, 0 inactive.
int intel_display_scanout_count(void);

#endif
