#ifndef XHCI_INTERNAL_H
#define XHCI_INTERNAL_H

// The xHCI driver's own seams -- shared by its files and included by
// nothing else (kernel/include/kernel/xhci.h is the seam to the USB
// stack above). Split by concern, as Linux splits drivers/usb/host:
//
//   xhci.c        finding the controller, bringing it up, init, lsusb's dump
//   xhci_xfer.c   contexts, the command ring, transfers, endpoints and the
//                 event ring that completes them (Linux's xhci-ring.c)
//   xhci_port.c   the root ports: socket pairing, resets, recovery,
//                 attach and the deferred work (Linux's xhci-hub.c)
//   xhci_ring.c   one TRB ring, as a data structure

#include "clockevent.h" // clockevent_idle_wake_by()
#include "usb.h"
#include "xhci.h"
#include "xhci_regs.h"
#include "xhci_ring.h"
#include "pci.h"
#include "lapic.h"
#include "pci_internal.h"
#include "pmm.h"
#include "paging.h"
#include "irq.h"
#include "pic.h"
#include "input.h"
#include "clocksource.h"
#include "usb_trace.h"
#include "klog.h"
#include "fault_inject.h"
#include "kfmt.h"
#include "string.h"
#include "barrier.h"
#include "timer.h"
#include "bootstage.h"
#include "usb_hid.h"
#include "multiboot.h" // multiboot_cmdline() -- the `nousb` flag
#include "driver.h" // DRIVER_DECLARE -- `lsdrv`
#include "pci_driver.h"


// DMA objects stay below 4 GiB unless the controller says ac64 -- the
// register window itself is mapped wherever it is (paging_map_device()).
#define XHCI_ADDR_LIMIT 0x100000000ull

#define TRBS_PER_RING 256   // 4096 / sizeof(struct xhci_trb)

struct xhci_port_state {
    uint8_t connected;
    uint8_t enabled;
    uint8_t speed;
    // This port's socket has already been power-cycled since the last
    // time something on it enumerated or went away. ONE per episode: a
    // device that is simply broken would otherwise cycle, re-attach,
    // fail, and cycle again for the life of the machine.
    uint8_t power_cycled;
    // HOW MANY TIMES THIS PORT HAS EXHAUSTED ITS ATTEMPTS, cleared
    // ONLY by an enumeration that worked.
    //
    // `power_cycled` above was meant to bound the recovery and cannot,
    // because the recovery DEFEATS IT: a mux cycle disconnects the
    // device, and the detach path clears the flag -- so a device that
    // is simply not going to enumerate cycles, re-attaches, fails and
    // cycles again for the life of the machine. Measured on a USB3
    // hub's USB2 half, where the loop starved the box badly enough to
    // make the built-in keyboard unusable (docs/bugs.md).
    uint8_t giveups;
    uint8_t oc_reported;   // over-current named once, not per poll
    // Warm resets since the last enumeration that WORKED -- cleared only
    // there, as `giveups` is, for the reason above: the reset itself
    // produces a connect, so clearing on connect would loop forever on a
    // link that fails again straight after.
    uint8_t warm_tries;
    uint32_t gone_sc;      // PORTSC when the device last went away
};

// ONE PROTOCOL'S PORT RANGE, from a Supported Protocol capability.
//
// **THE CAPABILITY GIVES RANGES, NOT PAIRS.** Compatible Port Offset
// and Count say "USB 2.0 is ports 1..11" and "USB 3.0 is 12..15" and
// nothing more -- there is no field anywhere in xHCI saying which USB2
// port is the same SOCKET as which USB3 one. See companion_port().
struct xhci_proto_range {
    uint8_t major;   // 2 or 3
    uint8_t first;   // 1-based port number
    uint8_t count;
};

struct xhci_hc {
    const struct pci_device *pci;
    volatile uint8_t *cap;
    volatile uint8_t *op;
    volatile uint8_t *rt;
    volatile uint8_t *db;

