// USB audio playback, UAC1 and UAC2: a speaker on the other end of an
// isochronous OUT endpoint, registered as a `struct sound_device` like
// any other card.
//
// THE FORMAT IS THE STREAM'S (abi/sound_abi.h). Each playable alternate
// setting is a width (usb_audio_parse's `formats`); the rate is set on
// the UAC2 clock, or on the UAC1 endpoint when its alternate lists more
// than one. set_format() records the pair and start() applies it --
// alternate, rate, packet sizes -- since the card is idle in alt 0
// between streams anyway.
//
// A PACKET IS THE RATE'S SHARE OF ONE SERVICE INTERVAL, and at 44.1 kHz
// that share is not whole: packets carry 44 or 45 frames a millisecond
// (usb_audio_pace_next()). The endpoint is driven off the bus's own SOF
// clock; an asynchronous device's feedback endpoint is not read, so its
// clock and ours may drift apart by its crystal's error.
//
// THE SAMPLES ARE COPIED, unlike the AC'97's descriptor list which
// points straight into the core's ring. A packet divides neither the
// ring nor its chunks, and an xHCI TRB may not cross a 64 KiB boundary
// -- so a zero-copy packet would need chained split TRBs at two kinds of
// edge. Copying into a frame of our own is what Linux's snd-usb-audio
// does for the same reason, and it is where the width is narrowed.
#include "usb.h"
#include "usb_audio.h"
#include "xhci.h"
#include "xhci_regs.h"
#include "sound.h"
#include "sound_abi.h"
#include "pmm.h"
#include "klog.h"
#include "kfmt.h"
#include "errno.h"
#include "string.h"
#include "ktest.h"
#include "barrier.h"   // cpu_relax() in the drain poll
#include "driver.h" // driver_bound() -- `lsdrv`

DRIVER_DECLARE("usb-audio", "sound", "USB audio class, UAC1 and UAC2");


#define TYPE_OUT_CLASS_IF      0x21   // host->device, class, interface
#define TYPE_IN_CLASS_IF       0xA1   // device->host, class, interface
#define TYPE_OUT_STD_IF        0x01   // host->device, standard, interface
#define TYPE_OUT_CLASS_EP      0x22   // host->device, class, endpoint
#define UAC1_EP_SAM_FREQ       0x01   // the endpoint's sampling frequency control

// Audio kept posted, in MICROSECONDS rather than in packets: enough
// that a busy moment cannot starve the endpoint, short enough that
// stopping does not leave a long tail playing. A packet is one service
// interval, so at the usual 1 ms this is the sixteen it has always
// been -- and on a 125 us endpoint it stays 16 ms instead of becoming 2.
#define AUDIO_INFLIGHT_US 16000
#define AUDIO_PACKETS_MAX 128

struct audio_dev {
    uint8_t  in_use;
    uint8_t  slot;
    uint8_t  running;
    struct usb_audio_stream s;

    // THE FORMAT: a rate and an entry of `s.formats`, chosen by
    // set_format() and applied at start. `clock_rates` is a UAC2 clock's
    // GET RANGE, read at bind.
    uint32_t rate;
    int      fmt;
    uint32_t clock_rates;
    uint32_t us;             // the service interval, from bInterval

    // ONE PACKET, in three units, and confusing them is silent. Its
    // FRAMES are the rate's share of one service interval, from `pace`
    // -- NOT the endpoint's wMaxPacketSize, which is a ceiling a device
    // may set well above the rate (the G6's is sized for 384 kHz). Each
    // slot's cost in the s32 ring and on the wire is kept per slot,
    // because at 44.1 kHz no two neighbours need agree. `stride` is the
    // slot size: the busiest packet at this rate and width.
    struct usb_audio_pace pace;
    uint16_t stride;
    uint16_t slot_ring[AUDIO_PACKETS_MAX];
    uint16_t slot_wire[AUDIO_PACKETS_MAX];
    uint8_t  done_slot;      // the first slot of the next group to complete
    // Packets per completion interrupt: one interrupt per millisecond,
    // so a 125 us endpoint costs 1000 a second and not 8000. Linux's
    // snd-usb-audio groups for the same reason.
    uint8_t  group;

    uint8_t *pkt;            // `packets` x stride, one frame
    uint64_t pkt_phys;
    uint8_t  packets;        // how many actually fit
    uint8_t  next_slot;      // the packet slot the next completion refills
    // TDs posted and not yet completed. Written by the event drain, so
    // volatile -- audio_stop() waits on it, and the wait is what keeps
    // a dropped alternate from stalling everything still in flight.
    volatile uint8_t inflight;

    const uint8_t *ring;     // the sound core's, read-only to us
    uint32_t copy_pos;       // ring offset of the next packet to copy
    uint32_t play_pos;       // ring offset the hardware has reached
    uint32_t reported;       // last chunk boundary handed to the core

    // Volume, in the device's own 1/256 dB units, learned at bind.
    int16_t  vol_min, vol_max;
    uint8_t  has_volume;

    char name[SOUND_NAME_MAX];
    char label[40];   // query_sound.label's size, so nothing truncates
    char devid[24];   // udevice.c's spelling: usb:<root port>:<vid>:<pid>
    struct sound_device dev;
};

// ONE PER ATTACHED DAC. This was a single device, on the reasoning that
// a second would "register and sit inactive" -- which is true and is
// exactly what is wanted: it sits in the tray's device list until
// somebody picks it. With two DACs attached the second was declined and
// could never be chosen at all. Linux's snd-usb-audio gives each its
// own card for the same reason.
#define USB_AUDIO_MAX 4
static struct audio_dev g_audio[USB_AUDIO_MAX];

// --- descriptor parsing (pure, and KTESTed below) ---------------------


// --- topology ---------------------------------------------------------
//
// Two short passes over the same descriptors the main walk reads. They
// are separate because both answer a question about an entity that can
// appear BEFORE or AFTER the thing that names it, and a single-pass
// answer would depend on the order a device happened to choose.

// The feature unit on the PLAYBACK path: the one feeding an output
// terminal that is not USB streaming, i.e. a speaker or a headphone
// jack. Taking the first feature unit instead is right on a device with
// one and picks a microphone's on a device with eight.

// --- the packet pump --------------------------------------------------

static int set_interface(uint8_t slot, uint8_t ifnum, uint8_t alt) {
    uint8_t setup[8] = { TYPE_OUT_STD_IF, REQ_SET_INTERFACE, alt, 0,
                         ifnum, 0, 0, 0 };
    return xhci_control(slot, setup, 0, 0, 0);
}


// One s32 ring sample as the device's subslot holds it: the TOP bytes,
// rounded, little-endian. Writing a narrower sample into the bottom of a
// wide subslot is a 256x attenuation, which sounds like silence rather
// than like a bug.
static void write_sample(uint8_t *dst, int32_t v, uint8_t subslot) {
    switch (subslot) {  // dispatch-ok: the three widths usb_audio_parse accepts
        case 2: {
            uint16_t u = (uint16_t)snd_s32_to_s16(v);
            dst[0] = (uint8_t)u; dst[1] = (uint8_t)(u >> 8);
            break;
        }
        case 3: {
            int64_t r = ((int64_t)v + 0x80) >> 8;
            uint32_t u = (uint32_t)(r > 0x7FFFFF ? 0x7FFFFF : r);
            dst[0] = (uint8_t)u; dst[1] = (uint8_t)(u >> 8); dst[2] = (uint8_t)(u >> 16);
            break;
        }
        default: {
            uint32_t u = (uint32_t)v;
            dst[0] = (uint8_t)u; dst[1] = (uint8_t)(u >> 8);
            dst[2] = (uint8_t)(u >> 16); dst[3] = (uint8_t)(u >> 24);
            break;
        }
    }
}

