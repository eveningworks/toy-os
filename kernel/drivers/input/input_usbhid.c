// USB HID, boot protocol only: a keyboard and a mouse, reported into
// the input core.
//
// BOOT PROTOCOL, NOT REPORT DESCRIPTORS. A HID device describes its own
// report format in a report descriptor, which is a small bytecode and a
// whole parser. The boot protocol is the fixed format every keyboard
// and mouse also speaks precisely so that a BIOS does not need that
// parser -- 8 bytes for a keyboard, 3 or 4 for a mouse. For "keyboard
// and mouse work" it is the entire job, and the parser is scope this
// driver deliberately does not have.
//
// THE DRIVER OWNS NOTHING ABOVE THE KEYCODE. It translates HID usages
// into evdev keycodes and calls input_report_key(); modifiers, the
// layout, Ctrl-folding, the key ring and the tty all belong to
// keyboard.c and are shared with PS/2. That is the payoff of the input
// core choosing evdev as its vocabulary -- there is no table here
// mapping to some internal encoding, only the standard one every USB
// keyboard already speaks.
#include "usb.h"
#include "usb_hid.h"
#include "xhci.h"
#include "xhci_regs.h"
#include "input.h"
#include "klog.h"
#include "hid_parse.h"   // the report descriptor, when a device has one worth reading
#include "kfmt.h"
#include "string.h"
#include "driver.h" // driver_bound() -- `lsdrv`

DRIVER_DECLARE("usb-hid", "input", "USB HID keyboards and mice");

// HID class requests, on the INTERFACE.
#define HID_REQ_SET_IDLE     0x0A
#define HID_REQ_SET_PROTOCOL 0x0B
#define HID_TYPE_CLASS_IF    0x21   // host->device, class, interface

#define HID_PROTO_BOOT       0
#define HID_PROTO_REPORT     1
#define HID_SUB_BOOT         1
#define HID_IF_KEYBOARD      1
#define HID_IF_MOUSE         2

