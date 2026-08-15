// The first ring-3 program in toy-os to own a REAL window on the
// desktop -- one that lives in the window manager's own window list,
// with ordinary chrome, a taskbar button, focus and z-order, alongside
// the kernel-space apps.
//
// This is the app-shaped counterpart to the two older experiments:
//   gui_test.c  maps the whole physical framebuffer and draws straight
//               onto the screen -- modal, no window at all.
//   win_test.c  gets a private buffer the kernel composites with a
//               hand-drawn title bar -- a real client/server split, but
//               still modal, single-window, and outside the WM's list.
// This one is a TWP client: it asks TWS for a window, draws into the
// shared buffer it gets back, and BLOCKS waiting for input rather than
// polling. While it waits it consumes no CPU at all.
//
// What it does: fills its window with a colour, and cycles to the next
// colour on any keypress or click. Esc, 'q', or the window's close
// button all make it exit cleanly, destroying its window on the way out.
//
// **It is also the smallest possible Toykit app**, which is the point
// of it now. Everything above the `paint()` function is gone: the
// create/title/present/destroy handshake, the blocking event loop and
// its switch, and the private win_request()/clear_req()/wait_event()
// helpers this file used to define for itself. That was 55 of its 141
// lines. What is left is what the program actually does.
#include <stdint.h>
#include "ui/uapp.h"

#define WIN_W 320
#define WIN_H 200

// 0xRRGGBB straight into the buffer -- this project's fixed target
// (QEMU -vga std, 32bpp) matches that layout, the same assumption
// gui_test.c and win_test.c already make.
static const uint32_t COLORS[] = {
    0x2E4053, 0x7D3C98, 0x1E8449, 0xB03A2E, 0xB7950B,
};
#define COLOR_COUNT (int)(sizeof(COLORS) / sizeof(COLORS[0]))

static int g_color;

// A border makes it obvious at a glance that the client's pixels land
// exactly inside the content area -- if TWS's clipping or the
// content-origin maths were off, this frame would be cut or offset
// rather than sitting flush, which a flat fill would hide completely.
static void on_draw(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    struct ugfx_surface *s = uapp_surface(d);
    for (int y = 0; y < s->h; y++) {
        for (int x = 0; x < s->w; x++) {
            int edge = (x < 3 || y < 3 || x >= s->w - 3 || y >= s->h - 3);
            s->pixels[y * s->w + x] = edge ? 0xECF0F1 : COLORS[g_color];
        }
    }
}

static void next_color(struct uapp *a) {
    g_color = (g_color + 1) % COLOR_COUNT;
    uapp_redraw(a);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    if (key == 0x1B || key == 'q') uapp_quit(a, 0); // Esc or q
    else next_color(a);
}

static void on_press(struct uapp *a, int x, int y, unsigned buttons) {
    (void)x; (void)y; (void)buttons;
    next_color(a);
}

int main(void) {
    struct uapp_desc desc = {
        .title   = "Ring 3 Client",
        // Opting in is one field. Everything behind it -- answering
        // TWS's proposal, reallocating the buffer, rebuilding the
        // surface, repainting -- is uapp's (ui/uapp.h). This client
        // adds no resize code at all: on_draw already fills whatever
        // surface it is handed, which is what "an app that has never
        // heard of resizing resizes correctly" means in practice.
        .flags   = UAPP_RESIZABLE,
        .min_w   = 120,
        .min_h   = 80,
        .w       = WIN_W,
        .h       = WIN_H,
        .x       = 260,
        .y       = 180,
        .on_draw = on_draw,
        .on_key  = on_key,
        .on_press = on_press,
    };
    return uapp_run(&desc);
}
