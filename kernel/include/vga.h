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

// ---- output sink redirection ----
//
// By default every vga_* call below goes straight to the physical
// console (legacy 0xB8000 text mode or the framebuffer text backend --
// see vga.c's top comment). A sink lets a caller redirect all of that
// output somewhere else instead -- e.g. a GUI app's own scrollback
// buffer -- WITHOUT touching every individual vga_write()/vga_putc()
// call site across the kernel (shell.c's ~40 command handlers chief
// among them). This is what makes shell.c's dispatch() reusable from a
// terminal-emulator GUI app: push a sink before calling dispatch(), pop
// it after, and every cmd_* function's existing vga_write() calls just
// work, unmodified.
//
// Only one sink can be active at a time (no stack) -- that's all any
// current caller needs, and it keeps this simple. vga_set_sink(0)
// restores the physical console. Whatever installs a sink is
// responsible for restoring it (typically to 0) when done; leaving a
// stale sink installed would silently swallow all future console
// output, including the shell's own prompt.
struct vga_sink {
    void *ctx;
    void (*putc)(void *ctx, char c);
    void (*backspace)(void *ctx);
    void (*clear)(void *ctx);
    void (*set_color)(void *ctx, enum vga_color fg, enum vga_color bg);
    // May be NULL -- vga_rows() falls back to a sane default (24) for
    // sinks that don't track a page height (e.g. pure loggers).
    uint32_t (*rows)(void *ctx);
};

// Installs `sink` as the active output redirect, or pass NULL to
// restore the physical console. Returns the PREVIOUSLY active sink (0
// if none), so callers can nest safely by saving and restoring it --
// see apps/terminal.c (phase 4) for the pattern.
const struct vga_sink *vga_set_sink(const struct vga_sink *sink);

// True while any sink is installed. Lets a handful of shell commands
// that would otherwise block on keyboard input mid-command (see
// apps/shell.c's console_page() and cmd_timezone()) detect "I might be
// running inside a non-blocking GUI callback right now, not the
// blocking-by-design interactive console loop" and skip the blocking
// part instead of hanging whatever's driving them through a sink.
int vga_sink_active(void);

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

// Current text color the physical console is drawing with. When a sink
// is active this still reflects the *physical* console's color state
// (sinks track their own color via their set_color callback, if any) --
// callers that need to know what color they're "logically" writing in
// while a sink might be active should track it themselves rather than
// relying on this.
enum vga_color vga_current_fg(void);

// The RGB value vga.c's framebuffer backend draws a given vga_color
// with (see vga.c's palette_rgb() -- this is a public wrapper around
// the exact same table, added so other renderers can match the
// console's palette exactly instead of picking their own colors that
// would look inconsistent side by side with it. First caller: the
// scrollback text widget (widgets.h), which renders each cell in
// whatever vga_color it was written with.
uint32_t vga_color_rgb(enum vga_color c);

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
