#include "keyboard.h"
#include "io.h"
#include "vga.h"
#include "klog.h"
#include "debug_console.h"

#define KBD_DATA_PORT 0x60

static volatile uint16_t ring_buf[256];
static volatile unsigned int ring_head = 0;
static volatile unsigned int ring_tail = 0;
static int shift_pressed = 0;
static int extended_prefix = 0;
static enum keyboard_layout current_layout = KB_LAYOUT_US;

// US QWERTY scancode set 1, unshifted
static const char scancode_ascii[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t', 'q','w','e','r','t','y','u','i','o','p','[',']', '\n',
    0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, '\\', 'z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ', 0,
};

// Shifted variant
static const char scancode_ascii_shift[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t', 'Q','W','E','R','T','Y','U','I','O','P','{','}', '\n',
    0, 'A','S','D','F','G','H','J','K','L',':','"','~',
    0, '|', 'Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0, ' ', 0,
};

// Swedish/Finnish physical layout -- identical to scancode_ascii[]
// except at the three scancodes whose physical keycap is Å/Ä/Ö on a
// real Nordic keyboard: scancode 0x1A (US '['), 0x27 (US ';'), and
// 0x28 (US '\''). Deliberately NOT a from-scratch remap of every key
// (AltGr-level symbols like @/{/} live elsewhere on a real Nordic
// keyboard too, but there's no AltGr handling in this driver at all --
// see keyboard.h's IS_NORDIC_CHAR() comment) -- this covers exactly
// the three letters the user asked for (Ä/Ö/Å) at their real physical
// positions, everything else stays US QWERTY. The values here are
// Latin-1 codepoints (font_ttf.h bakes glyphs for exactly these, see
// tools/genttf.py's EXTRA_CHARS) stored as `char` -- note this build
// has no -funsigned-char, so these bit patterns are negative as `char`
// but every reader casts through (unsigned char) before use (see
// keyboard_feed_byte()'s ring_push() call and gfx.c's
// font_ttf_glyph_index()) rather than relying on char's signedness.
static const char scancode_ascii_se[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t', 'q','w','e','r','t','y','u','i','o','p', (char)CHAR_A_RING_LC, ']', '\n',
    0, 'a','s','d','f','g','h','j','k','l', (char)CHAR_O_DIAERESIS_LC, (char)CHAR_A_DIAERESIS_LC, '`',
    0, '\\', 'z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ', 0,
};

static const char scancode_ascii_shift_se[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t', 'Q','W','E','R','T','Y','U','I','O','P', (char)CHAR_A_RING, '}', '\n',
    0, 'A','S','D','F','G','H','J','K','L', (char)CHAR_O_DIAERESIS, (char)CHAR_A_DIAERESIS, '~',
    0, '|', 'Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0, ' ', 0,
};

void keyboard_set_layout(enum keyboard_layout layout) {
    current_layout = layout;
    // Called once at boot (keyboard_config_init(), applying whatever
    // was persisted to /etc/toyos.conf) and again any time the `keyboard`
    // shell command switches it live -- logging here covers both
    // call sites for free instead of needing a separate log line at
    // each caller.
    klog_write("keyboard: layout set to ");
    klog_write(keyboard_layout_name(layout));
    klog_write("\n");
}

enum keyboard_layout keyboard_get_layout(void) {
    return current_layout;
}

const char *keyboard_layout_name(enum keyboard_layout layout) {
    return layout == KB_LAYOUT_SE ? "se" : "us";
}

#define LEFT_SHIFT_PRESS   0x2A
#define LEFT_SHIFT_RELEASE 0xAA
#define RIGHT_SHIFT_PRESS  0x36
#define RIGHT_SHIFT_RELEASE 0xB6

static void ring_push(uint16_t c) {
    unsigned int next = (ring_head + 1) % 256;
    if (next == ring_tail) return; // full, drop
    ring_buf[ring_head] = c;
    ring_head = next;
}

static int ring_pop(uint16_t *out) {
    if (ring_tail == ring_head) return 0; // empty
    *out = ring_buf[ring_tail];
    ring_tail = (ring_tail + 1) % 256;
    return 1;
}

#define SC_ARROW_UP    0x48
#define SC_ARROW_DOWN  0x50
#define SC_PAGE_UP     0x49
#define SC_PAGE_DOWN   0x51
#define SC_ARROW_LEFT  0x4B
#define SC_ARROW_RIGHT 0x4D
#define SC_HOME        0x47
#define SC_END         0x4F
#define SC_DELETE      0x53

// F2/F3, unlike the keys above, aren't 0xE0-prefixed extended scancodes
// -- they're plain scancodes like any letter key, just two this file
// didn't give a ring_push() before (scancode_ascii[0x3C]/[0x3D] are 0,
// their default zero-initialized value, so they silently did nothing on
// press). Checked explicitly, before the ASCII table lookup, same as
// the shift keys below them.
#define SC_F2 0x3C
#define SC_F3 0x3D

