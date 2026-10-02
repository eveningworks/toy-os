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
// The same, to the PHYSICAL console even while a sink is installed --
// for output that is not the sink installer's (ring 3's, tty0's echo).
void vga_putc_console(char c);
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

// Publishes everything the framebuffer console has drawn since the last
// call, and resumes ownership of the screen after something else (the
// window manager) has had it.
//
// The console draws into gfx.c's back buffer rather than at the display,
// so that it never READS the framebuffer -- a write-combined surface
// makes reads strictly worse, and shifting the visible pixels up to
// scroll was a whole screen of them. See vga.c's double-buffering
// comment. The consequence is that drawing and showing are now separate
// steps: text sits in RAM until something presents it.
//
// Cheap and idempotent when nothing has changed, so an idle path can
// call it unconditionally -- keyboard_getchar_mods()'s wait loop does,
// which is the physical shell's "output is finished, waiting for a
// human" moment and covers the tail of any burst. A path that prints
// and then HALTS without reaching that loop must call this itself, or
// its last output is never seen; the fault handler's panic report does.
//
// DOES NOTHING WHILE A COMPOSITOR HOLDS THE SCREEN. The console keeps
// drawing into its back buffer and stops blitting it over the desktop,
// which is Linux's KD_GRAPHICS; vga_resume() puts the accumulated text
// on screen when the desktop goes away. Without it any process writing
// to fd 1 paints the console over the whole desktop.
void vga_present(void);

// The same, ignoring that check. For the one caller whose job is to
// paint over whatever is there: a PANIC, which must be visible under a
// running desktop or it is not a panic report.
void vga_present_force(void);

// Re-enables console double buffering and repaints, after the window
// manager has been using the screen. Called on the way out of GUI mode.
void vga_resume(void);

// THE HELD FRAME: when the desktop DIES (a crash or a kill, not a clean
// exit), the screen keeps its last frame, dimmed, with a "Restarting the
// desktop" card -- rather than the console flashing up for the moment
// init takes to restart it. The frame must already be in the back buffer
// (gfx_load_frame()); begin draws the card and presents, and the console
// stops presenting until the hold ends.
//
// A new compositor ends it with `resume` 0. If none arrives within
// VGA_HOLD_S, vga_hold_expired() ends it with the console -- so a desktop
// init has given up on still leaves a usable screen. Returns 1 when
// holding.
#define VGA_HOLD_S 15
int  vga_hold_begin(void);
void vga_hold_end(int resume);
int  vga_held(void);
// Asked from the console's idle loop: 1 once, when the hold has just run
// out and the console has taken the screen back.
int  vga_hold_expired(void);

// Current console height in text rows -- 25 in legacy 80x25 text mode,
// or gfx_height()/gfx_char_h() in framebuffer mode (varies with the
// active font size, see vga_reflow()). Lets callers that print a lot of
// output (see the shell's console_page() in shell.c) know how many
// lines fit on screen before they'd start scrolling off the top.
uint32_t vga_rows(void);

// Current console width in text columns -- 80 in legacy 80x25 text
// mode, or gfx_width()/gfx_char_w() in framebuffer mode. Peer of
// vga_rows() above; first added for apps/editor.c's windowed CLI
// redraw, which needs to replicate the console's own line-wrapping math
// to keep a status bar pinned to the last row.
uint32_t vga_cols(void);