static const struct usb_audio_format *cur_fmt(const struct audio_dev *a) {
    return &a->s.formats[a->fmt];
}

static void copy_one_packet(struct audio_dev *a, uint8_t slot) {
    uint8_t *dst = a->pkt + (uint32_t)slot * a->stride;
    uint32_t at = a->copy_pos;
    uint8_t subslot = cur_fmt(a)->subslot;
    uint32_t frames = usb_audio_pace_next(&a->pace);
    a->slot_ring[slot] = (uint16_t)(frames * SND_FRAME_BYTES);
    a->slot_wire[slot] = (uint16_t)(frames * SND_CHANNELS * subslot);

    if (subslot == 4) {
        uint32_t n = a->slot_ring[slot];
        // The ring wraps mid-packet on most laps -- 192 divides neither
        // the ring nor a chunk -- so this is two copies, not one.
        uint32_t first = SND_RING_BYTES - at;
        if (first > n) first = n;
        k_memcpy(dst, a->ring + at, first);
        if (first < n) k_memcpy(dst + first, a->ring, n - first);
        a->copy_pos = (at + n) % SND_RING_BYTES;
        return;
    }

    // Sample at a time, because the widths differ. `at` is always a
    // multiple of 4 and the ring is a whole number of frames, so a sample
    // never straddles the wrap and only the loop's step has to check it.
    uint32_t samples = frames * SND_CHANNELS;
    for (uint32_t i = 0; i < samples; i++) {
        int32_t v;
        k_memcpy(&v, a->ring + at, 4);
        write_sample(dst, v, subslot);
        dst += subslot;
        at += 4;
        if (at >= SND_RING_BYTES) at = 0;
    }
    a->copy_pos = at;
}

// One TD finished. Called FROM THE EVENT DRAIN, so it does the two
// cheap things and nothing else: refill the slot that just drained and
// hand it straight back to the controller.
static void audio_packet_done(void *ctx, uint32_t bytes) {
    struct audio_dev *a = ctx;
    (void)bytes;   // a short isochronous packet is a dropped one, not a resync
    if (!a->in_use) return;

    // The hardware has consumed one packet's worth of the ring. Told to
    // the core only on a CHUNK boundary, which is the only granularity
    // abi/sound_abi.h's zeroing rule is defined at.
    // One event covers a GROUP of packets -- only the last of each
    // carries IOC -- so the ring advances by the whole group.
    uint32_t moved = 0;
    for (uint8_t k = 0; k < a->group; k++) {
        moved += a->slot_ring[a->done_slot];
        a->done_slot = (uint8_t)((a->done_slot + 1) % a->packets);
    }
    a->play_pos = (a->play_pos + moved) % SND_RING_BYTES;
    uint32_t chunk = (a->play_pos / SND_CHUNK_BYTES) * SND_CHUNK_BYTES;
    if (chunk != a->reported) {
        a->reported = chunk;
        sound_period_done(chunk);
    }

    a->inflight = (a->inflight > a->group) ? (uint8_t)(a->inflight - a->group) : 0;
    if (!a->running) return;
    for (uint8_t k = 0; k < a->group; k++) {
        uint8_t slot = a->next_slot;
        a->next_slot = (uint8_t)((slot + 1) % a->packets);
        copy_one_packet(a, slot);
        if (xhci_isoch_post(a->slot, a->s.ep,
                            a->pkt_phys + (uint64_t)slot * a->stride,
                            a->slot_wire[slot], k + 1 == a->group) == 0)
            a->inflight++;
    }
}

// A class request to an ENTITY on the AudioControl interface -- a
// feature unit, a clock source, a clock selector. The entity is an
// argument rather than always the feature unit because UAC2 puts the
// sample rate on a clock, and addressing it as the feature unit fails
// the transfer with nothing to see.
static int audio_control(struct audio_dev *a,
                         uint8_t req, uint8_t type, uint8_t cs, uint8_t channel,
                         uint8_t entity, uint8_t *buf, uint16_t len) {
    uint8_t setup[8];
    setup[0] = type;
    setup[1] = req;
    setup[2] = channel;              // wValue low: channel number
    setup[3] = cs;                   // wValue high: control selector
    setup[4] = a->s.ac_ifnum;        // wIndex low: the AudioControl interface
    setup[5] = entity;               // wIndex high: the entity addressed
    setup[6] = (uint8_t)(len & 0xFF);
    setup[7] = (uint8_t)(len >> 8);
    return xhci_control(a->slot, setup, buf, len, (type & 0x80) ? 1 : 0);
}

// The UAC2 clock SOURCE this stream runs from. A selector is ASKED for
// its current pin, not set: on the device this was written against it
// is a front-panel mode (a DSP path and a direct path), so choosing one
// would silently override what the owner set on the hardware. 0: none.
static uint8_t clock_source(struct audio_dev *a) {
    if (!a->s.clock_id) return 0;
    if (!a->s.clock_is_selector) return a->s.clock_id;
    if (!a->s.clock_pin_count) return 0;
    uint8_t pin = 0;
    if (audio_control(a, UAC2_REQ_CUR, TYPE_IN_CLASS_IF, CX_CLOCK_SELECT, 0,
                      a->s.clock_id, &pin, 1) < 1 ||
        !pin || pin > a->s.clock_pin_count) {
        klog_printf("usb-audio: clock selector %u answered %u; using "
                    "source %u\n", a->s.clock_id, pin, a->s.clock_pins[0]);
        pin = 1;
    }
    return a->s.clock_pins[pin - 1];
}

// UAC2: the rates the clock says it runs at (GET RANGE). A device that
// will not say is taken at SND_RATE alone, which bind then proves.
static uint32_t clock_rates(struct audio_dev *a) {
    uint8_t clock = clock_source(a);
    static uint8_t buf[2 + 12 * 16];
    k_memset(buf, 0, sizeof buf);
    int n = clock ? audio_control(a, UAC2_REQ_RANGE, TYPE_IN_CLASS_IF, CS_SAM_FREQ, 0,
                                  clock, buf, sizeof buf) : -1;
    uint32_t m = n > 0 ? usb_audio_range_rates(buf, (uint32_t)n) : 0;
    // SAID ONCE, AT BIND: what the clock answered, raw, beside what was
    // made of it -- a rate list that looks short is otherwise unexplained.
    klog_printf("usb-audio: clock %u GET RANGE: %d byte(s), %u subrange(s), rates %#x\n",
                clock, n, n >= 2 ? (unsigned)(buf[0] | (buf[1] << 8)) : 0u, m);
    return m ? m : SND_RATE_48000;
}

