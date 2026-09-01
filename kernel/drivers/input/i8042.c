#include "i8042.h"
#include "io.h"
#include "keyboard.h"
#include "mouse.h"
#include "input.h"
#include "driver.h" // DRIVER_DECLARE -- `lsdrv`

DRIVER_DECLARE("i8042", "input", "PS/2 keyboard and mouse controller");

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

// --- the two PS/2 sources ---------------------------------------------
//
// Registered as input sources so that `lsdev` lists every input device
// the same way, whatever bus it is on -- and so the answer to "how does
// this device get serviced?" is in one table rather than one answer per
// driver. Neither carries a poll(): both are driven by their own IRQ
// (1 and 12) into i8042_poll(), which is the interrupt-driven shape
// input.h describes.
//
// They do NOT report through input_report_*(): the 8042 already speaks
// AT set 1, and so do the layout tables, so it feeds its own state
// machine directly. See input.h on why that shortcut is deliberate and
// what removes it.
static const struct input_source ps2_keyboard = {
    .name = "ps2-keyboard",
    .driver = "i8042",
    .caps = INPUT_CAP_KEYS,
    .irq = 1,
};

static const struct input_source ps2_mouse = {
    .name = "ps2-mouse",
    .driver = "i8042",
    .caps = INPUT_CAP_REL | INPUT_CAP_WHEEL,
    .irq = 12,
};

void i8042_register_sources(void) {
    input_register_source(&ps2_keyboard);
    input_register_source(&ps2_mouse);
}
