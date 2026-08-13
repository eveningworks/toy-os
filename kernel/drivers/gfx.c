#include "gfx.h"
#include "vmsvga.h"
#include "multiboot.h"
#include "font_ttf.h"
#include <stddef.h>

static uint8_t *fb = 0;
static uint32_t pitch = 0;
static int width = 0;
static int height = 0;
static uint8_t bpp = 0;
static uint8_t red_pos, red_size, green_pos, green_size, blue_pos, blue_size;

// Which of font_ttf.h's eight baked sizes gfx_draw_char() currently
// uses. FONT_SIZE_18 (10x21 cell) is the default -- closest in on-screen
// area to the pre-build-347 default (the old named "small" size, 11x22),
// which itself was picked over the original 16x32 "medium" for reading
// as a bit large once real windows/taskbar text was on screen (see
// CHANGELOG). See CHANGELOG's build 347 entry for the tiny/small/medium/
// large -> numeric point size rename this replaced.
static enum font_size cur_font_size = FONT_SIZE_18;

int gfx_char_w(void) { return font_ttf_variants[cur_font_size].w; }
int gfx_char_h(void) { return font_ttf_variants[cur_font_size].h; }

int gfx_set_font_size(enum font_size size) {
    if (size < 0 || size >= FONT_SIZE_COUNT) return 0;
    cur_font_size = size;
    return 1;
}

enum font_size gfx_font_size(void) { return cur_font_size; }

const char *gfx_font_size_name(enum font_size size) {
    if (size < 0 || size >= FONT_SIZE_COUNT) return "?";
    return font_ttf_variants[size].name;
}

// --- double buffering ---
// Sized for the largest mode we're willing to buffer. 1920x1080 gives
// headroom above the 1280x720 mode boot.asm actually requests (in case
// GRUB/QEMU picks something bigger than the preference), at 4
// bytes/pixel that's ~8 MiB of .bss, which is fine given we
// identity-map the first 1 GiB and the kernel loads at 1 MiB.
#define GFX_MAX_PIXELS (1920 * 1080)
static uint32_t back_buffer[GFX_MAX_PIXELS];
static int double_buffered = 0;

// --- dirty-rectangle tracking ---
// A single bounding box (not a real dirty-rect list -- see gfx_present()'s
// comment) of every back-buffer pixel touched since the last gfx_present().
// [dirty_x0,dirty_x1) x [dirty_y0,dirty_y1); dirty_x1 <= dirty_x0 means
// "empty" (nothing to blit), which is also the reset state after each
// present. Tracked at the gfx_put_pixel() level -- the one place every
// drawing primitive in this file (fill_rect, draw_rect, draw_char,
// draw_string, clear) bottoms out at -- so callers never need to mark
// anything dirty themselves; it falls out of whatever they actually drew.
static int dirty_x0, dirty_y0, dirty_x1, dirty_y1;

static inline void dirty_mark(int x, int y) {
    if (dirty_x1 <= dirty_x0) { // was empty
        dirty_x0 = x; dirty_x1 = x + 1;
        dirty_y0 = y; dirty_y1 = y + 1;
        return;
    }
    if (x < dirty_x0) dirty_x0 = x;
    if (x + 1 > dirty_x1) dirty_x1 = x + 1;
    if (y < dirty_y0) dirty_y0 = y;
    if (y + 1 > dirty_y1) dirty_y1 = y + 1;
}

// Marks a whole rectangle dirty directly, for the rare code path that
// writes into the back buffer without going through gfx_put_pixel (see
// gfx_scroll_up()'s double-buffered branch, which memmoves rows).
static void dirty_mark_rect(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    dirty_mark(x, y);
    dirty_mark(x + w - 1, y + h - 1);
}

