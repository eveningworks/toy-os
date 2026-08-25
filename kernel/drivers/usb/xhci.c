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

    struct xhci_port_state ports[XHCI_MAX_PORTS];
};

static struct xhci_hc g_hc;

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

// Walks the extended capability chain, logging every id.
//
// This costs twenty lines and answers two questions for free that would
// otherwise each need their own investigation: whether this controller
// implements USB Legacy Support (xECP id 1, the BIOS handoff -- QEMU
// does not, which is why the handoff is not built), and where the
// Supported Protocol capabilities put the USB2 and USB3 port ranges.
static void walk_xecp(uint32_t hcc1) {
    uint32_t off = XHCI_HCC1_XECP(hcc1) * 4;   // xECP is in DWORDS
    int hops = 0;
    while (off && hops++ < 64) {               // bounded: the chain is device-supplied
        uint32_t v  = mr32(g_hc.cap, off);
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

    mw64(g_hc.op, XHCI_DCBAAP, dcbaa_phys);
    mw64(g_hc.op, XHCI_CRCR, cmd_phys | XHCI_CRCR_RCS);

    mw32(g_hc.rt, XHCI_IR0 + XHCI_ERSTSZ, 1);
    mw64(g_hc.rt, XHCI_IR0 + XHCI_ERDP, evt_phys);
    // ERSTBA LAST: writing it is what arms the interrupter, so the
    // segment table and the dequeue pointer must already be valid.
    mw64(g_hc.rt, XHCI_IR0 + XHCI_ERSTBA, erst_phys);
    return 1;
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

void xhci_service(void) {
    if (!g_hc.present || !g_hc.running) return;

    struct xhci_trb ev;
    int drained = 0;
    while (xhci_ring_event_pop(&g_hc.evt, &ev)) {
        g_hc.events_seen++;
        drained = 1;
        uint32_t type = XHCI_TRB_TYPE(ev.control);
        if (type == XHCI_TRB_PORT_STATUS_CHANGE) note_port_change();
        // Transfer and command completion events are consumed by the
        // enumeration and HID layers, which land in the next commits.
    }
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
}

// The polled fallback, used only when the controller has no usable IRQ
// line. Same shape as virtio_input.c's: an interrupt-driven source
// leaves poll NULL, so the two can never both run.
static void xhci_poll_source(void) { xhci_service(); }

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
        if (g_hc.ports[p].connected)
            klog_printf("usb: port %u: connected, %s, %s\n", p + 1,
                        speed_name(g_hc.ports[p].speed),
                        g_hc.ports[p].enabled ? "enabled" : "not enabled");
    }
}

// --- init -------------------------------------------------------------

void usb_init(void) {
    BOOT_REQUIRE(BOOT_SUB_PCI);
    BOOT_REQUIRE(BOOT_SUB_PMM);

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

    if (!reset_controller()) return;

    // PAGESIZE is only meaningful after reset. Bit n set means 2^(n+12).
    uint32_t ps = mr32(g_hc.op, XHCI_PAGESIZE) & 0xFFFFu;
    g_hc.page_size = ps ? (uint32_t)(4096u * (ps & (uint32_t)(-(int32_t)ps))) : 4096u;

    walk_xecp(hcc1);

    uint32_t slots = g_hc.max_slots;
    if (slots > XHCI_MAX_SLOTS) slots = XHCI_MAX_SLOTS;
    mw32(g_hc.op, XHCI_CONFIG, slots);

    if (!setup_rings()) return;

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

    scan_ports();
}

// --- introspection ----------------------------------------------------

int usb_controller_present(void) { return g_hc.present; }

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

int usb_device_count(void) { return 0; }          // enumeration lands next
const struct usb_device_info *usb_device_at(int i) { (void)i; return 0; }

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
    klog_printf("usb: evt ring 0x%llx deq %u ccs %u, %u event(s), %u irq(s)\n",
                (unsigned long long)g_hc.evt.phys, g_hc.evt.dequeue, g_hc.evt.ccs,
                g_hc.events_seen, g_hc.irqs_seen);
    for (uint32_t p = 0; p < g_hc.max_ports && p < XHCI_MAX_PORTS; p++) {
        uint32_t sc = mr32(g_hc.op, XHCI_PORTSC(p));
        if (!(sc & XHCI_PORTSC_CCS) && !(sc & XHCI_PORTSC_PED)) continue;
        klog_printf("usb: port %u portsc 0x%x %s %s %s\n", p + 1, sc,
                    (sc & XHCI_PORTSC_CCS) ? "connected" : "empty",
                    (sc & XHCI_PORTSC_PED) ? "enabled" : "disabled",
                    speed_name((uint8_t)XHCI_PORTSC_SPEED(sc)));
    }
}
