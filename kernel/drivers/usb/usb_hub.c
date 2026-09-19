// USB hubs: the class driver that makes "plugged into a hub" work.
//
// A hub is an ordinary device whose interrupt-IN endpoint carries a
// PORT-CHANGE BITMAP instead of input reports, plus a set of class
// requests for its ports. So it rides the same machinery HID does --
// xhci_add_interrupt_in() posts the status pipe, xhci_take_report()
// hands the bitmaps back -- and what this file adds is the port state
// machine: power, reset, speed, and the route string its children are
// addressed by.
//
// USB2 HUBS ONLY. A USB3 hub is a different beast (it presents a
// separate SuperSpeed hub with a different port-status format and
// depth-set requests), and this driver REFUSES one by name rather than
// driving it with USB2 requests. A USB3 hub's USB2 half -- which is
// how most physical hubs appear on a USB2 root port -- is just a USB2
// hub and works.
//
// EVERYTHING HERE RUNS IN DEFERRED/BOOT CONTEXT, never from the
// interrupt handler: every path does synchronous control transfers.
// xhci_poll_source() is the one caller of usb_hub_service().
#include "usb.h"
#include "clocksource.h" // clocksource_delay_ms -- a delay that needs no interrupt
#include "xhci.h"
#include "xhci_regs.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"
#include "timer.h"

// driver-none: hub handling inside the xHCI driver

#define USB_HUB_MAX      4
#define USB_HUB_MAX_PORTS 15   // the route string's port field is 4 bits

// Hub class requests (bmRequestType, request).
#define HUB_GET_DESC_TYPE  0xA0   // device-to-host, class, device
#define HUB_SET_PORT_TYPE  0x23   // host-to-device, class, other
#define HUB_GET_PORT_TYPE  0xA3   // device-to-host, class, other
#define REQ_GET_STATUS     0
#define REQ_CLEAR_FEATURE  1
#define REQ_SET_FEATURE    3
#define REQ_GET_DESCRIPTOR 6
#define DESC_HUB           0x29

// Port features and wPortStatus/wPortChange bits (USB 2.0 chapter 11).
#define PORT_CONNECTION    0
#define PORT_ENABLE        1
#define PORT_RESET         4
#define PORT_POWER         8
#define C_PORT_CONNECTION  16
#define C_PORT_ENABLE      17
#define C_PORT_RESET       20
#define ST_CONNECTION      (1u << 0)
#define ST_ENABLE          (1u << 1)
#define ST_RESET           (1u << 4)
#define ST_LOW_SPEED       (1u << 9)
#define ST_HIGH_SPEED      (1u << 10)

struct usb_hub {
    uint8_t  in_use;
    uint8_t  slot;
    uint8_t  n_ports;
    uint8_t  root_port;
    uint8_t  depth;        // hubs above THIS hub
    uint8_t  speed;        // the hub's own speed
    uint8_t  tt_slot;      // the TT the hub's own chain uses (inherited)
    uint8_t  tt_port;
    uint8_t  status_ep;
    uint32_t route;
};

static struct usb_hub g_hubs[USB_HUB_MAX];

static void wait_ms(uint32_t ms) { clocksource_delay_ms(ms); }

// --- the class requests ------------------------------------------------