// --- clip rect ---
// Separate from the dirty-rect tracking above -- that tracks what WAS
// touched, after the fact, purely for gfx_present()'s own blit; this
// restricts what CAN be touched, before drawing happens. Not active by
// default (clip_active == 0 means the full screen, same as no clip
// rect ever having been set). See gfx_set_clip_rect()'s doc comment in
// gfx.h for the first real caller (apps/wm/wm_render.c's damage-region
// compositor) -- clipping writes this way means the dirty-rect box
// above only ever grows to cover the active clip, not whatever a full
// unclipped repaint would have touched, which is the whole point.
static int clip_x0, clip_y0, clip_x1, clip_y1;
static int clip_active = 0;

void gfx_set_clip_rect(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) { clip_active = 0; return; }
    clip_x0 = x; clip_y0 = y;
    clip_x1 = x + w; clip_y1 = y + h;
    clip_active = 1;
}

void gfx_clear_clip_rect(void) {
    clip_active = 0;
}

int gfx_adopt_framebuffer(uint64_t addr, uint32_t new_pitch, uint32_t w, uint32_t h,
                           uint8_t new_bpp) {
    if (!addr || !new_pitch || !w || !h) return 0;
    if (new_bpp != 32 && new_bpp != 24) return 0;

    fb = (uint8_t *)(uintptr_t)addr;
    pitch = new_pitch;
    width = (int)w;
    height = (int)h;
    bpp = new_bpp;
    // A driver-set mode is plain little-endian ARGB/XRGB; the masks
    // multiboot reported for GRUB's mode don't describe it.
    red_pos = 16; red_size = 8;
    green_pos = 8; green_size = 8;
    blue_pos = 0; blue_size = 8;
    return 1;
}

int gfx_hw_cursor_available(void) { return vmsvga_cursor_available(); }

int gfx_hw_cursor_define(const uint32_t *argb, int w, int h, int hot_x, int hot_y) {
    return vmsvga_cursor_define(argb, w, h, hot_x, hot_y);
}

void gfx_hw_cursor_move(int x, int y) { vmsvga_cursor_move(x, y); }
void gfx_hw_cursor_show(int on) { vmsvga_cursor_show(on); }

int gfx_init(void) {
    struct framebuffer_info info;
    if (!multiboot_get_framebuffer(&info)) return 0;
    if (info.type != 1) return 0; // only direct RGB framebuffers supported
    if (info.bpp != 32 && info.bpp != 24) return 0;

    fb = (uint8_t *)(uintptr_t)info.addr;
    pitch = info.pitch;
    width = (int)info.width;
    height = (int)info.height;
    bpp = info.bpp;
    double_buffered = 0;
    red_pos = info.red_pos; red_size = info.red_size;
    green_pos = info.green_pos; green_size = info.green_size;
    blue_pos = info.blue_pos; blue_size = info.blue_size;

    return 1;
}

int gfx_width(void) { return width; }
int gfx_height(void) { return height; }

uint64_t gfx_framebuffer_phys(void) { return (uint64_t)(uintptr_t)fb; }
uint32_t gfx_framebuffer_pitch(void) { return pitch; }
uint32_t gfx_framebuffer_bpp(void) { return bpp; }

uint32_t gfx_rgb(uint8_t r, uint8_t g, uint8_t b) {
    uint32_t rr = (uint32_t)(r >> (8 - red_size)) << red_pos;
    uint32_t gg = (uint32_t)(g >> (8 - green_size)) << green_pos;
    uint32_t bb = (uint32_t)(b >> (8 - blue_size)) << blue_pos;
    return rr | gg | bb;
}

