#include "gfx.h"
#include "geom.h"
#include "display.h"
#include "heap.h"
#include "multiboot.h"
#include "font_ttf.h"
#include "font_face.h" // the runtime /usr/share/fonts atlas, when one is active
#include "knum.h"      // k_parse_u32(), for the baked sizes' numeric names
#include "random_hw.h" // arch_rdtsc(), for gfx_bench_fill()
#include "paging.h"    // paging_wc_name(), for gfx_write_combining_name()
#include "pmm.h"       // pmm_alloc_contiguous(, PMM_ZONE_DMA32), for the back buffer
#include "klog.h"
#include "kfmt.h"   // klog_printf()
#include <stddef.h>

// driver-none: rasterising onto whatever display.c chose

static uint8_t *fb = 0;
static uint32_t pitch = 0;
static int width = 0;
static int height = 0;
static uint8_t bpp = 0;
static uint8_t red_pos, red_size, green_pos, green_size, blue_pos, blue_size;

// Which of font_ttf.h's eight baked sizes gfx_draw_char() currently
// uses. FONT_SIZE_14 (8x17 cell) is the default.
//
// It has come down twice, each time for the same reason: more real UI
// on screen than the previous default was chosen against. The original
// was 16x32 ("medium"), then 11x22 ("small"), then FONT_SIZE_18
// (10x21, closest in area to that "small") at build 347's numeric
// rename. 14 is this step -- with the desktop now carrying eleven icons
// and a Start menu of thirteen rows, 18 spent a lot of vertical space
// on chrome, and the menu grows upward from the taskbar so every row
// added pushes it closer to the top of the screen.
//
// Everything here is font-DERIVED (window sizes come from each app's
// default_size(), the taskbar and title bars from gfx_char_h(), the
// desktop's column pitch from gfx_char_w()), so changing this reflows
// the whole UI rather than clipping it -- that is the property that
// makes a default change a one-liner. What it does NOT reflow is a
// hardcoded pixel constant in a test tool: tools/gui_flow.py's
// TASKBAR_H/ITEM_H are calibrated numbers and need re-measuring, which
// is exactly why DebugConsole.menu_row() (which asks the kernel) is the
// preferred way to locate anything now.
//
// A user can still override this per-machine: `fontsize` in the shell,
// or Control Panel, persisted as font_size in /etc/toyos.conf
// (kernel/lib/font_config.c).
static enum font_size cur_font_size = FONT_SIZE_14;

// The em size everything is actually drawn at, in pixels.
//
// **THIS, NOT cur_font_size, IS THE REAL SETTING NOW.** The eight baked
// sizes are a fallback set; with a face loaded from /usr/share/fonts
// (font_face.h) any size in FONT_PX_MIN..FONT_PX_MAX is rasterized on
// demand, which is the whole point of having a rasterizer -- `fontsize
// 13` and `fontsize 32` for a HiDPI panel are now questions the machine
// can answer rather than sizes somebody has to bake. cur_font_size
// tracks the NEAREST baked size and exists so that (a) the settings
// registry still has a bounded choice list to show and (b) a machine
// with no disk font behaves exactly as it did before.
static int cur_font_px = 14;

// The atlas in use, or NULL when the baked tables are. Everything below
// asks through active_*() rather than reading either source directly,
// which is what keeps the fallback honest: there is one place that
// decides, and it is three lines long.
// WHICH WEIGHT RING 0 IS DRAWING IN, as a graphics-context flag rather
// than a parameter on every text call. That is SelectObject()/LOGFONT's
// model and GTK/Cairo's: a device context carries a font, and drawing
// uses whatever it carries. The cost is the cost of any such flag --
// a caller that sets it and does not restore it changes text somewhere
// unrelated -- which is why gfx_set_bold() RETURNS THE PREVIOUS VALUE,
// so the save/restore idiom is the shortest thing to write.
//
// Ring 3 does not inherit this design: the toolkit has a uui_font
// handle instead, because a widget tree is exactly where an unrestored
// global goes wrong.
static int cur_bold;

// Defined with the rest of the glyph machinery further down; needed
// here by gfx_kern(), which has to sit beside the weight state it reads.
static int font_ttf_glyph_index(int c);

// **RING 0 DRAWS FROM THE BAKED TABLES AND NOTHING ELSE.** There is no
// runtime face here any more: a `.ttf` is untrusted input and parsing it
// moved to /bin/fontd (docs/commands/fontd.md), which serves the
// DESKTOP. The console, the kernel shell and a panic report all have to
// draw before any process exists, so their glyphs are compiled into the
// image -- the same split Windows makes, where the bugcheck screen uses
// a built-in font rather than the font host.
//
// The consequence, stated where it will be noticed: the text console and
// the desktop genuinely render in different typefaces, and `fontface`
// no longer changes this one.
static const struct font_atlas *active_atlas(void) { return 0; }

