// KTESTs for the configuration-descriptor walk.
//
// The fixture that matters is the COMPOSITE one: a wireless receiver is
// a keyboard interface followed by a mouse interface on one device, and
// the walk used to stop at the first HID interface -- which bound the
// keyboard and left the mouse silently dead. QEMU has no stock
// composite HID device, so the only way to cover that shape headlessly
// is to feed the walk the descriptor bytes directly, the same call
// input_usbhid_test.c makes for the differs.
#include "usb.h"
#include "ktest.h"
#include "string.h"
#include "etc_config.h"

// The shape of a Logitech Unifying receiver's configuration: three
// interfaces (boot keyboard, boot mouse, vendor HID), each with a HID
// descriptor between the interface and its interrupt-IN endpoint --
// which is exactly the descriptor the walk must step over without
// losing its place.
static const uint8_t UNIFYING_CFG[] = {
    9, 2, 84, 0, 3, 1, 4, 0xA0, 49,          // config: wTotalLength 84, 3 ifs
    9, 4, 0, 0, 1, 3, 1, 1, 0,               // if 0: HID boot keyboard
    9, 33, 0x11, 1, 0, 1, 34, 59, 0,         // HID descriptor
    7, 5, 0x81, 3, 8, 0, 8,                  // ep 0x81 interrupt IN, mps 8
    9, 4, 1, 0, 1, 3, 1, 2, 0,               // if 1: HID boot mouse
    9, 33, 0x11, 1, 0, 1, 34, 148, 0,        // HID descriptor
    7, 5, 0x82, 3, 8, 0, 2,                  // ep 0x82 interrupt IN, mps 8
    9, 4, 2, 0, 1, 3, 0, 0, 0,               // if 2: vendor HID, no boot
    9, 33, 0x11, 1, 0, 1, 34, 98, 0,         // HID descriptor
    7, 5, 0x83, 3, 32, 0, 2,                 // ep 0x83 interrupt IN, mps 32
};

KTEST("usb-enum", "a composite receiver yields every interface, in order") {
    struct usb_interface_info ifs[USB_MAX_INTERFACES];
    int n = usb_parse_config_interfaces(UNIFYING_CFG, sizeof UNIFYING_CFG,
                                        ifs, USB_MAX_INTERFACES);
    KTEST_ASSERT_EQ(n, 3);
    KTEST_ASSERT_EQ(ifs[0].if_protocol, 1);        // keyboard
    KTEST_ASSERT_EQ(ifs[0].ep, 0x81);
    KTEST_ASSERT_EQ(ifs[1].if_protocol, 2);        // the mouse, NOT dropped
    KTEST_ASSERT_EQ(ifs[1].ep, 0x82);
    KTEST_ASSERT_EQ(ifs[1].if_subclass, 1);        // boot-capable
    KTEST_ASSERT_EQ(ifs[1].mps, 8);
    KTEST_ASSERT_EQ(ifs[2].if_subclass, 0);        // vendor half: visible,
    KTEST_ASSERT_EQ(ifs[2].ep, 0x83);              // and not a boot device
}

KTEST("usb-enum", "a hub configuration yields its status-change endpoint") {
    static const uint8_t hub_cfg[] = {
        9, 2, 25, 0, 1, 1, 0, 0xE0, 0,
        9, 4, 0, 0, 1, 9, 0, 0, 0,             // if 0: class 9, hub
        7, 5, 0x81, 3, 2, 0, 255,              // status-change pipe
    };
    struct usb_interface_info ifs[USB_MAX_INTERFACES];
    int n = usb_parse_config_interfaces(hub_cfg, sizeof hub_cfg,
                                        ifs, USB_MAX_INTERFACES);
    KTEST_ASSERT_EQ(n, 1);
    KTEST_ASSERT_EQ(ifs[0].if_class, 9);
    KTEST_ASSERT_EQ(ifs[0].ep, 0x81);
    KTEST_ASSERT_EQ(ifs[0].mps, 2);
}

KTEST("usb-enum", "an alternate setting does not open a second interface") {
    static const uint8_t alt_cfg[] = {
        9, 2, 41, 0, 1, 1, 0, 0xA0, 49,
        9, 4, 0, 0, 1, 3, 1, 2, 0,             // if 0 alt 0: boot mouse
        7, 5, 0x81, 3, 8, 0, 8,
        9, 4, 0, 1, 1, 3, 1, 2, 0,             // if 0 alt 1: same interface
        7, 5, 0x82, 3, 16, 0, 4,               // ...whose ep must NOT win
    };
    struct usb_interface_info ifs[USB_MAX_INTERFACES];
    int n = usb_parse_config_interfaces(alt_cfg, sizeof alt_cfg,
                                        ifs, USB_MAX_INTERFACES);
    KTEST_ASSERT_EQ(n, 1);
    KTEST_ASSERT_EQ(ifs[0].ep, 0x81);          // alt 0's endpoint, kept
}

KTEST("usb-enum", "a malformed descriptor is refused, not guessed at") {
    struct usb_interface_info ifs[USB_MAX_INTERFACES];

    uint8_t zero_len[16];
    k_memcpy(zero_len, UNIFYING_CFG, sizeof zero_len);
    zero_len[9] = 0;                           // bLength 0: infinite loop bait
    KTEST_ASSERT_EQ(usb_parse_config_interfaces(zero_len, sizeof zero_len,
                                                ifs, USB_MAX_INTERFACES), -1);

    uint8_t overrun[16];
    k_memcpy(overrun, UNIFYING_CFG, sizeof overrun);
    overrun[9] = 200;                          // bLength past the buffer
    KTEST_ASSERT_EQ(usb_parse_config_interfaces(overrun, sizeof overrun,
                                                ifs, USB_MAX_INTERFACES), -1);
}

