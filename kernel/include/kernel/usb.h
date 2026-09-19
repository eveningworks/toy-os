#ifndef USB_H
#define USB_H

#include <stdint.h>

// USB, as the rest of the kernel sees it.
//
// WHAT THIS IS. One xHCI host controller driver, the device enumeration
// on top of it, and the hub driver -- the BUS. See docs/roadmap.md's
// USB section for what is deliberately left out.
//
// THE CLASS DRIVERS ARE NOT IN kernel/drivers/usb/. A driver lives with
// the registry it plugs into, so USB HID is in drivers/input/, USB
// audio in drivers/sound/, and both Ethernet adapters in drivers/net/
// (kernel/README.md has the rule). What crosses that seam is declared
// below: enumeration calls each class driver's _bind/_unbind by name,
// which is here because enumeration is the caller.
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
// drivers call virtqueue_submit() by name. So input_usbhid.c calls
// xhci_control() by name. If a second controller ever arrives,
// converting xhci.h's handful of functions is a mechanical afternoon.

#define USB_MAX_DEVICES   8
#define USB_MAX_INTERFACES 8   // per device; a Sound Blaster G6 has 5

// One interface out of a configuration descriptor, with its first
// interrupt-IN endpoint (ep 0 when it has none). A composite device --
// a wireless receiver is the canonical one: keyboard interface first,
// mouse second -- is several of these on one slot, and a driver that
// looks only at the first binds the keyboard half and leaves the mouse
// dead.
struct usb_interface_info {
    uint8_t  ifnum;
    uint8_t  if_class;
    uint8_t  if_subclass;
    uint8_t  if_protocol;
    uint8_t  ep;            // bEndpointAddress, so 0x81 is IN endpoint 1
    uint8_t  interval;
    uint16_t mps;
};

// What a device reported about itself, for `lsusb` and the debug
// console. Filled by enumeration; the strings are the device's own
// string descriptors, empty when it has none.
struct usb_device_info {
    uint8_t  in_use;
    uint8_t  port;          // 1-based port on its PARENT (root, or a hub)
    uint8_t  root_port;     // 1-based root port the whole chain hangs off
    uint8_t  slot;          // xHCI slot id
    uint8_t  speed;         // XHCI_SPEED_*
    // Topology. Route string 0 / depth 0 is a root-port device; a hub
    // child carries its hub's slot in parent_slot so a detach can take
    // the subtree down with it.
    uint32_t route;         // xHCI route string (4 bits per tier)
    uint8_t  parent_slot;   // 0 for a root-port device
    uint8_t  depth;         // hubs above this device
    uint8_t  tt_slot;       // the high-speed hub doing this device's
    uint8_t  tt_port;       //   split transactions; 0 when none
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t  dev_class;
    uint8_t  dev_subclass;
    uint8_t  dev_protocol;
    uint8_t  if_class;      // interface 0's triple, for lsusb
    uint8_t  if_subclass;
    uint8_t  if_protocol;
    char     manufacturer[32];
    char     product[32];

    struct usb_interface_info ifs[USB_MAX_INTERFACES];
    uint8_t  if_count;

    // Set by the class drivers: bound is what lsusb reports, and for a
    // HID device the first bound interface's endpoint mirrors into
    // hid_ep for the tests that predate composite support.
    uint8_t  bound;
    uint8_t  hid_ep;

