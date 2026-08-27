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
#include "pci_internal.h"
#include "pmm.h"
#include "irq.h"
#include "pic.h"
#include "input.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"
#include "barrier.h"
#include "timer.h"
#include "bootstage.h"
#include "usb_hid.h"
#include "multiboot.h" // multiboot_cmdline() -- the `nousb` flag

// The identity map covers the low 4 GiB and there is no
// paging_map_kernel_range(), so a BAR above that is unreachable rather
// than merely awkward. Same refusal, and the same reasoning, as
// virtio_pci.c's VIRTIO_ADDR_LIMIT: when the day comes that this fires,
// the message says what to build.
#define XHCI_ADDR_LIMIT 0x100000000ull

#define TRBS_PER_RING 256   // 4096 / sizeof(struct xhci_trb)

struct xhci_port_state {
    uint8_t connected;
    uint8_t enabled;
    uint8_t speed;
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
    uint8_t  ac64;
    uint32_t page_size;

    uint8_t  irq;            // 0 when polled
    uint8_t  present;
    uint8_t  running;

    struct xhci_ring cmd;
    struct xhci_ring evt;

    volatile uint64_t      *dcbaa;
    struct xhci_erst_entry *erst;

    uint32_t events_seen;
    uint32_t irqs_seen;
    uint32_t xfer_ok;
    uint32_t xfer_bad;
    uint32_t xfer_orphan;
    uint32_t last_bad_code;

    struct xhci_port_state ports[XHCI_MAX_PORTS];
};

