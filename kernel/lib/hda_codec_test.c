// The codec graph parser, driven by a codec made of a switch statement.
//
// A KTEST cannot assume an HD Audio controller is present, and the ones
// in hda.c skip without one. This file needs no hardware at all: the
// parser's transport is a callback (api/hda_codec.h), so a fake codec
// is a function -- which is the same reason hid_parse.c can be tested
// against a descriptor in a byte array.
//
// WHAT A BROKEN VERSION WOULD STILL PASS. A parser that returned the
// first pin it saw would satisfy "a route was found", so the checks
// name the SPECIFIC nids and the path's order. And the second half is
// the one that matters: a codec that LIES -- a cycle in its connection
// list, more widgets than the array holds, a list longer than the
// caller's buffer -- must be survived rather than trusted, because on a
// machine where the card is not what it claims those are the inputs.
#include "hda_codec.h"
#include "ktest.h"

// nids 2..5: a DAC, a mixer, a fixed speaker pin and a headphone jack.
//
//   pin 04 (speaker, fixed)  -> mixer 03 -> DAC 02
//   pin 05 (headphone, jack) -> mixer 03
#define T_DAC 0x02
#define T_MIX 0x03
#define T_SPK 0x04
#define T_HP  0x05

struct fake {
    int cycle;      // the mixer also lists the pin that feeds it
    int overwide;   // the AFG claims more widgets than HDA_MAX_WIDGET
    int overlong;   // the mixer claims a longer connection list than fits
    int calls;
};

// Default configuration: bits 31:30 connectivity, 23:20 device type.
#define DEFCFG(conn, dev) (((uint32_t)(conn) << 30) | ((uint32_t)(dev) << 20))

static int fake_cmd(void *ctx, uint8_t nid, uint32_t verb20, uint32_t *out) {
    struct fake *f = ctx;
    uint32_t verb = verb20 >> 8, payload = verb20 & 0xFF;
    f->calls++;
    if (out) *out = 0;   // NULL for a SET verb -- hda_codec.h

    if (verb == VERB_GET_PARAM) {
        switch (payload) {
        case PARAM_VENDOR_ID:
            *out = 0x10ec0888;
            return 0;
        case PARAM_NODE_COUNT:
            if (nid == 0) { *out = (1u << 16) | 1; return 0; }   // the AFG at nid 1
            if (nid == 1) {
                *out = (2u << 16) | (f->overwide ? 250u : 4u);
                return 0;
            }
            return 0;
        case PARAM_FUNC_TYPE:
            *out = (nid == 1) ? FUNC_AUDIO : 0;
            return 0;
        case PARAM_WIDGET_CAPS:
            switch (nid) {
            case T_DAC: *out = (WT_AUD_OUT << 20) | WCAP_OUT_AMP | WCAP_POWER; return 0;
            case T_MIX: *out = (WT_MIXER   << 20) | WCAP_CONN_LIST | WCAP_IN_AMP; return 0;
            case T_SPK:
            case T_HP:  *out = (WT_PIN     << 20) | WCAP_CONN_LIST | WCAP_OUT_AMP; return 0;
            default:    *out = (WT_AUD_IN  << 20); return 0;  // the padding widgets
            }
        case PARAM_PIN_CAPS:
            *out = PINCAP_OUTPUT | (nid == T_HP ? PINCAP_PRESENCE | PINCAP_HP_DRIVE : 0);
            return 0;
        case PARAM_CONN_LIST_LEN:
            if (nid == T_MIX) return (*out = f->overlong ? 100 : (f->cycle ? 2 : 1)), 0;
            if (nid == T_SPK || nid == T_HP) return (*out = 1), 0;
            return 0;
        case PARAM_AMP_OUT_CAPS:
            // 74 steps of 0.75 dB, 0 dB at step 57, mute-capable -- the
            // shape a real laptop amplifier reports.
            *out = 0x80000000u | (2u << 16) | (74u << 8) | 57u;
            return 0;
        default:
            return 0;
        }
    }

    if (verb == VERB_GET_CONN_LIST) {
        // Four 8-bit entries per response, starting at `payload`.
        if (nid == T_SPK || nid == T_HP) { *out = T_MIX; return 0; }
        if (nid == T_MIX) {
            if (f->overlong) { *out = 0x05040302; return 0; } // the same four, forever
            *out = f->cycle ? (uint32_t)((T_SPK << 8) | T_DAC) : T_DAC;
            return 0;
        }
        return 0;
    }

    if (verb == VERB_GET_CONFIG_DEF) {
        if (nid == T_SPK) { *out = DEFCFG(2, DEV_SPEAKER); return 0; } // fixed
        if (nid == T_HP)  { *out = DEFCFG(0, DEV_HP_OUT);  return 0; } // a jack
        return 0;
    }
    return 0;   // every SET verb is accepted and ignored
}

// FILE SCOPE, not locals: `struct hda_codec` is ~1.6 KB of widgets and
// a kernel stack is 16 KiB with a guard page. KTESTs never overlap.
static struct hda_codec c;
static struct fake f;

static void fake_reset(void) {
    for (unsigned i = 0; i < sizeof c; i++) ((unsigned char *)&c)[i] = 0;
    for (unsigned i = 0; i < sizeof f; i++) ((unsigned char *)&f)[i] = 0;
    c.cmd = fake_cmd;
    c.ctx = &f;
}

