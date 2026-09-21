// The xHCI host controller: find it, bring it up, and watch its root
// ports. See kernel/include/kernel/usb.h for what the USB stack as a
// whole is and is not, and kernel/include/kernel/xhci.h for the seam
// between this file and the USB-semantics half above it.
//
// EVERY DMA OBJECT HERE IS ITS OWN 4 KiB FRAME, AND THAT IS THE DESIGN.
// The spec wants 64-byte alignment on the rings and contexts, and it
// forbids a ring segment from crossing a 64 KiB boundary. This kernel's
// allocator only promises 4 KiB alignment -- but a 4 KiB-aligned 4 KiB
// block satisfies 64-byte alignment trivially and cannot cross any
// boundary of 4 KiB or more, so the rule never bites. The cost is
// waste: ~36 KiB for a single keyboard, which is nothing here.
//
// DO NOT "OPTIMISE" TWO OBJECTS INTO ONE FRAME. The moment a second
// object starts at an arbitrary offset inside a frame, its 64-byte
// alignment becomes something a human has to maintain, and a ring
// straddling 64 KiB becomes possible again. Both failures are silent.
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

DRIVER_DECLARE("xhci", "usb", "USB 3 xHCI host controller");

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
    volatile uint32_t attach_pending;
    volatile uint32_t detach_pending;
    // kernel.usb_reset, one bit per port. DEFERRED for the same reason
    // hot-plug is, plus a sharper one: the tunable is written from a
    // SYSCALL, which runs with interrupts off, and reset_port()'s
    // recovery wait spins on pit_ticks() -- a counter the timer
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

    struct xhci_port_state ports[XHCI_MAX_PORTS];

    // The USB2 and USB3 port ranges. Two, because a controller declares
    // one Supported Protocol capability per USB major version; a third
    // would be a generation this driver does not know and is ignored
    // rather than guessed at.
    struct xhci_proto_range usb2, usb3;
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
};

static struct xhci_hc g_hc;
static struct xhci_slot g_slots[XHCI_MAX_SLOTS + 1];   // slot ids are 1-based

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
};
static volatile struct xhci_completion g_cmd_done;
static volatile struct xhci_completion g_xfer_done;

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

// --- completion codes, as a TABLE ------------------------------------
//
// A table rather than a switch: the set runs past 35 entries, which is
// what tools/check_dispatch.py fails a build over, and this is data
// either way -- the same call the PCI class table already made.
static const char *const CC_NAMES[] = {
    "invalid", "success", "data buffer error", "babble detected",
    "usb transaction error", "trb error", "stall error", "resource error",
    "bandwidth error", "no slots available", "invalid stream type",
    "slot not enabled", "endpoint not enabled", "short packet",
    "ring underrun", "ring overrun", "vf event ring full", "parameter error",
    "bandwidth overrun", "context state error", "no ping response",
    "event ring full", "incompatible device", "missed service error",
    "command ring stopped", "command aborted", "stopped",
    "stopped - length invalid", "stopped - short packet",
    "max exit latency too large", "reserved", "isoch buffer overrun",
    "event lost", "undefined error", "invalid stream id", "secondary bandwidth error",
    "split transaction error",
};

const char *xhci_completion_name(uint32_t code) {
    if (code < sizeof(CC_NAMES) / sizeof(CC_NAMES[0])) return CC_NAMES[code];
    return "vendor-defined";
}

