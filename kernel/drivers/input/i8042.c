#include "i8042.h"
#include "io.h"
#include "keyboard.h"
#include "mouse.h"
#include "input.h"
#include "driver.h" // DRIVER_DECLARE -- `lsdrv`
#include "clocksource.h" // the write budget is TIME, not a loop count
#include "barrier.h"     // cpu_relax()
#include "irqflags.h"    // the probe must not have its reply stolen by IRQ1
#include "ratelimit.h"
#include "kfmt.h"
#include "klog.h"

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

// IS THERE A CONTROLLER AT ALL: decided ONCE, at registration
// (i8042_probe()), never from a runtime timeout -- a busy controller that
// misses one write is still there. Absent, nothing is registered, polled
// or written: an LED change from key_event(), with interrupts off, then
// costs nothing.
static int g_present;

int i8042_present(void) { return g_present; }

// Linux's budget for one controller wait (I8042_CTL_TIMEOUT: 10000 x
// 50 us). In TIME, because a bare loop count is a different budget on
// every CPU; the iteration cap is only a backstop for a clocksource that
// does not advance with interrupts off.
#define I8042_BUDGET_NS 500000000ull
static int wait_status(uint8_t mask, int set) {
    uint64_t end = clocksource_now_ns() + I8042_BUDGET_NS;
    for (long n = 0; n < 100000000L; n++) {
        if (!!(inb(STATUS_PORT) & mask) == set) return 1;
        if (clocksource_now_ns() >= end) return 0;
        cpu_relax();
    }
    return 0;
}

// One byte to the keyboard; 0 if the controller never took it -- that
// command fails and is said, at most a line a second, and nothing else.
static int kbd_write(uint8_t b) {
    if (!g_present) return 0;
    if (!wait_status(STATUS_INPUT_FULL, 0)) {
        static struct ratelimit rl;
        unsigned held = 0;
        if (ratelimit_ok(&rl, &held))
            klog_printf(KLOG_WARN "i8042: keyboard write 0x%x timed out (%u more not logged)\n",
                        b, held);
        return 0;
    }
    outb(DATA_PORT, b);
    return 1;
}

static void led_idle(void) {
    g_led_state = LED_IDLE;
    g_led_resends = 0;
}

static void i8042_set_leds(uint8_t leds) {
    if (!g_present) return;
    g_led_bits = leds & (INPUT_LED_SCROLL | INPUT_LED_NUM | INPUT_LED_CAPS);
    g_led_resends = 0;
    g_led_state = LED_WANT_ACK1;
    if (!kbd_write(KBD_CMD_SET_LEDS)) led_idle();
}

// 1 when `data` belonged to the LED exchange rather than to a key.
static int led_byte(uint8_t data) {
    if (g_led_state == LED_IDLE) return data == KBD_ACK;   // a stray ACK is never a key
    if (data == KBD_RESEND && g_led_resends++ < 2) {
        if (!kbd_write(g_led_state == LED_WANT_ACK1 ? KBD_CMD_SET_LEDS : g_led_bits)) led_idle();
        return 1;
    }
    if (data != KBD_ACK) return 0;
    if (g_led_state == LED_WANT_ACK1) {
        g_led_state = LED_WANT_ACK2;
        if (!kbd_write(g_led_bits)) led_idle();
    } else {
        led_idle();
    }
    return 1;
}

void i8042_poll(void) {
    if (!g_present) return;
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

// A controller answers: the status port is decoded (an empty one reads
// all ones), and asked for its command byte (0x20, as Linux's
// i8042_controller_init() does first) it replies within the budget.
// Interrupts off, or IRQ1's i8042_poll() would take the reply.
static int i8042_probe(void) {
    if (inb(STATUS_PORT) == 0xFF) return 0;
    uint64_t f = irq_save();
    for (int i = 0; i < 16 && (inb(STATUS_PORT) & STATUS_OUTPUT_FULL); i++)
        (void)inb(DATA_PORT);                       // whatever was waiting
    int ok = wait_status(STATUS_INPUT_FULL, 0);
    if (ok) {
        outb(STATUS_PORT, 0x20);                    // read the command byte
        ok = wait_status(STATUS_OUTPUT_FULL, 1);
        if (ok) (void)inb(DATA_PORT);
    }
    irq_restore(f);
    return ok;
}

void i8042_register_sources(void) {
    g_present = i8042_probe();
    if (!g_present) {
        // Not advertised: a source the input core lists must work.
        klog_write("i8042: no controller answered -- PS/2 keyboard and mouse not registered\n");
        return;
    }
    input_register_source(&ps2_keyboard);
    input_register_source(&ps2_mouse);
}
