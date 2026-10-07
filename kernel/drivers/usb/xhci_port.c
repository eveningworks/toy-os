// The xHCI driver's root ports: which USB2 and USB3 ports are one socket,
// resets and the recovery ladder, attach and detach, and the deferred
// work that runs them. See xhci_internal.h for where the rest of the driver is.
#include "xhci_internal.h"

// driver-none: part of the xHCI driver, which xhci.c declares

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
// is not a proof, which is why xhci_log_socket_map() prints the whole map at
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
    int c = xhci_companion_in(g_xhci.usb2.first, g_xhci.usb2.count,
                              g_xhci.usb3.first, g_xhci.usb3.count, p + 1);
    return c ? c - 1 : -1;
}

int xhci_companion_port(unsigned port) {
    if (!g_xhci.present || !port || port > XHCI_MAX_PORTS) return 0;
    return xhci_companion_in(g_xhci.usb2.first, g_xhci.usb2.count,
                             g_xhci.usb3.first, g_xhci.usb3.count, port);
}

// Prints the derived map once, so it can be CHECKED rather than
// trusted. A pairing this driver guessed wrong is otherwise invisible
// until it reports the wrong socket for a device somebody is looking
// at.
void xhci_log_socket_map(void) {
    if (!g_xhci.usb2.count || !g_xhci.usb3.count) {
        klog_printf("usb: no socket map -- USB%s port range not declared\n",
                    g_xhci.usb2.count ? "3" : "2");
        return;
    }
    for (uint32_t i = 0; i < g_xhci.usb3.count; i++) {
        uint32_t ss = g_xhci.usb3.first + i;
        int hs = xhci_companion_port(ss);
        if (hs) klog_printf("usb: socket %u = usb%d + usb%u\n", i + 1, hs, ss);
    }
}

static void reset_port(uint32_t p, int force) {
    uint32_t sc = mr32(g_xhci.op, XHCI_PORTSC(p));
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

    // A SUPERSPEED PORT TAKES A *WARM* RESET, NOT A HOT ONE. PR is the
    // USB2 reset; USB3 defines WPR (bit 31), and it is the only one
    // that re-trains the link and returns the device to Default. This
    // matters because a port the FIRMWARE enabled is in a state we
    // never put it in -- this controller comes from a BIOS handoff --
    // and Address Device to such a device hangs it (docs/bugs.md).
    uint32_t rst = (g_xhci.ports[p].speed == XHCI_SPEED_SUPER)
                 ? XHCI_PORTSC_WPR : XHCI_PORTSC_PR;
    portsc_write(p, rst, XHCI_PORTSC_CSC);

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
        uint32_t v = mr32(g_xhci.op, XHCI_PORTSC(p));
        if (v & XHCI_PORTSC_PED) break;         // reset succeeded
        if (!(v & XHCI_PORTSC_CCS)) return;     // device left mid-reset
        if (xhci_wait_over(&w)) {
            klog_printf("usb: port %u reset did not complete, portsc 0x%x\n",
                        p + 1, v);
            return;
        }
    }
    // WRC is the warm reset's own change bit; PRC is the hot one's.
    // Acknowledge both rather than branching -- a bit that was never
    // raised is a harmless RW1C write.
    portsc_write(p, 0, XHCI_PORTSC_PRC | XHCI_PORTSC_WRC |
                       XHCI_PORTSC_CSC | XHCI_PORTSC_PEC);

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

// HOW MANY FAILED EPISODES A PORT GETS before it is left alone. Each
// episode is ATTACH_ATTEMPTS tries plus a warm reset, a mux cycle and a
// re-attach, and all of that spins without yielding -- so this is a
// bound on how much of the machine one dead device may consume.
#define XHCI_MAX_GIVEUPS   3

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
    pend_clear(&g_xhci.detach_pending, p);
    if (usb_root_port_slot((uint8_t)(p + 1))) {
        klog_printf("usb: port %u: tearing down the device the cycle "
                    "disconnected\n", p + 1);
        usb_detach_root_port((uint8_t)(p + 1));
    }
    // ...and the recorded state goes with it, so a port-change event
    // that lands late reads CCS=1 against `connected == 0` and queues
    // the same attach rather than seeing no transition at all.
    g_xhci.ports[p].connected = 0;

    if (!(mr32(g_xhci.op, XHCI_PORTSC(p)) & XHCI_PORTSC_CCS)) return;
    klog_printf("usb: port %u: still connected after the cycle -- "
                "re-attaching\n", p + 1);
    pend_set(&g_xhci.attach_pending, p);
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
// Only ever a USB3 port: WPR is meaningless on a USB2 one, and asserting
// a reserved bit is not a diagnostic.
static int is_usb3_port(uint32_t p) {
    return p < 32 && (g_xhci.usb3_ports & (1u << p));
}