static const char *speed_name(uint8_t s) {
    switch (s) {  // dispatch-ok: bounded by the xHCI default speed ID set
        case XHCI_SPEED_FULL:  return "full-speed";
        case XHCI_SPEED_LOW:   return "low-speed";
        case XHCI_SPEED_HIGH:  return "high-speed";
        case XHCI_SPEED_SUPER: return "super-speed";
        default:               return "unknown-speed";
    }
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
static void portsc_rmw(uint32_t port, uint32_t set, uint32_t clear,
                       uint32_t rw1c_ack) {
    uint32_t v = mr32(g_hc.op, XHCI_PORTSC(port));
    v &= ~(uint32_t)XHCI_PORTSC_RW1C;   // do not clear what we did not mean to
    v &= ~(uint32_t)XHCI_PORTSC_PED;    // ...and do not disable the port
    v &= ~clear;                        // ...and only what the caller named
    v |= set;
    v |= rw1c_ack;                      // only the change bits named here
    mw32(g_hc.op, XHCI_PORTSC(port), v);
}

static void portsc_write(uint32_t port, uint32_t set, uint32_t rw1c_ack) {
    portsc_rmw(port, set, 0, rw1c_ack);
}

// --- discovery --------------------------------------------------------

// Every USB host controller is class 0x0C/0x03; prog_if tells the four
// generations apart, and naming the one found beats silence -- on a
// machine with only an EHCI controller the log then says why USB did
// not come up.
static const struct pci_match usb_matches[] = { PCI_MATCH_CLASS(0x0C, 0x03, PCI_ANY) };

static int is_xhci(const struct pci_device *d) {
    if (d->prog_if == 0x30) return 1;
    const char *kind = d->prog_if == 0x00 ? "UHCI" :
                       d->prog_if == 0x10 ? "OHCI" :
                       d->prog_if == 0x20 ? "EHCI" : "unknown";
    klog_printf("usb: %s controller at %02x:%02x.%u (prog_if %#x) -- not supported\n",
                kind, d->bus, d->device, d->function, d->prog_if);
    return 0;
}

// `usbtrace` on the boot line prints a line per bring-up STEP.
//
// Bring-up is a dozen MMIO writes with no output between them, so a
// machine that hangs in there shows its last message as whatever came
// BEFORE the whole sequence -- which is the extended-capability walk,
// and points at the wrong place. There is no serial on a laptop, so the
// screen is the only channel and the trace has to be on it.
//
// Off by default because a healthy boot would carry a dozen lines
// forever, and matched as a whole word like `nousb` beside it.
static int usb_traced(void) {
    static int cached = -1;
    if (cached >= 0) return cached;
    cached = 0;
    const char *cmdline = multiboot_cmdline();
    if (!cmdline) return 0;
    for (const char *p = cmdline; (p = k_strstr(p, "usbtrace")) != 0; p += 8) {
        if (p != cmdline && p[-1] != ' ') continue;
        char after = p[8];
        if (after == 0 || after == ' ') { cached = 1; break; }
    }
    return cached;
}

#define USBT(...) do { if (usb_traced()) klog_printf(__VA_ARGS__); } while (0)

// Walks the extended capability chain, logging every id.
//
// This costs twenty lines and answers two questions for free that would
// otherwise each need their own investigation: whether this controller
// implements USB Legacy Support (xECP id 1, the BIOS handoff -- QEMU
// does not, which is why the handoff is not built), and where the
// Supported Protocol capabilities put the USB2 and USB3 port ranges.
// How far into the register window the capability chain may reach.
//
// **THE HOP COUNT IS NOT A BOUND ON THE ADDRESS.** Each `next` is up to
// 255 DWORDS, so 64 hops can walk 65 KB -- past the end of a typical
// 64 KiB xHCI BAR and into MMIO nothing decodes. On real hardware that
// read does not politely return garbage: it is an unclaimed cycle, and
// one Intel laptop hangs on it hard enough that the boot stops mid-log
// line with no further output at all.
//
// The bound is the BAR's probed size (g_hc.cap_len); 64 KiB is the
// fallback for a controller whose BAR the probe could not size, and
// the all-ones check below is what stops the runaway either way.
#define XHCI_XECP_FALLBACK_LEN 0x10000u

static void walk_xecp(uint32_t hcc1) {
    uint32_t off = XHCI_HCC1_XECP(hcc1) * 4;   // xECP is in DWORDS
    int hops = 0;
    USBT("usb: trace: xecp walk from +0x%x\n", off);
    while (off && hops++ < 64) {               // bounded: the chain is device-supplied
        if (off + 4 > g_hc.cap_len) {
            klog_printf("usb: xECP chain leaves the register window at +0x%x "
                        "(BAR0 is 0x%x bytes) -- stopping\n", off, g_hc.cap_len);
            return;
        }
        // BEFORE the read, not only after: if the read itself is what
        // hangs -- an unclaimed MMIO cycle on real hardware -- then a
        // line printed afterwards never appears, and the trace names
        // the previous offset instead of the fatal one.
        USBT("usb: trace: xecp read +0x%x\n", off);
        uint32_t v  = mr32(g_hc.cap, off);
        USBT("usb: trace: xecp +0x%x = 0x%x\n", off, v);

        // ALL-ONES IS WHAT AN UNCLAIMED READ LOOKS LIKE when the bus is
        // well behaved, and it is a runaway if believed: id 0xFF is not
        // a capability and next 0xFF strides another 1020 bytes, so the
        // walk marches off through 64 hops of nothing.
        if (v == 0xFFFFFFFFu) {
            klog_printf("usb: xECP read at +0x%x came back all-ones "
                        "-- stopping\n", off);
            return;
        }
        uint32_t id = XHCI_XECP_ID(v);
        if (id == XHCI_XECP_ID_PROTO && off + 12 > g_hc.cap_len) {
            klog_printf("usb: xECP supported-protocol at +0x%x is cut off by the "
                        "register window -- stopping\n", off);
            return;
        }
        if (id == XHCI_XECP_ID_PROTO) {
            // Supported Protocol: name, then the port range it covers.
            uint32_t name  = mr32(g_hc.cap, off + 4);
            uint32_t ports = mr32(g_hc.cap, off + 8);
            uint32_t first = ports & 0xFFu, cnt = (ports >> 8) & 0xFFu;
            uint32_t major = (v >> 24) & 0xFFu;
            klog_printf("usb:  xECP %u supported-protocol USB %u.%u, ports %u..%u\n",
                        id, major, (v >> 16) & 0xFFu,
                        first, first + cnt - 1);
            // KEPT, where it used to be logged and dropped. The ranges
            // are what companion_port() pairs, and without them a
            // device that moves between the two port numbers of one
            // socket looks like it moved sockets.
            struct xhci_proto_range *r = major == 2 ? &g_hc.usb2
                                       : major == 3 ? &g_hc.usb3 : 0;
            if (r && first && cnt) {
                r->major = (uint8_t)major;
                r->first = (uint8_t)first;
                r->count = (uint8_t)cnt;
            }
            (void)name;
        } else if (id == XHCI_XECP_ID_LEGACY) {
            g_hc.legsup_off = off;   // the handoff below needs to find it again
            klog_printf("usb:  xECP %u usb-legacy-support (bios-owned=%u)\n",
                        id, (v & XHCI_LEGSUP_BIOS_OWNED) ? 1u : 0u);
        } else {
            klog_printf("usb:  xECP %u\n", id);
        }
        uint32_t next = XHCI_XECP_NEXT(v);
        if (!next) break;
        off += next * 4;
    }
}

// Ask the BIOS for the controller, then silence its SMIs.
//
// **THIS IS WHY ONE LAPTOP HUNG, and the driver did not do it at all.**
// The comment where the capability is logged said the handoff was not
// built because QEMU does not implement the capability -- true, and it
// meant the one machine that DOES implement it drove a controller its
// firmware still owned. Every register write then trapped into the
// BIOS's SMM handler, and the boot froze at an arbitrary point: mid-log
// line, in a different place each time, moving when unrelated logging
// changed the timing. Exactly what a CPU that has entered SMM and not
// come back looks like from outside.
//
// Linux does this in a PCI quirk (quirk_usb_handoff_xhci) that runs
// BEFORE its driver binds, and the ordering is the point: the request
// has to come before the reset, not after, or the reset is itself a
// write to somebody else's device.
//
// Returns 1 if the OS owns the controller afterwards. A REFUSAL IS NOT
// FATAL: Linux forces the bit clear on timeout and carries on, because
// a BIOS that will not answer has usually stopped caring rather than
// stayed active, and the SMI disable below is what actually protects
// us either way.
// --- bounded waits ----------------------------------------------------
//
// A POLL COUNT IS NOT A TIMEOUT, and on real hardware that is the
// difference between a device that enumerates and one that does not.
// 2,000,000 spins is however long 2,000,000 spins take on THIS CPU --
// on a 2.2 GHz Broadwell laptop that expired before a camera and a USB
// hub had answered, and the same devices enumerated fine on a slower
// boot. Every failure reported exactly "2000001 polls", which is the
// ceiling rather than a device saying no.
//
// So: a real deadline WHERE ONE CAN BE TRUSTED, and the poll count
// where it cannot. `clocksource_deadline_capable()` is the test, and it
// is a property of the source rather than its name -- the TSC advances
// with interrupts off, the PIT source (a count the timer INTERRUPT
// increments) does not, and a deadline off a stopped clock never
// expires. That was the original objection to deadlines here and it was
// right; what it did not have was a way to ask.
//
// The poll count stays as the backstop on the deadline path too, at a
// much higher ceiling: a clocksource that lies still has to terminate
// the loop somehow.
#define XHCI_WAIT_SPIN_CEILING (XHCI_POLL_BACKSTOP * 64u)

// BURN TIME, on a clock that advances where we are standing.
//
// Both callers below are MINIMA the USB spec owes a device -- reset
// recovery and the attach debounce -- so they burn time rather than
// detect anything. They used pit_ticks() with the note that it was
// "the only 10 ms-granularity source here", which stopped being true
// when the clocksource arrived: clocksource_now_ns() is finer AND, when
// it is deadline-capable, advances with interrupts off, where a counter
// the timer INTERRUPT increments does not.
//
// That difference is not academic. It is why `kernel.usb_reset` froze a
// laptop on its first outing: a syscall runs with interrupts off, and
// the pit_ticks() loop there could never end. The PIT path is kept for
// a machine whose clocksource cannot be trusted with a deadline, and
// there it carries the old precondition -- interrupts on.
// The policy moved to clocksource_delay_ms(), which five drivers had
// each hand-rolled. This name stays because usb.h publishes it.
void xhci_delay_ms(uint32_t ms) {
    clocksource_delay_ms(ms);
}

struct xhci_wait { uint32_t spins; uint64_t deadline; };

static void xhci_wait_start(struct xhci_wait *w, uint32_t ms) {
    w->spins = 0;
    w->deadline = clocksource_deadline_capable()
                ? clocksource_now_ns() + (uint64_t)ms * 1000000ull
                : 0;
}

// 1 when the caller should give up.
static int xhci_wait_over(struct xhci_wait *w) {
    w->spins++;
    if (w->deadline)
        return clocksource_now_ns() >= w->deadline ||
               w->spins > XHCI_WAIT_SPIN_CEILING;
    return w->spins > XHCI_POLL_BACKSTOP;
}

// --- the Intel port mux -----------------------------------------------
//
// ON AN INTEL PCH THE USB2 PORTS ARE SHARED WITH AN EHCI COMPANION, AND
// THE FIRMWARE USUALLY LEAVES THEM ROUTED TO IT. The xHC then comes up
// perfectly, reports its full port count, and sees NOTHING on any of
// them -- including the machine's own internal devices. That is not a
// hypothetical: it is the Lenovo Yoga 500-15IBD, where `lsusb` found
// nothing at all while an EHCI sat at 00:1d.0 holding the ports.
//
// Linux does this as a second, separate quirk beside the handoff
// (usb_enable_intel_xhci_ports() in pci-quirks.c) and the split is
// worth keeping in mind: the handoff decides WHO owns the controller,
// this decides WHICH PORTS the controller can see. A machine can need
// one, both or neither.
//
// IT GATES ITSELF ON THE HARDWARE. Each routing register has a MASK
// register beside it saying which ports are switchable at all, and the
// write is that mask, verbatim. A controller without the mux reads a
// zero mask and the write is a no-op -- which is why this needs no
// device-id list to maintain, and why the machine that does NOT have an
// EHCI is a usable control for it.
//
// WRITING THE MASK CANNOT UN-ROUTE A PORT, and that is measured rather
// than assumed: on a machine whose ports were already on the xHC,
// XUSB2PR read 0x7ff against a mask of 0x4ff, took the 0x4ff write, and
// read back 0x7ff. The bits outside the mask are read-only, which is
// what makes Linux's plain write of the mask safe rather than a way to
// switch working ports away.
// Also used by intel_mux_cycle() below, which is why these are up here
// rather than inside the one function that used to want them.
#define INTEL_XUSB2PR    0xD0   // xHC USB2 port routing (write)
#define INTEL_XUSB2PRM   0xD4   // ...and which ports may be routed (read)
#define INTEL_USB3_PSSEN 0xD8   // SuperSpeed enable (write)
#define INTEL_USB3PRM    0xDC   // ...and which may be enabled (read)

static void intel_port_mux(const struct pci_device *d) {
    if (d->vendor_id != 0x8086) return;

    // SUPERSPEED FIRST, then USB2 -- Linux's order. A USB3 port whose
    // SS half is not enabled falls back to its USB2 half, so doing it
    // the other way round can route a port to the xHC at full speed and
    // leave it there.
    uint32_t ss_mask = pci_config_read32(d, INTEL_USB3PRM);
    uint32_t ss_before = pci_config_read32(d, INTEL_USB3_PSSEN);
    if (ss_mask) pci_config_write32(d, INTEL_USB3_PSSEN, ss_mask);

    uint32_t hs_mask = pci_config_read32(d, INTEL_XUSB2PRM);
    uint32_t hs_before = pci_config_read32(d, INTEL_XUSB2PR);
    if (hs_mask) pci_config_write32(d, INTEL_XUSB2PR, hs_mask);

    // Reported even when nothing moved: a no-op and a controller whose
    // registers did not take the write look identical otherwise, and
    // this is the only evidence available on a machine with no serial
    // console.
    klog_printf("usb: intel port mux: usb2 0x%x -> 0x%x (mask 0x%x)\n",
                hs_before, pci_config_read32(d, INTEL_XUSB2PR), hs_mask);
    klog_printf("usb: intel port mux: usb3 0x%x -> 0x%x (mask 0x%x)\n",
                ss_before, pci_config_read32(d, INTEL_USB3_PSSEN), ss_mask);
}

static int legacy_handoff(void) {
    if (!g_hc.legsup_off) return 1;   // no such capability; nothing owns it

    uint32_t legsup = mr32(g_hc.cap, g_hc.legsup_off);
    if (legsup & XHCI_LEGSUP_BIOS_OWNED) {
        klog_printf("usb: BIOS owns the controller -- requesting handoff\n");
        mw32(g_hc.cap, g_hc.legsup_off, legsup | XHCI_LEGSUP_OS_OWNED);

        // The spec allows a BIOS a full second to let go.
        struct xhci_wait w; xhci_wait_start(&w, 1000);
        while (mr32(g_hc.cap, g_hc.legsup_off) & XHCI_LEGSUP_BIOS_OWNED) {
            if (xhci_wait_over(&w)) {
                klog_printf("usb: BIOS did not release the controller -- "
                            "taking it anyway\n");
                // Force both bits: claim ownership and drop the BIOS's
                // claim. Linux does the same, and leaving the BIOS bit
                // set would leave the firmware believing it still has a
                // device this driver is about to reset.
                uint32_t v = mr32(g_hc.cap, g_hc.legsup_off);
                v |= XHCI_LEGSUP_OS_OWNED;
                v &= ~XHCI_LEGSUP_BIOS_OWNED;
                mw32(g_hc.cap, g_hc.legsup_off, v);
                break;
            }
            cpu_relax();
        }
    }

    // SMIs OFF, AND THE STATUS BITS CLEARED, whether or not the handoff
    // above was granted. This is the half that stops the traps: an
    // enable left set means the next ordinary register write is another
    // trip into firmware.
    uint32_t ctl = mr32(g_hc.cap, g_hc.legsup_off + XHCI_LEGCTLSTS);
    ctl &= XHCI_LEGACY_DISABLE_SMI;
    ctl |= XHCI_LEGACY_SMI_EVENTS;
    mw32(g_hc.cap, g_hc.legsup_off + XHCI_LEGCTLSTS, ctl);

    uint32_t now = mr32(g_hc.cap, g_hc.legsup_off);
    klog_printf("usb: legacy handoff done (legsup 0x%x, os-owned=%u)\n",
                now, (now & XHCI_LEGSUP_OS_OWNED) ? 1u : 0u);
    return (now & XHCI_LEGSUP_BIOS_OWNED) ? 0 : 1;
}

// --- bring-up ---------------------------------------------------------

// One 4 KiB frame, zeroed, identity-mapped. Returns 0 on failure.
static void *alloc_frame(uint64_t *out_phys) {
    uint64_t p = pmm_alloc_contiguous(1, PMM_ZONE_DMA32);
    if (!p) return 0;
    if (p + 4096 > XHCI_ADDR_LIMIT) {
        // Cannot happen for a DMA32 frame, but the cast below would
        // silently truncate if the zone above ever changed.
        pmm_free_contiguous(p, 1);
        return 0;
    }
    void *v = (void *)(uintptr_t)p;
    k_memset(v, 0, 4096);
    *out_phys = p;
    return v;
}

static int reset_controller(void) {
    // Stop first. A controller that is running must halt before reset,
    // and HCH is how it says it has.
    uint32_t cmd = mr32(g_hc.op, XHCI_USBCMD);
    if (cmd & XHCI_CMD_RS) {
        mw32(g_hc.op, XHCI_USBCMD, cmd & ~(uint32_t)XHCI_CMD_RS);
        // The spec gives the controller 16 ms to halt.
        struct xhci_wait w; xhci_wait_start(&w, 100);
        while (!(mr32(g_hc.op, XHCI_USBSTS) & XHCI_STS_HCH)) {
            if (xhci_wait_over(&w)) {
                klog_printf("usb: controller will not halt (usbsts 0x%x)\n",
                            mr32(g_hc.op, XHCI_USBSTS));
                return 0;
            }
        }
    }

    mw32(g_hc.op, XHCI_USBCMD, XHCI_CMD_HCRST);

    // Reset is done when HCRST self-clears AND CNR clears. Waiting on
    // only the first is a real bug: the controller can drop HCRST while
    // still refusing register writes, and everything programmed in that
    // window is silently discarded.
    struct xhci_wait w; xhci_wait_start(&w, 1000);
    for (;;) {
        uint32_t c = mr32(g_hc.op, XHCI_USBCMD);
        uint32_t s = mr32(g_hc.op, XHCI_USBSTS);
        if (!(c & XHCI_CMD_HCRST) && !(s & XHCI_STS_CNR)) break;
        if (xhci_wait_over(&w)) {
            klog_printf("usb: reset did not complete (usbcmd 0x%x usbsts 0x%x)\n", c, s);
            return 0;
        }
    }
    return 1;
}

// Point the controller at the rings. Split out of setup_rings() because
// a controller RESET clears these registers while the frames behind
// them are still ours -- so a recovery re-programs and must NOT
// re-allocate. alloc_frame() identity-maps, so a ring's virtual address
// is its physical one.
static void program_rings(void) {
    xhci_ring_init(&g_hc.cmd, (void *)g_hc.cmd.trb, g_hc.cmd.phys,
                   g_hc.cmd.count, 0);
    xhci_ring_init(&g_hc.evt, (void *)g_hc.evt.trb, g_hc.evt.phys,
                   g_hc.evt.count, 1);
    g_hc.erst[0].base = g_hc.evt.phys;
    g_hc.erst[0].size = g_hc.evt.count;
    mw64(g_hc.op, XHCI_DCBAAP, (uint64_t)(uintptr_t)g_hc.dcbaa);
    mw64(g_hc.op, XHCI_CRCR, g_hc.cmd.phys | XHCI_CRCR_RCS);
    mw32(g_hc.rt, XHCI_IR0 + XHCI_ERSTSZ, 1);
    mw64(g_hc.rt, XHCI_IR0 + XHCI_ERDP, g_hc.evt.phys);
    // ERSTBA LAST: writing it arms the interrupter.
    mw64(g_hc.rt, XHCI_IR0 + XHCI_ERSTBA, (uint64_t)(uintptr_t)g_hc.erst);
}

static int setup_rings(void) {
    uint64_t dcbaa_phys = 0, cmd_phys = 0, evt_phys = 0, erst_phys = 0;

    USBT("usb: trace: allocating ring frames\n");
    g_hc.dcbaa = (volatile uint64_t *)alloc_frame(&dcbaa_phys);
    void *cmd_seg = alloc_frame(&cmd_phys);
    void *evt_seg = alloc_frame(&evt_phys);
    g_hc.erst = (struct xhci_erst_entry *)alloc_frame(&erst_phys);

    if (!g_hc.dcbaa || !cmd_seg || !evt_seg || !g_hc.erst) {
        klog_printf("usb: out of contiguous frames for the controller rings\n");
        return 0;
    }

    xhci_ring_init(&g_hc.cmd, cmd_seg, cmd_phys, TRBS_PER_RING, 0);
    xhci_ring_init(&g_hc.evt, evt_seg, evt_phys, TRBS_PER_RING, 1);

    g_hc.erst[0].base = evt_phys;
    g_hc.erst[0].size = TRBS_PER_RING;

    // Scratchpad buffers: the controller asks for N pages of its own
    // scratch, addressed through an array installed at DCBAA[0].
    //
    // QEMU asks for ZERO, so everything below is unreachable in every
    // test in this repo -- said plainly because an untested branch is a
    // guess, and this one is a guess that only real hardware can check.
    // The genuinely unbuildable case is a controller whose PAGESIZE is
    // above 4 KiB: the spec wants each buffer aligned to it, and this
    // allocator cannot promise more than 4 KiB. Refuse by name rather
    // than hand the controller a misaligned buffer.
    uint32_t hcs2 = mr32(g_hc.cap, XHCI_HCSPARAMS2);
    uint32_t spb  = XHCI_HCS2_SPB_MAX(hcs2);
    USBT("usb: trace: scratchpad wanted=%u pagesize=%u\n", spb, g_hc.page_size);
    if (spb) {
        if (g_hc.page_size > 4096) {
            klog_printf("usb: controller wants %u scratchpad pages of %u bytes;"
                        " this kernel's allocator aligns only to 4096, so a"
                        " >4 KiB page size needs an aligned allocator first\n",
                        spb, g_hc.page_size);
            return 0;
        }
        uint64_t arr_phys = 0;
        volatile uint64_t *arr = (volatile uint64_t *)alloc_frame(&arr_phys);
        if (!arr) return 0;
        if (spb > 512) spb = 512;          // one frame of pointers
        for (uint32_t i = 0; i < spb; i++) {
            uint64_t p = 0;
            if (!alloc_frame(&p)) return 0;
            arr[i] = p;
        }
        g_hc.dcbaa[0] = arr_phys;
        klog_printf("usb: %u scratchpad page(s)\n", spb);
    }

    program_rings();
    return 1;
}

// --- contexts ---------------------------------------------------------
//
// A context entry is 32 OR 64 bytes, and which one is HCCPARAMS1.CSZ.
// QEMU says 32; a great deal of real hardware says 64. Reading it wrong
// puts every field at the wrong offset and the controller reports
// nothing at all -- it simply parses garbage. So no code here indexes a
// context by a constant; everything goes through ctx_at().
static inline uint32_t ctx_size(void) { return g_hc.csz64 ? 64u : 32u; }

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

// Ringing a doorbell is what tells the controller to look at a ring.
// Slot 0 target 0 is the command ring; slot N target DCI is that
// device's endpoint.
static void ring_doorbell(uint32_t slot, uint32_t target) {
    // The ring writes must be visible before the doorbell, or the
    // controller reads a TRB that is not there yet. Free on x86, but
    // the compiler still has to be told -- see barrier.h.
    kbarrier();
    *(volatile uint32_t *)(g_hc.db + slot * 4) = target;
}

// The default max packet size for endpoint 0 at a given speed, used
// before the device descriptor has been read. A full-speed device may
// really be 8, 16, 32 or 64 and is corrected afterwards; see
// xhci_set_ep0_mps().
static uint16_t default_mps(uint8_t speed) {
    switch (speed) {  // dispatch-ok: bounded by the xHCI speed ID set
        case XHCI_SPEED_LOW:   return 8;
        case XHCI_SPEED_FULL:  return 8;
        case XHCI_SPEED_HIGH:  return 64;
        case XHCI_SPEED_SUPER: return 512;
        default:               return 8;
    }
}

// --- synchronous command and transfer submission ----------------------

// Spins until `c->done`, draining the event ring as it goes.
//
// Draining here as well as in the interrupt handler is deliberate: it
// keeps enumeration working whether or not the IRQ is live, which is
// what lets the polled fallback path be the same code. xhci_service()'s
// re-entrancy guard is what makes the two safe together.
static int wait_completion(volatile struct xhci_completion *c, const char *what) {
    // A SECOND, which is a thousandfold what a healthy control transfer
    // takes and the ceiling this driver used to hit on real hardware.
    // Linux gives its command ring five.
    struct xhci_wait w; xhci_wait_start(&w, 1000);
    while (!c->done) {
        xhci_service();
        if (xhci_wait_over(&w)) {
            klog_printf(KLOG_ERR "usb: %s timed out after %u polls (%s)\n", what, w.spins,
                        w.deadline ? "1000 ms" : "poll ceiling, no usable clock");
            return -1;
        }
    }
    return 0;
}

// WHAT THE COMMAND RING LOOKS LIKE WHEN A COMMAND DID NOT COME BACK.
//
// A boot on the ASUS lost every USB device because exactly one command
// ever completed and every command after it timed out, while port-change
// events kept arriving -- so the event ring was alive and the command
// ring was not (docs/bugs.md). Nothing in the log said which, because
// nothing read CRCR. This is the line that tells the two apart: CRR is
// read-only and answers "is the ring still running", and the enqueue
// index plus cycle state say where the DRIVER thinks it is, which is
// the half the controller cannot report.
static void cmd_ring_report(const char *when) {
    uint64_t crcr = (uint64_t)mr32(g_hc.op, XHCI_CRCR) |
                    ((uint64_t)mr32(g_hc.op, XHCI_CRCR + 4) << 32);
    klog_printf(KLOG_ERR
                "usb: cmd ring %s: crcr 0x%llx (CRR=%u RCS=%u) enq %u cyc %u "
                "base 0x%llx usbsts 0x%x\n",
                when, (unsigned long long)crcr,
                (unsigned)((crcr & XHCI_CRCR_CRR) ? 1 : 0),
                (unsigned)(crcr & XHCI_CRCR_RCS),
                g_hc.cmd.enqueue, g_hc.cmd.cycle,
                (unsigned long long)g_hc.cmd.phys, mr32(g_hc.op, XHCI_USBSTS));
}

// ABORT THE COMMAND RING AND PUT IT BACK, which is the only lever a
// driver has over one that has stopped: the controller owns the dequeue
// pointer, so the ring cannot simply be rewound.
//
// **THIS RUNS ONLY AFTER A COMMAND HAS ALREADY TIMED OUT**, i.e. on a
// path where the boot is otherwise lost -- every later command queues
// behind the dead one and every port gives up. That is what makes it
// safe to attempt: the state it is trying to repair is one where doing
// nothing is known to end with no USB at all.
//
// Returns 1 if the ring is running again.
static int cmd_ring_recover(void) {
    // CA is write-1 and self-clearing, and the RCS bit must be written
    // with it: this register's low bits are the ring's cycle state, and
    // dropping them here would tell the controller to expect the wrong
    // one when it restarts.
    mw64(g_hc.op, XHCI_CRCR, g_hc.cmd.phys |
                              (g_hc.cmd.cycle ? XHCI_CRCR_RCS : 0) |
                              XHCI_CRCR_CA);

    // The controller stops asynchronously and says so with a Command
    // Completion event (CC 24, Command Ring Stopped). Waiting on CRR
    // rather than on that event is deliberate -- the event may be the
    // very thing that is not arriving, and CRR is a register read that
    // cannot be starved.
    struct xhci_wait w; xhci_wait_start(&w, 50);
    for (;;) {
        xhci_service();                       // drain whatever it does post
        uint64_t crcr = (uint64_t)mr32(g_hc.op, XHCI_CRCR) |
                        ((uint64_t)mr32(g_hc.op, XHCI_CRCR + 4) << 32);
        if (!(crcr & XHCI_CRCR_CRR)) break;
        if (xhci_wait_over(&w)) {
            klog_printf(KLOG_ERR "usb: command ring will not stop -- "
                                  "abort ignored\n");
            return 0;
        }
    }

    // A stopped ring's dequeue pointer is undefined, so the ring is
    // rebuilt from scratch and CRCR re-pointed at it. xhci_ring_init()
    // re-zeroes the segment and restores cycle 1, which is what the
    // controller is told to expect on the next line.
    xhci_ring_init(&g_hc.cmd, (void *)g_hc.cmd.trb, g_hc.cmd.phys,
                   g_hc.cmd.count, 0);
    mw64(g_hc.op, XHCI_CRCR, g_hc.cmd.phys | XHCI_CRCR_RCS);
    cmd_ring_report("after abort");
    return 1;
}

int xhci_selftest_cmd_recovery(void);   // below cmd_submit; see its comment

// Enqueues one command, rings doorbell 0 and waits for its Command
// Completion event. Returns the completion code; `out_slot` receives
// the slot id the controller assigned, when the command allocates one.
static int cmd_submit(uint64_t param, uint32_t control, uint8_t *out_slot) {
    g_cmd_done.done = 0;
    uint64_t at = xhci_ring_push(&g_hc.cmd, param, 0, control);
    g_cmd_done.trb = at;
    ring_doorbell(0, 0);

    // The injector fakes the WAIT, not the controller: the command was
    // really posted above and the controller really will run it. That
    // is enough to drive the report and the abort/restart below against
    // a live ring, and is NOT a reproduction of the stalled-ring fault
    // (fault_inject.h says so at more length).
    if (fault_should_fail_usb_command() ||
        wait_completion(&g_cmd_done, "command") < 0) {
        usb_trace(USB_TR_CMD, 0, 0, XHCI_TRB_TYPE(control), 0xFFu); // timeout
        // ONCE PER CONTROLLER, not once per command: the failure mode
        // this exists for produces a timeout on every command for the
        // rest of the boot, and a report plus an abort on each of them
        // would be the probe outrunning the log that CLAUDE.md warns
        // about -- the evidence destroyed by the instrument.
        if (!g_hc.cmd_recovered) {
            cmd_ring_report("timeout");
            // 1 = the ring is running again, 2 = the abort did not take.
            // Recording WHICH, not merely that this ran: a flag set
            // before the attempt would be satisfied by an abort that
            // failed, and the self-test below would then pass on a
            // controller left exactly as broken as it found it.
            g_hc.cmd_recovered = cmd_ring_recover() ? 1 : 2;
            if (g_hc.cmd_recovered == 1)
                klog_printf(KLOG_ERR "usb: command ring aborted and "
                                      "restarted after a timeout\n");
        }
        return -XHCI_CC_INVALID - 1;
    }
    if (out_slot) *out_slot = g_cmd_done.slot;
    usb_trace(USB_TR_CMD, 0, g_cmd_done.slot,
              XHCI_TRB_TYPE(control), g_cmd_done.code);
    return (int)g_cmd_done.code;
}

// DRIVE THE COMMAND-RING RECOVERY ON PURPOSE, and then prove the ring
// still works. Called by the KTEST in xhci_cmd_test.c.
//
// The fault it exists for has only ever been seen on the bare-metal
// ASUS and never once under QEMU, so without this the abort/restart
// would ship to the one machine whose NETWORK IS A USB DEVICE having
// never executed. What this can and cannot show is worth being exact
// about: the injector fakes the driver's WAIT, so this exercises the
// report, the abort, the restart and whether the ring is usable
// afterwards -- against a live controller -- and it does NOT reproduce
// a genuinely stalled ring.
//
// A No-Op command (TRB type 23) is the probe both times: it is the one
// command with no arguments, no side effects and nothing to clean up.
//
// Returns 1 pass, 0 fail, -1 no controller to test.
int xhci_selftest_cmd_recovery(void) {
    if (!g_hc.op) return -1;

    uint8_t saved = g_hc.cmd_recovered;
    g_hc.cmd_recovered = 0;             // the guard is once-per-boot; re-arm it

    fault_fail_next_usb_commands(1);
    (void)cmd_submit(0, XHCI_TRB_SET_TYPE(XHCI_TRB_NOOP_CMD), 0);
    fault_fail_next_usb_commands(0);    // a test disarms what it arms

    int fired = (g_hc.cmd_recovered == 1);   // ran AND restarted the ring
    // THE ASSERTION THAT MATTERS IS THIS ONE. Recovery that runs and
    // leaves the ring unusable is worse than no recovery, because the
    // log then says it healed something it did not.
    int cc = cmd_submit(0, XHCI_TRB_SET_TYPE(XHCI_TRB_NOOP_CMD), 0);

    g_hc.cmd_recovered = saved;
    return (fired && cc == XHCI_CC_SUCCESS) ? 1 : 0;
}

int xhci_address_device(uint8_t root_port, uint32_t route, uint8_t speed,
                        uint8_t tt_slot, uint8_t tt_port) {
    uint8_t slot = 0;
    int cc = cmd_submit(0, XHCI_TRB_SET_TYPE(XHCI_TRB_ENABLE_SLOT), &slot);
    if (cc != XHCI_CC_SUCCESS) {
        klog_printf(KLOG_ERR "usb: enable slot failed: %s\n", xhci_completion_name((uint32_t)cc));
        return -cc;
    }
    if (!slot || slot > XHCI_MAX_SLOTS) {
        klog_printf("usb: controller assigned slot %u, out of range\n", slot);
        return -1;
    }

    // in_use from the moment the controller knows the slot, so every
    // failure below can hand cleanup to xhci_disable_slot() -- the
    // failed attempts used to leak their slots, and the retry then
    // burned a fresh one per try.
    struct xhci_slot *sl = &g_slots[slot];
    sl->in_use = 1;
    uint64_t ep0_phys = 0;
    sl->in_ctx  = alloc_frame(&sl->in_ctx_phys);
    sl->out_ctx = alloc_frame(&sl->out_ctx_phys);
    void *ep0_seg = alloc_frame(&ep0_phys);
    if (!sl->in_ctx || !sl->out_ctx || !ep0_seg) {
        klog_printf("usb: out of frames for slot %u\n", slot);
        xhci_disable_slot(slot);
        return -1;
    }
    xhci_ring_init(&sl->ep0, ep0_seg, ep0_phys, TRBS_PER_RING, 0);
    sl->port   = root_port;
    sl->speed  = speed;

    // The controller writes the Device Context, so it has to know where
    // it is BEFORE the Address Device command runs.
    g_hc.dcbaa[slot] = sl->out_ctx_phys;

    // Input Control Context: add the Slot Context (A0) and EP0 (A1).
    volatile uint32_t *icc = ctx_at(sl->in_ctx, 0);
    icc[0] = 0;                 // drop nothing
    icc[1] = (1u << 0) | (1u << 1);

    // Slot Context. The route string is 4 bits per tier below the root
    // port (0 for a root-port device); the root hub port number is the
    // ROOT port for every device in the chain, however deep. The TT
    // fields name the HIGH-speed hub doing split transactions for a
    // low/full-speed device -- zero when there is none, and the hub
    // driver is the only caller that ever passes them.
    volatile uint32_t *sc = ctx_at(sl->in_ctx, 1);
    sc[0] = ((uint32_t)1 << 27) |                 // context entries: EP0 only
            (((uint32_t)speed & 0xFu) << 20) |    // speed
            (route & 0xFFFFFu);                   // route string [19:0]
    sc[1] = ((uint32_t)root_port << 16);          // root hub port number
    sc[2] = (uint32_t)tt_slot | ((uint32_t)tt_port << 8);
    sc[3] = 0;

    // EP0 Context: a Control endpoint whose transfer ring is ep0.
    volatile uint32_t *ep = ctx_at(sl->in_ctx, 2);
    ep[0] = 0;
    ep[1] = (3u << 1) |                            // CErr = 3
            (4u << 3) |                            // EP type 4 = Control
            ((uint32_t)default_mps(speed) << 16);
    ep[2] = (uint32_t)(ep0_phys & 0xFFFFFFFFu) | 1u;   // DCS = 1
    ep[3] = (uint32_t)(ep0_phys >> 32);
    ep[4] = 8;                                     // average TRB length

    cc = cmd_submit(sl->in_ctx_phys,
                    XHCI_TRB_SET_TYPE(XHCI_TRB_ADDRESS_DEVICE) |
                    ((uint32_t)slot << 24), 0);
    if (cc != XHCI_CC_SUCCESS) {
        klog_printf(KLOG_ERR "usb: address device (slot %u) failed: %s\n",
                    slot, xhci_completion_name((uint32_t)cc));
        xhci_disable_slot(slot);
        return -cc;
    }
    return slot;
}

int xhci_set_ep0_mps(uint8_t slot, uint16_t mps) {
    if (!slot || slot > XHCI_MAX_SLOTS || !g_slots[slot].in_use) return -1;
    struct xhci_slot *sl = &g_slots[slot];

    volatile uint32_t *icc = ctx_at(sl->in_ctx, 0);
    icc[0] = 0;
    icc[1] = (1u << 1);          // evaluate EP0 only

    volatile uint32_t *ep = ctx_at(sl->in_ctx, 2);
    ep[1] = (ep[1] & 0x0000FFFFu) | ((uint32_t)mps << 16);

    int cc = cmd_submit(sl->in_ctx_phys,
                        XHCI_TRB_SET_TYPE(XHCI_TRB_EVALUATE_CONTEXT) |
                        ((uint32_t)slot << 24), 0);
    if (cc != XHCI_CC_SUCCESS) {
        klog_printf(KLOG_ERR "usb: evaluate context (slot %u, mps %u) failed: %s\n",
                    slot, mps, xhci_completion_name((uint32_t)cc));
        return -cc;
    }
    return 0;
}

// Reset Endpoint, then Set TR Dequeue Pointer: the two commands that
// bring a HALTED endpoint back. The dequeue is pointed at the ring's
// current ENQUEUE with its current cycle state -- everything the
// controller had in flight is abandoned, and the caller re-posts what
// it wants outstanding.
static int recover_halted(uint8_t slot, uint32_t dci, struct xhci_ring *ring) {
    int cc = cmd_submit(0, XHCI_TRB_SET_TYPE(XHCI_TRB_RESET_ENDPOINT) |
                           ((uint32_t)slot << 24) | (dci << 16), 0);
    if (cc != XHCI_CC_SUCCESS) return -cc;
    uint64_t deq = ring->phys + (uint64_t)ring->enqueue * sizeof(struct xhci_trb);
    cc = cmd_submit(deq | (ring->cycle ? 1u : 0u),
                    XHCI_TRB_SET_TYPE(XHCI_TRB_SET_TR_DEQUEUE) |
                    ((uint32_t)slot << 24) | (dci << 16), 0);
    if (cc != XHCI_CC_SUCCESS) return -cc;
    g_hc.ep_recoveries++;
    return 0;
}

void xhci_slot_set_hub(uint8_t slot, uint8_t n_ports, uint8_t ttt) {
    if (!slot || slot > XHCI_MAX_SLOTS || !g_slots[slot].in_use) return;
    volatile uint32_t *sc = ctx_at(g_slots[slot].in_ctx, 1);
    sc[0] |= (1u << 26);                                        // Hub flag
    sc[1] = (sc[1] & 0x00FFFFFFu) | ((uint32_t)n_ports << 24);  // Number of Ports
    sc[2] = (sc[2] & ~(3u << 16)) | (((uint32_t)ttt & 3u) << 16);
}

int xhci_control(uint8_t slot, const uint8_t setup[8],
                 void *buf, uint16_t len, int in) {
    if (!slot || slot > XHCI_MAX_SLOTS || !g_slots[slot].in_use) return -1;
    struct xhci_slot *sl = &g_slots[slot];

    // The SETUP packet rides in the TRB itself (IDT), so there is no
    // buffer to allocate for it. Transfer Type: 0 = no data stage,
    // 2 = OUT, 3 = IN.
    uint64_t setup_imm = 0;
    for (int i = 0; i < 8; i++) setup_imm |= (uint64_t)setup[i] << (8 * i);
    uint32_t trt = len ? (in ? 3u : 2u) : 0u;

    xhci_ring_push(&sl->ep0, setup_imm, 8,
                   XHCI_TRB_SET_TYPE(XHCI_TRB_SETUP_STAGE) |
                   XHCI_TRB_IDT | (trt << 16));

    uint64_t data_phys = 0;
    if (len) {
        // The caller's buffer is kernel memory, which is identity
        // mapped, so its virtual address IS its physical address --
        // the same property virtio_gpu.c relies on for its request
        // structs. No bounce buffer is needed.
        data_phys = (uint64_t)(uintptr_t)buf;
        xhci_ring_push(&sl->ep0, data_phys, len,
                       XHCI_TRB_SET_TYPE(XHCI_TRB_DATA_STAGE) |
                       (in ? (1u << 16) : 0) | XHCI_TRB_ISP);
    }

    // The Status Stage runs in the OPPOSITE direction to the data, and
    // IN when there was no data at all. It carries IOC, so it is the
    // TRB whose Transfer Event the waiter matches on.
    g_xfer_done.done = 0;
    g_xfer_done.ring_lo = sl->ep0.phys;
    g_xfer_done.ring_hi = sl->ep0.phys +
                          (uint64_t)sl->ep0.count * sizeof(struct xhci_trb);
    uint64_t status_trb = xhci_ring_push(&sl->ep0, 0, 0,
                              XHCI_TRB_SET_TYPE(XHCI_TRB_STATUS_STAGE) |
                              ((len && in) ? 0 : (1u << 16)) | XHCI_TRB_IOC);
    g_xfer_done.trb = status_trb;

    ring_doorbell(slot, dci_of(0));

    if (wait_completion(&g_xfer_done, "control transfer") < 0) {
        g_xfer_done.ring_lo = g_xfer_done.ring_hi = 0;
        // The first four setup bytes identify the request
        // (bmRequestType, bRequest, wValue) -- enough to say WHICH
        // request went unanswered, which is the whole question.
        usb_trace(USB_TR_CTRL, 0, slot,
                  (uint32_t)setup[0] | ((uint32_t)setup[1] << 8) |
                  ((uint32_t)setup[2] << 16) | ((uint32_t)setup[3] << 24),
                  0xFFu);
        return -1;
    }
    usb_trace(USB_TR_CTRL, 0, slot,
              (uint32_t)setup[0] | ((uint32_t)setup[1] << 8) |
              ((uint32_t)setup[2] << 16) | ((uint32_t)setup[3] << 24),
              g_xfer_done.code);
    if (g_xfer_done.code != XHCI_CC_SUCCESS &&
        g_xfer_done.code != XHCI_CC_SHORT_PACKET) {
        // A STALL is a legitimate answer to a request the device does
        // not support (SET_IDLE, commonly; a class request addressed to
        // an entity it does not have) -- and every code here HALTS ep0,
        // so without the reset the next control transfer to the device
        // fails too and one refused request kills the whole
        // enumeration. QEMU reaches this through the transaction-error
        // path rather than STALL, which is why the set is not just the
        // one code.
        if (g_xfer_done.code == XHCI_CC_STALL ||
            g_xfer_done.code == XHCI_CC_BABBLE ||
            g_xfer_done.code == XHCI_CC_USB_TRANSACTION_ERR ||
            g_xfer_done.code == XHCI_CC_DATA_BUFFER_ERROR)
            recover_halted(slot, 1, &sl->ep0);
        return -(int)g_xfer_done.code;
    }
    // A short packet is not an error -- it is how a device says "that is
    // all there was", and the residual says how much less it sent.
    uint32_t got = len;
    if (g_xfer_done.residual <= len) got = len - g_xfer_done.residual;
    return (int)got;
}

// --- interrupt-IN endpoints -------------------------------------------
//
// DEPTH IS THE POINT. Each endpoint keeps EP_DEPTH TRBs posted at all
// times, every one pointing at its own slice of a DMA frame, and each
// is re-posted the moment its report is taken. A ring with one
// outstanding TRB loses every report that arrives in the window between
// the controller completing it and the driver posting the next -- which
// under a polled drain is a very large window. virtio_input.c keeps 64
// buffers posted for exactly this reason.
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
};