// --- HID usage -> evdev keycode ---------------------------------------
//
// Usage page 0x07. The table is indexed by usage, so a gap is a 0 and
// an unmapped key is silently dropped rather than reported as some
// other key. Values are Linux's evdev numbering, which is what
// input_report_key() wants and what /usr/share/kbs is keyed on.
//
// Usage 0x64 is the one worth naming: the ISO key between Left Shift
// and Z, which carries `|` on every Nordic layout. input.h records that
// exact key going missing from the virtio-input table and making a
// shell pipeline untypeable on one input path and not the other, so it
// has its own KTEST below.
static const uint8_t HID_TO_EVDEV[0x100] = {
    [0x04] = 30, [0x05] = 48, [0x06] = 46, [0x07] = 32,   // a b c d
    [0x08] = 18, [0x09] = 33, [0x0A] = 34, [0x0B] = 35,   // e f g h
    [0x0C] = 23, [0x0D] = 36, [0x0E] = 37, [0x0F] = 38,   // i j k l
    [0x10] = 50, [0x11] = 49, [0x12] = 24, [0x13] = 25,   // m n o p
    [0x14] = 16, [0x15] = 19, [0x16] = 31, [0x17] = 20,   // q r s t
    [0x18] = 22, [0x19] = 47, [0x1A] = 17, [0x1B] = 45,   // u v w x
    [0x1C] = 21, [0x1D] = 44,                             // y z
    [0x1E] = 2,  [0x1F] = 3,  [0x20] = 4,  [0x21] = 5,    // 1..4
    [0x22] = 6,  [0x23] = 7,  [0x24] = 8,  [0x25] = 9,    // 5..8
    [0x26] = 10, [0x27] = 11,                             // 9 0
    [0x28] = INPUT_KEY_ENTER,
    [0x29] = INPUT_KEY_ESC,
    [0x2A] = 14,                       // backspace
    [0x2B] = 15,                       // tab
    [0x2C] = INPUT_KEY_SPACE,
    [0x2D] = 12, [0x2E] = 13,          // - =
    [0x2F] = 26, [0x30] = 27,          // [ ]
    [0x31] = 43, [0x32] = 43,          // backslash, non-US #
    [0x33] = 39, [0x34] = 40, [0x35] = 41,   // ; ' `
    [0x36] = 51, [0x37] = 52, [0x38] = 53,   // , . /
    [0x39] = INPUT_KEY_CAPSLOCK,
    [0x3A] = INPUT_KEY_F1, [0x3B] = INPUT_KEY_F2, [0x3C] = INPUT_KEY_F3,
    [0x3D] = INPUT_KEY_F4, [0x3E] = INPUT_KEY_F5, [0x3F] = INPUT_KEY_F6,
    [0x40] = INPUT_KEY_F7, [0x41] = INPUT_KEY_F8, [0x42] = INPUT_KEY_F9,
    [0x43] = INPUT_KEY_F10, [0x44] = INPUT_KEY_F11, [0x45] = INPUT_KEY_F12,
    [0x46] = INPUT_KEY_SYSRQ,
    [0x47] = INPUT_KEY_SCROLLLOCK,
    [0x48] = INPUT_KEY_PAUSE,
    [0x49] = INPUT_KEY_INSERT, [0x4A] = INPUT_KEY_HOME, [0x4B] = INPUT_KEY_PAGEUP,
    [0x4C] = INPUT_KEY_DELETE, [0x4D] = INPUT_KEY_END,  [0x4E] = INPUT_KEY_PAGEDOWN,
    [0x4F] = INPUT_KEY_RIGHT,  [0x50] = INPUT_KEY_LEFT,
    [0x51] = INPUT_KEY_DOWN,   [0x52] = INPUT_KEY_UP,
    [0x53] = INPUT_KEY_NUMLOCK,
    [0x54] = INPUT_KEY_KPSLASH, [0x55] = INPUT_KEY_KPASTERISK,
    [0x56] = INPUT_KEY_KPMINUS, [0x57] = INPUT_KEY_KPPLUS,
    [0x58] = INPUT_KEY_KPENTER,
    [0x59] = INPUT_KEY_KP1, [0x5A] = INPUT_KEY_KP2, [0x5B] = INPUT_KEY_KP3,
    [0x5C] = INPUT_KEY_KP4, [0x5D] = INPUT_KEY_KP5, [0x5E] = INPUT_KEY_KP6,
    [0x5F] = INPUT_KEY_KP7, [0x60] = INPUT_KEY_KP8, [0x61] = INPUT_KEY_KP9,
    [0x62] = INPUT_KEY_KP0, [0x63] = INPUT_KEY_KPDOT,
    [0x64] = INPUT_KEY_102ND,          // the ISO key -- see the comment above
    [0x65] = INPUT_KEY_COMPOSE,
    // Modifier usages, which also arrive as bits in byte 0.
    [0xE0] = INPUT_KEY_LEFTCTRL,  [0xE1] = INPUT_KEY_LEFTSHIFT,
    [0xE2] = INPUT_KEY_LEFTALT,   [0xE3] = INPUT_KEY_LEFTMETA,
    [0xE4] = INPUT_KEY_RIGHTCTRL, [0xE5] = INPUT_KEY_RIGHTSHIFT,
    [0xE6] = INPUT_KEY_RIGHTALT,  [0xE7] = INPUT_KEY_RIGHTMETA,
};

uint16_t usb_hid_keycode(uint8_t usage) { return HID_TO_EVDEV[usage]; }

// --- devices ----------------------------------------------------------

struct hid_dev {
    uint8_t in_use;
    uint8_t is_mouse;
    uint8_t slot;
    uint8_t ep;
    uint8_t prev[8];        // the last keyboard report, for the differ
    uint8_t buttons;        // the mouse's held mask
    // THE DEVICE'S OWN FORMAT, when its descriptor gave one up. Unset
    // means the BOOT format, which is what every device speaks and
    // what this driver asked for until a five-button mouse turned out
    // not to transmit its fifth button at all (api/hid_parse.h).
    uint8_t use_report;
    struct hid_layout layout;
    char    name[24];
    struct input_source src;
};