KTEST("usb-enum", "interfaces past the cap are counted and dropped whole") {
    // One MORE interface than the cap, whatever the cap is: the count
    // stops there, and no endpoint from the dropped last one bleeds
    // into the previous record. Derived rather than written out,
    // because a literal drifts the day the cap moves -- which it did.
    const uint8_t count = USB_MAX_INTERFACES + 1;
    uint8_t many[9 + (USB_MAX_INTERFACES + 1) * 16];
    uint32_t o = 0;
    const uint8_t cfg9[] = { 9, 2, 0, 0, count, 1, 0, 0xA0, 49 };
    k_memcpy(many + o, cfg9, 9); o += 9;
    for (uint8_t i = 0; i < count; i++) {
        const uint8_t ifd[] = { 9, 4, i, 0, 1, 3, 1, 2, 0 };
        k_memcpy(many + o, ifd, 9); o += 9;
        const uint8_t epd[] = { 7, 5, (uint8_t)(0x81 + i), 3, 8, 0, 8 };
        k_memcpy(many + o, epd, 7); o += 7;
    }
    struct usb_interface_info ifs[USB_MAX_INTERFACES];
    int n = usb_parse_config_interfaces(many, o, ifs, USB_MAX_INTERFACES);
    KTEST_ASSERT_EQ(n, USB_MAX_INTERFACES);
    KTEST_ASSERT_EQ(ifs[USB_MAX_INTERFACES - 1].ep,
                    0x81 + USB_MAX_INTERFACES - 1);
}

// --- one socket, two port numbers -------------------------------------
//
// The pairing is a HEURISTIC (xhci.c's companion_port): the controller
// declares two port RANGES and never says which port of one is the same
// socket as which port of the other. These pin the arithmetic, not the
// premise -- what would falsify the premise is the map logged at boot
// disagreeing with the physical machine.

// The ASUS UX305FA's own numbers, from its boot log: USB 2.0 ports
// 1..11, USB 3.0 ports 12..15. Stated as a fixture so the case that
// produced this code is the case that is checked.
#define ASUS 1, 11, 12, 4

KTEST("usb-sockets", "the observed pair is the one this derives") {
    // A UE300 was seen at port 3 (full-speed) and, after a replug, at
    // port 14 (SuperSpeed). Index 3 of each range.
    KTEST_ASSERT_EQ(xhci_companion_in(ASUS, 3), 14);
    KTEST_ASSERT_EQ(xhci_companion_in(ASUS, 14), 3);
}

KTEST("usb-sockets", "the pairing is symmetric across every socket") {
    for (unsigned ss = 12; ss <= 15; ss++) {
        unsigned hs = (unsigned)xhci_companion_in(ASUS, ss);
        KTEST_ASSERT(hs != 0);
        KTEST_ASSERT_EQ((int)xhci_companion_in(ASUS, hs), (int)ss);
    }
}

KTEST("usb-sockets", "a USB2 port past the shorter range has no companion") {
    // Eleven USB2 ports against four USB3 ones: ports 5..11 are
    // USB2-only, which is the ordinary case (webcams, Bluetooth).
    for (unsigned p = 5; p <= 11; p++)
        KTEST_ASSERT_EQ(xhci_companion_in(ASUS, p), 0);
}

KTEST("usb-sockets", "a port in neither range, and a controller with no USB3") {
    KTEST_ASSERT_EQ(xhci_companion_in(ASUS, 16), 0);
    KTEST_ASSERT_EQ(xhci_companion_in(ASUS, 0), 0);
    // QEMU declares no USB3 range on the machines this is tested on, so
    // the whole feature must be inert there rather than guessing.
    KTEST_ASSERT_EQ(xhci_companion_in(1, 11, 0, 0, 3), 0);
}

KTEST("usb-sockets", "ranges in the other order still pair") {
    // Nothing says USB2 comes first. A controller declaring USB3 at
    // 1..4 and USB2 at 5..15 must pair 1 with 5, not with itself.
    KTEST_ASSERT_EQ(xhci_companion_in(5, 11, 1, 4, 1), 5);
    KTEST_ASSERT_EQ(xhci_companion_in(5, 11, 1, 4, 5), 1);
}

// THE GATE ON THE BIGGEST HAMMER IN THE USB DRIVER must default to OFF.
// `system.usb_recover` decides whether a port that has exhausted every
// cheaper lever may reset the whole controller -- which takes the
// keyboard with it. A machine that has never been told to do that must
// not, so the interesting direction is the absent key, not the set one.
//
// Deliberately does NOT write the setting: a KTEST that turned this on
// and failed before restoring it would leave every later boot of that
// image resetting its controller (CLAUDE.md's "a test that applies a
// setting changes the machine for every later tool").
KTEST("usb", "system.usb_recover defaults to off") {
    char v[8];
    int present = etc_config_get("/etc/toyos.conf", "usb_recover", v, sizeof v);
    if (present && k_strcmp(v, "on") == 0)
        KTEST_SKIP("this machine has usb_recover ON deliberately");
    KTEST_ASSERT_EQ(usb_recover_enabled(), 0);
}
