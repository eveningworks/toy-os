// KTESTs for the HID boot-protocol decoding.
//
// These need no controller and no device: input_usbhid.c's two differs take
// their state as a parameter precisely so a test can drive them
// directly. That matters because the differs are where the bugs are and
// the half hardware cannot help with -- a real keyboard will not
// politely send a rollover report on demand.
//
// The differs call input_report_*(), which reaches the live key ring
// and the live pointer, so the tests that would move the cursor save
// and restore it. That is the same precondition-establishing rule
// input_test.c already follows.
#include "usb_hid.h"
#include "input.h"
#include "mouse.h"
#include "ktest.h"
#include "string.h"

// --- the usage table --------------------------------------------------

KTEST("usb-hid", "the HID usage table maps the keys it claims to") {
    // Anchors across the whole range rather than a spot check: letters,
    // digits, the named keys, the keypad and both modifier ends.
    KTEST_ASSERT_EQ(usb_hid_keycode(0x04), 30);                    // a
    KTEST_ASSERT_EQ(usb_hid_keycode(0x1D), 44);                    // z
    KTEST_ASSERT_EQ(usb_hid_keycode(0x1E), 2);                     // 1
    KTEST_ASSERT_EQ(usb_hid_keycode(0x27), 11);                    // 0
    KTEST_ASSERT_EQ(usb_hid_keycode(0x28), INPUT_KEY_ENTER);
    KTEST_ASSERT_EQ(usb_hid_keycode(0x29), INPUT_KEY_ESC);
    KTEST_ASSERT_EQ(usb_hid_keycode(0x2C), INPUT_KEY_SPACE);
    KTEST_ASSERT_EQ(usb_hid_keycode(0x3A), INPUT_KEY_F1);
    KTEST_ASSERT_EQ(usb_hid_keycode(0x45), INPUT_KEY_F12);
    KTEST_ASSERT_EQ(usb_hid_keycode(0x4F), INPUT_KEY_RIGHT);
    KTEST_ASSERT_EQ(usb_hid_keycode(0x62), INPUT_KEY_KP0);
    KTEST_ASSERT_EQ(usb_hid_keycode(0xE0), INPUT_KEY_LEFTCTRL);
    KTEST_ASSERT_EQ(usb_hid_keycode(0xE1), INPUT_KEY_LEFTSHIFT);
    KTEST_ASSERT_EQ(usb_hid_keycode(0xE5), INPUT_KEY_RIGHTSHIFT);
    KTEST_ASSERT_EQ(usb_hid_keycode(0xE7), INPUT_KEY_RIGHTMETA);
}

// The ISO key between Left Shift and Z. It carries `|` on every Nordic
// layout, and input.h records this exact key going missing from the
// virtio-input table -- so a pipeline could be typed on PS/2 and not on
// virtio, with nothing noticing. A regression guard for a bug this
// project has already had once, on a different input path.
KTEST("usb-hid", "usage 0x64 is the ISO key, not a hole in the table") {
    KTEST_ASSERT_EQ(usb_hid_keycode(0x64), INPUT_KEY_102ND);
    KTEST_ASSERT_EQ(usb_hid_keycode(0x64), 86);
}

KTEST("usb-hid", "an unmapped usage is dropped, not guessed") {
    // 0x00 is "no key", and the reserved space above the keypad has no
    // meaning here. Reporting some other key for these would be worse
    // than reporting nothing -- the same call ext_keycode() makes.
    KTEST_ASSERT_EQ(usb_hid_keycode(0x00), 0);
    KTEST_ASSERT_EQ(usb_hid_keycode(0xA5), 0);
    KTEST_ASSERT_EQ(usb_hid_keycode(0xFF), 0);
}

// --- the keyboard differ ----------------------------------------------
//
// These assert on the differ's own state rather than on the key ring,
// because the ring is live and shared with PS/2. What each one really
// checks is that `prev` tracked the report -- and, for the rollover
// case, that it did NOT.