// Room for a composite receiver (keyboard + mouse on one plug) beside
// a wired pair, with slots REUSED on unbind -- hot-unplug would
// otherwise burn one forever per replug.
#define MAX_HID 6
static struct hid_dev g_hid[MAX_HID];
static uint32_t g_reports;

static struct hid_dev *hid_alloc(void) {
    for (int i = 0; i < MAX_HID; i++)
        if (!g_hid[i].in_use) return &g_hid[i];
    return 0;
}

// Counted because its absence is INVISIBLE on QEMU. QEMU's usb-hid
// reports boot format whether or not SET_PROTOCOL was ever issued, so a
// driver that skips it passes every behavioural check here and then
// fails on real hardware. The only way to test for it is to assert the
// request was SENT, which means counting it.
static uint32_t g_setproto_ok;

uint32_t usb_hid_reports(void) { return g_reports; }
uint32_t usb_hid_boot_protocol_count(void) { return g_setproto_ok; }

int usb_hid_describe(int index, char *buf, uint32_t cap) {
    if (index < 0 || !buf || !cap) return 0;
    for (int i = 0; i < MAX_HID; i++) {
        if (!g_hid[i].in_use) continue;
        if (index-- == 0) {
            k_snprintf(buf, cap, "%s slot %u ep 0x%x",
                       g_hid[i].name, g_hid[i].slot, g_hid[i].ep);
            return 1;
        }
    }
    return 0;
}

// --- the keyboard differ ----------------------------------------------
//
// A boot keyboard report is ABSOLUTE: {modifiers, reserved, six keycodes
// currently held}. There are no press or release events on the wire, so
// the driver derives them by comparing against the previous report.
//
// Two rules that are easy to get wrong. A key present in both reports
// is STILL HELD and must emit nothing -- emitting a press per report
// would autorepeat at the polling rate. And the rollover report, 0x01
// in all six slots, means "more keys are down than I can report" and is
// not six presses of usage 1; treating it as data types garbage
// whenever someone rests a hand on the keyboard.
void usb_hid_keyboard_diff(uint8_t prev[8], const uint8_t *r, uint32_t len) {
    if (len < 8) return;

    for (int i = 2; i < 8; i++)
        if (r[i] == 0x01) return;      // rollover: not data

    // Modifiers are a bitmask in byte 0, and they are reported as
    // TRANSITIONS rather than as state. Reporting absolute state would
    // fight the PS/2 keyboard, whose modifier tracking in keyboard.c is
    // global across every input source -- a USB report saying "nothing
    // held" would clear a Shift that PS/2 is legitimately holding.
    uint8_t changed = (uint8_t)(r[0] ^ prev[0]);
    for (int b = 0; b < 8; b++) {
        if (!(changed & (1u << b))) continue;
        input_report_key(HID_TO_EVDEV[0xE0 + b], (r[0] >> b) & 1);
    }

    // Releases: in the previous report, gone from this one.
    for (int i = 2; i < 8; i++) {
        uint8_t u = prev[i];
        if (!u) continue;
        int still = 0;
        for (int j = 2; j < 8; j++) if (r[j] == u) { still = 1; break; }
        if (!still) input_report_key(HID_TO_EVDEV[u], 0);
    }
    // Presses: in this report, absent from the previous one.
    for (int i = 2; i < 8; i++) {
        uint8_t u = r[i];
        if (!u) continue;
        int was = 0;
        for (int j = 2; j < 8; j++) if (prev[j] == u) { was = 1; break; }
        if (!was) input_report_key(HID_TO_EVDEV[u], 1);
    }
    k_memcpy(prev, r, 8);
}

