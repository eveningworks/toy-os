// Console layer. Renders through one of two backends, chosen automatically
// at vga_init() time:
//   - legacy: direct writes to the 0xB8000 VGA text-mode buffer
//   - framebuffer: character cells drawn with the 8x8 font onto whatever
//     linear RGB framebuffer GRUB handed us (see gfx.c / multiboot.c)
// Every other file in the kernel just calls vga_write()/vga_putc()/etc and
// doesn't need to know which backend is actually active.

#include "vga.h"
#include "ansi.h"
#include "io.h"
#include "gfx.h"
#include "win_server.h" // win_server_any() -- is anything else painting the screen?
#include "timer.h"
#include "klog.h"
#include "knum.h"
#include "string.h"
#include <stddef.h>

// driver-none: the text console above whatever display.c chose

#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_MEM ((uint16_t *)0xB8000)

// Cell size used to come from compile-time FONT_CHAR_W/H macros; now
// that font size is runtime-selectable (see gfx_set_font_size()), every
// use below calls gfx_char_w()/gfx_char_h() fresh instead.
#define CELL_W gfx_char_w()
#define CELL_H gfx_char_h()

static int fb_mode = 0;

// ---- console double buffering ----
//
// The framebuffer console draws into gfx.c's back buffer and publishes
// with gfx_present(), rather than drawing straight at the display.
//
// The invariant this buys: **the console never READS the framebuffer.**
// It used to, in two places -- scrolling shifted the visible pixels up
// in place, and the cursor saved the cell underneath itself before
// painting over it. Both are reads of a surface that is write-combined
// (see paging.c), and WC is a write optimisation that makes reads
// strictly worse: stores coalesce into bursts, while each load is an
// uncached bus round trip with no cache fill and no prefetch. A
// full-screen shift is a whole screen of those, which on a real machine
// is slow enough to watch the scanline travel down the display. It is
// invisible under plain QEMU, whose TCG ignores guest memory types
// entirely -- `make run-kvm` and real hardware are where it shows.
//
// The trap, if you edit this: presenting is now a SEPARATE step from
// drawing, so anything that puts text on screen and then stops without
// reaching a flush point leaves that text in RAM only. The flush points
// are vga_present() from the keyboard's idle loop (the physical shell's
// "output is over, waiting for a human" moment), the throttled present
// at the end of each vga_putc(), and the explicit call on the panic
// path. A new path that prints and then halts needs its own.
//
// There is deliberately NO "console has drawn something" flag here.
// gfx.c already tracks the dirty bounding box, and gfx_present() returns
// immediately when it is empty, so that IS the answer -- a second copy
// of it kept in this file could only ever disagree with it, and the
// direction it would disagree in is the silent one (text drawn, flag not
// set, nothing ever shown). Present unconditionally and let gfx.c decide.
static uint64_t fb_last_present_tick;

static size_t row;
static size_t col;
static size_t console_cols;
static size_t console_rows;

static uint8_t legacy_color;
static uint16_t *buf = VGA_MEM;

static enum vga_color cur_fg = VGA_LIGHT_GREY;
static enum vga_color cur_bg = VGA_BLACK;

// The console's OWN background, as opposed to whatever colour a caller
// has temporarily set. A cell nobody has written to belongs to the
// console, so blank space introduced by a scroll is painted with this
// rather than with cur_bg -- see fb_scroll_if_needed(). Same reasoning
// cursor_hide() already spells out for the cursor cell.
#define CONSOLE_DEFAULT_BG VGA_BLACK

