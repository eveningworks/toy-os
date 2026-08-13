#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

void mouse_init(void);
void mouse_set_bounds(int width, int height);
// Called by i8042_poll() with one byte already read from the shared
// PS/2 data port. Don't call this from an IRQ handler directly.
void mouse_feed_byte(uint8_t data);

// Fills current absolute position and button bitmask (bit0=left,
// bit1=right, bit2=middle). Position is clamped to the configured bounds.
void mouse_get_state(int *x, int *y, uint8_t *buttons);

// Returns the scroll wheel movement accumulated since the last call, in
// notches (positive = wheel pushed away from the user / "up", negative =
// pulled toward the user / "down" -- the same sense as a typical desktop:
// scrolling "up" reveals earlier/older content), and resets the
// accumulator to 0. Always 0 if mouse_init() didn't find IntelliMouse
// wheel support on the attached device (see mouse.c's init sequence) --
// callers don't need to check for that separately, a plain 3-byte mouse
// just never reports anything here.
int mouse_get_wheel_delta(void);

#endif