int gfx_set_bold(int on) {
    int prev = cur_bold;
    cur_bold = on ? 1 : 0;
    return prev;
}

int gfx_bold(void) { return cur_bold; }

// The kerning adjustment between two already-drawn characters, in
// pixels -- 0 for the baked font, for a monospace face, and for any
// pair the face does not kern. `prev` is 0 at the start of a run.
//
// Applied by EVERY text path in this file, which is the property that
// matters: measurement and drawing must agree, or a clipped string is
// cut at a different character from the one it is drawn to.
int gfx_kern(int prev, int c) {
    if (!prev) return 0;
    const struct font_atlas *a = active_atlas();
    if (!a || !a->kern) return 0;
    int l = font_ttf_glyph_index(prev);
    int r = font_ttf_glyph_index(c);
    if (l < 0 || r < 0 || l >= a->count || r >= a->count) return 0;
    return a->kern[l * a->count + r];
}

// A baked size's name IS its point size ("8".."24", see genttf.py), so
// this parses rather than carrying a second table that could drift from
// font_ttf_variants[].
static int baked_px(enum font_size size) {
    uint32_t v = 0;
    if (size < 0 || size >= FONT_SIZE_COUNT) return 14;
    if (!k_parse_u32(font_ttf_variants[size].name, &v)) return 14;
    return (int)v;
}

static enum font_size nearest_baked(int px) {
    enum font_size best = FONT_SIZE_8;
    int best_d = -1;
    for (enum font_size i = 0; i < FONT_SIZE_COUNT; i++) {
        int d = baked_px(i) - px;
        if (d < 0) d = -d;
        if (best_d < 0 || d < best_d) { best_d = d; best = i; }
    }
    return best;
}

int gfx_char_w(void) {
    const struct font_atlas *a = active_atlas();
    return a ? a->cell_w : font_ttf_variants[cur_font_size].w;
}

int gfx_char_h(void) {
    const struct font_atlas *a = active_atlas();
    // THE LINE PITCH, NOT THE BITMAP HEIGHT. A glyph's coverage map is
    // taller than this (font_face.h explains why), and every caller of
    // gfx_char_h() is laying something out rather than indexing glyph
    // bytes -- the one place that indexes uses a->cell_h directly.
    return a ? a->line_h : font_ttf_variants[cur_font_size].h;
}

int gfx_font_px(void) { return cur_font_px; }

// Sets the em size. With a runtime face active this rasterizes a new
// atlas (cached, so going back to a previous size is free); with none
// it snaps to the nearest baked size, which is the honest answer -- a
// machine with no font file cannot produce a 13px glyph out of nothing,
// and silently drawing 12 while reporting 13 would be worse than
// rounding visibly.
//
// Returns 1 if the size took effect. A failed atlas build (see
// font_face_build) leaves the previous size in place and returns 0
// rather than dropping the machine back to the baked font mid-session.
// SNAPS TO A BAKED SIZE, always. An arbitrary pixel size needs a
// rasteriser and ring 0 has none; the desktop still gets any size it
// asks for, because fontd rasterizes it (abi/font_shm.h).
int gfx_set_font_px(int px) {
    if (px < 1 || px > 256) return 0;
    cur_font_size = nearest_baked(px);
    cur_font_px = baked_px(cur_font_size);
    return 1;
}

int gfx_set_font_size(enum font_size size) {
    if (size < 0 || size >= FONT_SIZE_COUNT) return 0;
    return gfx_set_font_px(baked_px(size));
}

enum font_size gfx_font_size(void) { return cur_font_size; }

const char *gfx_font_size_name(enum font_size size) {
    if (size < 0 || size >= FONT_SIZE_COUNT) return "?";
    return font_ttf_variants[size].name;
}

// --- double buffering ---
//
// **Allocated from the frame allocator at gfx_init(), sized to the mode
// that is actually on screen -- it used to be a fixed 1920x1080 array in
// .bss.** A fixed array is the wrong shape here twice over: it costs
// 8 MiB of image on a machine that boots at 640x480 and never needs it,
// and it is a hard ceiling on the display mode that nothing at the
// display layer can see -- a driver that programmed a bigger mode came
// up with a live screen the rasteriser silently declined to
// double-buffer (gfx_set_double_buffered() returns 0), which reads as a
// mysteriously slow console rather than as a size limit.
//
// CONTIGUOUS frames, because this is indexed as one flat array and the
// kernel's identity map is what makes a physical address usable as a
// pointer. That is also why it is allocated at gfx_init(), which runs
// early enough (kernel_main() calls it right after pmm_init(), before
// anything else has taken a frame) that a 33 MiB 4K buffer is still a
// request a pristine allocator can satisfy.
//
// A failure is not fatal and never has been: `back_buffer` stays NULL,
// gfx_set_double_buffered() refuses, and every path falls back to
// drawing straight into the framebuffer -- slower, and correct.
static uint32_t *back_buffer;
static uint32_t back_buffer_pixels;   // capacity, in pixels
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
// gfx.h for the first real caller (userland/wm/wm_render.c's damage-region
// compositor) -- clipping writes this way means the dirty-rect box
// above only ever grows to cover the active clip, not whatever a full
// unclipped repaint would have touched, which is the whole point.
static int clip_x0, clip_y0, clip_x1, clip_y1;
static int clip_active = 0;