    uint32_t max_slots;
    uint32_t max_ports;
    uint32_t max_intrs;
    uint8_t  csz64;          // HCCPARAMS1.CSZ: 64-byte contexts
    // Where the USB Legacy Support capability sits, in bytes from the
    // capability base, or 0 for a controller that has none (QEMU).
    // Found by the walk, used by the handoff.
    uint32_t legsup_off;
    uint32_t cap_len;    // bytes BAR0 decodes: the bound on every register offset
    uint8_t  ac64;
    uint32_t page_size;

    uint8_t  irq;            // INTx line, 0 when polled or on MSI
    // The MSI vector, or 0 when this controller is on a pin. The two are
    // mutually exclusive by construction -- pci_msi_enable() disables
    // INTx -- and `irq` is what lsdev and the HID sources mirror, so it
    // is left 0 rather than made to mean two things.
    uint8_t  msi_vector;
    uint8_t  msix;           // 1 when that vector came from MSI-X, not MSI
    uint8_t  ppc;            // HCCPARAMS1.PPC: ports need PORTSC.PP set
    uint8_t  present;
    uint8_t  running;

    // Deferred root-port work, one bit per port (0-based). Set by the
    // event dispatch, consumed by xhci_deferred_work() -- enumeration
    // is synchronous control transfers, and running those inside the
    // event drain would deadlock on the single-consumer guard.
    // EVERY update goes through pend_set()/pend_clear(): the interrupt
    // sets bits while the deferred work clears others, and a plain |= or
    // &= there loses whichever landed between its load and its store.
    volatile uint32_t attach_pending;
    volatile uint32_t detach_pending;
    // A USB3 port whose link failed (xhci_portsc_needs_warm()): warm-reset
    // it from the deferred work, which can wait for the reset to finish.
    volatile uint32_t warm_pending;
    // kernel.usb_reset, one bit per port. DEFERRED for the same reason
    // hot-plug is, plus a sharper one: the tunable is written from a
    // SYSCALL, which runs with interrupts off, and reset_port()'s
    // recovery wait spins on coarse_ticks() -- a counter the timer
    // interrupt advances. Done inline it never returns, which is how it
    // froze a laptop the first time it was tried.
    volatile uint32_t diag_reset_pending;
    // kernel.usb_hcreset / the give-up chain's last lever. DEFERRED for
    // BOTH reasons above at once: it is written from a syscall, and it
    // re-enumerates, which is synchronous control transfers that need
    // the events a disabled interrupt cannot deliver. Run inline it
    // stops dead on the first port that HAS a device -- measured, and
    // it looked exactly like the controller failing to come back.
    volatile uint32_t hcreset_pending;
    // kernel.usb_replug, same deferral and for the same reason.
    // Separate from the reset: they are different levers and the point
    // of having both is to tell which one a device needs.
    volatile uint32_t diag_power_pending;

    struct xhci_ring cmd;
    struct xhci_ring evt;

    volatile uint64_t      *dcbaa;
    struct xhci_erst_entry *erst;

    uint32_t events_seen;
    uint32_t irqs_seen;
    // Set the first time a command timed out and the ring was reported
    // and abort-restarted. ONCE per controller: the failure this exists
    // for times out every later command too, and repeating the report
    // would flood the klog ring that holds the evidence.
    uint8_t  cmd_recovered;
    // Command completions that named a TRB nobody was waiting for --
    // see the CMD_COMPLETION arm of xhci_service(). Non-zero means a
    // late completion arrived from a command this driver had already
    // given up on, which is the shape docs/bugs.md's cascade needs.
    uint32_t cmd_stale;
    uint32_t xfer_ok;
    uint32_t xfer_bad;
    uint32_t xfer_orphan;
    uint32_t last_bad_code;
    uint32_t ep_recoveries;
    uint32_t timeouts;          // completions that never arrived
    uint8_t  timeout_reported;  // ...and whether one has been logged
    // GIVEN UP ON. Every re-init has been spent and commands still do
    // not complete, so there is nothing left to try and every further
    // attempt only takes the CPU away from the rest of the machine.
    uint8_t  wedged;

