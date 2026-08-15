// A ring-3 client that can be made to HANG on purpose.
//
// WHY THIS EXISTS
// ---------------
// The window manager's not-responding detection and its force-quit
// dialog cannot be tested against a healthy app, and there was no
// unhealthy one. Every client in the tree answers its event queue
// promptly, which is exactly what the feature is for -- so without this
// the only way to see the dialog would be to break a real app by hand,
// look at it, and put it back. That is not a test.
//
// So: press 'h' and this client stops pumping its queue, permanently.
// The WM's ping goes unanswered, the title bar gains "(Not Responding)",
// and asking it to close raises the dialog. Force Quit kills it.
//
// HOW IT HANGS MATTERS
// --------------------
// It spins inside its own on_key callback rather than calling
// sys_exit() or blocking on a syscall, because those are different
// conditions with different correct outcomes: a process that exits is
// gone (the window server drops its windows), and one blocked in a
// syscall is descheduled but perfectly alive. What "not responding"
// means is specifically "the event loop is not turning" -- an app stuck
// in its own code -- and a busy spin inside a callback is that, exactly.
//
// It also means the process stays SCHED_READY and keeps consuming its
// scheduler slice, which is the realistic case: a hung app is usually
// burning CPU, not politely idle. The desktop staying responsive while
// this one spins is itself worth watching.
#include <stdint.h>
#include "rt/sys.h"
#include "ui/uapp.h"
#include "ui/ugfx.h"

#define WIN_W 260
#define WIN_H 140

static int g_hung;

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    struct ugfx_surface *s = uapp_surface(d);
    uint32_t bg = g_hung ? ugfx_rgb(200, 120, 120) : ugfx_rgb(140, 180, 140);
    ugfx_fill(s, bg);
    ugfx_draw_string(s, 10, 10, g_hung ? "hung" : "press h to hang",
                      ugfx_rgb(10, 10, 10), bg);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    if (key != 'h') return;

    g_hung = 1;
    // Paint the new state BEFORE wedging -- after this the loop never
    // runs again, so a redraw request would never be serviced and the
    // window would sit there looking healthy while being anything but.
    //
    // BOTH calls: uapp_flush() paints only what is already dirty, so on
    // its own it did nothing here and the window stayed the healthy
    // colour while the app was thoroughly wedged -- a misleading picture
    // of exactly the state this program exists to show.
    uapp_redraw(a);
    uapp_flush(a);
    sys_eprint("hangclient: hanging now\n");

    for (;;) {
        // Spin. Deliberately not sys_yield() in a loop either: yielding
        // would still never drain the event queue, but it would make
        // this a cooperative process rather than the CPU-burning kind a
        // real hang usually is.
    }
}

int main(void) {
    struct uapp_desc desc = {
        .title   = "Hang Test",
        .w       = WIN_W,
        .h       = WIN_H,
        .x       = 320,
        .y       = 240,
        .on_draw = on_draw,
        .on_key  = on_key,
    };
    return uapp_run(&desc);
}
