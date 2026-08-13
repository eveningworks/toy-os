#ifndef VESAFB_H
#define VESAFB_H

#include "display.h"

// GRUB's multiboot2 linear framebuffer, as an ordinary display driver.
// Registers LAST: it always claims, so it is the fallback every other
// driver gets first refusal ahead of. See vesafb.c's top comment for
// why the generic path is a driver at all.
void vesafb_register(void);

// GRUB's geometry, readable before any driver is active -- a
// hardware driver uses it to take the display over at exactly the mode
// already on screen.
void vesafb_get_probe_surface(struct display_surface *out);

#endif
