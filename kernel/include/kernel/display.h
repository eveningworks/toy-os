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
// happened -- see the git history).

// What a driver can do. A capability and its function pointer are one
// fact stated twice, deliberately: callers ask the caps, and
// display_probe() rejects a driver whose caps and pointers disagree.
#define DISPLAY_CAP_NEEDS_FLUSH  (1u << 0) // pixels don't appear until flush()
#define DISPLAY_CAP_CURSOR       (1u << 1) // hardware cursor plane
#define DISPLAY_CAP_ACCEL_FILL   (1u << 2) // device-side rectangle fill
#define DISPLAY_CAP_ACCEL_COPY   (1u << 3) // device-side rectangle copy
#define DISPLAY_CAP_MODESET      (1u << 4) // can list and select modes
#define DISPLAY_CAP_BACKLIGHT    (1u << 5) // a panel backlight it can dim
#define DISPLAY_CAP_FLIP         (1u << 6) // more than one scanout, switched at vblank
#define DISPLAY_CAP_SCALING      (1u << 7) // a scaler between the mode and a fixed panel

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

// How a mode smaller than the panel is placed on it, when the display
// has a scaler (DISPLAY_CAP_SCALING): i915's scaling-mode property and
// the Intel Windows driver's three choices. A driver with no scaler
// shows every mode at its own size and the enum is unused.
enum display_scaling {
    DISPLAY_SCALING_ASPECT = 0,   // largest same-aspect rectangle, centred
    DISPLAY_SCALING_FULL   = 1,   // stretched to the whole panel
    DISPLAY_SCALING_CENTER = 2,   // unscaled, centred
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

    // Required together when DISPLAY_CAP_BACKLIGHT. Levels are PERCENT,
    // 0..100; the driver owns the PWM duty behind them. set returns 1
    // when the hardware took it.
    int  (*backlight_get)(void);
    int  (*backlight_set)(int percent);

    // Required together when DISPLAY_CAP_FLIP. `scanout_count` buffers
    // (three, for a MAILBOX flip -- see win_surface.c), all the
    // geometry of get_surface(); index 0 IS that surface. flip(i) asks
    // for buffer i to be scanned from the next vblank and NEVER WAITS:
    // a second flip before the first lands simply replaces it, the
    // hardware showing whichever was last asked for. scanout_live() is
    // the buffer being scanned RIGHT NOW, which with the last flip
    // asked for is what tells a caller which buffer is free to draw
    // into. flip returns 1 when the request was accepted.
    int  (*scanout_count)(void);
    void (*scanout_at)(int index, struct display_surface *out);
    int  (*flip)(int index);
    int  (*scanout_live)(void);

    // Optional, and NOT a capability: the monitor's EDID base block,
    // raw. Copies up to `cap` bytes into `out` and returns how many, 0
    // when there is none. display_probe() asks once after a claim and
    // parses it (kernel/edid.h); a driver never parses its own.
    int  (*read_edid)(uint8_t *out, int cap);

    // Required when DISPLAY_CAP_SCALING: re-place the CURRENT mode on
    // the panel per `mode` (enum display_scaling). set_mode reads
    // display_scaling() itself; this is for a change with no mode
    // change. Returns 1 when the hardware took it.
    int  (*set_scaling)(int mode);

    // Optional: the system RAM the driver holds for the screen -- extra
    // scanouts, a cursor image, a framebuffer that IS guest RAM. Not
    // VRAM or stolen memory, which the frame allocator never counted.
    uint64_t (*ram_bytes)(void);
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
// Backlight, in percent. get returns -1 and set returns 0 without
// DISPLAY_CAP_BACKLIGHT.
int  display_backlight_get(void);
int  display_backlight_set(int percent);
// Scanouts. Without DISPLAY_CAP_FLIP the count is 1, index 0 is the
// surface, and flip refuses anything but 0 (which is a no-op).
int  display_scanout_count(void);
void display_scanout_at(int index, struct display_surface *out);
int  display_flip(int index);
int  display_scanout_live(void);

// RAM held for the screen: the active driver's ram_bytes() plus gfx.c's
// console back buffer. Task Manager's "Graphics" row (QUERY_MEMINFO).
uint64_t display_graphics_bytes(void);

// Which mechanism made the framebuffer write-combining at probe time
// (an enum paging_wc_result). Worth asking about because PAGING_WC_NONE
// is not a cosmetic difference -- it is the difference between a
// responsive desktop and a several-seconds-per-frame one on real
// hardware, and nothing on screen says which you got. `gfxbench` prints
// it beside its timings for exactly that reason.
int display_write_combining(void);

// The mode a MODESETTING driver should aim for, in pixels.
//
// Defaults to the size the multiboot2 header asks GRUB for
// (kernel/arch/x86_64/boot.asm), and is overridden by `video=<W>x<H>`
// on the GRUB command line -- see docs/boot-flags.md.
//
// WHY THIS EXISTS: GRUB can only pick from the modes the firmware
// offers, and a VESA BIOS with a short list (VirtualBox's VBoxVGA is
// the reported case) falls back to 640x480 however big the header's
// preference was. A driver that can program the CRTC itself does not
// have to live with that -- but it needs to be told what to aim for,
// and mirroring whatever GRUB settled on means it never asks for more.
//
// It is only meaningful to a driver with DISPLAY_CAP_MODESET-class
// ability. On a plain VESA framebuffer (vesafb) nothing can act on it,
// which is worth knowing before reporting the flag as broken.
void display_preferred_mode(int *out_w, int *out_h);

// The FALLBACK LADDER a modesetting driver walks. Index 0 is the
// preferred mode above; each next index is the next smaller standard
// size. Returns 1 while `index` names a candidate, 0 once exhausted.
//
// A ladder rather than a single attempt because "the adapter cannot do
// 1920x1080" should mean "then try 1600x900", not "give up and keep
// whatever GRUB left". Falling back to GRUB's mode is still the last
// resort, and it is always available -- but it should be the answer
// after the ladder, not instead of it.
//
// Candidates larger than the preferred mode are skipped: the flag says
// what the user asked for, and quietly exceeding it would be a
// different kind of wrong from quietly undershooting it.
int display_mode_candidate(int index, int *out_w, int *out_h);

// The standard sizes themselves, largest first, for a modesetting
// driver's mode list: returns 1 while `index` names one. A driver
// lists the entries it accepts, so System Settings offers only modes
// the adapter can show.
int display_ladder_mode(int index, int *out_w, int *out_h);

// What the monitor said about itself, parsed, or NULL when the active
// driver read none. Read at probe, never re-read: a hotplug is a later
// milestone. The preferred timing is timing[0].
struct display_edid;
const struct display_edid *display_edid(void);

// The scaling policy (enum display_scaling), kept by the display layer
// so a driver's set_mode can read it and a setting can store it before
// any mode is set. display_set_scaling() stores it and, with the
// capability, asks the driver to re-place the current mode; 1 when
// stored (the driver's refusal is logged, not returned).
int  display_scaling(void);
int  display_set_scaling(int mode);

// Re-applies write-combining to the ACTIVE surface -- after a mode
// change, when the address or the extent has moved.
void display_refresh_write_combining(void);

#endif
