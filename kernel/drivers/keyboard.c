#include "keyboard.h"
#include "keyboard_layout.h"
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
// -- they're plain scancodes like any letter key, just two no layout
// table maps to anything (scancode 0x3C/0x3D are unmapped -- 0, same
// "nothing happens" as any other unmapped slot). Checked explicitly,
// before the layout translation, same as the shift keys below them.
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
    char c = keyboard_layout_translate(sc, shift_pressed);
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
