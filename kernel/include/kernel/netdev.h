#ifndef KERNEL_NETDEV_H
#define KERNEL_NETDEV_H

#include <stdint.h>

// The network-device class -- N cards, one stack, the same registry
// shape block_device, display_driver, sound_device and clocksource
// use. A DRIVER owns its hardware: descriptor rings, the interrupt,
// and putting one Ethernet frame on the wire. The CORE (net.c) owns
// the device table, the names, the receive queue and the counters.
// The stack above (kernel/net/) owns protocols and never learns what
// a NIC is.
//
// PLURAL BY CONSTRUCTION, unlike block_device's singular "active"
// device: every entry point here takes a `struct net_device *`, and
// the L3 configuration below is PER DEVICE. A machine with two cards
// on two subnets is the shape this is built for, so the one-card case
// carries a device pointer it could have done without -- the price of
// not having to revisit every signature later.
//
// A DEVICE IS STATIC STORAGE THE DRIVER OWNS. The core keeps the
// pointer, never a copy: a driver updating dev->mac or a counter is
// seen by the stack immediately. net_unregister() takes it back out of
// the table; the storage itself is never freed, so a driver may
// re-register the same struct when its hardware comes back.

#define NET_NAME_MAX   16    // "net-718ebf", or a name from /etc/net.conf
#define NET_LOC_MAX    12    // "pci3.0", "usb2.13" -- NUL included
#define NET_MAX_DEVS   4
#define NET_MTU        1500  // payload; the frame is this + 14
#define NET_FRAME_MAX  1518  // 14 header + MTU + 4 FCS, the classic cap
#define NET_MAC_LEN    6

struct net_device {
    // ASSIGNED BY net_register(), NEVER THE DRIVER. A NAME IS AN
    // IDENTITY AND IDENTITY FOLLOWS THE CARD, so it is made from the
    // MAC -- "net-718ebf", the last three bytes, which are the vendor's
    // own serial for that card (the first three are the vendor). Moving
    // an adapter to a different socket therefore renames NOTHING, which
    // is where systemd's enp3s0 scheme was rejected: it encodes the
    // socket, so the same USB adapter in another port is a new
    // interface. A friendlier name comes from /etc/net.conf
    // (net_config.c); this is the fallback and the bootstrap.
    //
    // Nothing derives it from an INDEX any more. Probe order used to
    // decide it, which meant a name was handed to the next card once
    // the first went away -- and sockets bind by NAME (socket.c), so a
    // reused name silently moves a bound socket onto other hardware.
    char name[NET_NAME_MAX];

    // WHERE THE DEVICE IS, filled by the driver BEFORE net_register():
    // "pci3.0", "usb13". REPORTED, never part of the name -- `netctl`
    // prints it so a card can still be found physically, and it changes
    // freely when one is moved. Empty when the driver does not know.
    char location[NET_LOC_MAX];

    const char *driver;       // "e1000", "virtio-net" -- for lsdev/netctl
    uint8_t mac[NET_MAC_LEN];
    uint32_t mtu;             // 0 at registration means NET_MTU

    // LINK STATE, when the driver can know it. `link_known` is the
    // third answer: a card with no way to ask is not a card whose cable
    // is unplugged, and `netctl` must not claim otherwise. Bits per
    // second because that is what the wire negotiated, not what the bus
    // could carry -- a gigabit adapter on USB 2 still says 1000000000.
    uint8_t  link_known;
    uint8_t  link_up;
    uint32_t link_bps;

    // ADMINISTRATIVELY DOWN -- Linux's IFF_UP, inverted so a driver's
    // zeroed struct is up. Set from ring 3 (SYS_NET_CONFIG's
    // NET_IFC_DOWN), never by a driver: carrier is `link_up`, this is
    // somebody's decision. Down, net_tx() refuses and net_rx() drops.
    uint8_t  admin_down;

    // --- driver ops ---------------------------------------------------

    // Put ONE complete Ethernet frame (destination MAC first, no FCS --
    // the hardware appends that) on the wire. Returns 0, or a negative
    // errno; -ENOSPC when the transmit ring is full, which net_tx()
    // retries for a few milliseconds and then counts as a drop. A ring
    // whose completions arrive by interrupt must REAP them itself before
    // saying full (rtl_usb.c), or that retry never sees a slot free.
    int (*transmit)(struct net_device *dev, const void *frame, uint32_t len);