KTEST("hda_codec", "the graph a codec describes is the graph that is read") {
    fake_reset();
    KTEST_ASSERT_EQ(hda_codec_enumerate(&c), 0);
    KTEST_ASSERT_EQ(c.afg, 1);
    KTEST_ASSERT_EQ(c.nwidgets, 4);
    KTEST_ASSERT_EQ(hda_codec_widget(&c, T_DAC)->type, WT_AUD_OUT);
    KTEST_ASSERT_EQ(hda_codec_widget(&c, T_SPK)->type, WT_PIN);
    KTEST_ASSERT(hda_codec_widget(&c, 0x7F) == 0);   // a nid nobody declared

    uint8_t conns[HDA_CONN_MAX];
    KTEST_ASSERT_EQ(hda_codec_conn_list(&c, T_SPK, conns, HDA_CONN_MAX), 1);
    KTEST_ASSERT_EQ(conns[0], T_MIX);
    // A DAC has no connection list capability, so it has no list --
    // asked anyway, because a caller walking every widget will.
    KTEST_ASSERT_EQ(hda_codec_conn_list(&c, T_DAC, conns, HDA_CONN_MAX), 0);
}

KTEST("hda_codec", "the speaker outranks the jack and both reach the DAC") {
    fake_reset();
    KTEST_ASSERT_EQ(hda_codec_enumerate(&c), 0);
    KTEST_ASSERT_EQ(hda_codec_pick_outputs(&c), 0);

    // THE ORDER IS THE ASSERTION. A fixed speaker outranks a headphone
    // jack, and the path runs pin first, DAC last -- a parser that
    // returned them the other way round would still "find a route".
    KTEST_ASSERT_EQ(c.have_spk, 1);
    KTEST_ASSERT_EQ(c.spk.pin, T_SPK);
    KTEST_ASSERT_EQ(c.spk.dac, T_DAC);
    KTEST_ASSERT_EQ(c.spk.len, 3);
    KTEST_ASSERT_EQ(c.spk.path[0], T_SPK);
    KTEST_ASSERT_EQ(c.spk.path[1], T_MIX);
    KTEST_ASSERT_EQ(c.spk.path[2], T_DAC);

    KTEST_ASSERT_EQ(c.have_hp, 1);
    KTEST_ASSERT_EQ(c.hp.pin, T_HP);
    KTEST_ASSERT_EQ(c.hp.dac, T_DAC);

    // The knob: the first amplifier back from the DAC, which here is
    // the DAC's own. 74 steps of 0.75 dB is 3 quarter-dB per step.
    hda_codec_pick_volume(&c, &c.spk);
    KTEST_ASSERT_EQ(c.spk.vol_nid, T_DAC);
    KTEST_ASSERT_EQ(c.spk.vol_steps, 74);
    KTEST_ASSERT_EQ(c.spk.vol_offset, 57);
    KTEST_ASSERT_EQ(c.spk.vol_step_qdb, 3);
    KTEST_ASSERT_EQ(c.spk.vol_mute, 1);
}

KTEST("hda_codec", "A CODEC THAT LIES CANNOT WALK THE PARSER OFF AN ARRAY") {
    // A cycle: the mixer lists the pin that feeds it, so a depth-first
    // walk with no visited set recurses until the stack is gone -- and
    // the route must still be found, because the DAC is reachable.
    fake_reset();
    f.cycle = 1;
    KTEST_ASSERT_EQ(hda_codec_enumerate(&c), 0);
    KTEST_ASSERT_EQ(hda_codec_pick_outputs(&c), 0);
    KTEST_ASSERT_EQ(c.spk.dac, T_DAC);

    // More widgets than the array holds: clipped, not overrun, and the
    // graph is still usable.
    fake_reset();
    f.overwide = 1;
    KTEST_ASSERT_EQ(hda_codec_enumerate(&c), 0);
    KTEST_ASSERT_EQ(c.nwidgets, HDA_MAX_WIDGET);

    // A connection list longer than the caller's buffer, answered with
    // entries forever. `cap` is the bound, and a cap of 0 or 1 must be
    // honoured as strictly as the full one.
    fake_reset();
    f.overlong = 1;
    KTEST_ASSERT_EQ(hda_codec_enumerate(&c), 0);
    uint8_t small[8];
    KTEST_ASSERT_EQ(hda_codec_conn_list(&c, T_MIX, small, 4), 4);
    KTEST_ASSERT_EQ(hda_codec_conn_list(&c, T_MIX, small, 1), 1);
    KTEST_ASSERT_EQ(hda_codec_conn_list(&c, T_MIX, small, 0), 0);
    uint8_t full[HDA_CONN_MAX];
    KTEST_ASSERT_EQ(hda_codec_conn_list(&c, T_MIX, full, HDA_CONN_MAX), HDA_CONN_MAX);
}

KTEST("hda_codec", "a codec that answers nothing is not a graph") {
    // The transport failing is not the same as a codec with no audio
    // function group, and neither may be reported as a route.
    fake_reset();
    c.cmd = 0;
    KTEST_ASSERT_EQ(hda_codec_enumerate(&c), -1);
    KTEST_ASSERT_EQ(c.afg, 0);
    KTEST_ASSERT_EQ(hda_codec_pick_outputs(&c), -1);
    KTEST_ASSERT_EQ(c.have_spk, 0);
    KTEST_ASSERT_EQ(hda_codec_param(&c, 0, PARAM_VENDOR_ID), 0u);
}