static void warm_reset_companion(uint32_t p) {
    int c = companion_port(p);
    if (c < 0) return;
    uint32_t cp = (uint32_t)c;
    if (!is_usb3_port(cp)) return;

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
        uint32_t v = mr32(g_xhci.op, XHCI_PORTSC(cp));
        if (v & XHCI_PORTSC_WRC) break;
        if (xhci_wait_over(&w)) {
            klog_printf("usb: port %u warm reset did not complete, "
                        "portsc 0x%x\n", cp + 1, v);
            return;
        }
    }
    portsc_write(cp, 0, XHCI_PORTSC_WRC | XHCI_PORTSC_CSC | XHCI_PORTSC_PEC);
    // The device re-attaches on ONE of the two ports, and which is its
    // decision. Both are left to xhci_scan_ports(), which is already the one
    // place that notices a connect -- doing it here would be a second
    // attach path, and this driver has been bitten by two of those.
    xhci_delay_ms(20);
    klog_printf("usb: port %u: after warm reset portsc 0x%x; companion "
                "port %u portsc 0x%x\n",
                cp + 1, mr32(g_xhci.op, XHCI_PORTSC(cp)),
                p + 1, mr32(g_xhci.op, XHCI_PORTSC(p)));
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
    const struct pci_device *d = g_xhci.pci;
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
    if (g_xhci.usb2.count && p + 1 >= g_xhci.usb2.first &&
        p + 1 < (uint32_t)g_xhci.usb2.first + g_xhci.usb2.count)
        hs_bit = 1u << (p + 1 - g_xhci.usb2.first);
    if (c >= 0 && g_xhci.usb3.count && (uint32_t)c + 1 >= g_xhci.usb3.first &&
        (uint32_t)c + 1 < (uint32_t)g_xhci.usb3.first + g_xhci.usb3.count)
        ss_bit = 1u << ((uint32_t)c + 1 - g_xhci.usb3.first);

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
                mr32(g_xhci.op, XHCI_PORTSC(p)));
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
    // CCS -- xhci_power_ports() waits the same way after the initial power-on.
    xhci_delay_ms(100);

    // The connect that follows is a REAL one, so the change bits are
    // acknowledged and the attach is left to xhci_scan_ports() -- the one
    // place that notices a connect. A second attach path here is how
    // this driver has been bitten twice before.
    portsc_rmw(p, 0, 0, XHCI_PORTSC_CSC | XHCI_PORTSC_PEC);
    if (c >= 0) portsc_rmw((uint32_t)c, 0, 0, XHCI_PORTSC_CSC | XHCI_PORTSC_PEC);
    klog_printf("usb: port %u: after power cycle portsc 0x%x\n", p + 1,
                mr32(g_xhci.op, XHCI_PORTSC(p)));
    if (c >= 0)
        klog_printf("usb: port %u: companion after power cycle portsc 0x%x\n",
                    (uint32_t)c + 1, mr32(g_xhci.op, XHCI_PORTSC((uint32_t)c)));
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
    if (g_xhci.ports[p].power_cycled) {
        klog_printf("usb: port %u: already replugged this episode\n", p + 1);
        return 0;
    }
    g_xhci.ports[p].power_cycled = 1;
    if (g_xhci.ppc) { power_cycle_socket(p); return 1; }
    klog_printf("usb: port %u: no Port Power Control (PPC=0) -- trying the "
                "port mux instead\n", p + 1);
    if (intel_mux_cycle(p)) return 1;
    klog_printf("usb: port %u: nothing left to try -- this controller has "
                "neither port power nor a port mux\n", p + 1);
    return 0;
}

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