void gfx_set_clip_rect(int x, int y, int w, int h) {
    // A non-positive w/h is an EMPTY clip -- every pixel write is
    // rejected -- not "no clip". It used to clear the clip instead
    // (full screen drawable), the exact opposite, while gfx.h described
    // it as "nothing draws": a caller that computed an empty
    // intersection got an app's whole on_draw() painted UNCLIPPED. The
    // way that surfaced: on a clock-tick frame whose damage strip
    // grazed only a window's bottom border row, the window's content
    // repaint escaped the damage clip and buried the resize grip drawn
    // the frame before (tools/damage_sweep.py's standing 20px
    // violation). Clearing is what gfx_clear_clip_rect() is for.
    if (w <= 0 || h <= 0) {
        clip_x0 = clip_y0 = clip_x1 = clip_y1 = 0;
        clip_active = 1;
        return;
    }
    clip_x0 = x; clip_y0 = y;
    clip_x1 = x + w; clip_y1 = y + h;
    clip_active = 1;
}

void gfx_clear_clip_rect(void) {
    clip_active = 0;
}

// (gfx_adopt_framebuffer() is gone. A driver taking the display over
// no longer reaches into gfx to re-point it; it reports its surface
// through display_get_surface() and gfx_init() reads it like any
// other. That back-channel was the shape of the old special case.)


void gfx_flush(void) {
    // Nothing has reached the display yet when there is a back buffer --
    // publishing is gfx_present()'s job, and it calls display_flush()
    // itself. Returning early here is load-bearing rather than an
    // optimisation: this function's last act is to CLEAR the dirty box,
    // which while double-buffered would throw away the record of what
    // still needs presenting. The console draws a character, the
    // throttle correctly decides not to present yet, and a gfx_flush()
    // in between would erase the evidence -- which is exactly what left
    // the whole boot log in RAM with one stray glyph on screen.
    if (double_buffered) return;
    if (dirty_x1 <= dirty_x0) return;   // nothing drawn since last time
    // A no-op on a scanned framebuffer: display_flush() checks the
    // capability itself, so this file never asks which card it's on.
    display_flush(dirty_x0, dirty_y0, dirty_x1 - dirty_x0, dirty_y1 - dirty_y0);
    dirty_x0 = dirty_y0 = dirty_x1 = dirty_y1 = 0;
}

// Heap-allocated rather than another 8MB static: this is debug-only and
// most boots never enable it. Kept across frames so verification costs
// one allocation, not one per frame.
static uint32_t *verify_scratch;

int gfx_verify_snapshot(void) {
    if (!double_buffered) return 0;
    uint32_t n = (uint32_t)width * (uint32_t)height;
    if (!verify_scratch) {
        verify_scratch = (uint32_t *)kmalloc(n * sizeof(uint32_t));
        if (!verify_scratch) return 0;
    }
    for (uint32_t i = 0; i < n; i++) verify_scratch[i] = back_buffer[i];
    return 1;
}

int gfx_verify_diff(struct gfx_diff *out) {
    struct gfx_diff d = { 0, -1, -1, 0, 0, 0, 0 };
    if (!double_buffered || !verify_scratch) {
        if (out) *out = d;
        return 0;
    }
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint32_t i = (uint32_t)y * (uint32_t)width + (uint32_t)x;
            if (back_buffer[i] == verify_scratch[i]) continue;
            if (d.count == 0) {
                d.first_x = x; d.first_y = y;
                d.x0 = x; d.y0 = y; d.x1 = x + 1; d.y1 = y + 1;
            } else {
                if (x < d.x0) d.x0 = x;
                if (x + 1 > d.x1) d.x1 = x + 1;
                if (y + 1 > d.y1) d.y1 = y + 1;  // scan order fixes y0 already
            }
            d.count++;
        }
    }
    if (out) *out = d;
    return d.count;
}

void gfx_verify_release(void) {
    if (verify_scratch) { kfree(verify_scratch); verify_scratch = 0; }
}

