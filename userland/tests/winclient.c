// The first ring-3 program in toy-os to own a REAL window on the
// desktop -- one that lives in the window manager's own window list,
// with ordinary chrome, a taskbar button, focus and z-order, alongside
// the kernel-space apps.
//
// This is what separates it from the two older experiments:
//   gui_test.c  maps the whole physical framebuffer and draws straight
//               onto the screen -- modal, no window at all.
//   win_test.c  gets a private buffer the kernel composites with a
//               hand-drawn title bar -- a real client/server split, but
//               still modal, single-window, and outside the WM's list.
// This one is a client of the windowing protocol (abi/win_proto.h): it
// asks the server for a window, draws into the shared buffer it gets
// back, and BLOCKS waiting for input events rather than polling. While
// it waits it consumes no CPU at all.
//
// What it does: fills its window with a colour, and cycles to the next
// colour on any keypress or click. Esc, 'q', or the window's close
// button all make it exit cleanly, destroying its window on the way
// out.
#include <stdint.h>
#include "rt/sys.h"





static int win_request(struct win_request_msg *req) {
    return (int)sys_call(SYS_WIN_REQUEST, (uint64_t)(uintptr_t)req, 0, 0);
}

// The documented SYS_WAIT_EVENT loop -- a 0 return means "woken, ask
// again", not "no event". See syscall_abi.h for why the kernel can't
// hand the event over at wake time. Does not spin: every pass that
// finds nothing parks this process again.
static int wait_event(struct win_event *ev) {
    int64_t r;
    do {
        r = sys_call(SYS_WAIT_EVENT, (uint64_t)(uintptr_t)ev, 0, 0);
    } while (r == 0);
    return (int)r;
}

#define WIN_W 320
#define WIN_H 200

// 0xRRGGBB straight into the buffer -- this project's fixed target
// (QEMU -vga std, 32bpp) matches that layout, the same assumption
// gui_test.c and win_test.c already make.
static const uint32_t COLORS[] = {
    0x2E4053, 0x7D3C98, 0x1E8449, 0xB03A2E, 0xB7950B,
};
#define COLOR_COUNT (int)(sizeof(COLORS) / sizeof(COLORS[0]))

// A border makes it obvious at a glance that the client's pixels land
// exactly inside the content area -- if the WM's clipping or the
// content-origin maths were off, this frame would be cut or offset
// rather than sitting flush, which a flat fill would hide completely.
static void paint(volatile uint32_t *buf, uint32_t color) {
    for (int y = 0; y < WIN_H; y++) {
        for (int x = 0; x < WIN_W; x++) {
            int edge = (x < 3 || y < 3 || x >= WIN_W - 3 || y >= WIN_H - 3);
            buf[y * WIN_W + x] = edge ? 0xECF0F1 : color;
        }
    }
}

int main(void) {
    struct win_request_msg req;
    for (unsigned i = 0; i < sizeof(req); i++) ((uint8_t *)&req)[i] = 0;

    req.type = WIN_REQ_CREATE;
    req.a = WIN_W;
    req.b = WIN_H;
    req.c = 260; // where on screen; 0 would take the server's cascade
    req.d = 180;
    if (win_request(&req) != 1) sys_exit(1);

    uint32_t id = req.window;
    volatile uint32_t *buf = (volatile uint32_t *)(uintptr_t)win_buffer_vaddr(id);

    // The buffer address is DERIVED from the window id rather than
    // returned -- win_buffer_vaddr() is part of the protocol, so a
    // client never has to be told where its own pixels landed.

    for (unsigned i = 0; i < sizeof(req); i++) ((uint8_t *)&req)[i] = 0;
    req.type = WIN_REQ_TITLE;
    req.window = id;
    const char *title = "Ring 3 Client";
    int t = 0;
    for (; title[t] && t < WIN_TITLE_LEN - 1; t++) req.text[t] = title[t];
    req.text[t] = '\0';
    win_request(&req);

    int color = 0;
    paint(buf, COLORS[color]);

    for (unsigned i = 0; i < sizeof(req); i++) ((uint8_t *)&req)[i] = 0;
    req.type = WIN_REQ_PRESENT;
    req.window = id;
    win_request(&req);

    for (;;) {
        struct win_event ev;
        if (wait_event(&ev) != 1) break; // the server refused -- give up

        int quit = 0;
        int redraw = 0;

        if (ev.type == WIN_EV_CLOSE) {
            quit = 1;
        } else if (ev.type == WIN_EV_KEY) {
            if (ev.a == 0x1B || ev.a == 'q') quit = 1; // Esc or q
            else redraw = 1;
        } else if (ev.type == WIN_EV_MOUSE_DOWN) {
            redraw = 1;
        }

        if (quit) break;
        if (redraw) {
            color = (color + 1) % COLOR_COUNT;
            paint(buf, COLORS[color]);

            for (unsigned i = 0; i < sizeof(req); i++) ((uint8_t *)&req)[i] = 0;
            req.type = WIN_REQ_PRESENT;
            req.window = id;
            win_request(&req);
        }
    }

    // Destroy explicitly rather than relying on process teardown: the
    // server cleans up after a dead client anyway, but a client that
    // closes its own windows is the well-behaved case, and this is the
    // path that exercises WIN_REQ_DESTROY at all.
    for (unsigned i = 0; i < sizeof(req); i++) ((uint8_t *)&req)[i] = 0;
    req.type = WIN_REQ_DESTROY;
    req.window = id;
    win_request(&req);

    sys_exit(0);
}