// HID interfaces (a composite receiver is two on one device) plus one
// status-change endpoint per hub.
#define MAX_EPS 10
static struct xhci_ep g_eps[MAX_EPS];

static struct xhci_ep *ep_find(uint8_t slot, uint8_t ep_addr) {
    for (int i = 0; i < MAX_EPS; i++)
        if (g_eps[i].in_use && g_eps[i].slot == slot && g_eps[i].ep_addr == ep_addr)
            return &g_eps[i];
    return 0;
}

// Which endpoint owns the TRB a Transfer Event names. Address ranges
// rather than an id, because that is all the event carries.
static struct xhci_ep *ep_owning(uint64_t trb_phys) {
    for (int i = 0; i < MAX_EPS; i++) {
        struct xhci_ep *e = &g_eps[i];
        if (!e->in_use) continue;
        uint64_t lo = e->ring.phys;
        uint64_t hi = lo + (uint64_t)e->ring.count * sizeof(struct xhci_trb);
        if (trb_phys >= lo && trb_phys < hi) return e;
    }
    return 0;
}

// The endpoint an event names when it carries no TRB pointer -- a ring
// underrun does that, and it is an isochronous endpoint's normal way of
// saying "you gave me nothing to send".
static struct xhci_ep *ep_by_dci(uint8_t slot, uint32_t dci) {
    for (int i = 0; i < MAX_EPS; i++)
        if (g_eps[i].in_use && g_eps[i].slot == slot &&
            dci_of(g_eps[i].ep_addr) == dci)
            return &g_eps[i];
    return 0;
}

static void ep_post(struct xhci_ep *e, uint8_t slice) {
    uint64_t at = xhci_ring_push(&e->ring,
                                 e->buf_phys + (uint64_t)slice * EP_SLOT_SIZE,
                                 e->mps,
                                 XHCI_TRB_SET_TYPE(XHCI_TRB_NORMAL) |
                                 XHCI_TRB_IOC | XHCI_TRB_ISP);
    uint32_t idx = (uint32_t)((at - e->ring.phys) / sizeof(struct xhci_trb));
    if (idx < TRBS_PER_RING) e->buf_of_trb[idx] = slice;
    ring_doorbell(e->slot, dci_of(e->ep_addr));
}