// ---- output sink redirection (see vga.h's struct vga_sink comment) ----
static const struct vga_sink *active_sink = 0;

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
    // The scrolled-in row is blank, so it gets the console's own colours
    // rather than whatever legacy_color happens to be -- see
    // fb_scroll_if_needed() for the artifact this avoids.
    uint8_t blank = make_color(VGA_LIGHT_GREY, CONSOLE_DEFAULT_BG);
    for (size_t x = 0; x < VGA_WIDTH; x++)
        buf[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = make_entry(' ', blank);
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
// fb_putc()/fb_backspace(), both of which hide the cursor first).
//
// cursor_hide() erases using a hardcoded VGA_BLACK, not the live
// cur_bg -- a blank cell (nothing drawn there yet, only the cursor
// ever touched it) is always conceptually part of the console's plain
// page background, which is black, regardless of whatever transient
// text color happens to be active via vga_set_color() at the moment.
// Using live cur_bg here looks right for ordinary typing (bg is
// almost always black anyway) but breaks for a colored diagnostic
// banner: idt.c's panic handler prints white-on-red, and its trailing
// newline leaves the cursor auto-painted red on an otherwise-blank
// line purely because cur_bg happened to be red right then -- no red
// TEXT was ever actually meant to occupy that cell. Erasing with
// live cur_bg either leaves that red block behind (if a later
// vga_set_color() already moved cur_bg on, e.g. to
// ring3_test.c's own light-green-on-black follow-up) or is a
// silent no-op (erasing red with still-red cur_bg repaints the same
// red) -- both were confirmed live via `ring3test`. Hardcoding black
// sidesteps needing to track "what color was this blank cell really
// supposed to be" at all: it's always black, full stop.
static int cursor_on_screen = 0;
static uint64_t cursor_last_toggle_tick = 0;

// The cursor SAVES THE PIXELS IT COVERS and puts them back when it
// hides, rather than erasing its cell to a colour and hoping that was
// what used to be there.
//
// That older approach erased to black, which was right at the append
// point (always a blank cell) and quietly wrong anywhere else -- a
// cursor sitting on a character ate the character when it moved or
// blinked off. Line editing made "anywhere else" the common case: the
// first mid-line edit shipped a visible hole where a '.' had been.
// Saving and restoring is exact regardless of what's underneath, which
// also means the cursor can blink mid-line again (it had to be pinned
// solid to stay safe) and that any cursor SHAPE works, since nothing
// has to reconstruct the cell.
//
// Sized for the largest glyph cell the biggest baked font produces,
// with a runtime guard below rather than a silent overflow if that ever
// stops being true.
#define CURSOR_SAVE_W 32
#define CURSOR_SAVE_H 64
static uint32_t cursor_save[CURSOR_SAVE_W * CURSOR_SAVE_H];
static int cursor_save_w = 0, cursor_save_h = 0; // what's actually stored

// How strongly the translucent style tints the cell it covers. High
// enough to read as a cursor at a glance, low enough that the character
// underneath stays legible -- the whole point of the style.
// Two strengths, because a cell is mostly glyph at these font sizes.
// The background has to move enough for the cell to read as a block at
// a glance; the glyph has to move FURTHER, or it sinks into its own
// cursor. Tinting both by the same amount was tried and left the
// character washed out (contrast against the block dropped from 170 to
// 89); tinting only the background was also tried and produced a block
// two pixels wide, because a glyph like 'r' fills most of its cell.
#define CURSOR_TINT_ALPHA_BG 130
#define CURSOR_TINT_ALPHA_FG 205

static enum vga_cursor_style cursor_style = VGA_CURSOR_TRANSLUCENT;

// Kept in lockstep with enum vga_cursor_style (vga.h), same convention
// as debugflags.c's DBGFLAG_NAMES -- `cursor` (no args) and
// `cursor <name>` both read this generically.
const char *const VGA_CURSOR_STYLE_NAMES[VGA_CURSOR_STYLE_COUNT] = {
    "translucent", "underline", "beam", "reverse",
};

// 100 Hz PIT (see idt.c's pit_init(100) call) -- 50 ticks is 500ms, so
// a full on/off blink cycle is about a second, a fairly ordinary
// terminal cursor rate.
#define CURSOR_BLINK_TICKS 50

// Paints the cursor at (row, col) in the active style, saving whatever
// it covers first. A no-op if the cursor is already on screen -- saving
// then would capture the cursor's own pixels and "restore" them later.
// DECTCEM (`ESC[?25l`) sets this. A full-screen program hides the
// cursor for the whole time it is painting, and a blinking block left
// wandering across its output is the difference between a drawn screen
// and a flickering one.
//
// Checked in cursor_draw() rather than at each call site, because the
// cursor is drawn from four places (after a putc, after a move, on the
// blink tick, on resume) and one of them would eventually be missed.
static int cursor_suppressed = 0;

static void cursor_draw(void) {
    if (cursor_on_screen || cursor_suppressed) return;

    int w = (int)CELL_W, h = (int)CELL_H;
    if (w > CURSOR_SAVE_W || h > CURSOR_SAVE_H || w <= 0 || h <= 0) {
        return; // can't save it, so don't paint it -- see CURSOR_SAVE_W
    }
    int x0 = (int)(col * CELL_W), y0 = (int)(row * CELL_H);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            cursor_save[y * CURSOR_SAVE_W + x] = gfx_get_pixel(x0 + x, y0 + y);
        }
    }
    cursor_save_w = w;
    cursor_save_h = h;

    uint32_t color = palette_rgb(cur_fg);
    switch (cursor_style) {
    case VGA_CURSOR_UNDERLINE: {
        int thick = h >= 20 ? 3 : 2;
        gfx_fill_rect(x0, y0 + h - thick, w, thick, color);
        break;
    }
    case VGA_CURSOR_BEAM: {
        int thick = w >= 12 ? 2 : 1;
        gfx_fill_rect(x0, y0, thick, h, color);
        break;
    }
    case VGA_CURSOR_REVERSE:
        // Inverting the saved pixels needs no knowledge of which of them
        // are glyph and which are background -- the character comes out
        // dark-on-light either way.
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                uint32_t under = cursor_save[y * CURSOR_SAVE_W + x];
                gfx_put_pixel(x0 + x, y0 + y, ~under);
            }
        }
        break;
    case VGA_CURSOR_TRANSLUCENT:
    default: {
        // Tint every pixel in the cell instead of replacing it, so the
        // glyph shows through.
        //
        // Tint the whole cell, but the glyph harder than the
        // background (see the two alphas above) -- the background rises
        // enough to read as a block, the character rises further and
        // stays legible on top of it.
        //
        // "Background" is whatever matches the cell's own background
        // colour exactly. Antialiased glyph edges don't, so they get
        // the glyph treatment, which is the right side to err on: it
        // keeps the character's outline crisp.
        uint32_t bg_pixel = palette_rgb(cur_bg);
        uint32_t tint = (cur_bg <= VGA_DARK_GREY) ? palette_rgb(VGA_WHITE)
                                                   : palette_rgb(VGA_BLACK);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                uint32_t under = cursor_save[y * CURSOR_SAVE_W + x];
                uint8_t alpha = (under == bg_pixel) ? CURSOR_TINT_ALPHA_BG
                                                     : CURSOR_TINT_ALPHA_FG;
                gfx_put_pixel(x0 + x, y0 + y, gfx_blend(under, tint, alpha));
            }
        }
        break;
    }
    }
    cursor_on_screen = 1;
}