// TELL THE DEVICE ITS RATE. UAC2: a SET_CUR on the clock -- nothing in
// its descriptors states a rate, so this request is the negotiation.
// UAC1: a SET_CUR on the ENDPOINT, only when the alternate lists more
// than one rate (a single-rate alternate has nothing to choose, and some
// devices stall a control they do not have).
static int set_rate(struct audio_dev *a) {
    uint8_t v[4] = { (uint8_t)(a->rate & 0xFF), (uint8_t)((a->rate >> 8) & 0xFF),
                     (uint8_t)((a->rate >> 16) & 0xFF), (uint8_t)((a->rate >> 24) & 0xFF) };
    if (!a->s.uac2) {
        if (!cur_fmt(a)->rate_ctl) return 0;
        uint8_t setup[8] = { TYPE_OUT_CLASS_EP, AUDIO_REQ_SET_CUR, 0, UAC1_EP_SAM_FREQ,
                             a->s.ep, 0, 3, 0 };
        if (xhci_control(a->slot, setup, v, 3, 0) < 0) {
            klog_printf(KLOG_ERR "usb-audio: endpoint 0x%x refused %u Hz\n", a->s.ep,
                        (unsigned)a->rate);
            return -1;
        }
        return 0;
    }
    uint8_t clock = clock_source(a);
    if (!clock) {
        klog_write("usb-audio: UAC2 device names no clock for its stream\n");
        return -1;
    }
    if (audio_control(a, AUDIO_REQ_SET_CUR, TYPE_OUT_CLASS_IF, CS_SAM_FREQ, 0,
                      clock, v, 4) < 0) {
        klog_printf(KLOG_ERR "usb-audio: clock %u refused %u Hz\n", clock,
                    (unsigned)a->rate);
        return -1;
    }
    // READ BACK: a clock may take the request and run at something else,
    // and the answer is the only evidence of which.
    uint8_t back[4] = { 0, 0, 0, 0 };
    audio_control(a, UAC2_REQ_CUR, TYPE_IN_CLASS_IF, CS_SAM_FREQ, 0, clock, back, 4);
    klog_printf("usb-audio: clock %u set to %u Hz (reads back %u)\n", clock,
                (unsigned)a->rate, (unsigned)(back[0] | (back[1] << 8) | (back[2] << 16) |
                                              ((uint32_t)back[3] << 24)));
    return 0;
}

// The slot geometry for the format about to start: the busiest packet
// sizes a slot, and as many slots as AUDIO_INFLIGHT_US asks for (and
// one 4 KiB frame holds), whole groups only.
static void plan_packets(struct audio_dev *a) {
    uint32_t frames = usb_audio_pace_max(a->rate, a->us);
    a->stride = (uint16_t)(frames * SND_CHANNELS * cur_fmt(a)->subslot);
    uint32_t want = AUDIO_INFLIGHT_US / a->us;
    uint32_t fits = 4096 / a->stride;
    if (want > fits) want = fits;
    if (want > AUDIO_PACKETS_MAX) want = AUDIO_PACKETS_MAX;
    want -= want % a->group;
    if (want < a->group) want = a->group;
    a->packets = (uint8_t)want;
    a->pace.rate = a->rate;
    a->pace.us = a->us;
    a->pace.acc = 0;
}

static int audio_format(const struct sound_device *d, uint32_t rate, uint32_t bits) {
    struct audio_dev *a = d->priv;
    int f = usb_audio_pick_format(&a->s, a->clock_rates, rate, bits, a->us);
    if (f < 0) return -EINVAL;
    a->rate = rate;
    a->fmt = f;
    a->dev.rate = rate;
    a->dev.bits = a->s.formats[f].bits;
    return 0;
}

static int audio_start(const struct sound_device *d) {
    struct audio_dev *a = d->priv;
    if (!a->in_use || !a->ring) return -1;
    if (a->running) return 0;

    // The rate first, while the endpoint is idle -- what every host does.
    // On UAC1 it is addressed to the endpoint, which must exist, so it
    // follows the alternate below instead.
    if (a->s.uac2 && set_rate(a) < 0) return -1;

    // THE ALTERNATE SETTING FOLLOWS THE STREAM, not the bind. Alt 0 is
    // the "idle, no bandwidth" setting every UAC device carries, and
    // sitting in alt 1 while playing nothing is not merely untidy: the
    // device's output is LIVE the whole time, which on QEMU means it
    // holds an open voice on its audiodev and the AC'97 sharing that
    // audiodev never gets clocked -- the guest then blocks forever on a
    // hardware position that cannot advance. Measured: with both cards
    // on one audiodev, choosing the AC'97 played nothing until this
    // driver started standing down.
    uint8_t alt = cur_fmt(a)->alt;
    if (set_interface(a->slot, a->s.ifnum, alt) < 0) {
        klog_printf(KLOG_ERR "usb-audio: could not claim interface %u alt %u\n",
                    a->s.ifnum, alt);
        return -1;
    }
    if (!a->s.uac2 && set_rate(a) < 0) return -1;

    plan_packets(a);
    klog_printf("usb-audio: start at %u Hz, alt %u (%u-bit), up to %u B a packet, %u packets\n",
                (unsigned)a->rate, alt, cur_fmt(a)->bits, a->stride, a->packets);
    a->done_slot = 0;
    a->copy_pos = 0;
    a->play_pos = 0;
    a->reported = 0;
    a->next_slot = 0;
    a->inflight = 0;
    a->running = 1;

    // Every packet posted before the first doorbell matters: an
    // isochronous endpoint given one TD plays it and underruns, and the
    // stream then stutters at exactly the rate it is refilled.
    for (uint8_t i = 0; i < a->packets; i++) {
        copy_one_packet(a, i);
        if (xhci_isoch_post(a->slot, a->s.ep,
                            a->pkt_phys + (uint64_t)i * a->stride,
                            a->slot_wire[i],
                            (i + 1) % a->group == 0) < 0) {
            a->running = 0;
            return -1;
        }
        a->inflight++;
    }
    return 0;
}

static void audio_stop(const struct sound_device *d) {
    struct audio_dev *a = d->priv;
    // Nothing is cancelled: the TDs already posted play out over the
    // next few milliseconds and the ring then goes quiet on its own.
    // Stopping an isochronous endpoint properly means Stop Endpoint
    // plus Set TR Dequeue, which buys 16 ms of latency and a command
    // pair that can fail while the device is already unplugged.
    if (!a->running) return;
    a->running = 0;
    // Guarded on `in_use` because the DETACH path stops the device
    // after the hardware has gone, and a control transfer to a device
    // that is not there is two million polls of nothing.
    if (!a->in_use) return;

    // DRAIN BEFORE DROPPING THE ALTERNATE. The TDs already posted are
    // for an endpoint alt 0 does not have, so standing down while they
    // are outstanding STALLS every one of them -- measured at 32 bad
    // transfers per switch, which is exactly the packets in flight.
    //
    // POLLED, NOT WAITED ON, and that is not a preference: this is
    // reached from process teardown with INTERRUPTS OFF, where nothing
    // decrements the counter and the PIT does not advance either -- a
    // first version waited on `coarse_ticks()` and hung the machine solid
    // (RFL with IF clear, spinning in ring 0). Draining the event ring
    // by hand is what makes the wait independent of both. The backstop
    // is the driver's usual one, for a controller that has stopped
    // completing anything at all.
    uint32_t spins = 0;
    while (a->inflight && spins++ < XHCI_POLL_BACKSTOP) {
        xhci_service();
        cpu_relax();
    }

    // Back to the zero-bandwidth alternate -- see audio_start().
    set_interface(a->slot, a->s.ifnum, 0);
}

// --- volume -----------------------------------------------------------

static int16_t read_db(struct audio_dev *a, uint8_t req, int16_t fallback) {
    uint8_t v[2] = {0, 0};
    if (audio_control(a, req, TYPE_IN_CLASS_IF, AUDIO_CS_VOLUME, 1,
                      a->s.feature_unit, v, 2) < 2)
        return fallback;
    return (int16_t)((uint16_t)v[0] | ((uint16_t)v[1] << 8));
}

// UAC2 HAS NO GET_MIN/GET_MAX. It has one RANGE request answering with
// a block of subranges -- wNumSubRanges, then MIN/MAX/RES triples --
// and the first subrange is the one this uses. Asking a UAC2 device
// for GET_MIN is request 0x82, which it does not implement.
static void read_range(struct audio_dev *a, int16_t *min, int16_t *max) {
    uint8_t r[8] = {0};
    if (audio_control(a, UAC2_REQ_RANGE, TYPE_IN_CLASS_IF, AUDIO_CS_VOLUME, 1,
                      a->s.feature_unit, r, sizeof r) < 8)
        return;
    if (!(r[0] | r[1])) return;               // wNumSubRanges == 0
    *min = (int16_t)((uint16_t)r[2] | ((uint16_t)r[3] << 8));
    *max = (int16_t)((uint16_t)r[4] | ((uint16_t)r[5] << 8));
}

