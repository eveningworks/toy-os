// /lib/snd/usbaudio.so -- USB Audio Class playback, driven from ring 3.
//
// THE ONE PLUGIN THAT READS THE SAMPLES, and the reason
// SYS_SND_RING_MAP exists. A PCI sound card DMAs straight out of the
// core's ring: hda.so and ac97.so are told a PHYSICAL address and never
// see a sample. USB cannot work that way -- every packet must be copied
// out of the ring and CONVERTED to the device's sample width before it
// goes on the wire. So this one maps the ring READ-ONLY and does that
// work, which is what the maintainer weighed and accepted.
//
// AND THE HOST CONTROLLER IS NOT OURS. A claimed PCI device is handed
// over whole; a USB device is one of many on a bus the kernel keeps,
// so every transfer here is a syscall the kernel performs. That is
// Linux's usbfs, which is what libusb sits on.
//
// THE DESCRIPTOR WALK IS NOT IN THIS FILE. kernel/lib/usb_audio_parse.c
// is compiled for ring 3 as well, so the configuration this reads is
// parsed by the same implementation the kernel's driver uses -- and it
// is the untrusted half, which is the whole argument for being here.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "snd_driver.h"
#include "sound_abi.h"
#include "usb_audio_parse.h"
#include "query_abi.h"
#include "syscall_abi.h"
#include "rt/sys.h"

// Requests, addressed the way usb_audio_parse's notes describe.
#define TYPE_OUT_STD_IF   0x01
#define TYPE_OUT_CLASS_IF 0x21
#define REQ_SET_INTERFACE 0x0B
#define TYPE_IN_CLASS_IF  0xA1
#define UAC2_REQ_CUR      0x01
#define CS_SAM_FREQ       0x01
#define CX_CLOCK_SELECT   0x01

// PACKETS ENOUGH THAT ONE WAKEUP CAN OUTRUN THE ENDPOINT. The kernel
// driver fits its packets in a single frame because it refills from
// the interrupt handler and only ever needs the next one. A process
// turns around ~180 times a second here, and an endpoint at 125 us
// eats 8000 packets in that second -- so a wakeup has to queue ~45 to
// break even, and the buffer has to hold several times that.
#define PKT_BYTES 16384

static struct {
    int      slot;
    struct usb_audio_stream s;
    uint16_t frames;      // the rate's share of ONE service interval
    uint16_t ring_bytes;  // what that costs in the s16 ring
    uint16_t wire_bytes;  // ...and on the wire, which differ above 16 bits
    uint8_t  group;       // packets per completion
    uint8_t  packets;     // how many fit in the frame
    uint8_t  next;        // the packet slot the next completion refills

    uint8_t *pkt;         // the granted buffer, ours to write
    const uint8_t *ring;  // the core's, READ-ONLY to us
    uint32_t copy_pos;    // ring offset of the next packet to copy
    uint32_t play_pos;    // ring offset the hardware has reached
    int running;
} g;

// --- the format conversion, the kernel driver's shape ------------------

static void write_sample(uint8_t *dst, int16_t v, uint8_t subslot) {
    uint16_t u = (uint16_t)v;
    if (subslot == 2) { dst[0] = (uint8_t)u; dst[1] = (uint8_t)(u >> 8); }
    else if (subslot == 3) { dst[0] = 0; dst[1] = (uint8_t)u; dst[2] = (uint8_t)(u >> 8); }
    else { dst[0] = 0; dst[1] = 0; dst[2] = (uint8_t)u; dst[3] = (uint8_t)(u >> 8); }
}

static void copy_one_packet(uint8_t slot) {
    uint8_t *dst = g.pkt + (uint32_t)slot * g.wire_bytes;
    uint32_t at = g.copy_pos;

    if (g.s.subslot == 2) {
        // THE RING WRAPS MID-PACKET on most laps -- a packet divides
        // neither the ring nor a chunk -- so this is two copies.
        uint32_t n = g.ring_bytes, first = SND_RING_BYTES - at;
        if (first > n) first = n;
        memcpy(dst, g.ring + at, first);
        if (first < n) memcpy(dst + first, g.ring, n - first);
        g.copy_pos = (at + n) % SND_RING_BYTES;
        return;
    }
    // Sample at a time, because the widths differ. `at` is always even
    // and the ring is a whole number of frames, so a sample never
    // straddles the wrap and only the step has to check it.
    uint32_t samples = (uint32_t)g.frames * SND_CHANNELS;
    for (uint32_t i = 0; i < samples; i++) {
        int16_t v = (int16_t)((uint16_t)g.ring[at] | ((uint16_t)g.ring[at + 1] << 8));
        write_sample(dst, v, g.s.subslot);
        dst += g.s.subslot;
        at += 2;
        if (at >= SND_RING_BYTES) at = 0;
    }
    g.copy_pos = at;
}