static void cursor_hide(void) {
    if (!cursor_on_screen) return;
    int x0 = (int)(col * CELL_W), y0 = (int)(row * CELL_H);
    for (int y = 0; y < cursor_save_h; y++) {
        for (int x = 0; x < cursor_save_w; x++) {
            gfx_put_pixel(x0 + x, y0 + y, cursor_save[y * CURSOR_SAVE_W + x]);
        }
    }
    cursor_on_screen = 0;
}

static void cursor_show_and_reset_blink(void) {
    if (!fb_mode) return;
    cursor_draw();
    cursor_last_toggle_tick = pit_ticks(); // full interval before the next auto-toggle,
                                            // so it doesn't flicker right after typing
}

enum vga_cursor_style vga_cursor_style(void) { return cursor_style; }

void vga_set_cursor_style(enum vga_cursor_style style) {
    if (style >= VGA_CURSOR_STYLE_COUNT) return;
    if (fb_mode) cursor_hide(); // repaint in the new shape, not on top of the old one
    cursor_style = style;
    if (fb_mode) cursor_draw();
}

int vga_cursor_style_parse(const char *name, enum vga_cursor_style *out) {
    for (int i = 0; i < VGA_CURSOR_STYLE_COUNT; i++) {
        if (k_strcmp(name, VGA_CURSOR_STYLE_NAMES[i]) == 0) {
            *out = (enum vga_cursor_style)i;
            return 1;
        }
    }
    return 0;
}

// ---- framebuffer text-console backend ----

// Publishes everything the console has drawn since the last present.
// Cheap when nothing changed, so callers on an idle path can call it
// unconditionally -- only the dirty bounding box is blitted.
// Publishes the console's back buffer. TWO ENTRY POINTS, and the
// difference is who owns the screen.
//
// vga_present() is the ROUTINE one and does NOTHING while a compositor
// holds the screen: the console keeps drawing into its own back buffer,
// it just stops blitting that over the desktop. Linux's KD_GRAPHICS, for
// the same reason -- a program's output must not paint over a graphical
// session. Anything a process printed is still there and vga_resume()
// puts it on screen when the desktop goes away.
//
// vga_present_force() ignores that, for the one caller whose whole
// purpose is to paint over whatever is there: a PANIC. Guarding it would
// have made every panic under a running desktop invisible.
//
// Default-safe on purpose: a new routine caller gets the check without
// knowing it exists, and the two that mean to override say so.
static void present_now(void) {
    gfx_present();
    gfx_flush();
    fb_last_present_tick = pit_ticks();
}

void vga_present_force(void) {
    if (!fb_mode) return;
    present_now();
}

void vga_present(void) {
    if (!fb_mode) return;
    // THE DESKTOP OWNS THE SCREEN. Without this, any ring-3 process
    // writing to fd 1 blits the text console over the whole desktop --
    // DOOM's startup banner was the visible case, `dmesg` covers 100% of
    // the screen, and it is not the writer's fault in either.
    if (win_server_any()) return;
    // Both modes, in the one place, so no caller has to know which is
    // live. Double-buffered, gfx_present() blits the dirty box and
    // publishes it; drawing straight at the display it is a no-op and
    // gfx_flush() is what tells a driver-owned mode to show the region.
    // Exactly one of the two does the work on any given boot.
    present_now();
}

// The mid-burst present. Bounded to one per PIT tick (100Hz) so a
// command dumping hundreds of scrolling lines pays for one full-screen
// blit per tick rather than one per line, while still animating instead
// of appearing to freeze until it finishes. The tail of the burst is
// caught by vga_present() from the idle loop, so nothing relies on this
// firing on the last line.
static void fb_present_throttled(void) {
    if (pit_ticks() == fb_last_present_tick) return;
    vga_present();
}

static void fb_clear(void) {
    gfx_clear(palette_rgb(cur_bg));
    row = 0;
    col = 0;
    cursor_on_screen = 0; // whatever was drawn is gone along with everything else
    vga_present(); // a clear is a visible event in its own right, not part of a burst
}