    // Optional. Drain the receive ring, calling net_rx() per frame.
    // Set only when the device has no usable interrupt line -- the core
    // calls it from net_poll(), the way input_source.poll works. A
    // driver with an IRQ leaves this NULL and calls net_rx() from its
    // handler instead.
    void (*poll)(struct net_device *dev);
    // How often poll() must run with no interrupt to prompt it -- a
    // tickless idle wakes for it. 0: it only needs the wakes it gets.
    uint32_t poll_ms;

    void *drv;                // driver private state

    // --- per-device L3 configuration (host byte order) ----------------
    //
    // Zero means unconfigured, and an unconfigured device still
    // receives: ARP replies to nothing and IP drops, but the counters
    // move, which is what makes "the cable is live, the address is
    // wrong" diagnosable from `netctl` alone.
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;

    // --- counters, the core's ----------------------------------------
    uint64_t rx_packets, rx_bytes, rx_dropped;
    uint64_t tx_packets, tx_bytes, tx_dropped;
};

// Called by a driver once its device can transmit and its interrupt is
// live. The core assigns `name` (from /etc/net.conf if that names this
// card, else from its MAC) and takes the pointer.
// Returns 1, or 0 when the table is full or a device with that name is
// already registered -- the second is a double-register bug, and being
// refused is how the driver hears about it.
int net_register(struct net_device *dev);

// Fill in `location` from where the device actually is. A driver calls
// the one for its bus before net_register(); anything else leaves the
// field empty and `netctl` says nothing about where the card sits.
void net_location_pci(struct net_device *dev, uint8_t bus, uint8_t device,
                      uint8_t function);
void net_location_usb(struct net_device *dev, uint8_t root_port, uint8_t port);

// Give a registered device a different name. Refuses an empty, an
// over-long, a badly-shaped or an already-taken name -- a name reaches
// a lease filename and a socket binding, so it may not contain a space,
// an '=' or a '/'. Returns 1 when the device now holds `name`.
int net_rename(struct net_device *dev, const char *name);

// The device is GONE -- a USB adapter unplugged. It leaves the table,
// its ARP entries are flushed, frames of its already sitting in the
// receive queue are dropped, and its address is cleared, so nothing is
// left holding a pointer to a card that is not there. `lsdrv` stops
// naming it. Unknown devices are ignored, so a driver may call this
// unconditionally on teardown.
//
// THE NAME IS NOT FREED FOR REUSE, because it was never an index: the
// same hardware in the same socket registers under the same name
// again, and different hardware could not have had it in the first
// place.
void net_unregister(struct net_device *dev);

int net_device_count(void);
struct net_device *net_device_at(int index);
struct net_device *net_device_by_name(const char *name);

// The device outbound traffic uses when nothing named one: the first
// registered device that has an IP. NULL on a machine with no
// configured card.
struct net_device *net_default_device(void);

// A driver hands the core one received frame. SAFE FROM AN INTERRUPT
// HANDLER and does nothing but copy into the receive queue -- no
// protocol parsing, no kmalloc, no filesystem, which is the whole
// reason the queue exists (Linux's netif_rx and the NAPI split, and
// this kernel's own rule about what a non-reentrant subsystem may be
// reached from). An over-long frame or a full queue is counted as a
// drop and discarded, never truncated.
void net_rx(struct net_device *dev, const void *frame, uint32_t len);

// Transmit through the core, so the counters and the length check live
// in one place. Drivers do NOT call each other's transmit op.
int net_tx(struct net_device *dev, const void *frame, uint32_t len);

// THE CHANNEL A BLOCKED READER PARKS ON, and there is exactly one for
// the whole stack rather than one per socket. The waker is net_rx(),
// which runs in a driver's INTERRUPT and has not parsed anything -- it
// cannot know which socket the frame is for, so it wakes everybody and
// each woken reader runs the stack itself and looks again. With eight
// sockets that costs a spurious wake or two; what it buys is that
// protocol code still never runs at interrupt time, which is the
// property this whole receive path is built around.
const void *net_wait_chan(void);

// Run the stack over everything net_rx() has queued, and poll any
// device that has no interrupt. Called from scheduler_idle() and from
// the socket syscalls -- anywhere EXCEPT an interrupt handler.
void net_poll(void);

// Boot probe for each driver, called from kernel_main() beside the
// other PCI-scanning drivers. Finding no card is the common case.
void net_init(void);      // the core: the table and the queue


// QUERY_NETDEV's provider (`netctl` reads the table through it).
void net_query_init(void);

#endif