// Blends `over` onto `under` at `alpha`/255 and returns the result.
//
// Lives here rather than in a caller because the channel positions and
// widths are this file's private business (see gfx_rgb() above) -- a
// caller holding a packed pixel has no portable way to take it apart.
// Each channel is extracted, mixed, and repacked at its own width, so
// this is correct on a 16-bit framebuffer as well as the usual 32-bit
// one.
uint32_t gfx_blend(uint32_t under, uint32_t over, uint8_t alpha) {
    uint32_t out = 0;
    const uint8_t pos[3] = { red_pos, green_pos, blue_pos };
    const uint8_t size[3] = { red_size, green_size, blue_size };
    for (int i = 0; i < 3; i++) {
        uint32_t mask = (size[i] >= 32) ? 0xFFFFFFFFu : ((1u << size[i]) - 1u);
        uint32_t u = (under >> pos[i]) & mask;
        uint32_t o = (over >> pos[i]) & mask;
        uint32_t v = (u * (255u - alpha) + o * alpha) / 255u;
        out |= (v & mask) << pos[i];
    }
    return out;
}

// Perceived brightness of a packed pixel, 0 (black) to 255 (white).
// Lives here for the same reason gfx_blend() does: the channel
// positions and widths are this file's private business, so a caller
// holding a packed colour has no portable way to take it apart.
//
// The weights are the standard luma coefficients (~0.299R + 0.587G +
// 0.114B) -- green dominates perceived brightness, which matters here
// because the alternative (a plain average) calls a saturated red and a
// mid grey equally bright and would pick the wrong direction for the
// title bar's close button.
uint8_t gfx_luminance(uint32_t color) {
    const uint8_t pos[3] = { red_pos, green_pos, blue_pos };
    const uint8_t size[3] = { red_size, green_size, blue_size };
    const uint32_t weight[3] = { 77, 150, 29 }; // /256
    uint32_t total = 0;
    for (int i = 0; i < 3; i++) {
        uint32_t mask = (size[i] >= 32) ? 0xFFFFFFFFu : ((1u << size[i]) - 1u);
        uint32_t v = (color >> pos[i]) & mask;
        // Scale the channel up to 0..255 whatever its native width is.
        uint32_t v8 = (mask == 0) ? 0 : (v * 255u) / mask;
        total += v8 * weight[i];
    }
    return (uint8_t)(total >> 8);
}

void gfx_put_pixel(int x, int y, uint32_t color) {
    if (x < 0 || y < 0 || x >= width || y >= height) return;
    if (clip_active && (x < clip_x0 || x >= clip_x1 || y < clip_y0 || y >= clip_y1)) return;
    if (double_buffered) {
        back_buffer[(uint32_t)y * (uint32_t)width + (uint32_t)x] = color;
        dirty_mark(x, y);
        return;
    }
    uint8_t *p = fb + (uint32_t)y * pitch + (uint32_t)x * (bpp / 8);
    p[0] = (uint8_t)(color & 0xFF);
    p[1] = (uint8_t)((color >> 8) & 0xFF);
    p[2] = (uint8_t)((color >> 16) & 0xFF);
}

uint32_t gfx_get_pixel(int x, int y) {
    if (x < 0 || y < 0 || x >= width || y >= height) return 0;
    if (double_buffered) {
        return back_buffer[(uint32_t)y * (uint32_t)width + (uint32_t)x];
    }
    uint8_t *p = fb + (uint32_t)y * pitch + (uint32_t)x * (bpp / 8);
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

int gfx_set_double_buffered(int enabled) {
    if (!enabled) {
        double_buffered = 0;
        return 1;
    }
    if (width <= 0 || height <= 0) return 0;
    if ((uint32_t)width * (uint32_t)height > GFX_MAX_PIXELS) return 0;
    double_buffered = 1;
    dirty_x0 = dirty_x1 = dirty_y0 = dirty_y1 = 0; // nothing dirty in a freshly (re)enabled buffer yet
    return 1;
}

// Copies only the bounding box of what actually changed since the last
// present -- not the whole screen. This is the actual point of the dirty
// tracking above: gfx_put_pixel() is cheap-ish already (one bounds check,
// one write), but this loop touches real/MMIO framebuffer memory, which is
// the expensive part, and used to do it for all width*height pixels every
// single frame regardless of how much (if anything) changed. The window
// manager (apps/wm/) still draws whole windows/widgets into the back
// buffer when their content actually changes -- this only shrinks the
// final blit, it doesn't make drawing itself region-aware. The one caller
// that gets the full benefit of both halves is the cursor-only-moved case
// (see wm_render_cursor_move() in apps/wm/wm_render.c): a handful of
// pixels touched, a handful of pixels blitted, instead of a full frame.
void gfx_present(void) {
    if (!double_buffered) return;
    if (dirty_x1 <= dirty_x0) return; // nothing touched since the last present
    int bytes = bpp / 8;
    for (int y = dirty_y0; y < dirty_y1; y++) {
        const uint32_t *src = back_buffer + (uint32_t)y * (uint32_t)width + dirty_x0;
        uint8_t *dst = fb + (uint32_t)y * pitch + (uint32_t)dirty_x0 * bytes;
        for (int x = dirty_x0; x < dirty_x1; x++) {
            uint32_t c = *src++;
            dst[0] = (uint8_t)(c & 0xFF);
            dst[1] = (uint8_t)((c >> 8) & 0xFF);
            dst[2] = (uint8_t)((c >> 16) & 0xFF);
            dst += bytes;
        }
    }
    dirty_x0 = dirty_x1 = dirty_y0 = dirty_y1 = 0;
}

void gfx_fill_rect(int x, int y, int w, int h, uint32_t color) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            gfx_put_pixel(x + i, y + j, color);
}