// The xHCI Interval field is a LOG, and how to compute it depends on
// the speed -- which is why bInterval cannot simply be copied across.
// High/super speed already carry an exponent (1..16 meaning 2^(n-1)
// microframes); full/low speed carry a frame count, so it has to be
// converted. Getting this wrong does not fail, it just polls the device
// at the wrong rate.
static uint32_t interval_field(uint8_t speed, uint8_t b_interval) {
    if (speed == XHCI_SPEED_HIGH || speed == XHCI_SPEED_SUPER) {
        uint32_t v = b_interval ? (uint32_t)b_interval - 1 : 0;
        return v > 15 ? 15 : v;
    }
    uint32_t frames = b_interval ? b_interval : 1;
    uint32_t log2 = 0;
    while ((1u << (log2 + 1)) <= frames && log2 < 10) log2++;
    return log2 + 3;   // frames -> 125 us units
}

// The half both endpoint types share: claim a slot, build the input
// context and issue Configure Endpoint. `ep_type` is the xHCI code (1
// Isoch OUT, 7 Interrupt IN); `cerr` is 3 for an endpoint that retries
// and 0 for one that cannot, which is every isochronous endpoint.
static struct xhci_ep *ep_configure(uint8_t slot, uint8_t ep_addr, uint16_t mps,
                                    uint8_t interval, uint32_t ep_type,
                                    uint32_t cerr, int *out_cc) {
    *out_cc = 0;

    // AN ENDPOINT ALREADY CONFIGURED IS RECONFIGURED, NOT DUPLICATED.
    // This took the first free slot unconditionally, so configuring one
    // twice made a SECOND entry with a fresh ring and the new callback
    // -- while ep_find() kept answering with the FIRST. Every later
    // post then rang the stale ring's doorbell and every completion
    // went to the previous owner's callback. Measured against a real
    // device: 8 packets posted, 0 completed, with the TDs sitting in a
    // ring nothing was driving.
    //
    // Nothing had reconfigured an endpoint before a ring-3 driver could
    // claim a device away from its class driver, which is why it never
    // showed. The SEGMENT is reused rather than reallocated -- a second
    // frame here would leak one per rebind.
    struct xhci_ep *e = ep_find(slot, ep_addr);
    void *seg = 0;
    uint64_t ring_phys = 0;
    if (e) {
        seg = (void *)e->ring.trb;
        ring_phys = e->ring.phys;
    } else {
        for (int i = 0; i < MAX_EPS; i++) if (!g_eps[i].in_use) { e = &g_eps[i]; break; }
        if (!e) return 0;
        seg = alloc_frame(&ring_phys);
        if (!seg) return 0;
    }

    k_memset(e, 0, sizeof *e);
    e->slot = slot; e->ep_addr = ep_addr; e->mps = mps;
    k_memset(seg, 0, 4096);   // a reused segment still holds old TRBs
    xhci_ring_init(&e->ring, seg, ring_phys, TRBS_PER_RING, 0);

    struct xhci_slot *sl = &g_slots[slot];
    uint32_t dci = dci_of(ep_addr);

    volatile uint32_t *icc = ctx_at(sl->in_ctx, 0);
    icc[0] = 0;
    icc[1] = (1u << 0) | (1u << dci);        // slot context + this endpoint

    // The Slot Context has to grow: Context Entries is the HIGHEST DCI
    // in use, and a controller told 1 will simply not look at entry 3.
    volatile uint32_t *sc = ctx_at(sl->in_ctx, 1);
    uint32_t entries = (sc[0] >> 27) & 0x1Fu;
    if (dci > entries) sc[0] = (sc[0] & 0x07FFFFFFu) | (dci << 27);

    // The DEVICE's speed, not the root port's: behind a hub the two
    // differ (a low-speed mouse on a full-speed hub), and the interval
    // conversion is per-speed.
    volatile uint32_t *ep = ctx_at(sl->in_ctx, 1 + dci);
    ep[0] = interval_field(sl->speed, interval) << 16;
    ep[1] = (cerr << 1) | (ep_type << 3) | ((uint32_t)mps << 16);
    ep[2] = (uint32_t)(ring_phys & 0xFFFFFFFFu) | 1u;   // DCS = 1
    ep[3] = (uint32_t)(ring_phys >> 32);
    // Max ESIT Payload is what the endpoint moves per service interval,
    // and the controller reserves BANDWIDTH from it -- an isochronous
    // endpoint that leaves it at zero is scheduled for nothing.
    ep[4] = (uint32_t)mps | ((uint32_t)mps << 16);      // avg TRB len, max ESIT

    e->in_use = 1;   // published before the endpoint can complete anything

    int cc = cmd_submit(sl->in_ctx_phys,
                        XHCI_TRB_SET_TYPE(XHCI_TRB_CONFIGURE_ENDPOINT) |
                        ((uint32_t)slot << 24), 0);
    if (cc != XHCI_CC_SUCCESS) {
        klog_printf(KLOG_ERR "usb: configure endpoint 0x%x (slot %u) failed: %s\n",
                    ep_addr, slot, xhci_completion_name((uint32_t)cc));
        e->in_use = 0;
        *out_cc = cc;
        return 0;
    }
    return e;
}

int xhci_add_interrupt_in(uint8_t slot, uint8_t ep_addr, uint16_t mps,
                          uint8_t interval) {
    if (!slot || slot > XHCI_MAX_SLOTS || !g_slots[slot].in_use) return -1;
    if (mps == 0 || mps > EP_SLOT_SIZE) return -1;

    uint64_t buf_phys = 0;
    void *buf = alloc_frame(&buf_phys);
    if (!buf) return -1;

    int cc = 0;
    struct xhci_ep *e = ep_configure(slot, ep_addr, mps, interval, 7, 3, &cc);
    if (!e) return cc ? -cc : -1;
    e->buf = (uint8_t *)buf;
    e->buf_phys = buf_phys;

    for (uint8_t i = 0; i < EP_DEPTH; i++) ep_post(e, i);
    return 0;
}

int xhci_add_isoch_out(uint8_t slot, uint8_t ep_addr, uint16_t mps,
                       uint8_t interval, void (*done)(void *ctx, uint32_t bytes),
                       void *ctx) {
    if (!slot || slot > XHCI_MAX_SLOTS || !g_slots[slot].in_use) return -1;
    if (mps == 0 || (ep_addr & 0x80)) return -1;   // OUT endpoints only

    int cc = 0;
    // CErr = 0: an isochronous transfer is not retried, and the spec
    // has the controller ignore the field on such an endpoint anyway.
    struct xhci_ep *e = ep_configure(slot, ep_addr, mps, interval, 1, 0, &cc);
    if (!e) return cc ? -cc : -1;
    e->is_iso   = 1;
    e->iso_done = done;
    e->iso_ctx  = ctx;
    // Nothing is posted here: an isochronous ring with no TDs is idle,
    // not broken, and the stream's first packet is the driver's to
    // decide the timing of.
    return 0;
}

// A BULK endpoint, either direction. The xHCI endpoint type is the only
// thing that differs from an interrupt one -- 2 for OUT, 6 for IN -- so
// this is `ep_configure()` with a different code and a callback instead
// of the shared report buffers.
//
// CErr = 3, unlike the isochronous case: a bulk transfer IS retried, and
// a bulk endpoint DOES halt on a stall, which is what makes the existing
// halt recovery apply to it unchanged.
int xhci_add_bulk(uint8_t slot, uint8_t ep_addr, uint16_t mps,
                  void (*done)(void *ctx, uint64_t phys, uint32_t bytes, int ok),
                  void *ctx) {
    if (!slot || slot > XHCI_MAX_SLOTS || !g_slots[slot].in_use) return -1;
    if (mps == 0) return -1;

    int cc = 0;
    uint32_t type = (ep_addr & 0x80) ? 6u : 2u;
    // bInterval is meaningless for bulk -- the controller moves data
    // whenever there is bandwidth -- and 0 is what the spec wants.
    struct xhci_ep *e = ep_configure(slot, ep_addr, mps, 0, type, 3, &cc);
    if (!e) return cc ? -cc : -1;
    e->is_bulk   = 1;
    e->bulk_done = done;
    e->bulk_ctx  = ctx;
    return 0;
}

// Queue one buffer. Returns 0 when the TRB went on the ring; the
// callback runs later, from the event drain.
int xhci_bulk_post(uint8_t slot, uint8_t ep_addr, uint64_t buf_phys,
                   uint32_t len) {
    struct xhci_ep *e = ep_find(slot, ep_addr);
    if (!e || !e->is_bulk || len > 0x1FFFFu) return -1;
    if (e->halted) return -1;

    uint64_t at = xhci_ring_push(&e->ring, buf_phys, len,
                                 XHCI_TRB_SET_TYPE(XHCI_TRB_NORMAL) |
                                 XHCI_TRB_IOC | XHCI_TRB_ISP);
    uint32_t idx = (uint32_t)((at - e->ring.phys) / sizeof(struct xhci_trb));
    if (idx < TRBS_PER_RING) {
        e->buf_of_trb_phys[idx] = buf_phys;
        e->bulk_len_of_trb[idx] = len;
    }
    ring_doorbell(e->slot, dci_of(e->ep_addr));
    return 0;
}

// Is this endpoint wedged? A bulk endpoint halts on a stall like an
// interrupt one, and xhci_deferred_work() recovers it -- a caller that
// stopped getting completions should ask rather than assume.
int xhci_bulk_halted(uint8_t slot, uint8_t ep_addr) {
    struct xhci_ep *e = ep_find(slot, ep_addr);
    return e ? e->halted : 0;
}

int xhci_isoch_post(uint8_t slot, uint8_t ep_addr, uint64_t buf_phys,
                    uint32_t len, int ioc) {
    struct xhci_ep *e = ep_find(slot, ep_addr);
    if (!e || !e->is_iso) return -1;

    // SIA rather than a Frame ID: the alternative is tracking the
    // controller's own frame counter and predicting one interval ahead,
    // which buys nothing for a stream that is simply continuous.
    xhci_ring_push(&e->ring, buf_phys, len & 0x1FFFFu,
                   XHCI_TRB_SET_TYPE(XHCI_TRB_ISOCH) | XHCI_TRB_SIA |
                   (ioc ? XHCI_TRB_IOC : 0));
    ring_doorbell(e->slot, dci_of(e->ep_addr));
    return 0;
}

uint32_t xhci_isoch_underruns(uint8_t slot, uint8_t ep_addr) {
    struct xhci_ep *e = ep_find(slot, ep_addr);
    return e ? e->iso_underruns : 0;
}

int xhci_take_report(uint8_t slot, uint8_t ep_addr, void *buf, uint32_t cap) {
    struct xhci_ep *e = ep_find(slot, ep_addr);
    if (!e) return 0;

    uint8_t i = e->next_take;
    if (!e->ready[i]) return 0;

    uint32_t len = e->ready_len[i];
    if (len > cap) len = cap;
    k_memcpy(buf, e->buf + (uint32_t)i * EP_SLOT_SIZE, len);

    e->ready[i] = 0;
    e->next_take = (uint8_t)((i + 1) % EP_DEPTH);
    ep_post(e, i);            // straight back into circulation
    return (int)len;
}

void xhci_disable_slot(uint8_t slot) {
    if (!slot || slot > XHCI_MAX_SLOTS || !g_slots[slot].in_use) return;
    struct xhci_slot *sl = &g_slots[slot];

    // The controller first: Disable Slot stops every endpoint, so the
    // frames below have stopped being DMA targets before they are
    // freed. A refusal is logged and the frames freed anyway -- the
    // device is already gone, and a controller that will not answer
    // this command has bigger problems than a leak.
    int cc = cmd_submit(0, XHCI_TRB_SET_TYPE(XHCI_TRB_DISABLE_SLOT) |
                           ((uint32_t)slot << 24), 0);
    if (cc != XHCI_CC_SUCCESS)
        klog_printf("usb: disable slot %u: %s\n", slot,
                    xhci_completion_name((uint32_t)cc));

    for (int i = 0; i < MAX_EPS; i++) {
        struct xhci_ep *e = &g_eps[i];
        if (!e->in_use || e->slot != slot) continue;
        e->in_use = 0;   // unpublished before its memory goes away
        pmm_free_contiguous(e->ring.phys, 1);
        // An isochronous endpoint has no buffer of ours -- the driver
        // owns it. Freeing "frame zero" would hand real memory back.
        if (e->buf_phys) pmm_free_contiguous(e->buf_phys, 1);
    }

    g_hc.dcbaa[slot] = 0;
    // Guarded: the address-failure path arrives here with some of these
    // never allocated, and freeing "frame zero" would free real memory.
    if (sl->in_ctx_phys)  pmm_free_contiguous(sl->in_ctx_phys, 1);
    if (sl->out_ctx_phys) pmm_free_contiguous(sl->out_ctx_phys, 1);
    if (sl->ep0.phys)     pmm_free_contiguous(sl->ep0.phys, 1);
    k_memset(sl, 0, sizeof *sl);
}

// --- interrupts -------------------------------------------------------

// Acknowledging an xHCI interrupt is harder than virtio's single
// destructive byte read, and getting it wrong has two distinct failure
// modes. USBSTS.EINT and IMAN.IP are both RW1C, so a read-modify-write
// that writes the whole register back either fails to deassert the
// level-triggered line -- a storm that hangs the machine, which is the
// failure virtio-input already shipped once and only under KVM -- or
// clears a status bit belonging to something else.
//
// So: write back ONLY the bit being acknowledged, never the register.
// This is the one place either bit is written.
static int ack_interrupt(void) {
    uint32_t sts = mr32(g_hc.op, XHCI_USBSTS);
    if (!(sts & XHCI_STS_EINT)) return 0;    // "was it me?" -- it was not

    uint32_t iman = mr32(g_hc.rt, XHCI_IR0 + XHCI_IMAN);
    if (iman & XHCI_IMAN_IP)
        mw32(g_hc.rt, XHCI_IR0 + XHCI_IMAN, (iman & XHCI_IMAN_IE) | XHCI_IMAN_IP);

    mw32(g_hc.op, XHCI_USBSTS, XHCI_STS_EINT);
    return 1;
}

static void note_port_change(void) {
    for (uint32_t p = 0; p < g_hc.max_ports && p < XHCI_MAX_PORTS; p++) {
        uint32_t sc = mr32(g_hc.op, XHCI_PORTSC(p));
        uint32_t ack = sc & XHCI_PORTSC_RW1C;
        if (ack) portsc_write(p, 0, ack);

        uint8_t was = g_hc.ports[p].connected;
        uint8_t now = (sc & XHCI_PORTSC_CCS) ? 1 : 0;
        g_hc.ports[p].connected = now;
        g_hc.ports[p].enabled   = (sc & XHCI_PORTSC_PED) ? 1 : 0;
        g_hc.ports[p].speed     = (uint8_t)XHCI_PORTSC_SPEED(sc);

        // Hot-plug is DEFERRED, not done here: this runs inside the
        // event drain, and enumeration is synchronous control transfers
        // that would deadlock on the single-consumer guard. A plug
        // while the machine is busy still lands, because the pending
        // bit survives until the poll gets to it.
        if (now && !was) g_hc.attach_pending |= (1u << p);
        if (!now && was) {
            g_hc.detach_pending |= (1u << p);
            g_hc.attach_pending &= ~(1u << p);   // it left before we got there
        }
    }
}

// THE EVENT RING HAS EXACTLY ONE CONSUMER AT A TIME.
//
// Two things call xhci_service(): the interrupt handler, and a
// synchronous waiter spinning for its own completion. On a uniprocessor
// an interrupt can land in the middle of the waiter's pop, and both
// would then advance the dequeue index -- losing an event, or reading
// one slot twice.
//
// The guard makes the second caller a no-op rather than a race. It is
// safe for the interrupt handler to be the one turned away: it has
// already acknowledged the line by the time it gets here, so nothing
// storms, and the waiter it interrupted drains the event a moment
// later. A lock would be the wrong shape -- there is nothing to wait
// for, only something to skip.
static volatile uint8_t g_in_service;

