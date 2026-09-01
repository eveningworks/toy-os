// CDC Ethernet (ECM): a USB adapter that carries raw Ethernet frames on
// two bulk endpoints, registered as a `struct net_device` like any card.
//
// THIS IS NO LONGER THE DRIVER FOR AN RTL8153. The adapter this was
// written for -- a TP-Link UE300 -- offers Realtek's own protocol first
// and CDC-ECM second, and its ECM configuration receives nothing, as
// Linux's cdc_ether confirms by failing identically. net_usb_r8153.c binds
// the vendor configuration now, and usb_enum.c hands such a device
// there instead. What is left here is the STANDARD path, for an adapter
// whose ECM works -- which is still the one worth having, because it is
// a class driver rather than a per-chip register map.
//
// THREE THINGS LIVE IN CLASS-SPECIFIC DESCRIPTORS, which is why this
// takes the raw configuration the way sound_usb.c does: the MAC address
// (as a STRING index -- twelve hex characters, and the only place the
// address is written down), the DATA interface's number (in the Union
// functional descriptor), and the maximum segment size.
//
// AND THE DATA ENDPOINTS ARE IN AN ALTERNATE SETTING, alt 0 carrying
// none, exactly as an AudioStreaming interface does. A driver that
// binds alt 0 configures nothing and receives nothing.
#include "usb.h"
#include "xhci.h"
#include "xhci_regs.h"
#include "netdev.h"
#include "pmm.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"
#include "errno.h"
#include "driver.h" // driver_bound() -- `lsdrv`

#define CDC_CLASS            0x02
#define CDC_SUB_ECM          0x06
#define CDC_DATA_CLASS       0x0A
#define DESC_CS_INTERFACE    0x24
#define DESC_INTERFACE       0x04
#define DESC_ENDPOINT        0x05
#define CDC_FUNC_UNION       0x06
#define CDC_FUNC_ETHERNET    0x0F
#define REQ_SET_INTERFACE    0x0B
#define TYPE_OUT_STD_IF      0x01
#define TYPE_OUT_CLASS_IF    0x21

// SetEthernetPacketFilter (ECM 6.2.4). NOT OPTIONAL IN PRACTICE: the
// device decides what to pass up, and one that has never been told
// delivers NOTHING -- four receive buffers posted, zero completions, and
// an adapter that transmits perfectly while appearing to be on a dead
// network. Linux's cdc_ether sends it at open for the same reason.
#define REQ_SET_ETH_FILTER   0x43
#define ETH_FILTER_PROMISCUOUS 0x01
#define ETH_FILTER_DIRECTED  0x04
#define ETH_FILTER_BROADCAST 0x08
#define ETH_FILTER_MULTICAST 0x10

// Buffers, in frames of their own. Four each way: a receive ring deep
// enough that a burst is not dropped while the drain is elsewhere, and
// a transmit ring deep enough that a send does not have to block.
#define NET_BUFS      4
#define NET_BUF_SIZE  2048     // NET_FRAME_MAX rounded up

struct ecm_dev {
    uint8_t in_use;
    uint8_t slot;
    uint8_t data_ifnum, data_alt;
    uint8_t ep_in, ep_out, ep_notify;
    uint16_t mps;

    uint64_t mem_phys;
    uint8_t *rx[NET_BUFS];
    uint8_t *tx[NET_BUFS];
    uint64_t rx_phys[NET_BUFS], tx_phys[NET_BUFS];
    // Written by the completion callback (the event drain) and read by
    // transmit() (a syscall or the stack), so volatile -- a send that
    // reads a stale busy flag either drops a frame it could have sent or
    // overwrites one still on the wire.
    volatile uint8_t tx_busy[NET_BUFS];
    uint8_t tx_next;

    struct net_device dev;
};

static struct ecm_dev g_ecm;

// --- descriptor parsing ------------------------------------------------

