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
#define STATUS_INPUT_FULL  0x02
#define STATUS_AUX_DATA    0x20

#define KBD_CMD_SET_LEDS 0xED
#define KBD_ACK          0xFA
#define KBD_RESEND       0xFE

// THE LED EXCHANGE, as Linux's atkbd does it: 0xED, an ACK, the LED byte,
// a second ACK. NEVER WAITED FOR -- it starts inside the keyboard's own
// interrupt -- so it is a state the ACKs advance as i8042_poll() drains
// them. The ACKs are consumed here: fed to the scancode decoder, 0xFA
// would read as the release of a key that does not exist. A scancode
// that arrives mid-exchange is passed through and the state kept, since
// a keyboard waiting for its LED byte must still be sent one.
enum { LED_IDLE, LED_WANT_ACK1, LED_WANT_ACK2 };
static int g_led_state = LED_IDLE;
static uint8_t g_led_bits;
static int g_led_resends;

static void kbd_write(uint8_t b) {
    // Bounded: a controller that never drains its input buffer must not
    // wedge an interrupt handler.
    for (int i = 0; i < 10000 && (inb(STATUS_PORT) & STATUS_INPUT_FULL); i++) { }
    outb(DATA_PORT, b);
}

static void i8042_set_leds(uint8_t leds) {
    g_led_bits = leds & (INPUT_LED_SCROLL | INPUT_LED_NUM | INPUT_LED_CAPS);
    g_led_resends = 0;
    g_led_state = LED_WANT_ACK1;
    kbd_write(KBD_CMD_SET_LEDS);
}

// 1 when `data` belonged to the LED exchange rather than to a key.
static int led_byte(uint8_t data) {
    if (g_led_state == LED_IDLE) return data == KBD_ACK;   // a stray ACK is never a key
    if (data == KBD_RESEND && g_led_resends++ < 2) {
        kbd_write(g_led_state == LED_WANT_ACK1 ? KBD_CMD_SET_LEDS : g_led_bits);
        return 1;
    }
    if (data != KBD_ACK) return 0;
    if (g_led_state == LED_WANT_ACK1) {
        g_led_state = LED_WANT_ACK2;
        kbd_write(g_led_bits);
    } else {
        g_led_state = LED_IDLE;
    }
    return 1;
}

void i8042_poll(void) {
    // Bounded so a stuck/chattering controller can't wedge us in an
    // interrupt handler forever.
    for (int guard = 0; guard < 64; guard++) {
        uint8_t status = inb(STATUS_PORT);
        if (!(status & STATUS_OUTPUT_FULL)) return;

        uint8_t data = inb(DATA_PORT);
        if (status & STATUS_AUX_DATA) {
            mouse_feed_byte(data);
        } else if (!led_byte(data)) {
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
    .set_leds = i8042_set_leds,
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