// --- control requests --------------------------------------------------

static int set_interface(uint8_t ifnum, uint8_t alt) {
    uint8_t s[8] = { TYPE_OUT_STD_IF, REQ_SET_INTERFACE, alt, 0, ifnum, 0, 0, 0 };
    return sys_usb_control(g.slot, s, 0, 0, 0);
}

// THE ENTITY THE RATE IS SET ON, which is NOT always the one the
// streaming interface names.
//
// A CLOCK SELECTOR IS NOT A CLOCK SOURCE, and setting a rate on one
// fails with nothing to see. The G6 names selector 17, whose current
// pin is source 16 -- and asking the selector which pin it is on is the
// only way to know. usb_audio_parse() maps the 1-based answer back
// through `clock_pins`.
static uint8_t clock_entity(void) {
    if (!g.s.clock_is_selector) return g.s.clock_id;
    uint8_t pin = 0;
    uint8_t s[8] = { TYPE_IN_CLASS_IF, UAC2_REQ_CUR, 0, CX_CLOCK_SELECT,
                     g.s.ac_ifnum, g.s.clock_id, 1, 0 };
    if (sys_usb_control(g.slot, s, &pin, 1, 1) < 0 || !pin ||
        pin > g.s.clock_pin_count)
        return g.s.clock_id;          // answer it as stated, and let the
                                      // rate request report the failure
    return g.s.clock_pins[pin - 1];
}

// THE RATE IS NOT IN A UAC2 DESCRIPTOR -- it lives in a clock entity and
// is SET, which is why this writes to the device rather than reading it.
static int set_clock_rate(void) {
    if (!g.s.uac2 || !g.s.clock_id) return 0;
    uint8_t id = clock_entity();
    uint32_t hz = SND_RATE;
    uint8_t s[8] = { TYPE_OUT_CLASS_IF, UAC2_REQ_CUR, 0, CS_SAM_FREQ,
                     g.s.ac_ifnum, id, 4, 0 };
    int r = sys_usb_control(g.slot, s, &hz, 4, 0);
    if (r >= 0)
        fprintf(stderr, "usbaudio: clock %u set to %u Hz\n", id, (unsigned)SND_RATE);
    return r;
}

// --- the driver ops -----------------------------------------------------
//
// NO `match`. snd_driver.h's is handed a PCI device and a USB card is
// not one, so this driver finds its own in open() -- the seam a
// non-PCI plugin needs, and the reason `pci` is -1 there.

