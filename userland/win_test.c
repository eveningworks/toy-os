// A freestanding userland program exercising SYS_WIN_CREATE +
// SYS_WIN_PRESENT (see syscall_abi.h) -- a genuine client/server
// windowing split, unlike gui_test.c's SYS_GUI_INIT (which maps the
// WHOLE real framebuffer into the process and lets it draw straight
// onto the real screen). This process never touches the real
// framebuffer at all: it only ever sees the private w*h pixel buffer
// SYS_WIN_CREATE hands it, and asks the kernel to composite that (with
// a real title bar + close button drawn around it) via SYS_WIN_PRESENT.
//
// Like gui_test.c, it writes raw 0xRRGGBB values with no packing
// helper of its own -- this project's fixed target (QEMU, `-vga std`,
// 32bpp) happens to match that layout directly, same assumption
// gui_test.c already makes and that's worked fine so far.
//
// What it does: fills its buffer with a color, presents it, then on
// every keypress cycles to a new color and re-presents -- until 'q' or
// Esc, at which point it exits cleanly via the exit syscall.
#include <stdint.h>
#include "sys.h"





#define WIN_W 300
#define WIN_H 180

static void fill_buffer(volatile uint32_t *buf, uint32_t pitch, uint32_t color) {
    uint32_t pitch_pixels = pitch / 4;
    for (uint32_t y = 0; y < WIN_H; y++) {
        for (uint32_t x = 0; x < WIN_W; x++) {
            buf[y * pitch_pixels + x] = color;
        }
    }
}

int main(void) {
    struct win_request req;
    req.w = WIN_W;
    req.h = WIN_H;
    req.x = 200;
    req.y = 150;

    int64_t ok = sys_call(SYS_WIN_CREATE, (uint64_t)(uintptr_t)&req, 0, 0);
    if (!ok) sys_exit(1);

    volatile uint32_t *buf = (volatile uint32_t *)(uintptr_t)WIN_BUF_VADDR;

    uint32_t color = 0x00335588; // an arbitrary starting blue
    fill_buffer(buf, req.pitch, color);
    sys_call(SYS_WIN_PRESENT, 0, 0, 0);

    for (;;) {
        int64_t key = sys_call(SYS_READ_KEY, 0, 0, 0);
        if (key == -1) continue; // spin -- SYS_READ_KEY never blocks
        if (key == 'q' || key == 27) break;

        color = (color + 0x00224466) & 0x00FFFFFF; // cycle to a new color
        fill_buffer(buf, req.pitch, color);
        sys_call(SYS_WIN_PRESENT, 0, 0, 0);
    }

    sys_exit(0);
}
