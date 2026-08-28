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
#include <stddef.h>

static uint64_t abi_speed(uint8_t s) {
    switch (s) {  // dispatch-ok: bounded by the xHCI default speed IDs
        case XHCI_SPEED_LOW:   return QUERY_USB_SPEED_LOW;
        case XHCI_SPEED_FULL:  return QUERY_USB_SPEED_FULL;
        case XHCI_SPEED_HIGH:  return QUERY_USB_SPEED_HIGH;
        case XHCI_SPEED_SUPER: return QUERY_USB_SPEED_SUPER;
        default:               return QUERY_USB_SPEED_UNKNOWN;
    }
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
    return 1;
}

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

void usb_query_init(void) { query_register(&usb_provider); }