static int usbaudio_open(struct snd_dev *dev) {
    memset(&g, 0, sizeof g);
    g.slot = -1;

    // The device, and its configuration, both from QUERY -- which ring
    // 3 could already read before any of this existed.
    struct query_usb q;
    QUERY_FOREACH(QUERY_USB, q, i) {
        if (q.if_class != 1) continue;   // 1 = Audio
        g.slot = (int)q.slot;
        break;
    }
    if (g.slot < 0) {
        fprintf(stderr, "usbaudio: no USB audio device attached\n");
        return -1;
    }

    static uint8_t cfg[4096];
    uint32_t total = 0;
    struct query_usbdesc d2;
    QUERY_FOREACH(QUERY_USBDESC, d2, i) {
        if ((int)d2.slot != g.slot) continue;
        total = d2.total > sizeof cfg ? sizeof cfg : d2.total;
        if (d2.offset + d2.len <= sizeof cfg)
            memcpy(cfg + d2.offset, d2.data, d2.len);
    }
    if (!total) {
        fprintf(stderr, "usbaudio: slot %d: no configuration descriptor\n", g.slot);
        return -1;
    }

    // THE SAME WALK THE KERNEL USES, compiled for this ring.
    static struct usb_audio_report rep;
    if (!usb_audio_parse(cfg, total, &g.s, &rep)) {
        // THE REFUSAL'S EVIDENCE. "no 48 kHz stereo s16 stream" says
        // nothing about what the device DOES offer, and the report is
        // filled either way for exactly this.
        fprintf(stderr, "usbaudio: slot %d: no 48 kHz stereo s16 stream "
                        "(UAC %u.%u, %u alternate(s) seen)\n",
                g.slot, rep.uac_major, rep.uac_minor, rep.alts_seen);
        for (int i = 0; i < rep.alt_count; i++)
            fprintf(stderr, "usbaudio:   if %u alt %u: %u ch %u-bit %u Hz "
                            "ep 0x%02x mps %u\n",
                    rep.alts[i].ifnum, rep.alts[i].alt, rep.alts[i].channels,
                    rep.alts[i].bits, (unsigned)rep.alts[i].rate,
                    rep.alts[i].ep, rep.alts[i].mps);
        return -1;
    }
    if (sys_usb_claim(g.slot) != 0) {
        fprintf(stderr, "usbaudio: slot %d: cannot claim it\n", g.slot);
        return -1;
    }

    // One service interval's share, in three units -- confusing them is
    // silent. `frames` is NOT the endpoint's wMaxPacketSize, which is a
    // ceiling a device may set far above the rate.
    uint32_t us = g.s.interval > 1 ? (1u << (g.s.interval - 1)) * 125u : 125u;
    if (!g.s.uac2) us = 1000u;      // UAC1 bInterval is in frames
    uint32_t frames = (SND_RATE / 1000) * us / 1000;
    if (!frames) {
        fprintf(stderr, "usbaudio: %u Hz does not divide a %u us interval\n",
                (unsigned)SND_RATE, (unsigned)us);
        sys_usb_release(g.slot, USB_RELEASE_REBIND);
        return -1;
    }
    g.frames     = (uint16_t)frames;
    g.ring_bytes = (uint16_t)(frames * SND_CHANNELS * 2);
    g.wire_bytes = (uint16_t)(frames * SND_CHANNELS * g.s.subslot);
    // PACKETS PER COMPLETION, AND A RING-3 DRIVER WANTS MORE OF THEM
    // THAN THE KERNEL DOES. The in-kernel driver refills from inside
    // the interrupt handler, so one completion per millisecond costs it
    // nothing; here each one is a process wakeup plus a syscall, and at
    // 1000 a second that was MEASURED at 37% of the rate the endpoint
    // needs. Grouping ~4 ms per wakeup cuts the wakeups fourfold and
    // costs latency nobody can hear.
    uint32_t per_ms = us >= 1000 ? 1 : 1000 / us;
    uint32_t fits = PKT_BYTES / g.wire_bytes;
    // MEASURED, not chosen: 4 ms per wakeup sustained 94% of the
    // endpoint's rate, where 1 ms (the in-kernel driver's shape) gave
    // 90% and 6 and 8 ms collapsed below 10%.
    uint32_t group = per_ms * 4;
    if (group > fits / 3) group = fits / 3;
    if (group < per_ms) group = per_ms;
    g.group = (uint8_t)(group ? group : 1);

    // **THREE GROUPS, AND THE CEILING IS THE TRANSFER RING, NOT THE
    // BUFFER.** xHCI's ring here is 256 TRBs with one reserved for the
    // Link, and one descriptor is one TRB -- so the packets OUTSTANDING
    // must stay well under that or a refill overwrites entries the
    // controller has not consumed yet. Measured: 192 outstanding
    // collapsed the rate to 393 packets/s, where 96 sustained 5803.
    // Three groups is enough to cover a wakeup's turnaround and leaves
    // the ring two-thirds empty.
    // THREE GROUPS. Deeper was measured WORSE, repeatedly and on both
    // memory types -- 160 outstanding collapsed to 381 packets/s where
    // 96 sustained 7516. Why a deeper cushion hurts is NOT established
    // and docs/bugs.md says so.
    uint32_t want = g.group * 3;
    if (want > fits) want = fits - (fits % g.group);
    g.packets = (uint8_t)(want ? want : g.group);

    if (set_clock_rate() < 0) {
        fprintf(stderr, "usbaudio: clock %u would not take %u Hz\n",
                clock_entity(), (unsigned)SND_RATE);
        sys_usb_release(g.slot, USB_RELEASE_REBIND);
        return -1;
    }
    if (set_interface(g.s.ifnum, g.s.alt) < 0) {
        fprintf(stderr, "usbaudio: interface %u would not take alt %u\n",
                g.s.ifnum, g.s.alt);
        sys_usb_release(g.slot, USB_RELEASE_REBIND);
        return -1;
    }

    struct usb_isoch_msg im;
    memset(&im, 0, sizeof im);
    im.slot = (uint32_t)g.slot;
    im.ep = g.s.ep;
    im.mps = g.s.mps;
    im.interval = g.s.interval;
    im.dma_bytes = PKT_BYTES;
    if (sys_usb_isoch_open(&im) != 0) {
        fprintf(stderr, "usbaudio: endpoint 0x%x would not configure\n", g.s.ep);
        sys_usb_release(g.slot, USB_RELEASE_REBIND);
        return -1;
    }
    g.pkt = (uint8_t *)(uintptr_t)im.addr;
    memset(g.pkt, 0, PKT_BYTES);

    dev->rates = SND_RATE_48000;
    dev->depths = g.s.subslot == 2 ? SND_DEPTH_16 :
                  g.s.subslot == 3 ? SND_DEPTH_24 : SND_DEPTH_32;
    dev->pci = -1;                 // not a PCI device; see usbaudio_match
    dev->priv = &g;
    fprintf(stderr, "usbaudio: slot %d, %s, if %u alt %u ep 0x%x %u-bit, "
                    "%u frame(s) every %u us, %u packets, IOC every %u\n",
            g.slot, g.s.uac2 ? "UAC2" : "UAC1", g.s.ifnum, g.s.alt, g.s.ep,
            (unsigned)g.s.subslot * 8, (unsigned)g.frames, (unsigned)us,
            (unsigned)g.packets, (unsigned)g.group);
    return 0;
}

