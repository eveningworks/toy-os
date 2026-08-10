#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

// Special key codes pushed into the input stream alongside normal ASCII.
// Chosen outside the 0-127 ASCII range so they can't collide with real chars.
#define KEY_ARROW_UP   0x91
#define KEY_ARROW_DOWN 0x92
#define KEY_PAGE_UP    0x93
#define KEY_PAGE_DOWN  0x94

// Called by i8042_poll() with one byte already read from the shared
// PS/2 data port. Don't call this from an IRQ handler directly.
void keyboard_feed_byte(uint8_t sc);

// Blocking read of a single byte from the input stream: either an ASCII
// char or one of the KEY_* codes above.
int keyboard_getchar(void);

// Same as keyboard_getchar but returns -1 immediately if nothing is
// waiting, instead of blocking. Used by the GUI event loop.
int keyboard_try_getchar(void);

// Blocking read of one line into buf (max len-1 chars + null terminator).
// Echoes typed characters to the VGA console and handles backspace.
void keyboard_read_line(char *buf, unsigned int len);

#endif
