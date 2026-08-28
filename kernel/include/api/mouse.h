#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

void mouse_init(void);

// Pointer speed, as a numerator over MOUSE_SPEED_UNIT -- so
// MOUSE_SPEED_UNIT is 1x, half is 0.5x and double is 2x. A fraction
// rather than a multiplier because this kernel has no floating point.
// Clamped to [1, 4x]; 0 would freeze the pointer.
#define MOUSE_SPEED_UNIT 100
void mouse_set_speed(int numerator);

// Acceleration: motion faster than `threshold` counts in a single
// packet has its excess doubled. 0 turns it off, which is the default
// and what this driver did before the setting existed.
void mouse_set_accel_threshold(int threshold);

// The wheel's two knobs, applied where the delta is CONSUMED
// (mouse_get_wheel_delta), so every source -- PS/2's 4th byte,
// virtio-input's REL_WHEEL -- and every consumer see one behaviour.
void mouse_set_scroll_step(int step);     // notches multiplier, >= 1
void mouse_set_scroll_invert(int on);
int  mouse_scroll_step(void);
int  mouse_scroll_invert(void);
void mouse_set_bounds(int width, int height);
// Called by i8042_poll() with one byte already read from the shared
// PS/2 data port. Don't call this from an IRQ handler directly.
void mouse_feed_byte(uint8_t data);

// The pointer state, reachable by any input device -- these are what
// kernel/drivers/input/input.c routes a canonical report into, and the
// PS/2 decoder above is now one caller among several. Speed,
// acceleration and bounds live behind them, so a new device driver
// implements none of that. See kernel/include/kernel/input.h; a driver
// should call input_report_*() rather than these directly.
void mouse_feed_rel(int dx, int dy);
void mouse_feed_abs(int x, int y, int max_x, int max_y);
void mouse_feed_buttons(uint8_t mask);
void mouse_feed_wheel(int notches);

// Fills current absolute position and button bitmask (bit0=left,
// bit1=right, bit2=middle). Position is clamped to the configured bounds.
void mouse_get_state(int *x, int *y, uint8_t *buttons);

// The bounds the pointer is clamped to. These are NOT always the
// display size -- they are whatever mouse_set_bounds() was last given,
// which on a boot where nothing has set them is a small default.
void mouse_get_bounds(int *w, int *h);

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
