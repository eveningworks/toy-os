#ifndef VIRTIO_GPU_H
#define VIRTIO_GPU_H

#include <stdint.h>
#include "display.h"

// The virtio GPU device -- the DEVICE half. See
// kernel/drivers/virtio/virtio_gpu.c for the protocol and
// kernel/drivers/display/display_virtio.c for the display_driver that
// sits on top of it.
//
// THE SPLIT IS THE SAME ONE virtio-blk ALREADY MAKES. virtio_blk.c
// speaks the device's request format and block_virtio.c adapts it to
// the `block_device` registry; here virtio_gpu.c speaks the command
// protocol and display_virtio.c adapts it to `display_driver`. Two
// axes, and neither knows about the other: what a device SITS ON (the
// virtio transport) is not what it PLUGS INTO (the class registry).
//
// This is a 2D driver. There is no virgl, no 3D context, no capset, no
// EDID and no multi-scanout -- scanout 0 is the display, as it is on
// every configuration this OS boots.

// Claims the device, sets up the control and cursor queues and reads
// the host's preferred display size. 1 on success. Silent and
// allocation-free when there is no virtio-gpu, which is the ordinary
// case on a default boot.
//
// Must run before display_probe() and after pmm_init(): it needs frames
// for its virtqueues and for the framebuffer it is about to own. That
// ordering is why pmm_init() moved ahead of the display block in
// kernel_main() -- see kernel/drivers/display/display_virtio.c.
int virtio_gpu_init(void);

// Registers the display_driver that sits on this device
// (kernel/drivers/display/display_virtio.c). Called before
// display_probe(), and BEFORE vesafb_register() -- registration order
// is priority order, and a card that can program its own mode should
// not inherit the one the firmware settled on.
void virtio_gpu_display_register(void);

int virtio_gpu_present(void);

// The size the HOST says it wants (VIRTIO_GPU_CMD_GET_DISPLAY_INFO's
// preferred rect for scanout 0). 0 when the device reported none, in
// which case the caller falls back to display.h's ladder.
int virtio_gpu_preferred(uint32_t *out_w, uint32_t *out_h);

// Creates a framebuffer resource of this size, attaches guest memory to
// it, makes it scanout 0, and reports where the pixels are.
//
// The OLD mode survives a failure: the new resource and its frames are
// allocated first and the previous ones are released only once the
// device has accepted the new scanout. That is what display.h's
// set_mode contract requires ("must leave the old mode intact on
// failure") and it is not free -- it means both framebuffers exist at
// once for the length of the call.
int virtio_gpu_set_mode(uint32_t w, uint32_t h, struct display_surface *out);

// TRANSFER_TO_HOST_2D + RESOURCE_FLUSH for one rectangle. Nothing
// appears on screen until this runs: the framebuffer is ordinary guest
// RAM that the device reads on command, not memory anything scans.
void virtio_gpu_flush(int x, int y, int w, int h);

// Two more scanouts, created beside the first by set_mode when frames
// allow: count is 3 then, 1 otherwise. flip(i) is SET_SCANOUT to that
// resource, complete before it returns, so live == the last flip.
int  virtio_gpu_scanout_count(void);
void virtio_gpu_scanout_at(int index, struct display_surface *out);
int  virtio_gpu_flip(int index);
int  virtio_gpu_scanout_live(void);

// The cursor plane, on the device's second queue. A virtio-gpu cursor
// resource is 64x64 and nothing else, so a smaller image is padded with
// transparent pixels rather than refused.
int  virtio_gpu_cursor_available(void);
int  virtio_gpu_cursor_define(const uint32_t *argb, int w, int h, int hot_x, int hot_y);
void virtio_gpu_cursor_move(int x, int y);
void virtio_gpu_cursor_show(int on);

// Successful control-queue commands, cumulative. Exported so a KTEST
// can assert the driver really talked to the device.
uint32_t virtio_gpu_commands(void);

// The monitor's EDID for scanout 0, when the device offers it (a
// display_driver.read_edid). Bytes copied, 0 without the feature.
int virtio_gpu_read_edid(uint8_t *out, int cap);

#endif