int gfx_hw_cursor_available(void) { return display_has(DISPLAY_CAP_CURSOR); }

int gfx_hw_cursor_define(const uint32_t *argb, int w, int h, int hot_x, int hot_y) {
    return display_cursor_define(argb, w, h, hot_x, hot_y);
}

void gfx_hw_cursor_move(int x, int y) { display_cursor_move(x, y); }
void gfx_hw_cursor_show(int on) { display_cursor_show(on); }

int gfx_init(void) {
    // The display layer decides WHICH card; this only asks where the
    // pixels are. gfx.c used to read multiboot itself and later grew a
    // second, bolted-on path for a real driver -- one path now, and
    // adding a card touches nothing in this file.
    struct display_surface surf;
    display_get_surface(&surf);
    if (!surf.addr || !surf.width || !surf.height) return 0;
    if (surf.bpp != 32 && surf.bpp != 24) return 0;

    fb = (uint8_t *)(uintptr_t)surf.addr;
    pitch = surf.pitch;
    width = (int)surf.width;
    height = (int)surf.height;
    bpp = surf.bpp;
    red_pos = 16; red_size = 8;
    green_pos = 8; green_size = 8;
    blue_pos = 0; blue_size = 8;
    double_buffered = 0;
    dirty_x0 = dirty_y0 = dirty_x1 = dirty_y1 = 0;

    // The back buffer, sized to this mode. gfx_remode() calls back in
    // here after a mode change and keeps a buffer that is still big
    // enough (a smaller mode is the common direction to try).
    uint32_t pixels = (uint32_t)width * (uint32_t)height;
    if (back_buffer && back_buffer_pixels < pixels) {
        pmm_free_contiguous((uint64_t)(uintptr_t)back_buffer,
                            ((uint64_t)back_buffer_pixels * 4 + 4095) / 4096);
        back_buffer = 0;
        back_buffer_pixels = 0;
    }
    if (!back_buffer) {
        uint64_t pages = ((uint64_t)pixels * 4 + 4095) / 4096;
        uint64_t phys = pmm_alloc_contiguous(pages, PMM_ZONE_DMA32);
        if (phys) {
            back_buffer = (uint32_t *)(uintptr_t)phys;
            back_buffer_pixels = pixels;
        } else {
            // Not fatal -- see back_buffer's comment. Logged because the
            // symptom (a console that redraws visibly slowly) points
            // nowhere near a failed allocation.
            klog_printf("gfx: no back buffer for %dx%d (%uKB contiguous) -- "
                         "drawing direct to the framebuffer\n",
                         width, height, (uint32_t)(pages * 4));
        }
    }
    return 1;
}