struct ecm_info {
    uint8_t  ctrl_ifnum;    // the request above is addressed HERE, not to data
    uint8_t  mac_str;       // iMACAddress; 0 when the device names none
    uint8_t  data_ifnum;
    uint8_t  data_alt;
    uint8_t  ep_in, ep_out;
    uint16_t mps;
    uint16_t segment;       // wMaxSegmentSize
    uint8_t  have_union;
    uint8_t  ep_notify;     // the interrupt IN carrying link state
    uint8_t  notify_interval;
    uint16_t notify_mps;
};

// Walks the configuration for the ECM function: the functional
// descriptors on the CONTROL interface, then the alternate setting of
// the DATA interface that actually carries endpoints. Returns 1 when it
// found a usable pair, 0 otherwise -- refused rather than guessed at.
static int ecm_parse(const uint8_t *cfg, uint32_t total, struct ecm_info *out) {
    k_memset(out, 0, sizeof *out);
    int cur_if = -1, cur_alt = -1;
    uint8_t cur_class = 0;
    uint8_t in_data = 0, in_ecm_ctrl = 0;
    uint8_t cand_in = 0, cand_out = 0;
    uint16_t cand_mps = 0;

    for (uint32_t o = 0; o + 2 <= total; ) {
        uint32_t blen = cfg[o];
        uint8_t btype = cfg[o + 1];
        if (blen < 2 || o + blen > total) return 0;

        if (btype == DESC_INTERFACE && blen >= 9) {
            cur_if = cfg[o + 2];
            cur_alt = cfg[o + 3];
            cur_class = cfg[o + 5];
            in_ecm_ctrl = (cur_class == CDC_CLASS && cfg[o + 6] == CDC_SUB_ECM);
            if (in_ecm_ctrl) out->ctrl_ifnum = (uint8_t)cur_if;
            in_data = (cur_class == CDC_DATA_CLASS);
            cand_in = cand_out = 0;
            cand_mps = 0;
        } else if (in_ecm_ctrl && btype == DESC_CS_INTERFACE && blen >= 5 &&
                   cfg[o + 2] == CDC_FUNC_UNION) {
            // bControlInterface, then the first subordinate -- which is
            // the data interface. Taken from here rather than assumed to
            // be "the next one": the spec lets them be in any order.
            out->data_ifnum = cfg[o + 4];
            out->have_union = 1;
        } else if (in_ecm_ctrl && btype == DESC_CS_INTERFACE && blen >= 13 &&
                   cfg[o + 2] == CDC_FUNC_ETHERNET) {
            out->mac_str = cfg[o + 3];
            out->segment = (uint16_t)(cfg[o + 11] | ((uint16_t)cfg[o + 12] << 8));
        } else if (in_ecm_ctrl && btype == DESC_ENDPOINT && blen >= 7 &&
                   (cfg[o + 3] & 0x03) == 3 && (cfg[o + 2] & 0x80)) {
            // The control interface's interrupt IN. This is the only
            // place the device says whether the cable is live, which is
            // the difference between "the driver cannot receive" and
            // "there is nothing to receive".
            out->ep_notify = cfg[o + 2];
            out->notify_mps = (uint16_t)((cfg[o + 4] | ((uint16_t)cfg[o + 5] << 8)) & 0x7FF);
            out->notify_interval = cfg[o + 6];
        } else if (in_data && btype == DESC_ENDPOINT && blen >= 7) {
            uint8_t addr = cfg[o + 2];
            uint8_t attr = cfg[o + 3];
            uint16_t w = (uint16_t)(cfg[o + 4] | ((uint16_t)cfg[o + 5] << 8));
            if ((attr & 0x03) == 2) {                 // bulk
                if (addr & 0x80) cand_in = addr; else cand_out = addr;
                cand_mps = (uint16_t)(w & 0x7FF);
            }
            // The alternate that has BOTH is the one to claim. Alt 0 of
            // a CDC data interface carries none by design, so a driver
            // that took the first alternate would configure nothing.
            if (cand_in && cand_out && !out->ep_in &&
                (!out->have_union || (uint8_t)cur_if == out->data_ifnum)) {
                out->data_ifnum = (uint8_t)cur_if;
                out->data_alt = (uint8_t)cur_alt;
                out->ep_in = cand_in;
                out->ep_out = cand_out;
                out->mps = cand_mps;
            }
        }
        o += blen;
    }
    return out->ep_in && out->ep_out;
}