// HOW MANY CONTROLLER RE-INITS A BOOT MAY HAVE. This was a boolean
// latch, so the FIRST re-init disarmed recovery for the rest of the
// boot -- and on a machine whose only NIC is a USB device that is the
// difference between a bad boot and an unreachable one. Bounded rather
// than unlimited because a controller that needs a fourth re-init is
// not going to be talked round, and a re-init loop would be worse than
// the wedge.
#define XHCI_MAX_REINITS 3
static int g_hc_resets;
static int g_hc_reset_refused;

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
// so the link partner re-trains from scratch. xhci_reset_controller() does
// both, with deadline-bounded waits -- which matters here because this
// runs from a syscall with interrupts off, and a wait that cannot end
// would leave a machine unable to reboot at all. That is strictly worse
// than the bug, so it is the one property this must not lose.
void usb_shutdown(void) {
    if (!g_xhci.present || !g_xhci.running) return;
    klog_write("usb: halting the controller before the machine restarts\n");
    if (!xhci_reset_controller())
        klog_write(KLOG_WARN "usb: controller would not halt -- restarting anyway\n");
    g_xhci.running = 0;
}

// ARMS the re-init; xhci_deferred_work() performs it. Returns 1 for
// "accepted", which is all a caller can be told -- the work happens
// later, off the interrupt-disabled path that asked for it.
int usb_controller_reinit(void) {
    if (!g_xhci.present || !g_xhci.running) return 0;
    if (g_xhci.hcreset_pending) return 0;
    if (g_hc_resets >= XHCI_MAX_REINITS) {
        // ONCE. This is asked on every later command timeout, and a
        // line per refusal is the probe outrunning the log.
        if (!g_hc_reset_refused) {
            g_hc_reset_refused = 1;
            g_xhci.wedged = 1;
            klog_printf(KLOG_ERR "usb: controller re-init refused -- %d "
                        "already this boot. Giving up on the controller: "
                        "%u command(s) never completed, and every further "
                        "attempt only takes the CPU from the rest of the "
                        "machine\n", g_hc_resets, g_xhci.timeouts);
        }
        return 0;
    }
    g_xhci.hcreset_pending = 1;
    klog_printf("usb: controller re-init armed -- runs off the event path\n");
    return 1;
}