void gfx_draw_rect(int x, int y, int w, int h, uint32_t color) {
    gfx_fill_rect(x, y, w, 1, color);
    gfx_fill_rect(x, y + h - 1, w, 1, color);
    gfx_fill_rect(x, y, 1, h, color);
    gfx_fill_rect(x + w - 1, y, 1, h, color);
}

void gfx_clear(uint32_t color) {
    gfx_fill_rect(0, 0, width, height, color);
}

void gfx_scroll_up(int pixel_rows, uint32_t bg_color) {
    if (pixel_rows <= 0 || pixel_rows >= height) {
        gfx_clear(bg_color);
        return;
    }
    if (double_buffered) {
        for (int y = 0; y < height - pixel_rows; y++) {
            uint32_t *dst = back_buffer + (uint32_t)y * (uint32_t)width;
            const uint32_t *src = back_buffer + (uint32_t)(y + pixel_rows) * (uint32_t)width;
            for (int i = 0; i < width; i++) dst[i] = src[i];
        }
        // The memmove above writes straight into back_buffer, bypassing
        // gfx_put_pixel() -- mark the shifted region dirty by hand so
        // gfx_present() still picks it up. (Not currently reachable with
        // double buffering on -- the console, the only caller, leaves it
        // off -- but this keeps the function correct on its own terms
        // rather than relying on that caller-side invariant.)
        dirty_mark_rect(0, 0, width, height - pixel_rows);
        gfx_fill_rect(0, height - pixel_rows, width, pixel_rows, bg_color);
        return;
    }
    uint32_t row_bytes = (uint32_t)width * (bpp / 8);
    for (int y = 0; y < height - pixel_rows; y++) {
        uint8_t *dst = fb + (uint32_t)y * pitch;
        const uint8_t *src = fb + (uint32_t)(y + pixel_rows) * pitch;
        for (uint32_t i = 0; i < row_bytes; i++) dst[i] = src[i];
    }
    gfx_fill_rect(0, height - pixel_rows, width, pixel_rows, bg_color);
}

// Unpacks one 8-bit channel value (0-255) out of a pixel already packed
// into the framebuffer's native format by gfx_rgb() -- the inverse of
// gfx_rgb()'s own r>>(8-size)<<pos. Needed because alpha-blending a
// glyph has to happen in per-channel 0-255 space; the caller only ever
// hands gfx_draw_char() a pre-packed uint32_t, not the original r/g/b.
static inline uint8_t unpack_channel(uint32_t color, uint8_t pos, uint8_t size) {
    uint32_t mask = (1u << size) - 1;
    uint32_t v = (color >> pos) & mask;
    return size >= 8 ? (uint8_t)(v >> (size - 8)) : (uint8_t)((v * 255) / mask);
}

