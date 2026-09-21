#ifndef USB_AUDIO_H
#define USB_AUDIO_H

#include <stdint.h>
// The types and the parser are SHARED WITH RING 3 and live in api/;
// what is below is the ring-0 half -- binding a device and driving its
// endpoint, which only the kernel can do.
#include "usb_audio_parse.h"

struct usb_device_info;

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
