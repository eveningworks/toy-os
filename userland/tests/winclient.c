// The first ring-3 program in toy-os to own a REAL window on the
// desktop -- one that lives in the window manager's own window list,
// with ordinary chrome, a taskbar button, focus and z-order, alongside
// the kernel-space apps.
//
// The two experiments before it -- a whole-framebuffer map and a
// kernel-composited single window -- are both retired; this is the one
// that survived. It is a TWP client: it asks TWS for a window, draws into the
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
#include "rt/sys.h"
#include "ui/uapp.h"
#include <stdio.h>
#include <pthread.h>

#define WIN_W 320
#define WIN_H 200

// 0xRRGGBB straight into the buffer -- this project's fixed target
// (QEMU -vga std, 32bpp) matches that layout.
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

// --- WHAT IS CURRENTLY HELD DOWN -------------------------------------
//
// The other untested protocol claim this file exists to exercise, beside
// the on_close veto below: a client can know a key is HELD, not merely
// that it was struck (abi/win_proto.h's WIN_EV_KEY_UP). Nothing else in
// the tree tracks a held key, because nothing else needs to -- every app
// here edits text or clicks buttons, both of which act on the press.
//
// Kept as a SET rather than a count, so a key held long enough to
// autorepeat -- which produces many presses and one release -- is still
// "one key down". A counter would go up with every repeat and never come
// back to zero, which is precisely the bug a game would report as "the
// player will not stop walking".
#define HELD_MAX 8
static int g_held[HELD_MAX];
static int g_held_count;

static int held_index(int key) {
    for (int i = 0; i < g_held_count; i++) if (g_held[i] == key) return i;
    return -1;
}

// Logged to fd 2 (the kernel log, readable with `dmesg` -- see
// keyboard.h's note that fd 2 here is not a second terminal stream), so
// tools/keyup_test.py can assert on the transitions rather than on
// pixels. Only the SET's changes are logged, not every repeat, which is
// what makes the log a record of what the client believes rather than of
// what arrived.
static void log_held(const char *what, int key) {
    char line[64];
    // The code, not the character: a release carries whatever the press
    // produced, and printing it as a glyph would hide the difference
    // between 'w' and 'W' that this is here to demonstrate.
    snprintf(line, sizeof line, "winclient: %s %d held=%d\n",
             what, key, g_held_count);
    sys_eprint(line);
}

// --- a worker thread waking the main loop ----------------------------
//
// The only thing in the tree that exercises uapp_post(), and it is here
// rather than in a real app for the reason the on_close veto is: a
// protocol edge case belongs in a diagnostic. What it demonstrates is
// the whole contract in three lines -- a worker touches NOTHING of the
// toolkit, posts two numbers, and the main thread is what runs a
// callback.
//
// The tids are logged from both sides because "on_user ran" is not the
// claim being made: the claim is that it ran on the MAIN thread while
// some OTHER thread posted it, and only the pair of numbers says that.
static struct uapp *g_app;

static void *poster(void *arg) {
    (void)arg;
    // Deliberately not touching g_app for anything but the parameter --
    // see ui/uapp.h. Sleeping first is what makes the post arrive while
    // the main thread is parked in SYS_WAIT_EVENT rather than still
    // inside on_key.
    sys_sleep_ms(50);
    uapp_post(g_app, 42, sys_gettid());
    return NULL;
}

static int on_user(struct uapp *a, int a0, int a1) {
    char line[96];
    snprintf(line, sizeof line, "winclient: on_user a0=%d from_tid=%d on_tid=%d\n",
             a0, a1, sys_gettid());
    sys_eprint(line);
    next_color(a);
    return 1;
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    // **THE HELD-SET BOOKKEEPING RUNS FOR EVERY KEY, BEFORE ANY SPECIAL
    // CASE.** keyup_test.py reads this log to decide whether a press and
    // its release were both reported, so a key that takes an early exit
    // below leaves it seeing a release with no press -- which is exactly
    // what a stuck key looks like to that tool. Putting the trigger
    // above this line did that to 'w' for one build.
    if (held_index(key) < 0 && g_held_count < HELD_MAX) {
        g_held[g_held_count++] = key;
        log_held("keydown", key);
    }
    if (key == 'p') {
        // A DETACHED thread: nothing joins it, and the process outlives
        // it either way. 'p' for post, and deliberately not a key
        // keyup_test.py drives.
        g_app = a;
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &at, poster, NULL) != 0)
            sys_eprint("winclient: pthread_create FAILED\n");
        return;
    }
    if (key == 'q') uapp_quit(a, 0); // Esc no longer closes -- Alt+F4 does
    else next_color(a);
}

static void on_key_up(struct uapp *a, int key, unsigned mods) {
    (void)a; (void)mods;
    int i = held_index(key);
    // An unmatched release is legal and must be tolerated -- the WM
    // claims Super and Alt+F4 on the press and delivers the release
    // anyway (ui/uapp.h). Logged so a test can see it happened rather
    // than silently ignored.
    if (i < 0) { log_held("keyup-unmatched", key); return; }
    g_held[i] = g_held[--g_held_count];
    log_held("keyup", key);
}

// REFUSES the first two close requests and accepts the third.
//
// This is the on_close veto's only caller in the tree, and it is here
// on purpose: uapp has always documented that returning 0 refuses a
// close, and nothing exercised it, so "an app can decline to be closed"
// was an untested claim. A diagnostic in userland/tests/ is exactly the
// right place for it -- exercising a protocol edge case is what this
// program is for.
//
// Two refusals rather than one, because there are exactly three ways a
// user closes a window -- the context menu's Close, the title bar's X,
// and Alt+F4 -- and all three must ASK. Refusing twice lets one test
// walk all three in order and prove each of them both asks and is
// refusable. Right-click > Close did neither, until wm_request_close()
// existed: it tore the window down on the spot.
static int g_close_requests;

static int on_close(struct uapp *a) {
    (void)a;
    g_close_requests++;
    sys_eprint(g_close_requests <= 2 ? "winclient: close refused\n"
                                      : "winclient: close accepted\n");
    return g_close_requests > 2;
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
        .on_key_up = on_key_up,
        .on_user = on_user,
        .on_press = on_press,
        .on_close = on_close,
    };
    return uapp_run(&desc);
}
