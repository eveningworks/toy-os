// HD Audio: the register map, the codec's verbs, and the parser that
// walks what a codec says about itself.
//
// COMPILED TWICE, like geom.c and klineedit.c -- kernel/lib/hda_codec.c
// is built into the kernel for kernel/drivers/sound/hda.c and into
// /bin/lscodec for ring 3 (docs/umdf-design.md stage 3). So it must name
// nothing kernel-only: the transport is a CALLBACK, which is how one
// parser serves a CORB/RIRB in ring 0, another in ring 3, and a
// synthetic codec in a KTEST.
//
// THE SPLIT IS CODEC VERSUS CONTROLLER. Everything here talks to the
// CODEC -- reading what it says about itself, and configuring the route
// it should play through -- over one callback. What stays with the
// controller (kernel/drivers/sound/hda.c, or /bin/lscodec) is the
// CORB/RIRB, the stream descriptor, the buffer list and the interrupts.
//
// It was READ VERSUS WRITE until stage 5, on the argument that the
// write half is what makes sound and so should stay in ring 0. That
// line did not survive a ring-3 driver needing to route its own
// output: routing is verbs to the codec, indistinguishable from the
// reads beside them, and keeping it away meant duplicating the walk.
// The reads are still the interesting half -- they are UNTRUSTED
// INPUT, which is the argument that moved any of this.
//
// The controller registers are here rather than in hda.c because BOTH
// transports need them: a ring-3 driver programming its own CORB/RIRB
// would otherwise keep a second copy of the same offsets.
#ifndef API_HDA_CODEC_H
#define API_HDA_CODEC_H

#include <stdint.h>

// --- controller registers, for whoever drives the rings ---------------
#define HDA_GCAP      0x00 // 16: OSS 15:12, ISS 11:8, BSS 7:4, 64OK bit 0
#define HDA_VMIN      0x02 // 8
#define HDA_VMAJ      0x03 // 8
#define HDA_GCTL      0x08 // 32
#define HDA_WAKEEN    0x0C // 16
#define HDA_STATESTS  0x0E // 16, RW1C: a bit per codec that answered reset
#define HDA_INTCTL    0x20 // 32
#define HDA_INTSTS    0x24 // 32
#define HDA_CORBLBASE 0x40
#define HDA_CORBUBASE 0x44
#define HDA_CORBWP    0x48 // 16
#define HDA_CORBRP    0x4A // 16, bit 15 = reset
#define HDA_CORBCTL   0x4C // 8
#define HDA_CORBSIZE  0x4E // 8: 1:0 selected, 7:4 supported
#define HDA_RIRBLBASE 0x50
#define HDA_RIRBUBASE 0x54
#define HDA_RIRBWP    0x58 // 16, bit 15 = reset (write-only)
#define HDA_RINTCNT   0x5A // 16
#define HDA_RIRBCTL   0x5C // 8
#define HDA_RIRBSTS   0x5D // 8, RW1C
#define HDA_RIRBSIZE  0x5E // 8
#define HDA_SD_BASE   0x80 // stream descriptors, 0x20 apart, inputs first

#define GCTL_CRST   0x001
#define GCTL_UNSOL  0x100
#define INTCTL_GIE  0x80000000u
#define INTCTL_CIE  0x40000000u
#define INTSTS_GIS  0x80000000u
#define INTSTS_CIS  0x40000000u
#define CORBCTL_RUN 0x02
#define RIRBCTL_DMAEN 0x02
#define RIRBCTL_RINTCTL 0x01
#define RIRBSTS_ACK 0x05 // RINTFL | OIS

// Stream descriptor, relative to its base.
#define SD_CTL   0x00 // 24-bit; STS is the fourth byte of the same dword
#define SD_STS   0x03 // 8, RW1C
#define SD_LPIB  0x04 // 32: bytes played of the current lap
#define SD_CBL   0x08 // 32: cyclic buffer length
#define SD_LVI   0x0C // 16
#define SD_FMT   0x12 // 16
#define SD_BDPL  0x18
#define SD_BDPU  0x1C

