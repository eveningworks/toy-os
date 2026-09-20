#ifndef HID_PARSE_H
#define HID_PARSE_H

#include <stdint.h>

// The HID REPORT DESCRIPTOR, read well enough to decode a mouse or a
// keyboard -- and no further.
//
// A HID device describes its own report format in a small bytecode.
// input_usbhid.c used to avoid it entirely by asking every device for
// the BOOT protocol, the fixed format a BIOS can drive without a
// parser. That works until a device has something the boot format has
// no room for: a boot mouse report carries THREE buttons, so a
// five-button mouse's forward button is not merely mis-decoded, it is
// never transmitted (docs/decisions.md, "The HID descriptor is parsed
// now"). Measured on a Logitech G305: `back` arrives as a courtesy bit
// and `forward` does not exist on the wire at all.
//
// WHAT THIS IS NOT. It is not a general HID parser. It does not do
// Feature reports, Output reports, Push/Pop, delimiters, or usages it
// has no use for. It walks the item stream, tracks the state table a
// report's geometry needs, and fills in WHERE the fields it cares about
// sit. Anything else advances the bit offset and is dropped -- which is
// the one thing it must get right, because a field it ignores still
// occupies space and everything after it would otherwise be read from
// the wrong bits.
//
// IT PARSES UNTRUSTED INPUT. The bytes come off a device, and a device
// can say anything; every read is bounds-checked and every count is
// clamped, the rule api/ttf.h carries for the same reason. It allocates
// NOTHING -- the caller supplies the layout -- which is what lets one
// implementation serve ring 0, a KTEST and a host check.

// Where one field sits inside a report, and how to read it.
struct hid_field {
    uint16_t bit_off;   // from the start of the report BODY (after any ID)
    uint8_t  bits;      // width of ONE element
    uint8_t  count;     // how many elements
    uint8_t  is_signed; // its logical minimum is negative
    uint8_t  present;
};

// A mouse report has axes and a button bitmap; a keyboard report has a
// modifier bitmap and an ARRAY of keycodes. The two are different
// shapes rather than different fields -- a keyboard's `keys` holds the
// usages that are down, not one bit per key -- which is why they are
// named apart rather than shared.
struct hid_layout {
    uint8_t  report_id;    // 0 when the descriptor uses no report IDs
    uint8_t  is_mouse;
    uint8_t  is_keyboard;
    uint16_t report_bits;  // the body's size, ID byte excluded

    struct hid_field buttons;  // mouse: 1 bit each
    struct hid_field x, y;     // mouse: 8 or 16 bits, signed, relative
    struct hid_field wheel;
    struct hid_field pan;      // horizontal scroll (Consumer AC Pan)

    struct hid_field mods;     // keyboard: 8 modifier bits
    struct hid_field keys;     // keyboard: `count` usages, 8 bits each
};

// Fills `out` from the descriptor, for the first top-level collection
// whose usage is Mouse (want_mouse) or Keyboard (!want_mouse).
//
// Returns 1 when a layout was derived that is worth using, and 0 when
// it was not -- a descriptor this does not understand, or one whose
// mouse collection has no axes. **A CALLER MUST FALL BACK TO THE BOOT
// PROTOCOL ON 0**, never guess: decoding a report against a layout that
// does not fit it reads the pointer's movement out of the button bits,
// which is a mouse that flies across the screen rather than one with a
// missing button.
int hid_parse_report_descriptor(const uint8_t *desc, uint32_t len,
                                int want_mouse, struct hid_layout *out);

// Reads one field out of a report body, sign-extended when the field
// says so. `index` selects the element for a multi-element field.
// Returns 0 for a field that is absent or that does not fit the report.
int32_t hid_field_read(const struct hid_field *f, const uint8_t *body,
                       uint32_t body_len, uint8_t index);

#endif // HID_PARSE_H