// After the display driver has set a new mode: re-read the surface,
// re-size the back buffer, drop the clip and the dirty box. The
// double-buffer state is kept -- the console's mode is not the screen's.
int gfx_remode(void) {
    int was_double = double_buffered;
    if (!gfx_init()) return 0;
    clip_active = 0;
    if (verify_scratch) { kfree(verify_scratch); verify_scratch = 0; }
    if (was_double) gfx_set_double_buffered(1);
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
    // Marked here too, not only on the double-buffered path above.
    // Dirty tracking used to exist purely for gfx_present()'s blit, so
    // it was pointless when drawing straight to the framebuffer and was
    // skipped. That stopped being true the moment a display could need
    // to be TOLD what changed (gfx_flush()): with this missing, every
    // flush from the console found an empty box, published nothing, and
    // the screen froze on a driver-owned mode while memory held the
    // right pixels the whole time. The box means "what was touched",
    // regardless of where it was written.
    dirty_mark(x, y);
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
    if (!back_buffer) return 0;
    if ((uint32_t)width * (uint32_t)height > back_buffer_pixels) return 0;
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
// manager (userland/wm/) still draws whole windows/widgets into the back
// buffer when their content actually changes -- this only shrinks the
// final blit, it doesn't make drawing itself region-aware. The one caller
// that gets the full benefit of both halves is the cursor-only-moved case
// (see wm_render_cursor_move() in userland/wm/wm_render.c): a handful of
// pixels touched, a handful of pixels blitted, instead of a full frame.
int gfx_load_frame(uint64_t addr, uint32_t src_pitch, uint32_t w, uint32_t h,
                   uint8_t src_bpp, uint8_t dim) {
    if (!addr || !gfx_set_double_buffered(1)) return 0;
    int rows = (int)h < height ? (int)h : height;
    int cols = (int)w < width ? (int)w : width;
    unsigned keep = 255u - dim;
    for (int y = 0; y < height; y++) {
        uint32_t *d = back_buffer + (uint32_t)y * (uint32_t)width;
        const uint8_t *s = (const uint8_t *)(uintptr_t)addr + (uint64_t)y * src_pitch;
        for (int x = 0; x < width; x++) {
            uint32_t c = 0;
            if (y < rows && x < cols)
                c = src_bpp == 32 ? ((const uint32_t *)s)[x]
                                  : (uint32_t)s[3 * x] | ((uint32_t)s[3 * x + 1] << 8)
                                    | ((uint32_t)s[3 * x + 2] << 16);
            uint32_t r = ((c >> 16) & 0xFF) * keep / 255, g = ((c >> 8) & 0xFF) * keep / 255,
                     b = (c & 0xFF) * keep / 255;
            d[x] = (r << 16) | (g << 8) | b;
        }
    }
    dirty_x0 = 0; dirty_y0 = 0; dirty_x1 = width; dirty_y1 = height;
    return 1;
}

void gfx_present(void) {
    if (!double_buffered) return;
    if (dirty_x1 <= dirty_x0) return; // nothing touched since the last present
    int bytes = bpp / 8;
    for (int y = dirty_y0; y < dirty_y1; y++) {
        const uint32_t *src = back_buffer + (uint32_t)y * (uint32_t)width + dirty_x0;
        uint8_t *dst = fb + (uint32_t)y * pitch + (uint32_t)dirty_x0 * bytes;
        if (bytes == 4) {
            // One 32-bit store per pixel, not three 8-bit ones. This is
            // not micro-optimisation: the framebuffer is uncached MMIO
            // on real hardware, where each store is a separate bus
            // transaction the CPU stalls on, so three-per-pixel is
            // literally three times the cost of the whole blit. It is
            // free under QEMU, whose framebuffer is cached host RAM --
            // which is why the difference is invisible to every test
            // here and was measurable only on a real machine.
            //
            // Safe as an aligned 32-bit access because a 32bpp surface
            // has 4-byte pixels and `pitch` is a whole number of them.
            uint32_t *d32 = (uint32_t *)dst;
            for (int x = dirty_x0; x < dirty_x1; x++) *d32++ = *src++;
        } else {
            for (int x = dirty_x0; x < dirty_x1; x++) {
                uint32_t c = *src++;
                dst[0] = (uint8_t)(c & 0xFF);
                dst[1] = (uint8_t)((c >> 8) & 0xFF);
                dst[2] = (uint8_t)((c >> 16) & 0xFF);
                dst += bytes;
            }
        }
    }
    // Publish before clearing the box -- on a driver-owned mode the
    // adapter shows nothing until told. Called directly rather than
    // through gfx_flush(), which deliberately does nothing while double
    // buffering is on (see its comment).
    display_flush(dirty_x0, dirty_y0, dirty_x1 - dirty_x0, dirty_y1 - dirty_y0);
    dirty_x0 = dirty_x1 = dirty_y0 = dirty_y1 = 0;
}

void gfx_fill_rect(int x, int y, int w, int h, uint32_t color) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            gfx_put_pixel(x + i, y + j, color);
}

