// The USB trace ring -- see usb_trace.h for why it prints nothing until
// something fails.
//
// driver-none: a diagnostic ring the USB stack writes into, not a
// driver -- it binds nothing and claims no device.
#include "usb_trace.h"
#include "xhci_regs.h"
#include "clocksource.h"
#include "timer.h"   // pit_ticks() -- the fallback when the clocksource cannot be trusted
#include "klog.h"
#include "kfmt.h"

// 64 events is about four ports' worth of enumeration, so a dump after
// the fourth port still has the first port's events behind it. Bigger
// would cost nothing in memory and everything in usefulness: the point
// is the events around a failure, not a history of the boot.
#define TRACE_N 64

// The most a single dump prints. A failing boot can lose four ports,
// and four unbounded dumps is the flood this file exists to avoid --
// so the cap is per dump and deliberately smaller than the ring.
#define TRACE_DUMP_MAX 20

struct ev {
    uint64_t ns;
    uint8_t  kind, port, slot;
    uint32_t a, b;
};

static struct ev g_ev[TRACE_N];
static uint32_t g_next;   // monotonic; index is g_next % TRACE_N

void usb_trace(uint8_t kind, uint8_t port, uint8_t slot,
               uint32_t a, uint32_t b) {
    struct ev *e = &g_ev[g_next % TRACE_N];
    // clocksource_now_ns() where it can be trusted -- the same test the
    // driver's waits use -- and PIT ticks scaled to ns where it cannot.
    // The fallback is 10 ms-grained and useless for ordering two events
    // in the same millisecond, but it still separates a timeout from
    // the transfer before it, which is the gap that matters. Returning
    // 0 for everything, as this first did, made every delta 0 and the
    // column worthless.
    e->ns   = clocksource_deadline_capable()
                ? clocksource_now_ns()
                : (uint64_t)pit_ticks() * 10000000ull;
    e->kind = kind;
    e->port = port;
    e->slot = slot;
    e->a    = a;
    e->b    = b;
    g_next++;
}

uint32_t usb_trace_mark(void) { return g_next; }

static const char *kind_name(uint8_t k) {
    switch (k) {
        case USB_TR_CMD:  return "cmd ";
        case USB_TR_CTRL: return "ctrl";
        case USB_TR_PORT: return "port";
        default:          return "note";
    }
}

void usb_trace_dump(uint32_t mark, uint8_t port) {
    uint32_t have = g_next - mark;            // wraps correctly, both uint32
    if (!have) return;
    if (have > TRACE_N) have = TRACE_N;       // the ring ate the rest
    uint32_t shown = have > TRACE_DUMP_MAX ? TRACE_DUMP_MAX : have;
    uint32_t first = g_next - shown;

    // THE CONTROLLER'S OWN STATE, which no per-port trace can show and
    // which would explain a whole class of failure on its own: HCH says
    // it halted, HSE that it took a host system error, CNR that it is
    // not answering. Any of those makes every port after the first
    // failure fail for a reason that has nothing to do with the device
    // -- and until now nothing here looked.
    uint32_t sts = xhci_usbsts();
    // "the last N events", not "port N's events": the ring is global,
    // and that is deliberate -- when one port's failure is followed by
    // another's, what happened BETWEEN them is the evidence, and a
    // per-port filter would throw exactly that away.
    klog_printf("usb: port %u gave up; last %u event(s); usbsts 0x%x%s%s%s\n",
                port, shown, sts,
                (sts & XHCI_STS_HCH) ? " HALTED" : "",
                (sts & XHCI_STS_HSE) ? " HOST-SYSTEM-ERROR" : "",
                (sts & XHCI_STS_CNR) ? " NOT-READY" : "");

    uint64_t base = g_ev[first % TRACE_N].ns;
    for (uint32_t i = first; i != g_next; i++) {
        const struct ev *e = &g_ev[i % TRACE_N];
        // Microseconds since the first event shown: the GAPS are the
        // point -- a 1000 ms step is a timeout, and a long quiet stretch
        // before one says the controller stopped answering rather than
        // the device.
        uint32_t us = e->ns > base ? (uint32_t)((e->ns - base) / 1000) : 0;
        klog_printf("usb:   +%u us p%u s%u %s %#x -> %#x\n",
                    us, e->port, e->slot, kind_name(e->kind), e->a, e->b);
    }
}
