#ifndef GFX_H
#define GFX_H

#include <stdint.h>
#include "geom.h" // enum geom_aa, shared with the userland side
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

// --- damage verification (debug) --------------------------------------
//
// Support for catching the one bug class the WM's damage compositor can
// produce: something changed on screen that was never declared as
// damage, so it is drawn once and then never corrected. There is no
// crash and no wrong return value -- just stale pixels, often only in
// one specific interaction. Every rendering bug this project has had
// was of that shape.
//
// The check is: render a frame the normal (damage-limited) way, snapshot
// it, render the SAME frame with no damage limit, and compare. Any
// difference is a pixel the damage-limited path got wrong. See
// wm_render.c's verify mode, which drives these.
//
// Snapshots the back buffer. Returns 0 if double buffering is off (there
// is nothing to compare) or the scratch buffer can't be allocated.
int gfx_verify_snapshot(void);

// What a comparison found. The BOUNDING BOX matters as much as the
// count: 63 differing pixels are a caret, a window border or a whole
// scrollbar depending on their extent, and the first pixel's
// coordinates alone can't tell those apart.
struct gfx_diff {
    int count;              // differing pixels; 0 means the renders agreed
    int first_x, first_y;   // the first differing pixel, scanning top-down
    int x0, y0, x1, y1;     // half-open bounding box of every difference
};

// Compares the current back buffer against the last snapshot, filling
// *out. A count of 0 means the two renders agreed -- i.e. the damage
// rect covered everything that changed. Returns the count too, so a
// caller can branch on it directly.
int gfx_verify_diff(struct gfx_diff *out);

// Frees the scratch buffer.
void gfx_verify_release(void);

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
// regardless of the active clip. w/h <= 0 sets an EMPTY clip: nothing
// draws at all until the clip is changed or cleared. (It is NOT the
// same as gfx_clear_clip_rect(), which makes the whole screen drawable
// -- this doc used to conflate the two, the implementation followed
// the wrong half, and a compositor caller that computed an empty
// window-content/damage intersection got that window's entire
// on_draw() painted unclipped. See gfx.c's comment in
// gfx_set_clip_rect() itself.) First real caller: the window
// manager's damage-region compositor (userland/wm/wm_render.c, see
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

// Copies a w*h block of 32bpp pixels to (x, y). `src_pitch_px` is the
// source's row stride in PIXELS (not bytes), so a caller can blit a
// sub-rectangle out of a wider buffer. Goes through gfx_put_pixel(), so
// it honours the clip rect and damage tracking like every other
// primitive here. Used to composite a ring-3 client's window buffer --
// see kernel/proc/win_server.c.
void gfx_blit(int x, int y, int w, int h, const uint32_t *src, int src_pitch_px);
void gfx_draw_rect(int x, int y, int w, int h, uint32_t color);

// --- geometry ---------------------------------------------------------
//
// Thin bindings over kernel/lib/geom.c, which is SHARED with ring-3
// clients (userland/ugfx.h has the same set) -- one Bresenham and one
// ellipse rasteriser in the tree, not two. See geom.h.
//
// `aa` picks anti-aliased or hard-edged per call, because the right
// answer differs within one drawing: a wireframe's diagonals want AA,
// a 1px window border does not.
void gfx_draw_line(int x0, int y0, int x1, int y1, uint32_t color, enum geom_aa aa);
void gfx_draw_polyline(const int *xs, const int *ys, int count, int closed,
                        uint32_t color, enum geom_aa aa);
void gfx_draw_circle(int cx, int cy, int r, uint32_t color, enum geom_aa aa);
void gfx_draw_ellipse(int cx, int cy, int rx, int ry, uint32_t color, enum geom_aa aa);
void gfx_fill_circle(int cx, int cy, int r, uint32_t color);
void gfx_fill_ellipse(int cx, int cy, int rx, int ry, uint32_t color);
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