// Twelve ASCII hex characters into six bytes. The device's own answer,
// so it is REFUSED rather than partially decoded: an adapter whose MAC
// this cannot read is one the stack would give a wrong address to.
static int parse_mac(const char *s, uint8_t mac[NET_MAC_LEN]) {
    for (int i = 0; i < NET_MAC_LEN * 2; i++) {
        char c = s[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else return 0;
        if (i & 1) mac[i / 2] = (uint8_t)(mac[i / 2] | d);
        else       mac[i / 2] = (uint8_t)(d << 4);
    }
    return s[NET_MAC_LEN * 2] == '\0';
}

// --- the data path -----------------------------------------------------

static int set_interface(uint8_t slot, uint8_t ifnum, uint8_t alt) {
    uint8_t setup[8] = { TYPE_OUT_STD_IF, REQ_SET_INTERFACE, alt, 0,
                         ifnum, 0, 0, 0 };
    return xhci_control(slot, setup, 0, 0, 0);
}

// What the device should pass up. Addressed to the CONTROL interface,
// not the data one -- the same wIndex trap the audio driver's feature
// unit has, and it fails the same way: the transfer simply does not
// work, with nothing to see.
static int set_packet_filter(uint8_t slot, uint8_t ctrl_if, uint16_t filter) {
    uint8_t setup[8] = { TYPE_OUT_CLASS_IF, REQ_SET_ETH_FILTER,
                         (uint8_t)(filter & 0xFF), (uint8_t)(filter >> 8),
                         ctrl_if, 0, 0, 0 };
    return xhci_control(slot, setup, 0, 0, 0);
}

// One received frame, FROM THE EVENT DRAIN. net_rx() is documented safe
// from an interrupt handler -- it copies into a queue and does no
// protocol work -- and re-posting touches only this endpoint's ring.
static void rx_done(void *ctx, uint64_t phys, uint32_t bytes, int ok) {
    struct ecm_dev *e = ctx;
    if (!e->in_use) return;
    if (ok && bytes >= 14 && bytes <= NET_FRAME_MAX)
        net_rx(&e->dev, (const void *)(uintptr_t)phys, bytes);
    else if (ok)
        e->dev.rx_dropped++;      // a runt or an over-long frame
    xhci_bulk_post(e->slot, e->ep_in, phys, NET_BUF_SIZE);
}

static void tx_done(void *ctx, uint64_t phys, uint32_t bytes, int ok) {
    struct ecm_dev *e = ctx;
    (void)bytes; (void)ok;
    for (int i = 0; i < NET_BUFS; i++)
        if (e->tx_phys[i] == phys) { e->tx_busy[i] = 0; return; }
}

// CDC notifications. The core calls this from net_poll(); it is the one
// thing here that reads rather than being pushed a completion.
#define CDC_NOTIFY_NETWORK_CONNECTION 0x00
#define CDC_NOTIFY_SPEED_CHANGE       0x2A

static void ecm_poll(struct net_device *dev) {
    struct ecm_dev *e = dev->drv;
    if (!e->in_use || !e->ep_notify) return;

    uint8_t buf[16];
    int n;
    while ((n = xhci_take_report(e->slot, e->ep_notify, buf, sizeof buf)) >= 8) {
        // LOGGED ONLY ON A CHANGE. This adapter repeats its speed
        // notification many times a second, and a line each was ~15 a
        // second into a ring that holds a few hundred -- the probe
        // destroying the evidence it gathers, in CLAUDE.md's words. The
        // STATE is what a reader wants, and `ifconfig` has it.
        if (buf[1] == CDC_NOTIFY_NETWORK_CONNECTION) {
            uint8_t up = buf[2];
            if (!dev->link_known || dev->link_up != up)
                klog_printf("usb-net: %s link %s\n", dev->name,
                            up ? "UP" : "down");
            dev->link_up = up;
            dev->link_known = 1;
        } else if (buf[1] == CDC_NOTIFY_SPEED_CHANGE && n >= 16) {
            uint32_t down = (uint32_t)buf[8] | ((uint32_t)buf[9] << 8) |
                            ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24);
            if (dev->link_bps != down)
                klog_printf("usb-net: %s %u bit/s\n", dev->name, down);
            dev->link_bps = down;
            // A speed notification only arrives on a live link, and some
            // adapters send it without the connection one.
            if (!dev->link_known) { dev->link_up = 1; dev->link_known = 1; }
        }
    }
}