void xhci_service(void) {
    if (!g_hc.present || !g_hc.running) return;
    if (g_in_service) return;
    g_in_service = 1;

    struct xhci_trb ev;
    int drained = 0;
    while (xhci_ring_event_pop(&g_hc.evt, &ev)) {
        g_hc.events_seen++;
        drained = 1;
        uint32_t type = XHCI_TRB_TYPE(ev.control);
        uint64_t src  = (uint64_t)ev.p0 | ((uint64_t)ev.p1 << 32);
        uint32_t code = (ev.status >> 24) & 0xFFu;

        if (type == XHCI_TRB_PORT_STATUS_CHANGE) {
            note_port_change();
        } else if (type == XHCI_TRB_CMD_COMPLETION) {
            // MATCHED ON ITS TRB, exactly as a Transfer Event is below.
            // This arm used to overwrite g_cmd_done.trb and set done
            // unconditionally, so ANY command completion satisfied
            // whichever command was currently waiting -- and handed it
            // that event's completion code AND slot id.
            //
            // That is not hypothetical: wait_completion() gives up after
            // a second and cmd_submit() leaves the command ON THE RING,
            // so a controller that completes it late posts an event with
            // nobody waiting for it. The next port's cmd_submit() then
            // took it, read the OLD command's slot id, and failed --
            // which is exactly the "address device (slot N) failed"
            // signature, and exactly why one port's stumble broke every
            // port after it (docs/bugs.md's cascade). The command ring
            // and this matcher are per-CONTROLLER, which is how a
            // per-port fault crossed ports.
            if (src == g_cmd_done.trb) {
                g_cmd_done.code = code;
                g_cmd_done.slot = (uint8_t)((ev.control >> 24) & 0xFFu);
                g_cmd_done.done = 1;
            } else {
                g_hc.cmd_stale++;
                klog_printf("usb: stale command completion for trb %#lx "
                            "(waiting on %#lx) -- ignored\n",
                            (unsigned long)src,
                            (unsigned long)g_cmd_done.trb);
            }
        } else if (type == XHCI_TRB_TRANSFER_EVENT) {
            // A Transfer Event names the TRB that finished. Control
            // transfers wait on their Status Stage TRB; interrupt
            // endpoints are matched by the HID layer, which lands next.
            if (src == g_xfer_done.trb ||
                (!g_xfer_done.done && g_xfer_done.ring_hi &&
                 src >= g_xfer_done.ring_lo && src < g_xfer_done.ring_hi)) {
                g_xfer_done.code     = code;
                g_xfer_done.residual = ev.status & 0xFFFFFFu;
                g_xfer_done.slot     = (uint8_t)((ev.control >> 24) & 0xFFu);
                g_xfer_done.ring_lo  = 0;
                g_xfer_done.ring_hi  = 0;
                g_xfer_done.done     = 1;
            } else if (code == XHCI_CC_RING_UNDERRUN ||
                       code == XHCI_CC_RING_OVERRUN) {
                // No TRB pointer on these -- the endpoint is named by
                // the event itself. An isochronous stream that has
                // stopped being fed says exactly this, once, and it is
                // not an error to recover from.
                struct xhci_ep *e = ep_by_dci((uint8_t)((ev.control >> 24) & 0xFFu),
                                              (ev.control >> 16) & 0x1Fu);
                if (e) e->iso_underruns++;
                else   g_hc.xfer_orphan++;
            } else {
                struct xhci_ep *e = ep_owning(src);
                if (!e) g_hc.xfer_orphan++;
                else if (code != XHCI_CC_SUCCESS && code != XHCI_CC_SHORT_PACKET) {
                    g_hc.xfer_bad++; g_hc.last_bad_code = code;
                    // These four HALT the endpoint: it will ignore its
                    // doorbell until Reset Endpoint. Marked here, fixed
                    // in xhci_deferred_work() -- commands cannot be
                    // issued from inside this drain.
                    //
                    // An ISOCHRONOUS endpoint is exempt: it does not
                    // halt, and a missed interval is a dropped packet
                    // rather than a stream to reset. Resetting one on a
                    // stutter is how a glitch becomes silence.
                    if (!e->is_iso &&
                        (code == XHCI_CC_STALL || code == XHCI_CC_BABBLE ||
                         code == XHCI_CC_USB_TRANSACTION_ERR ||
                         code == XHCI_CC_DATA_BUFFER_ERROR))
                        e->halted = 1;
                } else g_hc.xfer_ok++;
                if (e && e->is_bulk) {
                    // The buffer is named by the TRB that carried it, so
                    // a driver with several in flight knows WHICH one
                    // came back -- a receive ring is exactly that.
                    uint32_t idx = (uint32_t)((src - e->ring.phys) /
                                              sizeof(struct xhci_trb));
                    uint64_t phys = idx < TRBS_PER_RING ? e->buf_of_trb_phys[idx] : 0;
                    uint32_t want = idx < TRBS_PER_RING ? e->bulk_len_of_trb[idx] : 0;
                    uint32_t resid = ev.status & 0xFFFFFFu;
                    uint32_t got = resid <= want ? want - resid : 0;
                    int ok = (code == XHCI_CC_SUCCESS || code == XHCI_CC_SHORT_PACKET);
                    if (e->bulk_done) e->bulk_done(e->bulk_ctx, phys, got, ok);
                } else if (e && e->is_iso) {
                    // The driver refills and re-posts from here, inside
                    // the drain. That is safe because posting touches
                    // only its own transfer ring and a doorbell -- never
                    // the event ring this loop owns.
                    uint32_t resid = ev.status & 0xFFFFFFu;
                    uint32_t got = resid <= e->mps ? e->mps - resid : 0;
                    if (e->iso_done) e->iso_done(e->iso_ctx, got);
                } else if (e && (code == XHCI_CC_SUCCESS || code == XHCI_CC_SHORT_PACKET)) {
                    uint32_t idx = (uint32_t)((src - e->ring.phys) /
                                              sizeof(struct xhci_trb));
                    if (idx < TRBS_PER_RING) {
                        uint8_t  slice = e->buf_of_trb[idx];
                        uint32_t resid = ev.status & 0xFFFFFFu;
                        uint32_t got   = resid <= e->mps ? e->mps - resid : 0;
                        if (slice < EP_DEPTH) {
                            e->ready_len[slice] = (uint16_t)got;
                            e->ready[slice]     = 1;
                        }
                    }
                }
            }
        }
    }
    g_in_service = 0;
    if (drained) {
        // Advancing ERDP is what tells the controller the slots are
        // reusable. A driver that never does it appears to work on QEMU
        // (which does not enforce a full event ring) and wedges on
        // hardware. EHB is RW1C and is set here for the same reason.
        mw64(g_hc.rt, XHCI_IR0 + XHCI_ERDP,
             xhci_ring_erdp(&g_hc.evt) | XHCI_ERDP_EHB);
    }
}

static void xhci_irq_handler(uint64_t *regs) {
    (void)regs;
    if (!g_hc.present) return;
    // The "was it me?" must come first and must be cheap: this line is
    // shared, so this runs on every interrupt any device on it raises.
    if (!ack_interrupt()) return;
    g_hc.irqs_seen++;
    xhci_service();
    // The drain above only moved reports into memory and marked them
    // ready; decoding them is the HID layer's job. Done here rather
    // than inside xhci_service() so the re-entrancy guard covers the
    // ring and nothing else.
    usb_hid_service_all();
}

// The controller's poll, registered ALWAYS -- alongside the IRQ, not
// instead of it. This deliberately breaks with virtio_input.c's
// either/or, because on real hardware the BIOS-reported INTx line can
// be plausible and dead (a stale PIRQ value on a PCH routing through
// the IOAPIC), and a driver that trusts it has a mouse that is silently
// and permanently deaf with no fallback. The single-consumer guards in
// xhci_service() and usb_hid_service_all() are what make the double
// drain safe; when the IRQ is live the poll almost always finds the
// ring already empty. Deferred work (hot-plug, halt recovery) lives
// here in any case: it issues commands and cannot run from the handler.
static void xhci_poll_source(void) {
    xhci_service();
    usb_hid_service_all();
    usb_hub_service();
    xhci_deferred_work();
}

static struct input_source g_hc_source;

// --- ports ------------------------------------------------------------

// --- one socket, two port numbers -------------------------------------
//
// A USB3 socket appears to the controller TWICE: once in the USB2 port
// range and once in the USB3 range. A SuperSpeed device that trains its
// link shows up on the USB3 number; one that does not FALLS BACK to the
// USB2 number, and the same adapter then reports a different port. That
// is not cosmetic -- `ifconfig` printed one adapter as `usb3` or
// `usb14` depending on which happened (docs/bugs.md).
//
// **THE CONTROLLER DOES NOT SAY WHICH PAIRS WITH WHICH.** The Supported
// Protocol capability gives two RANGES and no pairing; the authority is
// ACPI `_PLD`, which is what Linux matches on (`match_location()` in
// usb/core/port.c), falling back to position when ACPI is absent. This
// kernel's AML layer is a declaration WALK with no interpreter
// (kernel/include/kernel/aml.h), so `_PLD` is out of reach today.
//
// **SO THIS IS THE FALLBACK, AND IT IS A HEURISTIC**: the Nth port of
// the USB3 range is the same socket as the Nth of the USB2 range. It is
// what Linux does without ACPI, and it matches the one pair actually
// observed here (a UE300 seen at port 3 and at port 14 on a controller
// with USB2 1..11 and USB3 12..15 -- index 3 of each). One observation
// is not a proof, which is why log_socket_map() prints the whole map at
// init: a wrong pairing is then visible against the physical machine
// rather than silently believed.
//
// **NEITHER RANGE COMES FIRST.** The ASUS puts USB2 at 1..11 and USB3
// at 12..15; QEMU puts USB3 at 1..4 and USB2 at 5..8. Code that assumed
// an order would be right on one and silently wrong on the other.
//
// Ports beyond the shorter range have no companion, which is the
// ordinary case -- 11 USB2 ports against 4 USB3 ones means seven
// USB2-only ports (internal webcams, Bluetooth, card readers).
//
// **THE RULE IS ARITHMETIC AND TAKES NO CONTROLLER**, which is why it
// is a separate function: it can then be tested against the two ranges
// a machine reports without one, and a KTEST can state the ASUS's own
// numbers as a fixture (usb_enum_test.c). Everything is 1-BASED here,
// as the capability and every port number a user sees are; 0 means "no
// companion".
int xhci_companion_in(unsigned first2, unsigned count2,
                      unsigned first3, unsigned count3, unsigned port) {
    if (!count2 || !count3 || !port) return 0;
    if (port >= first2 && port < first2 + count2) {
        unsigned idx = port - first2;
        return idx < count3 ? first3 + idx : 0;
    }
    if (port >= first3 && port < first3 + count3) {
        unsigned idx = port - first3;
        return idx < count2 ? first2 + idx : 0;
    }
    return 0;   // a port in neither range: nothing declared it
}

// 0-BASED in and out, as every port helper inside this file is; -1 for
// "no companion".
static int companion_port(uint32_t p) {
    int c = xhci_companion_in(g_hc.usb2.first, g_hc.usb2.count,
                              g_hc.usb3.first, g_hc.usb3.count, p + 1);
    return c ? c - 1 : -1;
}

int xhci_companion_port(unsigned port) {
    if (!g_hc.present || !port || port > XHCI_MAX_PORTS) return 0;
    return xhci_companion_in(g_hc.usb2.first, g_hc.usb2.count,
                             g_hc.usb3.first, g_hc.usb3.count, port);
}

// Prints the derived map once, so it can be CHECKED rather than
// trusted. A pairing this driver guessed wrong is otherwise invisible
// until it reports the wrong socket for a device somebody is looking
// at.
static void log_socket_map(void) {
    if (!g_hc.usb2.count || !g_hc.usb3.count) {
        klog_printf("usb: no socket map -- USB%s port range not declared\n",
                    g_hc.usb2.count ? "3" : "2");
        return;
    }
    for (uint32_t i = 0; i < g_hc.usb3.count; i++) {
        uint32_t ss = g_hc.usb3.first + i;
        int hs = xhci_companion_port(ss);
        if (hs) klog_printf("usb: socket %u = usb%d + usb%u\n", i + 1, hs, ss);
    }
}

static void reset_port(uint32_t p, int force) {
    uint32_t sc = mr32(g_hc.op, XHCI_PORTSC(p));
    if (!(sc & XHCI_PORTSC_CCS)) return;

    // A USB3 port trains itself and comes up already Enabled; asserting
    // PR on one is wrong. A USB2 port never enables without an explicit
    // reset. Discriminating on PED rather than on the port range works
    // for both and needs no Supported Protocol lookup.
    //
    // **THAT SECOND SENTENCE IS CONTRADICTED BY BOTH TEST LAPTOPS**, on
    // which USB2 ports arrive already ENABLED because the firmware
    // enabled them -- so the reset is skipped and the device is left in
    // whatever state the firmware left it. Whether that is the cause of
    // the intermittent enumeration failures is NOT established (an
    // always-skipped reset should fail every boot, and it does not), so
    // the behaviour is UNCHANGED and the skip is merely reported. See
    // docs/bugs.md, and `config set kernel.usb_reset <port>` for the
    // experiment that would settle it.
    if ((sc & XHCI_PORTSC_PED) && !force) {
        klog_printf("usb: port %u: arrived ENABLED (portsc 0x%x) -- "
                    "reset skipped\n", p + 1, sc);
        return;
    }

    portsc_write(p, XHCI_PORTSC_PR, XHCI_PORTSC_CSC);

    // WAIT FOR THE OUTCOME, NOT FOR THE CHANGE BIT. The spec says a
    // completed port reset raises PRC, and on real hardware it does --
    // but QEMU performs the whole USB2 reset synchronously inside the
    // register write above and signals it by setting PED, never raising
    // PRC at all. A loop waiting on PRC therefore spins its entire
    // backstop (measured: half a second of boot) and then declares
    // failure for a port that came up perfectly: `port 5 reset did not
    // complete` immediately followed by `port 5: connected, enabled`.
    //
    // PED is what the reset was FOR, so waiting on it is both correct
    // and portable; PRC is acknowledged below if it did appear.
    // 50 ms of reset plus recovery, with room for a slow hub.
    struct xhci_wait w; xhci_wait_start(&w, 500);
    for (;;) {
        uint32_t v = mr32(g_hc.op, XHCI_PORTSC(p));
        if (v & XHCI_PORTSC_PED) break;         // reset succeeded
        if (!(v & XHCI_PORTSC_CCS)) return;     // device left mid-reset
        if (xhci_wait_over(&w)) {
            klog_printf("usb: port %u reset did not complete, portsc 0x%x\n",
                        p + 1, v);
            return;
        }
    }
    portsc_write(p, 0, XHCI_PORTSC_PRC | XHCI_PORTSC_CSC | XHCI_PORTSC_PEC);

    // The USB2 reset-recovery wait (TRSTRCY) is a MINIMUM, not a
    // timeout: the device is entitled to 10 ms of quiet before it is
    // addressed.
    xhci_delay_ms(20);
}

// Reset then enumerate one root port, with ONE retry through a fresh
// reset. A real low/full-speed device is entitled to fumble its first
// descriptor read (Linux retries and re-resets for the same reason),
// and a failed attempt disables its slot, so the retry starts from
// nothing rather than from a device stuck mid-enumeration.
// THREE ATTEMPTS, NOT TWO, and the third exists on a measurement rather
// than on principle. 2026-09-06, one boot on the Lenovo: port 5's
// control transfer timed out on attempt 0 and the device enumerated on
// attempt 1, while port 4 timed out on BOTH and was lost -- so the loop
// gave up at exactly the point where a device might still have come
// back. Linux retries port initialisation several times for the same
// reason. Every failure measured on these machines is the same shape: a
// device that does not answer GET_DESCRIPTOR(CONFIG) within a second
// (docs/bugs.md), which is a device to ask again rather than a device
// that is not there.
//
// WHAT IT COSTS, since this is on the boot path: one more 1000 ms
// timeout per port that is going to be lost anyway. A boot losing three
// devices pays three extra seconds. That is the trade -- boot time on a
// bad boot against a device on a recoverable one -- and it is worth
// re-measuring if the failure rate ever drops.
#define ATTACH_ATTEMPTS 3

// A RE-ATTACH WE CAUSED IS NOT A CONNECT EVENT, and that difference
// cost a device. Taking a port away and giving it back makes CCS rise
// again -- but the change bits have to be acknowledged (or the port
// change interrupt stays asserted), and CSC is the only edge the driver
// watches. Measured on the ASUS 2026-09-17: the webcam's port read
// `portsc 0x6e1` -- CCS set, link Polling -- and nothing ever looked at
// it again, so a recovery meant to rescue a device instead removed one.
//
// So the attach is QUEUED, through the same `attach_pending` the event
// path sets. Not a second attach path: this driver has been bitten
// twice by those, and the bit is consumed by the next pass of the
// deferred work rather than recursing into the attach this is running
// inside.
static void requeue_if_connected(uint32_t p) {
    if (p >= XHCI_MAX_PORTS) return;

    // COMPLETE THE DISCONNECT WE CAUSED, FIRST. The attach arm skips
    // any port that still has a slot -- "a port that already has an
    // enumerated device is not a plug", which is right for a spurious
    // connect change and wrong here -- and the detach event for our own
    // cycle arrives a few milliseconds AFTER this runs. Measured on the
    // ASUS 2026-09-17: `still connected -- re-attaching` at 24.45 s,
    // `device removed` at 24.46, and the attach in between was
    // discarded against the dying device's slot. The recovery removed
    // the webcam instead of rescuing it, twice, for this reason.
    //
    // So the teardown happens HERE, where the driver knows the device
    // went away because it took the port off it, rather than being
    // waited for. The pending detach is dropped with it: it describes
    // this same transition, and left set it would tear down whatever
    // comes back.
    g_hc.detach_pending &= ~(1u << p);
    if (usb_root_port_slot((uint8_t)(p + 1))) {
        klog_printf("usb: port %u: tearing down the device the cycle "
                    "disconnected\n", p + 1);
        usb_detach_root_port((uint8_t)(p + 1));
    }
    // ...and the recorded state goes with it, so a port-change event
    // that lands late reads CCS=1 against `connected == 0` and queues
    // the same attach rather than seeing no transition at all.
    g_hc.ports[p].connected = 0;

    if (!(mr32(g_hc.op, XHCI_PORTSC(p)) & XHCI_PORTSC_CCS)) return;
    klog_printf("usb: port %u: still connected after the cycle -- "
                "re-attaching\n", p + 1);
    g_hc.attach_pending |= (1u << p);
}