#define SD_CTL_SRST 0x01
#define SD_CTL_RUN  0x02
#define SD_CTL_IOCE 0x04
#define SD_CTL_FEIE 0x08
#define SD_CTL_DEIE 0x10
#define SD_CTL_STREAM_SHIFT 20
#define SD_STS_ACK  0x1C // BCIS | FIFOE | DESE
#define SD_STS_BCIS 0x04

// 48 kHz base, 16-bit, 2 channels -- SND_RATE/SND_CHANNELS as the codec
// spells them.
#define HDA_FMT_48K_S16_STEREO 0x0011
#define HDA_STREAM_TAG 1

// The CORB is 256 4-byte verbs and the RIRB 256 8-byte responses, so a
// transport's rings fit one 4 KiB frame with the RIRB at +1024. Both
// rings put them there; hda.c's stream descriptor list follows at +3072.
#define HDA_CORB_OFF   0
#define HDA_RIRB_OFF   1024
#define HDA_RING_BYTES 4096

// --- codec verbs -------------------------------------------------------
// A 20-bit verb+payload: 12-bit verb with an 8-bit payload, or 4-bit
// verb with 16 bits.
#define V12(verb, pl) (((uint32_t)(verb) << 8) | ((pl) & 0xFF))
#define V4(verb, pl)  (((uint32_t)(verb) << 16) | ((pl) & 0xFFFF))

#define VERB_GET_PARAM        0xF00
#define VERB_GET_CONN_LIST    0xF02
#define VERB_SET_CONN_SEL     0x701
#define VERB_GET_CONV         0xF06
#define VERB_SET_CONV         0x706
#define VERB_SET_POWER        0x705
#define VERB_SET_PIN_CTL      0x707
#define VERB_GET_PIN_CTL      0xF07
#define VERB_SET_UNSOL        0x708
#define VERB_GET_PIN_SENSE    0xF09
#define VERB_SET_EAPD         0x70C
#define VERB_GET_CONFIG_DEF   0xF1C
#define VERB_SET_AMP          0x3 // 4-bit
#define VERB_GET_AMP          0xB // 4-bit
#define VERB_SET_FORMAT       0x2 // 4-bit

#define PARAM_VENDOR_ID     0x00
#define PARAM_NODE_COUNT    0x04
#define PARAM_FUNC_TYPE     0x05
#define PARAM_WIDGET_CAPS   0x09
#define PARAM_PIN_CAPS      0x0C
#define PARAM_AMP_IN_CAPS   0x0D
#define PARAM_CONN_LIST_LEN 0x0E
#define PARAM_AMP_OUT_CAPS  0x12

#define FUNC_AUDIO 0x01

// Widget capabilities (PARAM_WIDGET_CAPS).
#define WCAP_TYPE(c)     (((c) >> 20) & 0xF)
#define WCAP_IN_AMP      0x002
#define WCAP_OUT_AMP     0x004
#define WCAP_AMP_OVRD    0x008
#define WCAP_UNSOL       0x080
#define WCAP_CONN_LIST   0x100
#define WCAP_DIGITAL     0x200
#define WCAP_POWER       0x400
#define WT_AUD_OUT  0x0
#define WT_AUD_IN   0x1
#define WT_MIXER    0x2
#define WT_SELECTOR 0x3
#define WT_PIN      0x4

// Pin capabilities and the default configuration.
#define PINCAP_PRESENCE 0x00004
#define PINCAP_HP_DRIVE 0x00008
#define PINCAP_OUTPUT   0x00010
#define PINCAP_EAPD     0x10000
#define DEFCFG_CONN(c)  (((c) >> 30) & 0x3) // 0 jack, 1 none, 2 fixed, 3 both
#define DEFCFG_DEV(c)   (((c) >> 20) & 0xF)
#define DEV_LINE_OUT 0x0
#define DEV_SPEAKER  0x1
#define DEV_HP_OUT   0x2

#define PINCTL_HP_EN  0x80
#define PINCTL_OUT_EN 0x40

