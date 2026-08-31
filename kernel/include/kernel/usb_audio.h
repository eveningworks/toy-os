#ifndef USB_AUDIO_H
#define USB_AUDIO_H

#include <stdint.h>

// USB Audio Class 1.0 and 2.0 playback -- see
// kernel/drivers/sound/sound_usb.c for what it binds and what it
// deliberately refuses.

struct usb_device_info;

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

// Binds an enumerated audio device: SET_INTERFACE to the streaming
// alternate, configure the isochronous endpoint, register a
// `sound_device`. `cfg`/`total` is the configuration descriptor
// enumeration already read -- passed in because the class-specific
// descriptors (the format, the feature unit) live between the standard
// ones and usb_enum.c's interface walk does not keep them. Returns 1
// when it took the device.
int usb_audio_bind(struct usb_device_info *info, const uint8_t *cfg,
                   uint32_t total);

// Releases the device on `slot`, if it is the bound one. The sound core
// publishes `device_gone` to whoever held the stream.
void usb_audio_unbind(uint8_t slot);

// Is a USB audio device bound? The KTESTs' skip condition.
int usb_audio_bound(void);

#endif
