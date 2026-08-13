#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>

// The display-device layer: what a graphics card provides, separated
// from the drawing code that renders into it.
//
// **Why this exists.** gfx.c used to be both things at once -- a
// rasteriser (put_pixel, fill_rect, draw_char, clip, fonts) AND the
// framebuffer's owner -- and when a second card arrived it grew
// `#include "vmsvga.h"` plus seven hardcoded calls to that one device.
// A third card would have meant another include and another if/else in
// each of them. Adding a card should be one new file and one registry
// line, touching nothing else; that is what this interface buys.
//
// **The layering.** A driver owns the device: modes, the framebuffer,
// telling the hardware what changed, the cursor. gfx.c owns pixels and
// knows nothing about which card it's on. vga.c's console and the WM sit
// above gfx.c and know less still.
//
// ---------------------------------------------------------------------
// Writing a driver
// ---------------------------------------------------------------------
//
// Fill in a `struct display_driver`, register it, done:
//
//   static const struct display_driver my_driver = {
//       .name = "mycard",
//       .probe = my_probe,          // 0 if the hardware isn't here
//       .get_surface = my_surface,  // where and how big the pixels are
//       .caps = DISPLAY_CAP_NEEDS_FLUSH,
//       .flush = my_flush,
//   };
//   display_register(&my_driver);   // from its *_init(), before display_probe()
//
// Only `name`, `probe` and `get_surface` are required. Everything else
// is optional -- leave the pointer NULL and the layer either falls back
// to software or reports the capability as absent. Advertise a
// capability ONLY if the matching function is present; display_probe()
// checks this and refuses a driver that lies, because a missing flush on
// a card that needs one produces a frozen display with the correct
// pixels sitting in memory, which is a genuinely hard bug to read (it
// happened -- see CHANGELOG.md).

// What a driver can do. A capability and its function pointer are one
// fact stated twice, deliberately: callers ask the caps, and
// display_probe() rejects a driver whose caps and pointers disagree.
#define DISPLAY_CAP_NEEDS_FLUSH  (1u << 0) // pixels don't appear until flush()
#define DISPLAY_CAP_CURSOR       (1u << 1) // hardware cursor plane
#define DISPLAY_CAP_ACCEL_FILL   (1u << 2) // device-side rectangle fill
#define DISPLAY_CAP_ACCEL_COPY   (1u << 3) // device-side rectangle copy
#define DISPLAY_CAP_MODESET      (1u << 4) // can list and select modes

// Where the pixels live and how they're laid out.
struct display_surface {
    uint64_t addr;   // physical == virtual here (low 4GiB is identity-mapped)
    uint32_t pitch;  // bytes per scanline, NOT width * bpp -- they differ
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;    // 32 or 24
};

struct display_mode {
    uint32_t width, height;
    uint8_t  bpp;
};

struct display_driver {
    const char *name;

    // Is this hardware present, and can this driver take it? Returns 1
    // to claim it. Called in registration order; the first to claim
    // wins, so register specific drivers before the fallback.
    int (*probe)(void);

    // Required. Where the framebuffer currently is.
    void (*get_surface)(struct display_surface *out);

    uint32_t caps;

    // Required when DISPLAY_CAP_NEEDS_FLUSH. Publishes a rectangle that
    // has changed. On a card that scans memory continuously this is
    // absent and gfx never calls it.
    void (*flush)(int x, int y, int w, int h);

    // Required together when DISPLAY_CAP_CURSOR.
    int  (*cursor_define)(const uint32_t *argb, int w, int h, int hot_x, int hot_y);
    void (*cursor_move)(int x, int y);
    void (*cursor_show)(int on);

    // Optional acceleration. gfx falls back to its own loops when
    // absent, so a driver may implement either, both or neither.
    void (*fill_rect)(int x, int y, int w, int h, uint32_t color);
    void (*copy_rect)(int sx, int sy, int dx, int dy, int w, int h);

    // Optional mode setting. mode_count/mode_at enumerate; set_mode
    // returns 1 on success and must leave the old mode intact on
    // failure. Required together when DISPLAY_CAP_MODESET.
    int  (*mode_count)(void);
    void (*mode_at)(int index, struct display_mode *out);
    int  (*set_mode)(const struct display_mode *mode);
};

// Called by each driver's own *_init() before display_probe() runs.
// Order is priority order: the first driver whose probe() claims the
// hardware becomes the active one, so the generic fallback registers
// last.
void display_register(const struct display_driver *drv);

// Runs every registered driver's probe() in order and activates the
// first that claims. Returns 1 if one did. Logs which, and its caps.
int display_probe(void);

// The active driver, or NULL before a successful display_probe().
const struct display_driver *display_active(void);

// Convenience wrappers -- these check the capability and the pointer, so
// callers don't have to. Safe to call with no active driver.
int  display_has(uint32_t cap);
void display_get_surface(struct display_surface *out);
void display_flush(int x, int y, int w, int h);
int  display_cursor_define(const uint32_t *argb, int w, int h, int hot_x, int hot_y);
void display_cursor_move(int x, int y);
void display_cursor_show(int on);
int  display_fill_rect(int x, int y, int w, int h, uint32_t color); // 0 = not accelerated, caller should do it
int  display_copy_rect(int sx, int sy, int dx, int dy, int w, int h);
int  display_mode_count(void);
void display_mode_at(int index, struct display_mode *out);
int  display_set_mode(const struct display_mode *mode);

#endif