// Copies a w*h block of 32bpp pixels to (x, y). `src_pitch_px` is the
// source's row stride IN PIXELS, which is not always `w` -- a caller
// blitting a sub-rectangle out of a larger buffer passes the larger
// buffer's width.
//
// Deliberately a gfx_put_pixel() loop rather than a row-wise memcpy
// into the back buffer, exactly like gfx_fill_rect() above: that is
// what makes it honour the active clip rect, the damage region and the
// dirty-row tracking gfx_present() depends on, with no second copy of
// any of that logic to keep in sync. gfx_fill_rect() already sets the
// precedent that per-pixel cost is acceptable here -- it paints the
// whole desktop background the same way.
void gfx_blit(int x, int y, int w, int h, const uint32_t *src, int src_pitch_px) {
    if (!src || w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        const uint32_t *row = src + (uint32_t)j * (uint32_t)src_pitch_px;
        for (int i = 0; i < w; i++) gfx_put_pixel(x + i, y + j, row[i]);
    }
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
        // gfx_present() still picks it up.
        dirty_mark_rect(0, 0, width, height - pixel_rows);
        gfx_fill_rect(0, height - pixel_rows, width, pixel_rows, bg_color);
        return;
    }
    // The degraded path: shifting the visible framebuffer in place, which
    // means READING it back. Write-combining does not help a load -- it
    // coalesces stores into burst transfers, while each read is still a
    // full bus round trip the CPU stalls on -- so on real hardware this
    // costs roughly a whole screen of uncached reads on top of the
    // writes, and it is slow enough to watch the scanline travel down
    // the display. Every ordinary console reaches the double-buffered
    // branch above instead and never reads the framebuffer at all; this
    // survives only for a surface with no back buffer behind it (the
    // allocation at gfx_init() failed), where there is nothing else to
    // shift.
    uint32_t row_bytes = (uint32_t)width * (bpp / 8);
    for (int y = 0; y < height - pixel_rows; y++) {
        uint8_t *dst = fb + (uint32_t)y * pitch;
        const uint8_t *src = fb + (uint32_t)(y + pixel_rows) * pitch;
        // Word at a time where the format allows it, for the same reason
        // gfx_present() does: one bus transaction per four bytes, not per
        // byte. Aligned because a 32bpp surface has 4-byte pixels and
        // `pitch` is a whole number of them.
        if (bpp == 32) {
            uint32_t *d32 = (uint32_t *)dst;
            const uint32_t *s32 = (const uint32_t *)src;
            for (uint32_t i = 0; i < row_bytes / 4; i++) d32[i] = s32[i];
        } else {
            for (uint32_t i = 0; i < row_bytes; i++) dst[i] = src[i];
        }
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
// real caller: the mouse cursor sprite (userland/wm/wm_render.c), which
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
// Maps a character to its glyph slot in font_ttf_variants[] -- the
// header's font_ttf_slot(), arithmetic over ASCII then Latin-1. Returns
// -1 for anything else (drawn as '?' below). `c` is an int already
// widened to 0-255: a signed `char` above 0x7F would be negative here
// (docs/decisions/drivers.md, the Nordic-keyboard entry).
static int font_ttf_glyph_index(int c) {
    return c < 0 ? -1 : font_ttf_slot((unsigned int)c);
}

// The same walk, under the name the rest of the kernel may call it by
// (api/gfx.h). A wrapper rather than a rename because this file uses
// the private name in a dozen places and on a path that runs per
// character drawn.
int gfx_glyph_index(int c) { return font_ttf_glyph_index(c); }

// Draws `cols` leftmost columns of one glyph's cell. gfx_draw_char()
// below passes the whole cell, which is the contract the console
// depends on (every cell fully painted, background included, so a
// character that replaces a wider one leaves nothing behind). String
// drawing passes the glyph's ADVANCE instead, so a proportional face
// does not paint cell_w of background past the last letter and over
// whatever the caller drew beside it.
static void draw_glyph_kerned(int x, int y, int c, int cols, int kern,
                              uint32_t fg, uint32_t bg);

static void draw_glyph(int x, int y, int c, int cols, uint32_t fg, uint32_t bg) {
    draw_glyph_kerned(x, y, c, cols, 0, fg, bg);
}

// **WHY A KERNED GLYPH CANNOT SIMPLY PAINT ITS OWN BACKGROUND.** A cell
// here is OPAQUE: every pixel is written, the background included, which
// is what lets the console overwrite a character in place. A negative
// kern moves the pen LEFT, so the new cell's leading columns sit on top
// of the previous glyph's last ones -- and painting background there
// erases the tail of the letter that was just drawn. "To" lost the right
// tip of the T's crossbar, which reads as a rasterizer bug rather than a
// spacing one.
//
// So the overlapping columns are BLENDED over whatever is already on
// screen (gfx_blend_pixel, which reads the framebuffer back) instead of
// composited against `bg`. Only those columns: the rest of the cell is
// opaque exactly as before, so the console's overwrite-in-place
// behaviour is untouched -- kern is 0 on every path it uses.
static void draw_glyph_kerned(int x, int y, int c, int cols, int kern,
                              uint32_t fg, uint32_t bg) {
    const struct font_atlas *a = active_atlas();
    int idx = font_ttf_glyph_index(c);
    if (idx < 0) idx = font_ttf_glyph_index('?');

    int gw, gh;
    const unsigned char *glyph;
    if (a) {
        if (idx >= a->count) idx = 0;
        gw = a->cell_w; gh = a->cell_h;
        glyph = a->glyphs + (size_t)idx * (size_t)gw * (size_t)gh;
    } else {
        const struct font_ttf_variant *fv = &font_ttf_variants[cur_font_size];
        gw = fv->w; gh = fv->h;
        glyph = fv->glyphs + (size_t)idx * (size_t)gw * (size_t)gh;
    }
    if (cols > gw) cols = gw;
    if (cols <= 0) return;

    uint8_t fg_r = unpack_channel(fg, red_pos, red_size);
    uint8_t fg_g = unpack_channel(fg, green_pos, green_size);
    uint8_t fg_b = unpack_channel(fg, blue_pos, blue_size);
    uint8_t bg_r = unpack_channel(bg, red_pos, red_size);
    uint8_t bg_g = unpack_channel(bg, green_pos, green_size);
    uint8_t bg_b = unpack_channel(bg, blue_pos, blue_size);

    int overlap = kern < 0 ? -kern : 0;
    if (overlap > cols) overlap = cols;

    // **RING 0 PAINTS ONLY THE LINE, NOT THE WHOLE BITMAP, so it still
    // clips descenders -- and that is the deliberate half of the
    // cell/line split.** A cell here is OPAQUE: every pixel is written,
    // background included, which is what lets the console overwrite a
    // character in place. Painting the taller bitmap would write two
    // rows of background into the row BELOW, erasing the previous line's
    // text on every character drawn.
    //
    // So the console keeps the old behaviour exactly, and it is ring 3 --
    // where ugfx_draw_char() skips background pixels and can overhang
    // harmlessly -- that gets full descenders. Fixing the console too
    // means growing the line pitch, which costs it ~18% of its rows.
    int rows = gh;
    if (a && a->line_h > 0 && a->line_h < rows) rows = a->line_h;

    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < cols; col++) {
            uint8_t alpha = glyph[row * gw + col];
            if (col < overlap) {
                gfx_blend_pixel(x + col, y + row, fg, alpha);
                continue;
            }
            uint32_t color;
            if (alpha == 0) {
                color = bg;
            } else if (alpha == 255) {
                color = fg;
            } else {
                uint8_t r = blend_channel(bg_r, fg_r, alpha);
                uint8_t g = blend_channel(bg_g, fg_g, alpha);
                uint8_t b = blend_channel(bg_b, fg_b, alpha);
                color = pack_channel(r, red_pos, red_size)
                      | pack_channel(g, green_pos, green_size)
                      | pack_channel(b, blue_pos, blue_size);
            }
            gfx_put_pixel(x + col, y + row, color);
        }
    }
}

