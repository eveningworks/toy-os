// The network-device core: the table, the names, and the receive queue.
//
// Everything here is about keeping two things apart that a NIC driver
// naturally wants to mix: the INTERRUPT, which must be short and may
// not allocate, parse or touch the filesystem, and the STACK, which
// does all three. A driver hands frames to net_rx() and returns; the
// protocols run later from net_poll(). That is Linux's netif_rx/NAPI
// split, and this kernel needs it for a sharper reason than Linux
// does -- kmalloc here is not interrupt-safe and the filesystem is not
// re-entrant, so parsing a packet in an ISR is not merely rude.
//
// THE QUEUE IS STATIC AND COPIES. A pool of frame-sized slots in .bss,
// written by the producer (an ISR) and read by the consumer
// (net_poll()), single-core lock-free on the usual head/tail rule:
// only the producer moves tail, only the consumer moves head. Copying
// costs a memcpy per frame and buys the driver its DMA buffer back
// immediately, which is what stops a slow consumer stalling the ring.
#include "clocksource.h"
#include "barrier.h"      // cpu_relax(), in net_tx()'s wait
#include "clockevent.h" // clockevent_idle_wake_by() -- polled devices and TCP timers
#include "netdev.h"
#include "net.h"   // eth_input(), tcp_tick()
#include "klog.h"
#include "scheduler.h"   // scheduler_wake() -- interrupt-safe, by contract
#include "syscall_abi.h" // SYS_RETRY
#include "kfmt.h"   // klog_printf
#include "string.h"
#include "errno.h"
#include "usb.h"  // xhci_companion_port() -- one socket, two port numbers
#include "driver.h" // driver_bound() -- `lsdrv`
#include "initcall.h"

// driver-none: the net class registry itself

#define NET_RX_QUEUE 256

static struct net_device *g_devs[NET_MAX_DEVS];
static int g_count;

struct rx_slot {
    struct net_device *dev;
    uint32_t len;
    uint8_t data[NET_FRAME_MAX];
};

static struct rx_slot g_rxq[NET_RX_QUEUE];

// The address IS the channel (api/scheduler.h). A byte of its own
// rather than reusing the queue's, so the channel keeps meaning
// "something arrived" if the queue is ever replaced.
static const char g_net_chan;
static volatile uint32_t g_rx_head;  // consumer
static volatile uint32_t g_rx_tail;  // producer (interrupt context)

// Set by net_init(). The table below is only meaningful after it.
static int g_inited;

// "pci<bus>.<device>", with ".<function>" only when it is not 0 -- and
// this is REPORTED, not part of the name. See netdev.h.
void net_location_pci(struct net_device *dev, uint8_t bus, uint8_t device,
                      uint8_t function) {
    if (!dev) return;
    if (function)
        k_snprintf(dev->location, NET_LOC_MAX, "pci%u.%u.%u", bus, device, function);
    else
        k_snprintf(dev->location, NET_LOC_MAX, "pci%u.%u", bus, device);
}

// "usb<root port>", with ".<port>" when the device hangs off a hub.
//
// **THE SOCKET, WHERE THE CONTROLLER HAS ONE.** A USB3 socket is two
// port numbers -- one in the controller's USB2 range and one in its
// USB3 range -- and which of them a device lands on depends on whether
// its SuperSpeed link trained. Reporting the raw port therefore names
// ONE ADAPTER two different things across reboots (measured: a UE300
// read `usb3` at full speed and `usb14` at 5 Gb/s), which reads as the
// adapter having moved. The socket is stable, so it leads; the port is
// kept beside it because it is what every other USB line in the log
// says.
void net_location_usb(struct net_device *dev, uint8_t root_port, uint8_t port) {
    if (!dev) return;
    if (port && port != root_port) {
        k_snprintf(dev->location, NET_LOC_MAX, "usb%u.%u", root_port, port);
        return;
    }
    int peer = xhci_companion_port(root_port);
    if (peer)
        k_snprintf(dev->location, NET_LOC_MAX, "usb%u+%u", root_port, peer);
    else
        k_snprintf(dev->location, NET_LOC_MAX, "usb%u", root_port);
}

// THE DEFAULT NAME IS THE CARD'S OWN SERIAL. The last three bytes of a
// MAC are the part the vendor assigns per device; the first three are
// the vendor. So this is an identity the card carries with it rather
// than a truncation chosen for length.
static void default_name(struct net_device *dev) {
    k_snprintf(dev->name, NET_NAME_MAX, "net-%02x%02x%02x",
               dev->mac[3], dev->mac[4], dev->mac[5]);
}

