#ifndef KERNEL_SCREEN_H
#define KERNEL_SCREEN_H
#include <stdint.h>

// A RUNTIME MODE CHANGE, in the one order that is safe -- the display
// driver, then gfx, the console, the pointer's bounds, the compositor's
// grant, and last the WIN_EV_SCREEN broadcast. font_config.c's rule for
// the font: one function changes the screen for real, so there is one
// place that can forget a step. Refuses a mode the driver does not
// list, and a refused mode leaves the old one running (every driver's
// set_mode contract). Returns 1 on success, 1 for the current mode.
int screen_set_mode(uint32_t w, uint32_t h);

// Is `w`x`h` in the active driver's mode list?
int screen_mode_listed(uint32_t w, uint32_t h);

#endif
