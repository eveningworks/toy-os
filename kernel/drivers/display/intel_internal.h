// Shared between the Intel display driver's files (intel_display.c,
// intel_aux.c, intel_readout.c): the MMIO accessors and the one fact
// the other files need. Not a boundary -- one driver in three files,
// split by concern (the plane and its planes; the AUX channel; the
// firmware-state readout), the userland/wm/ pattern.
#ifndef INTEL_INTERNAL_H
#define INTEL_INTERNAL_H

#include <stdint.h>

uint32_t intel_rd(uint32_t off);
void     intel_wr(uint32_t off, uint32_t v);
int      intel_display_pipe(void);   // the pipe scanning the framebuffer, or -1

// intel_aux.c -- DDI A's AUX channel, the eDP panel's.
void intel_aux_init(void);
int  intel_aux_read_edid(uint8_t *out, int cap);   // display_driver.read_edid
int  intel_aux_native_read(uint32_t addr, uint8_t *buf, int len); // DPCD; bytes or -1

// intel_readout.c -- what the firmware programmed, decoded and compared.
struct display_edid;
void intel_readout_log(const struct display_edid *edid);

#endif
