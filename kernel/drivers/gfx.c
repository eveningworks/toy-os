#include "gfx.h"
#include "multiboot.h"
#include "font_ttf.h"
#include <stddef.h>

static uint8_t *fb = 0;
static uint32_t pitch = 0;
static int width = 0;
static int height = 0;
static uint8_t bpp = 0;
static uint8_t red_pos, red_size, green_pos, green_size, blue_pos, blue_size;

// Which of font_ttf.h's three baked sizes gfx_draw_char() currently
// uses. FONT_SIZE_SMALL is the default -- see CHANGELOG for why (the
// original 16x32 "medium" size, this project's second font iteration,
// read as a bit large once real windows/taskbar text was on screen).
static enum font_size cur_font_size = FONT_SIZE_SMALL;

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

void gfx_put_pixel(int x, int y, uint32_t color) {
    if (x < 0 || y < 0 || x >= width || y >= height) return;
    if (double_buffered) {
        back_buffer[(uint32_t)y * (uint32_t)width + (uint32_t)x] = color;
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
    return 1;
}

void gfx_present(void) {
    if (!double_buffered) return;
    int bytes = bpp / 8;
    for (int y = 0; y < height; y++) {
        const uint32_t *src = back_buffer + (uint32_t)y * (uint32_t)width;
        uint8_t *dst = fb + (uint32_t)y * pitch;
        for (int x = 0; x < width; x++) {
            uint32_t c = src[x];
            dst[0] = (uint8_t)(c & 0xFF);
            dst[1] = (uint8_t)((c >> 8) & 0xFF);
            dst[2] = (uint8_t)((c >> 16) & 0xFF);
            dst += bytes;
        }
    }
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

// Draws one glyph from the baked TrueType-derived font (see font_ttf.h /
// tools/genttf.py): each pixel is an 8-bit alpha (0 = pure background,
// 255 = pure foreground, anything between blended per-channel), unlike
// the old 8x8 font's 1-bit-then-nearest-neighbor-upscale approach. This
// is the actual "sharper" part -- the anti-aliasing was baked in offline
// by a real font rasterizer (FreeType, via Pillow) at full glyph
// resolution, not synthesized here; this function's whole job is just to
// alpha-composite the already-antialiased glyph onto whatever fg/bg pair
// the caller wants.
void gfx_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg) {
    if (c < 32 || c > 126) c = '?';
    const struct font_ttf_variant *fv = &font_ttf_variants[cur_font_size];
    const unsigned char *glyph = fv->glyphs + (size_t)(c - 32) * (size_t)fv->w * (size_t)fv->h;

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
