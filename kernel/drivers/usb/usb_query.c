// The enumerated USB devices, as queryable FACTS.
//
// A LIST, one record per device, so /bin/lsusb can print them from ring
// 3 without a syscall of its own -- CLAUDE.md's rule that a fact is a
// PROVIDER, not a new syscall. `lspci` reaches the PCI table the older
// way (SYS_PCI_COUNT/SYS_PCI_INFO); this is the shape a new one takes.
//
// No caching and no disk access here, unlike partition_query.c: the
// device table is already in memory, filled once at enumeration.
#include "query.h"
#include "usb.h"
#include "xhci_regs.h"
#include "string.h"
#include "kfmt.h"
#include <stddef.h>

// driver-none: a QUERY provider over the enumerated devices

static uint64_t abi_speed(uint8_t s) {
    switch (s) {  // dispatch-ok: bounded by the xHCI default speed IDs
        case XHCI_SPEED_LOW:   return QUERY_USB_SPEED_LOW;
        case XHCI_SPEED_FULL:  return QUERY_USB_SPEED_FULL;
        case XHCI_SPEED_HIGH:  return QUERY_USB_SPEED_HIGH;
        case XHCI_SPEED_SUPER: return QUERY_USB_SPEED_SUPER;
        default:               return QUERY_USB_SPEED_UNKNOWN;
    }
}

void usb_device_id(const struct usb_device_info *info, char *out, unsigned cap) {
    k_snprintf(out, cap, "usb:%u:%04x:%04x", info->port, info->vendor_id, info->product_id);
}

static int usb_q_count(void) { return usb_device_count(); }

static int usb_q_fill(int index, void *out) {
    const struct usb_device_info *d = usb_device_at(index);
    if (!d) return 0;
    struct query_usb *q = out;
    k_memset(q, 0, sizeof *q);
    q->port        = d->port;
    q->slot        = d->slot;
    q->speed       = abi_speed(d->speed);
    q->vendor_id   = d->vendor_id;
    q->product_id  = d->product_id;
    q->dev_class   = d->dev_class;
    q->if_class    = d->if_class;
    q->if_subclass = d->if_subclass;
    q->if_protocol = d->if_protocol;
    // "Bound" means a driver in THIS build claimed it (HID or hub),
    // which is not the same as "it is a HID device" -- a HID device
    // whose endpoint could not be configured is present and unbound,
    // and lsusb should say so rather than implying it works.
    q->bound       = d->bound;
    k_strlcpy(q->manufacturer, d->manufacturer, sizeof q->manufacturer);
    k_strlcpy(q->product, d->product, sizeof q->product);
    if (d->driver) k_strlcpy(q->driver, d->driver, sizeof q->driver);
    return 1;
}

// --- QUERY_USBDESC: the raw configuration descriptors, in slices ------
//
// One record per QUERY_USBDESC_DATA bytes of one device's kept
// configuration, walked device by device. The index is a position in
// the CONCATENATION of every device's slices, because a list provider's
// index has nowhere to carry a second selector (QUERY_FONTGLYPH's
// reasoning, and QUERY_PROCMAP's) -- so /bin/lsusb groups by the
// `slot` field the record carries.

static uint32_t slices_of(uint32_t len) {
    return (len + QUERY_USBDESC_DATA - 1) / QUERY_USBDESC_DATA;
}

// Resolves a flat record index to (device, slice), or -1. Shared by
// count and fill so the two cannot disagree about the ordering.
static int locate(int index, int *out_dev, uint32_t *out_slice) {
    int n = usb_device_count();
    for (int i = 0; i < n; i++) {
        uint32_t len = 0;
        if (!usb_device_config(i, &len) || !len) continue;
        uint32_t slices = slices_of(len);
        if ((uint32_t)index < slices) {
            *out_dev = i;
            *out_slice = (uint32_t)index;
            return 0;
        }
        index -= (int)slices;
    }
    return -1;
}

static int usbdesc_q_count(void) {
    int total = 0, n = usb_device_count();
    for (int i = 0; i < n; i++) {
        uint32_t len = 0;
        if (usb_device_config(i, &len) && len) total += (int)slices_of(len);
    }
    return total;
}

static int usbdesc_q_fill(int index, void *out) {
    int dev = 0;
    uint32_t slice = 0;
    if (index < 0 || locate(index, &dev, &slice) < 0) return 0;

    uint32_t len = 0;
    const uint8_t *cfg = usb_device_config(dev, &len);
    const struct usb_device_info *d = usb_device_at(dev);
    if (!cfg || !d) return 0;

    struct query_usbdesc *q = out;
    k_memset(q, 0, sizeof *q);
    q->slot   = d->slot;
    q->total  = len;
    q->offset = slice * QUERY_USBDESC_DATA;
    q->len    = len - q->offset;
    if (q->len > QUERY_USBDESC_DATA) q->len = QUERY_USBDESC_DATA;
    k_memcpy(q->data, cfg + q->offset, q->len);
    return 1;
}

static const struct query_provider usbdesc_provider = {
    .cls = QUERY_USBDESC,
    .name = "usbdesc",
    .record_size = sizeof(struct query_usbdesc),
    .flags = QUERY_F_LIST,
    .count = usbdesc_q_count,
    .fill = usbdesc_q_fill,
    .fields = 0,
    .field_count = 0,
};

// No named fields: a LIST is not addressable as a flat name, for the
// reason partition_query.c gives -- an index baked into a name means a
// different record a moment later.
static const struct query_provider usb_provider = {
    .cls = QUERY_USB,
    .name = "usb",
    .record_size = sizeof(struct query_usb),
    .flags = QUERY_F_LIST,
    .count = usb_q_count,
    .fill = usb_q_fill,
    .fields = 0,
    .field_count = 0,
};

void usb_query_init(void) {
    query_register(&usb_provider);
    query_register(&usbdesc_provider);
}
