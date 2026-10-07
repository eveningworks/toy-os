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
#include "xhci_internal.h"

DRIVER_DECLARE("xhci", "usb", "USB 3 xHCI host controller");

// Defined here, shared through xhci_internal.h.
struct xhci_hc g_xhci;
struct xhci_slot g_xhci_slots[XHCI_MAX_SLOTS + 1];
volatile struct xhci_completion g_xhci_cmd_done;
volatile struct xhci_completion g_xhci_xfer_done;

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

const char *xhci_speed_name(uint8_t s) {
    switch (s) {  // dispatch-ok: bounded by the xHCI default speed ID set
        case XHCI_SPEED_FULL:  return "full-speed";
        case XHCI_SPEED_LOW:   return "low-speed";
        case XHCI_SPEED_HIGH:  return "high-speed";
        case XHCI_SPEED_SUPER: return "super-speed";
        default:               return "unknown-speed";
    }
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
// The bound is the BAR's probed size (g_xhci.cap_len); 64 KiB is the
// fallback for a controller whose BAR the probe could not size, and
// the all-ones check below is what stops the runaway either way.
#define XHCI_XECP_FALLBACK_LEN 0x10000u

static void walk_xecp(uint32_t hcc1) {
    uint32_t off = XHCI_HCC1_XECP(hcc1) * 4;   // xECP is in DWORDS
    int hops = 0;
    USBT("usb: trace: xecp walk from +0x%x\n", off);
    while (off && hops++ < 64) {               // bounded: the chain is device-supplied
        if (off + 4 > g_xhci.cap_len) {
            klog_printf("usb: xECP chain leaves the register window at +0x%x "
                        "(BAR0 is 0x%x bytes) -- stopping\n", off, g_xhci.cap_len);
            return;
        }
        // BEFORE the read, not only after: if the read itself is what
        // hangs -- an unclaimed MMIO cycle on real hardware -- then a
        // line printed afterwards never appears, and the trace names
        // the previous offset instead of the fatal one.
        USBT("usb: trace: xecp read +0x%x\n", off);
        uint32_t v  = mr32(g_xhci.cap, off);
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
        if (id == XHCI_XECP_ID_PROTO && off + 12 > g_xhci.cap_len) {
            klog_printf("usb: xECP supported-protocol at +0x%x is cut off by the "
                        "register window -- stopping\n", off);
            return;
        }
        if (id == XHCI_XECP_ID_PROTO) {
            // Supported Protocol: name, then the port range it covers.
            uint32_t name  = mr32(g_xhci.cap, off + 4);
            uint32_t ports = mr32(g_xhci.cap, off + 8);
            uint32_t first = ports & 0xFFu, cnt = (ports >> 8) & 0xFFu;
            uint32_t major = (v >> 24) & 0xFFu;
            klog_printf("usb:  xECP %u supported-protocol USB %u.%u, ports %u..%u\n",
                        id, major, (v >> 16) & 0xFFu,
                        first, first + cnt - 1);
            // KEPT, where it used to be logged and dropped. The ranges
            // are what companion_port() pairs, and without them a
            // device that moves between the two port numbers of one
            // socket looks like it moved sockets.
            struct xhci_proto_range *r = major == 2 ? &g_xhci.usb2
                                       : major == 3 ? &g_xhci.usb3 : 0;
            if (major == 3) g_xhci.usb3_ports |= xhci_port_range_mask(first, cnt);
            if (r && first && cnt) {
                r->major = (uint8_t)major;
                r->first = (uint8_t)first;
                r->count = (uint8_t)cnt;
            }
            (void)name;
        } else if (id == XHCI_XECP_ID_LEGACY) {
            g_xhci.legsup_off = off;   // the handoff below needs to find it again
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
// detect anything. They used coarse_ticks() with the note that it was
// "the only 10 ms-granularity source here", which stopped being true
// when the clocksource arrived: clocksource_now_ns() is finer AND, when
// it is deadline-capable, advances with interrupts off, where a counter
// the timer INTERRUPT increments does not.
//
// That difference is not academic. It is why `kernel.usb_reset` froze a
// laptop on its first outing: a syscall runs with interrupts off, and
// the coarse_ticks() loop there could never end. The PIT path is kept for
// a machine whose clocksource cannot be trusted with a deadline, and
// there it carries the old precondition -- interrupts on.
// The policy moved to clocksource_delay_ms(), which five drivers had
// each hand-rolled. This name stays because usb.h publishes it.
void xhci_delay_ms(uint32_t ms) {
    clocksource_delay_ms(ms);
}


void xhci_wait_start(struct xhci_wait *w, uint32_t ms) {
    w->spins = 0;
    w->deadline = clocksource_deadline_capable()
                ? clocksource_now_ns() + (uint64_t)ms * 1000000ull
                : 0;
}

// 1 when the caller should give up.
int xhci_wait_over(struct xhci_wait *w) {
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

// Intel's PORT DISABLE OVERRIDES (Sunrise Point and later): firmware
// may switch a socket's USB2 or SuperSpeed half off here, which looks
// exactly like a port nothing is plugged into. Read only -- logged
// because no other evidence of it exists on a machine with no serial.
#define INTEL_USB2PDO    0xE4
#define INTEL_USB3PDO    0xE8

static void intel_pdo_log(const struct pci_device *d) {
    if (d->vendor_id != 0x8086) return;
    klog_printf("usb: intel port disable overrides: usb2 0x%x usb3 0x%x\n",
                pci_config_read32(d, INTEL_USB2PDO), pci_config_read32(d, INTEL_USB3PDO));
}

// Every USB3 port's PORTSC and link-error count, once, after the scan:
// a SuperSpeed half that never trains reads RxDetect with nothing else
// to say so.
static void log_usb3_ports(void) {
    char line[256];
    int n = k_snprintf(line, sizeof line, "usb: usb3 ports:");
    for (uint32_t i = 0; i < g_xhci.usb3.count && n > 0 && n < (int)sizeof line - 24; i++) {
        uint32_t p = g_xhci.usb3.first - 1 + i;
        if (p >= g_xhci.max_ports || p >= XHCI_MAX_PORTS) break;
        n += k_snprintf(line + n, sizeof line - n, " %u=0x%x/%u", p + 1,
                        mr32(g_xhci.op, XHCI_PORTSC(p)),
                        mr32(g_xhci.op, XHCI_PORTSC(p) + 8) & 0xFFFFu);
    }
    klog_printf("%s\n", line);
}

static int legacy_handoff(void) {
    if (!g_xhci.legsup_off) return 1;   // no such capability; nothing owns it

    uint32_t legsup = mr32(g_xhci.cap, g_xhci.legsup_off);
    if (legsup & XHCI_LEGSUP_BIOS_OWNED) {
        klog_printf("usb: BIOS owns the controller -- requesting handoff\n");
        mw32(g_xhci.cap, g_xhci.legsup_off, legsup | XHCI_LEGSUP_OS_OWNED);

        // The spec allows a BIOS a full second to let go.
        struct xhci_wait w; xhci_wait_start(&w, 1000);
        while (mr32(g_xhci.cap, g_xhci.legsup_off) & XHCI_LEGSUP_BIOS_OWNED) {
            if (xhci_wait_over(&w)) {
                klog_printf("usb: BIOS did not release the controller -- "
                            "taking it anyway\n");
                // Force both bits: claim ownership and drop the BIOS's
                // claim. Linux does the same, and leaving the BIOS bit
                // set would leave the firmware believing it still has a
                // device this driver is about to reset.
                uint32_t v = mr32(g_xhci.cap, g_xhci.legsup_off);
                v |= XHCI_LEGSUP_OS_OWNED;
                v &= ~XHCI_LEGSUP_BIOS_OWNED;
                mw32(g_xhci.cap, g_xhci.legsup_off, v);
                break;
            }
            cpu_relax();
        }
    }

    // SMIs OFF, AND THE STATUS BITS CLEARED, whether or not the handoff
    // above was granted. This is the half that stops the traps: an
    // enable left set means the next ordinary register write is another
    // trip into firmware.
    uint32_t ctl = mr32(g_xhci.cap, g_xhci.legsup_off + XHCI_LEGCTLSTS);
    ctl &= XHCI_LEGACY_DISABLE_SMI;
    ctl |= XHCI_LEGACY_SMI_EVENTS;
    mw32(g_xhci.cap, g_xhci.legsup_off + XHCI_LEGCTLSTS, ctl);

    uint32_t now = mr32(g_xhci.cap, g_xhci.legsup_off);
    klog_printf("usb: legacy handoff done (legsup 0x%x, os-owned=%u)\n",
                now, (now & XHCI_LEGSUP_OS_OWNED) ? 1u : 0u);
    return (now & XHCI_LEGSUP_BIOS_OWNED) ? 0 : 1;
}

// --- bring-up ---------------------------------------------------------

// One 4 KiB frame, zeroed, identity-mapped. Returns 0 on failure.
void *xhci_alloc_frame(uint64_t *out_phys) {
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

int xhci_reset_controller(void) {
    // Stop first. A controller that is running must halt before reset,
    // and HCH is how it says it has.
    uint32_t cmd = mr32(g_xhci.op, XHCI_USBCMD);
    if (cmd & XHCI_CMD_RS) {
        mw32(g_xhci.op, XHCI_USBCMD, cmd & ~(uint32_t)XHCI_CMD_RS);
        // The spec gives the controller 16 ms to halt.
        struct xhci_wait w; xhci_wait_start(&w, 100);
        while (!(mr32(g_xhci.op, XHCI_USBSTS) & XHCI_STS_HCH)) {
            if (xhci_wait_over(&w)) {
                klog_printf("usb: controller will not halt (usbsts 0x%x)\n",
                            mr32(g_xhci.op, XHCI_USBSTS));
                return 0;
            }
        }
    }

    mw32(g_xhci.op, XHCI_USBCMD, XHCI_CMD_HCRST);

    // Reset is done when HCRST self-clears AND CNR clears. Waiting on
    // only the first is a real bug: the controller can drop HCRST while
    // still refusing register writes, and everything programmed in that
    // window is silently discarded.
    struct xhci_wait w; xhci_wait_start(&w, 1000);
    for (;;) {
        uint32_t c = mr32(g_xhci.op, XHCI_USBCMD);
        uint32_t s = mr32(g_xhci.op, XHCI_USBSTS);
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
// re-allocate. xhci_alloc_frame() identity-maps, so a ring's virtual address
// is its physical one.
void xhci_program_rings(void) {
    xhci_ring_init(&g_xhci.cmd, (void *)g_xhci.cmd.trb, g_xhci.cmd.phys,
                   g_xhci.cmd.count, 0);
    xhci_ring_init(&g_xhci.evt, (void *)g_xhci.evt.trb, g_xhci.evt.phys,
                   g_xhci.evt.count, 1);
    g_xhci.erst[0].base = g_xhci.evt.phys;
    g_xhci.erst[0].size = g_xhci.evt.count;
    mw64(g_xhci.op, XHCI_DCBAAP, (uint64_t)(uintptr_t)g_xhci.dcbaa);
    mw64(g_xhci.op, XHCI_CRCR, g_xhci.cmd.phys | XHCI_CRCR_RCS);
    mw32(g_xhci.rt, XHCI_IR0 + XHCI_ERSTSZ, 1);
    mw64(g_xhci.rt, XHCI_IR0 + XHCI_ERDP, g_xhci.evt.phys);
    // ERSTBA LAST: writing it arms the interrupter.
    mw64(g_xhci.rt, XHCI_IR0 + XHCI_ERSTBA, (uint64_t)(uintptr_t)g_xhci.erst);
}

static int setup_rings(void) {
    uint64_t dcbaa_phys = 0, cmd_phys = 0, evt_phys = 0, erst_phys = 0;

    USBT("usb: trace: allocating ring frames\n");
    g_xhci.dcbaa = (volatile uint64_t *)xhci_alloc_frame(&dcbaa_phys);
    void *cmd_seg = xhci_alloc_frame(&cmd_phys);
    void *evt_seg = xhci_alloc_frame(&evt_phys);
    g_xhci.erst = (struct xhci_erst_entry *)xhci_alloc_frame(&erst_phys);

    if (!g_xhci.dcbaa || !cmd_seg || !evt_seg || !g_xhci.erst) {
        klog_printf("usb: out of contiguous frames for the controller rings\n");
        return 0;
    }

    xhci_ring_init(&g_xhci.cmd, cmd_seg, cmd_phys, TRBS_PER_RING, 0);
    xhci_ring_init(&g_xhci.evt, evt_seg, evt_phys, TRBS_PER_RING, 1);

    g_xhci.erst[0].base = evt_phys;
    g_xhci.erst[0].size = TRBS_PER_RING;

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
    uint32_t hcs2 = mr32(g_xhci.cap, XHCI_HCSPARAMS2);
    uint32_t spb  = XHCI_HCS2_SPB_MAX(hcs2);
    USBT("usb: trace: scratchpad wanted=%u pagesize=%u\n", spb, g_xhci.page_size);
    if (spb) {
        if (g_xhci.page_size > 4096) {
            klog_printf("usb: controller wants %u scratchpad pages of %u bytes;"
                        " this kernel's allocator aligns only to 4096, so a"
                        " >4 KiB page size needs an aligned allocator first\n",
                        spb, g_xhci.page_size);
            return 0;
        }
        uint64_t arr_phys = 0;
        volatile uint64_t *arr = (volatile uint64_t *)xhci_alloc_frame(&arr_phys);
        if (!arr) return 0;
        if (spb > 512) spb = 512;          // one frame of pointers
        for (uint32_t i = 0; i < spb; i++) {
            uint64_t p = 0;
            if (!xhci_alloc_frame(&p)) return 0;
            arr[i] = p;
        }
        g_xhci.dcbaa[0] = arr_phys;
        klog_printf("usb: %u scratchpad page(s)\n", spb);
    }

    xhci_program_rings();
    return 1;
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

static int usb_probe(const struct pci_device *d) {
    BOOT_REQUIRE(BOOT_SUB_PCI);
    BOOT_REQUIRE(BOOT_SUB_PMM);

    if (usb_disabled()) return pci_probe_decline(d, "USB is off: `nousb` on the boot line");
    // The class match takes every USB host controller; only xHCI is driven.
    if (!is_xhci(d))
        return pci_probe_decline(d, "a USB %s controller; only xHCI is driven",
                                 d->prog_if == 0x20 ? "2.0 (EHCI)" :
                                 d->prog_if == 0x10 ? "1.1 (OHCI)" :
                                 d->prog_if == 0x00 ? "1.1 (UHCI)" : "host");
    if (g_xhci.pci) return pci_probe_decline(d, "a second xHCI controller; one is driven");

    uint64_t bar0 = pci_bar_mem_addr(d, 0);
    if (!bar0) return pci_probe_decline(d, "no usable memory BAR0");
    uint64_t bar0_len = pci_bar_mem_size(d, 0);
    volatile void *win = paging_map_device(bar0, bar0_len ? bar0_len : 0x1000);
    if (!win)
        return pci_probe_decline(d, "its register window at 0x%llx could not be mapped",
                                 (unsigned long long)bar0);

    g_xhci.pci = d;
    g_xhci.cap = (volatile uint8_t *)win;
    g_xhci.cap_len = (bar0_len && bar0_len <= 0xFFFFFFFFull) ? (uint32_t)bar0_len
                                                            : XHCI_XECP_FALLBACK_LEN;

    // Memory space and bus mastering on; INTx stays DISABLED until every
    // structure the handler reads is published. See the commit point at
    // the bottom of this function.
    pci_command_update(d, PCI_CMD_MEMORY | PCI_CMD_BUS_MASTER, 0);
    pci_command_update(d, PCI_CMD_INTX_DISABLE, 0);

    uint8_t  caplen = mr8(g_xhci.cap, XHCI_CAPLENGTH);
    uint32_t hcs1   = mr32(g_xhci.cap, XHCI_HCSPARAMS1);
    uint32_t hcc1   = mr32(g_xhci.cap, XHCI_HCCPARAMS1);

    g_xhci.op = g_xhci.cap + caplen;
    g_xhci.rt = g_xhci.cap + (mr32(g_xhci.cap, XHCI_RTSOFF) & ~0x1Fu);
    g_xhci.db = g_xhci.cap + (mr32(g_xhci.cap, XHCI_DBOFF)  & ~0x3u);

    g_xhci.max_slots = XHCI_HCS1_MAXSLOTS(hcs1);
    g_xhci.max_intrs = XHCI_HCS1_MAXINTRS(hcs1);
    g_xhci.max_ports = XHCI_HCS1_MAXPORTS(hcs1);
    g_xhci.ac64      = (uint8_t)XHCI_HCC1_AC64(hcc1);
    g_xhci.ppc       = (uint8_t)XHCI_HCC1_PPC(hcc1);

    // CSZ decides whether a context structure is 32 or 64 bytes. QEMU
    // says 32 and much real hardware says 64; reading it wrong makes
    // every context field offset wrong, and nothing reports an error --
    // the controller simply parses garbage. Recorded here and consulted
    // by everything that builds a context.
    g_xhci.csz64 = (uint8_t)XHCI_HCC1_CSZ(hcc1);

    klog_printf("usb: xHCI controller %04x:%04x at %02x:%02x.%u, BAR0 0x%llx (0x%x bytes)\n",
                d->vendor_id, d->device_id, d->bus, d->device, d->function,
                (unsigned long long)bar0, g_xhci.cap_len);

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
    intel_pdo_log(d);

    if (!xhci_reset_controller()) {
        // THE MOST IMPORTANT BOOT TO INSTRUMENT IS THE ONE WHERE THIS
        // FAILS, and it used to be the only one with no instrumentation
        // at all. A controller left dirty by a previous boot's reset
        // refuses both the firmware handoff and this, and the machine
        // then has no USB, therefore no NIC, therefore no way to be
        // asked anything.
        klog_printf(KLOG_ERR "usb: giving up on the controller at probe -- "
                    "no USB this boot\n");
        g_xhci_dead_source.name = "usb-dead";
        g_xhci_dead_source.caps = 0;
        g_xhci_dead_source.poll = xhci_dead_heartbeat;
        input_register_source(&g_xhci_dead_source);
        return pci_probe_decline(d, "the controller would not reset");
    }

    // PAGESIZE is only meaningful after reset. Bit n set means 2^(n+12).
    uint32_t ps = mr32(g_xhci.op, XHCI_PAGESIZE) & 0xFFFFu;
    g_xhci.page_size = ps ? (uint32_t)(4096u * (ps & (uint32_t)(-(int32_t)ps))) : 4096u;

    USBT("usb: trace: op=+0x%x rt=+0x%x db=+0x%x ac64=%u csz64=%u\n",
         (unsigned)(g_xhci.op - g_xhci.cap), (unsigned)(g_xhci.rt - g_xhci.cap),
         (unsigned)(g_xhci.db - g_xhci.cap), g_xhci.ac64, g_xhci.csz64);

    uint32_t slots = g_xhci.max_slots;
    if (slots > XHCI_MAX_SLOTS) slots = XHCI_MAX_SLOTS;
    USBT("usb: trace: writing CONFIG=%u\n", slots);
    mw32(g_xhci.op, XHCI_CONFIG, slots);

    USBT("usb: trace: setup_rings\n");
    if (!setup_rings()) return pci_probe_decline(d, "no memory for its rings");
    USBT("usb: trace: rings done\n");

    // PPC IS ON THIS LINE BECAUSE A RECOVERY DEPENDS ON IT. Port Power
    // Control is what makes power_cycle_socket() -- the software replug
    // -- possible at all, and with it unlogged the only way to know
    // whether a machine has it was to read a failure that never
    // mentioned the reason.
    klog_printf("usb: %u slots (%u used), %u interrupters, %u ports, "
                "%u-byte contexts, %u-byte pages, port power %s\n",
                g_xhci.max_slots, slots, g_xhci.max_intrs, g_xhci.max_ports,
                g_xhci.csz64 ? 64u : 32u, g_xhci.page_size,
                g_xhci.ppc ? "software-controlled (PPC)" : "always on (no PPC)");
    // SAID, never silently cut: a port past the cap is one nothing scans.
    if (g_xhci.max_ports > XHCI_MAX_PORTS)
        klog_printf(KLOG_WARN "usb: only ports 1..%u are driven; %u..%u are ignored\n",
                    XHCI_MAX_PORTS, XHCI_MAX_PORTS + 1, g_xhci.max_ports);

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
    g_xhci.msi_vector = pci_msi_request(d, xhci_irq_handler);
    g_xhci.msix = d->irq_msix;

    if (g_xhci.msi_vector) {
        g_xhci.irq = 0;   // nothing on a line any more
    } else if (line == IRQ_NONE) {
        klog_printf("usb: no usable INTx line (pin reports %u) -- polling\n", line);
        g_xhci.irq = 0;
    } else {
        g_xhci.irq = line;
    }

    g_xhci.present = 1;
    if (g_xhci.irq) irq_register_handler(g_xhci.irq, xhci_irq_handler);

    g_xhci_source.name = "usb-xhci";
    g_xhci_source.caps = 0;              // the controller reports no events itself
    // Poll ALWAYS, IRQ or not -- see xhci_poll_source()'s comment. The
    // per-HID sources still leave poll NULL when the IRQ is live; their
    // decode rides this one.
    g_xhci_source.poll = xhci_poll_source;
    g_xhci_source.poll_on_wake = g_xhci.msi_vector != 0; // an MSI is not a dead line
    g_xhci_source.irq  = g_xhci.irq;
    g_xhci_source.msi_vector = g_xhci.msi_vector;
    input_register_source(&g_xhci_source);

    // Run before arming, so a device already attached raises its port
    // change against a controller that is actually running.
    mw32(g_xhci.op, XHCI_USBCMD, mr32(g_xhci.op, XHCI_USBCMD) | XHCI_CMD_RS);
    struct xhci_wait w; xhci_wait_start(&w, 100);
    while (mr32(g_xhci.op, XHCI_USBSTS) & XHCI_STS_HCH) {
        if (xhci_wait_over(&w)) {
            return pci_probe_decline(d, "the controller would not start (usbsts 0x%x)",
                                     mr32(g_xhci.op, XHCI_USBSTS));
        }
    }
    g_xhci.running = 1;

    // --- and enable last ----------------------------------------------
    //
    // The interrupter is armed the same way either way: what differs is
    // only how the controller signals. pci_msi_enable() has already
    // disabled the pin for the MSI case, which is why the INTx clear
    // below belongs to the line case alone.
    if (g_xhci.irq || g_xhci.msi_vector) {
        mw32(g_xhci.rt, XHCI_IR0 + XHCI_IMAN,
             mr32(g_xhci.rt, XHCI_IR0 + XHCI_IMAN) | XHCI_IMAN_IE);
        mw32(g_xhci.op, XHCI_USBCMD, mr32(g_xhci.op, XHCI_USBCMD) | XHCI_CMD_INTE);
    }
    if (g_xhci.irq) {
        pci_command_update(d, 0, PCI_CMD_INTX_DISABLE);   // the commit point
        pic_clear_mask(g_xhci.irq);
    }

    if (g_xhci.msi_vector)
        klog_printf("usb: running, %s on vector %u\n",
                    g_xhci.msix ? "MSI-X" : "MSI", g_xhci.msi_vector);
    else
        klog_printf("usb: running, %s\n",
                    g_xhci.irq ? "interrupt-driven" : "polled");

    xhci_log_socket_map();
    usb_query_init();
    xhci_power_ports();
    xhci_scan_ports();
    log_usb3_ports();
    return 0;
}

// --- introspection ----------------------------------------------------

int usb_controller_present(void) { return g_xhci.present; }
uint8_t usb_controller_irq(void) { return g_xhci.irq; }
uint8_t usb_controller_msi_vector(void) { return g_xhci.msi_vector; }

int usb_controller_summary(char *buf, uint32_t cap) {
    if (!g_xhci.present || !buf || !cap) return 0;
    k_snprintf(buf, cap, "xHCI %04x:%04x at %02x:%02x.%u, %u ports, %s",
               g_xhci.pci->vendor_id, g_xhci.pci->device_id,
               g_xhci.pci->bus, g_xhci.pci->device, g_xhci.pci->function,
               g_xhci.max_ports,
               !g_xhci.running ? "not running"
                 : g_xhci.msi_vector ? (g_xhci.msix ? "running (MSI-X)" : "running (MSI)")
                 : g_xhci.irq        ? "running"
                                   : "running (polled)");
    return 1;
}

uint32_t usb_events_seen(void) { return g_xhci.events_seen; }
uint32_t usb_irqs_seen(void)   { return g_xhci.irqs_seen; }

void usb_dump(void) {
    if (!g_xhci.present) {
        klog_printf("usb: no xHCI controller\n");
        return;
    }
    klog_printf("usb: usbcmd 0x%x usbsts 0x%x crcr-lo 0x%x config 0x%x\n",
                mr32(g_xhci.op, XHCI_USBCMD), mr32(g_xhci.op, XHCI_USBSTS),
                mr32(g_xhci.op, XHCI_CRCR), mr32(g_xhci.op, XHCI_CONFIG));
    klog_printf("usb: iman 0x%x erstsz %u erdp-lo 0x%x\n",
                mr32(g_xhci.rt, XHCI_IR0 + XHCI_IMAN),
                mr32(g_xhci.rt, XHCI_IR0 + XHCI_ERSTSZ),
                mr32(g_xhci.rt, XHCI_IR0 + XHCI_ERDP));
    klog_printf("usb: cmd ring 0x%llx enq %u cyc %u\n",
                (unsigned long long)g_xhci.cmd.phys, g_xhci.cmd.enqueue, g_xhci.cmd.cycle);
    klog_printf("usb: xfer ok %u bad %u (last cc %u \"%s\") orphan %u, "
                "%u ep recover(ies)\n",
                g_xhci.xfer_ok, g_xhci.xfer_bad, g_xhci.last_bad_code,
                xhci_completion_name(g_xhci.last_bad_code), g_xhci.xfer_orphan,
                g_xhci.ep_recoveries);
    for (int i = 0; i < MAX_EPS; i++) {
        struct xhci_ep *e = &g_xhci_eps[i];
        if (!e->in_use) continue;
        uint32_t rmask = 0;
        for (int b = 0; b < EP_DEPTH; b++) if (e->ready[b]) rmask |= (1u << b);
        // THE ENDPOINT'S OWN STATE, out of the DEVICE context the
        // controller writes -- not our idea of it. The difference
        // matters: a ring with TRBs on it and no completions is either
        // an endpoint that is not Running or a device with nothing to
        // send, and only this tells them apart.
        const char *st = "?";
        struct xhci_slot *sl = &g_xhci_slots[e->slot];
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
                (unsigned long long)g_xhci.evt.phys, g_xhci.evt.dequeue, g_xhci.evt.ccs,
                g_xhci.events_seen, g_xhci.irqs_seen);
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
    for (uint32_t p = 0; p < g_xhci.max_ports && p < XHCI_MAX_PORTS; p++) {
        uint32_t sc = mr32(g_xhci.op, XHCI_PORTSC(p));
        if (!(sc & XHCI_PORTSC_CCS) && !(sc & XHCI_PORTSC_PED)) continue;
        klog_printf("usb: port %u portsc 0x%x %s %s %s\n", p + 1, sc,
                    (sc & XHCI_PORTSC_CCS) ? "connected" : "empty",
                    (sc & XHCI_PORTSC_PED) ? "enabled" : "disabled",
                    xhci_speed_name((uint8_t)XHCI_PORTSC_SPEED(sc)));
    }
}
PCI_DRIVER("xhci", usb_matches, usb_probe);