// One addressed device. The contexts are the controller's view of it,
// and ep0 is the transfer ring every control request rides on.
struct xhci_slot {
    uint8_t  in_use;
    uint8_t  port;
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
static void portsc_write(uint32_t port, uint32_t set, uint32_t rw1c_ack) {
    uint32_t v = mr32(g_hc.op, XHCI_PORTSC(port));
    v &= ~(uint32_t)XHCI_PORTSC_RW1C;   // do not clear what we did not mean to
    v &= ~(uint32_t)XHCI_PORTSC_PED;    // ...and do not disable the port
    v |= set;
    v |= rw1c_ack;                      // only the change bits named here
    mw32(g_hc.op, XHCI_PORTSC(port), v);
}

// --- discovery --------------------------------------------------------

static const struct pci_device *find_xhci(void) {
    int count = pci_device_count();
    for (int i = 0; i < count; i++) {
        const struct pci_device *d = pci_device_at(i);
        if (!d || d->class_code != 0x0C || d->subclass != 0x03) continue;

        // prog_if is what tells the four USB controller generations
        // apart. Naming the one we found and refusing it explicitly
        // beats silence: on a machine with only an EHCI controller the
        // log then says why USB did not come up.
        if (d->prog_if == 0x30) return d;

        const char *kind = d->prog_if == 0x00 ? "UHCI" :
                           d->prog_if == 0x10 ? "OHCI" :
                           d->prog_if == 0x20 ? "EHCI" : "unknown";
        klog_printf("usb: %s controller %04x:%04x at %02x:%02x.%u -- "
                    "this driver is xHCI only, ignoring\n",
                    kind, d->vendor_id, d->device_id,
                    d->bus, d->device, d->function);
    }
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
// 64 KiB because that is the usual window and this driver cannot ask
// for the real size -- there is no pci_bar_mem_size(). That is the
// proper fix and it is a PCI change (probe by writing all-ones with
// decode off), not an xHCI one; until it exists this is a conservative
// cap plus the all-ones check below, which is what actually stops the
// runaway.
#define XHCI_XECP_MAX_OFF 0x10000u

static void walk_xecp(uint32_t hcc1) {
    uint32_t off = XHCI_HCC1_XECP(hcc1) * 4;   // xECP is in DWORDS
    int hops = 0;
    USBT("usb: trace: xecp walk from +0x%x\n", off);
    while (off && hops++ < 64) {               // bounded: the chain is device-supplied
        if (off >= XHCI_XECP_MAX_OFF) {
            klog_printf("usb: xECP chain leaves the register window at +0x%x "
                        "-- stopping\n", off);
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
        if (id == XHCI_XECP_ID_PROTO) {
            // Supported Protocol: name, then the port range it covers.
            uint32_t name  = mr32(g_hc.cap, off + 4);
            uint32_t ports = mr32(g_hc.cap, off + 8);
            uint32_t first = ports & 0xFFu, cnt = (ports >> 8) & 0xFFu;
            klog_printf("usb:  xECP %u supported-protocol USB %u.%u, ports %u..%u\n",
                        id, (v >> 24) & 0xFFu, (v >> 16) & 0xFFu,
                        first, first + cnt - 1);
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
static int legacy_handoff(void) {
    if (!g_hc.legsup_off) return 1;   // no such capability; nothing owns it

    uint32_t legsup = mr32(g_hc.cap, g_hc.legsup_off);
    if (legsup & XHCI_LEGSUP_BIOS_OWNED) {
        klog_printf("usb: BIOS owns the controller -- requesting handoff\n");
        mw32(g_hc.cap, g_hc.legsup_off, legsup | XHCI_LEGSUP_OS_OWNED);

        uint32_t spins = 0;
        while (mr32(g_hc.cap, g_hc.legsup_off) & XHCI_LEGSUP_BIOS_OWNED) {
            if (++spins > XHCI_POLL_BACKSTOP) {
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
    uint64_t p = pmm_alloc_contiguous(1);
    if (!p) return 0;
    if (p + 4096 > XHCI_ADDR_LIMIT) {
        // Cannot happen today (pmm never hands out above 4 GiB), but
        // the cast below would silently truncate if it ever did.
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
        uint32_t spins = 0;
        while (!(mr32(g_hc.op, XHCI_USBSTS) & XHCI_STS_HCH)) {
            if (++spins > XHCI_POLL_BACKSTOP) {
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
    uint32_t spins = 0;
    for (;;) {
        uint32_t c = mr32(g_hc.op, XHCI_USBCMD);
        uint32_t s = mr32(g_hc.op, XHCI_USBSTS);
        if (!(c & XHCI_CMD_HCRST) && !(s & XHCI_STS_CNR)) break;
        if (++spins > XHCI_POLL_BACKSTOP) {
            klog_printf("usb: reset did not complete (usbcmd 0x%x usbsts 0x%x)\n", c, s);
            return 0;
        }
    }
    return 1;
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

    USBT("usb: trace: DCBAAP\n");
    mw64(g_hc.op, XHCI_DCBAAP, dcbaa_phys);
    USBT("usb: trace: CRCR\n");
    mw64(g_hc.op, XHCI_CRCR, cmd_phys | XHCI_CRCR_RCS);

    USBT("usb: trace: ERSTSZ\n");
    mw32(g_hc.rt, XHCI_IR0 + XHCI_ERSTSZ, 1);
    USBT("usb: trace: ERDP\n");
    mw64(g_hc.rt, XHCI_IR0 + XHCI_ERDP, evt_phys);
    USBT("usb: trace: ERSTBA\n");
    // ERSTBA LAST: writing it is what arms the interrupter, so the
    // segment table and the dequeue pointer must already be valid.
    mw64(g_hc.rt, XHCI_IR0 + XHCI_ERSTBA, erst_phys);
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
    uint32_t spins = 0;
    while (!c->done) {
        xhci_service();
        if (++spins > XHCI_POLL_BACKSTOP) {
            klog_printf("usb: %s timed out after %u polls\n", what, spins);
            return -1;
        }
    }
    return 0;
}

// Enqueues one command, rings doorbell 0 and waits for its Command
// Completion event. Returns the completion code; `out_slot` receives
// the slot id the controller assigned, when the command allocates one.
static int cmd_submit(uint64_t param, uint32_t control, uint8_t *out_slot) {
    g_cmd_done.done = 0;
    uint64_t at = xhci_ring_push(&g_hc.cmd, param, 0, control);
    g_cmd_done.trb = at;
    ring_doorbell(0, 0);

    if (wait_completion(&g_cmd_done, "command") < 0) return -XHCI_CC_INVALID - 1;
    if (out_slot) *out_slot = g_cmd_done.slot;
    return (int)g_cmd_done.code;
}

int xhci_address_device(uint8_t port, uint8_t speed) {
    uint8_t slot = 0;
    int cc = cmd_submit(0, XHCI_TRB_SET_TYPE(XHCI_TRB_ENABLE_SLOT), &slot);
    if (cc != XHCI_CC_SUCCESS) {
        klog_printf("usb: enable slot failed: %s\n", xhci_completion_name((uint32_t)cc));
        return -cc;
    }
    if (!slot || slot > XHCI_MAX_SLOTS) {
        klog_printf("usb: controller assigned slot %u, out of range\n", slot);
        return -1;
    }

    struct xhci_slot *sl = &g_slots[slot];
    uint64_t ep0_phys = 0;
    sl->in_ctx  = alloc_frame(&sl->in_ctx_phys);
    sl->out_ctx = alloc_frame(&sl->out_ctx_phys);
    void *ep0_seg = alloc_frame(&ep0_phys);
    if (!sl->in_ctx || !sl->out_ctx || !ep0_seg) {
        klog_printf("usb: out of frames for slot %u\n", slot);
        return -1;
    }
    xhci_ring_init(&sl->ep0, ep0_seg, ep0_phys, TRBS_PER_RING, 0);
    sl->in_use = 1;
    sl->port   = port;

    // The controller writes the Device Context, so it has to know where
    // it is BEFORE the Address Device command runs.
    g_hc.dcbaa[slot] = sl->out_ctx_phys;

    // Input Control Context: add the Slot Context (A0) and EP0 (A1).
    volatile uint32_t *icc = ctx_at(sl->in_ctx, 0);
    icc[0] = 0;                 // drop nothing
    icc[1] = (1u << 0) | (1u << 1);

    // Slot Context. Route string 0 means "attached to a root port"; a
    // device behind a hub would need the real route, which is the hub
    // support this driver deliberately does not have.
    volatile uint32_t *sc = ctx_at(sl->in_ctx, 1);
    sc[0] = ((uint32_t)1 << 27) |                 // context entries: EP0 only
            (((uint32_t)speed & 0xFu) << 20);     // speed
    sc[1] = ((uint32_t)port << 16);               // root hub port number
    sc[2] = 0;
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
        klog_printf("usb: address device (slot %u) failed: %s\n",
                    slot, xhci_completion_name((uint32_t)cc));
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
        klog_printf("usb: evaluate context (slot %u, mps %u) failed: %s\n",
                    slot, mps, xhci_completion_name((uint32_t)cc));
        return -cc;
    }
    return 0;
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
    uint64_t status_trb = xhci_ring_push(&sl->ep0, 0, 0,
                              XHCI_TRB_SET_TYPE(XHCI_TRB_STATUS_STAGE) |
                              ((len && in) ? 0 : (1u << 16)) | XHCI_TRB_IOC);
    g_xfer_done.trb = status_trb;

    ring_doorbell(slot, dci_of(0));

    if (wait_completion(&g_xfer_done, "control transfer") < 0) return -1;
    if (g_xfer_done.code != XHCI_CC_SUCCESS &&
        g_xfer_done.code != XHCI_CC_SHORT_PACKET) {
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
};

#define MAX_EPS 4
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

int xhci_add_interrupt_in(uint8_t slot, uint8_t ep_addr, uint16_t mps,
                          uint8_t interval) {
    if (!slot || slot > XHCI_MAX_SLOTS || !g_slots[slot].in_use) return -1;
    if (mps == 0 || mps > EP_SLOT_SIZE) return -1;

    struct xhci_ep *e = 0;
    for (int i = 0; i < MAX_EPS; i++) if (!g_eps[i].in_use) { e = &g_eps[i]; break; }
    if (!e) return -1;

    uint64_t ring_phys = 0, buf_phys = 0;
    void *seg = alloc_frame(&ring_phys);
    void *buf = alloc_frame(&buf_phys);
    if (!seg || !buf) return -1;

    e->slot = slot; e->ep_addr = ep_addr; e->mps = mps;
    e->buf = (uint8_t *)buf; e->buf_phys = buf_phys;
    e->next_take = 0;
    for (int i = 0; i < EP_DEPTH; i++) { e->ready[i] = 0; e->ready_len[i] = 0; }
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

    uint8_t speed = 0;
    for (uint32_t p = 0; p < XHCI_MAX_PORTS; p++)
        if (sl->port == p + 1) speed = g_hc.ports[p].speed;

    volatile uint32_t *ep = ctx_at(sl->in_ctx, 1 + dci);
    ep[0] = interval_field(speed, interval) << 16;
    ep[1] = (3u << 1) |                       // CErr = 3
            (7u << 3) |                       // EP type 7 = Interrupt IN
            ((uint32_t)mps << 16);
    ep[2] = (uint32_t)(ring_phys & 0xFFFFFFFFu) | 1u;   // DCS = 1
    ep[3] = (uint32_t)(ring_phys >> 32);
    ep[4] = (uint32_t)mps | ((uint32_t)mps << 16);      // avg TRB len, max ESIT

    e->in_use = 1;   // published before the endpoint can complete anything

    int cc = cmd_submit(sl->in_ctx_phys,
                        XHCI_TRB_SET_TYPE(XHCI_TRB_CONFIGURE_ENDPOINT) |
                        ((uint32_t)slot << 24), 0);
    if (cc != XHCI_CC_SUCCESS) {
        klog_printf("usb: configure endpoint 0x%x (slot %u) failed: %s\n",
                    ep_addr, slot, xhci_completion_name((uint32_t)cc));
        e->in_use = 0;
        return -cc;
    }

    for (uint8_t i = 0; i < EP_DEPTH; i++) ep_post(e, i);
    return 0;
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
        g_hc.ports[p].connected = (sc & XHCI_PORTSC_CCS) ? 1 : 0;
        g_hc.ports[p].enabled   = (sc & XHCI_PORTSC_PED) ? 1 : 0;
        g_hc.ports[p].speed     = (uint8_t)XHCI_PORTSC_SPEED(sc);
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
            g_cmd_done.trb  = src;
            g_cmd_done.code = code;
            g_cmd_done.slot = (uint8_t)((ev.control >> 24) & 0xFFu);
            g_cmd_done.done = 1;
        } else if (type == XHCI_TRB_TRANSFER_EVENT) {
            // A Transfer Event names the TRB that finished. Control
            // transfers wait on their Status Stage TRB; interrupt
            // endpoints are matched by the HID layer, which lands next.
            if (src == g_xfer_done.trb) {
                g_xfer_done.code     = code;
                g_xfer_done.residual = ev.status & 0xFFFFFFu;
                g_xfer_done.slot     = (uint8_t)((ev.control >> 24) & 0xFFu);
                g_xfer_done.done     = 1;
            } else {
                struct xhci_ep *e = ep_owning(src);
                if (!e) g_hc.xfer_orphan++;
                else if (code != XHCI_CC_SUCCESS && code != XHCI_CC_SHORT_PACKET) {
                    g_hc.xfer_bad++; g_hc.last_bad_code = code;
                } else g_hc.xfer_ok++;
                if (e && (code == XHCI_CC_SUCCESS || code == XHCI_CC_SHORT_PACKET)) {
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

// The polled fallback, used only when the controller has no usable IRQ
// line. Same shape as virtio_input.c's: an interrupt-driven source
// leaves poll NULL, so the two can never both run.
static void xhci_poll_source(void) { xhci_service(); usb_hid_service_all(); }

static struct input_source g_hc_source;

// --- ports ------------------------------------------------------------

static void reset_port(uint32_t p) {
    uint32_t sc = mr32(g_hc.op, XHCI_PORTSC(p));
    if (!(sc & XHCI_PORTSC_CCS)) return;

    // A USB3 port trains itself and comes up already Enabled; asserting
    // PR on one is wrong. A USB2 port never enables without an explicit
    // reset. Discriminating on PED rather than on the port range works
    // for both and needs no Supported Protocol lookup.
    if (sc & XHCI_PORTSC_PED) return;

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
    uint32_t spins = 0;
    for (;;) {
        uint32_t v = mr32(g_hc.op, XHCI_PORTSC(p));
        if (v & XHCI_PORTSC_PED) break;         // reset succeeded
        if (!(v & XHCI_PORTSC_CCS)) return;     // device left mid-reset
        if (++spins > XHCI_POLL_BACKSTOP) {
            klog_printf("usb: port %u reset did not complete, portsc 0x%x\n",
                        p + 1, v);
            return;
        }
    }
    portsc_write(p, 0, XHCI_PORTSC_PRC | XHCI_PORTSC_CSC | XHCI_PORTSC_PEC);

    // The USB2 reset-recovery wait is a MINIMUM, not a timeout -- the
    // device is entitled to 10 ms of quiet before it is addressed, so
    // this one burns time rather than detecting something. pit_ticks()
    // is the only 10 ms-granularity source here, and it works because
    // USB init runs well after idt_init() with interrupts on.
    uint64_t start = pit_ticks();
    while (pit_ticks() - start < 2) { }   // 2 ticks = 20 ms, comfortably over
}

static void scan_ports(void) {
    for (uint32_t p = 0; p < g_hc.max_ports && p < XHCI_MAX_PORTS; p++) {
        reset_port(p);
        uint32_t sc = mr32(g_hc.op, XHCI_PORTSC(p));
        g_hc.ports[p].connected = (sc & XHCI_PORTSC_CCS) ? 1 : 0;
        g_hc.ports[p].enabled   = (sc & XHCI_PORTSC_PED) ? 1 : 0;
        g_hc.ports[p].speed     = (uint8_t)XHCI_PORTSC_SPEED(sc);
        if (!g_hc.ports[p].connected) continue;
        klog_printf("usb: port %u: connected, %s, %s\n", p + 1,
                    speed_name(g_hc.ports[p].speed),
                    g_hc.ports[p].enabled ? "enabled" : "not enabled");
        if (g_hc.ports[p].enabled)
            usb_enumerate_port((uint8_t)(p + 1), g_hc.ports[p].speed);
    }
}

// --- init -------------------------------------------------------------

// `nousb` on the boot line skips the controller entirely, matched as a
// whole word -- the same shape as `noahci` and `novirtio`, and here for
// the same reason: a machine this driver hangs is a machine with no way
// to reach a prompt and say so. See docs/bugs.md.
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

void usb_init(void) {
    BOOT_REQUIRE(BOOT_SUB_PCI);
    BOOT_REQUIRE(BOOT_SUB_PMM);

    if (usb_disabled()) {
        klog_printf("usb: disabled by `nousb` on the boot line\n");
        return;
    }

    const struct pci_device *d = find_xhci();
    if (!d) return;

    uint64_t bar0 = pci_bar_mem_addr(d, 0);
    if (!bar0) {
        klog_printf("usb: xHCI at %02x:%02x.%u has no usable memory BAR0\n",
                    d->bus, d->device, d->function);
        return;
    }
    if (bar0 + 0x1000 > XHCI_ADDR_LIMIT) {
        klog_printf("usb: xHCI register window at 0x%llx is above 4 GiB -- this"
                    " kernel identity-maps only the low 4 GiB and has no"
                    " kernel-range mapper\n", (unsigned long long)bar0);
        return;
    }

    g_hc.pci = d;
    g_hc.cap = (volatile uint8_t *)(uintptr_t)bar0;

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

    // CSZ decides whether a context structure is 32 or 64 bytes. QEMU
    // says 32 and much real hardware says 64; reading it wrong makes
    // every context field offset wrong, and nothing reports an error --
    // the controller simply parses garbage. Recorded here and consulted
    // by everything that builds a context.
    g_hc.csz64 = (uint8_t)XHCI_HCC1_CSZ(hcc1);

    klog_printf("usb: xHCI controller %04x:%04x at %02x:%02x.%u, BAR0 0x%llx\n",
                d->vendor_id, d->device_id, d->bus, d->device, d->function,
                (unsigned long long)bar0);

    // THE CAPABILITY WALK COMES FIRST, because the BIOS handoff is in
    // it and the handoff has to happen before the reset -- resetting a
    // controller the firmware still owns is a write to somebody else's
    // device. This used to run after, which is how a machine whose BIOS
    // owned the controller hung before reaching any of the rest.
    walk_xecp(hcc1);
    legacy_handoff();

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

    klog_printf("usb: %u slots (%u used), %u interrupters, %u ports, "
                "%u-byte contexts, %u-byte pages\n",
                g_hc.max_slots, slots, g_hc.max_intrs, g_hc.max_ports,
                g_hc.csz64 ? 64u : 32u, g_hc.page_size);

    // --- publish first ------------------------------------------------
    //
    // Everything the interrupt handler reads must be in place before the
    // controller is allowed to assert. This ordering is the rule in
    // docs/conventions/kernel.md, and it exists because getting it
    // backwards hung this guest 3 boots in 3 under KVM and never once
    // under TCG.
    uint8_t line = d->interrupt_line;
    if (line == 0xFF || line == 0 || line >= 16) {
        klog_printf("usb: no usable INTx line (pin reports %u) -- polling\n", line);
        g_hc.irq = 0;
    } else {
        g_hc.irq = line;
    }

    g_hc.present = 1;
    if (g_hc.irq) irq_register_handler(g_hc.irq, xhci_irq_handler);

    g_hc_source.name = "usb-xhci";
    g_hc_source.caps = 0;              // the controller reports no events itself
    g_hc_source.poll = g_hc.irq ? 0 : xhci_poll_source;
    g_hc_source.irq  = g_hc.irq;
    input_register_source(&g_hc_source);

    // Run before arming, so a device already attached raises its port
    // change against a controller that is actually running.
    mw32(g_hc.op, XHCI_USBCMD, mr32(g_hc.op, XHCI_USBCMD) | XHCI_CMD_RS);
    uint32_t spins = 0;
    while (mr32(g_hc.op, XHCI_USBSTS) & XHCI_STS_HCH) {
        if (++spins > XHCI_POLL_BACKSTOP) {
            klog_printf("usb: controller will not start (usbsts 0x%x)\n",
                        mr32(g_hc.op, XHCI_USBSTS));
            return;
        }
    }
    g_hc.running = 1;

    // --- and enable last ----------------------------------------------
    if (g_hc.irq) {
        mw32(g_hc.rt, XHCI_IR0 + XHCI_IMAN,
             mr32(g_hc.rt, XHCI_IR0 + XHCI_IMAN) | XHCI_IMAN_IE);
        mw32(g_hc.op, XHCI_USBCMD, mr32(g_hc.op, XHCI_USBCMD) | XHCI_CMD_INTE);
        pci_command_update(d, 0, PCI_CMD_INTX_DISABLE);   // the commit point
        pic_clear_mask(g_hc.irq);
    }

    klog_printf("usb: running, %s\n",
                g_hc.irq ? "interrupt-driven" : "polled");

    usb_query_init();
    scan_ports();
}

// --- introspection ----------------------------------------------------

int usb_controller_present(void) { return g_hc.present; }
uint8_t usb_controller_irq(void) { return g_hc.irq; }

int usb_controller_summary(char *buf, uint32_t cap) {
    if (!g_hc.present || !buf || !cap) return 0;
    k_snprintf(buf, cap, "xHCI %04x:%04x at %02x:%02x.%u, %u ports, %s",
               g_hc.pci->vendor_id, g_hc.pci->device_id,
               g_hc.pci->bus, g_hc.pci->device, g_hc.pci->function,
               g_hc.max_ports,
               g_hc.running ? (g_hc.irq ? "running" : "running (polled)")
                            : "not running");
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
    klog_printf("usb: xfer ok %u bad %u (last cc %u \"%s\") orphan %u\n",
                g_hc.xfer_ok, g_hc.xfer_bad, g_hc.last_bad_code,
                xhci_completion_name(g_hc.last_bad_code), g_hc.xfer_orphan);
    for (int i = 0; i < MAX_EPS; i++) {
        struct xhci_ep *e = &g_eps[i];
        if (!e->in_use) continue;
        uint32_t rmask = 0;
        for (int b = 0; b < EP_DEPTH; b++) if (e->ready[b]) rmask |= (1u << b);
        klog_printf("usb: ep slot %u addr 0x%x next_take %u ready 0x%x enq %u cyc %u\n",
                    e->slot, e->ep_addr, e->next_take, rmask,
                    e->ring.enqueue, e->ring.cycle);
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
