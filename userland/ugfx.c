// ugfx -- see ugfx.h for what this is and why the font arrives the way
// it does.
#include "ugfx.h"
#include "syscall_abi.h"

static inline int64_t syscall2(uint64_t num, uint64_t arg1, uint64_t arg2) {
    int64_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2)
        : "memory"
    );
    return ret;
}

// Font state, filled by ugfx_font_init(). Zero until then, which makes
// a forgotten init draw nothing rather than dereference a wild pointer
// -- see ugfx.h.
static const unsigned char *g_glyphs = 0;
static int g_char_w = 0;
static int g_char_h = 0;
static int g_glyph_count = 0;

struct ugfx_surface ugfx_surface_for_window(uint32_t window, int w, int h) {
    struct ugfx_surface s;
    s.pixels = (uint32_t *)(uintptr_t)win_buffer_vaddr(window);
    s.w = w;
    s.h = h;
    return s;
}

void ugfx_fill_rect(struct ugfx_surface *s, int x, int y, int w, int h, uint32_t color) {
    if (!s || !s->pixels) return;
    // Clip rather than trust the caller: a client drawing outside its
    // own buffer would be corrupting whatever the allocator put after
    // it, and a bounds mistake is far easier to make in a client than
    // in the WM (the client computes its own layout with no help).
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0) return;

    for (int j = 0; j < h; j++) {
        uint32_t *row = s->pixels + (uint32_t)(y + j) * (uint32_t)s->w + (uint32_t)x;
        for (int i = 0; i < w; i++) row[i] = color;
    }
}

void ugfx_fill(struct ugfx_surface *s, uint32_t color) {
    if (!s) return;
    ugfx_fill_rect(s, 0, 0, s->w, s->h, color);
}

void ugfx_draw_rect(struct ugfx_surface *s, int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    ugfx_fill_rect(s, x, y, w, 1, color);
    ugfx_fill_rect(s, x, y + h - 1, w, 1, color);
    ugfx_fill_rect(s, x, y, 1, h, color);
    ugfx_fill_rect(s, x + w - 1, y, 1, h, color);
}

int ugfx_font_init(void) {
    struct win_request_msg req;
    for (unsigned i = 0; i < sizeof(req); i++) ((uint8_t *)&req)[i] = 0;
    req.type = WIN_REQ_FONT;
    if (syscall2(SYS_WIN_REQUEST, (uint64_t)(uintptr_t)&req, 0) != 1) return 0;

    g_char_w = req.a;
    g_char_h = req.b;
    g_glyph_count = req.c;
    // Glyph 0 sits at the mapping's base PLUS the data's offset within
    // its first page -- the tables are ordinary kernel .rodata and do
    // not start on a page boundary. Ignoring `d` here would shift every
    // glyph by a few bytes and render convincing-looking garbage.
    g_glyphs = (const unsigned char *)(uintptr_t)(WIN_FONT_VADDR + (uint32_t)req.d);
    return 1;
}

int ugfx_char_w(void) { return g_char_w; }
int ugfx_char_h(void) { return g_char_h; }

int ugfx_text_width(const char *str) {
    if (!str) return 0;
    int n = 0;
    while (str[n]) n++;
    return n * g_char_w;
}

// Character -> glyph slot. The shared table is ASCII 32..126 laid out
// contiguously from index 0; anything outside that draws as a space,
// which is the quiet-degradation choice (a client rendering a stray
// byte should look wrong, not read out of bounds).
static int glyph_index(unsigned char c) {
    int idx = (int)c - WIN_FONT_FIRST_CHAR;
    if (idx < 0 || idx >= g_glyph_count) return 0;
    return idx;
}

// Blends `fg` over `bg` by `alpha` (0..255), per channel. The glyph
// tables are coverage maps, not masks -- that is what makes this text
// anti-aliased rather than jagged, and it's why a plain "if (a > 128)"
// threshold would visibly degrade it.
static uint32_t blend(uint32_t fg, uint32_t bg, unsigned alpha) {
    unsigned fr = (fg >> 16) & 0xFF, fg_ = (fg >> 8) & 0xFF, fb = fg & 0xFF;
    unsigned br = (bg >> 16) & 0xFF, bg_ = (bg >> 8) & 0xFF, bb = bg & 0xFF;
    unsigned r = (fr * alpha + br * (255 - alpha)) / 255;
    unsigned g = (fg_ * alpha + bg_ * (255 - alpha)) / 255;
    unsigned b = (fb * alpha + bb * (255 - alpha)) / 255;
    return (r << 16) | (g << 8) | b;
}

void ugfx_draw_string(struct ugfx_surface *s, int x, int y,
                       const char *str, uint32_t color, uint32_t bg) {
    if (!s || !s->pixels || !str || !g_glyphs) return;

    for (int n = 0; str[n]; n++) {
        int gx = x + n * g_char_w;
        if (gx >= s->w) break;      // the rest is off the right edge
        if (gx + g_char_w <= 0) continue;

        const unsigned char *glyph =
            g_glyphs + win_glyph_offset((uint32_t)glyph_index((unsigned char)str[n]),
                                         g_char_w, g_char_h);

        for (int row = 0; row < g_char_h; row++) {
            int py = y + row;
            if (py < 0 || py >= s->h) continue;
            for (int col = 0; col < g_char_w; col++) {
                int px = gx + col;
                if (px < 0 || px >= s->w) continue;
                unsigned a = glyph[row * g_char_w + col];
                if (!a) continue; // fully background -- leave it alone
                uint32_t *p = &s->pixels[(uint32_t)py * (uint32_t)s->w + (uint32_t)px];
                *p = (a == 255) ? color : blend(color, bg, a);
            }
        }
    }
}