// Processes one byte already read from the 8042 by i8042_poll(). This
// must NOT read port 0x60 itself -- see i8042.h for why.
void keyboard_feed_byte(uint8_t sc) {

    if (sc == 0xE0) {
        extended_prefix = 1;
        return;
    }

    if (extended_prefix) {
        extended_prefix = 0;
        if (!(sc & 0x80)) { // key press, not release
            // Shift+arrow/Home/End get their own codes, decided right
            // here from the live `shift_pressed` state -- same timing
            // as the ASCII table swap below for ordinary letter keys,
            // so a shift release racing the arrow keypress resolves the
            // same way either family of key already does.
            if (sc == SC_ARROW_UP) ring_push(shift_pressed ? KEY_SHIFT_ARROW_UP : KEY_ARROW_UP);
            else if (sc == SC_ARROW_DOWN) ring_push(shift_pressed ? KEY_SHIFT_ARROW_DOWN : KEY_ARROW_DOWN);
            else if (sc == SC_PAGE_UP) ring_push(KEY_PAGE_UP);
            else if (sc == SC_PAGE_DOWN) ring_push(KEY_PAGE_DOWN);
            else if (sc == SC_ARROW_LEFT) ring_push(shift_pressed ? KEY_SHIFT_ARROW_LEFT : KEY_ARROW_LEFT);
            else if (sc == SC_ARROW_RIGHT) ring_push(shift_pressed ? KEY_SHIFT_ARROW_RIGHT : KEY_ARROW_RIGHT);
            else if (sc == SC_HOME) ring_push(shift_pressed ? KEY_SHIFT_HOME : KEY_HOME);
            else if (sc == SC_END) ring_push(shift_pressed ? KEY_SHIFT_END : KEY_END);
            else if (sc == SC_DELETE) ring_push(KEY_DELETE);
        }
        return;
    }

    if (sc == LEFT_SHIFT_PRESS || sc == RIGHT_SHIFT_PRESS) {
        shift_pressed = 1;
        return;
    }
    if (sc == LEFT_SHIFT_RELEASE || sc == RIGHT_SHIFT_RELEASE) {
        shift_pressed = 0;
        return;
    }
    if (sc & 0x80) return; // other key releases ignored

    if (sc == SC_F2) { ring_push(KEY_F2); return; }
    if (sc == SC_F3) { ring_push(KEY_F3); return; }

    if (sc >= 128) return;
    const char *table;
    if (current_layout == KB_LAYOUT_SE) {
        table = shift_pressed ? scancode_ascii_shift_se : scancode_ascii_se;
    } else {
        table = shift_pressed ? scancode_ascii_shift : scancode_ascii;
    }
    char c = table[sc];
    if (c) ring_push((uint8_t)c);
}

int keyboard_getchar(void) {
    uint16_t c;
    while (!ring_pop(&c)) {
        // hlt wakes on every interrupt, not just a real keypress -- most
        // commonly the 100Hz PIT tick -- so this is a convenient, cheap
        // place to drive the framebuffer console's blinking cursor while
        // otherwise idle waiting for input. vga_cursor_tick() gates its
        // own actual work internally, so calling it this often costs
        // nothing on the ticks where it doesn't toggle. debug_console_poll()
        // rides the same wakeup for the same reason -- this is the
        // physical shell's main idle point, so a serial debug session
        // stays responsive whenever nobody's actively typing at the
        // physical console (see docs/decisions.md for the honest
        // limitation: it does NOT get polled while a blocking command,
        // the GUI's own event loop, or a ring-3 process is running --
        // apps/wm/wm.c's loop covers the GUI case separately).
        vga_cursor_tick();
        debug_console_poll();
        __asm__ volatile ("hlt");
    }
    return c;
}

int keyboard_try_getchar(void) {
    uint16_t c;
    if (!ring_pop(&c)) return -1;
    return c;
}

void keyboard_read_line(char *buf, unsigned int len) {
    unsigned int pos = 0;
    for (;;) {
        int c = keyboard_getchar();
        // Ignore special keys (arrows, F2/F3, ...) in this simple reader,
        // but let Nordic letters through -- they also live at codepoints
        // >= 128, just not in the KEY_* range those special keys use.
        if (c >= 128 && !IS_NORDIC_CHAR(c)) continue;

        if (c == '\n') {
            vga_putc('\n');
            break;
        } else if (c == '\b') {
            if (pos > 0) {
                pos--;
                vga_backspace();
            }
        } else if (pos < len - 1) {
            buf[pos++] = c;
            vga_putc(c);
        }
    }
    buf[pos] = '\0';
}
