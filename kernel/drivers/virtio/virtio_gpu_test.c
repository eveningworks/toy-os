// KTESTs for virtio-gpu, through the live driver.
//
// THE GATE IS THE INTERESTING PART, as it is for every driver test
// here. These skip when there is no virtio-gpu -- which is EVERY
// ordinary boot, including the whole `make test` suite, because
// tools/serial_console.py launches with the default VGA adapter. So on
// their own they would be a file that never runs anything.
//
// What makes them real is tools/virtio_gpu_test.py: it boots a guest
// with `-vga virtio` and runs `ktest virtio-gpu` inside it, then checks
// the pixels from outside. The tests below are the half that can see
// the driver's own state; the tool is the half that can see the screen.
// Neither is sufficient and both are cheap.
#include "virtio_gpu.h"
#include "display.h"
#include "string.h"
#include "ktest.h"

KTEST("virtio-gpu", "the device is claimed and is the active display") {
    if (!virtio_gpu_present()) KTEST_SKIP("no virtio-gpu on this machine");

    const struct display_driver *d = display_active();
    KTEST_ASSERT(d != 0);
    // A virtio-gpu that came up and did NOT become the display means
    // the probe refused its own device, which is a failure rather than
    // a skip -- registration order puts it ahead of vesafb.
    KTEST_ASSERT_EQ(k_strcmp(d->name, "virtio-gpu"), 0);

    // The one capability that is not optional here: the framebuffer is
    // guest RAM the device reads on command, so a driver that forgot to
    // declare it renders a perfect frame into memory and shows nothing.
    KTEST_ASSERT(display_has(DISPLAY_CAP_NEEDS_FLUSH));
}

KTEST("virtio-gpu", "the surface is the one the driver allocated") {
    if (!virtio_gpu_present()) KTEST_SKIP("no virtio-gpu on this machine");

    struct display_surface s;
    display_get_surface(&s);
    KTEST_ASSERT(s.addr != 0);
    KTEST_ASSERT(s.width > 0 && s.height > 0);
    KTEST_ASSERT_EQ(s.bpp, 32);
    // The backing is one contiguous allocation of exactly width*4 per
    // row -- unlike a VESA framebuffer, where the firmware picks a
    // pitch and it is routinely larger than the visible width.
    KTEST_ASSERT_EQ(s.pitch, s.width * 4);
    // Below 4 GiB, because the device is handed this as a PHYSICAL
    // address and everything here relies on the identity map.
    KTEST_ASSERT(s.addr + (uint64_t)s.pitch * s.height <= 0x100000000ull);
}

KTEST("virtio-gpu", "a flush is two commands, and an off-screen one is none") {
    if (!virtio_gpu_present()) KTEST_SKIP("no virtio-gpu on this machine");

    struct display_surface s;
    display_get_surface(&s);

    // TRANSFER_TO_HOST_2D then RESOURCE_FLUSH. Counting them is what
    // distinguishes "the flush ran" from "the flush returned" -- the
    // command counter only advances on a completed round trip.
    uint32_t before = virtio_gpu_commands();
    virtio_gpu_flush(0, 0, 16, 16);
    KTEST_ASSERT_EQ(virtio_gpu_commands(), before + 2);

    // Clipped to nothing. gfx damages generously and a rectangle past
    // the edge is not a caller bug -- but the device REJECTS one, and a
    // rejected transfer means the whole frame fails to appear, so this
    // is clipped here rather than passed through.
    before = virtio_gpu_commands();
    virtio_gpu_flush((int)s.width + 8, (int)s.height + 8, 32, 32);
    KTEST_ASSERT_EQ(virtio_gpu_commands(), before);

    // A rectangle STRADDLING the edge is clipped, not dropped: the part
    // that exists still has to be published.
    before = virtio_gpu_commands();
    virtio_gpu_flush((int)s.width - 8, 0, 32, 32);
    KTEST_ASSERT_EQ(virtio_gpu_commands(), before + 2);
}

// The cursor image, static because the ring-0 frame budget is 1 KiB and
// this is 1 KiB on its own.
static uint32_t g_cursor[16 * 16];

KTEST("virtio-gpu", "the cursor plane accepts an image, a move and a hide") {
    if (!virtio_gpu_present()) KTEST_SKIP("no virtio-gpu on this machine");
    if (!virtio_gpu_cursor_available()) KTEST_SKIP("this virtio-gpu has no cursor queue");

    // The capability and the function pointers are one fact stated
    // twice; display_probe() refuses a driver where they disagree, so
    // asserting the flag here also asserts the pointers are there.
    KTEST_ASSERT(display_has(DISPLAY_CAP_CURSOR));

    for (int i = 0; i < 16 * 16; i++) g_cursor[i] = 0xFF00FF00u;

    // Smaller than the device's fixed 64x64 cursor resource, which is
    // the case that must be padded rather than refused.
    uint32_t before = virtio_gpu_commands();
    KTEST_ASSERT(virtio_gpu_cursor_define(g_cursor, 16, 16, 0, 0));
    // ONE control command, not two, and the difference is the whole
    // shape of this device: the image reaches the host through a
    // TRANSFER_TO_HOST_2D on the control queue, while UPDATE_CURSOR
    // goes out on the cursor queue -- which carries no response at all,
    // so it cannot be counted here. A cursor move costs nothing on the
    // control queue, which is the point of having a second one.
    KTEST_ASSERT_EQ(virtio_gpu_commands(), before + 1);

    // ...and a move really is free of control traffic.
    uint32_t after_define = virtio_gpu_commands();
    virtio_gpu_cursor_move(64, 64);
    KTEST_ASSERT_EQ(virtio_gpu_commands(), after_define);

    virtio_gpu_cursor_show(0);
    virtio_gpu_cursor_show(1);

    // A cursor larger than the resource is refused rather than
    // truncated -- truncation would silently draw a different pointer.
    KTEST_ASSERT_EQ(virtio_gpu_cursor_define(g_cursor, 128, 128, 0, 0), 0);
    KTEST_ASSERT_EQ(virtio_gpu_cursor_define(0, 16, 16, 0, 0), 0);

    // Leave it hidden: this test drew a green square on the screen and
    // the desktop underneath has no idea it is there.
    virtio_gpu_cursor_show(0);
}
