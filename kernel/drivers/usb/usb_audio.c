// USB Audio Class 1.0 playback: a speaker on the other end of an
// isochronous OUT endpoint, registered as a `struct sound_device` like
// any other card.
//
// WHY UAC1 AND NOT UAC2. Class 1.0 is what a device speaks when it
// wants to work everywhere without a driver -- every OS has carried it
// since Windows 98 -- and it is what QEMU's `usb-audio` emulates, which
// is the only way anything here is tested. UAC2 adds high-speed rates
// and a clock-source topology this OS has no use for at one fixed
// format.
//
// THE FORMAT IS NOT NEGOTIATED, and that is the whole reason this
// driver is small. abi/sound_abi.h fixes the stream at 48 kHz stereo
// s16le; a UAC1 device advertising exactly that is bound and one
// advertising anything else is REFUSED rather than resampled, because
// resampling belongs in userland/lib/usnd.h where it already exists.
// A packet is the rate's share of ONE SERVICE INTERVAL, derived from
// the endpoint's own bInterval -- 192 bytes per millisecond at 48 kHz.
// The endpoint is driven synchronously, off the bus's own SOF clock,
// so there is no drift to correct and no feedback endpoint to
// implement.
//
// THE SAMPLES ARE COPIED, unlike the AC'97's descriptor list which
// points straight into the core's ring. 192 divides neither the 64 KiB
// ring nor its 2 KiB chunks, and an xHCI TRB may not cross a 64 KiB
// boundary -- so a zero-copy packet would need chained split TRBs at
// two kinds of edge. Copying 192 KB/s into a frame of our own is
// nothing, and it is what Linux's snd-usb-audio does for the same
// reason. kernel/sound.h's class comment allows for exactly this.
#include "usb.h"
#include "usb_audio.h"
#include "xhci.h"
#include "xhci_regs.h"
#include "sound.h"
#include "sound_abi.h"
#include "pmm.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"
#include "ktest.h"
#include "barrier.h"   // cpu_relax() in the drain poll

// Audio class codes (USB Device Class Definition for Audio Devices 1.0).
#define AUDIO_CLASS            1
#define AUDIO_SUB_CONTROL      1
#define AUDIO_SUB_STREAMING    2

#define DESC_CS_INTERFACE      0x24
#define DESC_CS_ENDPOINT       0x25
#define DESC_INTERFACE         0x04
#define DESC_ENDPOINT          0x05

#define AC_HEADER              0x01
#define AC_INPUT_TERMINAL      0x02
#define AC_OUTPUT_TERMINAL     0x03
#define AC_FEATURE_UNIT        0x06
#define AC_CLOCK_SOURCE        0x0A
#define AC_CLOCK_SELECTOR      0x0B
#define AS_GENERAL             0x01
#define AS_FORMAT_TYPE         0x02
#define FORMAT_TYPE_I          0x01

// bInterfaceProtocol on every audio interface of a UAC2 device.
#define UAC2_PROTOCOL          0x20
#define TERM_USB_STREAMING     0x0101

// A feature unit's controls. UAC1 gives each control ONE bit; UAC2
// gives it TWO (01 read-only, 11 read/write), which is why the masks
// differ rather than just the stride.
#define FU1_MUTE               (1u << 0)
#define FU1_VOLUME             (1u << 1)
#define FU2_MUTE               0x03u
#define FU2_VOLUME             0x0Cu

// Class requests on an interface. SET_CUR/GET_* carry the control
// selector in the high byte of wValue and the channel in the low byte.
// THE REQUEST CODE IS VERSION-SPECIFIC. UAC1 puts the direction in the
// code (SET_CUR 0x01, GET_CUR 0x81); UAC2 has one CUR code and puts the
// direction in bmRequestType, so a UAC1 GET_CUR sent to a UAC2 device
// is request 0x81, which it does not implement -- a stall, not an
// error message.
#define AUDIO_REQ_SET_CUR      0x01
#define AUDIO_REQ_GET_CUR      0x81
#define AUDIO_REQ_GET_MIN      0x82
#define AUDIO_REQ_GET_MAX      0x83
#define UAC2_REQ_CUR           0x01
#define UAC2_REQ_RANGE         0x02

#define AUDIO_CS_MUTE          0x01
#define AUDIO_CS_VOLUME        0x02
#define CS_SAM_FREQ            0x01   // on a clock source
#define CX_CLOCK_SELECT        0x01   // on a clock selector
#define REQ_SET_INTERFACE      0x0B

