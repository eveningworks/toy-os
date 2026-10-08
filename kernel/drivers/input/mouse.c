#include "mouse.h"
#include "io.h"
#include "klog.h"

// driver-none: pointer state above the input core

#define CTRL_PORT 0x64
#define DATA_PORT 0x60

static int mouse_x = 0, mouse_y = 0;
static uint8_t mouse_buttons = 0;
static int explorer = 0;   // the device took the 5-button knock
static int bound_w = 80, bound_h = 25;

// Plain PS/2 mice send 3-byte packets (flags, dx, dy). The "IntelliMouse"
// extension (now what every real PS/2 -- and every PS/2-emulated USB --
// mouse speaks) adds a 4th byte carrying signed wheel notches, but only
// once the driver asks for it via a specific sample-rate "magic knock"
// (see mouse_init()). packet_size reflects whichever this device turned
// out to support, decided once at init and constant after.
// Pointer speed and acceleration, both settings (kernel/lib/mouse_config.c
// registers them). Defaults are 1x and off, so a machine with no
// /etc keys behaves exactly as it did before these existed.
static int g_speed_num = MOUSE_SPEED_UNIT;
static int g_accel_threshold = 0;

void mouse_set_speed(int numerator) {
    // Clamped rather than trusted: a zero would freeze the pointer with
    // no way to reach the setting that did it, which is the kind of
    // knob that needs a boot to undo.
    if (numerator < 1) numerator = 1;
    if (numerator > MOUSE_SPEED_UNIT * 4) numerator = MOUSE_SPEED_UNIT * 4;
    g_speed_num = numerator;
}

void mouse_set_accel_threshold(int threshold) {
    if (threshold < 0) threshold = 0;
    g_accel_threshold = threshold;
}

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

    // THE SECOND KNOCK, ONLY IF THE FIRST TOOK. `200, 200, 80` on a
    // device already at ID 3 moves it to ID 4 -- Microsoft's
    // IntelliMouse Explorer -- which is the ONLY way a PS/2 mouse
    // reports its two thumb buttons. Asked in this order because the
    // Explorer knock is defined as following the wheel one; a device
    // that declined the first cannot take the second.
    //
    // **IT CHANGES THE WHEEL FIELD.** At ID 3 byte 3 is a signed 8-bit
    // notch count; at ID 4 it is a signed FOUR-bit count in bits 0-3
    // with the thumb buttons in bits 4 and 5. Decoding an ID 4 packet
    // with the ID 3 rule reads a thumb press as a wheel spin of -16 or
    // -32, which is why the two are split at the decode below rather
    // than sharing a path.
    if (device_id == 3) {
        static const uint8_t knock2[3] = {200, 200, 80};
        for (int i = 0; i < 3; i++) {
            mouse_write(0xF3);
            mouse_read(); // ACK
            mouse_write(knock2[i]);
            mouse_read(); // ACK
        }
        mouse_write(0xF2);
        mouse_read();     // ACK
        device_id = mouse_read();
    }
    explorer = (device_id == 4);
    packet_size = (device_id == 3 || device_id == 4) ? 4 : 3;

    klog_write(explorer
        ? "mouse: PS/2 5-button wheel mouse detected (Explorer, 4-byte packets)\n"
        : packet_size == 4
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
static void clamp_to_bounds(void) {
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= bound_w) mouse_x = bound_w - 1;
    if (mouse_y >= bound_h) mouse_y = bound_h - 1;
}

// See api/mouse.h. Clamped through the same helper the motion path
// uses, so a warp cannot put the pointer anywhere a device could not.
void mouse_set_position(int x, int y) {
    mouse_x = x;
    mouse_y = y;
    clamp_to_bounds();
}

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

    mouse_feed_rel(dx, dy);
    // The thumb buttons live in the WHEEL byte, not in byte 0, so the
    // mask is assembled from both -- and only on an Explorer device,
    // where bits 4/5 of byte 3 mean that. On an ID 3 wheel mouse those
    // same bits are part of the notch count.
    uint8_t btn = flags & 0x07;
    if (explorer) btn |= (uint8_t)((packet[3] >> 1) & 0x18);
    mouse_feed_buttons(btn);

    if (explorer) {
        // FOUR-BIT SIGNED, sign-extended by hand: bit 3 is the sign, so
        // 0x8..0xF are -8..-1. Negated for the same reason as below.
        int notch = packet[3] & 0x0F;
        if (notch & 0x08) notch -= 16;
        mouse_feed_wheel(-notch);
    } else if (packet_size == 4) {
        // Wheel byte is a signed 8-bit notch count -- almost always
        // -1 or +1 per physical click of the wheel, occasionally more
        // if it's spun fast. Convention (matches every real mouse):
        // negative raw value = wheel pushed away from the user, which
        // is the "scroll up / reveal older content" direction, so this
        // is negated before accumulating into wheel_delta's
        // positive-means-up sense documented in mouse.h.
        int8_t raw = (int8_t)packet[3];
        mouse_feed_wheel(-raw);
    }
}

