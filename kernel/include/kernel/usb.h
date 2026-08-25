#ifndef USB_H
#define USB_H

#include <stdint.h>

// USB, as the rest of the kernel sees it.
//
// WHAT THIS IS. One xHCI host controller driver, the device enumeration
// on top of it, and a HID boot-protocol class driver that registers
// keyboards and mice with the input core. Nothing else: no hubs, no
// mass storage, no HID report-descriptor parsing. See docs/roadmap.md's
// USB section for what is deliberately left out.
//
// WHY xHCI AND ONLY xHCI. UHCI/OHCI/EHCI are perhaps a quarter of the
// code between them, and they run on no machine made since roughly
// 2010 -- Intel dropped the EHCI companion controllers at Skylake. A
// machine with no PS/2 port has an xHCI controller and nothing else,
// and that machine is the entire reason this driver exists.
//
// THERE IS NO HCD ABSTRACTION, and that is deliberate rather than
// deferred. An ops table needs a second implementer to be worth
// anything, and there is no plausible one. The precedent here is exact:
// virtio_pci.c + virtqueue.c are a shared transport under four device
// drivers and there is no `struct virtio_transport` vtable either --
// drivers call virtqueue_submit() by name. So usb_hid.c calls
// xhci_control() by name. If a second controller ever arrives,
// converting xhci.h's handful of functions is a mechanical afternoon.

#define USB_MAX_DEVICES 8

// What a device reported about itself, for `lsusb` and the debug
// console. Filled by enumeration; the strings are the device's own
// string descriptors, empty when it has none.
struct usb_device_info {
    uint8_t  in_use;
    uint8_t  port;          // 1-based root port
    uint8_t  slot;          // xHCI slot id
    uint8_t  speed;         // XHCI_SPEED_*
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t  dev_class;
    uint8_t  dev_subclass;
    uint8_t  dev_protocol;
    uint8_t  if_class;      // the interface this driver bound, if any
    uint8_t  if_subclass;
    uint8_t  if_protocol;
    char     manufacturer[32];
    char     product[32];

    // The HID interrupt-IN endpoint, when this device has one. Zero
    // `hid_ep` means nothing here bound it.
    uint8_t  hid_ep;        // bEndpointAddress, so 0x81 is IN endpoint 1
    uint16_t hid_mps;
    uint8_t  hid_interval;
    uint8_t  hid_ifnum;
};

// Finds and brings up an xHCI controller, enumerates what is attached,
// and registers any HID keyboard or mouse with the input core.
//
// Called from kernel_main() after pmm_init() (DMA frames), idt_init()
// (interrupts, and a ticking PIT for the reset waits) and
// i8042_register_sources() -- the last so PS/2 stays input source 0 and
// `lsdev`'s ordering does not shift under the tests.
//
// Never panics. A controller that will not come up is a log line and a
// return, because a hung reset here would hang every headless test in
// the repo on a machine that happens to have USB.
void usb_init(void);

// Is there a controller at all? 0 on a machine with no xHCI.
int usb_controller_present(void);

// One line about the controller, for `lsdev` and `lsusb`.
// Returns 0 when there is none.
int usb_controller_summary(char *buf, uint32_t cap);

int usb_device_count(void);
const struct usb_device_info *usb_device_at(int index);

// Brings one connected root port to "configured and described", adding
// it to the table above. Returns its index, or -1. Called by xhci.c's
// port scan; the split is the seam xhci.h describes.
int usb_enumerate_port(uint8_t port, uint8_t speed);

// Diagnostic counters. These exist so a test can distinguish "the
// driver never ran" from "the driver ran and decoded nothing", which
// an assertion on behaviour alone cannot.
uint32_t usb_events_seen(void);
uint32_t usb_irqs_seen(void);

// Dumps controller registers and both ring states to the kernel log.
// The `usb` debug-console command; this is how stages 2-4 are actually
// debugged, so it exists from the start rather than being retrofitted.
void usb_dump(void);

#endif