#define TYPE_OUT_CLASS_IF      0x21   // host->device, class, interface
#define TYPE_IN_CLASS_IF       0xA1   // device->host, class, interface
#define TYPE_OUT_STD_IF        0x01   // host->device, standard, interface

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

    // ONE PACKET, in three units, and confusing them is silent. `frames`
    // is the rate's share of one service interval -- NOT the endpoint's
    // wMaxPacketSize, which is a ceiling a device may set well above the
    // rate (the G6's is sized for 384 kHz). `ring_bytes` is what that
    // costs in the s16 ring; `wire_bytes` is what it costs on the wire,
    // and the two differ on every device wider than 16 bits.
    uint16_t frames;
    uint16_t ring_bytes;
    uint16_t wire_bytes;
    // Packets per completion interrupt: one interrupt per millisecond,
    // so a 125 us endpoint costs 1000 a second and not 8000. Linux's
    // snd-usb-audio groups for the same reason.
    uint8_t  group;

    uint8_t *pkt;            // `packets` x wire_bytes, one frame
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
    char label[32];
    struct sound_device dev;
};

// One is all the sound core can use anyway -- a second USB card would
// register and sit inactive, which is exactly what a second one of any
// kind does.
static struct audio_dev g_audio;

// --- descriptor parsing (pure, and KTESTed below) ---------------------