// --- the pointer state, reachable by ANY device -----------------------
//
// Extracted from the PS/2 packet decoder above so that a device which is
// not a PS/2 mouse -- virtio-input today, USB HID later -- moves the
// same pointer through the same speed, acceleration and bounds rules,
// rather than each driver growing its own copy of them. The core
// (kernel/drivers/input/input.c) is what routes to these; the decoder
// above is now just one caller among several.

void mouse_feed_rel(int dx, int dy) {
    // SPEED, then ACCELERATION -- both applied here, at the one place
    // raw device deltas become screen motion, so nothing downstream
    // needs to know either exists.
    //
    // Speed is a numerator over MOUSE_SPEED_UNIT rather than a float:
    // there is no floating point in this kernel (-mno-sse). Doing it in
    // one multiply-then-divide keeps a slow setting from rounding every
    // small movement to zero, which dividing first would.
    dx = dx * g_speed_num / MOUSE_SPEED_UNIT;
    dy = dy * g_speed_num / MOUSE_SPEED_UNIT;

    // Acceleration is the classic threshold rule -- move faster than
    // `threshold` counts in one packet and the excess is doubled. This
    // is what PS/2 mice and X11's original "mouse acceleration" did,
    // deliberately rather than a curve: a curve needs tuning constants
    // nobody here can measure, and the threshold rule is predictable.
    // Applied per AXIS on the raw magnitude; a diagonal flick therefore
    // accelerates on both, which is what makes it feel symmetric.
    if (g_accel_threshold > 0) {
        if (dx > g_accel_threshold)  dx += dx - g_accel_threshold;
        if (dx < -g_accel_threshold) dx += dx + g_accel_threshold;
        if (dy > g_accel_threshold)  dy += dy - g_accel_threshold;
        if (dy < -g_accel_threshold) dy += dy + g_accel_threshold;
    }

    mouse_x += dx;
    mouse_y -= dy; // PS/2 Y increases upward; screen Y increases downward
    clamp_to_bounds();
}

// An ABSOLUTE device -- a tablet or a touchscreen -- reports a position
// in its own axis range, and the scaling to the screen happens HERE
// rather than in the driver: the range is a property of the device, the
// screen size is not something a driver should have to track, and doing
// it in one place means both facts meet exactly once.
//
// Speed and acceleration are deliberately NOT applied. They exist to
// turn a relative device's counts into comfortable screen motion; an
// absolute device is already saying where the pointer IS, and scaling
// that would move the pointer somewhere the user is not pointing.
void mouse_feed_abs(int x, int y, int max_x, int max_y) {
    if (max_x <= 0 || max_y <= 0) return;   // no range: nothing to scale by
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > max_x) x = max_x;
    if (y > max_y) y = max_y;

    // 64-bit intermediates: a tablet's range is commonly 0..32767 and
    // the screen can be 1920 wide, which overflows a 32-bit multiply
    // only just -- but "only just" is how this class of bug ships.
    mouse_x = (int)(((int64_t)x * (bound_w - 1)) / max_x);
    mouse_y = (int)(((int64_t)y * (bound_h - 1)) / max_y);
    clamp_to_bounds();
}