    struct xhci_port_state ports[XHCI_MAX_PORTS];

    // The USB2 and USB3 port ranges. Two, because a controller declares
    // one Supported Protocol capability per USB major version; a third
    // would be a generation this driver does not know and is ignored
    // rather than guessed at.
    struct xhci_proto_range usb2, usb3;
    // EVERY USB3 port, from every Supported Protocol range with major 3;
    // `usb3` keeps one range, for companion_port(), and a controller may
    // list 3.0 and 3.1 separately.
    uint32_t usb3_ports;
};

// One addressed device. The contexts are the controller's view of it,
// and ep0 is the transfer ring every control request rides on.
struct xhci_slot {
    uint8_t  in_use;
    uint8_t  port;          // ROOT port; a hub child's own speed differs
    uint8_t  speed;         // the DEVICE's speed, for endpoint intervals
    void    *in_ctx;        // Input Context: control + slot + endpoints
    uint64_t in_ctx_phys;
    void    *out_ctx;       // Device Context, written BY the controller
    uint64_t out_ctx_phys;
    struct xhci_ring ep0;
    uint8_t  ep0_dead;      // a control transfer timed out (xhci_slot_usable())
};

extern struct xhci_hc g_xhci;

_Static_assert(XHCI_MAX_PORTS <= 32, "a port's pending bit must fit its uint32_t mask");

static inline void pend_set(volatile uint32_t *mask, uint32_t bit) {
    __atomic_fetch_or(mask, 1u << bit, __ATOMIC_SEQ_CST);
}
static inline void pend_clear(volatile uint32_t *mask, uint32_t bit) {
    __atomic_fetch_and(mask, ~(1u << bit), __ATOMIC_SEQ_CST);
}
extern struct xhci_slot g_xhci_slots[XHCI_MAX_SLOTS + 1];   // slot ids are 1-based

// Where a synchronous waiter picks up its answer. The event ring has
// ONE consumer at a time (see the guard in xhci_service), so a
// completion is recorded here and the waiter spins on `done` rather
// than popping the ring itself and racing the interrupt handler.
struct xhci_completion {
    volatile uint8_t done;
    uint64_t trb;           // which TRB this completes
    uint32_t code;          // completion code
    uint32_t residual;      // bytes NOT transferred
    uint8_t  slot;
    // THE WHOLE TD, not just the TRB carrying IOC. A control transfer
    // that FAILS is reported against the stage that failed -- the Data
    // Stage, commonly -- and the Status Stage TRB this waiter was
    // armed on is then never executed at all. Waiting for it alone is
    // a 2-million-poll hang per refused request, and a permanently
    // halted endpoint after it. Zeroed when the wait completes, so a
    // late event cannot be mistaken for the NEXT transfer's.
    uint64_t ring_lo, ring_hi;
    // A SHORT DATA STAGE IS NOT THE END OF A CONTROL TRANSFER: its event
    // records how much less arrived, and the Status Stage's event, which
    // follows, is what completes it (xHCI 4.10.1.1; Linux's
    // process_ctrl_td waits the same way). Taking the data stage's event
    // as the end let the Status event land on the NEXT transfer, which
    // then "completed" before its data came -- every second USB string
    // read on real hardware, while QEMU posts both events at once.
    uint8_t data_short;
};
extern volatile struct xhci_completion g_xhci_cmd_done;
extern volatile struct xhci_completion g_xhci_xfer_done;