// A name reaches a DHCP lease filename (/var/dhcp-<name>.lease) and a
// socket's device binding, so the characters that would break either
// are refused here rather than at whichever of them noticed first.
static int name_is_usable(const char *name) {
    if (!name || !name[0]) return 0;
    uint32_t n = (uint32_t)k_strlen(name);
    if (n >= NET_NAME_MAX) return 0;
    for (uint32_t i = 0; i < n; i++) {
        char c = name[i];
        if (c == ' ' || c == '\t' || c == '=' || c == '/' || c == '\n') return 0;
    }
    return 1;
}

int net_rename(struct net_device *dev, const char *name) {
    if (!dev || !name_is_usable(name)) return 0;
    if (k_strcmp(dev->name, name) == 0) return 1;
    struct net_device *clash = net_device_by_name(name);
    if (clash && clash != dev) {
        klog_printf(KLOG_ERR "net: %s cannot be renamed to %s -- that name is taken\n",
                    dev->name, name);
        return 0;
    }
    // lsdrv records a binding by name, so the old one has to go before
    // the new one arrives or the driver lists the card twice.
    driver_unbound(dev->driver, dev->name);
    k_strlcpy(dev->name, name, NET_NAME_MAX);
    driver_bound(dev->driver, dev->name);
    return 1;
}

int net_register(struct net_device *dev) {
    if (!dev || !dev->transmit) return 0;
    // REFUSED, LOUDLY, rather than accepted into a table net_init() is
    // about to zero. A USB Ethernet adapter enumerating before the core
    // did exactly that: it registered, said so, and then did not exist.
    // CLAUDE.md's rule is that using a subsystem before its init() is a
    // hard failure, and a silent one is worse than a panic.
    if (!g_inited) {
        klog_printf(KLOG_ERR "net: %s registered BEFORE net_init() -- refused; "
                    "fix the order in kernel_main()\n",
                    dev->driver ? dev->driver : "a device");
        return 0;
    }
    if (g_count >= NET_MAX_DEVS) {
        klog_printf("net: no room for another device (max %d)\n", NET_MAX_DEVS);
        return 0;
    }
    // THE BOOTSTRAP NAME ONLY. /etc/net.conf is read by /bin/netd, which
    // renames through SYS_NET_RENAME once the filesystem is up -- naming
    // POLICY is not the kernel's, the same call this project already
    // made for NTP, DHCP and DNS.
    default_name(dev);

    // A name already in the table means the same device was registered
    // twice -- which is what an unplug with no net_unregister() used to
    // produce, one adapter listed under two names. Refused rather than
    // accepted, so the driver hears about its own bug.
    if (net_device_by_name(dev->name)) {
        klog_printf(KLOG_ERR "net: %s is already registered -- refused\n", dev->name);
        return 0;
    }

    driver_bound(dev->driver, dev->name);
    if (!dev->mtu) dev->mtu = NET_MTU;

    g_devs[g_count++] = dev;
    klog_printf("net: %s: %s %02x:%02x:%02x:%02x:%02x:%02x mtu %u\n",
                dev->name, dev->driver ? dev->driver : "?",
                dev->mac[0], dev->mac[1], dev->mac[2],
                dev->mac[3], dev->mac[4], dev->mac[5], dev->mtu);
    return 1;
}

// Compacting under net_poll()'s device walk is safe on this
// uniprocessor for the reason input_unregister_source() gives: the
// unregister runs FROM a driver's own teardown, so the walk merely sees
// a shorter list on its next index. What it must not leave behind is a
// pointer to the departed device, and there are exactly two -- frames
// already in the receive queue, and the ARP entries it resolved.
void net_unregister(struct net_device *dev) {
    int idx = -1;
    for (int i = 0; i < g_count; i++) if (g_devs[i] == dev) { idx = i; break; }
    if (idx < 0) return;

    // Published slots are the consumer's; the producer only ever moves
    // tail, so marking these is safe against an interrupt still filling
    // the ring behind us. net_poll() skips a zero-length slot.
    for (uint32_t i = g_rx_head; i != g_rx_tail; i = (i + 1) % NET_RX_QUEUE)
        if (g_rxq[i].dev == dev) g_rxq[i].len = 0;

    arp_flush_device(dev);
    dev->ip = dev->netmask = dev->gateway = 0;
    dev->link_known = dev->link_up = 0;
    dev->link_bps = 0;

    driver_unbound(dev->driver, dev->name);
    for (int i = idx; i + 1 < g_count; i++) g_devs[i] = g_devs[i + 1];
    g_count--;
    klog_printf("net: %s removed\n", dev->name);
}

const void *net_wait_chan(void) { return &g_net_chan; }

int net_device_count(void) { return g_count; }

struct net_device *net_device_at(int index) {
    if (index < 0 || index >= g_count) return 0;
    return g_devs[index];
}

struct net_device *net_device_by_name(const char *name) {
    if (!name) return 0;
    for (int i = 0; i < g_count; i++)
        if (!k_strcmp(g_devs[i]->name, name)) return g_devs[i];
    return 0;
}

