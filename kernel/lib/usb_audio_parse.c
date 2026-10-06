// THE USB AUDIO DESCRIPTOR WALK, compiled TWICE -- once into the
// kernel for sound_usb.c, once into /lib/snd/usbaudio.so for the ring-3
// driver. The geom.c/klineedit.c/hda_codec.c rule: one implementation
// of a parser, not two that drift.
//
// **IT PARSES UNTRUSTED INPUT.** Every field here comes off a device
// that is free to lie -- lengths, counts, entity ids -- which is the
// whole argument for the walk running where a mistake kills a process
// rather than the machine. Every read is bounded by `total` and every
// descriptor by its own bLength, and a descriptor that would step
// backwards or past the end ends the walk.
//
// IT ALLOCATES NOTHING and calls nothing but k_memset, which is what
// lets one implementation serve ring 0, ring 3 and a test.
#include "usb_audio_parse.h"
#include "sound_abi.h"   // SND_RATE/SND_CHANNELS -- the format it looks for
#include "string.h"

static uint32_t le24(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

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
// its rates in the format descriptor, and the DEFAULT alternate must
// list 48 kHz (SND_RATE, what a stream opens at); a UAC2 device states
// no rate at all, so the check that matters for one is the SET_CUR in
// set_clock_rate() at bind. Every playable alternate -- 48 kHz or not --
// goes in `formats`, which is what a format change chooses from.
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
    uint32_t cand_rate = 0, cand_rates = 0;
    uint8_t cand_rate_ctl = 0;
    int format_ok = 0;
    uint8_t fmt_if[USB_AUDIO_MAX_FORMATS];

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
            cand_rates = 0; cand_rate_ctl = 0;
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
                if (freq_type == 0 && blen >= 14) {
                    // A continuous range: tLowerSamFreq, tUpperSamFreq.
                    uint32_t lo = le24(&cfg[o + 8]), hi = le24(&cfg[o + 11]);
                    for (int i = 0; i < SND_RATE_COUNT; i++)
                        if (snd_rate_hz(i) >= lo && snd_rate_hz(i) <= hi) cand_rates |= 1u << i;
                    cand_rate = (lo <= SND_RATE && hi >= SND_RATE) ? SND_RATE : lo;
                    cand_rate_ctl = 1;
                } else {
                    for (uint32_t i = 0; i < freq_type; i++) {
                        uint32_t at = o + 8 + i * 3;
                        if (at + 3 > o + blen) break;
                        if (!cand_rate) cand_rate = le24(&cfg[at]);
                        cand_rates |= snd_rate_mask(le24(&cfg[at]));
                    }
                    if (cand_rates & SND_RATE_48000) cand_rate = SND_RATE;
                    cand_rate_ctl = freq_type > 1;
                }
                format_ok = cand_rates && cand_channels == SND_CHANNELS;
            }
            // The sample widths the packet copy can write: 32-bit is a
            // memcpy from the s32 ring, 16 and 24 are narrowed per sample.
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
            int playable = format_ok && !(addr & 0x80) && mult == 1;
            if (playable && out->nformats < USB_AUDIO_MAX_FORMATS) {
                struct usb_audio_format *f = &out->formats[out->nformats];
                fmt_if[out->nformats++] = (uint8_t)cur_if;
                f->alt = (uint8_t)cur_alt;
                f->ep = addr;
                f->interval = cfg[o + 6];
                f->subslot = cand_subslot;
                f->bits = cand_bits;
                f->mps = mps;
                f->rates = out->uac2 ? 0 : cand_rates;
                f->rate_ctl = out->uac2 ? 0 : cand_rate_ctl;
            }
            // THE DEFAULT: the deepest that plays 48 kHz, so a 24-bit
            // file reaches a 24/32-bit DAC whole; on a tie the first seen.
            if (playable && (out->uac2 || (cand_rates & SND_RATE_48000)) &&
                (!out->ep || cand_bits > out->bits)) {
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

    // Only the default's interface and endpoint: a format change swaps
    // the alternate under one configured endpoint, never the endpoint.
    uint8_t keep = 0;
    for (uint8_t i = 0; i < out->nformats; i++)
        if (out->ep && fmt_if[i] == out->ifnum && out->formats[i].ep == out->ep)
            out->formats[keep++] = out->formats[i];
    out->nformats = keep;

    find_feature_unit(cfg, total, out);
    find_clock(cfg, total, out);
    return out->ep != 0;
}

uint32_t usb_audio_range_rates(const uint8_t *buf, uint32_t len) {
    if (!buf || len < 2) return 0;
    uint32_t n = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8), mask = 0;
    for (uint32_t r = 0; r < n && 2 + (r + 1) * 12 <= len; r++) {
        const uint8_t *p = buf + 2 + r * 12;
        uint32_t lo  = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        uint32_t hi  = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
        uint32_t res = (uint32_t)p[8] | ((uint32_t)p[9] << 8) | ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24);
        for (int i = 0; i < SND_RATE_COUNT; i++) {
            uint32_t hz = snd_rate_hz(i);
            if (hz >= lo && hz <= hi && (!res || (hz - lo) % res == 0))
                mask |= 1u << i;
        }
    }
    return mask;
}

uint32_t usb_audio_pace_max(uint32_t rate, uint32_t us) {
    return (uint32_t)(((uint64_t)rate * us + 999999u) / 1000000u);
}

uint32_t usb_audio_pace_next(struct usb_audio_pace *p) {
    uint64_t a = (uint64_t)p->acc + (uint64_t)p->rate * p->us;
    uint32_t n = (uint32_t)(a / 1000000u);
    p->acc = (uint32_t)(a - (uint64_t)n * 1000000u);
    return n;
}

int usb_audio_pick_format(const struct usb_audio_stream *s, uint32_t clock_rates,
                          uint32_t rate, uint32_t bits, uint32_t us) {
    uint32_t want = snd_rate_mask(rate);
    uint32_t frames = usb_audio_pace_max(rate, us);
    int best = -1;
    for (int i = 0; i < s->nformats; i++) {
        const struct usb_audio_format *f = &s->formats[i];
        uint32_t rates = s->uac2 ? clock_rates : f->rates;
        if (!want || !(rates & want)) continue;
        if (frames * SND_CHANNELS * f->subslot > f->mps) continue;
        if (bits && f->bits != bits) continue;
        if (best < 0 || f->bits > s->formats[best].bits) best = i;
    }
    return best;
}
