// The first userland program in this project to draw to the real screen
// and read real input, entirely from ring 3 -- via SYS_GUI_INIT (maps
// the linear framebuffer directly into this process's own address
// space) and SYS_GUI_POLL_KEY (a non-blocking keyboard read).
//
// Scope, honestly: this is MODAL, not a real window. There's no
// scheduler yet, so this process has the real screen entirely to
// itself while it runs -- it isn't a window inside the kernel-space
// window manager (apps/wm.c), which is a separate, so-far-untouched
// piece of this project. See apps/README.md's "GUI in user space" note.
//
// What it does: fills the screen with a solid color, then on every
// keypress cycles to a new color, until 'q' is pressed, at which point
// it exits cleanly via the exit syscall.
#include <stdint.h>
#include "sys.h"





static void fill_screen(volatile uint32_t *fb, const struct gui_info *info, uint32_t color) {
    uint32_t pitch_pixels = info->pitch / 4; // assumes 32bpp, matching gfx.c's own assumption
    for (uint32_t y = 0; y < info->height; y++) {
        for (uint32_t x = 0; x < info->width; x++) {
            fb[y * pitch_pixels + x] = color;
        }
    }
}

int main(void) {
    struct gui_info info;
    int64_t ok = sys_call(SYS_GUI_INIT, (uint64_t)(uintptr_t)&info, 0, 0);
    if (!ok) sys_exit(1);

    volatile uint32_t *fb = (volatile uint32_t *)(uintptr_t)GUI_FB_VADDR;

    uint32_t color = 0x00224477; // an arbitrary starting blue
    fill_screen(fb, &info, color);

    for (;;) {
        int64_t key = sys_call(SYS_GUI_POLL_KEY, 0, 0, 0);
        if (key == 'q') break;
        if (key >= 0) {
            color = (color + 0x00335577) & 0x00FFFFFF; // cycle to a new color
            fill_screen(fb, &info, color);
        }
    }

    sys_exit(0);
}