void gfx_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg) {
    draw_glyph(x, y, (unsigned char)c, gfx_char_w(), fg, bg);
}

// How far the pen moves after drawing `c`, in pixels.
//
// **THE FUNCTION THAT MAKES PROPORTIONAL TEXT POSSIBLE AT ALL**, and
// the reason every measurement below goes through it rather than
// multiplying by gfx_char_w(). With the baked font (or any monospace
// face) it returns the cell width for every character and the arithmetic
// is what it always was; with a proportional face loaded from
// /usr/share/fonts an 'i' is genuinely narrower than a 'W'.
int gfx_char_advance(int c) {
    const struct font_atlas *a = active_atlas();
    if (!a || !a->advances) return gfx_char_w();
    int idx = font_ttf_glyph_index(c);
    if (idx < 0) idx = font_ttf_glyph_index('?');
    if (idx < 0 || idx >= a->count) return a->cell_w;
    int adv = a->advances[idx];
    return adv > 0 ? adv : a->cell_w;
}

// Pixel width of `s` at the current font. A multiplication today
// A REAL MEASUREMENT: it sums per-glyph advances (gfx_char_advance())
// rather than multiplying a character count by the cell width, which is
// why callers were always told to ask this rather than writing
// `k_strlen(s) * gfx_char_w()` themselves -- those call sites would all
// have had to change when a proportional face became loadable, and this
// one did instead. Stops at a newline: a multi-line string has no single
// width, and every caller of this is measuring one row.
int gfx_text_width(const char *s) {
    // NULL is an answer, not a fault: an optional label (a button with
    // none is legal, see ui_button_draw) reaches measuring code
    // unguarded, and the ring-3 ugfx_text_width() has always accepted
    // it. Two halves of one API disagreeing about NULL is its own bug.
    if (!s) return 0;
    int w = 0, prev = 0;
    for (int i = 0; s[i] && s[i] != '\n'; i++) {
        int c = (unsigned char)s[i];
        w += gfx_kern(prev, c) + gfx_char_advance(c);
        prev = c;
    }
    return w;
}

// How many leading characters of `s` fit within `max_w` pixels --
// whole glyphs only, never a partial one. The measurement half of
// gfx_draw_string_clipped(), separated because callers that do their
// own windowing (a text field scrolling to follow its cursor, see
// apps/ui/ui_textbox.c) need the count without the drawing.
int gfx_text_fit_chars(const char *s, int max_w) {
    if (!s) return 0;
    if (gfx_char_w() <= 0) return 0;
    int n = 0;
    int used = 0, prev = 0;
    while (s[n] && s[n] != '\n') {
        int c = (unsigned char)s[n];
        int adv = gfx_kern(prev, c) + gfx_char_advance(c);
        if (used + adv > max_w) break;
        used += adv;
        prev = c;
        n++;
    }
    return n;
}

// Cursor stepping. See gfx.h for why these are functions rather than
// `i + 1` / `i - 1` at each call site.
int gfx_text_next(const char *s, int i) {
    if (!s || i < 0) return 0;
    return s[i] ? i + 1 : i;
}