// A LAST RESORT FOR A DEVICE THAT FELL BACK TO USB2 AND THEN FAILED:
// warm-reset the SuperSpeed side of the same socket.
//
// **WHY THIS IS NOT "LOOK AT THE COMPANION".** When a SuperSpeed device
// gives up on its link and falls back, the USB3 port shows NO
// CONNECTION -- there is nothing there to look at, and the first draft
// of this idea would have found an empty port. What the companion is
// good for is the one lever that re-runs LINK TRAINING: a warm reset
// (PORTSC.WPR), which a USB2 hot reset cannot do. Either the device
// comes back on the SuperSpeed side, or it re-attaches on the USB2 side
// having redone the handshake it botched.
//
// THE EVIDENCE, and its size: on 2026-09-09 a UE300 came up on the
// ASUS's port 3 as FULL-speed -- wrong for its class, it is a
// high-speed device at worst -- and `address device` failed all three
// attempts with no control transfer even attempted. A replug put it on
// port 14 at SuperSpeed and it bound first try. That is ONE
// observation, so this is an experiment with a fix's shape, and
// `tools/boot_rate.py --grep "GAVE UP"` is how it gets a number.
//
// Refused on anything but a USB2 port with a declared companion, so a
// controller whose ranges this driver could not read does nothing new.
static void warm_reset_companion(uint32_t p) {
    int c = companion_port(p);
    if (c < 0) return;
    // Only ever the USB3 half: WPR is meaningless on a USB2 port, and
    // asserting a reserved bit is not a diagnostic.
    if (!g_hc.usb3.count) return;
    uint32_t cp = (uint32_t)c;
    if (cp + 1 < g_hc.usb3.first ||
        cp + 1 >= (uint32_t)g_hc.usb3.first + g_hc.usb3.count) return;

    klog_printf("usb: port %u gave up -- warm-resetting its SuperSpeed "
                "companion, port %u\n", p + 1, cp + 1);
    portsc_write(cp, XHCI_PORTSC_WPR, XHCI_PORTSC_CSC);

    // WRC is the warm reset's own change bit. **QEMU IS NOT EXEMPT FROM
    // THIS PATH**, which the first draft of this comment claimed: it
    // declares both ranges (USB 3.0 at ports 1..4 and USB 2.0 at 5..8,
    // the opposite order from the ASUS), so a give-up there reaches
    // here too. What is NOT known is whether it implements WPR; if it
    // does not, this waits out the deadline and logs, which costs half
    // a second on a port that had already failed three times.
    struct xhci_wait w; xhci_wait_start(&w, 500);
    for (;;) {
        uint32_t v = mr32(g_hc.op, XHCI_PORTSC(cp));
        if (v & XHCI_PORTSC_WRC) break;
        if (xhci_wait_over(&w)) {
            klog_printf("usb: port %u warm reset did not complete, "
                        "portsc 0x%x\n", cp + 1, v);
            return;
        }
    }
    portsc_write(cp, 0, XHCI_PORTSC_WRC | XHCI_PORTSC_CSC | XHCI_PORTSC_PEC);
    // The device re-attaches on ONE of the two ports, and which is its
    // decision. Both are left to scan_ports(), which is already the one
    // place that notices a connect -- doing it here would be a second
    // attach path, and this driver has been bitten by two of those.
    xhci_delay_ms(20);
    klog_printf("usb: port %u: after warm reset portsc 0x%x; companion "
                "port %u portsc 0x%x\n",
                cp + 1, mr32(g_hc.op, XHCI_PORTSC(cp)),
                p + 1, mr32(g_hc.op, XHCI_PORTSC(p)));
    // Same edge problem as the cycle below: the change bits were
    // acknowledged, so a device that DID come back on the SuperSpeed
    // side needs its attach queued rather than waited for.
    requeue_if_connected(cp);
}

// DROP VBUS AND BRING IT BACK -- the software replug, and the lever a
// warm reset is not.
//
// WHAT THE EVIDENCE SAYS (docs/bugs.md, three occurrences). A UE300
// lands on the USB2 half of its socket at FULL speed -- two tiers below
// its class, so it failed SuperSpeed link training AND the USB2
// high-speed chirp -- and then answers Address Device with a USB
// TRANSACTION ERROR, three times. Enable Slot and Disable Slot around
// it both succeed, so the ring and the controller are fine and the
// DEVICE is wedged. Every recorded cure has been a physical replug or a
// cold boot; more resets have never worked, and on 2026-09-17 the warm
// reset of the SuperSpeed companion was observed firing and changing
// nothing -- `port 14: after warm reset portsc 0x2a0` is CCS=0, PLS=5
// (RxDetect): it re-trained a port with nothing on it, while the device
// sat on port 3 untouched.
//
// The mechanism that makes a REBOOT the trigger is that a warm reboot
// never drops VBUS, so a wedged device stays wedged into the next boot.
// Power is therefore the one thing a replug does that this driver had
// no way to do.
//
// WHAT REAL SYSTEMS DO: Linux's hub driver power-cycles a port through
// `usb_hub_set_port_power()` when resets fail, and its `usb_port`
// runtime PM uses the same two writes. This is that, on the root ports
// of one socket.
//
// Refused when HCCPARAMS1.PPC is 0 -- then port power is not
// software-controllable at all and there is nothing to cycle. QEMU
// reports PPC=0, so this path is hardware-only and says so once rather
// than silently doing nothing.
// NO VBUS SWITCH, SO TAKE THE PORT AWAY FROM THE CONTROLLER INSTEAD.
//
// On an Intel PCH the same two registers `intel_port_mux()` programs at
// startup can be written at any time: XUSB2PR routes a USB2 port to the
// xHC or away from it, and USB3PSSEN enables the SuperSpeed half. A
// port un-routed and re-routed presents its terminations afresh, which
// is what a device's link state machine reacts to -- the nearest thing
// to a replug available when HCCPARAMS1.PPC says port power is not
// software-controllable. The ASUS is exactly that machine: `port power
// always on (no PPC)`, measured 2026-09-17.
//
// THE DRIVER'S OWN COMMENT ON intel_port_mux() ALREADY NAMES THIS BUG'S
// SHAPE -- "a USB3 port whose SS half is not enabled falls back to its
// USB2 half" -- which is the failing signature exactly: the adapter
// lands on the USB2 companion, below its class's speed, and will not
// address. This puts both halves back through that transition
// deliberately.
//
// GATED ON THE MASK REGISTERS, like the startup write: a bit outside
// XUSB2PRM or USB3PRM is read-only, so a controller without the mux
// does nothing here and needs no device-id list. Returns 1 when
// something was actually toggled.
static int intel_mux_cycle(uint32_t p) {
    const struct pci_device *d = g_hc.pci;
    if (!d || d->vendor_id != 0x8086) return 0;

    uint32_t hs_mask = pci_config_read32(d, INTEL_XUSB2PRM);
    uint32_t ss_mask = pci_config_read32(d, INTEL_USB3PRM);
    if (!hs_mask && !ss_mask) return 0;

    // The bit is the port's index WITHIN ITS PROTOCOL'S RANGE, not the
    // port number: these registers count USB2 ports from 0 and USB3
    // ports from 0, while everything a person reads counts all ports
    // from 1.
    uint32_t hs_bit = 0, ss_bit = 0;
    int c = companion_port(p);
    if (g_hc.usb2.count && p + 1 >= g_hc.usb2.first &&
        p + 1 < (uint32_t)g_hc.usb2.first + g_hc.usb2.count)
        hs_bit = 1u << (p + 1 - g_hc.usb2.first);
    if (c >= 0 && g_hc.usb3.count && (uint32_t)c + 1 >= g_hc.usb3.first &&
        (uint32_t)c + 1 < (uint32_t)g_hc.usb3.first + g_hc.usb3.count)
        ss_bit = 1u << ((uint32_t)c + 1 - g_hc.usb3.first);

    hs_bit &= hs_mask;      // a bit the hardware will not take is not a lever
    ss_bit &= ss_mask;
    if (!hs_bit && !ss_bit) {
        klog_printf("usb: port %u: not routable -- XUSB2PRM 0x%x USB3PRM 0x%x\n",
                    p + 1, hs_mask, ss_mask);
        return 0;
    }

    uint32_t hs = pci_config_read32(d, INTEL_XUSB2PR);
    uint32_t ss = pci_config_read32(d, INTEL_USB3_PSSEN);
    klog_printf("usb: port %u: MUX CYCLING the socket (usb2 bit 0x%x of 0x%x, "
                "usb3 bit 0x%x of 0x%x)\n", p + 1, hs_bit, hs, ss_bit, ss);

    // SUPERSPEED FIRST, THEN USB2 -- the same order intel_port_mux()
    // takes and for the same reason: an SS half that goes away while
    // the USB2 half is still routed is precisely how a device ends up
    // on the USB2 side, which is the state being escaped from here.
    if (ss_bit) pci_config_write32(d, INTEL_USB3_PSSEN, ss & ~ss_bit);
    if (hs_bit) pci_config_write32(d, INTEL_XUSB2PR, hs & ~hs_bit);
    xhci_delay_ms(100);

    // RESTORED to what was read, not to "the bit set": the bits outside
    // the mask are read-only and the rest are somebody else's ports.
    if (hs_bit) pci_config_write32(d, INTEL_XUSB2PR, hs);
    if (ss_bit) pci_config_write32(d, INTEL_USB3_PSSEN, ss);
    xhci_delay_ms(100);

    portsc_rmw(p, 0, 0, XHCI_PORTSC_CSC | XHCI_PORTSC_PEC);
    if (c >= 0) portsc_rmw((uint32_t)c, 0, 0, XHCI_PORTSC_CSC | XHCI_PORTSC_PEC);
    klog_printf("usb: port %u: after mux cycle usb2 0x%x usb3 0x%x, "
                "portsc 0x%x\n", p + 1,
                pci_config_read32(d, INTEL_XUSB2PR),
                pci_config_read32(d, INTEL_USB3_PSSEN),
                mr32(g_hc.op, XHCI_PORTSC(p)));
    // EITHER HALF may be where it comes back -- that is the whole
    // premise of this bug.
    requeue_if_connected(p);
    if (c >= 0) requeue_if_connected((uint32_t)c);
    return 1;
}

static void power_cycle_socket(uint32_t p) {

    // BOTH HALVES OF THE SOCKET. They are one connector with one VBUS,
    // and the device may be on either -- this whole bug is it being on
    // the half nobody expected.
    int c = companion_port(p);
    klog_printf("usb: port %u: POWER CYCLING the socket%s\n", p + 1,
                c >= 0 ? " (with its companion)" : "");
    portsc_rmw(p, 0, XHCI_PORTSC_PP, 0);
    if (c >= 0) portsc_rmw((uint32_t)c, 0, XHCI_PORTSC_PP, 0);

    // OFF LONG ENOUGH TO BE SEEN. VBUS has to fall far enough for the
    // device's own link state machine to take it as a disconnect, which
    // is the entire point; 100 ms is the same order as this driver's
    // power-good wait and as a hub's own port-power delay.
    xhci_delay_ms(100);

    portsc_rmw(p, XHCI_PORTSC_PP, 0, 0);
    if (c >= 0) portsc_rmw((uint32_t)c, XHCI_PORTSC_PP, 0, 0);
    // Power good, then the USB2 attach debounce, before anything reads
    // CCS -- power_ports() waits the same way after the initial power-on.
    xhci_delay_ms(100);

    // The connect that follows is a REAL one, so the change bits are
    // acknowledged and the attach is left to scan_ports() -- the one
    // place that notices a connect. A second attach path here is how
    // this driver has been bitten twice before.
    portsc_rmw(p, 0, 0, XHCI_PORTSC_CSC | XHCI_PORTSC_PEC);
    if (c >= 0) portsc_rmw((uint32_t)c, 0, 0, XHCI_PORTSC_CSC | XHCI_PORTSC_PEC);
    klog_printf("usb: port %u: after power cycle portsc 0x%x\n", p + 1,
                mr32(g_hc.op, XHCI_PORTSC(p)));
    if (c >= 0)
        klog_printf("usb: port %u: companion after power cycle portsc 0x%x\n",
                    (uint32_t)c + 1, mr32(g_hc.op, XHCI_PORTSC((uint32_t)c)));
    requeue_if_connected(p);
    if (c >= 0) requeue_if_connected((uint32_t)c);
}

// THE SOFTWARE REPLUG, by whichever lever this machine has. Port power
// when the controller allows it; the Intel port mux when it does not,
// which is the only one the ASUS has. Once per episode either way.
// 1 if a replug was actually attempted, 0 if there was nothing left to
// try on this port -- which is what promotes the caller to the next
// lever rather than leaving the port lost.
static int software_replug(uint32_t p) {
    if (g_hc.ports[p].power_cycled) {
        klog_printf("usb: port %u: already replugged this episode\n", p + 1);
        return 0;
    }
    g_hc.ports[p].power_cycled = 1;
    if (g_hc.ppc) { power_cycle_socket(p); return 1; }
    klog_printf("usb: port %u: no Port Power Control (PPC=0) -- trying the "
                "port mux instead\n", p + 1);
    if (intel_mux_cycle(p)) return 1;
    klog_printf("usb: port %u: nothing left to try -- this controller has "
                "neither port power nor a port mux\n", p + 1);
    return 0;
}

static void power_ports(void);
static void scan_ports(void);
static void attach_root_port(uint32_t p);

// A STAGE MARKER, because the failure this exists to find is SILENCE.
// The first attempt stopped somewhere between the restart and the scan
// and printed nothing at all for 150 s, so "where" could not be read
// off the log (docs/bugs.md). One line per stage makes the boundary
// visible in a single run; they stay because a recovery nobody can
// watch is how this cost two attempts already.
#define HCSTAGE(name) klog_printf("usb: hcreset stage: " name "\n")

// THE USB2 ATTACH DEBOUNCE (TATTDB), in one place because two paths
// need it and only one of them ever had it. A plug is a mechanical
// event and a connection bounces; resetting inside the bounce is how
// the high-speed chirp is lost, and a device that loses it comes up
// FULL-speed -- docs/bugs.md's signature exactly. Linux waits for a
// STABLE connection (hub_port_debounce_be_stable) before it touches
// reset. Settable to 0 through kernel.usb_attach_delay, which is what
// lets the failure be provoked rather than waited for at 1 boot in 50.
static unsigned g_attach_delay_ms = 100;

void usb_set_attach_delay_ms(unsigned ms) {
    g_attach_delay_ms = ms > 1000 ? 1000 : ms;
}
unsigned usb_attach_delay_ms(void) { return g_attach_delay_ms; }

static int g_hc_reset_done;

// QUIESCE THE CONTROLLER ON THE WAY OUT, which is the one place this
// driver never acted. Every recovery it has is on the way IN -- a port
// reset, a warm reset of the SuperSpeed companion, a mux cycle, a
// controller re-init -- and docs/bugs.md records each of them failing
// to rescue a device that came up wrong. What none of them address is
// how the device got there: a warm reboot resets the CPU and leaves the
// xHC running with its links up, so a device is cut off mid-transfer
// and never sees the link go down. The next boot then re-initialises a
// controller whose devices are in a state neither side agreed on.
//
// Linux does this from the PCI ->shutdown() hook (`xhci_shutdown()`):
// halt, and reset on the platforms that need the links dropped. This is
// that, unconditionally, because a warm reboot is the only way this
// machine ever restarts.
//
// HALT **AND** RESET, not halt alone: halting stops SOFs and a device
// merely suspends, while HCRST returns the root ports to Disconnected
// so the link partner re-trains from scratch. reset_controller() does
// both, with deadline-bounded waits -- which matters here because this
// runs from a syscall with interrupts off, and a wait that cannot end
// would leave a machine unable to reboot at all. That is strictly worse
// than the bug, so it is the one property this must not lose.
void usb_shutdown(void) {
    if (!g_hc.present || !g_hc.running) return;
    klog_write("usb: halting the controller before the machine restarts\n");
    if (!reset_controller())
        klog_write(KLOG_WARN "usb: controller would not halt -- restarting anyway\n");
    g_hc.running = 0;
}

