// The display_driver on top of virtio-gpu.
//
// The device half is kernel/drivers/virtio/virtio_gpu.c; this file is
// only the adapter, exactly as block_virtio.c adapts virtio-blk to the
// `block_device` registry. Two axes: what the device SITS ON (the
// virtio transport) and what it PLUGS INTO (this registry).
//
// WHY THIS DRIVER CAN PROBE AT ALL, which is the interesting part.
// Every other card here probes with nothing but port I/O and a BAR, so
// display_probe() used to run before the frame allocator existed. A
// virtio device cannot: its virtqueues come from pmm_alloc_contiguous(),
// and so does the framebuffer it is about to own. So pmm_init() moved
// ahead of the display block in kernel_main(). It depends on nothing
// but the multiboot memory map and the kernel's own symbols, which have
// both been true since the first instruction of kernel_main() -- it sat
// later purely because nothing earlier had asked for a frame.
//
// The alternative was a late handover: boot on vesafb, bring virtio-gpu
// up after the heap, then re-point the active driver. That needs gfx to
// re-read its surface, the console and compositor to repaint, and it
// has to deal with a ring-3 compositor that may already hold a
// framebuffer grant. Moving one call that had no dependencies was the
// smaller change by a wide margin.
//
// REGISTRATION ORDER IS PRIORITY ORDER, so this registers before
// vesafb: on `-vga virtio` GRUB leaves a perfectly usable VESA
// framebuffer behind, and taking it would mean living with whatever
// mode the firmware settled on instead of programming the one that was
// asked for. Same reasoning as vmsvga's.
#include "display.h"
#include "virtio_gpu.h"
#include "klog.h"
#include "kfmt.h"

static struct display_surface g_surface;
static int g_active = 0;

// Adds the cursor capability once the device is up. It has to happen
// here rather than at registration, because whether there IS a cursor
// plane is only known after the queues are negotiated -- and
// display_probe() refuses a driver advertising a capability it lacks,
// so the flag and the function pointers must be set together.
static void adopt_cursor_plane(void);

static int virtio_drv_probe(void) {
    if (!virtio_gpu_init()) return 0;

    // THE LADDER FIRST, and the host's own preference only as a
    // fallback -- which is the opposite of what it looks like it should
    // be, so the reason matters. display_mode_candidate(0) IS
    // `video=<W>x<H>` when that flag was given (docs/boot-flags.md), and
    // a flag the user typed must not be silently overruled by what the
    // host happens to prefer. QEMU resizes its window to whatever
    // scanout the guest sets, so honouring the flag costs nothing here.
    int w, h;
    for (int i = 0; display_mode_candidate(i, &w, &h); i++) {
        if (virtio_gpu_set_mode((uint32_t)w, (uint32_t)h, &g_surface)) {
            klog_printf("virtio-gpu: set %dx%d\n", w, h);
            g_active = 1;
            adopt_cursor_plane();
            return 1;
        }
    }

    // Nothing on the ladder was accepted. GET_DISPLAY_INFO's preferred
    // rect is the last thing to try: it is what the host says it wants,
    // so a device refusing everything else will usually take it.
    uint32_t pw = 0, ph = 0;
    if (virtio_gpu_preferred(&pw, &ph) && virtio_gpu_set_mode(pw, ph, &g_surface)) {
        klog_printf("virtio-gpu: no ladder mode accepted -- using the host's preferred %ux%u\n",
                    pw, ph);
        g_active = 1;
        adopt_cursor_plane();
        return 1;
    }

    // Claimed the device and could not drive it. Refusing here lets
    // vesafb take over the framebuffer GRUB left, which is a working
    // display rather than a black screen -- but only on a device with
    // VGA compatibility (`-vga virtio`); a bare virtio-gpu-pci has no
    // such framebuffer and there is nothing to fall back to.
    klog_write("virtio-gpu: no mode could be set -- leaving the display to another driver\n");
    return 0;
}

static void virtio_drv_get_surface(struct display_surface *out) { *out = g_surface; }

static void virtio_drv_flush(int x, int y, int w, int h) { virtio_gpu_flush(x, y, w, h); }

// --- why this driver does NOT advertise DISPLAY_CAP_MODESET ----------
//
// It can set a mode -- that is what the probe above does, and what
// `video=<W>x<H>` reaches. What it cannot do is set one AFTER boot,
// and the reason is not in this file: virtio_gpu_set_mode() allocates
// a new framebuffer and frees the old one, while gfx.c caches the
// surface pointer it was given at gfx_init() and the ring-3 compositor
// holds a mapping of those exact frames (win_surface.c). A live mode
// change would leave both of them writing into freed memory, and
// nothing in the display layer re-plumbs either.
//
// So the capability is not claimed. A driver that advertises modeset
// and returns 1 from set_mode() while handing the rest of the system a
// dangling framebuffer is worse than one that never offered -- which is
// exactly the failure display_probe()'s honesty check exists to stop,
// arriving from the other direction. vmsvga is in the same position and
// makes the same choice. docs/roadmap.md carries the runtime mode
// switch as its own item, because it needs gfx re-init and a compositor
// re-grant, not a driver change.

// --- cursor ------------------------------------------------------------

static int virtio_drv_cursor_define(const uint32_t *argb, int w, int h, int hx, int hy) {
    return virtio_gpu_cursor_define(argb, w, h, hx, hy);
}
static void virtio_drv_cursor_move(int x, int y) { virtio_gpu_cursor_move(x, y); }
static void virtio_drv_cursor_show(int on) { virtio_gpu_cursor_show(on); }

static struct display_driver virtio_gpu_display = {
    .name = "virtio-gpu",
    .probe = virtio_drv_probe,
    .get_surface = virtio_drv_get_surface,
    // NEEDS_FLUSH always: the framebuffer is guest RAM the device reads
    // on command, so pixels written and never transferred are invisible.
    // CURSOR is decided at claim time -- see adopt_cursor_plane(). No
    // MODESET, and the block above says why.
    .caps = DISPLAY_CAP_NEEDS_FLUSH,
    .flush = virtio_drv_flush,
};

void virtio_gpu_display_register(void) { display_register(&virtio_gpu_display); }

static void adopt_cursor_plane(void) {
    if (!virtio_gpu_cursor_available()) return;
    virtio_gpu_display.caps |= DISPLAY_CAP_CURSOR;
    virtio_gpu_display.cursor_define = virtio_drv_cursor_define;
    virtio_gpu_display.cursor_move = virtio_drv_cursor_move;
    virtio_gpu_display.cursor_show = virtio_drv_cursor_show;
}