static int ecm_transmit(struct net_device *dev, const void *frame, uint32_t len) {
    struct ecm_dev *e = dev->drv;
    if (!e->in_use) return -ENODEV;
    if (len > NET_FRAME_MAX) return -EINVAL;

    // A free buffer, or the caller is told to drop -- netdev.h's
    // contract for a full ring, and the reason this does not block.
    int slot = -1;
    for (int i = 0; i < NET_BUFS; i++) {
        int at = (e->tx_next + i) % NET_BUFS;
        if (!e->tx_busy[at]) { slot = at; break; }
    }
    if (slot < 0) return -ENOSPC;

    k_memcpy(e->tx[slot], frame, len);
    e->tx_busy[slot] = 1;
    e->tx_next = (uint8_t)((slot + 1) % NET_BUFS);
    if (xhci_bulk_post(e->slot, e->ep_out, e->tx_phys[slot], len) < 0) {
        e->tx_busy[slot] = 0;
        return -EIO;
    }
    return 0;
}

// --- binding -----------------------------------------------------------

int usb_net_bind(struct usb_device_info *info, const uint8_t *cfg,
                 uint32_t total) {
    if (!info || g_ecm.in_use) return 0;

    struct ecm_info e;
    if (!ecm_parse(cfg, total, &e)) {
        klog_printf("usb: slot %u: CDC device has no bulk data pair -- "
                    "not bound\n", info->slot);
        return 0;
    }

    struct ecm_dev *d = &g_ecm;
    k_memset(d, 0, sizeof *d);
    d->slot = info->slot;
    d->data_ifnum = e.data_ifnum;
    d->data_alt = e.data_alt;
    d->ep_in = e.ep_in;
    d->ep_out = e.ep_out;
    d->mps = e.mps;

    // THE MAC IS A STRING, and an adapter whose address cannot be read
    // is refused: giving the stack an address the hardware does not have
    // is worse than having no adapter.
    char macs[16] = {0};
    if (e.mac_str) usb_read_string(info->slot, e.mac_str, macs, sizeof macs);
    if (!parse_mac(macs, d->dev.mac)) {
        klog_printf("usb: slot %u: CDC-ECM MAC string %u unreadable (\"%s\") "
                    "-- not bound\n", info->slot, e.mac_str, macs);
        return 0;
    }

    // Two frames' worth: NET_BUFS each way at NET_BUF_SIZE.
    uint64_t need = (uint64_t)NET_BUFS * 2 * NET_BUF_SIZE;
    uint64_t pages = (need + 4095) / 4096;
    d->mem_phys = pmm_alloc_contiguous(pages);
    if (!d->mem_phys) {
        klog_write("usb: no contiguous frames for the CDC-ECM buffers\n");
        return 0;
    }
    uint8_t *base = (uint8_t *)(uintptr_t)d->mem_phys;   // identity-mapped
    for (int i = 0; i < NET_BUFS; i++) {
        d->rx[i] = base + (uint32_t)i * NET_BUF_SIZE;
        d->rx_phys[i] = d->mem_phys + (uint64_t)i * NET_BUF_SIZE;
        d->tx[i] = base + (uint32_t)(NET_BUFS + i) * NET_BUF_SIZE;
        d->tx_phys[i] = d->mem_phys + (uint64_t)(NET_BUFS + i) * NET_BUF_SIZE;
    }

    // The endpoints only exist in the alternate setting -- see the file
    // comment -- so this comes before configuring them.
    if (set_interface(info->slot, e.data_ifnum, e.data_alt) < 0) {
        klog_printf("usb: slot %u: set interface %u alt %u failed\n",
                    info->slot, e.data_ifnum, e.data_alt);
        pmm_free_contiguous(d->mem_phys, pages);
        return 0;
    }
    d->ep_notify = e.ep_notify;
    if (e.ep_notify &&
        xhci_add_interrupt_in(info->slot, e.ep_notify, e.notify_mps,
                              e.notify_interval) < 0) {
        klog_printf("usb-net: slot %u: notification endpoint 0x%02x not "
                    "configured -- link state will be unknown\n",
                    info->slot, e.ep_notify);
        d->ep_notify = 0;
    }
    if (xhci_add_bulk(info->slot, e.ep_in, e.mps, rx_done, d) < 0 ||
        xhci_add_bulk(info->slot, e.ep_out, e.mps, tx_done, d) < 0) {
        klog_printf("usb: slot %u: bulk endpoints 0x%02x/0x%02x not "
                    "configured\n", info->slot, e.ep_in, e.ep_out);
        pmm_free_contiguous(d->mem_phys, pages);
        return 0;
    }

    // TOLD WHAT TO PASS UP, before anything is posted to receive it.
    // A failure here is logged and not fatal: some adapters manage
    // without, and an interface that transmits is still worth having.
    // PROMISCUOUS included while the receive path is being proven: it is
    // the difference between "the device is filtering us out" and "the
    // transfers are not completing", and those are the two candidates.
    uint16_t want = ETH_FILTER_PROMISCUOUS | ETH_FILTER_DIRECTED |
                    ETH_FILTER_BROADCAST | ETH_FILTER_MULTICAST;
    int fr = set_packet_filter(info->slot, e.ctrl_ifnum, want);
    klog_printf("usb-net: slot %u: packet filter 0x%x on if %u -> %d\n",
                info->slot, want, e.ctrl_ifnum, fr);

    d->in_use = 1;                 // published before a completion can arrive
    d->dev.driver = "cdc-ecm";
    d->dev.mtu = e.segment > 14 ? (uint32_t)(e.segment - 14) : NET_MTU;
    if (d->dev.mtu > NET_MTU) d->dev.mtu = NET_MTU;
    d->dev.transmit = ecm_transmit;
    // Receive is push (the bulk completion calls net_rx), so poll() is
    // only the notification endpoint -- which is why it exists at all.
    d->dev.poll = e.ep_notify ? ecm_poll : 0;
    d->dev.drv = d;

    if (!net_register(&d->dev)) {
        d->in_use = 0;
        pmm_free_contiguous(d->mem_phys, pages);
        return 0;
    }

    // Every receive buffer posted before anything else: a bulk IN with
    // nothing queued drops what arrives, and an adapter on a live
    // network starts receiving broadcast traffic immediately.
    for (int i = 0; i < NET_BUFS; i++)
        xhci_bulk_post(info->slot, e.ep_in, d->rx_phys[i], NET_BUF_SIZE);

    info->bound = 1;
    DRIVER_REGISTER("cdc-ecm", "net");
    driver_bound("cdc-ecm", d->dev.name);
    klog_printf("usb: slot %u: bound as cdc-ecm, if %u alt %u, "
                "ep in 0x%02x out 0x%02x, %u B/packet, mtu %u\n",
                info->slot, e.data_ifnum, e.data_alt, e.ep_in, e.ep_out,
                e.mps, d->dev.mtu);
    klog_printf("usb-net: %02x:%02x:%02x:%02x:%02x:%02x\n",
                d->dev.mac[0], d->dev.mac[1], d->dev.mac[2],
                d->dev.mac[3], d->dev.mac[4], d->dev.mac[5]);
    return 1;
}

void usb_net_unbind(uint8_t slot) {
    struct ecm_dev *d = &g_ecm;
    if (!d->in_use || d->slot != slot) return;
    // NOTHING UNREGISTERS from the net core -- netdev.h says a device is
    // never removed, because the stack holds the pointer and there is no
    // hotplug path. So the device stays listed and its transmit refuses:
    // `ifconfig` showing a card that cannot send is a better answer than
    // a dangling pointer.
    d->in_use = 0;
    klog_printf("usb-net: %s removed -- the interface stays listed and "
                "cannot send\n", d->dev.name);
}
