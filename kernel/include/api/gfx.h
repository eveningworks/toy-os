#ifndef GFX_H
#define GFX_H

#include <stdint.h>
#include "font_ttf.h"

// On-screen size of one character cell -- of WHICHEVER of the four
// baked font sizes (font_ttf.h) is currently selected via
// gfx_set_font_size(). These used to be compile-time constants; now
// that font size can change at runtime (see the shell's `fontsize`
// command), every caller that used to hardcode FONT_CHAR_W/H must call
// gfx_char_w()/gfx_char_h() instead and re-derive any layout that
// depends on them each time it draws, rather than caching the value.
int gfx_char_w(void);
int gfx_char_h(void);

// Selects one of the point sizes in enum font_size (font_ttf.h --
// FONT_SIZE_8 .. FONT_SIZE_24 as of build 347). Returns 1 on
// success, 0 for an out-of-range index. Takes effect on the very next
// gfx_draw_char()/gfx_draw_string() call -- there's no cached state
// anywhere in gfx.c itself, but callers that cache pixel layout (vga.c's
// console_cols/console_rows) need to recompute it; see vga_reflow().
int gfx_set_font_size(enum font_size size);
enum font_size gfx_font_size(void);
const char *gfx_font_size_name(enum font_size size);

// Returns 1 on success (a usable RGB framebuffer was found), 0 otherwise.
int gfx_init(void);

int gfx_width(void);
int gfx_height(void);

// Physical address, pitch (bytes per row), and bits-per-pixel of the
// real linear framebuffer -- used by the experimental userspace-GUI
// syscalls (SYS_GUI_INIT, see syscall.c) to map real screen memory
// directly into a ring-3 process, so it can draw without going through
// kernel-space code at all. Only meaningful after a successful
// gfx_init(). Since the kernel identity-maps its own memory (no
// higher-half split), this physical address is numerically the same
// value gfx.c's own internal pointer uses.
uint64_t gfx_framebuffer_phys(void);
uint32_t gfx_framebuffer_pitch(void);
uint32_t gfx_framebuffer_bpp(void);

// Pack 8-bit r/g/b into whatever pixel format the framebuffer actually uses.
uint32_t gfx_rgb(uint8_t r, uint8_t g, uint8_t b);

// Restricts gfx_put_pixel() (and therefore every drawing primitive
// below, all of which bottom out at it) to writing only within
// [x, x+w) x [y, y+h) -- on top of the plain screen-bounds check it
// already does. gfx_get_pixel() is deliberately NOT clipped (see
// gfx.c's comment) -- a caller reading existing content, e.g. to save
// pixels before drawing over them, still wants the real content
// regardless of the active clip. w/h <= 0 is treated the same as
// gfx_clear_clip_rect() (nothing draws). First real caller: the window
// manager's damage-region compositor (apps/wm/wm_render.c, see
// docs/decisions.md) -- restricts a repaint to the screen region that
// actually needs it instead of always touching everything.
void gfx_set_clip_rect(int x, int y, int w, int h);

// Removes any active clip -- drawing reaches the full screen again
// (still gated by the ordinary screen-bounds check). The default state
// before any gfx_set_clip_rect() call.
void gfx_clear_clip_rect(void);

void gfx_put_pixel(int x, int y, uint32_t color);
uint32_t gfx_get_pixel(int x, int y);
// Alpha-blends `color` over whatever's currently at (x, y) -- see this
// function's own comment in gfx.c for the per-channel math and its
// first real caller (the mouse cursor sprite).
void gfx_blend_pixel(int x, int y, uint32_t color, uint8_t alpha);
void gfx_fill_rect(int x, int y, int w, int h, uint32_t color);
void gfx_draw_rect(int x, int y, int w, int h, uint32_t color);
void gfx_clear(uint32_t color);
void gfx_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg);
void gfx_draw_string(int x, int y, const char *s, uint32_t fg, uint32_t bg);

// Shifts the whole framebuffer content up by `pixel_rows` pixels (for a
// scrolling text console) and fills the newly exposed bottom strip with
// bg_color.
void gfx_scroll_up(int pixel_rows, uint32_t bg_color);

// Double buffering: when enabled, all drawing goes to an off-screen
// buffer and only becomes visible on gfx_present(). This is what stops
// the window manager from flickering -- without it you're watching the
// screen get cleared and repainted piece by piece.
//
// The console (vga.c) leaves this OFF, so its writes go straight to the
// display and appear immediately, exactly as before.
//
// Returns 1 if double buffering could be enabled (the back buffer is a
// fixed-size static array, so a mode larger than GFX_MAX_PIXELS can't be
// buffered), 0 otherwise -- callers can still draw either way.
int gfx_set_double_buffered(int enabled);

// Copies the back buffer to the visible framebuffer. No-op when double
// buffering is off.
void gfx_present(void);

#endif