int gfx_text_prev(const char *s, int i) {
    (void)s;
    return i > 0 ? i - 1 : 0;
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
    int cx = x, prev = 0;
    for (int i = 0; i < n; i++) {
        int c = (unsigned char)s[i];
        int k = gfx_kern(prev, c);
        cx += k;
        draw_glyph_kerned(cx, y, c, gfx_char_advance(c), k, fg, bg);
        cx += gfx_char_advance(c);
        prev = c;
    }
    return s[n] == '\0' || s[n] == '\n';
}

void gfx_draw_string(int x, int y, const char *s, uint32_t fg, uint32_t bg) {
    int cx = x, prev = 0;
    int ch = gfx_char_h();
    while (*s) {
        if (*s == '\n') {
            cx = x;
            y += ch;
            prev = 0; // a new row starts a new run -- nothing kerns across it
        } else {
            int c = (unsigned char)*s;
            int k = gfx_kern(prev, c);
            cx += k;
            draw_glyph_kerned(cx, y, c, gfx_char_advance(c), k, fg, bg);
            cx += gfx_char_advance(c);
            prev = c;
        }
        s++;
    }
}

// --- geometry bindings ------------------------------------------------
//
// One line over kernel/lib/geom.c, which is SHARED with the ring-3
// clients (see geom.h). The only kernel-specific part is this plot
// function: opaque pixels go through gfx_put_pixel() and
// partial ones through gfx_blend_pixel(), so anti-aliased geometry
// composites against whatever is already on screen -- and both honour
// the clip rect and the damage tracking, which is why geometry needed
// no special handling in the compositor.
static void gfx_geom_plot(void *ctx, int x, int y, uint32_t color, uint8_t alpha) {
    (void)ctx;
    if (alpha >= 255) gfx_put_pixel(x, y, color);
    else              gfx_blend_pixel(x, y, color, alpha);
}

static const struct geom_target GFX_TARGET = { gfx_geom_plot, 0 };

void gfx_fill_circle(int cx, int cy, int r, uint32_t color) {
    geom_fill_circle(&GFX_TARGET, cx, cy, r, color);
}



int gfx_bpp(void) { return bpp; }

const char *gfx_write_combining_name(void) {
    return paging_wc_name(display_write_combining());
}

// See gfx.h. The store loop mirrors gfx_present()'s exactly -- one
// 32-bit write per pixel at 32bpp, three byte writes at 24 -- because
// the point is to measure that loop and not a tuned stand-in for it.
uint64_t gfx_bench_fill(uint32_t color, int iterations) {
    if (!fb || width <= 0 || height <= 0 || iterations <= 0) return 0;

    int bytes = bpp / 8;
    uint64_t start = arch_rdtsc();

    for (int n = 0; n < iterations; n++) {
        for (int y = 0; y < height; y++) {
            uint8_t *dst = fb + (uint32_t)y * pitch;
            if (bytes == 4) {
                uint32_t *d32 = (uint32_t *)dst;
                for (int x = 0; x < width; x++) *d32++ = color;
            } else {
                for (int x = 0; x < width; x++) {
                    dst[0] = (uint8_t)(color & 0xFF);
                    dst[1] = (uint8_t)((color >> 8) & 0xFF);
                    dst[2] = (uint8_t)((color >> 16) & 0xFF);
                    dst += bytes;
                }
            }
        }
    }

    // Write-combining buffers are flushed by a serialising instruction,
    // not by the last store retiring -- without this the final burst
    // lands outside the measured window and WC looks faster than it is.
    __asm__ volatile ("mfence" ::: "memory");
    return arch_rdtsc() - start;
}

// See gfx.h. Measures a BURST -- n scrolls then one present -- because
// that is the shape console output actually has, and because the two
// modes put the cost in different places: unbuffered, every scroll is a
// full-screen read-modify-write of the framebuffer and the present is a
// no-op; double-buffered, every scroll is a RAM memmove and the single
// present at the end is the only thing that touches the display.
// Averaging over the burst is what makes the two comparable.
uint64_t gfx_bench_scroll(int pixel_rows, int iterations) {
    if (!fb || width <= 0 || height <= 0) return 0;
    if (pixel_rows <= 0 || pixel_rows >= height || iterations <= 0) return 0;

    uint32_t bg = 0x000000;
    uint64_t start = arch_rdtsc();

    for (int n = 0; n < iterations; n++) gfx_scroll_up(pixel_rows, bg);
    gfx_present(); // no-op when unbuffered; the whole cost when not

    __asm__ volatile ("mfence" ::: "memory");
    return arch_rdtsc() - start;
}

int gfx_double_buffered(void) { return double_buffered; }

uint64_t gfx_back_buffer_bytes(void) {
    return back_buffer ? ((uint64_t)back_buffer_pixels * 4 + 4095) / 4096 * 4096 : 0;
}