// --- the mouse differ -------------------------------------------------
//
// {buttons, dx, dy} and optionally a wheel byte, all signed.
//
// THE Y AXIS IS NEGATED, and the reason is worth stating because the
// obvious reading of it is backwards. input_report_rel() wants
// UP-POSITIVE dy: mouse_feed_rel() ends in `mouse_y -= dy`, because it
// was written against a PS/2 mouse and PS/2 Y increases upward. A HID
// boot mouse reports dy the other way -- positive is toward the user,
// i.e. DOWN the screen, the same sense as evdev's REL_Y. So the sign
// has to be flipped here, exactly as virtio_input.c flips it.
//
// Writing this the "obvious" way inverts the mouse, and a KTEST catches
// it (input_usbhid_test.c, "mouse dy is not negated" -- named for the wrong
// answer it was written to reject, and it did reject this one).
void usb_hid_mouse_diff(uint8_t *buttons, const uint8_t *r, uint32_t len) {
    if (len < 3) return;

    // Buttons are a HELD MASK: edit the bit that changed and report the
    // whole mask, or pressing right would clear a held left.
    // FIVE BUTTONS OUT OF THE BOOT REPORT. The boot-protocol mouse
    // descriptor formally defines three, and every real 5-button mouse
    // puts the thumb buttons in bits 3 and 4 of that same byte and
    // reports them in boot mode anyway -- which is what makes this
    // work without the report-descriptor parser this driver does not
    // have. A device that does not have them simply never sets the bits.
    uint8_t mask = (uint8_t)(r[0] & 0x1F);
    if (mask != *buttons) {
        *buttons = mask;
        input_report_buttons(mask);
    }

    int dx = (int)(int8_t)r[1];
    int dy = (int)(int8_t)r[2];
    if (dx || dy) input_report_rel(dx, -dy);

    if (len >= 4) {
        int wheel = (int)(int8_t)r[3];
        if (wheel) input_report_wheel(wheel);
    }
}

// The same device, decoded through its OWN descriptor rather than the
// boot format. Everything here is a field lookup, because where the
// fields are is exactly what the boot format could not tell us.
static void hid_mouse_diff_report(struct hid_dev *d, const uint8_t *r, uint32_t len) {
    const struct hid_layout *L = &d->layout;
    const uint8_t *body = r;
    uint32_t blen = len;

    // A DESCRIPTOR WITH REPORT IDS PREFIXES EVERY REPORT WITH ONE, and
    // the device sends the OTHER collections' reports down the same
    // endpoint -- the G305's consumer and vendor collections share it.
    // A report that is not ours is not a mouse report; decoding it
    // anyway moves the pointer when somebody presses a media key.
    if (L->report_id) {
        if (len < 1 || r[0] != L->report_id) return;
        body = r + 1;
        blen = len - 1;
    }

    // FIVE BITS OUT OF HOWEVER MANY THE DEVICE HAS. The mask this
    // kernel carries names left, right, middle and the two thumb
    // buttons (kernel/input.h); a sixteen-button mouse's remaining
    // eleven have nowhere to go yet, and dropping them is honest.
    uint8_t mask = 0;
    for (uint8_t b = 0; b < L->buttons.count && b < 5; b++)
        if (hid_field_read(&L->buttons, body, blen, b))
            mask |= (uint8_t)(1u << b);
    if (mask != d->buttons) {
        d->buttons = mask;
        input_report_buttons(mask);
    }

    int dx = (int)hid_field_read(&L->x, body, blen, 0);
    int dy = (int)hid_field_read(&L->y, body, blen, 0);
    // Y NEGATED, the same as the boot path and for the same reason:
    // input_report_rel() wants up-positive, HID reports down-positive.
    if (dx || dy) input_report_rel(dx, -dy);

    if (L->wheel.present) {
        int w = (int)hid_field_read(&L->wheel, body, blen, 0);
        if (w) input_report_wheel(w);
    }
}

