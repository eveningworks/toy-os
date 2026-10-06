#ifndef API_USB_AUDIO_PARSE_H
#define API_USB_AUDIO_PARSE_H

#include <stdint.h>

// THE USB AUDIO DESCRIPTOR WALK's types and entry point, in `api/`
// because BOTH RINGS parse them: kernel/drivers/sound/sound_usb.c and
// /lib/snd/usbaudio.so share kernel/lib/usb_audio_parse.c, compiled
// twice -- the geom.c/hda_codec.c rule.
//
// What a driver DOES with a parsed stream -- binding it, driving the
// endpoint -- is not here; kernel/usb_audio.h has the ring-0 half.

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

// A clock selector's input pins. Four is what the one device this was
// written against has two of; a selector with more is used up to here
// and its later pins are unreachable, which costs a clock nobody
// selected rather than the stream.
#define USB_AUDIO_MAX_CLOCK_PINS 4

// One playback stream, as the configuration descriptor describes it.
// `alt` is the load-bearing field: an AudioStreaming interface's alt 0
// carries NO endpoint by design (it is the "idle, no bandwidth"
// setting), so the endpoint below only exists after a SET_INTERFACE to
// this alternate.
// ONE ALTERNATE SETTING THIS STACK CAN PLAY: stereo PCM, a subslot the
// packet copy writes, an isochronous OUT endpoint of one transaction.
// A card's depths are its formats' `bits`; switching depth is switching
// alternate setting, at start.
#define USB_AUDIO_MAX_FORMATS 4
struct usb_audio_format {
    uint8_t  alt, ep, interval, subslot, bits;
    uint16_t mps;
    // SND_RATE_* the alternate lists -- UAC1 only. A UAC2 alternate
    // states none: its rates are the clock's (usb_audio_range_rates()).
    uint32_t rates;
    // UAC1: it lists more than one rate, so the endpoint's sampling
    // frequency control has to be SET to pick one.
    uint8_t  rate_ctl;
};

struct usb_audio_stream {
    uint8_t  ifnum;
    uint8_t  alt;
    uint8_t  ep;           // bEndpointAddress -- isochronous OUT
    uint16_t mps;          // bytes per service interval, not a ceiling
    uint8_t  interval;     // bInterval
    // A feature-unit request is addressed to the AUDIO CONTROL
    // interface, never to the streaming one -- wIndex is
    // (unit << 8) | ac_ifnum. Getting that wrong is not a refusal you
    // can see: the device simply fails the transfer.
    uint8_t  ac_ifnum;
    uint8_t  feature_unit; // the AudioControl unit volume is set on, 0 if none
    uint8_t  has_volume;
    uint8_t  has_mute;

    // UAC2. The version changes the LAYOUT of almost every descriptor
    // above and the ENCODING of every request below -- UAC1 puts the
    // direction in the request code (SET_CUR 0x01, GET_CUR 0x81) and
    // UAC2 puts it in bmRequestType with one CUR code. A driver that
    // gets this wrong does not fail visibly; the device stalls.
    uint8_t  uac2;
    uint8_t  subslot;      // BYTES per sample on the wire: 2, 3 or 4
    uint8_t  bits;         // bBitResolution, <= subslot * 8
    uint8_t  terminal_link; // the AS interface's bTerminalLink

    // THE RATE IS NOT IN A UAC2 DESCRIPTOR. It lives in a Clock Source
    // entity and is SET by a class request, so binding one of these
    // devices is the first thing here that has to write to a device
    // rather than read it. `clock_id` is the entity the streaming
    // interface's input terminal names -- which may be a SELECTOR, in
    // which case its current pin is asked for and `clock_pins` maps
    // that 1-based answer back to a source.
    uint8_t  clock_id;
    uint8_t  clock_is_selector;
    uint8_t  clock_pin_count;
    uint8_t  clock_pins[USB_AUDIO_MAX_CLOCK_PINS];

    // Every playable alternate on the chosen interface and endpoint, in
    // descriptor order; the fields above are the default among them.
    uint8_t  nformats;
    struct usb_audio_format formats[USB_AUDIO_MAX_FORMATS];
};

// One AudioStreaming alternate setting the walk saw, whether or not it
// was usable. This is the REFUSAL's evidence: "no 48 kHz stereo s16
// stream" says nothing about what the device does offer, and on a
// machine that is not here the log line is all there is.
struct usb_audio_alt {
    uint8_t  ifnum, alt;
    uint8_t  channels;   // 0 when no Type I format descriptor preceded it
    uint8_t  bits;
    uint32_t rate;       // 0 = stated elsewhere (a UAC2 clock source)
    uint8_t  ep;         // bEndpointAddress, so 0x81 is an IN endpoint
    uint8_t  sync;       // bmAttributes' sync type: 0 none .. 3 synchronous
    uint8_t  interval;   // bInterval, in the endpoint's own units
    uint8_t  mult;       // transactions per interval; >1 is high-bandwidth
    uint16_t mps;        // bytes per transaction (wMaxPacketSize's low 11)
};

#define USB_AUDIO_MAX_ALTS 8

struct usb_audio_report {
    uint8_t uac_major;   // bcdADC's high byte; 0 when there was no AC header
    uint8_t uac_minor;
    uint8_t alt_count;   // rows filled, capped at USB_AUDIO_MAX_ALTS
    uint8_t alts_seen;   // rows the device actually had, uncapped
    struct usb_audio_alt alts[USB_AUDIO_MAX_ALTS];
};

// Finds a stream at abi/sound_abi.h's fixed format (48 kHz stereo s16)
// in a configuration descriptor. Returns 1 and fills `out`, or 0 --
// which is also the answer for a malformed descriptor, refused rather
// than walked past. Exported for the KTESTs, which is the only way this
// is checked on a machine with no USB audio attached.
//
// `rep` is optional and is filled EITHER WAY: a bind that succeeds and
// one that refuses both walked the same descriptors, and only the
// refusal has anything to explain.
int usb_audio_parse(const uint8_t *cfg, uint32_t total,
                    struct usb_audio_stream *out,
                    struct usb_audio_report *rep);

// A UAC2 GET RANGE answer for a clock's sampling frequency --
// wNumSubRanges, then (dMIN, dMAX, dRES) per subrange, little-endian --
// as SND_RATE_* bits: every listed rate some subrange reaches. A short
// or malformed answer yields what it fully holds.
uint32_t usb_audio_range_rates(const uint8_t *buf, uint32_t len);

// Which format plays `rate` at `bits` (0: the deepest that can), given
// the clock's rates on UAC2, and a service interval of `us` -- the
// busiest packet must fit the alternate's wMaxPacketSize. -1 for none.
int usb_audio_pick_format(const struct usb_audio_stream *s, uint32_t clock_rates,
                          uint32_t rate, uint32_t bits, uint32_t us);

// THE FRAMES IN EACH PACKET. 44.1 kHz is 44.1 frames a millisecond, so
// packets carry 44 or 45 and an accumulator keeps the total exact --
// what Linux's snd-usb-audio and every UAC host do. Start with zero.
struct usb_audio_pace {
    uint32_t rate, us;
    uint32_t acc;          // millionths of a frame carried over
};
uint32_t usb_audio_pace_next(struct usb_audio_pace *p);
// The most frames any one packet can carry at that rate and interval.
uint32_t usb_audio_pace_max(uint32_t rate, uint32_t us);


#endif // API_USB_AUDIO_PARSE_H