// **EVERY CHANGE IS ALSO QUEUED, BECAUSE THE LEVEL ALONE LOSES CLICKS.**
// Two samplers sit above this -- the USB HID drain takes every queued
// report in one pass, and win_input_poll() runs from scheduler_idle() --
// so a press and its release both landing between two passes read back
// as "nothing happened". evdev emits BTN_* transitions and NT's mouse
// class driver queues per-button DOWN/UP flags; keyboard.c beside this
// file already does it (keyboard_try_get_key). The thumb buttons
// made it visible because a thumb tap is short.
// Each edge keeps WHERE the pointer was when the driver reported it: a
// press and a release a long way apart, drained in one pass, must not
// both read the pass's final position (a click becomes a drag).
#define BTN_TRANS_MAX 32
static struct { uint8_t mask; int x, y; } btn_trans[BTN_TRANS_MAX];
static int btn_trans_head, btn_trans_tail;

// FIVE BITS: left, right, middle, then the two thumb buttons (SIDE and
// EXTRA, kernel/input.h). Masked rather than stored whole so a driver
// reporting a button nothing here has a name for cannot invent one --
// a HID mouse's byte 0 has three more bits above these.
//
// **A REMOTE DESKTOP'S BUTTONS ARE A SECOND MASK, OR'D IN**
// (mouse_inject_buttons(), kernel/input.h): every device report carries
// its whole mask, so one shared level would let a local mouse's next
// packet release a remote drag.
static uint8_t g_dev_buttons, g_inj_buttons;

static void set_buttons(uint8_t mask) {
    if (mask != mouse_buttons) {
        int next = (btn_trans_head + 1) % BTN_TRANS_MAX;
        // Dropping the OLDEST keeps the most recent releases, which are
        // the ones that un-stick a button -- keyboard.c's rule.
        if (next == btn_trans_tail)
            btn_trans_tail = (btn_trans_tail + 1) % BTN_TRANS_MAX;
        btn_trans[btn_trans_head].mask = mask;
        btn_trans[btn_trans_head].x = mouse_x;
        btn_trans[btn_trans_head].y = mouse_y;
        btn_trans_head = next;
    }
    mouse_buttons = mask;
}

void mouse_feed_buttons(uint8_t mask) {
    g_dev_buttons = mask & 0x1F;
    set_buttons(g_dev_buttons | g_inj_buttons);
}

void mouse_inject_buttons(uint8_t mask) {
    g_inj_buttons = mask & 0x1F;
    set_buttons(g_dev_buttons | g_inj_buttons);
}

int mouse_try_get_button_edge(uint8_t *out_mask, int *out_x, int *out_y) {
    if (btn_trans_tail == btn_trans_head) return 0;
    if (out_mask) *out_mask = btn_trans[btn_trans_tail].mask;
    if (out_x) *out_x = btn_trans[btn_trans_tail].x;
    if (out_y) *out_y = btn_trans[btn_trans_tail].y;
    btn_trans_tail = (btn_trans_tail + 1) % BTN_TRANS_MAX;
    return 1;
}

int mouse_button_edge_pending(void) { return btn_trans_tail != btn_trans_head; }

int mouse_try_get_button_transition(uint8_t *out_mask) {
    return mouse_try_get_button_edge(out_mask, 0, 0);
}

void mouse_feed_wheel(int notches) { wheel_delta += notches; }

// See api/mouse.h: applied at the CONSUMING read, the one point every
// wheel source and consumer share -- transforming at feed time would
// re-multiply whatever accumulated between reads of the setting.
static int scroll_step = 1;
static int scroll_invert = 0;

void mouse_set_scroll_step(int step)  { scroll_step = step >= 1 ? step : 1; }
void mouse_set_scroll_invert(int on)  { scroll_invert = on ? 1 : 0; }
int  mouse_scroll_step(void)          { return scroll_step; }
int  mouse_scroll_invert(void)        { return scroll_invert; }

int mouse_get_wheel_delta(void) {
    int d = wheel_delta;
    wheel_delta = 0;
    d *= scroll_step;
    return scroll_invert ? -d : d;
}

// The bounds the pointer is currently clamped to. NOT necessarily the
// display's size: they start at a small default and are set by whoever
// owns presentation (win_input.c, once a compositor appears). A caller
// that needs to reason about where the pointer can go must ASK rather
// than assume the screen -- a KTEST assuming gfx_width() computed
// positions 16x too small on a boot where nothing had set them yet.
void mouse_get_bounds(int *w, int *h) {
    if (w) *w = bound_w;
    if (h) *h = bound_h;
}

void mouse_get_state(int *x, int *y, uint8_t *buttons) {
    *x = mouse_x;
    *y = mouse_y;
    *buttons = mouse_buttons;
}