// A keyboard's report protocol, normalised back into the eight bytes
// the differ already speaks: modifiers, a reserved byte, six usages.
// Worth the copy rather than a second differ -- the rollover logic is
// the subtle part and there should be one of it.
static void hid_kbd_diff_report(struct hid_dev *d, const uint8_t *r, uint32_t len) {
    const struct hid_layout *L = &d->layout;
    const uint8_t *body = r;
    uint32_t blen = len;
    if (L->report_id) {
        if (len < 1 || r[0] != L->report_id) return;
        body = r + 1;
        blen = len - 1;
    }

    uint8_t boot[8];
    for (int i = 0; i < 8; i++) boot[i] = 0;
    for (uint8_t b = 0; b < L->mods.count && b < 8; b++)
        if (hid_field_read(&L->mods, body, blen, b))
            boot[0] |= (uint8_t)(1u << b);
    for (uint8_t k = 0; k < L->keys.count && k < 6; k++)
        boot[2 + k] = (uint8_t)hid_field_read(&L->keys, body, blen, k);

    usb_hid_keyboard_diff(d->prev, boot, sizeof boot);
}

// --- polling ----------------------------------------------------------

static void hid_service_one(struct hid_dev *d) {
    uint8_t report[64];
    int n;
    // Drain everything queued, not just one: the endpoint keeps a depth
    // of TRBs posted, so several reports can be waiting after a busy
    // moment, and taking one per poll would lag behind the typist.
    while ((n = xhci_take_report(d->slot, d->ep, report, sizeof report)) > 0) {
        g_reports++;
        if (d->use_report) {
            if (d->is_mouse) hid_mouse_diff_report(d, report, (uint32_t)n);
            else             hid_kbd_diff_report(d, report, (uint32_t)n);
        } else if (d->is_mouse) {
            usb_hid_mouse_diff(&d->buttons, report, (uint32_t)n);
        } else {
            usb_hid_keyboard_diff(d->prev, report, (uint32_t)n);
        }
    }
}

// ONE consumer of the report queues at a time. The controller's poll
// runs beside its IRQ now (see xhci_poll_source), so the interrupt can
// land in the middle of a polled decode; without this both would
// advance next_take and re-post one slice twice. The turned-away
// caller loses nothing -- the reports stay queued for the next pass.
// Same shape as xhci_service()'s guard, for the same reason.
static volatile uint8_t g_in_hid;

// One thunk per slot, because struct input_source carries no context
// pointer -- the same reason virtio_input.c has poll_0..poll_3.
static void poll_slot(int i) {
    if (g_in_hid) return;
    g_in_hid = 1;
    if (g_hid[i].in_use) hid_service_one(&g_hid[i]);
    g_in_hid = 0;
}
static void poll_0(void) { poll_slot(0); }
static void poll_1(void) { poll_slot(1); }
static void poll_2(void) { poll_slot(2); }
static void poll_3(void) { poll_slot(3); }
static void poll_4(void) { poll_slot(4); }
static void poll_5(void) { poll_slot(5); }
static void (*const POLLS[MAX_HID])(void) = {
    poll_0, poll_1, poll_2, poll_3, poll_4, poll_5,
};

// The interrupt path: the controller's IRQ has already moved the report
// into memory, so this only has to decode what is waiting.
void usb_hid_service_all(void) {
    if (g_in_hid) return;
    g_in_hid = 1;
    for (int i = 0; i < MAX_HID; i++)
        if (g_hid[i].in_use) hid_service_one(&g_hid[i]);
    g_in_hid = 0;
}

// --- binding ----------------------------------------------------------