KTEST("usb-hid", "a key held across two reports is not re-pressed") {
    uint8_t prev[8];
    k_memset(prev, 0, sizeof prev);

    uint8_t r1[8] = { 0, 0, 0x04, 0, 0, 0, 0, 0 };   // 'a' down
    usb_hid_keyboard_diff(prev, r1, 8);
    KTEST_ASSERT_EQ(prev[2], 0x04);

    // The same report again is a HELD key, not a second press. A driver
    // emitting one press per report autorepeats at the polling rate,
    // which on a 125 Hz endpoint is 125 characters a second.
    usb_hid_keyboard_diff(prev, r1, 8);
    KTEST_ASSERT_EQ(prev[2], 0x04);

    uint8_t r2[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };      // released
    usb_hid_keyboard_diff(prev, r2, 8);
    KTEST_ASSERT_EQ(prev[2], 0);
}

// The one a real keyboard will not send on demand. 0x01 in all six
// slots is ErrorRollOver -- "more keys are down than I can report" --
// and treating it as data types six copies of usage 1 whenever someone
// rests a hand on the keyboard.
KTEST("usb-hid", "a rollover report is not six presses of usage 1") {
    uint8_t prev[8];
    k_memset(prev, 0, sizeof prev);

    uint8_t roll[8] = { 0, 0, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01 };
    usb_hid_keyboard_diff(prev, roll, 8);

    // Refused outright: prev must be untouched, so the next real report
    // still differs against the last real one rather than against the
    // rollover.
    for (int i = 0; i < 8; i++) KTEST_ASSERT_EQ(prev[i], 0);
}