    // THE CONFIGURATION DESCRIPTOR, KEPT. One DMA frame per device,
    // freed on detach. It is here because the interesting half of a
    // device this build cannot bind is the class-specific descriptors
    // no driver walked -- `lsusb -D` prints them, and they are what a
    // KTEST fixture is made of.
    uint8_t *cfg;
    uint64_t cfg_phys;
    uint32_t cfg_len;
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

// Registers the QUERY_USB provider. Called from usb_init().
void usb_query_init(void);

// Is there a controller at all? 0 on a machine with no xHCI.
int usb_controller_present(void);

// One line about the controller, for `lsdev` and `lsusb`.
// Returns 0 when there is none.
int usb_controller_summary(char *buf, uint32_t cap);

int usb_device_count(void);
const struct usb_device_info *usb_device_at(int index);

// Brings one connected root port to "configured and described", adding
// it to the table above. Returns its index, or -1. Called by xhci.c's
// port scan; the split is the seam xhci.h describes. Cleans its slot up
// and retries once internally -- a real low/full-speed device is
// entitled to fumble its first descriptor read, and Linux retries too.
// Burn `ms` on whichever clock actually advances here -- the
// clocksource when it is deadline-capable, the PIT otherwise. Shared so
// the enumeration layer can pace a device without a second
// implementation of the same choice.
void xhci_delay_ms(uint32_t ms);

// `patient` inserts the pauses a slow device needs (see
// usb_enum.c's read_configuration). It is 0 on the first attempt and 1
// on a retry: fast when the device is well, careful when it is not.
int usb_enumerate_port(uint8_t port, uint8_t speed, int patient);

// The same, for a device behind a hub: the route string and TT fields
// come from the hub driver, which is the only caller that has them.
int usb_enumerate_device(uint8_t root_port, uint8_t parent_port,
                         uint32_t route, uint8_t depth, uint8_t speed,
                         uint8_t parent_slot, uint8_t tt_slot, uint8_t tt_port,
                         int patient);

// Tears down every device on `root_port` -- the device itself and, when
// it is a hub, everything behind it. Called from the deferred detach
// work; never from interrupt context (it issues commands).
void usb_detach_root_port(uint8_t root_port);

// The slot of the device sitting directly on `root_port`, or 0 for an
// empty port. How the deferred attach tells a REAL plug from the
// connect-change the bring-up itself raises for a device scan_ports()
// already enumerated.
uint8_t usb_root_port_slot(uint8_t root_port);

// Tears down one device (and its subtree, when it is a hub) by slot.
void usb_detach_slot(uint8_t slot);

// The configuration-descriptor walk, exported for the KTESTs: fills
// `out` with up to `max` interfaces, each carrying its first
// interrupt-IN endpoint. Returns the interface count, or -1 for a
// descriptor that is malformed (refused, never guessed at).
int usb_parse_config_interfaces(const uint8_t *cfg, uint32_t total,
                                struct usb_interface_info *out, int max);

// The kept configuration descriptor of `index` in the device table, or
// 0. `*len` is its length. Read by the QUERY_USBDESC provider.
const uint8_t *usb_device_config(int index, uint32_t *len);

// One of the device's string descriptors, as ASCII. Exported because a
// class driver can need one the enumeration had no reason to keep -- a
// CDC-ECM adapter's MAC address is a STRING, twelve hex characters,
// which is the only place it is written down.
void usb_read_string(uint8_t slot, uint8_t index, char *out, uint32_t cap);

// --- CDC Ethernet (drivers/net/net_usb_ecm.c) -------------------------

// Binds an enumerated CDC-ECM adapter and registers a `net_device`.
// `cfg`/`total` is the configuration already read, because the MAC, the
// data interface and the segment size all live in CLASS-SPECIFIC
// descriptors the interface walk does not keep -- the same reason
// usb_audio_bind() takes them. Returns 1 when it took the device.
int usb_net_bind(struct usb_device_info *info, const uint8_t *cfg,
                 uint32_t total);

// Releases the device on `slot`, if it is the bound one.
void usb_net_unbind(uint8_t slot);

// --- Realtek USB Ethernet, the vendor protocol (drivers/net/rtl_usb.c) --

// Is this a device the Realtek driver claims? Asked by enumeration
// BEFORE a configuration is chosen, because the vendor configuration is
// only driveable when this says so -- a vendor-specific interface says
// nothing about what is behind it.
int usb_r8153_claims(uint16_t vid, uint16_t pid);

// Binds the vendor configuration of an RTL8153 or RTL8156 and registers a
// `net_device`. Takes the raw configuration for the same reason
// usb_net_bind() does. Returns 1 when it took the device.
int usb_r8153_bind(struct usb_device_info *info, const uint8_t *cfg,
                   uint32_t total);

void usb_r8153_unbind(uint8_t slot);

// The framing, exported for the KTESTs -- a bulk transfer is not a
// frame here, and this is the part that can be tested without the
// hardware. `usb_r8153_rx_step` walks ONE frame out of a completed
// receive transfer: it returns the bytes that frame occupies
// (descriptor, payload and padding) or 0 to stop, and names the frame
// through *out_off/*out_len, with *out_len 0 for one to drop.
uint32_t usb_r8153_rx_step(const uint8_t *buf, uint32_t avail,
                           uint32_t *out_off, uint32_t *out_len);

// The 8-byte transmit descriptor a frame of `len` bytes is sent behind.
void usb_r8153_tx_desc(uint8_t out[8], uint32_t len);

// --- hubs (usb_hub.c) -------------------------------------------------

// Binds an enumerated hub: reads its hub descriptor, powers its ports,
// scans them, and configures the status-change endpoint so later
// connects and disconnects are seen. Returns 1 when it took the device.
int usb_hub_bind(struct usb_device_info *d);

// Processes any queued hub status-change reports: resets and enumerates
// new devices, detaches unplugged ones. Deferred work only -- it does
// control transfers, so it must never run from the interrupt handler.
void usb_hub_service(void);

// Forgets the hub state for `slot` (called by the detach path).
void usb_hub_forget(uint8_t slot);

// Diagnostic counters. These exist so a test can distinguish "the
// driver never ran" from "the driver ran and decoded nothing", which
// an assertion on behaviour alone cannot.
// The controller's IRQ line, or 0 when it is polled. A HID source
// mirrors it, so `lsdev` reports the same servicing for the device as
// for the controller that actually fields its interrupts.
uint8_t usb_controller_irq(void);

// ...or the MSI/MSI-X vector, when the controller is not on a line at
// all. A HID source mirrors whichever is set, so `lsdev` reports the
// device the same way it reports the controller that fields its
// interrupts -- and so a driver does not install a poll thunk for a
// controller that is perfectly capable of interrupting.
uint8_t usb_controller_msi_vector(void);

uint32_t usb_events_seen(void);
uint32_t usb_irqs_seen(void);

// Dumps controller registers and both ring states to the kernel log.
// The `usb` debug-console command; this is how stages 2-4 are actually
// debugged, so it exists from the start rather than being retrofitted.
void usb_dump(void);

// `config set kernel.usb_reset <port>`: force a root port through a
// real reset and re-enumerate it, without touching the cable. A
// DIAGNOSTIC for the intermittent enumeration failure in docs/bugs.md,
// whose known cure is a replug -- and a replug both re-connects the
// port and power-cycles the device, where this does only the first. So
// a success implicates the port state and a failure clears it.
// Returns 1 if the device enumerated. Ports are 1-based, as logged.
int usb_diag_reset_port(unsigned port);

// `config set kernel.usb_replug <port>`: the SOFTWARE REPLUG of that
// port's socket -- drop its power and bring it back where the
// controller allows that (HCCPARAMS1.PPC), and otherwise un-route and
// re-route it through the Intel port mux, which is the only such lever
// the ASUS has. The recovery the driver runs by itself after an
// enumeration gives up; exposed as a knob so the MECHANISM can be
// verified on a device that currently WORKS, instead of only when the
// intermittent failure fires. Returns 1 if it was queued; which lever
// ran, and whether the machine has one at all, is in the log.
int usb_diag_replug_port(unsigned port);

// Re-initialise the whole controller (kernel.usb_hcreset). See xhci.c.
int usb_controller_reinit(void);

// The OTHER port number of `port`'s physical socket, or 0 if it has
// none. Both are 1-based, as every port number a user sees is.
//
// A USB3 socket is two ports to the controller -- one in its USB2 range
// and one in its USB3 range -- and which one a device appears on is
// decided by whether its SuperSpeed link trained. **THE CONTROLLER
// DOES NOT SAY WHICH PAIRS WITH WHICH**; this is derived by position
// within the two ranges, which is Linux's fallback when ACPI `_PLD` is
// unavailable, and it is logged at init so a wrong guess is visible.
// See companion_port() in xhci.c.
int xhci_companion_port(unsigned port);

// The same rule with the ranges passed IN, so it can be checked without
// a controller. All 1-based; 0 for "no companion" and for a port in
// neither range.
int xhci_companion_in(unsigned first2, unsigned count2,
                      unsigned first3, unsigned count3, unsigned port);

#endif
