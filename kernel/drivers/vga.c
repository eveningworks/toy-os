// Console layer. Renders through one of two backends, chosen automatically
// at vga_init() time:
//   - legacy: direct writes to the 0xB8000 VGA text-mode buffer
//   - framebuffer: character cells drawn with the 8x8 font onto whatever
//     linear RGB framebuffer GRUB handed us (see gfx.c / multiboot.c)
// Every other file in the kernel just calls vga_write()/vga_putc()/etc and
// doesn't need to know which backend is actually active.

#include "vga.h"
#include "io.h"
#include "gfx.h"
#include "timer.h"
#include <stddef.h>

#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_MEM ((uint16_t *)0xB8000)

// Cell size used to come from compile-time FONT_CHAR_W/H macros; now
// that font size is runtime-selectable (see gfx_set_font_size()), every
// use below calls gfx_char_w()/gfx_char_h() fresh instead.
#define CELL_W gfx_char_w()
#define CELL_H gfx_char_h()

static int fb_mode = 0;

static size_t row;
static size_t col;
static size_t console_cols;
static size_t console_rows;

static uint8_t legacy_color;
static uint16_t *buf = VGA_MEM;

static enum vga_color cur_fg = VGA_LIGHT_GREY;
static enum vga_color cur_bg = VGA_BLACK;

static uint32_t palette_rgb(enum vga_color c) {
    switch (c) {
        case VGA_BLACK:         return gfx_rgb(0, 0, 0);
        case VGA_BLUE:          return gfx_rgb(0, 0, 170);
        case VGA_GREEN:         return gfx_rgb(0, 170, 0);
        case VGA_CYAN:          return gfx_rgb(0, 170, 170);
        case VGA_RED:           return gfx_rgb(170, 0, 0);
        case VGA_MAGENTA:       return gfx_rgb(170, 0, 170);
        case VGA_BROWN:         return gfx_rgb(170, 85, 0);
        case VGA_LIGHT_GREY:    return gfx_rgb(170, 170, 170);
        case VGA_DARK_GREY:     return gfx_rgb(85, 85, 85);
        case VGA_LIGHT_BLUE:    return gfx_rgb(85, 85, 255);
        case VGA_LIGHT_GREEN:   return gfx_rgb(85, 255, 85);
        case VGA_LIGHT_CYAN:    return gfx_rgb(85, 255, 255);
        case VGA_LIGHT_RED:     return gfx_rgb(255, 85, 85);
        case VGA_LIGHT_MAGENTA: return gfx_rgb(255, 85, 255);
        case VGA_LIGHT_BROWN:   return gfx_rgb(255, 255, 85);
        case VGA_WHITE:         return gfx_rgb(255, 255, 255);
    }
    return gfx_rgb(170, 170, 170);
}

// ---- legacy 0xB8000 backend ----

static inline uint8_t make_color(enum vga_color fg, enum vga_color bg) {
    return fg | (bg << 4);
}

static inline uint16_t make_entry(char c, uint8_t clr) {
    return (uint16_t)c | ((uint16_t)clr << 8);
}