// ARMS the re-init; xhci_deferred_work() performs it. Returns 1 for
// "accepted", which is all a caller can be told -- the work happens
// later, off the interrupt-disabled path that asked for it.
int usb_controller_reinit(void) {
    if (!g_hc.present || !g_hc.running) return 0;
    if (g_hc_reset_done || g_hc.hcreset_pending) {
        klog_printf("usb: controller re-init already done or pending\n");
        return 0;
    }
    g_hc.hcreset_pending = 1;
    klog_printf("usb: controller re-init armed -- runs off the event path\n");
    return 1;
}

static void hcreset_perform(void) {
    if (g_hc_reset_done) return;
    g_hc_reset_done = 1;
    klog_printf(KLOG_WARN "usb: RE-INITIALISING THE CONTROLLER\n");

    for (uint32_t p = 0; p < g_hc.max_ports && p < XHCI_MAX_PORTS; p++) {
        if (mr32(g_hc.op, XHCI_PORTSC(p)) & XHCI_PORTSC_CCS)
            usb_detach_root_port((uint8_t)(p + 1));
    }
    g_hc.detach_pending = 0;
    g_hc.running = 0;
    HCSTAGE("detached");

    if (!reset_controller()) {
        klog_printf(KLOG_ERR "usb: re-init FAILED at the reset\n");
        return;
    }
    HCSTAGE("reset done");

    uint32_t slots = g_hc.max_slots;
    if (slots > XHCI_MAX_SLOTS) slots = XHCI_MAX_SLOTS;
    mw32(g_hc.op, XHCI_CONFIG, slots);
    for (uint32_t i = 1; i <= slots; i++) g_hc.dcbaa[i] = 0;
    HCSTAGE("config + dcbaa");

    program_rings();
    HCSTAGE("rings programmed");

    mw32(g_hc.op, XHCI_USBCMD, mr32(g_hc.op, XHCI_USBCMD) | XHCI_CMD_RS);
    struct xhci_wait w; xhci_wait_start(&w, 100);
    while (mr32(g_hc.op, XHCI_USBSTS) & XHCI_STS_HCH) {
        if (xhci_wait_over(&w)) {
            klog_printf(KLOG_ERR "usb: will not restart (usbsts 0x%x)\n",
                        mr32(g_hc.op, XHCI_USBSTS));
            return;
        }
    }
    g_hc.running = 1;
    HCSTAGE("running");

    if (g_hc.irq || g_hc.msi_vector) {
        mw32(g_hc.rt, XHCI_IR0 + XHCI_IMAN,
             mr32(g_hc.rt, XHCI_IR0 + XHCI_IMAN) | XHCI_IMAN_IE);
        mw32(g_hc.op, XHCI_USBCMD, mr32(g_hc.op, XHCI_USBCMD) | XHCI_CMD_INTE);
    }
    for (uint32_t p = 0; p < XHCI_MAX_PORTS; p++) {
        g_hc.ports[p].connected = 0;
        g_hc.ports[p].enabled = 0;
        g_hc.ports[p].speed = 0;
    }
    HCSTAGE("interrupter armed");

    power_ports();
    HCSTAGE("ports powered");

    for (uint32_t p = 0; p < g_hc.max_ports && p < XHCI_MAX_PORTS; p++) {
        klog_printf("usb: hcreset scanning port %u (portsc 0x%x)\n",
                    p + 1, mr32(g_hc.op, XHCI_PORTSC(p)));
        attach_root_port(p);
    }
    HCSTAGE("scanned");
}

static void attach_root_port(uint32_t p) {
    // One mark for the whole attach, so the dump carries every attempt
    // -- the interesting comparison is what the retry did DIFFERENTLY,
    // which a per-attempt mark would throw away.
    uint32_t trace_mark = usb_trace_mark();
    for (int attempt = 0; attempt < ATTACH_ATTEMPTS; attempt++) {
        // THE RETRY FORCES THE RESET, and until 2026-09-06 it did not.
        // reset_port() returns early on a port that already reports PED
        // -- which, after attempt 0's own successful reset, is every
        // port -- so the second attempt repeated the first byte for
        // byte and this loop's "resetting and retrying" was a lie. Both
        // test laptops showed it, and the Lenovo showed why it matters
        // in one boot: port 2 survived a control-transfer timeout and
        // enumerated, while port 5 hit the same timeout, fell into the
        // retry, and died with nothing having changed between attempts.
        //
        // NOT on a SuperSpeed port: asserting PR on one is a separate
        // question (see reset_port()), and every failure measured here
        // is low-, full- or high-speed. Attempt 0 read the speed.
        int force = attempt && g_hc.ports[p].speed != XHCI_SPEED_SUPER;
        uint8_t was_speed = g_hc.ports[p].speed;
        reset_port(p, force);
        uint32_t sc = mr32(g_hc.op, XHCI_PORTSC(p));
        g_hc.ports[p].connected = (sc & XHCI_PORTSC_CCS) ? 1 : 0;
        g_hc.ports[p].enabled   = (sc & XHCI_PORTSC_PED) ? 1 : 0;
        g_hc.ports[p].speed     = (uint8_t)XHCI_PORTSC_SPEED(sc);
        if (!g_hc.ports[p].connected) return;
        // THE SPEED ON EVERY ATTEMPT, not just the first. A device that
        // comes up at the WRONG speed is the sharpest lead this bug has
        // (docs/bugs.md): the gigabit adapter enumerated `full-speed` on
        // a failing boot and `high-speed` on working ones, which is a
        // high-speed CHIRP that did not happen during reset. The retry
        // used to log nothing, so nobody could see whether the forced
        // reset re-negotiated it -- which is exactly the question.
        if (attempt == 0) {
            klog_printf("usb: port %u: connected, %s, %s (portsc 0x%x)\n",
                        p + 1, speed_name(g_hc.ports[p].speed),
                        g_hc.ports[p].enabled ? "enabled" : "not enabled", sc);
        } else {
            usb_trace(USB_TR_PORT, (uint8_t)(p + 1), 0, sc, (uint32_t)attempt);
            klog_printf("usb: port %u: retry %d %s, %s (portsc 0x%x)%s\n",
                        p + 1, attempt, speed_name(g_hc.ports[p].speed),
                        g_hc.ports[p].enabled ? "enabled" : "not enabled", sc,
                        // Called out rather than left to be diffed: a
                        // speed that CHANGES across a reset is the chirp
                        // succeeding the second time, and it is the one
                        // observation that would turn the lead into a
                        // cause.
                        g_hc.ports[p].speed != was_speed
                            ? "  <- SPEED CHANGED" : "");
        }
        if (!g_hc.ports[p].enabled) return;
        // `attempt` is the patience flag too: the first try is fast,
        // and a device that has already failed is asked again slowly.
        if (usb_enumerate_port((uint8_t)(p + 1), g_hc.ports[p].speed,
                               attempt) >= 0) {
            g_hc.ports[p].power_cycled = 0;   // this episode ended well
            return;
        }
        // THE TWO OUTCOMES MUST NOT SHARE A PREFIX. "enumeration
        // failed -- resetting and retrying" is a device that may still
        // come back, and since the retry started actually re-resetting
        // it usually does; "gave up" is one that did not. Counting
        // "enumeration failed" over a run of boots silently conflated
        // them and made a rate that could not tell a recovery from a
        // loss (tools/boot_rate.py).
        // The speed rides the failure line too, so one grep for the
        // give-up carries what the device negotiated without needing
        // the surrounding lines -- which a truncated log may not have.
        int gave_up = attempt + 1 >= ATTACH_ATTEMPTS;
        klog_printf("usb: port %u: %s (at %s)\n", p + 1,
                    gave_up ? "enumeration GAVE UP"
                            : "enumeration failed -- resetting and retrying",
                    speed_name(g_hc.ports[p].speed));
        // ONLY ON THE WAY OUT. A retry that is about to try again does
        // not need its history printed; a port being lost does, and it
        // is the one case where the log lines are worth their space.
        if (gave_up) {
            usb_trace_dump(trace_mark, (uint8_t)(p + 1));
            // AFTER the dump, so the trace covers the attempts that
            // failed rather than the recovery -- and the recovery's own
            // lines then follow it in order.
            //
            // TWO LEVERS, CHEAPEST FIRST. The warm reset re-runs link
            // training and costs nothing when it works; the power cycle
            // is a replug and takes the port away for 200 ms, so it
            // only runs when the device is still sitting there after
            // the warm reset -- which is exactly what was measured on
            // 2026-09-17 and is the case this bug has always been.
            warm_reset_companion(p);
            if (mr32(g_hc.op, XHCI_PORTSC(p)) & XHCI_PORTSC_CCS) {
                // THIRD AND LAST, and only once the two above have had
                // their turn AND been shown not to work -- which
                // software_replug() reports by refusing a second
                // attempt in the same episode. That is exactly the
                // state the 2026-09-19 log ended in, and the point at
                // which a REBOOT was the only thing left.
                //
                // Behind `system.usb_recover`, off by default: this
                // takes every USB device down for a moment, and on a
                // laptop that is the keyboard. It is armed here and
                // performed off the event path -- see
                // usb_controller_reinit().
                if (!software_replug(p) && usb_recover_enabled())
                    usb_controller_reinit();
            }
        }
    }
}

// THE EXPERIMENT `kernel.usb_reset` RUNS. Force a port through a real
// reset and re-enumerate it, without anyone touching the cable.
//
// It exists to settle one question and it is worth stating so that the
// answer is not misread. The intermittent enumeration failure
// (docs/bugs.md) survives a boot and is cured by REPLUGGING the device,
// which points at the boot-time path -- but a replug does two things at
// once: it gives the port a genuine connect, AND it power-cycles the
// device. This does only the first. So:
//
//   works  -> the port state was the problem, and the skipped reset
//             above is implicated
//   fails  -> the device itself needs a real disconnect, and the reset
//             theory is dead
//
// Either answer is worth having, which is why this is a diagnostic and
// not a fix.
// The controller's own status word, for usb_trace_dump() -- which must
// be able to say "the controller halted" without reaching into g_hc.
uint32_t xhci_usbsts(void) {
    if (!g_hc.present) return 0;
    return mr32(g_hc.op, XHCI_USBSTS);
}

int usb_diag_replug_port(unsigned port) {
    if (!g_hc.running) return 0;
    if (port < 1 || port > g_hc.max_ports || port > XHCI_MAX_PORTS) return 0;
    // NOT gated on PPC: software_replug() picks whichever lever this
    // controller has and says so, and refusing here would hide the
    // answer to the question the knob exists to ask.
    //
    // QUEUED for the same reason the forced reset is: the caller is a
    // syscall with interrupts off, and this waits on pit_ticks().
    g_hc.diag_power_pending |= (1u << (port - 1));
    klog_printf("usb: port %u: replug queued\n", port);
    return 1;
}

int usb_diag_reset_port(unsigned port) {
    if (!g_hc.running) return 0;
    if (port < 1 || port > g_hc.max_ports || port > XHCI_MAX_PORTS) return 0;
    // QUEUED, NEVER DONE HERE. The caller is a syscall (kernel.usb_reset)
    // and this work spins on pit_ticks(), which only the timer interrupt
    // advances -- see diag_reset_pending. Returns 1 for "accepted"; the
    // OUTCOME is in the log a moment later, because there is nobody left
    // to return it to.
    g_hc.diag_reset_pending |= (1u << (port - 1));
    klog_printf("usb: port %u: forced reset queued\n", port);
    return 1;
}

// The queued reset, run from deferred work with interrupts on.
static void diag_reset_port(uint32_t p) {
    uint32_t before = mr32(g_hc.op, XHCI_PORTSC(p));
    klog_printf("usb: port %u: FORCED reset, portsc 0x%x\n", p + 1, before);
    if (!(before & XHCI_PORTSC_CCS)) {
        klog_printf("usb: port %u: nothing connected\n", p + 1);
        return;
    }

    unsigned port = p + 1;

    // A PORT THAT ALREADY HAS A DEVICE MUST BE TORN DOWN FIRST. Its
    // slot is still allocated, and enumerating over the top of one
    // fails -- which is not a finding, it is the diagnostic failing to
    // work. attach_root_port()'s retry gets this for free because a
    // FAILED attempt disables its own slot; a working port has to be
    // detached deliberately.
    //
    // This is also what makes a success on a good port mean something:
    // an instrument that cannot re-enumerate a device that is working
    // says nothing when it cannot re-enumerate one that is not.
    if (usb_root_port_slot((uint8_t)port)) {
        klog_printf("usb: port %u: releasing the existing device first\n", port);
        usb_detach_root_port((uint8_t)port);
    }

    reset_port(p, 1);
    uint32_t sc = mr32(g_hc.op, XHCI_PORTSC(p));
    g_hc.ports[p].connected = (sc & XHCI_PORTSC_CCS) ? 1 : 0;
    g_hc.ports[p].enabled   = (sc & XHCI_PORTSC_PED) ? 1 : 0;
    g_hc.ports[p].speed     = (uint8_t)XHCI_PORTSC_SPEED(sc);
    klog_printf("usb: port %u: after reset %s, %s (portsc 0x%x)\n", port,
                speed_name(g_hc.ports[p].speed),
                g_hc.ports[p].enabled ? "enabled" : "not enabled", sc);
    if (!g_hc.ports[p].connected || !g_hc.ports[p].enabled) return;

    // PATIENT: a forced reset is a diagnostic on a device that has
    // already misbehaved, so there is nothing to be gained by being
    // quick about it.
    int rc = usb_enumerate_port((uint8_t)port, g_hc.ports[p].speed, 1);
    klog_printf("usb: port %u: forced re-enumeration %s\n", port,
                rc >= 0 ? "SUCCEEDED" : "FAILED");
}

// On a controller with Port Power Control, ports come out of reset
// UNPOWERED: CCS never rises on a port nobody powered, so a connected
// mouse reads as an empty port with nothing logged anywhere. QEMU
// reports PPC=0, which is how this stayed unwritten for the driver's
// whole QEMU life.
static void power_ports(void) {
    if (!g_hc.ppc) return;
    int powered = 0;
    for (uint32_t p = 0; p < g_hc.max_ports && p < XHCI_MAX_PORTS; p++) {
        if (mr32(g_hc.op, XHCI_PORTSC(p)) & XHCI_PORTSC_PP) continue;
        portsc_write(p, XHCI_PORTSC_PP, 0);
        powered = 1;
    }
    if (!powered) return;
    klog_printf("usb: ports powered on (PPC)\n");
    // Power-good plus the USB2 attach debounce, before the scan reads
    // CCS. A minimum, like reset_port()'s recovery wait.
    clocksource_delay_ms(100);
}

static void scan_ports(void) {
    for (uint32_t p = 0; p < g_hc.max_ports && p < XHCI_MAX_PORTS; p++)
        attach_root_port(p);
}

void xhci_deferred_work(void) {
    if (!g_hc.present || !g_hc.running) return;

    // FIRST, and it takes every device with it -- so nothing below
    // should run against the state it is about to replace.
    if (g_hc.hcreset_pending) {
        g_hc.hcreset_pending = 0;
        hcreset_perform();
        return;
    }

    for (uint32_t p = 0; p < g_hc.max_ports && p < XHCI_MAX_PORTS; p++) {
        if (g_hc.detach_pending & (1u << p)) {
            g_hc.detach_pending &= ~(1u << p);
            // A REAL DISCONNECT ENDS THE EPISODE, so a device unplugged
            // and plugged back in gets the power cycle again if it needs
            // it -- the flag is "already tried for THIS device", not
            // "tried once ever".
            g_hc.ports[p].power_cycled = 0;
            klog_printf("usb: port %u: device removed\n", p + 1);
            usb_detach_root_port((uint8_t)(p + 1));
        }
        // The forced diagnostic reset (kernel.usb_reset). Before the
        // attach below, so a port that is pending both is reset once
        // deliberately rather than attached and then reset under it.
        if (g_hc.diag_power_pending & (1u << p)) {
            g_hc.diag_power_pending &= ~(1u << p);
            // FORCED, so the once-per-episode guard is cleared first:
            // the operator asking for it is the whole point, and a
            // refusal saying "already cycled" would be answering a
            // question nobody asked.
            g_hc.ports[p].power_cycled = 0;
            software_replug(p);
        }
        if (g_hc.diag_reset_pending & (1u << p)) {
            g_hc.diag_reset_pending &= ~(1u << p);
            diag_reset_port(p);
        }
        if (g_hc.attach_pending & (1u << p)) {
            g_hc.attach_pending &= ~(1u << p);
            // The bring-up ITSELF raises a connect change for a device
            // that was there all along, before scan_ports() has
            // recorded anything -- and acting on that re-resets a
            // working port, which kills its endpoints (measured: the
            // boot keyboard went deaf for half a second and dropped
            // the first keystrokes). A port that already has an
            // enumerated device is not a plug.
            if (usb_root_port_slot((uint8_t)(p + 1))) continue;
            // The USB2 attach debounce (TATTDB), shared with the boot
            // scan below -- see g_attach_delay_ms.
            xhci_delay_ms(g_attach_delay_ms);
            attach_root_port(p);
        }
    }

    for (int i = 0; i < MAX_EPS; i++) {
        struct xhci_ep *e = &g_eps[i];
        if (!e->in_use || !e->halted) continue;
        if (e->recover_tries > 4) continue;   // gave up; logged below once
        e->recover_tries++;
        int rc = recover_halted(e->slot, dci_of(e->ep_addr), &e->ring);
        if (rc == 0) {
            // Everything in flight was abandoned by the dequeue move,
            // so rebuild the posted set from scratch. Queued-but-unread
            // reports are dropped with it: they are stale by the width
            // of an error anyway.
            e->next_take = 0;
            for (int b = 0; b < EP_DEPTH; b++) { e->ready[b] = 0; e->ready_len[b] = 0; }
            e->halted = 0;
            for (uint8_t b = 0; b < EP_DEPTH; b++) ep_post(e, b);
            klog_printf("usb: slot %u ep 0x%x recovered from halt\n",
                        e->slot, e->ep_addr);
        } else if (e->recover_tries > 4) {
            klog_printf(KLOG_ERR "usb: slot %u ep 0x%x halt recovery failed (%s) -- giving up\n",
                        e->slot, e->ep_addr, xhci_completion_name((uint32_t)-rc));
        }
    }
}