// 0..100 onto the device's own dB range. The taper is the interesting
// part: UAC1 volume is 1/256 dB, and a device commonly reports a
// minimum near -128 dB, so interpolating a percentage linearly across
// the whole range puts 50% at -64 dB -- silence. The bottom of the
// slider is capped at 40 dB of attenuation, which is the range a
// physical volume knob covers, and 0 is the mute control rather than a
// number.
#define AUDIO_TAPER_DB 40

static void audio_set_volume(const struct sound_device *d, int pct) {
    struct audio_dev *a = d->priv;
    if (!a->in_use || !a->s.feature_unit) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    if (a->s.has_mute) {
        uint8_t mute = (uint8_t)(pct == 0);
        audio_control(a, AUDIO_REQ_SET_CUR, TYPE_OUT_CLASS_IF, AUDIO_CS_MUTE,
                      0, a->s.feature_unit, &mute, 1);
    }
    if (!a->has_volume || pct == 0) return;

    int32_t floor_db = (int32_t)a->vol_max - AUDIO_TAPER_DB * 256;
    if (floor_db < a->vol_min) floor_db = a->vol_min;
    int32_t value = floor_db + ((int32_t)a->vol_max - floor_db) * pct / 100;
    uint8_t v[2] = { (uint8_t)(value & 0xFF), (uint8_t)((value >> 8) & 0xFF) };
    // Channel 1 and 2 rather than 0: a feature unit commonly carries
    // volume per channel and only mute on the master, which is exactly
    // what QEMU's usb-audio reports.
    audio_control(a, AUDIO_REQ_SET_CUR, TYPE_OUT_CLASS_IF, AUDIO_CS_VOLUME, 1,
                  a->s.feature_unit, v, 2);
    audio_control(a, AUDIO_REQ_SET_CUR, TYPE_OUT_CLASS_IF, AUDIO_CS_VOLUME, 2,
                  a->s.feature_unit, v, 2);
}

// --- binding ----------------------------------------------------------

// The service interval an endpoint's bInterval means, in microseconds.
// High and super speed carry an EXPONENT of 125 us microframes; full
// and low speed carry a count of 1 ms frames. Reading one as the other
// is off by a factor of eight, which is a stream at the wrong rate
// rather than an error anything reports.
static uint32_t interval_us(uint8_t speed, uint8_t b_interval) {
    if (speed == XHCI_SPEED_HIGH || speed == XHCI_SPEED_SUPER) {
        uint32_t e = b_interval ? (uint32_t)b_interval - 1 : 0;
        if (e > 15) e = 15;
        return 125u << e;
    }
    return 1000u * (b_interval ? b_interval : 1);
}

// What the device offered, when none of it was usable. The point is
// that "no 48 kHz stereo PCM stream" is not actionable on a machine
// nobody here owns -- the version and the alternates are.
static void log_refusal(uint8_t slot, const struct usb_audio_report *rep) {
    if (rep->uac_major)
        klog_printf("usb-audio: slot %u: UAC %u.%02u device offers no %u Hz "
                    "stereo PCM stream (%u alternate(s)):\n", slot,
                    rep->uac_major, rep->uac_minor, (unsigned)SND_RATE,
                    rep->alts_seen);
    else
        klog_printf("usb-audio: slot %u: audio device with no class header "
                    "offers no %u Hz stereo PCM stream (%u alternate(s)):\n",
                    slot, (unsigned)SND_RATE, rep->alts_seen);

    for (uint8_t i = 0; i < rep->alt_count; i++) {
        const struct usb_audio_alt *t = &rep->alts[i];
        klog_printf("usb-audio:   if %u alt %u: %u ch, %u-bit, %u Hz, "
                    "ep 0x%02x %s sync %u, %u B x%u, bInterval %u\n",
                    t->ifnum, t->alt, t->channels, t->bits, t->rate,
                    t->ep, (t->ep & 0x80) ? "IN" : "OUT", t->sync,
                    t->mps, t->mult, t->interval);
    }
    // A UAC2 format descriptor carries no rate at all -- the clock
    // source does, over a class request this driver does not make.
    if (rep->uac_major >= 2)
        klog_write("usb-audio: a UAC2 rate is the clock source's, not the "
                   "format descriptor's -- 0 Hz above means \"not stated "
                   "here\", not \"none\"\n");
}

// A STRING OFF A USB DEVICE IS NOT TRUSTED TO BE TEXT. Both DACs on
// the test laptop answer "?" for their PRODUCT while their MANUFACTURER
// reads perfectly (docs/bugs.md has the mojibake entry), and "?" was
// what the tray's device list showed. Same ladder as usbaudio.so's.
static int str_readable(const char *s) {
    if (!s[0] || s[0] == '?') return 0;
    for (const char *p = s; *p; p++)
        if (*p < 0x20 || *p > 0x7e) return 0;
    return 1;
}

static void audio_label(struct audio_dev *a, const struct usb_device_info *info) {
    if (str_readable(info->product)) {
        k_strlcpy(a->label, info->product, sizeof a->label);
        return;
    }
    if (str_readable(info->manufacturer)) {
        // The suffix only when it fits -- a label that says the maker
        // and nothing else still names the row, and lssound's own
        // driver column already says what it is.
        if (k_strlen(info->manufacturer) + sizeof(" USB Audio") <= sizeof a->label)
            k_snprintf(a->label, sizeof a->label, "%s USB Audio", info->manufacturer);
        else
            k_strlcpy(a->label, info->manufacturer, sizeof a->label);
        return;
    }
    k_snprintf(a->label, sizeof a->label, "USB Audio %04x:%04x",
              info->vendor_id, info->product_id);
}