static void usbaudio_close(struct snd_dev *dev) {
    (void)dev;
    if (g.slot >= 0) {
        set_interface(g.s.ifnum, 0);   // alt 0: idle, no bandwidth
        sys_usb_release(g.slot, USB_RELEASE_REBIND);
        g.slot = -1;
    }
}

static int usbaudio_start(struct snd_dev *dev, uint64_t ring_phys) {
    (void)dev; (void)ring_phys;   // we READ the ring, we do not point at it
    if (!g.pkt) return -1;

    // THE RING IS MAPPED HERE, NOT AT open(), and the ordering is the
    // reason: only the REGISTERED driver may ask for it, and the host
    // registers after open() returns. start() is the first call that
    // happens on the far side of that, and it is also the first moment
    // the samples are wanted.
    if (!g.ring) {
        uint64_t addr = 0;
        if (sys_snd_ring_map(&addr) < 0) {
            fprintf(stderr, "usbaudio: the sound ring was not granted\n");
            return -1;
        }
        g.ring = (const uint8_t *)(uintptr_t)addr;
        fprintf(stderr, "usbaudio: ring mapped read-only at %llx\n",
                (unsigned long long)addr);
    }
    g.copy_pos = 0;
    g.play_pos = 0;
    g.next = 0;
    g.running = 1;
    // PRIME IT: an isochronous endpoint with nothing posted is idle,
    // and the first completion is what drives every refill after it.
    // The COPIES are per packet; the POSTS are one call per group, for
    // the reason syscall_abi.h gives.
    for (uint8_t k = 0; k < g.packets; k++) copy_one_packet(k);
    for (uint8_t k = 0; k + g.group <= g.packets; k = (uint8_t)(k + g.group))
        sys_usb_isoch_post(g.slot, g.s.ep, (uint32_t)k * g.wire_bytes,
                           g.wire_bytes, 1, g.group, g.wire_bytes);
    g.next = 0;
    return 0;
}

static void usbaudio_stop(struct snd_dev *dev) {
    (void)dev;
    // NOTHING IS CANCELLED: the TDs already posted play out over the
    // next few milliseconds and the endpoint goes quiet on its own.
    // Stopping one properly is Stop Endpoint plus Set TR Dequeue, which
    // buys latency and a command pair that can fail on a device that is
    // already unplugged.
    g.running = 0;
}

static int usbaudio_period(struct snd_dev *dev) {
    (void)dev;
    if (!g.running) return SND_IRQ_NOT_MINE;
    int n = sys_usb_isoch_status(g.slot, g.s.ep);
    if (n <= 0) return SND_IRQ_NOT_MINE;

    // ONE EVENT COVERS A GROUP -- only the last of each carries IOC --
    // so the ring advances by the whole group, per event.
    for (int e = 0; e < n; e++) {
        g.play_pos = (g.play_pos + (uint32_t)g.ring_bytes * g.group) % SND_RING_BYTES;
        // ONE CALL FOR THE WHOLE GROUP. The packets are contiguous in
        // the buffer, so a stride covers them -- unless the group wraps
        // the buffer's end, which is posted as two calls rather than
        // one that would run off it.
        uint8_t first = g.next;
        uint8_t room = (uint8_t)(g.packets - first);
        uint8_t run = room < g.group ? room : g.group;
        for (uint8_t k = 0; k < g.group; k++)
            copy_one_packet((uint8_t)((first + k) % g.packets));
        sys_usb_isoch_post(g.slot, g.s.ep, (uint32_t)first * g.wire_bytes,
                           g.wire_bytes, run == g.group, run, g.wire_bytes);
        if (run < g.group)
            sys_usb_isoch_post(g.slot, g.s.ep, 0, g.wire_bytes, 1,
                               (uint8_t)(g.group - run), g.wire_bytes);
        g.next = (uint8_t)((first + g.group) % g.packets);
    }
    // ON A CHUNK BOUNDARY, which is the only granularity the core's
    // zeroing rule is defined at (abi/sound_abi.h).
    return (int)((g.play_pos / SND_CHUNK_BYTES) * SND_CHUNK_BYTES);
}

const struct snd_driver snd_driver = {
    .abi = SND_DRIVER_ABI,
    .name = "usbaudio",
    .label = "USB Audio (ring 3)",
    .dma_bytes = 0,            // isoch_open grants its own packet buffer
    .open = usbaudio_open,
    .close = usbaudio_close,
    .start = usbaudio_start,
    .stop = usbaudio_stop,
    .period = usbaudio_period,
};