static void fb_scroll_if_needed(void) {
    if (row < console_rows) return;
    // CONSOLE_DEFAULT_BG, not cur_bg: the row scrolling in is blank, and
    // filling it with a transient colour paints a full-width band that
    // no text asked for and that later text on that row only partly
    // repaints. That is what left ragged red stripes across the lines
    // after a panic banner.
    gfx_scroll_up(CELL_H, palette_rgb(CONSOLE_DEFAULT_BG));
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


// ---------------------------------------------------------------------
// Scrollback
//
// The framebuffer console draws glyphs straight into the framebuffer and
// scrolls by blitting pixels upward (fb_scroll_if_needed() ->
// gfx_scroll_up()). Nothing keeps what scrolled off, which is why the
// boot messages -- and the output of anything longer than a screen --
// used to be simply gone. The GUI Terminal has had scrollback since its
// text_scrollback widget existed; the physical console never did.
//
// This is a ring of logical output LINES, recorded at vga_putc() level
// so it works the same for both backends. It stores the character and
// its colour per cell (one byte each), because output here is routinely
// multi-coloured within a line -- a green prompt then grey input, `ls`'s
// per-type colouring -- and replaying it in a single colour would be a
// visibly worse copy of what you saw.
//
// Sized as a fixed .bss array rather than a heap allocation for the same
// reason the free-block bitmap is: it must work before heap_init(), and
// its size is a compile-time constant either way. 256 lines x 512 cols
// x 2 bytes = 256KB. 512 columns covers the widest this console gets at
// the largest mode display.c will now ask for (3840px / the 8pt font's
// cell width is 480); a narrower screen or a wider font just leaves the
// tail of each row unused.
//
// It was 256, sized against a 1280px screen, and the failure it produced
// was quiet by design: sb_record_char() simply stops recording past
// SB_COLS, so scrollback of a wide console showed correct lines with
// their right-hand ends missing. Raise it alongside DISPLAY_MAX_W.
#define SB_LINES 256
#define SB_COLS  512

static char sb_char[SB_LINES][SB_COLS];
static uint8_t sb_attr[SB_LINES][SB_COLS];
static uint16_t sb_len[SB_LINES];      // used columns per line
static uint32_t sb_total;              // lines ever completed (monotonic)
static uint32_t sb_cur_len;            // columns used in the line being written
static uint32_t sb_view;               // 0 = live; N = showing N lines further back
static int sb_recording = 1;           // suppressed while redrawing, so a redraw can't record itself

static char *sb_line_chars(uint32_t line) { return sb_char[line % SB_LINES]; }
static uint8_t *sb_line_attrs(uint32_t line) { return sb_attr[line % SB_LINES]; }

// Oldest line still retained. Everything before it has been overwritten.
static uint32_t sb_oldest(void) {
    return sb_total >= SB_LINES ? sb_total - SB_LINES + 1 : 0;
}

static void sb_start_line(void) {
    sb_total++;
    sb_cur_len = 0;
    uint32_t idx = sb_total % SB_LINES;
    sb_len[idx] = 0;
}

// Records one character into the current scrollback line. Wrapping is
// recorded as a line break, matching what the screen actually showed --
// the alternative (unbounded logical lines re-wrapped at display time)
// would be more faithful to the text but would not reproduce the screen
// after a `fontsize` change, and reproducing the screen is the point.
static void sb_record(char c) {
    if (!sb_recording) return;
    if (c == '\n') { sb_start_line(); return; }
    if (c == '\r') { sb_cur_len = 0; return; }

    if (sb_cur_len >= SB_COLS) return; // beyond what's retained; the screen wraps and starts a new line
    uint32_t idx = sb_total % SB_LINES;
    sb_line_chars(sb_total)[sb_cur_len] = c;
    sb_line_attrs(sb_total)[sb_cur_len] = make_color(cur_fg, cur_bg);
    sb_cur_len++;
    if (sb_cur_len > sb_len[idx]) sb_len[idx] = (uint16_t)sb_cur_len;
}

static void sb_record_backspace(void) {
    if (!sb_recording || sb_cur_len == 0) return;
    sb_cur_len--;
    uint32_t idx = sb_total % SB_LINES;
    if (sb_len[idx] > sb_cur_len) sb_len[idx] = (uint16_t)sb_cur_len;
}

// Repaints the whole screen from the ring, ending `back` lines above the
// newest. Used for both directions of scrolling and for returning to
// live.
static void sb_repaint(uint32_t back) {
    uint32_t rows = (uint32_t)console_rows;
    if (rows == 0) return;

    enum vga_color saved_fg = cur_fg, saved_bg = cur_bg;
    sb_recording = 0;
    cursor_hide();

    // The window is `rows` lines ENDING at (total - back), clamped so it
    // never starts before the oldest retained line -- and then filled
    // DOWNWARD from that start, so scrolling to the very top shows a
    // full screen of history rather than one line stranded at the top
    // with blankness under it.
    uint32_t oldest = sb_oldest();
    uint32_t newest = sb_total - back;
    uint32_t first = (newest + 1 >= rows) ? newest + 1 - rows : 0;
    if (first < oldest) first = oldest;
    uint32_t last_possible = first + rows - 1;
    if (newest < last_possible) newest = last_possible > sb_total ? sb_total : last_possible;

    if (fb_mode) gfx_clear(palette_rgb(saved_bg));
    else legacy_clear();
    row = 0;
    col = 0;

    for (uint32_t line = first; line <= newest && row < rows; line++) {
        uint32_t idx = line % SB_LINES;
        uint32_t len = sb_len[idx];
        for (uint32_t i = 0; i < len && i < (uint32_t)console_cols; i++) {
            uint8_t attr = sb_line_attrs(line)[i];
            cur_fg = (enum vga_color)(attr & 0x0F);
            cur_bg = (enum vga_color)((attr >> 4) & 0x0F);
            if (fb_mode) {
                gfx_draw_char((int)(i * CELL_W), (int)(row * CELL_H), sb_line_chars(line)[i],
                              palette_rgb(cur_fg), palette_rgb(cur_bg));
            } else {
                buf[row * console_cols + i] = make_entry(sb_line_chars(line)[i], attr);
            }
        }
        row++;
        col = 0;
    }

    cur_fg = saved_fg;
    cur_bg = saved_bg;
    sb_recording = 1;

    if (back == 0) {
        // Back to live: the cursor belongs where output will continue.
        if (row > 0) row--;
        col = sb_len[sb_total % SB_LINES] < console_cols ? sb_len[sb_total % SB_LINES] : 0;
        cursor_show_and_reset_blink();
    }

    // A repaint is a whole new screen and it is the direct answer to a
    // PageUp/PageDown keypress, so it publishes immediately rather than
    // waiting for the idle loop -- this is not part of an output burst.
    vga_present();
}

void vga_scroll_back(int lines) {
    if (lines <= 0 || console_rows == 0) return;
    uint32_t available = sb_total - sb_oldest();
    // How far back the view may go: to the oldest line still retained.
    //
    // This deliberately does NOT subtract console_rows. The obvious
    // formula ("only scroll if there's more history than fits a screen")
    // is wrong here because vga_clear() wipes the screen without
    // discarding history -- and boot does exactly that, via
    // vga_reflow() when the persisted font size loads. After a clear the
    // screen shows three lines while the ring holds thirty, and the
    // subtracting version concluded there was nothing to scroll to.
    // How far back the view may go. When there's more history than fits,
    // that's "until the oldest line reaches the top of the screen".
    //
    // The `: 1` case matters and is easy to miss: vga_clear() wipes the
    // screen WITHOUT discarding history, and boot does exactly that via
    // vga_reflow() when the persisted font size loads. Afterwards the
    // screen shows three lines while the ring holds thirty -- all of
    // which "fit", so a formula based only on counting would conclude
    // there was nothing to scroll to. One step back is what reveals
    // them.
    uint32_t limit = available > (uint32_t)console_rows
                         ? available - (uint32_t)console_rows + 1
                         : 1;
    uint32_t want = sb_view + (uint32_t)lines;
    if (want > limit) want = limit;
    if (want == sb_view) return;
    sb_view = want;
    sb_repaint(sb_view);
}

void vga_scroll_forward(int lines) {
    if (lines <= 0 || sb_view == 0) return;
    sb_view = (uint32_t)lines >= sb_view ? 0 : sb_view - (uint32_t)lines;
    sb_repaint(sb_view);
}

int vga_scrolled_back(void) {
    return sb_view != 0;
}

// Any new output snaps the view back to the bottom first -- otherwise
// writes would land on a screen showing history, and the two would
// interleave into nonsense.
static void sb_snap_to_live(void) {
    if (sb_view == 0) return;
    sb_view = 0;
    sb_repaint(0);
}

// ---- public API ----

void vga_init(void) {
    legacy_color = make_color(VGA_LIGHT_GREY, VGA_BLACK);

    if (gfx_init()) {
        fb_mode = 1;
        // Draw into the back buffer, not at the display -- see the
        // double-buffering comment at the top of this file for why the
        // console must never read the framebuffer. Falls back to drawing
        // straight at the screen if the surface is too large for
        // back_buffer, which still works, just slowly on a real machine.
        gfx_set_double_buffered(1);
        console_cols = (size_t)gfx_width() / CELL_W;
        console_rows = (size_t)gfx_height() / CELL_H;
        fb_clear();
        // klog_write(), not vga_write() -- serial_init() has already run
        // by the time kernel.c calls vga_init() (see kernel.c's ordering),
        // so this reaches dmesg fine even though the physical console
        // isn't ready to receive its own "toy-os booting..." line yet.
        klog_write("vga: linear framebuffer console active (");
        klog_write_dec((uint32_t)gfx_width());
        klog_write("x");
        klog_write_dec((uint32_t)gfx_height());
        klog_write(")\n");
    } else {
        fb_mode = 0;
        legacy_clear();
        klog_write("vga: no linear framebuffer from GRUB -- legacy text-mode console\n");
    }
}

const struct vga_sink *vga_set_sink(const struct vga_sink *sink) {
    const struct vga_sink *prev = active_sink;
    active_sink = sink;
    return prev;
}

int vga_sink_active(void) {
    return active_sink != 0;
}

void vga_clear(void) {
    if (active_sink) {
        if (active_sink->clear) active_sink->clear(active_sink->ctx);
        return;
    }
    if (fb_mode) fb_clear();
    else legacy_clear();
}

void vga_resume(void) {
    if (!fb_mode) return;
    // The window manager turned double buffering on for itself and the
    // back buffer still holds its last frame, which has nothing to do
    // with the console's idea of where text is. Re-enable (a no-op if it
    // is already on -- but it also resets the dirty box, which is right:
    // nothing of the console's is pending) and repaint from scrollback,
    // so the screen and this file's row/col agree again.
    gfx_set_double_buffered(1);
    sb_repaint(0);
    // _force, so this does not depend on the caller having already
    // cleared the compositor registration. It has (win_server.c's
    // compositor_gone() runs after), but a repaint whose whole point is
    // "the desktop is gone, show the console" must not be silently
    // skippable by a future reordering.
    vga_present_force();
}

void vga_reflow(void) {
    if (!fb_mode) return; // legacy 80x25 text mode has no variable cell size
    console_cols = (size_t)gfx_width() / CELL_W;
    console_rows = (size_t)gfx_height() / CELL_H;
    // THE CLEAR IS SKIPPED WHILE A COMPOSITOR OWNS THE SCREEN. Recomputing
    // the geometry is always right; painting is not -- fb_clear() ends in
    // vga_present(), which blits the console's whole buffer over whatever
    // the desktop has on screen. `fontsize` at a running desktop wiped the
    // top two thirds of it, and the console the user cannot see is the one
    // that does not need clearing. Same reasoning as the suspended keyboard
    // reader (win_server_set_compositor()); see docs/conventions/gui.md.
    if (!win_server_any()) fb_clear();
}

uint32_t vga_rows(void) {
    if (active_sink) {
        return active_sink->rows ? active_sink->rows(active_sink->ctx) : 24;
    }
    return (uint32_t)(fb_mode ? console_rows : VGA_HEIGHT);
}

// Current console width in text columns -- 80 in legacy 80x25 text
// mode, or gfx_width()/gfx_char_w() in framebuffer mode (varies with
// the active font size, see vga_reflow()). Peer of vga_rows() above,
// added for the same reason: a caller doing its own line-wrapping math
// against the console (see apps/editor.c's windowed redraw) needs both
// dimensions, not just the row count console_page() already needed.
// Sinks don't track a column count any more than they track rows (see
// vga_rows()'s comment) -- 80 is a sane default for the same reason.
uint32_t vga_cols(void) {
    if (active_sink) return 80;
    return (uint32_t)(fb_mode ? console_cols : VGA_WIDTH);
}

// Called from keyboard_getchar()'s wait loop (see keyboard.c) on every
// wake-up, which happens on every interrupt including the 100Hz PIT
// tick -- so this runs roughly every 10ms while idle at a prompt, but
// only actually does anything (a fill_rect) once every
// CURSOR_BLINK_TICKS of those, toggling the cursor block on/off. A
// plain no-op in legacy text mode, where the hardware cursor already
// blinks on its own.
void vga_cursor_tick(void) {
    // A sink owns its own cursor presentation (or has none) -- nothing
    // for the physical console's blink logic to do while one's active.
    if (active_sink) return;
    if (!fb_mode) return;
    if (pit_ticks() - cursor_last_toggle_tick < CURSOR_BLINK_TICKS) return;
    cursor_last_toggle_tick = pit_ticks();
    // Safe mid-line now that hiding restores the pixels it saved -- this
    // used to be suppressed off the append point, because the old
    // erase-to-black hide would have eaten the character underneath.
    if (cursor_on_screen) cursor_hide();
    else cursor_draw();
    vga_present(); // the blink is its own event, not part of an output burst
}

// Repositions the insertion point without erasing anything -- see
// vga.h for the contract and why a line editor needs it.
//
// Works in text-cell space (one flat index over rows*cols) rather than
// per-row bookkeeping, so wrapping across a row boundary is just
// division and there's no separate "am I at column 0" case to get
// wrong.
void vga_cursor_move(int delta) {
    if (active_sink) return; // a sink owns its own cursor -- see vga_cursor_tick()
    if (delta == 0) return;
    sb_snap_to_live(); // moving the cursor is an edit; don't do it while scrolled back

    uint32_t cols = fb_mode ? console_cols : VGA_WIDTH;
    uint32_t rows = fb_mode ? console_rows : VGA_HEIGHT;
    if (cols == 0 || rows == 0) return;

    if (fb_mode) cursor_hide(); // restores whatever it covered, so moving
                                 // off a character no longer eats it

    long target = (long)row * (long)cols + (long)col + delta;
    if (target < 0) target = 0;
    long last = (long)rows * (long)cols - 1;
    if (target > last) target = last;

    row = (size_t)(target / (long)cols);
    col = (size_t)(target % (long)cols);

    if (fb_mode) {
        cursor_draw();
    } else {
        legacy_update_cursor(); // real hardware cursor; nothing to erase
    }
}

// Public wrapper around cursor_hide() -- every vga_putc()/fb_putc()
// call unconditionally repaints the cursor solid at the new (row, col)
// after drawing (see fb_putc()'s own comment), which is exactly the
// interactive-typing behavior wanted at a shell prompt but looks like
// a stray block hovering past a program's own output when nothing is
// actually waiting for a keystroke -- e.g. `stress`'s progress bar,
// which redraws in place (see stress_print_progress() in
// apps/shell_sys.c) and never calls vga_cursor_tick() during its tight
// write/read loop, so the cursor painted by its own trailing pad
// spaces just sits there solid instead of blinking. A caller doing
// exactly that can call this once after its own output to suppress it
// -- the next real vga_write()/vga_putc() (e.g. the next prompt) shows
// a fresh one again automatically, no explicit "show" call needed.
// No-op outside framebuffer mode, same as vga_cursor_tick() above --
// legacy text mode's hardware cursor isn't under this kind of
// per-character software control.
void vga_cursor_hide(void) {
    if (!fb_mode) return;
    cursor_hide();
}

void vga_set_color(enum vga_color fg, enum vga_color bg) {
    if (active_sink) {
        if (active_sink->set_color) active_sink->set_color(active_sink->ctx, fg, bg);
        return;
    }
    cur_fg = fg;
    cur_bg = bg;
    legacy_color = make_color(fg, bg);
}

enum vga_color vga_current_fg(void) {
    return cur_fg;
}

uint32_t vga_color_rgb(enum vga_color c) {
    return palette_rgb(c);
}

// ANSI ESCAPES ARE PARSED HERE, IN FRONT OF EVERYTHING ELSE -- before
// the sink check below, so a GUI Terminal's scrollback gets the COLOUR
// rather than the escape bytes, and both surfaces behave the same
// without either of them knowing what an escape is. One parser, because
// there is one console; a per-terminal one is the TTY milestone's job.
//
// The state it carries is why colour cannot simply be re-derived per
// call: a sequence arrives one byte at a time through vga_putc(), so
// `ESC [ 3 1 m` is five separate calls with the decision made on the
// last one.
static struct ansi_parser g_ansi;
static int g_ansi_armed;

// --- ANSI cursor and erase ------------------------------------------
//
// The console's own row/col ARE the terminal cursor -- there is no
// second position to keep in step, which is what makes this small.
//
// WHAT THIS DOES NOT FIX, stated because it is the reason cursor
// movement was swallowed until now: the SCROLLBACK records the byte
// stream (sb_record(), called from the printing path), so a program
// that paints a screen by moving the cursor leaves a history that is
// not a transcript of anything. Real terminals have the same problem
// and answer it with an alternate screen buffer, which this console
// does not have. Erasing does not touch the history either -- `ESC[2J`
// clears the SCREEN, and a terminal that threw away scrollback on it
// would be losing the user's data on a program's say-so. That is also
// why ED mode 3 ("and the scrollback") is treated as mode 2.
//
// Every op snaps to live first: moving or erasing is an edit, and doing
// one while the user is scrolled back would paint into a view of the
// past. vga_cursor_move() already had that rule.

// Clamps a 1-based ANSI coordinate to the console and stores it.
static void ansi_goto(size_t r, size_t c) {
    size_t rows = fb_mode ? console_rows : VGA_HEIGHT;
    size_t cols = fb_mode ? console_cols : VGA_WIDTH;
    if (rows == 0 || cols == 0) return;
    if (r >= rows) r = rows - 1;
    if (c >= cols) c = cols - 1;
    row = r;
    col = c;
}

// Blanks an inclusive span of cells, in reading order from (r0,c0) to
// (r1,c1). Uses the CURRENT background, so erasing after a colour
// change paints what the program asked for rather than the default --
// which is how a full-screen program gets a coloured backdrop.
static void erase_span(size_t r0, size_t c0, size_t r1, size_t c1) {
    size_t rows = fb_mode ? console_rows : VGA_HEIGHT;
    size_t cols = fb_mode ? console_cols : VGA_WIDTH;
    if (rows == 0 || cols == 0) return;
    if (r0 >= rows) return;
    if (r1 >= rows) r1 = rows - 1;
    if (c1 >= cols) c1 = cols - 1;

    for (size_t r = r0; r <= r1; r++) {
        size_t from = (r == r0) ? c0 : 0;
        size_t to = (r == r1) ? c1 : cols - 1;
        if (from > to || from >= cols) continue;
        if (fb_mode) {
            // One rect per row rather than one per cell: the whole
            // point of erasing is that it is cheaper than printing
            // spaces, and gfx_fill_rect is the call that makes it so.
            gfx_fill_rect((int)(from * CELL_W), (int)(r * CELL_H),
                          (int)((to - from + 1) * CELL_W), (int)CELL_H,
                          palette_rgb(cur_bg));
        } else {
            for (size_t x = from; x <= to; x++)
                buf[r * VGA_WIDTH + x] = make_entry(' ', legacy_color);
        }
    }
}

static size_t g_saved_row, g_saved_col;

static void vga_ansi_ctrl(const struct ansi_parser *p) {
    if (active_sink) return;  // a logger has no cursor -- see vga_sink

    sb_snap_to_live();
    if (fb_mode) cursor_hide();   // erase it before row/col move, as
                                   // vga_cursor_move() does

    size_t rows = fb_mode ? console_rows : VGA_HEIGHT;
    size_t cols = fb_mode ? console_cols : VGA_WIDTH;
    size_t n = p->a;

    switch (p->op) {
    case ANSI_OP_MOVE_TO: ansi_goto(p->a - 1, p->b - 1); break;
    case ANSI_OP_UP:      ansi_goto(row > n ? row - n : 0, col); break;
    case ANSI_OP_DOWN:    ansi_goto(row + n, col); break;
    case ANSI_OP_RIGHT:   ansi_goto(row, col + n); break;
    case ANSI_OP_LEFT:    ansi_goto(row, col > n ? col - n : 0); break;
    case ANSI_OP_COLUMN:  ansi_goto(row, p->a - 1); break;
    case ANSI_OP_ROW:     ansi_goto(p->a - 1, col); break;

    case ANSI_OP_ERASE_DISPLAY:
        if (p->a == 0)      erase_span(row, col, rows - 1, cols - 1);
        else if (p->a == 1) erase_span(0, 0, row, col);
        else {
            erase_span(0, 0, rows - 1, cols - 1);
            // ED 2 does NOT home the cursor -- that is `ESC[H` and
            // programs send both. Clearing and homing together is a
            // common shortcut and it breaks anything that erases, then
            // positions, then draws.
        }
        break;

    case ANSI_OP_ERASE_LINE:
        if (p->a == 0)      erase_span(row, col, row, cols - 1);
        else if (p->a == 1) erase_span(row, 0, row, col);
        else                erase_span(row, 0, row, cols - 1);
        break;

    case ANSI_OP_SAVE:    g_saved_row = row; g_saved_col = col; break;
    case ANSI_OP_RESTORE: ansi_goto(g_saved_row, g_saved_col); break;

    case ANSI_OP_HIDE:
        // cursor_hide() above already took it off the screen; the flag
        // is what stops the next draw putting it back.
        cursor_suppressed = 1;
        break;
    case ANSI_OP_SHOW:
        cursor_suppressed = 0;
        break;
    default: break;
    }

    if (fb_mode) {
        cursor_show_and_reset_blink();
        vga_present();  // a cursor-addressed program paints in bursts and
                         // may never emit a newline, so nothing else would
                         // push this to the screen
    } else {
        legacy_update_cursor();
    }
}

void vga_putc(char c) {
    if (!g_ansi_armed) { ansi_init(&g_ansi, cur_fg, cur_bg); g_ansi_armed = 1; }
    switch (ansi_feed(&g_ansi, c)) {
    case ANSI_PASS:
        break;
    case ANSI_SGR:
        vga_set_color(g_ansi.fg, g_ansi.bg);
        return;
    case ANSI_CTRL:
        vga_ansi_ctrl(&g_ansi);
        return;
    case ANSI_EATEN:
    default:
        return;
    }

    if (active_sink) {
        if (c == '\b') {
            if (active_sink->backspace) active_sink->backspace(active_sink->ctx);
        } else {
            if (active_sink->putc) active_sink->putc(active_sink->ctx, c);
        }
        return;
    }
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
    sb_snap_to_live();

    // A newline while a non-default background is set PADS TO THE END OF
    // THE LINE with spaces instead of just moving down, so a coloured
    // run reads as one full-width band (a panic banner) rather than as a
    // ragged highlight whose right edge depends on the text length.
    //
    // Done here, in the one funnel, so the padding goes through
    // sb_record() as well -- a fix applied inside fb_putc() alone would
    // look right until the user pressed PageUp and the line redrew from
    // scrollback without it.
    //
    // The padding REPLACES the newline: writing to the last column wraps
    // (col back to 0, row++), which is the same move, so emitting '\n'
    // as well would leave a blank line behind.
    if (c == '\n' && cur_bg != CONSOLE_DEFAULT_BG) {
        // The count is computed ONCE, before any space is written.
        // Looping on `col < width` instead does not terminate: writing
        // the last column wraps col back to 0, which satisfies the
        // condition again, and the console fills solid.
        size_t width = fb_mode ? console_cols : VGA_WIDTH;
        size_t pad = width - col;
        for (size_t i = 0; i < pad; i++) {
            sb_record(' ');
            if (fb_mode) fb_putc(' ');
            else legacy_putc(' ');
        }
        // The screen's line break came from the wrap above, but
        // scrollback's has to be recorded explicitly -- sb_record('\n')
        // is what calls sb_start_line(). Without it every padded line
        // accumulates into ONE scrollback line, and a PageUp/PageDown
        // round trip redraws the banner as a single stripe with four of
        // its five lines missing.
        sb_record('\n');
        fb_present_throttled();
        return;
    }

    sb_record(c);
    if (fb_mode) fb_putc(c);
    else legacy_putc(c);
    fb_present_throttled();
}

void vga_backspace(void) {
    if (active_sink) {
        if (active_sink->backspace) active_sink->backspace(active_sink->ctx);
        return;
    }
    sb_snap_to_live();
    sb_record_backspace();
    if (fb_mode) fb_backspace();
    else legacy_backspace();
}

void vga_write(const char *s) {
    while (*s) vga_putc(*s++);
}

// These two are now three lines each over knum.h's converters. They
// used to be a full digit loop apiece -- and klog.c had its own
// identical pair, with a comment explaining that duplicating them was
// cheaper than the dependency. knum.c is that dependency, deliberately
// depending on nothing itself, so both sinks can share one
// implementation (and one set of tests) without either pulling in the
// other.
void vga_write_dec(uint32_t n) {
    char buf[21];
    k_utoa(n, buf, sizeof buf);
    vga_write(buf);
}

void vga_write_exit_code(int code) {
    // Negative here only ever means PROCESS_CRASHED (process.h) in
    // practice -- no real exit code produces one (see its comment) --
    // but this stays generic ("any negative value") rather than
    // importing process.h and comparing against that exact constant, to
    // avoid a driver (this file) depending on kernel/core.
    if (code < 0) {
        vga_write("CRASHED");
    } else {
        vga_write_dec((uint32_t)code);
    }
}

void vga_write_hex(uint64_t n) {
    char buf[17];
    k_htoa(n, buf, sizeof buf, 0); // 0 = shortest form, no leading zeros
    vga_write("0x");
    vga_write(buf);
}