static uint32_t le24(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

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
static void find_feature_unit(const uint8_t *cfg, uint32_t total,
                              struct usb_audio_stream *out) {
    uint8_t want = 0;
    for (uint32_t o = 0; o + 2 <= total; ) {
        uint32_t blen = cfg[o];
        if (blen < 2 || o + blen > total) break;
        // bSourceID is at offset 7 in an output terminal in BOTH
        // versions -- UAC2 only appends fields after it.
        if (cfg[o + 1] == DESC_CS_INTERFACE && blen >= 9 &&
            cfg[o + 2] == AC_OUTPUT_TERMINAL && !want) {
            uint16_t type = (uint16_t)(cfg[o + 4] | ((uint16_t)cfg[o + 5] << 8));
            if (type != TERM_USB_STREAMING) want = cfg[o + 7];
        }
        o += blen;
    }

    for (uint32_t o = 0; o + 2 <= total; ) {
        uint32_t blen = cfg[o];
        if (blen < 2 || o + blen > total) break;
        if (cfg[o + 1] != DESC_CS_INTERFACE || blen < 7 ||
            cfg[o + 2] != AC_FEATURE_UNIT) { o += blen; continue; }
        if ((want && cfg[o + 3] != want) || (!want && out->feature_unit)) {
            o += blen;
            continue;
        }
        out->feature_unit = cfg[o + 3];
        if (out->uac2) {
            for (uint32_t i = 5; i + 4 <= blen - 1; i += 4) {
                if (cfg[o + i] & FU2_VOLUME) out->has_volume = 1;
                if (cfg[o + i] & FU2_MUTE)   out->has_mute = 1;
            }
        } else {
            // bControlSize, then bmaControls[] of that width. Only the
            // first byte of each is read: everything past mute and
            // volume is a control this driver does not touch.
            uint8_t size = cfg[o + 5];
            if (size >= 1)
                for (uint32_t i = 6; i + size <= blen - 1; i += size) {
                    if (cfg[o + i] & FU1_VOLUME) out->has_volume = 1;
                    if (cfg[o + i] & FU1_MUTE)   out->has_mute = 1;
                }
        }
        break;
    }
}

// The clock entity whose rate this stream runs at (UAC2 only): the one
// named by the input terminal the streaming interface links to. It may
// be a SELECTOR, which is a question to ask the device rather than a
// clock -- see set_clock_rate().
static void find_clock(const uint8_t *cfg, uint32_t total,
                       struct usb_audio_stream *out) {
    if (!out->uac2 || !out->terminal_link) return;

    uint8_t entity = 0;
    for (uint32_t o = 0; o + 2 <= total; ) {
        uint32_t blen = cfg[o];
        if (blen < 2 || o + blen > total) break;
        if (cfg[o + 1] == DESC_CS_INTERFACE && blen >= 8 &&
            cfg[o + 2] == AC_INPUT_TERMINAL && cfg[o + 3] == out->terminal_link) {
            entity = cfg[o + 7];          // bCSourceID
            break;
        }
        o += blen;
    }
    if (!entity) return;

    for (uint32_t o = 0; o + 2 <= total; ) {
        uint32_t blen = cfg[o];
        if (blen < 2 || o + blen > total) break;
        if (cfg[o + 1] != DESC_CS_INTERFACE || blen < 6 || cfg[o + 3] != entity) {
            o += blen;
            continue;
        }
        if (cfg[o + 2] == AC_CLOCK_SOURCE) {
            out->clock_id = entity;
            return;
        }
        if (cfg[o + 2] == AC_CLOCK_SELECTOR) {
            out->clock_id = entity;
            out->clock_is_selector = 1;
            uint32_t pins = cfg[o + 4];
            if (pins > USB_AUDIO_MAX_CLOCK_PINS) pins = USB_AUDIO_MAX_CLOCK_PINS;
            if (5 + pins > blen) pins = blen > 5 ? blen - 5 : 0;
            for (uint32_t i = 0; i < pins; i++) out->clock_pins[i] = cfg[o + 5 + i];
            out->clock_pin_count = (uint8_t)pins;
            return;
        }
        o += blen;
    }
}

// --- the configuration walk -------------------------------------------

// Walks a configuration for a playback stream this kernel can carry,
// UAC1 or UAC2. Returns 1 and fills `out` when it finds one, and fills
// `rep` with every AudioStreaming alternate it saw either way.
//
// The alternate settings are the point. An AudioStreaming interface
// always has an alt 0 with NO endpoints -- "idle, using no bandwidth" --
// and its real endpoint lives in alt 1 or later, which is why the
// interface walk in usb_enum.c (which records alt 0 only) cannot see
// it and this parse exists.
//
// WHAT IS CHECKED HERE IS THE FORMAT, NOT THE RATE. A UAC1 device lists
// its rates in the format descriptor and one without 48 kHz is refused
// here; a UAC2 device states no rate at all, so the check that matters
// for one is the SET_CUR in set_clock_rate() at bind.
int usb_audio_parse(const uint8_t *cfg, uint32_t total,
                    struct usb_audio_stream *out,
                    struct usb_audio_report *rep) {
    if (!cfg || !out) return 0;
    k_memset(out, 0, sizeof *out);
    if (rep) k_memset(rep, 0, sizeof *rep);

    uint32_t o = 0;
    int in_streaming = 0;       // inside an AudioStreaming alt setting
    int in_control = 0;
    int cur_if = -1, cur_alt = -1;
    uint8_t cand_channels = 0, cand_bits = 0, cand_subslot = 0;
    uint8_t cand_link = 0;
    uint32_t cand_rate = 0;
    int format_ok = 0;

    while (o + 2 <= total) {
        uint32_t blen = cfg[o];
        uint8_t  btype = cfg[o + 1];
        if (blen < 2 || o + blen > total) return 0;   // refuse, do not guess

        if (btype == DESC_INTERFACE && blen >= 9) {
            // A new interface descriptor ends the previous alt setting.
            cur_if  = cfg[o + 2];
            cur_alt = cfg[o + 3];
            int audio = (cfg[o + 5] == AUDIO_CLASS);
            in_streaming = audio && cfg[o + 6] == AUDIO_SUB_STREAMING;
            in_control   = audio && cfg[o + 6] == AUDIO_SUB_CONTROL;
            if (audio && cfg[o + 7] == UAC2_PROTOCOL) out->uac2 = 1;
            if (in_control) out->ac_ifnum = cfg[o + 2];
            cand_channels = 0; cand_bits = 0; cand_subslot = 0;
            cand_rate = 0; cand_link = 0; format_ok = 0;
        } else if (in_control && btype == DESC_CS_INTERFACE && blen >= 5 &&
                   cfg[o + 2] == AC_HEADER) {
            // bcdADC, and the one field that says UAC1 from UAC2. The
            // two share subtype numbers and agree on almost nothing
            // else, so a refusal that cannot name the version is a
            // refusal nobody can act on.
            if (cfg[o + 4] >= 2) out->uac2 = 1;
            if (rep && !rep->uac_major) {
                rep->uac_minor = cfg[o + 3];
                rep->uac_major = cfg[o + 4];
            }
        } else if (in_streaming && btype == DESC_CS_INTERFACE && blen >= 7 &&
                   cfg[o + 2] == AS_GENERAL) {
            cand_link = cfg[o + 3];             // bTerminalLink, both versions
            if (out->uac2 && blen >= 16) {
                // bmFormats is a BITMAP in UAC2 where UAC1 had a tag,
                // and bit 0 is PCM.
                if (!(cfg[o + 6] & 0x01)) cand_link = 0;
                cand_channels = cfg[o + 10];
            }
        } else if (in_streaming && btype == DESC_CS_INTERFACE && blen >= 6 &&
                   cfg[o + 2] == AS_FORMAT_TYPE && cfg[o + 3] == FORMAT_TYPE_I) {
            if (out->uac2) {
                // No channel count and NO RATE here: bSubslotSize and
                // bBitResolution are the whole descriptor.
                cand_subslot = cfg[o + 4];
                cand_bits    = cfg[o + 5];
                format_ok = cand_link && cand_channels == SND_CHANNELS;
            } else if (blen >= 8) {
                cand_channels = cfg[o + 4];
                cand_subslot  = cfg[o + 5];
                cand_bits     = cfg[o + 6];
                uint8_t freq_type = cfg[o + 7];
                int rate_ok = 0;
                if (freq_type == 0 && blen >= 14) {
                    // A continuous range: tLowerSamFreq, tUpperSamFreq.
                    rate_ok = (le24(&cfg[o + 8]) <= SND_RATE &&
                               le24(&cfg[o + 11]) >= SND_RATE);
                    cand_rate = rate_ok ? SND_RATE : le24(&cfg[o + 8]);
                } else {
                    for (uint32_t i = 0; i < freq_type; i++) {
                        uint32_t at = o + 8 + i * 3;
                        if (at + 3 > o + blen) break;
                        if (!cand_rate) cand_rate = le24(&cfg[at]);
                        if (le24(&cfg[at]) == SND_RATE) {
                            rate_ok = 1;
                            cand_rate = SND_RATE;
                            break;
                        }
                    }
                }
                format_ok = rate_ok && cand_channels == SND_CHANNELS;
            }
            // The sample widths the packet copy can write. 16-bit is a
            // memcpy from the ring; 24 and 32 are a shift per sample.
            if (cand_subslot < 2 || cand_subslot > 4 ||
                cand_bits > (uint8_t)(cand_subslot * 8))
                format_ok = 0;
        } else if (in_streaming && btype == DESC_ENDPOINT && blen >= 7) {
            uint8_t addr = cfg[o + 2];
            uint8_t attr = cfg[o + 3];
            uint16_t w = (uint16_t)(cfg[o + 4] | ((uint16_t)cfg[o + 5] << 8));
            // wMaxPacketSize is not a plain number on a high-speed
            // endpoint: bits 11-12 are ADDITIONAL transactions per
            // interval, so the whole 16 bits taken as a size is a
            // packet three times too big offered to the controller.
            uint16_t mps  = (uint16_t)(w & 0x7FF);
            uint8_t  mult = (uint8_t)(((w >> 11) & 3) + 1);
            if ((attr & 0x03) != 1) { o += blen; continue; }   // isochronous only

            if (rep) {
                rep->alts_seen++;
                if (rep->alt_count < USB_AUDIO_MAX_ALTS) {
                    struct usb_audio_alt *a = &rep->alts[rep->alt_count++];
                    a->ifnum = (uint8_t)cur_if;
                    a->alt = (uint8_t)cur_alt;
                    a->channels = cand_channels;
                    a->bits = cand_bits;
                    a->rate = cand_rate;
                    a->ep = addr;
                    a->sync = (uint8_t)((attr >> 2) & 3);
                    a->interval = cfg[o + 6];
                    a->mult = mult;
                    a->mps = mps;
                }
            }

            // A complete candidate: an alt setting with both the format
            // and an isochronous OUT endpoint we can actually program.
            // Taken as soon as it is seen, so a device offering two
            // usable widths binds the first -- 24-bit before 32-bit on
            // the one this was written against, which is the narrower
            // of the two and no worse at 16 bits of source.
            if (format_ok && !(addr & 0x80) && mult == 1 && !out->ep) {
                out->ifnum    = (uint8_t)cur_if;
                out->alt      = (uint8_t)cur_alt;
                out->ep       = addr;
                out->mps      = mps;
                out->interval = cfg[o + 6];
                out->subslot  = cand_subslot;
                out->bits     = cand_bits;
                out->terminal_link = cand_link;
            }
        }
        o += blen;
    }

    find_feature_unit(cfg, total, out);
    find_clock(cfg, total, out);
    return out->ep != 0;
}

// --- the packet pump --------------------------------------------------

static int set_interface(uint8_t slot, uint8_t ifnum, uint8_t alt) {
    uint8_t setup[8] = { TYPE_OUT_STD_IF, REQ_SET_INTERFACE, alt, 0,
                         ifnum, 0, 0, 0 };
    return xhci_control(slot, setup, 0, 0, 0);
}


// One s16 sample, MSB-aligned into the device's subslot. A 24-bit
// device wants the sample in the TOP of three bytes -- writing it into
// the bottom is a 256x attenuation, which sounds like silence rather
// than like a bug.
static void write_sample(uint8_t *dst, int16_t v, uint8_t subslot) {
    uint16_t u = (uint16_t)v;
    switch (subslot) {  // dispatch-ok: the three widths usb_audio_parse accepts
        case 2:
            dst[0] = (uint8_t)u; dst[1] = (uint8_t)(u >> 8);
            break;
        case 3:
            dst[0] = 0; dst[1] = (uint8_t)u; dst[2] = (uint8_t)(u >> 8);
            break;
        default:
            dst[0] = 0; dst[1] = 0;
            dst[2] = (uint8_t)u; dst[3] = (uint8_t)(u >> 8);
            break;
    }
}

static void copy_one_packet(struct audio_dev *a, uint8_t slot) {
    uint8_t *dst = a->pkt + (uint32_t)slot * a->wire_bytes;
    uint32_t at = a->copy_pos;

    if (a->s.subslot == 2) {
        uint32_t n = a->ring_bytes;
        // The ring wraps mid-packet on most laps -- 192 divides neither
        // the ring nor a chunk -- so this is two copies, not one.
        uint32_t first = SND_RING_BYTES - at;
        if (first > n) first = n;
        k_memcpy(dst, a->ring + at, first);
        if (first < n) k_memcpy(dst + first, a->ring, n - first);
        a->copy_pos = (at + n) % SND_RING_BYTES;
        return;
    }

    // Sample at a time, because the widths differ. `at` is always even
    // and the ring is a whole number of frames, so a sample never
    // straddles the wrap and only the loop's step has to check it.
    uint32_t samples = (uint32_t)a->frames * SND_CHANNELS;
    for (uint32_t i = 0; i < samples; i++) {
        int16_t v = (int16_t)((uint16_t)a->ring[at] |
                              ((uint16_t)a->ring[at + 1] << 8));
        write_sample(dst, v, a->s.subslot);
        dst += a->s.subslot;
        at += 2;
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
    a->play_pos = (a->play_pos + (uint32_t)a->ring_bytes * a->group) % SND_RING_BYTES;
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
                            a->pkt_phys + (uint64_t)slot * a->wire_bytes,
                            a->wire_bytes, k + 1 == a->group) == 0)
            a->inflight++;
    }
}