static void hcreset_perform(void) {
    if (g_hc_resets >= XHCI_MAX_REINITS) return;
    g_hc_resets++;
    klog_printf(KLOG_WARN "usb: RE-INITIALISING THE CONTROLLER\n");

    for (uint32_t p = 0; p < g_xhci.max_ports && p < XHCI_MAX_PORTS; p++) {
        if (mr32(g_xhci.op, XHCI_PORTSC(p)) & XHCI_PORTSC_CCS)
            usb_detach_root_port((uint8_t)(p + 1));
    }
    g_xhci.detach_pending = 0;
    g_xhci.running = 0;
    HCSTAGE("detached");

    if (!xhci_reset_controller()) {
        klog_printf(KLOG_ERR "usb: re-init FAILED at the reset\n");
        return;
    }
    HCSTAGE("reset done");

    uint32_t slots = g_xhci.max_slots;
    if (slots > XHCI_MAX_SLOTS) slots = XHCI_MAX_SLOTS;
    mw32(g_xhci.op, XHCI_CONFIG, slots);
    for (uint32_t i = 1; i <= slots; i++) g_xhci.dcbaa[i] = 0;
    HCSTAGE("config + dcbaa");

    xhci_program_rings();
    HCSTAGE("rings programmed");

    mw32(g_xhci.op, XHCI_USBCMD, mr32(g_xhci.op, XHCI_USBCMD) | XHCI_CMD_RS);
    struct xhci_wait w; xhci_wait_start(&w, 100);
    while (mr32(g_xhci.op, XHCI_USBSTS) & XHCI_STS_HCH) {
        if (xhci_wait_over(&w)) {
            klog_printf(KLOG_ERR "usb: will not restart (usbsts 0x%x)\n",
                        mr32(g_xhci.op, XHCI_USBSTS));
            return;
        }
    }
    g_xhci.running = 1;
    HCSTAGE("running");

    if (g_xhci.irq || g_xhci.msi_vector) {
        mw32(g_xhci.rt, XHCI_IR0 + XHCI_IMAN,
             mr32(g_xhci.rt, XHCI_IR0 + XHCI_IMAN) | XHCI_IMAN_IE);
        mw32(g_xhci.op, XHCI_USBCMD, mr32(g_xhci.op, XHCI_USBCMD) | XHCI_CMD_INTE);
    }
    for (uint32_t p = 0; p < XHCI_MAX_PORTS; p++) {
        g_xhci.ports[p].connected = 0;
        g_xhci.ports[p].enabled = 0;
        g_xhci.ports[p].speed = 0;
    }
    HCSTAGE("interrupter armed");

    xhci_power_ports();
    HCSTAGE("ports powered");

    for (uint32_t p = 0; p < g_xhci.max_ports && p < XHCI_MAX_PORTS; p++) {
        klog_printf("usb: hcreset scanning port %u (portsc 0x%x)\n",
                    p + 1, mr32(g_xhci.op, XHCI_PORTSC(p)));
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
        // SUPERSPEED IS NO LONGER EXCLUDED. It was, because asserting
        // PR on such a port is wrong -- but reset_port() now issues a
        // WARM reset there, which is the right instrument, and a
        // SuperSpeed port that arrives ENABLED from a BIOS handoff was
        // otherwise never reset at all, not even on a retry. That is
        // the state in which Address Device hangs this controller
        // (docs/bugs.md). Attempt 0 read the speed.
        int force = attempt != 0;
        uint8_t was_speed = g_xhci.ports[p].speed;
        reset_port(p, force);
        uint32_t sc = mr32(g_xhci.op, XHCI_PORTSC(p));
        g_xhci.ports[p].connected = (sc & XHCI_PORTSC_CCS) ? 1 : 0;
        g_xhci.ports[p].enabled   = (sc & XHCI_PORTSC_PED) ? 1 : 0;
        g_xhci.ports[p].speed     = (uint8_t)XHCI_PORTSC_SPEED(sc);
        if (!g_xhci.ports[p].connected) return;
        // THE SPEED ON EVERY ATTEMPT, not just the first. A device that
        // comes up at the WRONG speed is the sharpest lead this bug has
        // (docs/bugs.md): the gigabit adapter enumerated `full-speed` on
        // a failing boot and `high-speed` on working ones, which is a
        // high-speed CHIRP that did not happen during reset. The retry
        // used to log nothing, so nobody could see whether the forced
        // reset re-negotiated it -- which is exactly the question.
        if (attempt == 0) {
            klog_printf("usb: port %u: connected, %s, %s (portsc 0x%x)\n",
                        p + 1, xhci_speed_name(g_xhci.ports[p].speed),
                        g_xhci.ports[p].enabled ? "enabled" : "not enabled", sc);
        } else {
            usb_trace(USB_TR_PORT, (uint8_t)(p + 1), 0, sc, (uint32_t)attempt);
            klog_printf("usb: port %u: retry %d %s, %s (portsc 0x%x)%s\n",
                        p + 1, attempt, xhci_speed_name(g_xhci.ports[p].speed),
                        g_xhci.ports[p].enabled ? "enabled" : "not enabled", sc,
                        // Called out rather than left to be diffed: a
                        // speed that CHANGES across a reset is the chirp
                        // succeeding the second time, and it is the one
                        // observation that would turn the lead into a
                        // cause.
                        g_xhci.ports[p].speed != was_speed
                            ? "  <- SPEED CHANGED" : "");
        }
        if (!g_xhci.ports[p].enabled) return;
        // `attempt` is the patience flag too: the first try is fast,
        // and a device that has already failed is asked again slowly.
        if (usb_enumerate_port((uint8_t)(p + 1), g_xhci.ports[p].speed,
                               attempt) >= 0) {
            g_xhci.ports[p].power_cycled = 0;   // this episode ended well
            g_xhci.ports[p].giveups     = 0;    // ...so the count starts over
            g_xhci.ports[p].warm_tries  = 0;
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
                    xhci_speed_name(g_xhci.ports[p].speed));
        // ONLY ON THE WAY OUT. A retry that is about to try again does
        // not need its history printed; a port being lost does, and it
        // is the one case where the log lines are worth their space.
        if (gave_up) {
            usb_trace_dump(trace_mark, (uint8_t)(p + 1));
            // ABANDON A PORT THAT WILL NOT COME UP, rather than recover
            // it round the same circle again. Every lever below takes
            // the CPU for hundreds of milliseconds in a spin that does
            // not yield, so an unbounded loop is not merely futile --
            // it starves the machine.
            if (++g_xhci.ports[p].giveups >= XHCI_MAX_GIVEUPS) {
                klog_printf(KLOG_ERR "usb: port %u: %u failed episodes -- "
                            "abandoning it until something enumerates "
                            "there\n", p + 1, g_xhci.ports[p].giveups);
                return;
            }
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
            if (mr32(g_xhci.op, XHCI_PORTSC(p)) & XHCI_PORTSC_CCS) {
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
// be able to say "the controller halted" without reaching into g_xhci.
uint32_t xhci_usbsts(void) {
    if (!g_xhci.present) return 0;
    return mr32(g_xhci.op, XHCI_USBSTS);
}

int usb_diag_replug_port(unsigned port) {
    if (!g_xhci.running) return 0;
    if (port < 1 || port > g_xhci.max_ports || port > XHCI_MAX_PORTS) return 0;
    // NOT gated on PPC: software_replug() picks whichever lever this
    // controller has and says so, and refusing here would hide the
    // answer to the question the knob exists to ask.
    //
    // QUEUED for the same reason the forced reset is: the caller is a
    // syscall with interrupts off, and this waits on coarse_ticks().
    pend_set(&g_xhci.diag_power_pending, port - 1);
    klog_printf("usb: port %u: replug queued\n", port);
    return 1;
}

int usb_diag_reset_port(unsigned port) {
    if (!g_xhci.running) return 0;
    if (port < 1 || port > g_xhci.max_ports || port > XHCI_MAX_PORTS) return 0;
    // QUEUED, NEVER DONE HERE. The caller is a syscall (kernel.usb_reset)
    // and this work spins on coarse_ticks(), which only the timer interrupt
    // advances -- see diag_reset_pending. Returns 1 for "accepted"; the
    // OUTCOME is in the log a moment later, because there is nobody left
    // to return it to.
    pend_set(&g_xhci.diag_reset_pending, port - 1);
    klog_printf("usb: port %u: forced reset queued\n", port);
    return 1;
}

// The queued reset, run from deferred work with interrupts on.
static void diag_reset_port(uint32_t p) {
    uint32_t before = mr32(g_xhci.op, XHCI_PORTSC(p));
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
    uint32_t sc = mr32(g_xhci.op, XHCI_PORTSC(p));
    g_xhci.ports[p].connected = (sc & XHCI_PORTSC_CCS) ? 1 : 0;
    g_xhci.ports[p].enabled   = (sc & XHCI_PORTSC_PED) ? 1 : 0;
    g_xhci.ports[p].speed     = (uint8_t)XHCI_PORTSC_SPEED(sc);
    klog_printf("usb: port %u: after reset %s, %s (portsc 0x%x)\n", port,
                xhci_speed_name(g_xhci.ports[p].speed),
                g_xhci.ports[p].enabled ? "enabled" : "not enabled", sc);
    if (!g_xhci.ports[p].connected || !g_xhci.ports[p].enabled) return;

    // PATIENT: a forced reset is a diagnostic on a device that has
    // already misbehaved, so there is nothing to be gained by being
    // quick about it.
    int rc = usb_enumerate_port((uint8_t)port, g_xhci.ports[p].speed, 1);
    klog_printf("usb: port %u: forced re-enumeration %s\n", port,
                rc >= 0 ? "SUCCEEDED" : "FAILED");
}

// On a controller with Port Power Control, ports come out of reset
// UNPOWERED: CCS never rises on a port nobody powered, so a connected
// mouse reads as an empty port with nothing logged anywhere. QEMU
// reports PPC=0, which is how this stayed unwritten for the driver's
// whole QEMU life.
void xhci_power_ports(void) {
    if (!g_xhci.ppc) return;
    int powered = 0;
    for (uint32_t p = 0; p < g_xhci.max_ports && p < XHCI_MAX_PORTS; p++) {
        if (mr32(g_xhci.op, XHCI_PORTSC(p)) & XHCI_PORTSC_PP) continue;
        portsc_write(p, XHCI_PORTSC_PP, 0);
        powered = 1;
    }
    if (!powered) return;
    klog_printf("usb: ports powered on (PPC)\n");
    // Power-good plus the USB2 attach debounce, before the scan reads
    // CCS. A minimum, like reset_port()'s recovery wait.
    clocksource_delay_ms(100);
}

void xhci_scan_ports(void) {
    for (uint32_t p = 0; p < g_xhci.max_ports && p < XHCI_MAX_PORTS; p++)
        attach_root_port(p);
}

// ALIVE-AT, AND ONLY ON A BOOT THAT IS ALREADY BROKEN. A machine that
// resets with nothing logged leaves the time of death unknown to within
// the log's sync window, and that window is the only evidence there is.
// This pins it to a second. It would be pure noise on a healthy boot,
// so it runs solely once the controller has been given up on -- which
// is exactly the boot that resets.
static void wedged_heartbeat(void) {
    static uint64_t last;
    if (!g_xhci.wedged) return;
    uint64_t now = clocksource_now_ns();
    if (now - last < 1000000000ull) return;
    last = now;
    // WHAT THE HARDWARE SAYS, not just that we are alive. USBSTS.HSE is
    // the controller's own "I hit something fatal" bit and nothing in
    // this driver has ever read it; the PCI status register carries the
    // master/target abort and SERR bits. A machine that resets with a
    // perfectly regular heartbeat, a clean memory audit and no
    // over-current has to be leaving a mark SOMEWHERE, and these are
    // the two registers nobody has looked at.
    uint32_t sts = mr32(g_xhci.op, XHCI_USBSTS);
    uint32_t pcists = g_xhci.pci
                    ? (pci_config_read32(g_xhci.pci, 0x04) >> 16) : 0;
    klog_printf(KLOG_WARN "usb: alive at %llu s, given up on "
                "(%u timeout(s)) usbsts 0x%x%s pcistatus 0x%x\n",
                (unsigned long long)(now / 1000000000ull), g_xhci.timeouts,
                sts, (sts & XHCI_STS_HSE) ? " HOST-SYSTEM-ERROR" : "",
                pcists);
}

static const char *pls_name(uint32_t sc) {
    static const char *const n[16] = {
        "U0", "U1", "U2", "U3", "Disabled", "RxDetect", "SS.Inactive", "Polling",
        "Recovery", "Hot Reset", "Compliance", "Test", "?", "?", "?", "Resume",
    };
    return n[XHCI_PORTSC_PLS(sc)];
}

// A USB3 LINK THAT FAILED, with the device still plugged in: SS.Inactive
// or Compliance (xhci_portsc_needs_warm()). The port stays dark until a
// warm reset retrains it -- a replug does not -- and this is Linux's
// response too (hub_port_warm_reset_required()). If the device answers,
// it re-enters through attach_pending like any connect. Bounded by
// XHCI_MAX_WARM tries between working enumerations.
#define XHCI_MAX_WARM 3
static void link_recover(uint32_t p) {
    uint32_t sc = mr32(g_xhci.op, XHCI_PORTSC(p));
    if (!is_usb3_port(p) || !xhci_portsc_needs_warm(sc)) return;
    if (g_xhci.ports[p].warm_tries >= XHCI_MAX_WARM) {
        if (g_xhci.ports[p].warm_tries == XHCI_MAX_WARM) {   // once; saturates
            g_xhci.ports[p].warm_tries++;
            klog_printf(KLOG_ERR "usb: port %u: link still %s after %d warm "
                        "resets -- giving up until a device enumerates there\n",
                        p + 1, pls_name(sc), XHCI_MAX_WARM);
        }
        return;
    }
    g_xhci.ports[p].warm_tries++;
    klog_printf("usb: port %u: link %s -- warm reset %u of %d\n", p + 1,
                pls_name(sc), g_xhci.ports[p].warm_tries, XHCI_MAX_WARM);
    portsc_write(p, XHCI_PORTSC_WPR, XHCI_PORTSC_CSC);
    // NOT WRC ALONE: the port-change interrupt acknowledges every change
    // bit, WRC included, and can do so before this loop reads it (QEMU
    // completes the reset inside the write). PR is not a change bit: it
    // clears when the reset ends, and the link has then left 6/10.
    struct xhci_wait w; xhci_wait_start(&w, 500);
    for (;;) {
        uint32_t now = mr32(g_xhci.op, XHCI_PORTSC(p));
        if ((now & XHCI_PORTSC_WRC) ||
            (!(now & XHCI_PORTSC_PR) && !xhci_portsc_needs_warm(now)))
            break;
        if (xhci_wait_over(&w)) {
            klog_printf("usb: port %u: warm reset did not complete, portsc 0x%x\n",
                        p + 1, mr32(g_xhci.op, XHCI_PORTSC(p)));
            return;
        }
    }
    portsc_write(p, 0, XHCI_PORTSC_WRC | XHCI_PORTSC_CSC | XHCI_PORTSC_PEC |
                       XHCI_PORTSC_PRC | XHCI_PORTSC_PLC);
    xhci_delay_ms(20);
    sc = mr32(g_xhci.op, XHCI_PORTSC(p));
    klog_printf("usb: port %u: after warm reset portsc 0x%x, link %s%s\n", p + 1,
                sc, pls_name(sc), (sc & XHCI_PORTSC_CCS) ? " -- device back" : "");
    if (sc & XHCI_PORTSC_CCS) {
        // The change bits were cleared above, so no event will announce
        // it: queue it the way note_port_change() would have.
        g_xhci.ports[p].connected = 1;
        pend_set(&g_xhci.attach_pending, p);
    }
}

void xhci_deferred_work(void) {
    // BEFORE the running check: a controller that has been given up on
    // is usually no longer `running`, and that is precisely the boot
    // this needs to time.
    wedged_heartbeat();
    if (!g_xhci.present || !g_xhci.running) return;

    // FIRST, and it takes every device with it -- so nothing below
    // should run against the state it is about to replace.
    if (g_xhci.hcreset_pending) {
        g_xhci.hcreset_pending = 0;
        hcreset_perform();
        return;
    }

    for (uint32_t p = 0; p < g_xhci.max_ports && p < XHCI_MAX_PORTS; p++) {
        if (g_xhci.detach_pending & (1u << p)) {
            pend_clear(&g_xhci.detach_pending, p);
            // A REAL DISCONNECT ENDS THE EPISODE, so a device unplugged
            // and plugged back in gets the power cycle again if it needs
            // it -- the flag is "already tried for THIS device", not
            // "tried once ever".
            g_xhci.ports[p].power_cycled = 0;
            // The link state says WHY: RxDetect is an unplug, SS.Inactive
            // a failed link with the device still there.
            uint32_t gone = g_xhci.ports[p].gone_sc;
            // A real unplug ends the warm-reset episode too, so the next
            // device in this socket gets its tries even after a give-up.
            if (XHCI_PORTSC_PLS(gone) == XHCI_PLS_RXDETECT) g_xhci.ports[p].warm_tries = 0;
            klog_printf("usb: port %u: device removed (portsc 0x%x, link %s)\n",
                        p + 1, gone, pls_name(gone));
            usb_detach_root_port((uint8_t)(p + 1));
        }
        if (g_xhci.warm_pending & (1u << p)) {
            pend_clear(&g_xhci.warm_pending, p);
            link_recover(p);
        }
        // The forced diagnostic reset (kernel.usb_reset). Before the
        // attach below, so a port that is pending both is reset once
        // deliberately rather than attached and then reset under it.
        if (g_xhci.diag_power_pending & (1u << p)) {
            pend_clear(&g_xhci.diag_power_pending, p);
            // FORCED, so the once-per-episode guard is cleared first:
            // the operator asking for it is the whole point, and a
            // refusal saying "already cycled" would be answering a
            // question nobody asked.
            g_xhci.ports[p].power_cycled = 0;
            software_replug(p);
        }
        if (g_xhci.diag_reset_pending & (1u << p)) {
            pend_clear(&g_xhci.diag_reset_pending, p);
            diag_reset_port(p);
        }
        if (g_xhci.attach_pending & (1u << p)) {
            pend_clear(&g_xhci.attach_pending, p);
            // The bring-up ITSELF raises a connect change for a device
            // that was there all along, before xhci_scan_ports() has
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
        struct xhci_ep *e = &g_xhci_eps[i];
        if (!e->in_use || !e->halted) continue;
        if (e->recover_tries > 4) continue;   // gave up; logged below once
        e->recover_tries++;
        int rc = xhci_recover_halted(e->slot, dci_of(e->ep_addr), &e->ring);
        if (rc == 0) {
            // Everything in flight was abandoned by the dequeue move,
            // so rebuild the posted set from scratch. Queued-but-unread
            // reports are dropped with it: they are stale by the width
            // of an error anyway.
            e->next_take = 0;
            for (int b = 0; b < EP_DEPTH; b++) { e->ready[b] = 0; e->ready_len[b] = 0; }
            e->halted = 0;
            for (uint8_t b = 0; b < EP_DEPTH; b++) xhci_ep_post(e, b);
            klog_printf("usb: slot %u ep 0x%x recovered from halt\n",
                        e->slot, e->ep_addr);
        } else if (e->recover_tries > 4) {
            klog_printf(KLOG_ERR "usb: slot %u ep 0x%x halt recovery failed (%s) -- giving up\n",
                        e->slot, e->ep_addr, xhci_completion_name((uint32_t)-rc));
        }
    }
}

