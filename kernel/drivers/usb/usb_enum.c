// USB device enumeration: the standard requests, and the descriptor
// walk that turns a connected port into a described device.
//
// This is the USB-semantics half of the seam kernel/include/kernel/xhci.h
// describes -- it knows about descriptors and requests, and nothing at
// all about TRBs, doorbells or contexts.
//
// DESCRIPTORS ARE UNTRUSTED INPUT. Every length and offset in them comes
// from the device, so the walk below bounds-checks each step rather than
// trusting bLength or wTotalLength. That is the same posture ttf.c takes
// for a font file and for the same reason: this is attacker-shaped data
// being parsed in ring 0. A malformed descriptor makes the device be
// REJECTED, never guessed at -- CLAUDE.md's rule that a parser refuses
// rather than invents.
#include "usb.h"
#include "xhci.h"
#include "usb_hid.h"
#include "xhci_regs.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"

// --- standard requests ------------------------------------------------
#define REQ_GET_DESCRIPTOR   6
#define REQ_SET_CONFIGURATION 9

#define DESC_DEVICE     1
#define DESC_CONFIG     2
#define DESC_STRING     3
#define DESC_INTERFACE  4
#define DESC_ENDPOINT   5

#define USB_CLASS_HID   3

// bmRequestType
#define DIR_IN          0x80
#define TYPE_STANDARD   0x00
#define RECIP_DEVICE    0x00

// One 4 KiB staging buffer for descriptors, reused across devices.
//
// Static rather than heap because this runs at boot and the frame
// budget matters (virtio_input.c makes the same call). It is a DMA
// target, so it must be aligned and it must not be on a stack: a
// ring-0 stack is 16 KiB with a guard page and a 1 KiB frame budget.
static uint8_t g_desc_buf[4096] __attribute__((aligned(64)));

static struct usb_device_info g_devs[USB_MAX_DEVICES];
static int g_dev_count;

static void fill_setup(uint8_t out[8], uint8_t type, uint8_t req,
                       uint16_t value, uint16_t index, uint16_t len) {
    out[0] = type;
    out[1] = req;
    out[2] = (uint8_t)(value & 0xFF);
    out[3] = (uint8_t)(value >> 8);
    out[4] = (uint8_t)(index & 0xFF);
    out[5] = (uint8_t)(index >> 8);
    out[6] = (uint8_t)(len & 0xFF);
    out[7] = (uint8_t)(len >> 8);
}

static int get_descriptor(uint8_t slot, uint8_t type, uint8_t index,
                          uint16_t langid, void *buf, uint16_t len) {
    uint8_t setup[8];
    fill_setup(setup, DIR_IN | TYPE_STANDARD | RECIP_DEVICE, REQ_GET_DESCRIPTOR,
               (uint16_t)((uint16_t)type << 8 | index), langid, len);
    return xhci_control(slot, setup, buf, len, 1);
}

static int set_configuration(uint8_t slot, uint8_t value) {
    uint8_t setup[8];
    fill_setup(setup, TYPE_STANDARD | RECIP_DEVICE, REQ_SET_CONFIGURATION,
               value, 0, 0);
    return xhci_control(slot, setup, 0, 0, 0);
}

// --- strings ----------------------------------------------------------
//
// A USB string descriptor is UTF-16LE, and this console is ASCII. A
// codepoint outside ASCII becomes '?' rather than being dropped or
// truncated: the string is a human label, so a visibly wrong character
// is better than a silently shorter name. (Contrast fat32.c, where a
// non-ASCII long name is REFUSED on create -- there the string is an
// identity that has to round-trip, and here it is a caption.)
static void read_string(uint8_t slot, uint8_t index, char *out, uint32_t cap) {
    out[0] = 0;
    if (!index || cap < 2) return;

    // String descriptor 0 is the list of language ids, not a string.
    uint8_t lang[4];
    if (get_descriptor(slot, DESC_STRING, 0, 0, lang, sizeof lang) < 4) return;
    uint16_t langid = (uint16_t)(lang[2] | ((uint16_t)lang[3] << 8));

    uint8_t buf[256];
    int got = get_descriptor(slot, DESC_STRING, index, langid, buf, sizeof buf);
    if (got < 2) return;

    uint32_t blen = buf[0];
    if (blen < 2 || blen > (uint32_t)got) return;   // the device's own length, checked

    uint32_t o = 0;
    for (uint32_t i = 2; i + 1 < blen && o + 1 < cap; i += 2) {
        uint16_t wc = (uint16_t)(buf[i] | ((uint16_t)buf[i + 1] << 8));
        out[o++] = (wc && wc < 0x80) ? (char)wc : '?';
    }
    out[o] = 0;
}

// --- the walk ---------------------------------------------------------