// A class request to an ENTITY on the AudioControl interface -- a
// feature unit, a clock source, a clock selector. The entity is an
// argument rather than always the feature unit because UAC2 puts the
// sample rate on a clock, and addressing it as the feature unit fails
// the transfer with nothing to see.
static int audio_control(uint8_t req, uint8_t type, uint8_t cs, uint8_t channel,
                         uint8_t entity, uint8_t *buf, uint16_t len) {
    struct audio_dev *a = &g_audio;
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

// UAC2 only: tell the device what rate to run at. Nothing in a UAC2
// descriptor states a rate, so THIS REQUEST IS THE NEGOTIATION -- a
// device that refuses it is one we cannot play through, which is why
// it is checked at bind rather than assumed at start.
static int set_clock_rate(void) {
    struct audio_dev *a = &g_audio;
    if (!a->s.uac2) return 0;               // UAC1: the rate is the format's
    if (!a->s.clock_id) {
        klog_write("usb-audio: UAC2 device names no clock for its stream\n");
        return -1;
    }

    uint8_t clock = a->s.clock_id;
    if (a->s.clock_is_selector) {
        // WHICH clock is ASKED, not chosen. On the device this was
        // written against the selector is a front-panel mode (a DSP
        // path and a direct path), so picking one at bind would
        // silently override what the owner set on the hardware.
        if (!a->s.clock_pin_count) return -1;
        uint8_t pin = 0;
        if (audio_control(UAC2_REQ_CUR, TYPE_IN_CLASS_IF, CX_CLOCK_SELECT, 0,
                          a->s.clock_id, &pin, 1) < 1 ||
            !pin || pin > a->s.clock_pin_count) {
            klog_printf("usb-audio: clock selector %u answered %u; using "
                        "source %u\n", a->s.clock_id, pin, a->s.clock_pins[0]);
            pin = 1;
        }
        clock = a->s.clock_pins[pin - 1];
    }

    uint8_t v[4] = { (uint8_t)(SND_RATE & 0xFF),
                     (uint8_t)((SND_RATE >> 8) & 0xFF),
                     (uint8_t)((SND_RATE >> 16) & 0xFF),
                     (uint8_t)((SND_RATE >> 24) & 0xFF) };
    if (audio_control(AUDIO_REQ_SET_CUR, TYPE_OUT_CLASS_IF, CS_SAM_FREQ, 0,
                      clock, v, 4) < 0) {
        klog_printf("usb-audio: clock %u refused %u Hz\n", clock,
                    (unsigned)SND_RATE);
        return -1;
    }
    klog_printf("usb-audio: clock %u set to %u Hz\n", clock, (unsigned)SND_RATE);
    return 0;
}

static int audio_start(void) {
    struct audio_dev *a = &g_audio;
    if (!a->in_use || !a->ring) return -1;
    if (a->running) return 0;

    // The rate first: a UAC2 clock is independent of the alternate, and
    // setting it while the endpoint is idle is what every host does.
    if (set_clock_rate() < 0) return -1;

    // THE ALTERNATE SETTING FOLLOWS THE STREAM, not the bind. Alt 0 is
    // the "idle, no bandwidth" setting every UAC device carries, and
    // sitting in alt 1 while playing nothing is not merely untidy: the
    // device's output is LIVE the whole time, which on QEMU means it
    // holds an open voice on its audiodev and the AC'97 sharing that
    // audiodev never gets clocked -- the guest then blocks forever on a
    // hardware position that cannot advance. Measured: with both cards
    // on one audiodev, choosing the AC'97 played nothing until this
    // driver started standing down.
    if (set_interface(a->slot, a->s.ifnum, a->s.alt) < 0) {
        klog_printf("usb-audio: could not claim interface %u alt %u\n",
                    a->s.ifnum, a->s.alt);
        return -1;
    }

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
                            a->pkt_phys + (uint64_t)i * a->wire_bytes,
                            a->wire_bytes,
                            (i + 1) % a->group == 0) < 0) {
            a->running = 0;
            return -1;
        }
        a->inflight++;
    }
    return 0;
}

