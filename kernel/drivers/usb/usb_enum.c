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
#include "usb_audio.h"
#include "xhci_regs.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"
#include "pmm.h"

// --- standard requests ------------------------------------------------
#define REQ_GET_DESCRIPTOR   6
#define REQ_SET_CONFIGURATION 9

#define DESC_DEVICE     1
#define DESC_CONFIG     2
#define DESC_STRING     3
#define DESC_INTERFACE  4
#define DESC_ENDPOINT   5

#define USB_CLASS_CDC   2
#define USB_CLASS_HID   3
#define USB_CLASS_HUB   9
#define USB_CLASS_AUDIO 1
#define USB_CLASS_VENDOR 0xFF

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
void usb_read_string(uint8_t slot, uint8_t index, char *out, uint32_t cap) {
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

// Walks a configuration and records EVERY interface, each with its
// first interrupt-IN endpoint (ep 0 when it has none). All of them,
// not the first HID one: a wireless receiver is a keyboard interface
// followed by a mouse interface on one device, and a walk that stopped
// at the first bound the keyboard and left the mouse dead.
//
// The bounds here are the point. `total` is the device's own
// wTotalLength, already clamped by the caller to what was actually
// read; every descriptor's bLength is checked to be non-zero and to fit
// before it is stepped over. A zero bLength would otherwise be an
// infinite loop, and an oversized one would walk off the buffer.
int usb_parse_config_interfaces(const uint8_t *cfg, uint32_t total,
                                struct usb_interface_info *out, int max) {
    uint32_t o = 0;
    int count = 0;
    struct usb_interface_info *cur = 0;
    while (o + 2 <= total) {
        uint32_t blen = cfg[o];
        uint8_t  btype = cfg[o + 1];
        if (blen < 2 || o + blen > total) return -1;   // refuse, do not guess

        if (btype == DESC_INTERFACE && blen >= 9) {
            // An ALTERNATE SETTING re-describes an interface number
            // already seen; setting 0 is what SET_CONFIGURATION selects,
            // so only bAlternateSetting 0 opens a new record.
            if (cfg[o + 3] != 0) {
                cur = 0;
            } else if (count < max) {
                cur = &out[count++];
                cur->ifnum       = cfg[o + 2];
                cur->if_class    = cfg[o + 5];
                cur->if_subclass = cfg[o + 6];
                cur->if_protocol = cfg[o + 7];
                cur->ep = 0; cur->mps = 0; cur->interval = 0;
            } else {
                cur = 0;   // over the cap: counted structure, dropped detail
            }
        } else if (btype == DESC_ENDPOINT && blen >= 7 && cur && !cur->ep) {
            uint8_t addr = cfg[o + 2];
            uint8_t attr = cfg[o + 3];
            if ((addr & 0x80) && (attr & 0x03) == 3) {   // interrupt IN
                cur->ep       = addr;
                cur->mps      = (uint16_t)(cfg[o + 4] | ((uint16_t)cfg[o + 5] << 8));
                cur->interval = cfg[o + 6];
            }
        }
        o += blen;
    }
    return count;
}

// Reads configuration `index` whole into g_desc_buf. Two requests: nine
// bytes for wTotalLength, then the rest. Clamped to the staging buffer,
// so a device claiming more than 4 KiB is refused rather than allowed to
// overrun.
static int read_configuration(uint8_t slot, uint8_t index,
                              uint32_t *out_total, uint8_t *out_value) {
    int got = get_descriptor(slot, DESC_CONFIG, index, 0, g_desc_buf, 9);
    if (got < 9) return 0;
    uint32_t total = (uint32_t)(g_desc_buf[2] | ((uint32_t)g_desc_buf[3] << 8));
    if (total < 9 || total > sizeof g_desc_buf) return 0;
    if (out_value) *out_value = g_desc_buf[5];
    got = get_descriptor(slot, DESC_CONFIG, index, 0, g_desc_buf, (uint16_t)total);
    if (got < (int)total) total = (uint32_t)(got < 0 ? 0 : got);
    if (out_total) *out_total = total;
    return total >= 9;
}

// Does this configuration hold an interface a driver here could take?
//
// A VENDOR-SPECIFIC interface counts only when a driver has said, BY
// DEVICE ID, that it knows what is behind it -- class 0xFF describes
// nothing, so treating it as driveable in general would claim the
// vendor configuration of every device that has one.
static int configuration_is_driveable(const uint8_t *cfg, uint32_t total,
                                      uint16_t vid, uint16_t pid) {
    for (uint32_t o = 0; o + 2 <= total; ) {
        uint32_t blen = cfg[o];
        if (blen < 2 || o + blen > total) return 0;
        if (cfg[o + 1] == DESC_INTERFACE && blen >= 9) {
            uint8_t cls = cfg[o + 5];
            if (cls == USB_CLASS_HID || cls == USB_CLASS_HUB ||
                cls == USB_CLASS_AUDIO || cls == USB_CLASS_CDC)
                return 1;
            if (cls == USB_CLASS_VENDOR && usb_r8153_claims(vid, pid))
                return 1;
        }
        o += blen;
    }
    return 0;
}

// WHICH CONFIGURATION. Index 0 unless a LATER one holds a class this
// build can actually drive and index 0 does not -- which is not a
// preference but the only way to reach some devices at all: a TP-Link
// UE300 offers a vendor-specific configuration FIRST and standard
// CDC-ECM second, so taking the first leaves a perfectly ordinary
// Ethernet adapter unusable.
//
// BIASED TOWARDS 0 ON PURPOSE. Every device this kernel handled before
// had exactly one configuration, and a device whose first configuration
// is already driveable keeps it -- so this cannot change what a
// keyboard, a mouse, a hub or a sound card does. The cost of being
// wrong here is a device that used to work and stops, which is why the
// rule is "only when index 0 offers nothing".
static uint8_t pick_configuration(uint8_t slot, uint8_t configs,
                                  uint8_t port, uint16_t vid, uint16_t pid) {
    if (configs <= 1) return 0;

    uint32_t total = 0;
    if (read_configuration(slot, 0, &total, 0) &&
        configuration_is_driveable(g_desc_buf, total, vid, pid))
        return 0;

    for (uint8_t i = 1; i < configs && i < 8; i++) {
        if (!read_configuration(slot, i, &total, 0)) continue;
        if (!configuration_is_driveable(g_desc_buf, total, vid, pid)) continue;
        klog_printf("usb: port %u: %u configurations, choosing %u "
                    "(the first this build can drive)\n", port, configs, i);
        return i;
    }
    return 0;
}

static struct usb_device_info *dev_alloc(void) {
    for (int i = 0; i < USB_MAX_DEVICES; i++)
        if (!g_devs[i].in_use) return &g_devs[i];
    return 0;
}

// Brings one device -- root-port or hub-child -- all the way to
// "configured and described". Returns the index into the device table,
// or -1 WITH ITS SLOT DISABLED: the failure path frees what it
// allocated, which is what makes the caller's reset-and-retry start
// from nothing instead of from a wedged half-enumeration.
int usb_enumerate_device(uint8_t root_port, uint8_t parent_port,
                         uint32_t route, uint8_t depth, uint8_t speed,
                         uint8_t parent_slot, uint8_t tt_slot, uint8_t tt_port) {
    struct usb_device_info *d = dev_alloc();
    if (!d) return -1;

    int slot = xhci_address_device(root_port, route, speed, tt_slot, tt_port);
    if (slot < 0) return -1;

    // Eight bytes first, because bMaxPacketSize0 is byte 7 and the
    // controller was configured with a GUESS. Asking for all 18 before
    // correcting it is what hangs on a device whose real MPS is 64.
    int got = get_descriptor((uint8_t)slot, DESC_DEVICE, 0, 0, g_desc_buf, 8);
    if (got < 8) {
        klog_printf("usb: port %u: short device descriptor (%d)\n", parent_port, got);
        xhci_disable_slot((uint8_t)slot);
        return -1;
    }
    uint16_t real_mps = g_desc_buf[7];
    if (speed == XHCI_SPEED_SUPER) real_mps = (uint16_t)(1u << g_desc_buf[7]);
    if (real_mps && real_mps != 8)
        xhci_set_ep0_mps((uint8_t)slot, real_mps);

    got = get_descriptor((uint8_t)slot, DESC_DEVICE, 0, 0, g_desc_buf, 18);
    if (got < 18) {
        klog_printf("usb: port %u: device descriptor truncated (%d)\n", parent_port, got);
        xhci_disable_slot((uint8_t)slot);
        return -1;
    }

    k_memset(d, 0, sizeof *d);
    d->port         = parent_port;
    d->root_port    = root_port;
    d->route        = route;
    d->depth        = depth;
    d->parent_slot  = parent_slot;
    d->tt_slot      = tt_slot;
    d->tt_port      = tt_port;
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
    uint8_t configs = g_desc_buf[17];      // bNumConfigurations
    uint8_t pick = pick_configuration((uint8_t)slot, configs, parent_port,
                                      d->vendor_id, d->product_id);

    uint32_t total = 0;
    uint8_t cfg_value = 0;
    if (!read_configuration((uint8_t)slot, pick, &total, &cfg_value)) {
        klog_printf("usb: port %u: configuration %u unreadable\n",
                    parent_port, pick);
        xhci_disable_slot((uint8_t)slot);
        return -1;
    }

    // Kept, not just walked -- see usb.h's `cfg`. A frame we cannot get
    // costs the dump and nothing else, so this never fails enumeration.
    d->cfg_phys = pmm_alloc_contiguous(1);
    if (d->cfg_phys) {
        d->cfg = (uint8_t *)(uintptr_t)d->cfg_phys;   // identity-mapped
        d->cfg_len = total > 4096 ? 4096 : total;
        k_memcpy(d->cfg, g_desc_buf, d->cfg_len);
    }

    usb_read_string((uint8_t)slot, i_manuf, d->manufacturer, sizeof d->manufacturer);
    usb_read_string((uint8_t)slot, i_prod,  d->product,      sizeof d->product);

    if (set_configuration((uint8_t)slot, cfg_value) < 0) {
        klog_printf("usb: port %u: set configuration failed\n", parent_port);
        if (d->cfg_phys) pmm_free_contiguous(d->cfg_phys, 1);
        d->cfg = 0; d->cfg_phys = 0; d->cfg_len = 0;
        xhci_disable_slot((uint8_t)slot);
        return -1;
    }

    int ifc = usb_parse_config_interfaces(g_desc_buf, total, d->ifs,
                                          USB_MAX_INTERFACES);
    if (ifc > 0) {
        d->if_count    = (uint8_t)ifc;
        d->if_class    = d->ifs[0].if_class;
        d->if_subclass = d->ifs[0].if_subclass;
        d->if_protocol = d->ifs[0].if_protocol;
    }

    d->in_use = 1;
    g_dev_count++;

    klog_printf("usb: %s %u: %04x:%04x \"%s\" %s, class %u/%u/%u, %u interface(s)\n",
                depth ? "hub port" : "port", parent_port,
                d->vendor_id, d->product_id,
                d->product[0] ? d->product : "(no product string)",
                d->manufacturer[0] ? d->manufacturer : "",
                d->if_class, d->if_subclass, d->if_protocol, d->if_count);

    // Binding is the class drivers' decision, not enumeration's: a
    // device this build has no driver for stays in the table and is
    // reported by lsusb, it just does nothing.
    //
    // ANY interface, not interface 0. A composite device is entitled to
    // put its HID controls first and its audio second, and a dispatch
    // on ifs[0] alone never offers such a device to the audio driver at
    // all -- and offers a DAC with buttons to only one of the two.
    if (d->dev_class == USB_CLASS_HUB ||
        (d->if_count && d->ifs[0].if_class == USB_CLASS_HUB)) {
        usb_hub_bind(d);
    } else {
        // The vendor driver first: a device it claims is in its vendor
        // configuration BECAUSE this driver exists (see
        // configuration_is_driveable), so nothing else is offered it.
        for (int i = 0; i < d->if_count; i++) {
            if (d->ifs[i].if_class != USB_CLASS_VENDOR) continue;
            if (!usb_r8153_bind(d, g_desc_buf, total)) continue;
            break;
        }
        for (int i = 0; i < d->if_count; i++) {
            if (d->ifs[i].if_class != USB_CLASS_CDC) continue;
            // CDC is a family; only the Ethernet model is driven here.
            if (d->ifs[i].if_subclass != 0x06) continue;
            usb_net_bind(d, g_desc_buf, total);
            break;
        }
        for (int i = 0; i < d->if_count; i++) {
            if (d->ifs[i].if_class != USB_CLASS_AUDIO) continue;
            // The raw configuration goes with it: an audio device's
            // format and its volume control are CLASS-SPECIFIC
            // descriptors sitting between the standard ones, and the
            // interface walk above keeps neither. g_desc_buf is still
            // the one just read.
            usb_audio_bind(d, g_desc_buf, total);
            break;
        }
        usb_hid_bind(d);   // no-op on a device with no HID boot interface
    }
    return (int)(d - g_devs);
}

int usb_enumerate_port(uint8_t port, uint8_t speed) {
    return usb_enumerate_device(port, port, 0, 0, speed, 0, 0, 0);
}

// --- detach -----------------------------------------------------------

static struct usb_device_info *dev_by_slot(uint8_t slot) {
    for (int i = 0; i < USB_MAX_DEVICES; i++)
        if (g_devs[i].in_use && g_devs[i].slot == slot) return &g_devs[i];
    return 0;
}

// Depth-first: a hub's children go before the hub, because a child's
// Disable Slot while its route still exists is the orderly direction.
void usb_detach_slot(uint8_t slot) {
    struct usb_device_info *d = dev_by_slot(slot);
    if (!d) return;
    d->in_use = 0;           // off the table first, so recursion terminates
    g_dev_count--;

    for (int i = 0; i < USB_MAX_DEVICES; i++)
        if (g_devs[i].in_use && g_devs[i].parent_slot == slot)
            usb_detach_slot(g_devs[i].slot);

    usb_hid_unbind(slot);
    usb_audio_unbind(slot);
    usb_net_unbind(slot);
    usb_r8153_unbind(slot);
    usb_hub_forget(slot);
    xhci_disable_slot(slot);
    if (d->cfg_phys) pmm_free_contiguous(d->cfg_phys, 1);
    d->cfg = 0; d->cfg_phys = 0; d->cfg_len = 0;
    klog_printf("usb: %04x:%04x \"%s\" detached\n",
                d->vendor_id, d->product_id,
                d->product[0] ? d->product : "");
}

uint8_t usb_root_port_slot(uint8_t root_port) {
    for (int i = 0; i < USB_MAX_DEVICES; i++)
        if (g_devs[i].in_use && g_devs[i].root_port == root_port &&
            g_devs[i].depth == 0)
            return g_devs[i].slot;
    return 0;
}

void usb_detach_root_port(uint8_t root_port) {
    for (int i = 0; i < USB_MAX_DEVICES; i++)
        if (g_devs[i].in_use && g_devs[i].root_port == root_port &&
            g_devs[i].depth == 0)
            usb_detach_slot(g_devs[i].slot);
}

// --- the table, as lsusb sees it ---------------------------------------
//
// `index` is a position among the LIVE entries, not a table offset --
// a detach leaves no hole visible from outside, so QUERY_USB's
// count/fill pair stays consistent.
int usb_device_count(void) { return g_dev_count; }

const uint8_t *usb_device_config(int index, uint32_t *len) {
    const struct usb_device_info *d = usb_device_at(index);
    if (len) *len = d ? d->cfg_len : 0;
    return d ? d->cfg : 0;
}

const struct usb_device_info *usb_device_at(int index) {
    if (index < 0) return 0;
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (!g_devs[i].in_use) continue;
        if (index-- == 0) return &g_devs[i];
    }
    return 0;
}
