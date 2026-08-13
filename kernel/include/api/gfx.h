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

// Replaces the surface gfx_init() found with a different one -- used by
// a display driver that takes the adapter over after boot (see
// kernel/drivers/vmsvga.c). `addr` must already be mapped; QEMU's
// display BARs sit under 4GiB, which this kernel identity-maps.
// Returns 1 if accepted, 0 if the geometry/depth is unusable, in which
// case the previous surface is left untouched.
//
// Callers must repaint afterwards -- nothing here preserves the old
// contents, and the console's cell grid needs vga_reflow().
int gfx_adopt_framebuffer(uint64_t addr, uint32_t pitch, uint32_t w, uint32_t h, uint8_t bpp);

// Publishes everything drawn since the last flush to the display.
//
// A no-op on an ordinary framebuffer, where the adapter scans memory
// continuously and drawing IS showing. It exists because that is NOT
// universally true: a driver-owned mode (see kernel/drivers/vmsvga.c)
// only shows a region once the guest names it, so without this the
// screen simply stops updating. Called for you by gfx_present() and by
// the console's own draw paths; a caller drawing straight to the
// framebuffer outside both should call it when finished.
void gfx_flush(void);

// --- hardware cursor -------------------------------------------------
//
// A cursor the DISPLAY ADAPTER composites, not something drawn into the
// framebuffer. When available it makes cursor motion free: no sprite
// blit, no save/restore of the pixels underneath, no damage rect, no
// repaint of any kind -- the WM just tells it where to be.
//
// Only some adapters have one. Plain VGA (`-vga std`, and any real
// machine without a GPU driver) has none at all, so the WM keeps its
// software sprite and uses this only when gfx_hw_cursor_available()
// says so. Check first; the rest are no-ops otherwise.
int gfx_hw_cursor_available(void);

// Uploads a 32-bit ARGB image, with the click point at (hot_x, hot_y).
// Returns 1 on success.
int gfx_hw_cursor_define(const uint32_t *argb, int w, int h, int hot_x, int hot_y);

void gfx_hw_cursor_move(int x, int y);
void gfx_hw_cursor_show(int on);

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

// Blends `over` onto `under` at `alpha`/255 (0 = all under, 255 = all
// over). Packed-pixel in, packed-pixel out -- the channel layout is
// gfx.c's private business, which is exactly why this can't live in a
// caller. Added for the console's translucent cursor (vga.c), which
// tints the cell it sits on instead of covering the character.
uint32_t gfx_blend(uint32_t under, uint32_t over, uint8_t alpha);

// Perceived brightness of a packed pixel, 0..255. For callers deciding
// whether to shift a colour lighter or darker -- see apps/ui/'s
// ui_state_bg(), which cannot lighten an already-light control and has
// to know which way to go.
uint8_t gfx_luminance(uint32_t color);

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

// --- measuring and clipping text ------------------------------------
//
// gfx_draw_string() above deliberately draws every character it is
// given, past any boundary the caller had in mind (see
// docs/decisions.md). These are what a caller drawing into a fixed box
// should use instead of budgeting the width by hand -- which two
// separate callers got wrong, the second of them AFTER the first was
// written up as a lesson.

// Pixel width of one row of `s` (stops at a newline). Ask this instead
// of `k_strlen(s) * gfx_char_w()`: that identity holds only while every
// glyph is one fixed cell wide, and Milestone 21's proportional metrics
// are where it stops holding.
int gfx_text_width(const char *s);

// How many leading characters of `s` fit within `max_w` pixels, whole
// glyphs only. For callers doing their own windowing (a text field
// scrolling to follow its cursor) that need the count, not the drawing.
int gfx_text_fit_chars(const char *s, int max_w);

// gfx_draw_string() bounded to `max_w` pixels. Returns 1 if the whole
// string fitted, 0 if it was cut -- so a caller can add an ellipsis or
// widen itself without measuring twice.
int gfx_draw_string_clipped(int x, int y, int max_w, const char *s,
                             uint32_t fg, uint32_t bg);

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