KTEST("usb-hid", "every modifier bit is its own key, and state is tracked") {
    uint8_t prev[8];
    k_memset(prev, 0, sizeof prev);

    // All eight down at once, then all eight up. What is being checked
    // is that byte 0 is treated as eight independent bits rather than
    // as a value -- a differ comparing it as a number reports one
    // change for eight keys.
    uint8_t all[8] = { 0xFF, 0, 0, 0, 0, 0, 0, 0 };
    usb_hid_keyboard_diff(prev, all, 8);
    KTEST_ASSERT_EQ(prev[0], 0xFF);

    uint8_t none[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    usb_hid_keyboard_diff(prev, none, 8);
    KTEST_ASSERT_EQ(prev[0], 0);
}

KTEST("usb-hid", "six simultaneous keys all survive the differ") {
    uint8_t prev[8];
    k_memset(prev, 0, sizeof prev);
    uint8_t six[8] = { 0, 0, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09 };
    usb_hid_keyboard_diff(prev, six, 8);
    for (int i = 2; i < 8; i++) KTEST_ASSERT_EQ(prev[i], six[i]);
}

KTEST("usb-hid", "a short keyboard report is refused") {
    uint8_t prev[8];
    k_memset(prev, 0, sizeof prev);
    uint8_t stub[4] = { 0, 0, 0x04, 0 };
    usb_hid_keyboard_diff(prev, stub, 4);   // fewer than 8 bytes is not a report
    for (int i = 0; i < 8; i++) KTEST_ASSERT_EQ(prev[i], 0);
}

// --- the mouse differ -------------------------------------------------

KTEST("usb-hid", "mouse buttons are a mask, so one does not clear another") {
    uint8_t buttons = 0;
    uint8_t left[3]  = { 0x01, 0, 0 };
    usb_hid_mouse_diff(&buttons, left, 3);
    KTEST_ASSERT_EQ(buttons, 0x01);

    // Right pressed while left is still held must report BOTH.
    uint8_t both[3]  = { 0x03, 0, 0 };
    usb_hid_mouse_diff(&buttons, both, 3);
    KTEST_ASSERT_EQ(buttons, 0x03);

    uint8_t none[3]  = { 0x00, 0, 0 };
    usb_hid_mouse_diff(&buttons, none, 3);
    KTEST_ASSERT_EQ(buttons, 0x00);

    // Only the three real buttons; the upper bits of the byte are not
    // buttons and must not reach the pointer as one.
    uint8_t noisy[3] = { 0xF8, 0, 0 };
    usb_hid_mouse_diff(&buttons, noisy, 3);
    KTEST_ASSERT_EQ(buttons, 0x00);
}

// THE Y SIGN, and this test has already earned itself once: the driver
// was written without the negation and this is what caught it.
//
// input_report_rel() wants UP-POSITIVE dy, because mouse_feed_rel()
// ends in `mouse_y -= dy` -- it was written against a PS/2 mouse, whose
// Y increases upward. A HID boot mouse reports the other sense:
// positive dy is toward the user, DOWN the screen. So the driver must
// negate, and the name of this test is the wrong answer it rejects.
//
// This one drives the real pointer, so it saves and restores it.
KTEST("usb-hid", "a report saying down moves the pointer down the screen") {
    int save_x = 0, save_y = 0, w = 0, h = 0, x = 0, y = 0;
    uint8_t bstate = 0;
    mouse_get_state(&save_x, &save_y, &bstate);
    mouse_get_bounds(&w, &h);
    KTEST_ASSERT(w > 1 && h > 1);

    // Park in the middle so neither move can clamp at an edge, which
    // would make the comparison meaningless.
    mouse_feed_abs(w / 2, h / 2, w, h);
    mouse_get_state(&x, &y, &bstate);
    int mid_y = y;

    uint8_t buttons = 0;
    uint8_t down[3] = { 0, 0, 10 };        // dy = +10, "toward the user"
    usb_hid_mouse_diff(&buttons, down, 3);
    mouse_get_state(&x, &y, &bstate);
    KTEST_ASSERT(y > mid_y);               // ...must move DOWN the screen

    int after_down = y;
    uint8_t up[3] = { 0, 0, (uint8_t)(int8_t)-10 };
    usb_hid_mouse_diff(&buttons, up, 3);
    mouse_get_state(&x, &y, &bstate);
    KTEST_ASSERT(y < after_down);

    mouse_feed_abs(save_x, save_y, w, h);
}

KTEST("usb-hid", "mouse deltas are signed bytes") {
    int save_x = 0, save_y = 0, w = 0, h = 0, x = 0, y = 0;
    uint8_t bstate = 0;
    mouse_get_state(&save_x, &save_y, &bstate);
    mouse_get_bounds(&w, &h);
    KTEST_ASSERT(w > 1 && h > 1);
    mouse_feed_abs(w / 2, h / 2, w, h);
    mouse_get_state(&x, &y, &bstate);
    int mid_x = x;

    // 0xF6 is -10, not 246. Reading it unsigned sends the pointer a
    // quarter of the way across the screen in one report.
    uint8_t buttons = 0;
    uint8_t left[3] = { 0, 0xF6, 0 };
    usb_hid_mouse_diff(&buttons, left, 3);
    mouse_get_state(&x, &y, &bstate);
    KTEST_ASSERT(x < mid_x);

    mouse_feed_abs(save_x, save_y, w, h);
}

KTEST("usb-hid", "a 3-byte mouse report has no wheel, a 4-byte one does") {
    uint8_t buttons = 0;
    uint8_t three[3] = { 0, 0, 0 };
    usb_hid_mouse_diff(&buttons, three, 3);   // must not read byte 3
    KTEST_ASSERT_EQ(buttons, 0);

    uint8_t four[4] = { 0, 0, 0, 1 };
    usb_hid_mouse_diff(&buttons, four, 4);
    KTEST_ASSERT_EQ(buttons, 0);

    uint8_t stub[2] = { 0x01, 0 };
    usb_hid_mouse_diff(&buttons, stub, 2);    // too short to be a report
    KTEST_ASSERT_EQ(buttons, 0);
}