int usb_audio_bind(struct usb_device_info *info, const uint8_t *cfg,
                   uint32_t total) {
    if (!info) return 0;

    // A FREE SLOT, AND SAYING SO WHEN THERE IS NONE. Every other
    // refusal below logs a reason; a silent one read as a device the
    // parser had rejected.
    struct audio_dev *a = 0;
    for (int i = 0; i < USB_AUDIO_MAX; i++)
        if (!g_audio[i].in_use) { a = &g_audio[i]; break; }
    if (!a) {
        klog_printf("usb-audio: slot %u: %d USB DACs already bound -- "
                    "not bound (try snddrv --usb-id %04x:%04x)\n",
                    info->slot, USB_AUDIO_MAX,
                    info->vendor_id, info->product_id);
        return 0;
    }

    struct usb_audio_stream s;
    struct usb_audio_report rep;
    if (!usb_audio_parse(cfg, total, &s, &rep)) {
        log_refusal(info->slot, &rep);
        return 0;
    }

    // The default -- 48 kHz at the deepest alternate the parse chose --
    // must fit its endpoint, as every format set later is checked to.
    uint32_t us = interval_us(info->speed, s.interval);
    uint32_t wire = usb_audio_pace_max(SND_RATE, us) * SND_CHANNELS * s.subslot;
    if (s.mps < wire) {
        klog_printf("usb-audio: slot %u: endpoint 0x%02x holds %u bytes and "
                    "%u Hz needs %u every %u us -- not bound\n",
                    info->slot, s.ep, s.mps, (unsigned)SND_RATE, wire, us);
        return 0;
    }

    k_memset(a, 0, sizeof *a);
    a->slot = info->slot;
    a->s = s;
    a->us = us;
    a->rate = SND_RATE;
    for (int f = 0; f < s.nformats; f++)
        if (s.formats[f].alt == s.alt) a->fmt = f;
    a->group = (uint8_t)(us >= 1000 ? 1 : 1000 / us);
    // THE ENDPOINT IS SIZED FOR ITS BIGGEST ALTERNATE: the controller's
    // reservation is made once here, and a deeper width at start only
    // swaps the device's alternate under it.
    uint16_t mps = s.mps;
    for (int f = 0; f < s.nformats; f++)
        if (s.formats[f].mps > mps) mps = s.formats[f].mps;

    a->ring = sound_ring_alloc(0);
    if (!a->ring) {
        klog_write("usb: no contiguous frames for the sound ring\n");
        return 0;
    }
    a->pkt_phys = pmm_alloc_contiguous(1, PMM_ZONE_DMA32);
    if (!a->pkt_phys) return 0;
    a->pkt = (uint8_t *)(uintptr_t)a->pkt_phys;   // identity-mapped
    k_memset(a->pkt, 0, 4096);

    // The endpoint only exists in the alternate setting, so this has to
    // come before configuring it -- and the device is entitled to
    // refuse, which is a device we cannot play through rather than one
    // to configure anyway.
    if (set_interface(info->slot, s.ifnum, s.alt) < 0) {
        klog_printf(KLOG_ERR "usb: slot %u: set interface %u alt %u failed\n",
                    info->slot, s.ifnum, s.alt);
        pmm_free_contiguous(a->pkt_phys, 1);
        return 0;
    }
    if (xhci_add_isoch_out(info->slot, s.ep, mps, s.interval,
                           audio_packet_done, a) < 0) {
        klog_printf("usb: slot %u: isochronous endpoint 0x%x not configured\n",
                    info->slot, s.ep);
        pmm_free_contiguous(a->pkt_phys, 1);
        return 0;
    }

    a->in_use = 1;   // published before any control transfer can be answered

    // Configured, and immediately IDLE. The endpoint stays configured
    // in the controller either way; what alt 0 releases is the DEVICE's
    // side of it, which is what stops a bound-but-silent card from
    // holding output open for the whole session.
    set_interface(info->slot, s.ifnum, 0);

    // PROVEN AT BIND, not assumed at start: on UAC2 this request is the
    // whole rate negotiation, and a device that refuses it is one this
    // driver cannot play through however well it parsed.
    if (s.uac2) a->clock_rates = clock_rates(a);
    if (s.uac2 && set_rate(a) < 0) {
        klog_printf(KLOG_ERR "usb-audio: slot %u: cannot set %u Hz -- not bound\n",
                    info->slot, (unsigned)SND_RATE);
        a->in_use = 0;
        pmm_free_contiguous(a->pkt_phys, 1);
        return 0;
    }

    if (s.has_volume) {
        if (s.uac2) {
            read_range(a, &a->vol_min, &a->vol_max);
        } else {
            a->vol_min = read_db(a, AUDIO_REQ_GET_MIN, 0);
            a->vol_max = read_db(a, AUDIO_REQ_GET_MAX, 0);
        }
        a->has_volume = (a->vol_max > a->vol_min);
    }

    // THE NAME IS THE STABLE ID the `audio_device` setting persists, so
    // two DACs cannot both be "usb-audio". The ids are what lsusb
    // prints and what --usb-id takes, and they survive the device-table
    // reshuffles a port number does not. SOUND_NAME_MAX is 16 and this
    // is 13.
    k_snprintf(a->name, sizeof a->name, "usb-%04x%04x",
               info->vendor_id, info->product_id);
    // TWO OF THE SAME MODEL share a vendor/product pair, so the id is
    // not unique by itself. The second one onwards takes a numbered
    // form -- and WHICH of an identical pair keeps the plain name
    // follows enumeration order, so `audio_device` cannot pin one of
    // them across a replug. That is inherent to identical devices; a
    // port number would move too.
    for (int n = 2; n <= USB_AUDIO_MAX; n++) {
        int taken = 0;
        for (int i = 0; i < USB_AUDIO_MAX; i++)
            if (&g_audio[i] != a && g_audio[i].in_use &&
                k_strcmp(g_audio[i].name, a->name) == 0) taken = 1;
        if (!taken) break;
        k_snprintf(a->name, sizeof a->name, "usb%d-%04x%04x", n,
                   info->vendor_id, info->product_id);
    }
    audio_label(a, info);
    a->dev.name       = a->name;
    a->dev.driver     = "usb-audio";
    k_snprintf(a->devid, sizeof a->devid, "usb:%u:%04x:%04x", info->root_port,
               info->vendor_id, info->product_id);
    a->dev.device_id  = a->devid;
    a->dev.priv       = a;          // how every op finds THIS DAC
    a->dev.label      = a->label;
    a->dev.start      = audio_start;
    a->dev.stop       = audio_stop;
    a->dev.set_volume = audio_set_volume;
    a->dev.set_format = audio_format;
    // WHAT CAN BE ASKED FOR: the playable alternates' widths, and their
    // rates -- a UAC2 clock's for all of them. A pairing no alternate
    // holds (a rate too busy for the 16-bit one's packet) is refused by
    // audio_format() when it is asked.
    for (int f = 0; f < s.nformats; f++) {
        a->dev.rates  |= s.uac2 ? a->clock_rates : s.formats[f].rates;
        a->dev.depths |= snd_depth_mask(s.formats[f].bits);
    }
    a->dev.bits = s.bits;   // the default: the deepest that plays 48 kHz
    a->dev.rate = SND_RATE;

    if (!sound_register(&a->dev, (void *)a->ring, 0)) {
        a->in_use = 0;
        pmm_free_contiguous(a->pkt_phys, 1);
        return 0;
    }
    usb_mark_bound(info, "usb-audio");
    klog_printf("usb: slot %u: bound as usb-audio, UAC%u, if %u alt %u, "
                "ep 0x%x %u-bit\n", info->slot, s.uac2 ? 2 : 1,
                s.ifnum, s.alt, s.ep, s.bits);
    a->pace.rate = SND_RATE;   // plan the default, for the log's numbers
    plan_packets(a);
    klog_printf("usb-audio: %u frame(s) every %u us = %u B/interval at %u Hz "
                "(endpoint holds %u), %u packets, IOC every %u, %u format(s), "
                "rates %#x%s\n",
                usb_audio_pace_max(SND_RATE, us), us, a->stride, (unsigned)SND_RATE,
                mps, a->packets, a->group, s.nformats, a->dev.rates,
                a->has_volume ? ", volume" : "");
    return 1;
}

void usb_audio_unbind(uint8_t slot) {
    struct audio_dev *a = 0;
    for (int i = 0; i < USB_AUDIO_MAX; i++)
        if (g_audio[i].in_use && g_audio[i].slot == slot) { a = &g_audio[i]; break; }
    if (!a) return;
    a->running = 0;
    a->in_use = 0;         // unpublished before the core can call start()
    sound_unregister(&a->dev);
    pmm_free_contiguous(a->pkt_phys, 1);
    a->pkt = 0;
    a->pkt_phys = 0;
}

int usb_audio_bound(void) {
    for (int i = 0; i < USB_AUDIO_MAX; i++)
        if (g_audio[i].in_use) return 1;
    return 0;
}

