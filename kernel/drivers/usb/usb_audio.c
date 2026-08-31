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
#define AC_FEATURE_UNIT        0x06
#define AS_FORMAT_TYPE         0x02
#define FORMAT_TYPE_I          0x01

#define FU_CONTROL_MUTE        (1u << 0)
#define FU_CONTROL_VOLUME      (1u << 1)

// Class requests on an interface. SET_CUR/GET_* carry the control
// selector in the high byte of wValue and the channel in the low byte.
#define AUDIO_REQ_SET_CUR      0x01
#define AUDIO_REQ_GET_CUR      0x81
#define AUDIO_REQ_GET_MIN      0x82
#define AUDIO_REQ_GET_MAX      0x83
#define AUDIO_CS_MUTE          0x01
#define AUDIO_CS_VOLUME        0x02
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

    // BYTES PER SERVICE INTERVAL, which is NOT the endpoint's maximum:
    // wMaxPacketSize is a ceiling a device may set above the rate, and
    // sending that many every interval plays the stream fast. 48000/1000
    // divides exactly, so there is no fractional accumulator here -- a
    // rate that did not divide would need one.
    uint16_t pkt_bytes;

    uint8_t *pkt;            // `packets` x pkt_bytes, one frame
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

// Walks a configuration for a UAC1 playback stream at the ONE format
// this kernel carries. Returns 1 and fills `out` when it finds one, and
// fills `rep` with every AudioStreaming alternate it saw either way.
//
// The alternate settings are the point. An AudioStreaming interface
// always has an alt 0 with NO endpoints -- "idle, using no bandwidth" --
// and its real endpoint lives in alt 1 or later, which is why the
// interface walk in usb_enum.c (which records alt 0 only) cannot see
// it and this parse exists.
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
    uint8_t cand_channels = 0, cand_bits = 0;
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
            in_streaming = (cfg[o + 5] == AUDIO_CLASS &&
                            cfg[o + 6] == AUDIO_SUB_STREAMING);
            in_control   = (cfg[o + 5] == AUDIO_CLASS &&
                            cfg[o + 6] == AUDIO_SUB_CONTROL);
            if (in_control) out->ac_ifnum = cfg[o + 2];
            cand_channels = 0; cand_bits = 0; cand_rate = 0; format_ok = 0;
        } else if (in_control && btype == DESC_CS_INTERFACE && blen >= 5 &&
                   cfg[o + 2] == AC_HEADER && rep && !rep->uac_major) {
            // bcdADC, and the one field that says UAC1 from UAC2. The
            // two share subtype numbers and agree on almost nothing
            // else, so a refusal that cannot name the version is a
            // refusal nobody can act on.
            rep->uac_minor = cfg[o + 3];
            rep->uac_major = cfg[o + 4];
        } else if (btype == DESC_CS_INTERFACE && blen >= 4 &&
                   cfg[o + 2] == AC_FEATURE_UNIT && blen >= 8 && !out->feature_unit) {
            // bUnitID, then bmaControls[] of bControlSize bytes each.
            // Only the FIRST byte of each is read: mute and volume are
            // bits 0 and 1, and everything past them is a control this
            // driver does not touch.
            uint8_t size = cfg[o + 5];
            if (size >= 1) {
                out->feature_unit = cfg[o + 3];
                for (uint32_t i = 6; i + size <= blen - 1; i += size) {
                    if (cfg[o + i] & FU_CONTROL_VOLUME) out->has_volume = 1;
                    if (cfg[o + i] & FU_CONTROL_MUTE)   out->has_mute = 1;
                }
            }
        } else if (in_streaming && btype == DESC_CS_INTERFACE && blen >= 8 &&
                   cfg[o + 2] == AS_FORMAT_TYPE && cfg[o + 3] == FORMAT_TYPE_I) {
            cand_channels = cfg[o + 4];
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
            format_ok = rate_ok && cand_channels == SND_CHANNELS && cand_bits == 16;
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
            // Taken as soon as it is seen, so a device offering the same
            // format twice binds the first.
            if (format_ok && !(addr & 0x80) && mult == 1 && !out->ep) {
                out->ifnum    = (uint8_t)cur_if;
                out->alt      = (uint8_t)cur_alt;
                out->ep       = addr;
                out->mps      = mps;
                out->interval = cfg[o + 6];
            }
        }
        o += blen;
    }
    return out->ep != 0;
}

// --- the packet pump --------------------------------------------------

static int set_interface(uint8_t slot, uint8_t ifnum, uint8_t alt) {
    uint8_t setup[8] = { TYPE_OUT_STD_IF, REQ_SET_INTERFACE, alt, 0,
                         ifnum, 0, 0, 0 };
    return xhci_control(slot, setup, 0, 0, 0);
}