static inline uint32_t pack_channel(uint8_t v, uint8_t pos, uint8_t size) {
    uint32_t packed = size >= 8 ? ((uint32_t)v >> (8 - size))
                                 : (((uint32_t)v * ((1u << size) - 1)) / 255);
    return packed << pos;
}

static inline uint8_t blend_channel(uint8_t bg_c, uint8_t fg_c, uint8_t alpha) {
    return (uint8_t)(((uint32_t)bg_c * (255 - alpha) + (uint32_t)fg_c * alpha) / 255);
}

// Alpha-blends `color` over whatever's already at (x, y) -- reads it
// back via gfx_get_pixel() first, same per-channel math gfx_draw_char()
// already uses for font anti-aliasing, just exposed as its own function
// for any caller that wants a soft edge without being a glyph. First
// real caller: the mouse cursor sprite (apps/wm/wm_render.c), which
// needed the same "baked alpha mask, blended per-pixel" approach the
// font already uses -- see docs/decisions.md. `alpha` 0 leaves the
// pixel untouched, 255 fully replaces it with `color`.
void gfx_blend_pixel(int x, int y, uint32_t color, uint8_t alpha) {
    if (alpha == 0) return;
    if (alpha == 255) { gfx_put_pixel(x, y, color); return; }
    uint32_t bg = gfx_get_pixel(x, y);
    uint8_t r = blend_channel(unpack_channel(bg, red_pos, red_size), unpack_channel(color, red_pos, red_size), alpha);
    uint8_t g = blend_channel(unpack_channel(bg, green_pos, green_size), unpack_channel(color, green_pos, green_size), alpha);
    uint8_t b = blend_channel(unpack_channel(bg, blue_pos, blue_size), unpack_channel(color, blue_pos, blue_size), alpha);
    gfx_put_pixel(x, y, pack_channel(r, red_pos, red_size) | pack_channel(g, green_pos, green_size) | pack_channel(b, blue_pos, blue_size));
}

// Draws one glyph from the baked TrueType-derived font (see font_ttf.h /
// tools/genttf.py): each pixel is an 8-bit alpha (0 = pure background,
// 255 = pure foreground, anything between blended per-channel), unlike
// the old 8x8 font's 1-bit-then-nearest-neighbor-upscale approach. This
// is the actual "sharper" part -- the anti-aliasing was baked in offline
// by a real font rasterizer (FreeType, via Pillow) at full glyph
// resolution, not synthesized here; this function's whole job is just to
// alpha-composite the already-antialiased glyph onto whatever fg/bg pair
// the caller wants.
// Maps a character to its glyph slot in font_ttf_variants[]: ASCII
// 32-126 map straight to indices 0..94 (c - 32), and the six baked
// Nordic letters (font_ttf_extra_codepoints[], font_ttf.h) map to
// 95..100 by linear scan -- fine at 6 entries, not worth a table for
// this few. Returns -1 for anything else (falls back to '?' below).
// `c` comes in as int rather than char specifically so callers can
// pass an already-widened codepoint (0-255) without the signed-char
// sign-extension landmine documented in docs/decisions.md -- see that
// entry for why char is signed in this build and what it broke before
// callers started passing unsigned char/int through here.
static int font_ttf_glyph_index(int c) {
    if (c >= 32 && c <= 126) return c - 32;
    for (int i = 0; i < FONT_TTF_EXTRA_COUNT; i++) {
        if (font_ttf_extra_codepoints[i] == (unsigned char)c) return FONT_TTF_ASCII_COUNT + i;
    }
    return -1;
}

