#ifndef USB_HID_H
#define USB_HID_H

#include <stdint.h>
#include "usb.h"

// USB HID, boot protocol. See kernel/drivers/usb/usb_hid.c for why the
// boot protocol and not report descriptors.
//
// THE TWO DIFFERS TAKE THEIR STATE AS A PARAMETER, on purpose. A boot
// report is ABSOLUTE -- it says what is held right now, not what
// changed -- so turning it into events needs the previous report.
// Passing that in rather than hiding it in a static is what lets the
// KTESTs drive these directly, with no controller and no device: they
// are the half of this driver where the bugs are, and the half that
// hardware cannot help test.

// Turns one 8-byte boot keyboard report into key events, against
// `prev` (8 bytes, caller-owned, zeroed before the first report) which
// it updates. Emits nothing for a key held across both reports, and
// nothing at all for a rollover report.
void usb_hid_keyboard_diff(uint8_t prev[8], const uint8_t *report, uint32_t len);

// Turns one 3- or 4-byte boot mouse report into pointer events, against
// the held button mask `buttons`, which it updates.
void usb_hid_mouse_diff(uint8_t *buttons, const uint8_t *report, uint32_t len);

// The evdev keycode for a HID usage, or 0 when this build maps none.
uint16_t usb_hid_keycode(uint8_t usage);

// Binds EVERY boot keyboard/mouse interface the enumerated device
// carries (a composite receiver is two on one plug) and registers each
// with the input core. Marks `info` bound. Returns how many it took.
int usb_hid_bind(struct usb_device_info *info);

// Unbinds everything bound on `slot` and unregisters its input
// sources. The detach path; safe against an interrupt mid-way.
void usb_hid_unbind(uint8_t slot);

// Decodes whatever reports have arrived. Called from the controller's
// interrupt handler, and from the input core's poll when there is no
// usable IRQ line.
void usb_hid_service_all(void);

// How many reports have been decoded. A test uses this to tell "the
// driver never ran" from "the driver ran and decoded nothing", which no
// assertion on behaviour alone can distinguish.
uint32_t usb_hid_reports(void);

// How many SET_PROTOCOL(boot) requests were accepted. QEMU reports boot
// format regardless, so skipping the request is invisible in behaviour
// here and breaks on real hardware -- a test therefore has to assert
// the request happened, not its effect.
uint32_t usb_hid_boot_protocol_count(void);

// One line describing bound HID device `index`, for the `usb` dump.
int usb_hid_describe(int index, char *buf, uint32_t cap);

#endif
