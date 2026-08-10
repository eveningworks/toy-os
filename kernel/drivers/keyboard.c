#include "keyboard.h"
#include "io.h"
#include "vga.h"

#define KBD_DATA_PORT 0x60

static volatile uint16_t ring_buf[256];
static volatile unsigned int ring_head = 0;
static volatile unsigned int ring_tail = 0;
static int shift_pressed = 0;
static int extended_prefix = 0;

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

#define SC_ARROW_UP   0x48
#define SC_ARROW_DOWN 0x50
#define SC_PAGE_UP    0x49
#define SC_PAGE_DOWN  0x51

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
            if (sc == SC_ARROW_UP) ring_push(KEY_ARROW_UP);
            else if (sc == SC_ARROW_DOWN) ring_push(KEY_ARROW_DOWN);
            else if (sc == SC_PAGE_UP) ring_push(KEY_PAGE_UP);
            else if (sc == SC_PAGE_DOWN) ring_push(KEY_PAGE_DOWN);
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

    if (sc >= 128) return;
    char c = shift_pressed ? scancode_ascii_shift[sc] : scancode_ascii[sc];
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
        // nothing on the ticks where it doesn't toggle.
        vga_cursor_tick();
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
        if (c >= 128) continue; // ignore special keys in this simple reader

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