static int hub_get_descriptor(uint8_t slot, uint8_t *buf, uint16_t len) {
    uint8_t setup[8] = { HUB_GET_DESC_TYPE, REQ_GET_DESCRIPTOR,
                         0, DESC_HUB, 0, 0,
                         (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
    return xhci_control(slot, setup, buf, len, 1);
}

static int hub_port_feature(uint8_t slot, uint8_t req, uint8_t feature,
                            uint8_t port) {
    uint8_t setup[8] = { HUB_SET_PORT_TYPE, req, feature, 0, port, 0, 0, 0 };
    return xhci_control(slot, setup, 0, 0, 0);
}

// wPortChange in the high half, wPortStatus in the low. -1 on failure.
static int64_t hub_port_status(uint8_t slot, uint8_t port) {
    uint8_t st[4] = { 0 };
    uint8_t setup[8] = { HUB_GET_PORT_TYPE, REQ_GET_STATUS, 0, 0, port, 0, 4, 0 };
    if (xhci_control(slot, setup, st, 4, 1) < 4) return -1;
    return (int64_t)((uint32_t)st[0] | ((uint32_t)st[1] << 8) |
                     ((uint32_t)st[2] << 16) | ((uint32_t)st[3] << 24));
}

// --- attach and detach, one hub port ------------------------------------

static uint8_t child_slot_of(uint8_t hub_slot, uint8_t port) {
    for (int i = 0; i < usb_device_count(); i++) {
        const struct usb_device_info *d = usb_device_at(i);
        if (d && d->parent_slot == hub_slot && d->port == port) return d->slot;
    }
    return 0;
}

static void hub_attach_port(struct usb_hub *h, uint8_t port) {
    for (int attempt = 0; attempt < 2; attempt++) {
        if (hub_port_feature(h->slot, REQ_SET_FEATURE, PORT_RESET, port) < 0)
            return;

        // The hub reports reset completion in the port's change bits;
        // poll for it rather than sleeping the worst case.
        int64_t st = -1;
        for (int spins = 0; spins < 60; spins++) {
            wait_ms(10);
            st = hub_port_status(h->slot, port);
            if (st < 0) return;
            // Feature numbers 16..20 land on the change bits' positions
            // in the combined status dword, so this IS bit 4 of
            // wPortChange -- reset complete.
            if ((uint32_t)st & (1u << C_PORT_RESET)) break;
            if (!((uint32_t)st & ST_RESET) && ((uint32_t)st & ST_ENABLE))
                break;                        // hub cleared reset already
        }
        hub_port_feature(h->slot, REQ_CLEAR_FEATURE, C_PORT_RESET, port);
        wait_ms(20);                          // TRSTRCY, a minimum

        st = hub_port_status(h->slot, port);
        if (st < 0 || !((uint32_t)st & ST_CONNECTION)) return;   // left mid-reset
        if (!((uint32_t)st & ST_ENABLE)) continue;               // retry the reset

        uint8_t speed = ((uint32_t)st & ST_LOW_SPEED)  ? XHCI_SPEED_LOW :
                        ((uint32_t)st & ST_HIGH_SPEED) ? XHCI_SPEED_HIGH :
                                                         XHCI_SPEED_FULL;
        // The route string gains this port at the hub's own tier; the
        // TT is this hub when it is the HIGH-speed hub doing splits for
        // a low/full-speed child, and inherited otherwise (a chain of
        // full-speed hubs all shares the topmost high-speed hub's TT).
        uint32_t route = h->route |
                         ((uint32_t)(port > 15 ? 15 : port) << (4 * h->depth));
        uint8_t tt_slot = h->tt_slot, tt_port = h->tt_port;
        if (h->speed == XHCI_SPEED_HIGH && speed != XHCI_SPEED_HIGH) {
            tt_slot = h->slot;
            tt_port = port;
        }
        // `attempt` doubles as the patience flag, exactly as the root
        // port's loop uses it: fast first, careful once it has failed.
        if (usb_enumerate_device(h->root_port, port, route,
                                 (uint8_t)(h->depth + 1), speed,
                                 h->slot, tt_slot, tt_port, attempt) >= 0)
            return;
        klog_printf(KLOG_ERR "usb: hub slot %u port %u: enumeration failed%s\n",
                    h->slot, port, attempt ? "" : " -- retrying");
    }
}

static void hub_port_change(struct usb_hub *h, uint8_t port) {
    int64_t st64 = hub_port_status(h->slot, port);
    if (st64 < 0) return;
    uint32_t st = (uint32_t)st64;
    uint32_t change = st >> 16;

    if (change & ST_ENABLE)
        hub_port_feature(h->slot, REQ_CLEAR_FEATURE, C_PORT_ENABLE, port);
    if (change & ST_RESET)
        hub_port_feature(h->slot, REQ_CLEAR_FEATURE, C_PORT_RESET, port);
    if (!(change & ST_CONNECTION)) return;

    hub_port_feature(h->slot, REQ_CLEAR_FEATURE, C_PORT_CONNECTION, port);
    uint8_t old = child_slot_of(h->slot, port);
    if (old) {
        klog_printf("usb: hub slot %u port %u: device removed\n", h->slot, port);
        usb_detach_slot(old);
    }
    if (st & ST_CONNECTION) {
        wait_ms(100);                         // attach debounce, as at the root
        hub_attach_port(h, port);
    }
}

// --- the class driver's three entry points ------------------------------

int usb_hub_bind(struct usb_device_info *d) {
    if (!d) return 0;
    if (d->speed == XHCI_SPEED_SUPER) {
        klog_printf("usb: slot %u is a USB3 hub -- not supported, skipping\n",
                    d->slot);
        return 0;
    }
    // Route strings carry five tiers; a hub at depth 4 could not
    // address its children.
    if (d->depth >= 4) {
        klog_printf("usb: slot %u: hub nested too deep (%u)\n", d->slot, d->depth);
        return 0;
    }

    const struct usb_interface_info *ifc = 0;
    for (int i = 0; i < d->if_count; i++)
        if (d->ifs[i].if_class == 9 && d->ifs[i].ep) { ifc = &d->ifs[i]; break; }
    if (!ifc) return 0;

    uint8_t hd[16] = { 0 };
    int got = hub_get_descriptor(d->slot, hd, sizeof hd);
    if (got < 5) {
        klog_printf("usb: slot %u: hub descriptor unreadable (%d)\n", d->slot, got);
        return 0;
    }
    uint8_t n = hd[2];
    if (!n) return 0;
    if (n > USB_HUB_MAX_PORTS) n = USB_HUB_MAX_PORTS;

    struct usb_hub *h = 0;
    for (int i = 0; i < USB_HUB_MAX; i++)
        if (!g_hubs[i].in_use) { h = &g_hubs[i]; break; }
    if (!h) {
        klog_printf("usb: hub table full, slot %u unbound\n", d->slot);
        return 0;
    }
    k_memset(h, 0, sizeof *h);
    h->slot      = d->slot;
    h->n_ports   = n;
    h->root_port = d->root_port;
    h->depth     = d->depth;
    h->speed     = d->speed;
    h->route     = d->route;
    h->status_ep = ifc->ep;
    // Inherit the chain's TT: a full-speed hub behind a high-speed one
    // still names the topmost high-speed hub for its children's splits.
    h->tt_slot   = d->tt_slot;
    h->tt_port   = d->tt_port;

    // The controller must know this slot routes: hub flag, port count
    // and (for a high-speed hub) TT think time, applied by the
    // Configure Endpoint command the status pipe setup issues next.
    uint8_t ttt = (uint8_t)((hd[3] >> 5) & 3u);   // wHubCharacteristics [6:5]
    xhci_slot_set_hub(d->slot, n, ttt);
    if (xhci_add_interrupt_in(d->slot, ifc->ep, ifc->mps, ifc->interval) < 0) {
        klog_printf(KLOG_ERR "usb: slot %u: hub status pipe failed\n", d->slot);
        h->in_use = 0;
        return 0;
    }
    h->in_use = 1;

    // Power every port, then wait ONCE for the longest power-good the
    // hub asked for (bPwrOn2PwrGood is in 2 ms units) plus the attach
    // debounce -- per-port waits would multiply a constant.
    for (uint8_t p = 1; p <= n; p++)
        hub_port_feature(d->slot, REQ_SET_FEATURE, PORT_POWER, p);
    wait_ms((uint32_t)hd[5] * 2 + 100);

    klog_printf("usb: slot %u: hub with %u port(s)\n", d->slot, n);
    d->bound = 1;

    for (uint8_t p = 1; p <= n; p++) {
        int64_t st = hub_port_status(d->slot, p);
        if (st >= 0 && ((uint32_t)st & ST_CONNECTION))
            hub_attach_port(h, p);
    }
    return 1;
}

void usb_hub_service(void) {
    for (int i = 0; i < USB_HUB_MAX; i++) {
        struct usb_hub *h = &g_hubs[i];
        if (!h->in_use) continue;
        uint8_t bitmap[8];
        int n;
        while ((n = xhci_take_report(h->slot, h->status_ep,
                                     bitmap, sizeof bitmap)) > 0) {
            // Bit 0 is the hub's own status change; acknowledge it or
            // the hub keeps reporting it forever. Nothing here acts on
            // local-power/over-current beyond clearing them.
            if (bitmap[0] & 1u) {
                uint8_t setup[8] = { HUB_GET_DESC_TYPE, REQ_GET_STATUS,
                                     0, 0, 0, 0, 4, 0 };
                uint8_t st[4];
                (void)xhci_control(h->slot, setup, st, 4, 1);
                uint8_t clr[8] = { 0x20, REQ_CLEAR_FEATURE, 0, 0, 0, 0, 0, 0 };
                (void)xhci_control(h->slot, clr, 0, 0, 0);   // C_HUB_LOCAL_POWER
                clr[2] = 1;                                  // C_HUB_OVER_CURRENT
                (void)xhci_control(h->slot, clr, 0, 0, 0);
            }
            for (uint8_t p = 1; p <= h->n_ports; p++) {
                if (p / 8 >= (uint8_t)n) break;
                if (bitmap[p / 8] & (1u << (p % 8)))
                    hub_port_change(h, p);
            }
            if (!h->in_use) break;   // the hub detached itself mid-drain
        }
    }
}

void usb_hub_forget(uint8_t slot) {
    for (int i = 0; i < USB_HUB_MAX; i++)
        if (g_hubs[i].in_use && g_hubs[i].slot == slot)
            g_hubs[i].in_use = 0;
}
