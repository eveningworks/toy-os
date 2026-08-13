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
#include "klog.h"
#include "knum.h"
#include "string.h"
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
static void cursor_draw(void) {
    if (cursor_on_screen) return;

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
// its size is a compile-time constant either way. 256 lines x 256 cols
// x 2 bytes = 128KB. 256 columns covers the widest this console gets
// (1280px / the 8pt font's cell width); a narrower font just leaves the
// tail of each row unused.
#define SB_LINES 256
#define SB_COLS  256

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

void vga_reflow(void) {
    if (!fb_mode) return; // legacy 80x25 text mode has no variable cell size
    console_cols = (size_t)gfx_width() / CELL_W;
    console_rows = (size_t)gfx_height() / CELL_H;
    fb_clear();
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

void vga_putc(char c) {
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
    sb_record(c);
    if (fb_mode) fb_putc(c);
    else legacy_putc(c);
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