// SET_PROTOCOL(report), but only once the descriptor has been read and
// understood. The order matters: a device switched to its own format
// before anyone knows what that format is reports bytes nobody can
// decode, and a mouse whose axes are read from the wrong bits is worse
// than a mouse missing a button.
//
// Returns 1 when the device is now in report protocol and `d->layout`
// describes it.
static int hid_try_report_protocol(struct hid_dev *d, uint8_t slot, uint8_t ifnum) {
    // One buffer, reused: this runs at bind time, one device at a time,
    // and a per-device copy of a descriptor nobody keeps would be 256
    // bytes each for nothing. Static rather than on the stack because
    // it is a DMA target -- usb_enum.c's rule, and the overrun that
    // broke it there cost a panic (docs/bugs.md).
    static uint8_t desc[256];
    uint8_t get[8] = { 0x81, 0x06, 0x00, 0x22, ifnum, 0,
                       (uint8_t)sizeof desc, (uint8_t)(sizeof desc >> 8) };
    int n = xhci_control(slot, get, desc, (uint16_t)sizeof desc, 1);
    if (n <= 0) return 0;
    if (n > (int)sizeof desc) n = (int)sizeof desc;

    if (!hid_parse_report_descriptor(desc, (uint32_t)n, d->is_mouse, &d->layout))
        return 0;

    uint8_t setup[8];
    setup[0] = HID_TYPE_CLASS_IF; setup[1] = HID_REQ_SET_PROTOCOL;
    setup[2] = HID_PROTO_REPORT;  setup[3] = 0;
    setup[4] = ifnum;             setup[5] = 0;
    setup[6] = 0;                 setup[7] = 0;
    if (xhci_control(slot, setup, 0, 0, 0) < 0) return 0;

    // SET_IDLE(0) as in the boot path: report on change, not on a timer.
    setup[1] = HID_REQ_SET_IDLE;
    setup[2] = 0; setup[3] = 0;
    (void)xhci_control(slot, setup, 0, 0, 0);

    if (d->is_mouse)
        klog_printf("usb: slot %u if %u: report protocol -- %u button(s), "
                    "%u-bit axes%s%s\n", slot, ifnum,
                    d->layout.buttons.count, d->layout.x.bits,
                    d->layout.wheel.present ? ", wheel" : "",
                    d->layout.pan.present ? ", h-scroll" : "");
    else
        klog_printf("usb: slot %u if %u: report protocol -- %u modifier(s), "
                    "%u key slot(s)\n", slot, ifnum,
                    d->layout.mods.count, d->layout.keys.count);
    return 1;
}

static int hid_set_idle_and_boot(uint8_t slot, uint8_t ifnum) {
    uint8_t setup[8];

    // SET_PROTOCOL(boot). QEMU's usb-hid already reports boot-format
    // regardless, so omitting this is INVISIBLE here and breaks on real
    // hardware -- which is why the test asserts the request was issued
    // rather than asserting on its effect.
    setup[0] = HID_TYPE_CLASS_IF; setup[1] = HID_REQ_SET_PROTOCOL;
    setup[2] = HID_PROTO_BOOT;    setup[3] = 0;
    setup[4] = ifnum;             setup[5] = 0;
    setup[6] = 0;                 setup[7] = 0;
    if (xhci_control(slot, setup, 0, 0, 0) < 0) return -1;
    g_setproto_ok++;

    // SET_IDLE(0) -- report only on change, never on a timer. Without
    // it a keyboard re-sends its state every few milliseconds, which
    // the differ handles correctly but which is pure noise.
    setup[1] = HID_REQ_SET_IDLE;
    setup[2] = 0; setup[3] = 0;
    (void)xhci_control(slot, setup, 0, 0, 0);   // optional; a stall is fine
    return 0;
}