// Finds the first HID interface in a configuration and its interrupt-IN
// endpoint. Returns 1 on success.
//
// The bounds here are the point. `total` is the device's own
// wTotalLength, already clamped by the caller to what was actually
// read; every descriptor's bLength is checked to be non-zero and to fit
// before it is stepped over. A zero bLength would otherwise be an
// infinite loop, and an oversized one would walk off the buffer.
static int find_hid_interface(const uint8_t *cfg, uint32_t total,
                              uint8_t *out_if_class, uint8_t *out_if_sub,
                              uint8_t *out_if_proto, uint8_t *out_ep,
                              uint16_t *out_mps, uint8_t *out_interval,
                              uint8_t *out_ifnum) {
    uint32_t o = 0;
    int in_hid = 0;
    while (o + 2 <= total) {
        uint32_t blen = cfg[o];
        uint8_t  btype = cfg[o + 1];
        if (blen < 2 || o + blen > total) return 0;   // refuse, do not guess

        if (btype == DESC_INTERFACE && blen >= 9) {
            in_hid = (cfg[o + 5] == USB_CLASS_HID);
            if (in_hid) {
                *out_ifnum   = cfg[o + 2];
                *out_if_class = cfg[o + 5];
                *out_if_sub   = cfg[o + 6];
                *out_if_proto = cfg[o + 7];
            }
        } else if (btype == DESC_ENDPOINT && blen >= 7 && in_hid) {
            uint8_t addr = cfg[o + 2];
            uint8_t attr = cfg[o + 3];
            if ((addr & 0x80) && (attr & 0x03) == 3) {   // interrupt IN
                *out_ep       = addr;
                *out_mps      = (uint16_t)(cfg[o + 4] | ((uint16_t)cfg[o + 5] << 8));
                *out_interval = cfg[o + 6];
                return 1;
            }
        }
        o += blen;
    }
    return 0;
}

// Brings one connected port all the way to "configured and described".
// Returns the index into the device table, or -1.
int usb_enumerate_port(uint8_t port, uint8_t speed) {
    if (g_dev_count >= USB_MAX_DEVICES) return -1;

    int slot = xhci_address_device(port, speed);
    if (slot < 0) return -1;

    // Eight bytes first, because bMaxPacketSize0 is byte 7 and the
    // controller was configured with a GUESS. Asking for all 18 before
    // correcting it is what hangs on a device whose real MPS is 64.
    int got = get_descriptor((uint8_t)slot, DESC_DEVICE, 0, 0, g_desc_buf, 8);
    if (got < 8) {
        klog_printf("usb: port %u: short device descriptor (%d)\n", port, got);
        return -1;
    }
    uint16_t real_mps = g_desc_buf[7];
    if (speed == XHCI_SPEED_SUPER) real_mps = (uint16_t)(1u << g_desc_buf[7]);
    if (real_mps && real_mps != 8)
        xhci_set_ep0_mps((uint8_t)slot, real_mps);

    got = get_descriptor((uint8_t)slot, DESC_DEVICE, 0, 0, g_desc_buf, 18);
    if (got < 18) {
        klog_printf("usb: port %u: device descriptor truncated (%d)\n", port, got);
        return -1;
    }

    struct usb_device_info *d = &g_devs[g_dev_count];
    k_memset(d, 0, sizeof *d);
    d->port         = port;
    d->slot         = (uint8_t)slot;
    d->speed        = speed;
    d->dev_class    = g_desc_buf[4];
    d->dev_subclass = g_desc_buf[5];
    d->dev_protocol = g_desc_buf[6];
    d->vendor_id    = (uint16_t)(g_desc_buf[8]  | ((uint16_t)g_desc_buf[9]  << 8));
    d->product_id   = (uint16_t)(g_desc_buf[10] | ((uint16_t)g_desc_buf[11] << 8));
    uint8_t i_manuf = g_desc_buf[14], i_prod = g_desc_buf[15];

    // The configuration, twice: nine bytes to learn wTotalLength, then
    // the whole thing. Clamped to the staging buffer -- a device
    // claiming more than 4 KiB of descriptors is refused rather than
    // allowed to overrun.
    got = get_descriptor((uint8_t)slot, DESC_CONFIG, 0, 0, g_desc_buf, 9);
    if (got < 9) return -1;
    uint32_t total = (uint32_t)(g_desc_buf[2] | ((uint32_t)g_desc_buf[3] << 8));
    uint8_t  cfg_value = g_desc_buf[5];
    if (total < 9 || total > sizeof g_desc_buf) {
        klog_printf("usb: port %u: config descriptor claims %u bytes\n", port, total);
        return -1;
    }
    got = get_descriptor((uint8_t)slot, DESC_CONFIG, 0, 0, g_desc_buf, (uint16_t)total);
    if (got < (int)total) total = (uint32_t)(got < 0 ? 0 : got);

    read_string((uint8_t)slot, i_manuf, d->manufacturer, sizeof d->manufacturer);
    read_string((uint8_t)slot, i_prod,  d->product,      sizeof d->product);

    if (set_configuration((uint8_t)slot, cfg_value) < 0) {
        klog_printf("usb: port %u: set configuration failed\n", port);
        return -1;
    }

    uint8_t ep = 0, interval = 0, ifnum = 0;
    uint16_t mps = 0;
    if (find_hid_interface(g_desc_buf, total, &d->if_class, &d->if_subclass,
                           &d->if_protocol, &ep, &mps, &interval, &ifnum)) {
        d->hid_ep       = ep;
        d->hid_mps      = mps;
        d->hid_interval = interval;
        d->hid_ifnum    = ifnum;
    }

    d->in_use = 1;
    g_dev_count++;

    klog_printf("usb: port %u: %04x:%04x \"%s\" %s, class %u/%u/%u\n",
                port, d->vendor_id, d->product_id,
                d->product[0] ? d->product : "(no product string)",
                d->manufacturer[0] ? d->manufacturer : "",
                d->if_class, d->if_subclass, d->if_protocol);

    // Binding is the class driver's decision, not enumeration's: a
    // device this build has no driver for stays in the table and is
    // reported by lsusb, it just does nothing.
    usb_hid_bind(d);
    return g_dev_count - 1;
}

int usb_device_count(void) { return g_dev_count; }

const struct usb_device_info *usb_device_at(int index) {
    if (index < 0 || index >= g_dev_count) return 0;
    return &g_devs[index];
}