static void audio_stop(void) {
    struct audio_dev *a = &g_audio;
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
    // first version waited on `pit_ticks()` and hung the machine solid
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

static int16_t read_db(uint8_t req, int16_t fallback) {
    uint8_t v[2] = {0, 0};
    if (audio_control(req, TYPE_IN_CLASS_IF, AUDIO_CS_VOLUME, 1,
                      g_audio.s.feature_unit, v, 2) < 2)
        return fallback;
    return (int16_t)((uint16_t)v[0] | ((uint16_t)v[1] << 8));
}

// UAC2 HAS NO GET_MIN/GET_MAX. It has one RANGE request answering with
// a block of subranges -- wNumSubRanges, then MIN/MAX/RES triples --
// and the first subrange is the one this uses. Asking a UAC2 device
// for GET_MIN is request 0x82, which it does not implement.
static void read_range(int16_t *min, int16_t *max) {
    uint8_t r[8] = {0};
    if (audio_control(UAC2_REQ_RANGE, TYPE_IN_CLASS_IF, AUDIO_CS_VOLUME, 1,
                      g_audio.s.feature_unit, r, sizeof r) < 8)
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

static void audio_set_volume(int pct) {
    struct audio_dev *a = &g_audio;
    if (!a->in_use || !a->s.feature_unit) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    if (a->s.has_mute) {
        uint8_t mute = (uint8_t)(pct == 0);
        audio_control(AUDIO_REQ_SET_CUR, TYPE_OUT_CLASS_IF, AUDIO_CS_MUTE,
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
    audio_control(AUDIO_REQ_SET_CUR, TYPE_OUT_CLASS_IF, AUDIO_CS_VOLUME, 1,
                  a->s.feature_unit, v, 2);
    audio_control(AUDIO_REQ_SET_CUR, TYPE_OUT_CLASS_IF, AUDIO_CS_VOLUME, 2,
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
// that "no 48 kHz stereo s16 stream" is not actionable on a machine
// nobody here owns -- the version and the alternates are.
static void log_refusal(uint8_t slot, const struct usb_audio_report *rep) {
    if (rep->uac_major)
        klog_printf("usb-audio: slot %u: UAC %u.%02u device offers no %u Hz "
                    "stereo s16 stream (%u alternate(s)):\n", slot,
                    rep->uac_major, rep->uac_minor, (unsigned)SND_RATE,
                    rep->alts_seen);
    else
        klog_printf("usb-audio: slot %u: audio device with no class header "
                    "offers no %u Hz stereo s16 stream (%u alternate(s)):\n",
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

int usb_audio_bind(struct usb_device_info *info, const uint8_t *cfg,
                   uint32_t total) {
    if (!info || g_audio.in_use) return 0;

    struct usb_audio_stream s;
    struct usb_audio_report rep;
    if (!usb_audio_parse(cfg, total, &s, &rep)) {
        log_refusal(info->slot, &rep);
        return 0;
    }

    // The rate's share of ONE service interval -- see audio_dev.frames.
    _Static_assert(SND_RATE % 1000 == 0, "frames per interval must be exact");
    uint32_t us = interval_us(info->speed, s.interval);
    uint32_t frames = (SND_RATE / 1000) * us / 1000;
    if (!frames || ((SND_RATE / 1000) * us) % 1000) {
        klog_printf("usb-audio: slot %u: %u Hz does not divide a %u us "
                    "interval -- not bound\n", info->slot,
                    (unsigned)SND_RATE, us);
        return 0;
    }
    uint32_t wire = frames * SND_CHANNELS * s.subslot;
    if (s.mps < wire) {
        klog_printf("usb-audio: slot %u: endpoint 0x%02x holds %u bytes and "
                    "%u Hz needs %u every %u us -- not bound\n",
                    info->slot, s.ep, s.mps, (unsigned)SND_RATE, wire, us);
        return 0;
    }

    struct audio_dev *a = &g_audio;
    k_memset(a, 0, sizeof *a);
    a->slot = info->slot;
    a->s = s;
    a->frames     = (uint16_t)frames;
    a->ring_bytes = (uint16_t)(frames * SND_FRAME_BYTES);
    a->wire_bytes = (uint16_t)wire;
    a->group      = (uint8_t)(us >= 1000 ? 1 : 1000 / us);

    uint32_t want = AUDIO_INFLIGHT_US / us;
    uint32_t fits = 4096 / a->wire_bytes;         // one frame holds them all
    if (want > fits) want = fits;
    if (want > AUDIO_PACKETS_MAX) want = AUDIO_PACKETS_MAX;
    want -= want % a->group;                      // whole groups only
    if (want < a->group) want = a->group;
    a->packets = (uint8_t)want;

    a->ring = sound_ring_alloc(0);
    if (!a->ring) {
        klog_write("usb: no contiguous frames for the sound ring\n");
        return 0;
    }
    a->pkt_phys = pmm_alloc_contiguous(1);
    if (!a->pkt_phys) return 0;
    a->pkt = (uint8_t *)(uintptr_t)a->pkt_phys;   // identity-mapped
    k_memset(a->pkt, 0, 4096);

    // The endpoint only exists in the alternate setting, so this has to
    // come before configuring it -- and the device is entitled to
    // refuse, which is a device we cannot play through rather than one
    // to configure anyway.
    if (set_interface(info->slot, s.ifnum, s.alt) < 0) {
        klog_printf("usb: slot %u: set interface %u alt %u failed\n",
                    info->slot, s.ifnum, s.alt);
        pmm_free_contiguous(a->pkt_phys, 1);
        return 0;
    }
    if (xhci_add_isoch_out(info->slot, s.ep, s.mps, s.interval,
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
    if (set_clock_rate() < 0) {
        klog_printf("usb-audio: slot %u: cannot set %u Hz -- not bound\n",
                    info->slot, (unsigned)SND_RATE);
        a->in_use = 0;
        pmm_free_contiguous(a->pkt_phys, 1);
        return 0;
    }

    if (s.has_volume) {
        if (s.uac2) {
            read_range(&a->vol_min, &a->vol_max);
        } else {
            a->vol_min = read_db(AUDIO_REQ_GET_MIN, 0);
            a->vol_max = read_db(AUDIO_REQ_GET_MAX, 0);
        }
        a->has_volume = (a->vol_max > a->vol_min);
    }

    k_strlcpy(a->name, "usb-audio", sizeof a->name);
    k_strlcpy(a->label, info->product[0] ? info->product : "USB Audio",
              sizeof a->label);
    a->dev.name       = a->name;
    a->dev.label      = a->label;
    a->dev.start      = audio_start;
    a->dev.stop       = audio_stop;
    a->dev.set_volume = audio_set_volume;

    if (!sound_register(&a->dev, (void *)a->ring, 0)) {
        a->in_use = 0;
        pmm_free_contiguous(a->pkt_phys, 1);
        return 0;
    }
    info->bound = 1;
    klog_printf("usb: slot %u: bound as usb-audio, UAC%u, if %u alt %u, "
                "ep 0x%x %u-bit\n", info->slot, s.uac2 ? 2 : 1,
                s.ifnum, s.alt, s.ep, s.bits);
    klog_printf("usb-audio: %u frame(s) every %u us = %u B/interval "
                "(endpoint holds %u), %u packets, IOC every %u%s\n",
                a->frames, us, a->wire_bytes, s.mps, a->packets, a->group,
                a->has_volume ? ", volume" : "");
    return 1;
}

void usb_audio_unbind(uint8_t slot) {
    struct audio_dev *a = &g_audio;
    if (!a->in_use || a->slot != slot) return;
    a->running = 0;
    a->in_use = 0;         // unpublished before the core can call start()
    sound_unregister(&a->dev);
    pmm_free_contiguous(a->pkt_phys, 1);
    a->pkt = 0;
    a->pkt_phys = 0;
}

int usb_audio_bound(void) { return g_audio.in_use; }

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

KTEST("usb-audio", "the G6's UAC2 descriptors yield its 24-bit alternate") {
    struct usb_audio_stream s;
    struct usb_audio_report rep;
    KTEST_ASSERT(usb_audio_parse(G6_AUDIO_CFG, sizeof G6_AUDIO_CFG, &s, &rep));
    KTEST_ASSERT_EQ(sizeof G6_AUDIO_CFG, 617);   // the device's own wTotalLength
    KTEST_ASSERT_EQ(s.uac2, 1);
    KTEST_ASSERT_EQ(rep.uac_major, 2);
    KTEST_ASSERT_EQ(s.ifnum, 1);
    KTEST_ASSERT_EQ(s.alt, 1);        // 24-bit, not alt 2's 32
    KTEST_ASSERT_EQ(s.ep, 0x01);      // the OUT endpoint, not 0x81's feedback
    KTEST_ASSERT_EQ(s.subslot, 3);
    KTEST_ASSERT_EQ(s.bits, 24);
    // 294 is the ENDPOINT's ceiling, sized for 384 kHz plus a frame.
    // Sending that many every 125 us is what it must not do.
    KTEST_ASSERT_EQ(s.mps, 294);
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

KTEST("usb-audio", "an s16 sample is written to the TOP of a wide subslot") {
    // Into the bottom is a 256x attenuation, which sounds like silence
    // rather than like a bug.
    uint8_t b[4] = {0xAA, 0xAA, 0xAA, 0xAA};
    write_sample(b, (int16_t)0x1234, 3);
    KTEST_ASSERT_EQ(b[0], 0x00);
    KTEST_ASSERT_EQ(b[1], 0x34);
    KTEST_ASSERT_EQ(b[2], 0x12);
    write_sample(b, (int16_t)0x1234, 4);
    KTEST_ASSERT_EQ(b[0], 0x00);
    KTEST_ASSERT_EQ(b[1], 0x00);
    KTEST_ASSERT_EQ(b[2], 0x34);
    KTEST_ASSERT_EQ(b[3], 0x12);
    write_sample(b, (int16_t)0x1234, 2);
    KTEST_ASSERT_EQ(b[0], 0x34);
    KTEST_ASSERT_EQ(b[1], 0x12);
}