// Binds EVERY boot keyboard/mouse interface an enumerated device
// carries -- a composite wireless receiver is a keyboard interface
// followed by a mouse interface on one plug, and binding only the
// first is a receiver whose mouse half is silently dead. Returns how
// many it took.
int usb_hid_bind(struct usb_device_info *info) {
    if (!info) return 0;
    int took = 0;
    for (int i = 0; i < info->if_count; i++) {
        const struct usb_interface_info *ifc = &info->ifs[i];

        // SAY WHY AN INTERFACE WAS PASSED OVER. This filter used to drop
        // one in silence, so a mouse this driver cannot drive and a
        // mouse that failed to enumerate produced the same dmesg --
        // nothing. A HID interface is worth a line either way; a
        // non-HID one is not, since every composite device has several.
        if (ifc->if_class != 3) continue;
        if (ifc->if_subclass != HID_SUB_BOOT ||
            (ifc->if_protocol != HID_IF_KEYBOARD &&
             ifc->if_protocol != HID_IF_MOUSE)) {
            klog_printf("usb: slot %u if %u: HID 3/%u/%u -- not a boot "
                        "keyboard or mouse, skipped\n",
                        info->slot, ifc->ifnum, ifc->if_subclass, ifc->if_protocol);
            continue;
        }
        if (!ifc->ep) {
            klog_printf("usb: slot %u if %u: boot HID with no interrupt "
                        "endpoint, skipped\n", info->slot, ifc->ifnum);
            continue;
        }

        struct hid_dev *d = hid_alloc();
        if (!d) break;
        k_memset(d, 0, sizeof *d);
        d->slot     = info->slot;
        d->ep       = ifc->ep;
        d->is_mouse = (ifc->if_protocol == HID_IF_MOUSE);

        // THE DESCRIPTOR FIRST, THE BOOT PROTOCOL AS THE FALLBACK.
        // Asking a device for its own format is the only way to reach
        // what the boot format has no room for -- a fourth and fifth
        // button, 16-bit axes, horizontal scroll. A descriptor this
        // does not understand simply leaves use_report clear and the
        // device is driven exactly as it was before.
        d->use_report = hid_try_report_protocol(d, info->slot, ifc->ifnum);
        if (!d->use_report && hid_set_idle_and_boot(info->slot, ifc->ifnum) < 0) {
            klog_printf(KLOG_ERR "usb: slot %u if %u: set protocol(boot) failed\n",
                        info->slot, ifc->ifnum);
            continue;
        }
        if (xhci_add_interrupt_in(info->slot, ifc->ep,
                                  ifc->mps, ifc->interval) < 0) {
            klog_printf("usb: slot %u if %u: no interrupt endpoint for "
                        "ep 0x%x (mps %u, interval %u)\n",
                        info->slot, ifc->ifnum, ifc->ep, ifc->mps, ifc->interval);
            continue;
        }

        k_snprintf(d->name, sizeof d->name, "usb-%s",
                   d->is_mouse ? "mouse" : "keyboard");
        d->src.name = d->name;
        d->src.driver = "usb-hid";
        d->src.caps = d->is_mouse ? (INPUT_CAP_REL | INPUT_CAP_WHEEL)
                                  : INPUT_CAP_KEYS;
        // An interrupt-driven source leaves poll NULL -- its decode
        // rides the controller's own always-on poll; a polled one gets
        // its thunk, indexed by the slot it landed in.
        d->src.irq  = usb_controller_irq();
        d->src.msi_vector = usb_controller_msi_vector();
        // Interrupt-driven if the controller is, WHICHEVER way it is
        // signalled -- reading `irq` alone made every HID device on an
        // MSI controller install a poll thunk and report itself polled.
        d->src.poll = (d->src.irq || d->src.msi_vector) ? 0 : POLLS[d - g_hid];

        d->in_use = 1;          // published before the source can be polled
        input_register_source(&d->src);

        if (!info->hid_ep) info->hid_ep = ifc->ep;
        usb_mark_bound(info, "usb-hid");
        took++;
        klog_printf("usb: slot %u: bound as %s on endpoint 0x%x\n",
                    info->slot, d->name, ifc->ep);
    }
    return took;
}

// Unbinds every interface bound on `slot` -- the detach path. The slot
// is cleared BEFORE the source unregisters, so an interrupt landing in
// between decodes nothing rather than touching a dying endpoint.
void usb_hid_unbind(uint8_t slot) {
    for (int i = 0; i < MAX_HID; i++) {
        struct hid_dev *d = &g_hid[i];
        if (!d->in_use || d->slot != slot) continue;
        d->in_use = 0;
        input_unregister_source(&d->src);
    }
}