// --- the rest of the text chokepoints ---------------------------------
//
// Together with gfx_text_width()/gfx_text_fit_chars() above, these are
// every question a caller can ask about where characters SIT. The rule
// they exist to enforce, and the reason they are grouped here:
//
//   **No widget does character arithmetic on a string itself.** Not
//   `k_strlen(s) * gfx_char_w()` to measure it, not `i + 1` to step a
//   cursor. Every one of those is a place that has to change when text
//   stops being one byte per fixed-width cell -- and there are enough of
//   them, spread across enough widgets, that finding them all later is
//   the hard part. Routed through here, that migration is these four
//   functions plus the glyph lookup.
//
// This is NOT a ban on gfx_char_w(). Using it as a LAYOUT UNIT is fine
// and stays -- a scrollbar `gfx_char_w() + 4` wide, padding of
// `gfx_char_w() / 2`, a grid of `w / gfx_char_w()` columns. Those are
// "size this proportionally to the font", which remains meaningful
// whatever the font does. The banned pattern is specifically measuring
// or indexing A STRING with it.
//
// Milestone 37 (UTF-8) and Milestone 21 (proportional metrics) are the
// two changes this is aimed at; see docs/uapp-design.md's "Text, and
// Unicode later", including the two things that will still hurt
// regardless (the ASCII-contiguous glyph lookup, and
// ui_scrollback.c's one-byte cells).

// Step one character forward/back from index `i` in `s`, clamped to
// [0, length]. Trivial today (i+1 / i-1) and deliberately still a
// function: these are the two places a multi-byte encoding has to skip
// a whole sequence rather than a byte, and a widget that wrote `++`
// inline is a widget that will be wrong then.
int gfx_text_next(const char *s, int i);
int gfx_text_prev(const char *s, int i);

// DELIBERATELY ABSENT: a `gfx_text_index_at_x()` turning a click offset
// into a character index. It belongs in this set and will be needed the
// day a caret can be placed by clicking -- but nothing does that today
// (clicking a ui_textbox only focuses it), and the wrapped-buffer
// callers that look like they want it, ui_scrollback and utext, are
// asking a different question: which COLUMN of a wrapped grid, not
// which index of a string. Added when a real caller turns up, per
// CLAUDE.md's bar -- the same rule that had k_strstr written, unused,
// and deleted again before it landed.

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

// Times `iterations` full-screen fills of the VISIBLE framebuffer and
// returns the total in TSC cycles. Deliberately bypasses the back buffer
// and the dirty-rect machinery: what it measures is the one thing that
// makes drawing slow on real hardware -- the cost of getting bytes
// across the bus into an uncached-or-not MMIO aperture.
//
// It leaves the screen filled with `color`, which is the caller's
// problem (the `gfxbench` command fills with the console background, so
// it reads as a clear). Meaningless under QEMU, where the framebuffer is
// cached host RAM; the number only says something on real hardware.
uint64_t gfx_bench_fill(uint32_t color, int iterations);

// Times a BURST of `iterations` scrolls of `pixel_rows` each, followed
// by one present -- the shape a run of console output actually has.
// Returns elapsed TSC cycles, 0 if the arguments are unusable.
//
// This is the console's other half, and the two modes put the cost in
// completely different places: drawing straight at the display, every
// scroll shifts the visible pixels in place and so READS the whole
// framebuffer, which write-combining makes worse rather than better;
// double-buffered, every scroll is a RAM memmove and only the single
// present touches the display at all. Measuring the burst rather than
// one scroll is what makes those comparable.
//
// Like gfx_bench_fill(), the number is only meaningful on a real
// machine or under `make run-kvm`: plain QEMU's TCG ignores guest
// memory types entirely, so the expensive case simply does not happen
// there. See paging.h.
uint64_t gfx_bench_scroll(int pixel_rows, int iterations);

// Whether drawing currently goes to the back buffer rather than
// straight at the display. Reported by `gfxbench`, since it decides
// which of the two cost models above applies.
int gfx_double_buffered(void);

// Bits per pixel of the active surface (32 or 24), and a name for the
// mechanism that made its framebuffer write-combining ("PAT", "MTRR" or
// "none (uncached)").
//
// Both exist so `gfxbench` can report them WITHOUT reaching into
// display.h or paging.h, which are kernel-internal and off the apps/
// include path on purpose. A shell command wanting a kernel fact gets a
// function here; it does not get to include its way around the
// boundary.
int gfx_bpp(void);
const char *gfx_write_combining_name(void);


#endif