// --- init -------------------------------------------------------------

// `nousb` on the boot line skips the controller entirely, matched as a
// whole word -- the same shape as `noahci` and `novirtio`, and here for
// the same reason: a machine this driver hangs is a machine with no way
// to reach a prompt and say so (one did, until the BIOS handoff --
// docs/decisions/drivers.md).
static int usb_disabled(void) {
    const char *cmdline = multiboot_cmdline();
    if (!cmdline) return 0;
    for (const char *p = cmdline; (p = k_strstr(p, "nousb")) != 0; p += 5) {
        if (p != cmdline && p[-1] != ' ') continue;
        char after = p[5];
        if (after == 0 || after == ' ') return 1;
    }
    return 0;
}

static void usb_probe(const struct pci_device *d) {
    BOOT_REQUIRE(BOOT_SUB_PCI);
    BOOT_REQUIRE(BOOT_SUB_PMM);

    if (usb_disabled()) {
        klog_printf("usb: disabled by `nousb` on the boot line\n");
        return;
    }
    if (!is_xhci(d)) return;
    if (g_hc.pci) {
        klog_printf("usb: a second xHCI at %02x:%02x.%u -- one is driven\n",
                    d->bus, d->device, d->function);
        return;
    }

    uint64_t bar0 = pci_bar_mem_addr(d, 0);
    if (!bar0) {
        klog_printf("usb: xHCI at %02x:%02x.%u has no usable memory BAR0\n",
                    d->bus, d->device, d->function);
        return;
    }
    uint64_t bar0_len = pci_bar_mem_size(d, 0);
    volatile void *win = paging_map_device(bar0, bar0_len ? bar0_len : 0x1000);
    if (!win) {
        klog_printf(KLOG_ERR "usb: xHCI register window at 0x%llx could not be mapped\n",
                    (unsigned long long)bar0);
        return;
    }

    g_hc.pci = d;
    g_hc.cap = (volatile uint8_t *)win;
    g_hc.cap_len = (bar0_len && bar0_len <= 0xFFFFFFFFull) ? (uint32_t)bar0_len
                                                            : XHCI_XECP_FALLBACK_LEN;

    // Memory space and bus mastering on; INTx stays DISABLED until every
    // structure the handler reads is published. See the commit point at
    // the bottom of this function.
    pci_command_update(d, PCI_CMD_MEMORY | PCI_CMD_BUS_MASTER, 0);
    pci_command_update(d, PCI_CMD_INTX_DISABLE, 0);

    uint8_t  caplen = mr8(g_hc.cap, XHCI_CAPLENGTH);
    uint32_t hcs1   = mr32(g_hc.cap, XHCI_HCSPARAMS1);
    uint32_t hcc1   = mr32(g_hc.cap, XHCI_HCCPARAMS1);

    g_hc.op = g_hc.cap + caplen;
    g_hc.rt = g_hc.cap + (mr32(g_hc.cap, XHCI_RTSOFF) & ~0x1Fu);
    g_hc.db = g_hc.cap + (mr32(g_hc.cap, XHCI_DBOFF)  & ~0x3u);

    g_hc.max_slots = XHCI_HCS1_MAXSLOTS(hcs1);
    g_hc.max_intrs = XHCI_HCS1_MAXINTRS(hcs1);
    g_hc.max_ports = XHCI_HCS1_MAXPORTS(hcs1);
    g_hc.ac64      = (uint8_t)XHCI_HCC1_AC64(hcc1);
    g_hc.ppc       = (uint8_t)XHCI_HCC1_PPC(hcc1);

    // CSZ decides whether a context structure is 32 or 64 bytes. QEMU
    // says 32 and much real hardware says 64; reading it wrong makes
    // every context field offset wrong, and nothing reports an error --
    // the controller simply parses garbage. Recorded here and consulted
    // by everything that builds a context.
    g_hc.csz64 = (uint8_t)XHCI_HCC1_CSZ(hcc1);

    klog_printf("usb: xHCI controller %04x:%04x at %02x:%02x.%u, BAR0 0x%llx (0x%x bytes)\n",
                d->vendor_id, d->device_id, d->bus, d->device, d->function,
                (unsigned long long)bar0, g_hc.cap_len);

    // THE CAPABILITY WALK COMES FIRST, because the BIOS handoff is in
    // it and the handoff has to happen before the reset -- resetting a
    // controller the firmware still owns is a write to somebody else's
    // device. This used to run after, which is how a machine whose BIOS
    // owned the controller hung before reaching any of the rest.
    walk_xecp(hcc1);
    legacy_handoff();

    // AFTER the handoff, BEFORE the reset: routing is the firmware's to
    // hand over first, and a port that arrives during the reset is one
    // the port scan below will find anyway.
    intel_port_mux(d);

    if (!reset_controller()) return;

    // PAGESIZE is only meaningful after reset. Bit n set means 2^(n+12).
    uint32_t ps = mr32(g_hc.op, XHCI_PAGESIZE) & 0xFFFFu;
    g_hc.page_size = ps ? (uint32_t)(4096u * (ps & (uint32_t)(-(int32_t)ps))) : 4096u;

    USBT("usb: trace: op=+0x%x rt=+0x%x db=+0x%x ac64=%u csz64=%u\n",
         (unsigned)(g_hc.op - g_hc.cap), (unsigned)(g_hc.rt - g_hc.cap),
         (unsigned)(g_hc.db - g_hc.cap), g_hc.ac64, g_hc.csz64);

    uint32_t slots = g_hc.max_slots;
    if (slots > XHCI_MAX_SLOTS) slots = XHCI_MAX_SLOTS;
    USBT("usb: trace: writing CONFIG=%u\n", slots);
    mw32(g_hc.op, XHCI_CONFIG, slots);

    USBT("usb: trace: setup_rings\n");
    if (!setup_rings()) return;
    USBT("usb: trace: rings done\n");

    // PPC IS ON THIS LINE BECAUSE A RECOVERY DEPENDS ON IT. Port Power
    // Control is what makes power_cycle_socket() -- the software replug
    // -- possible at all, and with it unlogged the only way to know
    // whether a machine has it was to read a failure that never
    // mentioned the reason.
    klog_printf("usb: %u slots (%u used), %u interrupters, %u ports, "
                "%u-byte contexts, %u-byte pages, port power %s\n",
                g_hc.max_slots, slots, g_hc.max_intrs, g_hc.max_ports,
                g_hc.csz64 ? 64u : 32u, g_hc.page_size,
                g_hc.ppc ? "software-controlled (PPC)" : "always on (no PPC)");

    // --- publish first ------------------------------------------------
    //
    // Everything the interrupt handler reads must be in place before the
    // controller is allowed to assert. This ordering is the rule in
    // docs/conventions/kernel.md, and it exists because getting it
    // backwards hung this guest 3 boots in 3 under KVM and never once
    // under TCG.
    // MSI FIRST, THE PIN AS A FALLBACK. This controller is the one
    // device here that gains most from it: its INTx line is whatever
    // the BIOS wrote into config byte 0x3C, routinely shared (an AC'97
    // and this controller both land on IRQ 11 on QEMU's pc machine),
    // and level-triggered, so the "was it mine?" read runs on every
    // interrupt anything on that line raises. An MSI belongs to one
    // device and cannot be shared at all.
    //
    // Falling back is the ordinary path, not an error: no LAPIC (an
    // older CPU, or `nomsi` on the GRUB line) and no MSI capability
    // both mean the pin, exactly as before.
    uint8_t line = pci_irq_line(d);
    g_hc.msi_vector = pci_msi_request(d, xhci_irq_handler);
    g_hc.msix = d->irq_msix;

    if (g_hc.msi_vector) {
        g_hc.irq = 0;   // nothing on a line any more
    } else if (line == IRQ_NONE) {
        klog_printf("usb: no usable INTx line (pin reports %u) -- polling\n", line);
        g_hc.irq = 0;
    } else {
        g_hc.irq = line;
    }

    g_hc.present = 1;
    if (g_hc.irq) irq_register_handler(g_hc.irq, xhci_irq_handler);

    g_hc_source.name = "usb-xhci";
    g_hc_source.caps = 0;              // the controller reports no events itself
    // Poll ALWAYS, IRQ or not -- see xhci_poll_source()'s comment. The
    // per-HID sources still leave poll NULL when the IRQ is live; their
    // decode rides this one.
    g_hc_source.poll = xhci_poll_source;
    g_hc_source.irq  = g_hc.irq;
    g_hc_source.msi_vector = g_hc.msi_vector;
    input_register_source(&g_hc_source);

    // Run before arming, so a device already attached raises its port
    // change against a controller that is actually running.
    mw32(g_hc.op, XHCI_USBCMD, mr32(g_hc.op, XHCI_USBCMD) | XHCI_CMD_RS);
    struct xhci_wait w; xhci_wait_start(&w, 100);
    while (mr32(g_hc.op, XHCI_USBSTS) & XHCI_STS_HCH) {
        if (xhci_wait_over(&w)) {
            klog_printf("usb: controller will not start (usbsts 0x%x)\n",
                        mr32(g_hc.op, XHCI_USBSTS));
            return;
        }
    }
    g_hc.running = 1;

    // --- and enable last ----------------------------------------------
    //
    // The interrupter is armed the same way either way: what differs is
    // only how the controller signals. pci_msi_enable() has already
    // disabled the pin for the MSI case, which is why the INTx clear
    // below belongs to the line case alone.
    if (g_hc.irq || g_hc.msi_vector) {
        mw32(g_hc.rt, XHCI_IR0 + XHCI_IMAN,
             mr32(g_hc.rt, XHCI_IR0 + XHCI_IMAN) | XHCI_IMAN_IE);
        mw32(g_hc.op, XHCI_USBCMD, mr32(g_hc.op, XHCI_USBCMD) | XHCI_CMD_INTE);
    }
    if (g_hc.irq) {
        pci_command_update(d, 0, PCI_CMD_INTX_DISABLE);   // the commit point
        pic_clear_mask(g_hc.irq);
    }

    if (g_hc.msi_vector)
        klog_printf("usb: running, %s on vector %u\n",
                    g_hc.msix ? "MSI-X" : "MSI", g_hc.msi_vector);
    else
        klog_printf("usb: running, %s\n",
                    g_hc.irq ? "interrupt-driven" : "polled");

    log_socket_map();
    usb_query_init();
    power_ports();
    scan_ports();
}

// --- introspection ----------------------------------------------------

int usb_controller_present(void) { return g_hc.present; }
uint8_t usb_controller_irq(void) { return g_hc.irq; }
uint8_t usb_controller_msi_vector(void) { return g_hc.msi_vector; }

int usb_controller_summary(char *buf, uint32_t cap) {
    if (!g_hc.present || !buf || !cap) return 0;
    k_snprintf(buf, cap, "xHCI %04x:%04x at %02x:%02x.%u, %u ports, %s",
               g_hc.pci->vendor_id, g_hc.pci->device_id,
               g_hc.pci->bus, g_hc.pci->device, g_hc.pci->function,
               g_hc.max_ports,
               !g_hc.running ? "not running"
                 : g_hc.msi_vector ? (g_hc.msix ? "running (MSI-X)" : "running (MSI)")
                 : g_hc.irq        ? "running"
                                   : "running (polled)");
    return 1;
}

uint32_t usb_events_seen(void) { return g_hc.events_seen; }
uint32_t usb_irqs_seen(void)   { return g_hc.irqs_seen; }

void usb_dump(void) {
    if (!g_hc.present) {
        klog_printf("usb: no xHCI controller\n");
        return;
    }
    klog_printf("usb: usbcmd 0x%x usbsts 0x%x crcr-lo 0x%x config 0x%x\n",
                mr32(g_hc.op, XHCI_USBCMD), mr32(g_hc.op, XHCI_USBSTS),
                mr32(g_hc.op, XHCI_CRCR), mr32(g_hc.op, XHCI_CONFIG));
    klog_printf("usb: iman 0x%x erstsz %u erdp-lo 0x%x\n",
                mr32(g_hc.rt, XHCI_IR0 + XHCI_IMAN),
                mr32(g_hc.rt, XHCI_IR0 + XHCI_ERSTSZ),
                mr32(g_hc.rt, XHCI_IR0 + XHCI_ERDP));
    klog_printf("usb: cmd ring 0x%llx enq %u cyc %u\n",
                (unsigned long long)g_hc.cmd.phys, g_hc.cmd.enqueue, g_hc.cmd.cycle);
    klog_printf("usb: xfer ok %u bad %u (last cc %u \"%s\") orphan %u, "
                "%u ep recover(ies)\n",
                g_hc.xfer_ok, g_hc.xfer_bad, g_hc.last_bad_code,
                xhci_completion_name(g_hc.last_bad_code), g_hc.xfer_orphan,
                g_hc.ep_recoveries);
    for (int i = 0; i < MAX_EPS; i++) {
        struct xhci_ep *e = &g_eps[i];
        if (!e->in_use) continue;
        uint32_t rmask = 0;
        for (int b = 0; b < EP_DEPTH; b++) if (e->ready[b]) rmask |= (1u << b);
        // THE ENDPOINT'S OWN STATE, out of the DEVICE context the
        // controller writes -- not our idea of it. The difference
        // matters: a ring with TRBs on it and no completions is either
        // an endpoint that is not Running or a device with nothing to
        // send, and only this tells them apart.
        const char *st = "?";
        struct xhci_slot *sl = &g_slots[e->slot];
        if (sl->out_ctx) {
            volatile uint32_t *ep = ctx_at(sl->out_ctx, dci_of(e->ep_addr));
            switch (ep[0] & 7u) {  // dispatch-ok: the five EP states
                case 0: st = "disabled"; break;
                case 1: st = "running";  break;
                case 2: st = "halted";   break;
                case 3: st = "stopped";  break;
                case 4: st = "error";    break;
                default: break;
            }
        }
        klog_printf("usb: ep slot %u addr 0x%x %s next_take %u ready 0x%x "
                    "enq %u cyc %u%s\n",
                    e->slot, e->ep_addr, st, e->next_take, rmask,
                    e->ring.enqueue, e->ring.cycle,
                    e->halted ? " HALTED" : "");
    }
    klog_printf("usb: evt ring 0x%llx deq %u ccs %u, %u event(s), %u irq(s)\n",
                (unsigned long long)g_hc.evt.phys, g_hc.evt.dequeue, g_hc.evt.ccs,
                g_hc.events_seen, g_hc.irqs_seen);
    klog_printf("usb: %d device(s), %u report(s) decoded, "
                "%u set-protocol(boot) accepted\n",
                usb_device_count(), usb_hid_reports(),
                usb_hid_boot_protocol_count());
    for (int i = 0; i < usb_device_count(); i++) {
        const struct usb_device_info *d = usb_device_at(i);
        if (d) klog_printf("usb:  dev %d port %u %04x:%04x \"%s\" class %u/%u/%u\n",
                           i, d->port, d->vendor_id, d->product_id,
                           d->product, d->if_class, d->if_subclass, d->if_protocol);
    }
    char hl[64];
    for (int i = 0; usb_hid_describe(i, hl, sizeof hl); i++)
        klog_printf("usb:  hid %d %s\n", i, hl);
    for (uint32_t p = 0; p < g_hc.max_ports && p < XHCI_MAX_PORTS; p++) {
        uint32_t sc = mr32(g_hc.op, XHCI_PORTSC(p));
        if (!(sc & XHCI_PORTSC_CCS) && !(sc & XHCI_PORTSC_PED)) continue;
        klog_printf("usb: port %u portsc 0x%x %s %s %s\n", p + 1, sc,
                    (sc & XHCI_PORTSC_CCS) ? "connected" : "empty",
                    (sc & XHCI_PORTSC_PED) ? "enabled" : "disabled",
                    speed_name((uint8_t)XHCI_PORTSC_SPEED(sc)));
    }
}
PCI_DRIVER("xhci", usb_matches, usb_probe);
