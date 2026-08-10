#ifndef VGA_H
#define VGA_H

#include <stdint.h>

enum vga_color {
    VGA_BLACK = 0, VGA_BLUE = 1, VGA_GREEN = 2, VGA_CYAN = 3,
    VGA_RED = 4, VGA_MAGENTA = 5, VGA_BROWN = 6, VGA_LIGHT_GREY = 7,
    VGA_DARK_GREY = 8, VGA_LIGHT_BLUE = 9, VGA_LIGHT_GREEN = 10,
    VGA_LIGHT_CYAN = 11, VGA_LIGHT_RED = 12, VGA_LIGHT_MAGENTA = 13,
    VGA_LIGHT_BROWN = 14, VGA_WHITE = 15,
};

void vga_init(void);
void vga_clear(void);
void vga_putc(char c);
void vga_write(const char *s);
void vga_write_dec(uint32_t n);
void vga_write_hex(uint64_t n);

// Prints a process_run_ring3() return value the way every *test command
// wants it shown: a real (non-negative) exit code as a plain decimal
// number via vga_write_dec(), or -- for PROCESS_CRASHED (process.h),
// the sentinel a caught ring-3 fault returns -- "CRASHED" instead of
// blindly casting a negative int to vga_write_dec()'s uint32_t (which
// would print something like 4294967294 for -2, not "-2": exactly the
// kind of confusing output introducing PROCESS_CRASHED made possible
// for the first time, since every exit code before it was always >= 0).
void vga_write_exit_code(int code);
void vga_set_color(enum vga_color fg, enum vga_color bg);
void vga_backspace(void);

// Recomputes the framebuffer console's column/row count from the
// current gfx_char_w()/gfx_char_h() and clears the screen -- call after
// gfx_set_font_size() changes the active font, since console_cols/rows
// are cached (recalculating them on every vga_putc() would be wasteful)
// and would otherwise still reflect the previous font's cell size. No-op
// in legacy 80x25 text mode (that console's cell size never changes).
void vga_reflow(void);

// Current console height in text rows -- 25 in legacy 80x25 text mode,
// or gfx_height()/gfx_char_h() in framebuffer mode (varies with the
// active font size, see vga_reflow()). Lets callers that print a lot of
// output (see the shell's console_page() in shell.c) know how many
// lines fit on screen before they'd start scrolling off the top.
uint32_t vga_rows(void);

// Drives the framebuffer console's blinking cursor -- call this
// periodically while idle (see keyboard_getchar() in keyboard.c, which
// calls it once per wake-up from its blocking wait loop). No-op in
// legacy 80x25 text mode, where the real VGA hardware cursor already
// blinks without any help.
void vga_cursor_tick(void);

#endif