static void legacy_update_cursor(void) {
    uint16_t pos = (uint16_t)(row * VGA_WIDTH + col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

static void legacy_clear(void) {
    for (size_t y = 0; y < VGA_HEIGHT; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            buf[y * VGA_WIDTH + x] = make_entry(' ', legacy_color);
    row = 0;
    col = 0;
    legacy_update_cursor();
}

static void legacy_scroll_if_needed(void) {
    if (row < VGA_HEIGHT) return;
    for (size_t y = 1; y < VGA_HEIGHT; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            buf[(y - 1) * VGA_WIDTH + x] = buf[y * VGA_WIDTH + x];
    for (size_t x = 0; x < VGA_WIDTH; x++)
        buf[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = make_entry(' ', legacy_color);
    row = VGA_HEIGHT - 1;
}

static void legacy_putc(char c) {
    if (c == '\n') {
        col = 0;
        row++;
    } else if (c == '\r') {
        col = 0;
    } else {
        buf[row * VGA_WIDTH + col] = make_entry(c, legacy_color);
        col++;
        if (col >= VGA_WIDTH) {
            col = 0;
            row++;
        }
    }
    legacy_scroll_if_needed();
    legacy_update_cursor();
}

static void legacy_backspace(void) {
    if (col == 0) {
        if (row == 0) return;
        row--;
        col = VGA_WIDTH - 1;
    } else {
        col--;
    }
    buf[row * VGA_WIDTH + col] = make_entry(' ', legacy_color);
    legacy_update_cursor();
}

// ---- blinking cursor (framebuffer mode only) ----
//
// The legacy 0xB8000 backend gets a blinking cursor for free from real
// VGA text-mode hardware (legacy_update_cursor() just tells it where to
// blink, not whether); the framebuffer backend draws its own console
// text pixel-by-pixel and has no such hardware to lean on, so it needs
// this. Kept deliberately simple: a solid block the size of one glyph
// cell at the current (row, col), toggled on a timer.
//
// cursor_on_screen tracks whether that block is currently painted, so
// cursor_hide() is a safe no-op when there's nothing to erase. It's
// always painted at the *current* (row, col) -- the cell the next
// character will land in -- which is guaranteed blank whenever the
// cursor is shown (nothing else writes there without going through
// fb_putc()/fb_backspace(), both of which hide the cursor first), so
// painting over it and erasing it back to cur_bg is always safe.
static int cursor_on_screen = 0;
static uint64_t cursor_last_toggle_tick = 0;

// 100 Hz PIT (see idt.c's pit_init(100) call) -- 50 ticks is 500ms, so
// a full on/off blink cycle is about a second, a fairly ordinary
// terminal cursor rate.
#define CURSOR_BLINK_TICKS 50

static void cursor_paint(uint32_t color) {
    gfx_fill_rect((int)(col * CELL_W), (int)(row * CELL_H), (int)CELL_W, (int)CELL_H, color);
}

static void cursor_hide(void) {
    if (!cursor_on_screen) return;
    cursor_paint(palette_rgb(cur_bg));
    cursor_on_screen = 0;
}

static void cursor_show_and_reset_blink(void) {
    if (!fb_mode) return;
    cursor_paint(palette_rgb(cur_fg));
    cursor_on_screen = 1;
    cursor_last_toggle_tick = pit_ticks(); // full interval before the next auto-toggle,
                                            // so it doesn't flicker right after typing
}

// ---- framebuffer text-console backend ----

static void fb_clear(void) {
    gfx_clear(palette_rgb(cur_bg));
    row = 0;
    col = 0;
    cursor_on_screen = 0; // whatever was drawn is gone along with everything else
}

static void fb_scroll_if_needed(void) {
    if (row < console_rows) return;
    gfx_scroll_up(CELL_H, palette_rgb(cur_bg));
    row = console_rows - 1;
}

static void fb_putc(char c) {
    cursor_hide(); // erase before row/col move -- see cursor_hide()'s comment
    if (c == '\n') {
        col = 0;
        row++;
    } else if (c == '\r') {
        col = 0;
    } else {
        gfx_draw_char((int)(col * CELL_W), (int)(row * CELL_H), c,
                      palette_rgb(cur_fg), palette_rgb(cur_bg));
        col++;
        if (col >= console_cols) {
            col = 0;
            row++;
        }
    }
    fb_scroll_if_needed();
    cursor_show_and_reset_blink(); // reappear solid at the new position right away
}

static void fb_backspace(void) {
    cursor_hide();
    if (col == 0) {
        if (row == 0) return;
        row--;
        col = console_cols - 1;
    } else {
        col--;
    }
    gfx_draw_char((int)(col * CELL_W), (int)(row * CELL_H), ' ',
                  palette_rgb(cur_fg), palette_rgb(cur_bg));
    cursor_show_and_reset_blink();
}

// ---- public API ----

void vga_init(void) {
    legacy_color = make_color(VGA_LIGHT_GREY, VGA_BLACK);

    if (gfx_init()) {
        fb_mode = 1;
        console_cols = (size_t)gfx_width() / CELL_W;
        console_rows = (size_t)gfx_height() / CELL_H;
        fb_clear();
    } else {
        fb_mode = 0;
        legacy_clear();
    }
}

void vga_clear(void) {
    if (fb_mode) fb_clear();
    else legacy_clear();
}

void vga_reflow(void) {
    if (!fb_mode) return; // legacy 80x25 text mode has no variable cell size
    console_cols = (size_t)gfx_width() / CELL_W;
    console_rows = (size_t)gfx_height() / CELL_H;
    fb_clear();
}

uint32_t vga_rows(void) {
    return (uint32_t)(fb_mode ? console_rows : VGA_HEIGHT);
}

// Called from keyboard_getchar()'s wait loop (see keyboard.c) on every
// wake-up, which happens on every interrupt including the 100Hz PIT
// tick -- so this runs roughly every 10ms while idle at a prompt, but
// only actually does anything (a fill_rect) once every
// CURSOR_BLINK_TICKS of those, toggling the cursor block on/off. A
// plain no-op in legacy text mode, where the hardware cursor already
// blinks on its own.
void vga_cursor_tick(void) {
    if (!fb_mode) return;
    if (pit_ticks() - cursor_last_toggle_tick < CURSOR_BLINK_TICKS) return;
    cursor_last_toggle_tick = pit_ticks();
    if (cursor_on_screen) {
        cursor_hide();
    } else {
        cursor_paint(palette_rgb(cur_fg));
        cursor_on_screen = 1;
    }
}

void vga_set_color(enum vga_color fg, enum vga_color bg) {
    cur_fg = fg;
    cur_bg = bg;
    legacy_color = make_color(fg, bg);
}

void vga_putc(char c) {
    // '\b' is a control code, not a printable glyph -- keyboard_read_line()
    // (the kernel's own line reader) never sends it through here, it calls
    // vga_backspace() directly. But SYS_WRITE (syscall.c) forwards
    // whatever bytes a ring-3 process hands it straight through vga_putc()
    // one byte at a time, with no such special-casing of its own -- so a
    // process that writes a literal '\b' (see userland/echo.c) needs this
    // to actually erase-and-move-back instead of drawing font_ttf's glyph
    // for character 8, which is neither of those things (see gfx_draw_char).
    if (c == '\b') {
        vga_backspace();
        return;
    }
    if (fb_mode) fb_putc(c);
    else legacy_putc(c);
}

void vga_backspace(void) {
    if (fb_mode) fb_backspace();
    else legacy_backspace();
}

void vga_write(const char *s) {
    while (*s) vga_putc(*s++);
}

void vga_write_dec(uint32_t n) {
    char tmp[11];
    int i = 0;
    if (n == 0) {
        vga_putc('0');
        return;
    }
    while (n > 0) {
        tmp[i++] = '0' + (n % 10);
        n /= 10;
    }
    while (i > 0) vga_putc(tmp[--i]);
}

void vga_write_hex(uint64_t n) {
    vga_write("0x");
    char buf[17];
    for (int i = 15; i >= 0; i--) {
        uint8_t nibble = (n >> (i * 4)) & 0xF;
        buf[15 - i] = nibble < 10 ? (char)('0' + nibble) : (char)('a' + nibble - 10);
    }
    buf[16] = '\0';
    int start = 0;
    while (start < 15 && buf[start] == '0') start++;
    vga_write(buf + start);
}
