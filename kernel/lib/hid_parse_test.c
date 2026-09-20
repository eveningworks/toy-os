// KTESTs for the HID report-descriptor walker (api/hid_parse.h).
//
// The fixtures are CAPTURED, not invented: a Logitech G305 receiver's
// mouse interface and QEMU's usb-mouse, read off the wire with
// GET_DESCRIPTOR(0x22). A descriptor written by the same person who
// wrote the parser proves only that the two agree.
//
// tools/hid_parse_hostcheck.py checks the same parser against the same
// bytes plus both keyboards and a round trip, on the host. This exists
// so the GATE covers it on every build -- the hostcheck is not in
// preflight, by the rule that the gate must not require a tool a
// checkout may not have.
#include "hid_parse.h"
#include "ktest.h"

// Report ID 2, sixteen buttons, SIGNED 16-BIT axes, wheel and AC Pan.
// None of that fits the boot protocol, which is the whole reason this
// parser exists: this mouse's forward button is not mis-decoded
// without it, it is never transmitted at all.
static const uint8_t G305_MOUSE[] = {
    0x05,0x01,0x09,0x02,0xa1,0x01,0x85,0x02,0x09,0x01,0xa1,0x00,0x95,0x10,0x75,0x01,
    0x15,0x00,0x25,0x01,0x05,0x09,0x19,0x01,0x29,0x10,0x81,0x02,0x95,0x02,0x75,0x10,
    0x16,0x01,0x80,0x26,0xff,0x7f,0x05,0x01,0x09,0x30,0x09,0x31,0x81,0x06,0x95,0x01,
    0x75,0x08,0x15,0x81,0x25,0x7f,0x09,0x38,0x81,0x06,0x95,0x01,0x05,0x0c,0x0a,0x38,
    0x02,0x81,0x06,0xc0,0xc0,0x05,0x0c,0x09,0x01,0xa1,0x01,0x85,0x03,0x95,0x02,0x75,
    0x10,0x15,0x01,0x26,0xff,0x02,0x19,0x01,0x2a,0xff,0x02,0x81,0x00,0xc0,0x05,0x01,
    0x09,0x80,0xa1,0x01,0x85,0x04,0x95,0x01,0x75,0x02,0x15,0x01,0x25,0x03,0x09,0x82,
    0x09,0x81,0x09,0x83,0x81,0x00,0x75,0x06,0x81,0x03,0xc0,0x06,0xbc,0xff,0x09,0x88,
    0xa1,0x01,0x85,0x08,0x95,0x01,0x75,0x08,0x15,0x01,0x26,0xff,0x00,0x19,0x01,0x29,
    0xff,0x81,0x00,0xc0,
};

// QEMU's usb-mouse: five buttons, three padding bits, three 8-bit
// signed axes, no report ID.
static const uint8_t QEMU_MOUSE[] = {
    0x05,0x01,0x09,0x02,0xa1,0x01,0x09,0x01,0xa1,0x00,0x05,0x09,0x19,0x01,0x29,0x05,
    0x15,0x00,0x25,0x01,0x95,0x05,0x75,0x01,0x81,0x02,0x95,0x01,0x75,0x03,0x81,0x01,
    0x05,0x01,0x09,0x30,0x09,0x31,0x09,0x38,0x15,0x81,0x25,0x7f,0x75,0x08,0x95,0x03,
    0x81,0x06,0xc0,0xc0,
};

KTEST("hid", "a report descriptor gives up where its fields actually are") {
    struct hid_layout L;

    KTEST_ASSERT(hid_parse_report_descriptor(QEMU_MOUSE, sizeof QEMU_MOUSE, 1, &L));
    KTEST_ASSERT_EQ(L.report_id, 0);
    KTEST_ASSERT_EQ(L.buttons.count, 5);
    KTEST_ASSERT_EQ(L.buttons.bit_off, 0);
    // THE THREE PADDING BITS STILL OCCUPY SPACE. A parser that skips a
    // Constant field without advancing puts X at bit 5 and reads the
    // pointer's movement out of the button byte.
    KTEST_ASSERT_EQ(L.x.bit_off, 8);
    KTEST_ASSERT_EQ(L.y.bit_off, 16);
    KTEST_ASSERT_EQ(L.x.bits, 8);
    KTEST_ASSERT(L.x.is_signed);

    KTEST_ASSERT(hid_parse_report_descriptor(G305_MOUSE, sizeof G305_MOUSE, 1, &L));
    KTEST_ASSERT_EQ(L.report_id, 2);
    KTEST_ASSERT_EQ(L.buttons.count, 16);   // the boot format carries three
    KTEST_ASSERT_EQ(L.x.bits, 16);
    KTEST_ASSERT_EQ(L.x.bit_off, 16);
    KTEST_ASSERT_EQ(L.y.bit_off, 32);
    KTEST_ASSERT(L.pan.present);            // horizontal scroll, invisible to boot
}

KTEST("hid", "a field reads back what a report put in it, sign and all") {
    struct hid_layout L;
    KTEST_ASSERT(hid_parse_report_descriptor(G305_MOUSE, sizeof G305_MOUSE, 1, &L));

    // Body (no ID byte): buttons 0x0010 = button 5 "forward", X = -7,
    // Y = +300. The negative is the point: read unsigned, -7 is 65529
    // and the pointer leaps across the screen.
    uint8_t body[8] = { 0x10, 0x00, 0xf9, 0xff, 0x2c, 0x01, 0x00, 0x00 };
    KTEST_ASSERT_EQ(hid_field_read(&L.x, body, sizeof body, 0), -7);
    KTEST_ASSERT_EQ(hid_field_read(&L.y, body, sizeof body, 0), 300);
    KTEST_ASSERT_EQ(hid_field_read(&L.buttons, body, sizeof body, 4), 1);
    KTEST_ASSERT_EQ(hid_field_read(&L.buttons, body, sizeof body, 3), 0);

    // A report SHORTER than the layout claims must read 0, not walk
    // off the end -- a device may send anything.
    KTEST_ASSERT_EQ(hid_field_read(&L.y, body, 2, 0), 0);
}

KTEST("hid", "a descriptor it cannot use is refused, not guessed at") {
    struct hid_layout L;

    // **THE FALLBACK IS THE POINT.** Returning 0 sends input_usbhid.c
    // back to the boot protocol, where a mouse works with three
    // buttons; returning a layout that does not fit gives a pointer
    // that flies across the screen, which is worse than the bug this
    // parser was written to fix.
    KTEST_ASSERT(!hid_parse_report_descriptor(0, 0, 1, &L));
    KTEST_ASSERT(!hid_parse_report_descriptor(QEMU_MOUSE, 0, 1, &L));
    KTEST_ASSERT(!hid_parse_report_descriptor(QEMU_MOUSE, 6, 1, &L));
    // A mouse descriptor is not a keyboard, however much of it parses.
    KTEST_ASSERT(!hid_parse_report_descriptor(QEMU_MOUSE, sizeof QEMU_MOUSE, 0, &L));

    // Every truncation, not just a convenient one: a prefix that ends
    // mid-item must stop rather than read the byte after it.
    for (uint32_t n = 1; n < sizeof QEMU_MOUSE; n++) {
        struct hid_layout t;
        if (hid_parse_report_descriptor(QEMU_MOUSE, n, 1, &t))
            KTEST_ASSERT(t.x.present && t.y.present);
    }
}