struct net_device *net_default_device(void) {
    for (int i = 0; i < g_count; i++)
        if (g_devs[i]->ip && !g_devs[i]->admin_down) return g_devs[i];
    return 0;
}

void net_rx(struct net_device *dev, const void *frame, uint32_t len) {
    if (!dev || !frame) return;
    if (dev->admin_down) return;   // switched off: not ours to count, as Linux
    if (len < ETH_HDR_LEN || len > NET_FRAME_MAX) { dev->rx_dropped++; return; }

    uint32_t tail = g_rx_tail;
    uint32_t next = (tail + 1) % NET_RX_QUEUE;
    if (next == g_rx_head) { dev->rx_dropped++; return; }  // full: drop the NEW one

    struct rx_slot *s = &g_rxq[tail];
    s->dev = dev;
    s->len = len;
    k_memcpy(s->data, frame, len);
    g_rx_tail = next;   // publish last: the consumer must not see a half-filled slot

    dev->rx_packets++;
    dev->rx_bytes += len;

    // WAKE HERE, IN THE INTERRUPT, and do nothing else. A reader parked
    // on an empty socket has no other way to learn that a frame exists:
    // net_poll() runs from scheduler_idle(), which is not reached while
    // anything else is runnable, so without this a receive could sleep
    // through a packet that had already arrived. The woken process runs
    // the stack in its OWN context (sys_recvfrom calls net_poll first),
    // which is what keeps protocol code out of here.
    //
    // scheduler_wake() is safe from an interrupt handler by contract --
    // it only flips state and writes an already-saved trapframe. Nothing
    // else in this function may grow to be less careful.
    scheduler_wake(net_wait_chan(), SYS_RETRY);
}

// How long a full transmit ring is waited on before the frame is dropped.
// A fragmented datagram is a burst of up to 45 frames, which outruns a
// 16-slot ring, and one fragment lost loses the whole datagram. Linux
// queues behind a stopped driver (the qdisc); this waits for the ring,
// which the device's completion interrupt drains in well under this.
#define NET_TX_FULL_WAIT_NS 5000000ull

int net_tx(struct net_device *dev, const void *frame, uint32_t len) {
    if (!dev || !frame) return -EINVAL;
    if (dev->admin_down) return -ENETDOWN;
    if (len < ETH_HDR_LEN || len > dev->mtu + ETH_HDR_LEN) return -EINVAL;

    int rc = dev->transmit(dev, frame, len);
    if (rc == -ENOSPC) {
        uint64_t until = clocksource_now_ns() + NET_TX_FULL_WAIT_NS;
        while (rc == -ENOSPC && clocksource_now_ns() < until) {
            cpu_relax();
            rc = dev->transmit(dev, frame, len);
        }
    }
    if (rc < 0) { dev->tx_dropped++; return rc; }
    dev->tx_packets++;
    dev->tx_bytes += len;
    return 0;
}

void net_poll(void) {
    // Re-entrancy guard: a ring-3 process is preemptible inside a
    // syscall, and both a socket call and scheduler_idle() reach here.
    // Two consumers on one head index would hand the same frame to the
    // stack twice. (ata_cache.c's idle hook has the same guard.)
    static int in_poll;
    if (in_poll) return;
    in_poll = 1;

    uint64_t now = clocksource_now_ns();
    for (int i = 0; i < g_count; i++) {
        if (!g_devs[i]->poll) continue;
        g_devs[i]->poll(g_devs[i]);
        if (g_devs[i]->poll_ms)
            clockevent_idle_wake_by(now + (uint64_t)g_devs[i]->poll_ms * 1000000ull);
    }

    // Retransmission and any segment ARP deferred. BEFORE the drain, so
    // a process woken by its own retransmit deadline does the work it
    // woke up for even when no frame arrived.
    tcp_tick();
    ipv4_frag_expire(now);   // lazily -- see ipv4_frag.c
    // An ORPHANED connection has no process parked on its deadline, so
    // the idle loop is what has to wake for it.
    uint64_t tcp_due = tcp_next_deadline();
    if (tcp_due) clockevent_idle_wake_by(tcp_due);

    while (g_rx_head != g_rx_tail) {
        struct rx_slot *s = &g_rxq[g_rx_head];
        // Zero length is a frame whose device was unregistered while it
        // sat here -- dropped rather than parsed against a card that is
        // gone. net_rx() never queues one, having refused it as short.
        if (s->len) eth_input(s->dev, s->data, s->len);
        g_rx_head = (g_rx_head + 1) % NET_RX_QUEUE;
    }

    in_poll = 0;
}

void net_init(void) {
    g_count = 0;
    g_rx_head = g_rx_tail = 0;
    g_inited = 1;
}
INITCALL(net_init, INIT_CORE);