static void copy_one_packet(struct audio_dev *a, uint8_t slot) {
    uint8_t *dst = a->pkt + (uint32_t)slot * a->pkt_bytes;
    uint32_t n = a->pkt_bytes;
    uint32_t at = a->copy_pos;
    // The ring wraps mid-packet on most laps -- 192 divides neither the
    // ring nor a chunk -- so this is two copies, not one.
    uint32_t first = SND_RING_BYTES - at;
    if (first > n) first = n;
    k_memcpy(dst, a->ring + at, first);
    if (first < n) k_memcpy(dst + first, a->ring, n - first);
    a->copy_pos = (at + n) % SND_RING_BYTES;
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
    a->play_pos = (a->play_pos + a->pkt_bytes) % SND_RING_BYTES;
    uint32_t chunk = (a->play_pos / SND_CHUNK_BYTES) * SND_CHUNK_BYTES;
    if (chunk != a->reported) {
        a->reported = chunk;
        sound_period_done(chunk);
    }

    if (a->inflight) a->inflight--;
    if (!a->running) return;
    uint8_t slot = a->next_slot;
    a->next_slot = (uint8_t)((slot + 1) % a->packets);
    copy_one_packet(a, slot);
    if (xhci_isoch_post(a->slot, a->s.ep,
                        a->pkt_phys + (uint64_t)slot * a->pkt_bytes,
                        a->pkt_bytes, 1) == 0)
        a->inflight++;
}

static int audio_start(void) {
    struct audio_dev *a = &g_audio;
    if (!a->in_use || !a->ring) return -1;
    if (a->running) return 0;

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
                            a->pkt_phys + (uint64_t)i * a->pkt_bytes,
                            a->pkt_bytes, 1) < 0) {
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

static int audio_control(uint8_t req, uint8_t type, uint8_t cs, uint8_t channel,
                         uint8_t *buf, uint16_t len) {
    struct audio_dev *a = &g_audio;
    uint8_t setup[8];
    setup[0] = type;
    setup[1] = req;
    setup[2] = channel;              // wValue low: channel number
    setup[3] = cs;                   // wValue high: control selector
    setup[4] = a->s.ac_ifnum;        // wIndex low: the AudioControl interface
    setup[5] = a->s.feature_unit;    // wIndex high: the unit being addressed
    setup[6] = (uint8_t)(len & 0xFF);
    setup[7] = (uint8_t)(len >> 8);
    return xhci_control(a->slot, setup, buf, len, (type & 0x80) ? 1 : 0);
}

static int16_t read_db(uint8_t req, int16_t fallback) {
    uint8_t v[2] = {0, 0};
    if (audio_control(req, TYPE_IN_CLASS_IF, AUDIO_CS_VOLUME, 1, v, 2) < 2)
        return fallback;
    return (int16_t)((uint16_t)v[0] | ((uint16_t)v[1] << 8));
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
                      0, &mute, 1);
    }
    if (!a->has_volume || pct == 0) return;

    int32_t floor_db = (int32_t)a->vol_max - AUDIO_TAPER_DB * 256;
    if (floor_db < a->vol_min) floor_db = a->vol_min;
    int32_t value = floor_db + ((int32_t)a->vol_max - floor_db) * pct / 100;
    uint8_t v[2] = { (uint8_t)(value & 0xFF), (uint8_t)((value >> 8) & 0xFF) };
    // Channel 1 and 2 rather than 0: a feature unit commonly carries
    // volume per channel and only mute on the master, which is exactly
    // what QEMU's usb-audio reports.
    audio_control(AUDIO_REQ_SET_CUR, TYPE_OUT_CLASS_IF, AUDIO_CS_VOLUME, 1, v, 2);
    audio_control(AUDIO_REQ_SET_CUR, TYPE_OUT_CLASS_IF, AUDIO_CS_VOLUME, 2, v, 2);
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

    // The rate's share of ONE service interval, which is what actually
    // goes out per packet -- see audio_dev.pkt_bytes.
    uint32_t us = interval_us(info->speed, s.interval);
    uint32_t need = (uint32_t)SND_RATE * SND_FRAME_BYTES / 1000 * us / 1000;
    if (!need || s.mps < need) {
        klog_printf("usb-audio: slot %u: endpoint 0x%02x holds %u bytes and "
                    "%u Hz needs %u every %u us -- not bound\n",
                    info->slot, s.ep, s.mps, (unsigned)SND_RATE, need, us);
        return 0;
    }

    struct audio_dev *a = &g_audio;
    k_memset(a, 0, sizeof *a);
    a->slot = info->slot;
    a->s = s;
    a->pkt_bytes = (uint16_t)need;
    uint32_t want = AUDIO_INFLIGHT_US / us;
    uint32_t fits = 4096 / a->pkt_bytes;          // one frame holds them all
    if (want > fits) want = fits;
    if (want > AUDIO_PACKETS_MAX) want = AUDIO_PACKETS_MAX;
    if (want < 2) want = 2;
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

    if (s.has_volume) {
        a->vol_min = read_db(AUDIO_REQ_GET_MIN, 0);
        a->vol_max = read_db(AUDIO_REQ_GET_MAX, 0);
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
    klog_printf("usb: slot %u: bound as usb-audio, ep 0x%x, %u B/interval "
                "(endpoint holds %u), %u packets%s\n", info->slot, s.ep,
                a->pkt_bytes, s.mps, a->packets,
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