// --- KTESTs: the descriptor walk, device-free -------------------------
//
// The fixture is QEMU's usb-audio configuration, byte for byte off the
// wire (captured with -device usb-audio,pcap=). A hand-written one
// would only ever agree with this parser's own idea of the layout.
static const uint8_t QEMU_AUDIO_CFG[] = {
    0x09, 0x02, 0x71, 0x00, 0x02, 0x01, 0x04, 0xc0, 0x32,
    0x09, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x05,
    0x09, 0x24, 0x01, 0x00, 0x01, 0x2b, 0x00, 0x01, 0x01,
    0x0c, 0x24, 0x02, 0x01, 0x01, 0x01, 0x00, 0x02, 0x03, 0x00, 0x00, 0x06,
    0x0d, 0x24, 0x06, 0x02, 0x01, 0x02, 0x01, 0x00, 0x02, 0x00, 0x02, 0x00, 0x07,
    0x09, 0x24, 0x03, 0x03, 0x01, 0x03, 0x00, 0x02, 0x08,
    0x09, 0x04, 0x01, 0x00, 0x00, 0x01, 0x02, 0x00, 0x09,
    0x09, 0x04, 0x01, 0x01, 0x01, 0x01, 0x02, 0x00, 0x0a,
    0x07, 0x24, 0x01, 0x01, 0x00, 0x01, 0x00,
    0x0b, 0x24, 0x02, 0x01, 0x02, 0x02, 0x10, 0x01, 0x80, 0xbb, 0x00,
    0x09, 0x05, 0x01, 0x0d, 0xc0, 0x00, 0x01, 0x00, 0x00,
    0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00,
};

KTEST("usb-audio", "QEMU's descriptors yield the alt-1 isochronous endpoint") {
    struct usb_audio_stream s;
    struct usb_audio_report rep;
    KTEST_ASSERT(usb_audio_parse(QEMU_AUDIO_CFG, sizeof QEMU_AUDIO_CFG, &s, &rep));
    KTEST_ASSERT_EQ(s.ifnum, 1);
    KTEST_ASSERT_EQ(s.alt, 1);          // NOT alt 0, which has no endpoint
    KTEST_ASSERT_EQ(s.ep, 0x01);        // isochronous OUT
    KTEST_ASSERT_EQ(s.mps, 192);        // 48000 x 4 bytes / 1000 frames
    KTEST_ASSERT_EQ(s.interval, 1);
    KTEST_ASSERT_EQ(s.ac_ifnum, 0);     // interface 0, NOT the streaming one
    KTEST_ASSERT_EQ(s.feature_unit, 2);
    KTEST_ASSERT_EQ(s.has_volume, 1);
    KTEST_ASSERT_EQ(s.has_mute, 1);
    // The report is filled on a SUCCESSFUL parse too -- it is the same
    // walk, and a report only produced on failure could not be checked
    // by a test on hardware that works.
    KTEST_ASSERT_EQ(rep.uac_major, 1);
    KTEST_ASSERT_EQ(rep.uac_minor, 0);
    KTEST_ASSERT_EQ(rep.alt_count, 1);
    KTEST_ASSERT_EQ(rep.alts[0].channels, 2);
    KTEST_ASSERT_EQ(rep.alts[0].bits, 16);
    KTEST_ASSERT_EQ(rep.alts[0].rate, SND_RATE);
    KTEST_ASSERT_EQ(rep.alts[0].mult, 1);
    KTEST_ASSERT_EQ(rep.alts[0].mps, 192);
}

KTEST("usb-audio", "a device at another rate is refused, not resampled") {
    uint8_t cfg[sizeof QEMU_AUDIO_CFG];
    k_memcpy(cfg, QEMU_AUDIO_CFG, sizeof cfg);
    // tSamFreq: 44100 (0x00ac44) where the format descriptor said 48000.
    for (unsigned i = 0; i + 3 < sizeof cfg; i++) {
        if (cfg[i] == 0x80 && cfg[i + 1] == 0xbb && cfg[i + 2] == 0x00) {
            cfg[i] = 0x44; cfg[i + 1] = 0xac; cfg[i + 2] = 0x00;
            break;
        }
    }
    struct usb_audio_stream s;
    KTEST_ASSERT_EQ(usb_audio_parse(cfg, sizeof cfg, &s, 0), 0);
}

KTEST("usb-audio", "a truncated descriptor is refused rather than guessed at") {
    struct usb_audio_stream s;
    // A bLength that runs past the buffer, which is what a hostile or
    // broken device gives you and what a walk without bounds follows.
    uint8_t cfg[] = { 0x09, 0x02, 0x71, 0x00, 0x02, 0x01, 0x04, 0xc0, 0x32,
                      0x40, 0x04, 0x00, 0x00 };
    KTEST_ASSERT_EQ(usb_audio_parse(cfg, sizeof cfg, &s, 0), 0);
}

// A Sound BlasterX G6 (041e:3256), the UAC2 device this support was
// written for. RECONSTRUCTED from its `lsusb -v`, not captured -- and
// the reason that is worth trusting is that a configuration states its
// own length twice: the AC header's wTotalLength (0x013e) and the
// configuration's (0x0269). Both come out exactly, which no wrong
// layout does. `lsusb -D` inside toy-os prints the captured bytes.
static const uint8_t G6_AUDIO_CFG[] = {
    0x09, 0x02, 0x69, 0x02, 0x05, 0x01, 0x00, 0x80, 0xfa, 0x08, 0x0b, 0x00,
    0x03, 0x01, 0x00, 0x20, 0x00, 0x09, 0x04, 0x00, 0x00, 0x01, 0x01, 0x01,
    0x20, 0x02, 0x09, 0x24, 0x01, 0x00, 0x02, 0x08, 0x3e, 0x01, 0x00, 0x08,
    0x24, 0x0a, 0x0f, 0x03, 0x03, 0x00, 0x0c, 0x08, 0x24, 0x0a, 0x10, 0x03,
    0x03, 0x00, 0x0d, 0x09, 0x24, 0x0b, 0x11, 0x02, 0x0f, 0x10, 0x03, 0x00,
    0x11, 0x24, 0x02, 0x21, 0x01, 0x01, 0x00, 0x11, 0x02, 0x03, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x24, 0x02, 0x23, 0x03, 0x06, 0x00,
    0x11, 0x02, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x11, 0x24,
    0x02, 0x24, 0x01, 0x02, 0x00, 0x11, 0x02, 0x03, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x07, 0x11, 0x24, 0x02, 0x25, 0x05, 0x06, 0x00, 0x11, 0x02,
    0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x11, 0x24, 0x02, 0x27,
    0x02, 0x06, 0x00, 0x11, 0x02, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x05, 0x0c, 0x24, 0x03, 0x2a, 0x01, 0x03, 0x00, 0x01, 0x11, 0x00, 0x00,
    0x04, 0x0c, 0x24, 0x03, 0x2b, 0x01, 0x01, 0x00, 0x0e, 0x11, 0x00, 0x00,
    0x00, 0x12, 0x24, 0x06, 0x01, 0x0d, 0x03, 0x00, 0x00, 0x00, 0x0c, 0x00,
    0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x12, 0x24, 0x06, 0x03, 0x23,
    0x03, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00,
    0x00, 0x12, 0x24, 0x06, 0x04, 0x24, 0x03, 0x00, 0x30, 0x00, 0x0c, 0x00,
    0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x12, 0x24, 0x06, 0x05, 0x25,
    0x03, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00,
    0x00, 0x12, 0x24, 0x06, 0x06, 0x27, 0x03, 0x00, 0x00, 0x00, 0x0c, 0x00,
    0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x12, 0x24, 0x06, 0x09, 0x23,
    0x03, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00,
    0x00, 0x12, 0x24, 0x06, 0x0a, 0x24, 0x03, 0x00, 0x00, 0x00, 0x0c, 0x00,
    0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x12, 0x24, 0x06, 0x0c, 0x25,
    0x03, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00,
    0x00, 0x0b, 0x24, 0x05, 0x0e, 0x04, 0x03, 0x04, 0x05, 0x06, 0x03, 0x00,
    0x14, 0x24, 0x04, 0x0d, 0x04, 0x21, 0x09, 0x0a, 0x0c, 0x02, 0x03, 0x00,
    0x00, 0x00, 0x00, 0x99, 0x99, 0x99, 0x00, 0x00, 0x07, 0x05, 0x84, 0x03,
    0x06, 0x00, 0x01, 0x09, 0x04, 0x01, 0x00, 0x00, 0x01, 0x02, 0x20, 0x00,
    0x09, 0x04, 0x01, 0x01, 0x02, 0x01, 0x02, 0x20, 0x00, 0x10, 0x24, 0x01,
    0x21, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00,
    0x00, 0x06, 0x24, 0x02, 0x01, 0x03, 0x18, 0x07, 0x05, 0x01, 0x05, 0x26,
    0x01, 0x01, 0x08, 0x25, 0x01, 0x00, 0x00, 0x01, 0x0a, 0x00, 0x07, 0x05,
    0x81, 0x11, 0x04, 0x00, 0x04, 0x09, 0x04, 0x01, 0x02, 0x02, 0x01, 0x02,
    0x20, 0x00, 0x10, 0x24, 0x01, 0x21, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00,
    0x02, 0x03, 0x00, 0x00, 0x00, 0x00, 0x06, 0x24, 0x02, 0x01, 0x04, 0x20,
    0x07, 0x05, 0x01, 0x05, 0x88, 0x01, 0x01, 0x08, 0x25, 0x01, 0x00, 0x00,
    0x01, 0x0a, 0x00, 0x07, 0x05, 0x81, 0x11, 0x04, 0x00, 0x04, 0x09, 0x04,
    0x02, 0x00, 0x00, 0x01, 0x02, 0x20, 0x00, 0x09, 0x04, 0x02, 0x01, 0x01,
    0x01, 0x02, 0x20, 0x00, 0x10, 0x24, 0x01, 0x2b, 0x00, 0x01, 0x01, 0x00,
    0x00, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00, 0x00, 0x06, 0x24, 0x02, 0x01,
    0x03, 0x18, 0x07, 0x05, 0x82, 0x05, 0x26, 0x01, 0x01, 0x08, 0x25, 0x01,
    0x00, 0x00, 0x01, 0x0a, 0x00, 0x09, 0x04, 0x02, 0x02, 0x01, 0x01, 0x02,
    0x20, 0x00, 0x10, 0x24, 0x01, 0x2b, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00,
    0x02, 0x03, 0x00, 0x00, 0x00, 0x00, 0x06, 0x24, 0x02, 0x01, 0x04, 0x20,
    0x07, 0x05, 0x82, 0x05, 0x88, 0x01, 0x01, 0x08, 0x25, 0x01, 0x00, 0x00,
    0x01, 0x0a, 0x00, 0x09, 0x04, 0x03, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
    0x09, 0x21, 0x00, 0x01, 0x00, 0x01, 0x22, 0x44, 0x00, 0x07, 0x05, 0x83,
    0x03, 0x10, 0x00, 0x01, 0x09, 0x04, 0x04, 0x00, 0x01, 0x03, 0x00, 0x00,
    0x00, 0x09, 0x21, 0x00, 0x01, 0x00, 0x01, 0x22, 0x1d, 0x00, 0x07, 0x05,
    0x85, 0x03, 0x40, 0x00, 0x02,
};

