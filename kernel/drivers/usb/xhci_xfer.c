// The xHCI driver's transfers: device contexts, the command ring, control,
// interrupt, bulk and isochronous endpoints, and the event ring that
// completes them. See xhci_internal.h for where the rest of the driver is.
#include "xhci_internal.h"

// driver-none: part of the xHCI driver, which xhci.c declares

// --- contexts ---------------------------------------------------------
//
// A context entry is 32 OR 64 bytes, and which one is HCCPARAMS1.CSZ.
// QEMU says 32; a great deal of real hardware says 64. Reading it wrong
// puts every field at the wrong offset and the controller reports
// nothing at all -- it simply parses garbage. So no code here indexes a
// context by a constant; everything goes through ctx_at().

// Ringing a doorbell is what tells the controller to look at a ring.
// Slot 0 target 0 is the command ring; slot N target DCI is that
// device's endpoint.
static void ring_doorbell(uint32_t slot, uint32_t target) {
    // The ring writes must be visible before the doorbell, or the
    // controller reads a TRB that is not there yet. Free on x86, but
    // the compiler still has to be told -- see barrier.h.
    kbarrier();
    *(volatile uint32_t *)(g_xhci.db + slot * 4) = target;
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
// ONCE A CONTROLLER HAS MISSED ONE DEADLINE, IT DOES NOT DESERVE THE
// FULL ONE AGAIN. A healthy command completes in microseconds, so the
// second is for a controller that is merely slow -- and a controller
// that has already failed to answer is not slow, it is sick. Measured
// on the ASUS with a USB3 hub attached: 39 command timeouts in one
// boot, 39 SECONDS of a spin that does not yield, across six ports,
// which is what made the machine take seven seconds to open a menu.
// Shortening the repeat turns that into under two.
#define XHCI_WAIT_MS       1000
#define XHCI_WAIT_SICK_MS    50

static int wait_completion(volatile struct xhci_completion *c, const char *what) {
    uint32_t ms = g_xhci.cmd_recovered ? XHCI_WAIT_SICK_MS : XHCI_WAIT_MS;
    struct xhci_wait w; xhci_wait_start(&w, ms);
    while (!c->done) {
        xhci_service();
        if (xhci_wait_over(&w)) {
            // ONE LINE PER CONTROLLER, not per command. 39 of these is
            // the probe outrunning the log CLAUDE.md warns about, and
            // the tally below is what a reader actually needs.
            if (!g_xhci.timeout_reported) {
                g_xhci.timeout_reported = 1;
                klog_printf(KLOG_ERR "usb: %s timed out after %u polls (%s)"
                            " -- later ones are counted, not logged\n",
                            what, w.spins,
                            w.deadline ? "deadline" : "poll ceiling, no usable clock");
            }
            g_xhci.timeouts++;
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
    uint64_t crcr = (uint64_t)mr32(g_xhci.op, XHCI_CRCR) |
                    ((uint64_t)mr32(g_xhci.op, XHCI_CRCR + 4) << 32);
    // RCS IS NOT REPORTED, AND THAT IS DELIBERATE: xHCI 5.4.5 makes
    // CRCR bits 2:0 and the pointer field read as ZERO, so printing
    // RCS=0 forever only invited the conclusion that the controller had
    // lost the cycle state. CRR is the one bit that does read back.
    // `base` is the DRIVER's own idea of the ring; the controller's is
    // unreadable, so the two cannot be compared here -- say which is
    // which rather than implying agreement.
    klog_printf(KLOG_ERR
                "usb: cmd ring %s: crcr 0x%llx (CRR=%u) enq %u cyc %u "
                "our base 0x%llx usbsts 0x%x\n",
                when, (unsigned long long)crcr,
                (unsigned)((crcr & XHCI_CRCR_CRR) ? 1 : 0),
                g_xhci.cmd.enqueue, g_xhci.cmd.cycle,
                (unsigned long long)g_xhci.cmd.phys, mr32(g_xhci.op, XHCI_USBSTS));
    // The first three command TRBs' control dwords: a ring the
    // controller is parsing differently shows up here as cycle bits
    // that do not match what we believe we published.
    klog_printf(KLOG_ERR "usb: cmd ring trb[0..2] ctrl %#x %#x %#x\n",
                g_xhci.cmd.trb[0].control, g_xhci.cmd.trb[1].control,
                g_xhci.cmd.trb[2].control);
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
    mw64(g_xhci.op, XHCI_CRCR, g_xhci.cmd.phys |
                              (g_xhci.cmd.cycle ? XHCI_CRCR_RCS : 0) |
                              XHCI_CRCR_CA);

    // The controller stops asynchronously and says so with a Command
    // Completion event (CC 24, Command Ring Stopped). Waiting on CRR
    // rather than on that event is deliberate -- the event may be the
    // very thing that is not arriving, and CRR is a register read that
    // cannot be starved.
    struct xhci_wait w; xhci_wait_start(&w, 50);
    for (;;) {
        xhci_service();                       // drain whatever it does post
        uint64_t crcr = (uint64_t)mr32(g_xhci.op, XHCI_CRCR) |
                        ((uint64_t)mr32(g_xhci.op, XHCI_CRCR + 4) << 32);
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
    xhci_ring_init(&g_xhci.cmd, (void *)g_xhci.cmd.trb, g_xhci.cmd.phys,
                   g_xhci.cmd.count, 0);
    mw64(g_xhci.op, XHCI_CRCR, g_xhci.cmd.phys | XHCI_CRCR_RCS);
    cmd_ring_report("after abort");
    return 1;
}

int xhci_selftest_cmd_recovery(void);   // below cmd_submit; see its comment

// Enqueues one command, rings doorbell 0 and waits for its Command
// Completion event. Returns the completion code; `out_slot` receives
// the slot id the controller assigned, when the command allocates one.
static int cmd_submit(uint64_t param, uint32_t control, uint8_t *out_slot) {
    // A CONTROLLER THAT HAS BEEN GIVEN UP ON IS NOT ASKED AGAIN. Without
    // this the driver keeps queueing commands nothing will answer, at a
    // spin apiece, for the life of the boot -- which is how one dead
    // hub made a laptop take seven seconds to open a menu.
    if (g_xhci.wedged) return -XHCI_CC_INVALID - 1;

    // ARMED BEFORE THE PUSH, and the order is load-bearing.
    // xhci_ring_push() sets the cycle bit last, which is what hands the
    // TRB to the controller -- and a controller with CRR=1 may fetch it
    // without waiting for the doorbell. A completion drained in that
    // window would find g_xhci_cmd_done.trb still holding the PREVIOUS
    // command's address, be rejected as stale, and cost a full 1000 ms
    // timeout on a command that actually succeeded.
    g_xhci_cmd_done.trb  = xhci_ring_enq_phys(&g_xhci.cmd);
    g_xhci_cmd_done.done = 0;
    uint64_t at = xhci_ring_push(&g_xhci.cmd, param, 0, control);
    // The two must agree, or the waiter is armed for a TRB the
    // controller will never report -- every command would then time
    // out, which is a failure mode worth naming rather than deducing.
    if (at != g_xhci_cmd_done.trb) {
        klog_printf(KLOG_ERR "usb: command armed for %#lx but pushed at "
                    "%#lx -- ring bookkeeping disagrees\n",
                    (unsigned long)g_xhci_cmd_done.trb, (unsigned long)at);
        g_xhci_cmd_done.trb = at;
    }
    ring_doorbell(0, 0);

    // The injector fakes the WAIT, not the controller: the command was
    // really posted above and the controller really will run it. That
    // is enough to drive the report and the abort/restart below against
    // a live ring, and is NOT a reproduction of the stalled-ring fault
    // (fault_inject.h says so at more length).
    if (fault_should_fail_usb_command() ||
        wait_completion(&g_xhci_cmd_done, "command") < 0) {
        usb_trace(USB_TR_CMD, 0, 0, XHCI_TRB_TYPE(control), 0xFFu); // timeout
        // ONCE PER CONTROLLER, not once per command: the failure mode
        // this exists for produces a timeout on every command for the
        // rest of the boot, and a report plus an abort on each of them
        // would be the probe outrunning the log that CLAUDE.md warns
        // about -- the evidence destroyed by the instrument.
        if (!g_xhci.cmd_recovered) {
            cmd_ring_report("timeout");
            // 1 = the ring is running again, 2 = the abort did not take.
            // Recording WHICH, not merely that this ran: a flag set
            // before the attempt would be satisfied by an abort that
            // failed, and the self-test below would then pass on a
            // controller left exactly as broken as it found it.
            g_xhci.cmd_recovered = cmd_ring_recover() ? 1 : 2;
            if (g_xhci.cmd_recovered == 1)
                klog_printf(KLOG_ERR "usb: command ring aborted and "
                                      "restarted after a timeout\n");
        } else {
            // THE RING WAS ALREADY PUT BACK AND A COMMAND STILL TIMED
            // OUT, so what is wedged is the CONTROLLER and not the
            // ring. Measured on the ASUS: the abort leaves `trb[0..2]
            // ctrl 0 0 0` and CRR clear -- a clean ring -- and the next
            // command times out regardless. Escalate to a re-init,
            // which is bounded inside and refuses once it has had
            // enough. Arming is just a flag; it runs off the event
            // path, which is why this is safe to call from here.
            usb_controller_reinit();
        }
        return -XHCI_CC_INVALID - 1;
    }
    if (out_slot) *out_slot = g_xhci_cmd_done.slot;
    usb_trace(USB_TR_CMD, 0, g_xhci_cmd_done.slot,
              XHCI_TRB_TYPE(control), g_xhci_cmd_done.code);
    return (int)g_xhci_cmd_done.code;
}

// TURN A COMMAND RESULT INTO A FAILURE RETURN, and never by negating it
// blindly. A completion code is POSITIVE (xHCI's own); this driver's
// timeout is the NEGATIVE sentinel from cmd_submit() above. `-cc` on
// that sentinel produced +1, and every caller tests `< 0` -- so a
// timed-out Enable Slot was read as "you were given slot 1" and the
// enumeration went on to drive, and then DISABLE, a slot belonging to
// some other device. Live on the bare-metal ASUS: it is what puts the
// `-1` in that machine's `short device descriptor (-1)`.
static inline int xhci_fail(int cc) {
    return cc < 0 ? cc : (cc ? -cc : -1);
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
    if (!g_xhci.op) return -1;

    uint8_t saved = g_xhci.cmd_recovered;
    g_xhci.cmd_recovered = 0;             // the guard is once-per-boot; re-arm it

    fault_fail_next_usb_commands(1);
    (void)cmd_submit(0, XHCI_TRB_SET_TYPE(XHCI_TRB_NOOP_CMD), 0);
    fault_fail_next_usb_commands(0);    // a test disarms what it arms

    int fired = (g_xhci.cmd_recovered == 1);   // ran AND restarted the ring
    // THE ASSERTION THAT MATTERS IS THIS ONE. Recovery that runs and
    // leaves the ring unusable is worse than no recovery, because the
    // log then says it healed something it did not.
    int cc = cmd_submit(0, XHCI_TRB_SET_TYPE(XHCI_TRB_NOOP_CMD), 0);

    g_xhci.cmd_recovered = saved;
    return (fired && cc == XHCI_CC_SUCCESS) ? 1 : 0;
}

int xhci_address_device(uint8_t root_port, uint32_t route, uint8_t speed,
                        uint8_t tt_slot, uint8_t tt_port) {
    uint8_t slot = 0;
    int cc = cmd_submit(0, XHCI_TRB_SET_TYPE(XHCI_TRB_ENABLE_SLOT), &slot);
    if (cc != XHCI_CC_SUCCESS) {
        klog_printf(KLOG_ERR "usb: enable slot failed: %s\n", xhci_completion_name((uint32_t)cc));
        return xhci_fail(cc);
    }
    if (!slot || slot > XHCI_MAX_SLOTS) {
        klog_printf("usb: controller assigned slot %u, out of range\n", slot);
        return -1;
    }

    // in_use from the moment the controller knows the slot, so every
    // failure below can hand cleanup to xhci_disable_slot() -- the
    // failed attempts used to leak their slots, and the retry then
    // burned a fresh one per try.
    struct xhci_slot *sl = &g_xhci_slots[slot];
    sl->in_use = 1;
    uint64_t ep0_phys = 0;
    sl->in_ctx  = xhci_alloc_frame(&sl->in_ctx_phys);
    sl->out_ctx = xhci_alloc_frame(&sl->out_ctx_phys);
    void *ep0_seg = xhci_alloc_frame(&ep0_phys);
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
    g_xhci.dcbaa[slot] = sl->out_ctx_phys;

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
        return xhci_fail(cc);
    }
    return slot;
}

int xhci_set_ep0_mps(uint8_t slot, uint16_t mps) {
    if (!slot || slot > XHCI_MAX_SLOTS || !g_xhci_slots[slot].in_use) return -1;
    struct xhci_slot *sl = &g_xhci_slots[slot];

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
        return xhci_fail(cc);
    }
    return 0;
}

// Reset Endpoint, then Set TR Dequeue Pointer: the two commands that
// bring a HALTED endpoint back. The dequeue is pointed at the ring's
// current ENQUEUE with its current cycle state -- everything the
// controller had in flight is abandoned, and the caller re-posts what
// it wants outstanding.
int xhci_recover_halted(uint8_t slot, uint32_t dci, struct xhci_ring *ring) {
    int cc = cmd_submit(0, XHCI_TRB_SET_TYPE(XHCI_TRB_RESET_ENDPOINT) |
                           ((uint32_t)slot << 24) | (dci << 16), 0);
    if (cc != XHCI_CC_SUCCESS) return xhci_fail(cc);
    uint64_t deq = ring->phys + (uint64_t)ring->enqueue * sizeof(struct xhci_trb);
    cc = cmd_submit(deq | (ring->cycle ? 1u : 0u),
                    XHCI_TRB_SET_TYPE(XHCI_TRB_SET_TR_DEQUEUE) |
                    ((uint32_t)slot << 24) | (dci << 16), 0);
    if (cc != XHCI_CC_SUCCESS) return xhci_fail(cc);
    g_xhci.ep_recoveries++;
    return 0;
}

void xhci_slot_set_hub(uint8_t slot, uint8_t n_ports, uint8_t ttt) {
    if (!slot || slot > XHCI_MAX_SLOTS || !g_xhci_slots[slot].in_use) return;
    volatile uint32_t *sc = ctx_at(g_xhci_slots[slot].in_ctx, 1);
    sc[0] |= (1u << 26);                                        // Hub flag
    sc[1] = (sc[1] & 0x00FFFFFFu) | ((uint32_t)n_ports << 24);  // Number of Ports
    sc[2] = (sc[2] & ~(3u << 16)) | (((uint32_t)ttt & 3u) << 16);
}

int xhci_slot_usable(uint8_t slot) {
    return slot && slot <= XHCI_MAX_SLOTS && g_xhci_slots[slot].in_use && !g_xhci_slots[slot].ep0_dead;
}

int xhci_control(uint8_t slot, const uint8_t setup[8],
                 void *buf, uint16_t len, int in) {
    if (!slot || slot > XHCI_MAX_SLOTS || !g_xhci_slots[slot].in_use) return -1;
    struct xhci_slot *sl = &g_xhci_slots[slot];
    if (sl->ep0_dead) return -1;

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
    g_xhci_xfer_done.done = 0;
    g_xhci_xfer_done.data_short = 0;
    g_xhci_xfer_done.residual = 0;
    g_xhci_xfer_done.ring_lo = sl->ep0.phys;
    g_xhci_xfer_done.ring_hi = sl->ep0.phys +
                          (uint64_t)sl->ep0.count * sizeof(struct xhci_trb);
    uint64_t status_trb = xhci_ring_push(&sl->ep0, 0, 0,
                              XHCI_TRB_SET_TYPE(XHCI_TRB_STATUS_STAGE) |
                              ((len && in) ? 0 : (1u << 16)) | XHCI_TRB_IOC);
    g_xhci_xfer_done.trb = status_trb;

    ring_doorbell(slot, dci_of(0));

    if (wait_completion(&g_xhci_xfer_done, "control transfer") < 0) {
        g_xhci_xfer_done.ring_lo = g_xhci_xfer_done.ring_hi = 0;
        // NOT CANCELLED, so ep0 is wedged behind it: refuse what follows
        // rather than let a driver's poll spend a deadline per read --
        // an RTL8156 bind once held USB's only worker for minutes this way.
        sl->ep0_dead = 1;
        klog_printf(KLOG_WARN "usb: slot %u: control endpoint stuck -- "
                    "refused until the device is set up again\n", slot);
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
              g_xhci_xfer_done.code);
    if (g_xhci_xfer_done.code != XHCI_CC_SUCCESS &&
        g_xhci_xfer_done.code != XHCI_CC_SHORT_PACKET) {
        // A STALL is a legitimate answer to a request the device does
        // not support (SET_IDLE, commonly; a class request addressed to
        // an entity it does not have) -- and every code here HALTS ep0,
        // so without the reset the next control transfer to the device
        // fails too and one refused request kills the whole
        // enumeration. QEMU reaches this through the transaction-error
        // path rather than STALL, which is why the set is not just the
        // one code.
        if (g_xhci_xfer_done.code == XHCI_CC_STALL ||
            g_xhci_xfer_done.code == XHCI_CC_BABBLE ||
            g_xhci_xfer_done.code == XHCI_CC_USB_TRANSACTION_ERR ||
            g_xhci_xfer_done.code == XHCI_CC_DATA_BUFFER_ERROR)
            xhci_recover_halted(slot, 1, &sl->ep0);
        return -(int)g_xhci_xfer_done.code;
    }
    // A short packet is not an error -- it is how a device says "that is
    // all there was", and the residual says how much less it sent.
    uint32_t got = len;
    if (g_xhci_xfer_done.residual <= len) got = len - g_xhci_xfer_done.residual;
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
struct xhci_ep g_xhci_eps[MAX_EPS];

static struct xhci_ep *ep_find(uint8_t slot, uint8_t ep_addr) {
    for (int i = 0; i < MAX_EPS; i++)
        if (g_xhci_eps[i].in_use && g_xhci_eps[i].slot == slot && g_xhci_eps[i].ep_addr == ep_addr)
            return &g_xhci_eps[i];
    return 0;
}

// Which endpoint owns the TRB a Transfer Event names. Address ranges
// rather than an id, because that is all the event carries.
static struct xhci_ep *ep_owning(uint64_t trb_phys) {
    for (int i = 0; i < MAX_EPS; i++) {
        struct xhci_ep *e = &g_xhci_eps[i];
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
        if (g_xhci_eps[i].in_use && g_xhci_eps[i].slot == slot &&
            dci_of(g_xhci_eps[i].ep_addr) == dci)
            return &g_xhci_eps[i];
    return 0;
}

void xhci_ep_post(struct xhci_ep *e, uint8_t slice) {
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
        for (int i = 0; i < MAX_EPS; i++) if (!g_xhci_eps[i].in_use) { e = &g_xhci_eps[i]; break; }
        if (!e) return 0;
        seg = xhci_alloc_frame(&ring_phys);
        if (!seg) return 0;
    }

    k_memset(e, 0, sizeof *e);
    e->slot = slot; e->ep_addr = ep_addr; e->mps = mps;
    k_memset(seg, 0, 4096);   // a reused segment still holds old TRBs
    xhci_ring_init(&e->ring, seg, ring_phys, TRBS_PER_RING, 0);

    struct xhci_slot *sl = &g_xhci_slots[slot];
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
    if (!slot || slot > XHCI_MAX_SLOTS || !g_xhci_slots[slot].in_use) return -1;
    if (mps == 0 || mps > EP_SLOT_SIZE) return -1;

    uint64_t buf_phys = 0;
    void *buf = xhci_alloc_frame(&buf_phys);
    if (!buf) return -1;

    int cc = 0;
    struct xhci_ep *e = ep_configure(slot, ep_addr, mps, interval, 7, 3, &cc);
    if (!e) return xhci_fail(cc);
    e->buf = (uint8_t *)buf;
    e->buf_phys = buf_phys;

    for (uint8_t i = 0; i < EP_DEPTH; i++) xhci_ep_post(e, i);
    return 0;
}

int xhci_add_isoch_out(uint8_t slot, uint8_t ep_addr, uint16_t mps,
                       uint8_t interval, void (*done)(void *ctx, uint32_t bytes),
                       void *ctx) {
    if (!slot || slot > XHCI_MAX_SLOTS || !g_xhci_slots[slot].in_use) return -1;
    if (mps == 0 || (ep_addr & 0x80)) return -1;   // OUT endpoints only

    int cc = 0;
    // CErr = 0: an isochronous transfer is not retried, and the spec
    // has the controller ignore the field on such an endpoint anyway.
    struct xhci_ep *e = ep_configure(slot, ep_addr, mps, interval, 1, 0, &cc);
    if (!e) return xhci_fail(cc);
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
    if (!slot || slot > XHCI_MAX_SLOTS || !g_xhci_slots[slot].in_use) return -1;
    if (mps == 0) return -1;

    int cc = 0;
    uint32_t type = (ep_addr & 0x80) ? 6u : 2u;
    // bInterval is meaningless for bulk -- the controller moves data
    // whenever there is bandwidth -- and 0 is what the spec wants.
    struct xhci_ep *e = ep_configure(slot, ep_addr, mps, 0, type, 3, &cc);
    if (!e) return xhci_fail(cc);
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

    // **A RING-3 DRIVER THAT OVER-POSTS MUST NOT BE ABLE TO LAP THE
    // RING**, which silences the endpoint for good (xhci_ring_room()).
    // Refused instead, as Linux's xhci does (room_on_ring()).
    if (!xhci_ring_room(&e->ring)) {
        if (!e->iso_refused++)
            klog_printf(KLOG_ERR "usb: isoch ep 0x%x (slot %u) transfer ring full "
                        "-- refusing posts\n", ep_addr, slot);
        return -2;
    }

    int irq = ioc;
    if (ioc) e->iso_since_ioc = 0;
    else if (++e->iso_since_ioc >= ISO_FORCE_IOC) { irq = 1; e->iso_since_ioc = 0; }

    // BEFORE the push: the push hands the TRB over, and a running
    // endpoint can complete it before the next line runs.
    uint32_t idx = (uint32_t)((xhci_ring_enq_phys(&e->ring) - e->ring.phys) /
                              sizeof(struct xhci_trb));
    if (idx < TRBS_PER_RING) e->iso_want_ioc[idx] = (uint8_t)(ioc ? 1 : 0);

    // SIA rather than a Frame ID: the alternative is tracking the
    // controller's own frame counter and predicting one interval ahead,
    // which buys nothing for a stream that is simply continuous.
    xhci_ring_push(&e->ring, buf_phys, len & 0x1FFFFu,
                   XHCI_TRB_SET_TYPE(XHCI_TRB_ISOCH) | XHCI_TRB_SIA |
                   (irq ? XHCI_TRB_IOC : 0));
    ring_doorbell(e->slot, dci_of(e->ep_addr));
    return 0;
}

uint32_t xhci_isoch_underruns(uint8_t slot, uint8_t ep_addr) {
    struct xhci_ep *e = ep_find(slot, ep_addr);
    return e ? e->iso_underruns : 0;
}

uint32_t xhci_isoch_refused(uint8_t slot, uint8_t ep_addr) {
    struct xhci_ep *e = ep_find(slot, ep_addr);
    return e ? e->iso_refused : 0;
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
    xhci_ep_post(e, i);            // straight back into circulation
    return (int)len;
}

void xhci_disable_slot(uint8_t slot) {
    if (!slot || slot > XHCI_MAX_SLOTS || !g_xhci_slots[slot].in_use) return;
    struct xhci_slot *sl = &g_xhci_slots[slot];

    // The controller first: Disable Slot stops every endpoint, so the
    // frames below have stopped being DMA targets before they are
    // freed.
    int cc = cmd_submit(0, XHCI_TRB_SET_TYPE(XHCI_TRB_DISABLE_SLOT) |
                           ((uint32_t)slot << 24), 0);

    // AND IF IT DID NOT ANSWER, THE FRAMES ARE NOT OURS TO TAKE BACK.
    // This used to free them anyway, reasoning that a device is already
    // gone and a leak is cheaper. That holds for an unplug and NOT for
    // a stalled command ring, where the controller is still running
    // (CRR=1) and may write into them at any time -- and xhci_alloc_frame()
    // takes these from PMM_ZONE_DMA32, the same zone paging.c allocates
    // PAGE TABLES from. A late DMA into what has become a PML4 is an
    // instant, unlogged reset, and docs/bugs.md records exactly that on
    // the bare-metal ASUS. Quarantining a few frames is the cheaper
    // failure by a very wide margin.
    int quarantine = (cc != XHCI_CC_SUCCESS);
    if (quarantine)
        klog_printf(KLOG_ERR "usb: disable slot %u: %s -- keeping its DMA "
                    "frames, the controller may still be writing them\n",
                    slot, xhci_completion_name((uint32_t)cc));

    for (int i = 0; i < MAX_EPS; i++) {
        struct xhci_ep *e = &g_xhci_eps[i];
        if (!e->in_use || e->slot != slot) continue;
        e->in_use = 0;   // unpublished before its memory goes away
        if (quarantine) continue;
        pmm_free_contiguous(e->ring.phys, 1);
        // An isochronous endpoint has no buffer of ours -- the driver
        // owns it. Freeing "frame zero" would hand real memory back.
        if (e->buf_phys) pmm_free_contiguous(e->buf_phys, 1);
    }

    g_xhci.dcbaa[slot] = 0;
    // Guarded: the address-failure path arrives here with some of these
    // never allocated, and freeing "frame zero" would free real memory.
    if (!quarantine) {
        if (sl->in_ctx_phys)  pmm_free_contiguous(sl->in_ctx_phys, 1);
        if (sl->out_ctx_phys) pmm_free_contiguous(sl->out_ctx_phys, 1);
        if (sl->ep0.phys)     pmm_free_contiguous(sl->ep0.phys, 1);
    }
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
    uint32_t sts = mr32(g_xhci.op, XHCI_USBSTS);
    if (!(sts & XHCI_STS_EINT)) return 0;    // "was it me?" -- it was not

    uint32_t iman = mr32(g_xhci.rt, XHCI_IR0 + XHCI_IMAN);
    if (iman & XHCI_IMAN_IP)
        mw32(g_xhci.rt, XHCI_IR0 + XHCI_IMAN, (iman & XHCI_IMAN_IE) | XHCI_IMAN_IP);

    mw32(g_xhci.op, XHCI_USBSTS, XHCI_STS_EINT);
    return 1;
}

static void note_port_change(void) {
    for (uint32_t p = 0; p < g_xhci.max_ports && p < XHCI_MAX_PORTS; p++) {
        uint32_t sc = mr32(g_xhci.op, XHCI_PORTSC(p));
        uint32_t ack = sc & XHCI_PORTSC_RW1C;

        // OVER-CURRENT WAS BEING CLEARED WITHOUT EVER BEING READ. OCC
        // rides in the RW1C mask above, so the platform could be
        // shedding a port's power and nothing here would say so -- and
        // this controller reports no Port Power Control, so we cannot
        // shed it ourselves either. Worth a line because the symptom it
        // would explain (a machine that resets with a bus-powered hub
        // attached and nothing logged) is otherwise indistinguishable
        // from a software fault. Once per port per episode.
        if (sc & (XHCI_PORTSC_OCA | XHCI_PORTSC_OCC)) {
            if (!g_xhci.ports[p].oc_reported) {
                g_xhci.ports[p].oc_reported = 1;
                klog_printf(KLOG_ERR "usb: port %u: OVER-CURRENT (portsc "
                            "0x%x, active=%u) -- the platform is limiting "
                            "this port\n", p + 1, sc,
                            (sc & XHCI_PORTSC_OCA) ? 1u : 0u);
            }
        } else {
            g_xhci.ports[p].oc_reported = 0;
        }

        if (ack) portsc_write(p, 0, ack);

        uint8_t was = g_xhci.ports[p].connected;
        uint8_t now = (sc & XHCI_PORTSC_CCS) ? 1 : 0;
        g_xhci.ports[p].connected = now;
        g_xhci.ports[p].enabled   = (sc & XHCI_PORTSC_PED) ? 1 : 0;
        g_xhci.ports[p].speed     = (uint8_t)XHCI_PORTSC_SPEED(sc);

        // Hot-plug is DEFERRED, not done here: this runs inside the
        // event drain, and enumeration is synchronous control transfers
        // that would deadlock on the single-consumer guard. A plug
        // while the machine is busy still lands, because the pending
        // bit survives until the poll gets to it.
        if (now && !was) pend_set(&g_xhci.attach_pending, p);
        if (!now && was) {
            g_xhci.ports[p].gone_sc = sc;
            pend_set(&g_xhci.detach_pending, p);
            pend_clear(&g_xhci.attach_pending, p);   // it left before we got there
        }
        if (!now && xhci_portsc_needs_warm(sc)) pend_set(&g_xhci.warm_pending, p);
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
    if (!g_xhci.present || !g_xhci.running) return;
    if (g_in_service) return;
    g_in_service = 1;

    struct xhci_trb ev;
    int drained = 0;
    while (xhci_ring_event_pop(&g_xhci.evt, &ev)) {
        g_xhci.events_seen++;
        drained = 1;
        uint32_t type = XHCI_TRB_TYPE(ev.control);
        uint64_t src  = (uint64_t)ev.p0 | ((uint64_t)ev.p1 << 32);
        uint32_t code = (ev.status >> 24) & 0xFFu;

        if (type == XHCI_TRB_PORT_STATUS_CHANGE) {
            note_port_change();
        } else if (type == XHCI_TRB_CMD_COMPLETION) {
            // MATCHED ON ITS TRB, exactly as a Transfer Event is below.
            // This arm used to overwrite g_xhci_cmd_done.trb and set done
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
            if (src == g_xhci_cmd_done.trb) {
                g_xhci_cmd_done.code = code;
                g_xhci_cmd_done.slot = (uint8_t)((ev.control >> 24) & 0xFFu);
                g_xhci_cmd_done.done = 1;
            } else {
                g_xhci.cmd_stale++;
                // THE CODE AND THE TYPE ARE THE WHOLE DIAGNOSIS. Without
                // them this line cannot tell a genuinely stale event
                // from CC 24 (Command Ring Stopped), which the abort
                // path generates DELIBERATELY and whose TRB Pointer is
                // the ring's dequeue pointer rather than any completed
                // command -- so it lands here and reads as a fault.
                klog_printf("usb: stale command completion for trb %#lx "
                            "(waiting on %#lx) cc=%u type=%u -- ignored\n",
                            (unsigned long)src,
                            (unsigned long)g_xhci_cmd_done.trb,
                            (unsigned)code,
                            (unsigned)XHCI_TRB_TYPE(ev.control));
            }
        } else if (type == XHCI_TRB_TRANSFER_EVENT) {
            // A Transfer Event names the TRB that finished. Control
            // transfers wait on their Status Stage TRB; interrupt
            // endpoints are matched by the HID layer, which lands next.
            if (!g_xhci_xfer_done.done && src != g_xhci_xfer_done.trb &&
                code == XHCI_CC_SHORT_PACKET && g_xhci_xfer_done.ring_hi &&
                src >= g_xhci_xfer_done.ring_lo && src < g_xhci_xfer_done.ring_hi) {
                // The data stage came up short: note by how much, and
                // keep waiting for the Status Stage (see data_short).
                g_xhci_xfer_done.residual   = ev.status & 0xFFFFFFu;
                g_xhci_xfer_done.data_short = 1;
            } else if (src == g_xhci_xfer_done.trb ||
                (!g_xhci_xfer_done.done && g_xhci_xfer_done.ring_hi &&
                 src >= g_xhci_xfer_done.ring_lo && src < g_xhci_xfer_done.ring_hi)) {
                g_xhci_xfer_done.code = (code == XHCI_CC_SUCCESS && g_xhci_xfer_done.data_short)
                                   ? XHCI_CC_SHORT_PACKET : code;
                if (!g_xhci_xfer_done.data_short)
                    g_xhci_xfer_done.residual = ev.status & 0xFFFFFFu;
                g_xhci_xfer_done.slot     = (uint8_t)((ev.control >> 24) & 0xFFu);
                g_xhci_xfer_done.ring_lo  = 0;
                g_xhci_xfer_done.ring_hi  = 0;
                g_xhci_xfer_done.done     = 1;
            } else if (code == XHCI_CC_RING_UNDERRUN ||
                       code == XHCI_CC_RING_OVERRUN) {
                // No TRB pointer on these -- the endpoint is named by
                // the event itself. An isochronous stream that has
                // stopped being fed says exactly this, once, and it is
                // not an error to recover from.
                struct xhci_ep *e = ep_by_dci((uint8_t)((ev.control >> 24) & 0xFFu),
                                              (ev.control >> 16) & 0x1Fu);
                if (e) e->iso_underruns++;
                else   g_xhci.xfer_orphan++;
            } else {
                struct xhci_ep *e = ep_owning(src);
                if (!e) g_xhci.xfer_orphan++;
                else if (code != XHCI_CC_SUCCESS && code != XHCI_CC_SHORT_PACKET) {
                    g_xhci.xfer_bad++; g_xhci.last_bad_code = code;
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
                } else g_xhci.xfer_ok++;
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
                    // How far the controller has provably got, which is
                    // what xhci_isoch_post() measures its room against.
                    xhci_ring_consumed(&e->ring, src);
                    uint32_t idx = (uint32_t)((src - e->ring.phys) /
                                              sizeof(struct xhci_trb));
                    int want = idx < TRBS_PER_RING ? e->iso_want_ioc[idx] : 1;
                    // The driver refills and re-posts from here, inside
                    // the drain. That is safe because posting touches
                    // only its own transfer ring and a doorbell -- never
                    // the event ring this loop owns.
                    uint32_t resid = ev.status & 0xFFFFFFu;
                    uint32_t got = resid <= e->mps ? e->mps - resid : 0;
                    if (want && e->iso_done) e->iso_done(e->iso_ctx, got);
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
        mw64(g_xhci.rt, XHCI_IR0 + XHCI_ERDP,
             xhci_ring_erdp(&g_xhci.evt) | XHCI_ERDP_EHB);
    }
}

void xhci_irq_handler(uint64_t *regs) {
    (void)regs;
    if (!g_xhci.present) return;
    // The "was it me?" must come first and must be cheap: this line is
    // shared, so this runs on every interrupt any device on it raises.
    if (!ack_interrupt()) return;
    g_xhci.irqs_seen++;
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
void xhci_poll_source(void) {
    xhci_service();
    usb_hid_service_all();
    usb_hub_service();
    xhci_deferred_work();
    // The given-up heartbeat is the one deferred job due at a time.
    if (g_xhci.wedged) clockevent_idle_wake_by(clocksource_now_ns() + 1000000000ull);
}

struct input_source g_xhci_source;

// A SECOND SOURCE, FOR A CONTROLLER THAT NEVER CAME UP AT ALL. The real
// one polls hardware, so it cannot be registered before the reset
// succeeds -- which left the boot that matters most with no
// instrumentation whatever: `usb_probe()` returns early when the reset
// fails, and the machine then ran a minute and reset with the log
// ending at 3.7 s and nothing to say why. This touches no registers.
struct input_source g_xhci_dead_source;

void xhci_dead_heartbeat(void) {
    static uint64_t last;
    uint64_t now = clocksource_now_ns();
    if (now - last < 1000000000ull) return;
    last = now;
    klog_printf(KLOG_WARN "usb: alive at %llu s -- the controller never "
                "initialised, nothing here is driving it\n",
                (unsigned long long)(now / 1000000000ull));
}

