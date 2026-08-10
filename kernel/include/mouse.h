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

#endif