KTEST("usb-audio", "the G6's UAC2 descriptors yield its deepest alternate, the 32-bit one") {
    struct usb_audio_stream s;
    struct usb_audio_report rep;
    KTEST_ASSERT(usb_audio_parse(G6_AUDIO_CFG, sizeof G6_AUDIO_CFG, &s, &rep));
    KTEST_ASSERT_EQ(sizeof G6_AUDIO_CFG, 617);   // the device's own wTotalLength
    KTEST_ASSERT_EQ(s.uac2, 1);
    KTEST_ASSERT_EQ(rep.uac_major, 2);
    KTEST_ASSERT_EQ(s.ifnum, 1);
    KTEST_ASSERT_EQ(s.alt, 2);        // 32-bit, over alt 1's 24
    KTEST_ASSERT_EQ(s.ep, rep.alts[2].ep);
    KTEST_ASSERT(!(s.ep & 0x80));     // the OUT endpoint, not a feedback one
    KTEST_ASSERT_EQ(s.subslot, 4);
    KTEST_ASSERT_EQ(s.bits, 32);
    // The ENDPOINT's ceiling, sized for 384 kHz plus a frame. Sending
    // that many every 125 us is what it must not do.
    KTEST_ASSERT_EQ(s.mps, rep.alts[2].mps);
    KTEST_ASSERT_EQ(s.interval, 1);
    KTEST_ASSERT_EQ(s.terminal_link, 33);
    KTEST_ASSERT_EQ(s.ac_ifnum, 0);
}

KTEST("usb-audio", "the G6's PLAYBACK feature unit is chosen, not the first") {
    struct usb_audio_stream s;
    // Eight feature units; unit 1 is the one feeding the Speaker output
    // terminal, and unit 3 is a line input's. Taking the first would
    // put the system volume slider on a microphone.
    KTEST_ASSERT(usb_audio_parse(G6_AUDIO_CFG, sizeof G6_AUDIO_CFG, &s, 0));
    KTEST_ASSERT_EQ(s.feature_unit, 1);
    KTEST_ASSERT_EQ(s.has_volume, 1);
    KTEST_ASSERT_EQ(s.has_mute, 1);
}

// Two feature units where the PLAYBACK one is not the first, which the
// G6 fixture cannot distinguish -- its unit 1 is both. Synthetic on
// purpose: this tests our topology walk, not a device.
static const uint8_t TWO_FU_CFG[] = {
    0x09, 0x02, 0x36, 0x00, 0x02, 0x01, 0x00, 0x80, 0x32,
    0x09, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x20, 0x00,     // AC, UAC2
    0x09, 0x24, 0x01, 0x00, 0x02, 0x01, 0x0f, 0x00, 0x00,     // HEADER 2.00
    0x12, 0x24, 0x06, 0x07, 0x01,                             // FEATURE_UNIT 7
        0x03, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00,
    0x12, 0x24, 0x06, 0x09, 0x02,                             // FEATURE_UNIT 9
        0x03, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00,
    // The speaker's source is unit 9, not the unit that came first.
    0x0c, 0x24, 0x03, 0x2a, 0x01, 0x03, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00,
};

KTEST("usb-audio", "the feature unit FOLLOWS the speaker, not descriptor order") {
    struct usb_audio_stream s;
    // No stream in this fixture, so the parse returns 0 -- the feature
    // unit is still resolved, which is what is being checked.
    usb_audio_parse(TWO_FU_CFG, sizeof TWO_FU_CFG, &s, 0);
    KTEST_ASSERT_EQ(s.feature_unit, 9);
    KTEST_ASSERT_EQ(s.has_volume, 1);
    KTEST_ASSERT_EQ(s.has_mute, 1);
}

KTEST("usb-audio", "the G6's rate goes to a clock SELECTOR, not a source") {
    struct usb_audio_stream s;
    KTEST_ASSERT(usb_audio_parse(G6_AUDIO_CFG, sizeof G6_AUDIO_CFG, &s, 0));
    KTEST_ASSERT_EQ(s.clock_id, 17);          // the selector the terminal names
    KTEST_ASSERT_EQ(s.clock_is_selector, 1);
    KTEST_ASSERT_EQ(s.clock_pin_count, 2);
    KTEST_ASSERT_EQ(s.clock_pins[0], 15);     // DSP clock
    KTEST_ASSERT_EQ(s.clock_pins[1], 16);     // stereo direct
}

