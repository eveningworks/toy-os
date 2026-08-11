#include "mouse.h"
#include "io.h"
#include "klog.h"

#define CTRL_PORT 0x64
#define DATA_PORT 0x60

static int mouse_x = 0, mouse_y = 0;
static uint8_t mouse_buttons = 0;
static int bound_w = 80, bound_h = 25;

// Plain PS/2 mice send 3-byte packets (flags, dx, dy). The "IntelliMouse"
// extension (now what every real PS/2 -- and every PS/2-emulated USB --
// mouse speaks) adds a 4th byte carrying signed wheel notches, but only
// once the driver asks for it via a specific sample-rate "magic knock"
// (see mouse_init()). packet_size reflects whichever this device turned
// out to support, decided once at init and constant after.
static uint8_t packet[4];
static int packet_index = 0;
static int packet_size = 3;
static int wheel_delta = 0;

static void wait_input_clear(void) {
    int timeout = 100000;
    while ((inb(CTRL_PORT) & 0x02) && timeout--) ;
}

static void wait_output_full(void) {
    int timeout = 100000;
    while (!(inb(CTRL_PORT) & 0x01) && timeout--) ;
}

static void mouse_write(uint8_t b) {
    wait_input_clear();
    outb(CTRL_PORT, 0xD4);
    wait_input_clear();
    outb(DATA_PORT, b);
}

static uint8_t mouse_read(void) {
    wait_output_full();
    return inb(DATA_PORT);
}

void mouse_set_bounds(int width, int height) {
    bound_w = width;
    bound_h = height;
    if (mouse_x >= bound_w) mouse_x = bound_w - 1;
    if (mouse_y >= bound_h) mouse_y = bound_h - 1;
}

void mouse_init(void) {
    // mouse_read()/mouse_write() busy-poll ports 0x64/0x60 directly,
    // synchronously, outside of interrupt context. If a keyboard (or
    // stray mouse) IRQ fires during that window, i8042_poll() drains
    // bytes from the same port asynchronously and can steal the exact
    // ACK byte this handshake is waiting for -- an intermittent race,
    // worse now that i8042_poll() drains in a loop instead of one byte
    // at a time. Disabling interrupts for this short setup sequence
    // removes the race entirely; IRQ1/IRQ12 have nothing to hand off to
    // yet at this point anyway (the window manager hasn't started
    // reading mouse state), so there's nothing lost by doing so.
    __asm__ volatile ("cli");

    wait_input_clear();
    outb(CTRL_PORT, 0xA8); // enable auxiliary (mouse) device

    wait_input_clear();
    outb(CTRL_PORT, 0x20); // read controller command byte
    uint8_t status = mouse_read();
    status |= 0x02;   // enable IRQ12
    status &= ~0x20;  // enable mouse clock

    wait_input_clear();
    outb(CTRL_PORT, 0x60);
    wait_input_clear();
    outb(DATA_PORT, status);

    mouse_write(0xF6); // set defaults
    mouse_read();       // ACK

    // IntelliMouse "magic knock": setting the sample rate to 200, then
    // 100, then 80 in a row (each a set-sample-rate command 0xF3 with no
    // pause for anything else in between) is a no-op for the mouse's
    // actual sample rate on a wheel-capable device, but it's the
    // documented, universally-supported handshake that switches such a
    // device into reporting 4-byte packets with a wheel delta in the
    // 4th byte instead of plain 3-byte packets. A non-wheel mouse just
    // sets its sample rate three times and ignores the significance.
    // Reading the device ID (0xF2) afterwards tells us which happened:
    // ID 0 is a plain mouse, ID 3 is a wheel mouse that took the knock.
    static const uint8_t knock[3] = {200, 100, 80};
    for (int i = 0; i < 3; i++) {
        mouse_write(0xF3);
        mouse_read(); // ACK
        mouse_write(knock[i]);
        mouse_read(); // ACK
    }
    mouse_write(0xF2); // read device ID
    mouse_read();       // ACK
    uint8_t device_id = mouse_read();
    packet_size = (device_id == 3) ? 4 : 3;

    klog_write(packet_size == 4
        ? "mouse: PS/2 wheel mouse detected (4-byte packets)\n"
        : "mouse: PS/2 mouse detected (3-byte packets, no wheel)\n");

    mouse_write(0xF4); // enable data reporting
    mouse_read();       // ACK

    __asm__ volatile ("sti");

    mouse_x = bound_w / 2;
    mouse_y = bound_h / 2;
    packet_index = 0;
    wheel_delta = 0;
}

// Processes one byte already read from the 8042 by i8042_poll(). This
// must NOT read port 0x60 itself -- see i8042.h for why.
void mouse_feed_byte(uint8_t data) {

    if (packet_index == 0 && !(data & 0x08)) {
        return; // not a valid first byte (sync bit missing) -- resync
    }

    packet[packet_index++] = data;
    if (packet_index < packet_size) return;
    packet_index = 0;

    uint8_t flags = packet[0];
    int dx = packet[1];
    int dy = packet[2];

    if (flags & 0x10) dx -= 256; // sign-extend 9th bit (negative X)
    if (flags & 0x20) dy -= 256; // sign-extend 9th bit (negative Y)

    mouse_x += dx;
    mouse_y -= dy; // PS/2 Y increases upward; screen Y increases downward

    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= bound_w) mouse_x = bound_w - 1;
    if (mouse_y >= bound_h) mouse_y = bound_h - 1;

    mouse_buttons = flags & 0x07;

    if (packet_size == 4) {
        // Wheel byte is a signed 8-bit notch count -- almost always
        // -1 or +1 per physical click of the wheel, occasionally more
        // if it's spun fast. Convention (matches every real mouse):
        // negative raw value = wheel pushed away from the user, which
        // is the "scroll up / reveal older content" direction, so this
        // is negated before accumulating into wheel_delta's
        // positive-means-up sense documented in mouse.h.
        int8_t raw = (int8_t)packet[3];
        wheel_delta -= raw;
    }
}

int mouse_get_wheel_delta(void) {
    int d = wheel_delta;
    wheel_delta = 0;
    return d;
}

void mouse_get_state(int *x, int *y, uint8_t *buttons) {
    *x = mouse_x;
    *y = mouse_y;
    *buttons = mouse_buttons;
}