// --- MMIO -------------------------------------------------------------
//
// `volatile` here is not decoration and it is not about caching: without
// it GCC hoists a status read out of a polling loop and spins on a stale
// register forever. Copied in shape from virtio_pci.c's accessors.
static inline uint32_t mr32(volatile uint8_t *p, uint32_t off) {
    return *(volatile uint32_t *)(p + off);
}
static inline void mw32(volatile uint8_t *p, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(p + off) = v;
}
static inline uint8_t mr8(volatile uint8_t *p, uint32_t off) {
    return *(volatile uint8_t *)(p + off);
}

// A 64-bit register is written as TWO 32-bit stores, low half first.
// A controller may declare a maximum access width and latch the whole
// register on the high write, so the order is part of the contract
// rather than an optimisation. virtqueue.c's common_w64() is the same
// rule for the same reason; CRCR, DCBAAP, ERSTBA and ERDP all need it.
static inline void mw64(volatile uint8_t *p, uint32_t off, uint64_t v) {
    *(volatile uint32_t *)(p + off)     = (uint32_t)(v & 0xFFFFFFFFu);
    *(volatile uint32_t *)(p + off + 4) = (uint32_t)(v >> 32);
}


// --- PORTSC, through one door ----------------------------------------
//
// THE SINGLE MOST COMMONLY SHIPPED xHCI BUG lives in this register.
// Seven of its bits are RW1C, and PED (bit 1) is write-1-to-DISABLE. So
// the obvious read-modify-write --
//
//     portsc = read(); portsc |= PR; write(portsc);
//
// -- disables the port AND clears every change bit that happened to
// read as 1, losing the connect event you were about to act on. Nothing
// in this driver writes PORTSC any other way.
static inline void portsc_rmw(uint32_t port, uint32_t set, uint32_t clear,
                       uint32_t rw1c_ack) {
    uint32_t v = mr32(g_xhci.op, XHCI_PORTSC(port));
    v &= ~(uint32_t)XHCI_PORTSC_RW1C;   // do not clear what we did not mean to
    v &= ~(uint32_t)XHCI_PORTSC_PED;    // ...and do not disable the port
    v &= ~clear;                        // ...and only what the caller named
    v |= set;
    v |= rw1c_ack;                      // only the change bits named here
    mw32(g_xhci.op, XHCI_PORTSC(port), v);
}

static inline void portsc_write(uint32_t port, uint32_t set, uint32_t rw1c_ack) {
    portsc_rmw(port, set, 0, rw1c_ack);
}


// --- the endpoint table (xhci_xfer.c) ------------------------------------

#define EP_DEPTH     16
#define EP_SLOT_SIZE 64     // comfortably over any HID boot report

struct xhci_ep {
    uint8_t  in_use;
    uint8_t  slot;
    uint8_t  ep_addr;
    uint16_t mps;
    struct xhci_ring ring;
    uint8_t *buf;                       // EP_DEPTH slices of EP_SLOT_SIZE
    uint64_t buf_phys;
    uint8_t  buf_of_trb[TRBS_PER_RING]; // which slice a given TRB filled
    // Completed reports, oldest first. Written by the event dispatch
    // (interrupt context), read by xhci_take_report().
    volatile uint8_t  ready[EP_DEPTH];
    volatile uint16_t ready_len[EP_DEPTH];
    uint8_t  next_take;
    // A STALL or transaction error HALTS the endpoint, and a halted
    // endpoint ignores its doorbell forever -- so without recovery one
    // marginal packet kills the mouse silently. Marked by the event
    // dispatch, recovered by xhci_deferred_work().
    volatile uint8_t halted;
    uint8_t  recover_tries;

    // BULK, either direction. Like the isochronous case and unlike the
    // interrupt one, THE DRIVER OWNS THE BUFFERS -- an Ethernet frame is
    // 1514 bytes and the interrupt path's slices are 64, so there is no
    // shared geometry to borrow. Completion is a callback carrying the
    // byte count and the buffer that was posted.
    uint8_t  is_bulk;
    void (*bulk_done)(void *ctx, uint64_t phys, uint32_t bytes, int ok);
    void    *bulk_ctx;
    uint64_t buf_of_trb_phys[TRBS_PER_RING];  // which buffer a TRB carried
    uint32_t bulk_len_of_trb[TRBS_PER_RING];  // ...and how much was asked for