// SET_AMP payload bits.
#define AMP_OUT   0x8000
#define AMP_IN    0x4000
#define AMP_LEFT  0x2000
#define AMP_RIGHT 0x1000
#define AMP_MUTE  0x0080
#define AMP_IDX(i) (((i) & 0xF) << 8)

// --- the graph ---------------------------------------------------------

#define HDA_MAX_WIDGET 96
#define HDA_PATH_MAX   6
#define HDA_CONN_MAX   32

struct hda_widget {
    uint8_t  nid;
    uint8_t  type;
    uint32_t caps;
    uint32_t pincap; // pins only
    uint32_t defcfg; // pins only
};

// One analog output: a pin, and the route from it back to a DAC.
struct hda_out {
    uint8_t pin, dac;
    uint8_t path[HDA_PATH_MAX]; // pin first, DAC last
    int     len;
    // The volume knob on this route: the first widget from the DAC end
    // with a stepped output amplifier. `offset` is the 0 dB step.
    uint8_t vol_nid;
    uint8_t vol_steps, vol_offset;
    uint8_t vol_step_qdb; // dB per step, in quarter-dB (caps field + 1)
    int     vol_mute;     // the amp can mute
};

// The transport. Returns 0 with *out filled, or -1 if the codec did not
// answer -- and the parser treats a timeout as a zero parameter rather
// than giving up, because one unanswered widget must not lose the graph.
struct hda_codec {
    int (*cmd)(void *ctx, uint8_t nid, uint32_t verb20, uint32_t *out);
    void *ctx;

    uint8_t  afg;      // the audio function group's nid, 0 until enumerated
    uint32_t vendor;   // PARAM_VENDOR_ID at nid 0
    struct hda_widget widgets[HDA_MAX_WIDGET];
    int nwidgets;

    struct hda_out spk, hp;
    int have_spk, have_hp;
};

uint32_t hda_codec_param(struct hda_codec *c, uint8_t nid, uint8_t param);
struct hda_widget *hda_codec_widget(struct hda_codec *c, uint8_t nid);

// The connection list of `nid`, expanded (a range entry stands for
// every node between the previous entry and itself). Returns how many
// landed in `out`, never more than `cap`.
int hda_codec_conn_list(struct hda_codec *c, uint8_t nid, uint8_t *out, int cap);

// Find the audio function group, power it up, and read every widget it
// declares. 0 on success, -1 when there is no AFG or no widget under it.
int hda_codec_enumerate(struct hda_codec *c);

uint32_t hda_codec_amp_caps(struct hda_codec *c, const struct hda_widget *w, int out);

// Depth-first from a pin toward an analog DAC, through mixers and
// selectors only. Fills `path` pin-first; returns its length or 0.
// `visited` is a 256-bit set the caller zeroes -- uint32_t[8].
int hda_codec_find_path(struct hda_codec *c, uint8_t nid, uint8_t *path,
                        int depth, uint32_t *visited);

// How good an output pin this is: 0 for none, higher is better.
int hda_codec_out_rank(const struct hda_widget *w);

// Choose the speaker (or line-out) pin and, separately, a headphone
// jack, and walk each back to a DAC. 0 on success, -1 when the codec
// has no analog output at all -- which is the answer on a display-audio
// codec, and not a failure.
int hda_codec_pick_outputs(struct hda_codec *c);

// The volume knob on a routed output: the first widget from the DAC end
// whose output amplifier has steps or can mute. Fills `o`'s vol_*
// fields, leaving vol_nid 0 when the route has no such amplifier.
void hda_codec_pick_volume(struct hda_codec *c, struct hda_out *o);

// Make `o` the live output: power every widget on its path, put the
// DAC on `stream_tag` at `fmt`, unmute each amplifier at 0 dB and
// enable the pin. The two arguments are the CONTROLLER's business --
// which stream this codec should listen to, and in what format -- so
// they are passed rather than known here.
void hda_codec_route_output(struct hda_codec *c, struct hda_out *o,
                            uint16_t fmt, uint8_t stream_tag);

#endif // API_HDA_CODEC_H