void gfx_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg) {
    int idx = font_ttf_glyph_index((unsigned char)c);
    if (idx < 0) idx = font_ttf_glyph_index('?');
    const struct font_ttf_variant *fv = &font_ttf_variants[cur_font_size];
    const unsigned char *glyph = fv->glyphs + (size_t)idx * (size_t)fv->w * (size_t)fv->h;

    uint8_t fg_r = unpack_channel(fg, red_pos, red_size);
    uint8_t fg_g = unpack_channel(fg, green_pos, green_size);
    uint8_t fg_b = unpack_channel(fg, blue_pos, blue_size);
    uint8_t bg_r = unpack_channel(bg, red_pos, red_size);
    uint8_t bg_g = unpack_channel(bg, green_pos, green_size);
    uint8_t bg_b = unpack_channel(bg, blue_pos, blue_size);

    for (int row = 0; row < fv->h; row++) {
        for (int col = 0; col < fv->w; col++) {
            uint8_t a = glyph[row * fv->w + col];
            uint32_t color;
            if (a == 0) {
                color = bg;
            } else if (a == 255) {
                color = fg;
            } else {
                uint8_t r = blend_channel(bg_r, fg_r, a);
                uint8_t g = blend_channel(bg_g, fg_g, a);
                uint8_t b = blend_channel(bg_b, fg_b, a);
                color = pack_channel(r, red_pos, red_size)
                      | pack_channel(g, green_pos, green_size)
                      | pack_channel(b, blue_pos, blue_size);
            }
            gfx_put_pixel(x + col, y + row, color);
        }
    }
}

// Pixel width of `s` at the current font. A multiplication today
// (every glyph is one fixed cell wide), a real measurement once
// Milestone 21 lands proportional metrics -- which is exactly why
// callers should ask this rather than writing `k_strlen(s) *
// gfx_char_w()` themselves. Stops at a newline: a multi-line string has
// no single width, and every caller of this is measuring one row.
int gfx_text_width(const char *s) {
    int n = 0;
    while (s[n] && s[n] != '\n') n++;
    return n * gfx_char_w();
}

// How many leading characters of `s` fit within `max_w` pixels --
// whole glyphs only, never a partial one. The measurement half of
// gfx_draw_string_clipped(), separated because callers that do their
// own windowing (a text field scrolling to follow its cursor, see
// apps/ui/ui_textbox.c) need the count without the drawing.
int gfx_text_fit_chars(const char *s, int max_w) {
    int cw = gfx_char_w();
    if (cw <= 0 || max_w < cw) return 0;
    int n = 0;
    int used = 0;
    while (s[n] && s[n] != '\n' && used + cw <= max_w) { used += cw; n++; }
    return n;
}

// gfx_draw_string(), but stopping at `max_w` pixels.
//
// **This exists because the unclipped version is a repeat offender.**
// gfx_draw_string() draws every character it is handed, past any border
// its caller imagined -- documented in docs/decisions.md after a long
// filename drew straight through a text field's edge. That entry told
// callers to budget the width themselves, and the very next fixed-box
// caller (the Control Panel's applet labels) hit the identical bug
// anyway, rendering "Date & TSystem Info". A lesson that gets re-learned
// is a missing function, not a missing reader.
//
// Returns 1 if the whole string fitted, 0 if it was cut -- so a caller
// that wants to show an ellipsis, widen itself, or log can, without
// measuring a second time.
int gfx_draw_string_clipped(int x, int y, int max_w, const char *s,
                             uint32_t fg, uint32_t bg) {
    int n = gfx_text_fit_chars(s, max_w);
    int cw = gfx_char_w();
    for (int i = 0; i < n; i++) gfx_draw_char(x + i * cw, y, s[i], fg, bg);
    return s[n] == '\0' || s[n] == '\n';
}

void gfx_draw_string(int x, int y, const char *s, uint32_t fg, uint32_t bg) {
    int cx = x;
    int cw = gfx_char_w(), ch = gfx_char_h();
    while (*s) {
        if (*s == '\n') {
            cx = x;
            y += ch;
        } else {
            gfx_draw_char(cx, y, *s, fg, bg);
            cx += cw;
        }
        s++;
    }
}