    // ISOCHRONOUS OUT. Nothing above is used by one: the driver owns
    // the buffer (there is no `buf` to allocate) and takes its
    // completions through the callback rather than by polling ready[].
    uint8_t  is_iso;
    void   (*iso_done)(void *ctx, uint32_t bytes);
    void    *iso_ctx;
    volatile uint32_t iso_underruns;
    volatile uint32_t iso_refused;   // posts turned away by a full ring
    // Which TRBs the CALLER asked a completion for. The kernel adds its
    // own every ISO_FORCE_IOC (see xhci_isoch_post) and must not report
    // those, or a driver counting completions per group miscounts.
    uint8_t  iso_want_ioc[TRBS_PER_RING];
    uint16_t iso_since_ioc;
};

// THE RING'S ROOM IS ONLY LEARNED FROM EVENTS, so a caller that posts
// with no IOC for a whole ring would leave it "full" for ever -- the
// guard itself becoming the silence it exists to prevent. Well above
// any group a driver here uses, so a well-behaved one never sees it.
#define ISO_FORCE_IOC 64

// HID interfaces (a composite receiver is two on one device) plus one
// status-change endpoint per hub.
#define MAX_EPS 10
extern struct xhci_ep g_xhci_eps[MAX_EPS];

// --- contexts, bounded waits, the Intel routing registers --------------------

static inline uint32_t ctx_size(void) { return g_xhci.csz64 ? 64u : 32u; }

// Entry `i` of a context block. Entry 0 of an Input Context is the
// Input Control Context, entry 1 is the Slot Context, and entry
// 1 + DCI is an endpoint. In a Device (output) Context entry 0 is the
// Slot Context, so the two are offset by one -- which is why the caller
// names the entry rather than the endpoint.
static inline volatile uint32_t *ctx_at(void *base, uint32_t i) {
    return (volatile uint32_t *)((uint8_t *)base + (uint64_t)i * ctx_size());
}

// Endpoint 0 is DCI 1; an IN endpoint N is DCI 2N+1, an OUT endpoint N
// is DCI 2N. This is the number the doorbell wants and the index the
// contexts are laid out by.
static inline uint32_t dci_of(uint8_t ep_addr) {
    uint8_t num = ep_addr & 0x0F;
    if (!num) return 1;
    return (uint32_t)(2 * num + ((ep_addr & 0x80) ? 1 : 0));
}

struct xhci_wait { uint32_t spins; uint64_t deadline; };

#define INTEL_XUSB2PR    0xD0   // xHC USB2 port routing (write)
#define INTEL_XUSB2PRM   0xD4   // ...and which ports may be routed (read)
#define INTEL_USB3_PSSEN 0xD8   // SuperSpeed enable (write)
#define INTEL_USB3PRM    0xDC   // ...and which may be enabled (read)

// --- what crosses files ------------------------------------------------------

extern struct input_source g_xhci_source;
extern struct input_source g_xhci_dead_source;
int xhci_reset_controller(void);
void xhci_program_rings(void);
const char *xhci_speed_name(uint8_t s);
void *xhci_alloc_frame(uint64_t *out_phys);
void xhci_log_socket_map(void);
void xhci_power_ports(void);
void xhci_scan_ports(void);
int xhci_recover_halted(uint8_t slot, uint32_t dci, struct xhci_ring *ring);
void xhci_ep_post(struct xhci_ep *e, uint8_t slice);
void xhci_wait_start(struct xhci_wait *w, uint32_t ms);
int xhci_wait_over(struct xhci_wait *w);
void xhci_irq_handler(uint64_t *regs);
void xhci_poll_source(void);
void xhci_dead_heartbeat(void);

#endif
