#include "i8042.h"
#include "io.h"
#include "keyboard.h"
#include "mouse.h"

#define STATUS_PORT 0x64
#define DATA_PORT   0x60

#define STATUS_OUTPUT_FULL 0x01
#define STATUS_AUX_DATA    0x20

void i8042_poll(void) {
    // Bounded so a stuck/chattering controller can't wedge us in an
    // interrupt handler forever.
    for (int guard = 0; guard < 64; guard++) {
        uint8_t status = inb(STATUS_PORT);
        if (!(status & STATUS_OUTPUT_FULL)) return;

        uint8_t data = inb(DATA_PORT);
        if (status & STATUS_AUX_DATA) {
            mouse_feed_byte(data);
        } else {
            keyboard_feed_byte(data);
        }
    }
}