KTEST("usb-audio", "every one of the G6's isochronous alternates is reported") {
    struct usb_audio_stream s;
    struct usb_audio_report rep;
    KTEST_ASSERT(usb_audio_parse(G6_AUDIO_CFG, sizeof G6_AUDIO_CFG, &s, &rep));
    // Four streaming alternates, and the two playback ones carry a
    // feedback endpoint each: six isochronous endpoints in all.
    KTEST_ASSERT_EQ(rep.alts_seen, 6);
    KTEST_ASSERT_EQ(rep.alts[0].bits, 24);
    KTEST_ASSERT_EQ(rep.alts[0].sync, 1);     // asynchronous
    KTEST_ASSERT_EQ(rep.alts[0].mult, 1);
    KTEST_ASSERT_EQ(rep.alts[1].ep, 0x81);    // the feedback endpoint
    KTEST_ASSERT_EQ(rep.alts[2].bits, 32);
    // A UAC2 format descriptor states no rate, and the report says 0
    // rather than inventing one.
    KTEST_ASSERT_EQ(rep.alts[0].rate, 0);
}

KTEST("usb-audio", "bInterval means microframes at high speed and frames below") {
    // The same 1 is 125 us on the G6 and 1 ms on a full-speed device --
    // a factor of eight in how much audio a packet must carry.
    KTEST_ASSERT_EQ(interval_us(XHCI_SPEED_HIGH, 1), 125);
    KTEST_ASSERT_EQ(interval_us(XHCI_SPEED_HIGH, 4), 1000);
    KTEST_ASSERT_EQ(interval_us(XHCI_SPEED_FULL, 1), 1000);
    KTEST_ASSERT_EQ(interval_us(XHCI_SPEED_FULL, 8), 8000);
}

KTEST("usb-audio", "an s32 sample becomes the TOP bytes of the subslot, rounded") {
    // The bottom bytes would be a 256x attenuation, which sounds like
    // silence rather than like a bug.
    uint8_t b[4] = {0xAA, 0xAA, 0xAA, 0xAA};
    write_sample(b, 0x12345678, 4);
    KTEST_ASSERT_EQ(b[0], 0x78);
    KTEST_ASSERT_EQ(b[3], 0x12);
    write_sample(b, 0x12345680, 3);            // 0x80 rounds the 24-bit sample up
    KTEST_ASSERT_EQ(b[0], 0x57);
    KTEST_ASSERT_EQ(b[1], 0x34);
    KTEST_ASSERT_EQ(b[2], 0x12);
    write_sample(b, 0x12348000, 2);            // and 0x8000 the 16-bit one
    KTEST_ASSERT_EQ(b[0], 0x35);
    KTEST_ASSERT_EQ(b[1], 0x12);
    write_sample(b, 0x7FFFFFFF, 3);            // the top clamps, never wraps
    KTEST_ASSERT_EQ(b[2], 0x7F);
    KTEST_ASSERT_EQ(b[1], 0xFF);
}

// --- formats, rates and pacing (pure, so any machine runs them) ---------

KTEST("usb-audio", "the G6 offers both playback alternates as formats, on one endpoint") {
    struct usb_audio_stream s;
    struct usb_audio_report rep;
    KTEST_ASSERT(usb_audio_parse(G6_AUDIO_CFG, sizeof G6_AUDIO_CFG, &s, &rep));
    KTEST_ASSERT_EQ(s.nformats, 2);
    KTEST_ASSERT_EQ(s.formats[0].bits, 24);
    KTEST_ASSERT_EQ(s.formats[1].bits, 32);
    KTEST_ASSERT_EQ(s.formats[0].ep, s.ep);
    KTEST_ASSERT_EQ(s.formats[1].ep, s.ep);
    KTEST_ASSERT_EQ(s.formats[1].alt, s.alt);
    // UAC2: an alternate states no rate; the clock's range is the list.
    KTEST_ASSERT_EQ(s.formats[0].rates, 0);
}

KTEST("usb-audio", "a format is picked by rate and width, the deepest when asked for none") {
    struct usb_audio_stream s;
    KTEST_ASSERT(usb_audio_parse(G6_AUDIO_CFG, sizeof G6_AUDIO_CFG, &s, 0));
    uint32_t clock = SND_RATE_44100 | SND_RATE_48000 | SND_RATE_96000;
    KTEST_ASSERT_EQ(usb_audio_pick_format(&s, clock, 44100, 0, 125), 1);    // 32-bit
    KTEST_ASSERT_EQ(usb_audio_pick_format(&s, clock, 44100, 24, 125), 0);
    KTEST_ASSERT_EQ(usb_audio_pick_format(&s, clock, 44100, 16, 125), -1);   // no such alternate
    KTEST_ASSERT_EQ(usb_audio_pick_format(&s, clock, 192000, 0, 125), -1);  // not the clock's
    // QEMU's UAC1 device lists 48 kHz alone, in its format descriptor.
    KTEST_ASSERT(usb_audio_parse(QEMU_AUDIO_CFG, sizeof QEMU_AUDIO_CFG, &s, 0));
    KTEST_ASSERT_EQ(s.nformats, 1);
    KTEST_ASSERT_EQ(s.formats[0].rates, SND_RATE_48000);
    KTEST_ASSERT_EQ(usb_audio_pick_format(&s, 0, 48000, 0, 1000), 0);
    KTEST_ASSERT_EQ(usb_audio_pick_format(&s, 0, 44100, 0, 1000), -1);
}

KTEST("usb-audio", "44.1 kHz packets alternate 44 and 45 frames and sum exactly") {
    struct usb_audio_pace p = { 44100, 1000, 0 };
    uint32_t total = 0;
    for (int i = 0; i < 1000; i++) {
        uint32_t n = usb_audio_pace_next(&p);
        KTEST_ASSERT(n == 44 || n == 45);
        total += n;
    }
    KTEST_ASSERT_EQ(total, 44100u);                       // one second, to the frame
    KTEST_ASSERT_EQ(usb_audio_pace_max(44100, 1000), 45u);
    struct usb_audio_pace q = { 48000, 125, 0 };          // high speed: exactly 6
    for (int i = 0; i < 16; i++) KTEST_ASSERT_EQ(usb_audio_pace_next(&q), 6u);
    KTEST_ASSERT_EQ(usb_audio_pace_max(48000, 125), 6u);
}

KTEST("usb-audio", "a clock's GET RANGE answer becomes rate bits") {
    // Three discrete subranges, then a continuous one from a second
    // device: 44.1/48/96, and 8..48 kHz at any step.
    static const uint8_t three[2 + 36] = {
        3, 0,
        0x44, 0xac, 0, 0,  0x44, 0xac, 0, 0,  0, 0, 0, 0,
        0x80, 0xbb, 0, 0,  0x80, 0xbb, 0, 0,  0, 0, 0, 0,
        0x00, 0x77, 1, 0,  0x00, 0x77, 1, 0,  0, 0, 0, 0,
    };
    KTEST_ASSERT_EQ(usb_audio_range_rates(three, sizeof three),
                    SND_RATE_44100 | SND_RATE_48000 | SND_RATE_96000);
    static const uint8_t span[2 + 12] = {
        1, 0, 0x40, 0x1f, 0, 0,  0x80, 0xbb, 0, 0,  0, 0, 0, 0,
    };
    KTEST_ASSERT_EQ(usb_audio_range_rates(span, sizeof span),
                    SND_RATE_8000 | SND_RATE_11025 | SND_RATE_16000 | SND_RATE_22050 |
                    SND_RATE_32000 | SND_RATE_44100 | SND_RATE_48000);
    // Cut short: only what it fully holds.
    KTEST_ASSERT_EQ(usb_audio_range_rates(three, 2 + 12), SND_RATE_44100);
}