// ---- cursor style ----
//
// How the framebuffer console draws its cursor. The default is
// TRANSLUCENT: the block tints the cell rather than covering it, so the
// character underneath stays readable -- a solid block hides whatever
// it sits on, which stopped being acceptable once the shell could put
// the cursor in the middle of a line.
//
// Every style works by saving the pixels it covers and restoring them
// on hide (see vga.c), so none of them destroy what's underneath and
// adding another needs no new bookkeeping. Persisted across reboot via
// cursor_config.h; changed at runtime with the shell's `cursor`
// command. Legacy 80x25 text mode ignores all of this -- there the
// hardware draws its own cursor and there is nothing to choose.
enum vga_cursor_style {
    VGA_CURSOR_TRANSLUCENT = 0, // tinted block; the glyph shows through
    VGA_CURSOR_UNDERLINE,       // bar along the bottom of the cell
    VGA_CURSOR_BEAM,            // thin bar at the left edge (an insertion point)
    VGA_CURSOR_REVERSE,         // inverted cell: dark glyph on a light block
    VGA_CURSOR_STYLE_COUNT
};

// Kept in lockstep with the enum above, same convention as
// debugflags.c's DBGFLAG_NAMES -- the `cursor` command reads it
// generically, so a new style needs no changes there.
extern const char *const VGA_CURSOR_STYLE_NAMES[VGA_CURSOR_STYLE_COUNT];

enum vga_cursor_style vga_cursor_style(void);
void vga_set_cursor_style(enum vga_cursor_style style);

// Exact match against VGA_CURSOR_STYLE_NAMES ("reject rather than
// guess", same as dbgflag_parse()). Returns 1 and sets *out on a match.
int vga_cursor_style_parse(const char *name, enum vga_cursor_style *out);

// Drives the framebuffer console's blinking cursor -- call this
// periodically while idle (see keyboard_getchar() in keyboard.c, which
// calls it once per wake-up from its blocking wait loop). No-op in
// legacy 80x25 text mode, where the real VGA hardware cursor already
// blinks without any help.
void vga_cursor_tick(void);

// Moves the console's insertion point by `delta` cells (negative =
// left) WITHOUT erasing anything, wrapping across row boundaries and
// clamping at the first/last cell. Subsequent vga_putc() writes at the
// new position, and vga_backspace() erases relative to it.
//
// Added for the shell's readline-style line editing
// (kernel/lib/klineedit.c): every other console primitive here only
// ever appends or removes at the end, which is precisely why the shell
// could not edit mid-line before.
//
// Two things a caller has to know:
//   - It does not repaint text. A caller that moves back over
//     characters and then wants them redrawn must redraw them itself
//     (the shell repaints the whole input line each keystroke).
//   - The cursor keeps blinking wherever it lands, including on top of
//     a character. That's safe because the cursor saves and restores
//     the pixels it covers (see vga.c) -- it was NOT safe in the first
//     version of this function, where the blink's "off" phase erased
//     its cell to black and ate the glyph underneath, so the cursor had
//     to be pinned solid off the append point. If you're reading this
//     because something is eating characters, that mechanism is where
//     to look.
// No-op while an output sink is active (a sink owns its own cursor --
// the GUI Terminal renders one through its scrollback widget instead).
void vga_cursor_move(int delta);

// Suppresses the cursor block vga_putc()/vga_write() otherwise repaint
// solid after every character -- for a caller producing its own
// output on a timer/loop with nothing actually waiting on a keystroke
// (e.g. stress's in-place progress bar), where that block would
// otherwise just sit there statically instead of blinking. The next
// real vga_write()/vga_putc() call shows a fresh cursor again on its
// own -- no matching "show" call needed. No-op in legacy 80x25 text
// mode, same as vga_cursor_tick() above.
void vga_cursor_hide(void);

// ---- scrollback ----
//
// The console keeps the last few hundred output lines (see vga.c's
// scrollback section) so anything that scrolled off -- the boot
// messages, the tail of a long command -- can be read back.
// keyboard_getchar() drives these from PageUp/PageDown, so they work
// wherever the kernel is waiting for a keypress; nothing else needs to
// call them.
//
// Any new output snaps the view back to the bottom first, so the live
// console and the history can't interleave on screen.
void vga_scroll_back(int lines);     // toward older output
void vga_scroll_forward(int lines);  // back toward live
int vga_scrolled_back(void);         // 1 while showing history

#endif
