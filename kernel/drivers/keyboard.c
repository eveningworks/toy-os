#include "keyboard.h"
#include "keyboard_layout.h"
#include "io.h"
#include "vga.h"
#include "klog.h"
#include "debug_console.h"

#define KBD_DATA_PORT 0x60

// Each slot is (mods << 16) | key -- see keyboard.h's "Modifier bits"
// comment. The KEY is unchanged from what this driver has always
// pushed (terminal-encoded: Ctrl-A is 0x01, Alt-B is ESC then 'b'), so
// every existing consumer that calls keyboard_getchar() and gets the
// low half back behaves exactly as before. The mods half is additional
// information for callers that need to tell Shift-Tab from Tab, which
// the terminal encoding genuinely cannot express.
static volatile uint32_t ring_buf[256];
static volatile unsigned int ring_head = 0;
static volatile unsigned int ring_tail = 0;
static int shift_pressed = 0;
static int altgr_pressed = 0;
static int ctrl_pressed = 0;
static int alt_pressed = 0;   // LEFT Alt only -- right Alt is AltGr, see below
static int extended_prefix = 0;

#define LEFT_SHIFT_PRESS   0x2A
#define LEFT_SHIFT_RELEASE 0xAA
#define RIGHT_SHIFT_PRESS  0x36
#define RIGHT_SHIFT_RELEASE 0xB6

// Right Alt = AltGr on a PS/2 keyboard, sent as an 0xE0-prefixed
// (extended) scancode. Left Alt is the SAME 0x38/0xB8 byte pair
// without the prefix, which is what makes telling them apart free:
// the extended block below sees only AltGr, the plain path below sees
// only left Alt. That split matters here -- AltGr is a layout modifier
// (it picks a third character from the keyboard layout tables), while
// left Alt is readline's Meta. Conflating them would make `AltGr-b`
// try to be Meta-b on a Nordic layout.
#define RIGHT_ALT_PRESS    0x38
#define RIGHT_ALT_RELEASE  0xB8
#define LEFT_ALT_PRESS     0x38
#define LEFT_ALT_RELEASE   0xB8

// Left Ctrl is plain 0x1D/0x9D; right Ctrl is the same pair with an
// 0xE0 prefix. Both set the same state -- nothing here distinguishes
// them, same as the two Shift keys.
#define CTRL_PRESS         0x1D
#define CTRL_RELEASE       0x9D

// The modifiers physically held RIGHT NOW. Sampled by ring_push() at
// the moment a key is pushed -- i.e. at scancode-processing time, the
// same instant the layout table decides between 'a' and 'A'.
//
// That timing is the whole point, and is why this is captured here
// rather than exposed as a "what is held now?" query an app polls
// later: a modifier release racing a keypress then resolves the same
// way for the mods as it already does for the character itself. See
// docs/decisions.md -- the Shift+arrow family was given discrete codes
// for exactly this reason, and this generalises that decision rather
// than reversing it.
static uint8_t current_mods(void) {
    uint8_t m = 0;
    if (shift_pressed) m |= KEY_MOD_SHIFT;
    if (ctrl_pressed) m |= KEY_MOD_CTRL;
    if (alt_pressed) m |= KEY_MOD_ALT;
    if (altgr_pressed) m |= KEY_MOD_ALTGR;
    return m;
}

static void ring_push(uint16_t c) {
    unsigned int next = (ring_head + 1) % 256;
    if (next == ring_tail) return; // full, drop
    ring_buf[ring_head] = ((uint32_t)current_mods() << 16) | c;
    ring_head = next;
}

static int ring_pop(uint32_t *out) {
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

// The function keys, unlike the keys above, aren't 0xE0-prefixed
// extended scancodes -- they're plain scancodes like any letter key,
// just ones no layout table maps to anything (0x3C-0x3E and 0x44 are
// unmapped -- 0, same "nothing happens" as any other unmapped slot).
// Checked explicitly, before the layout translation, same as the shift
// keys below them.
//
// Only the four with callers exist: F2/F3 (the file manager), F10 (focus
// the menu bar) and F4 (Alt+F4 closes a window). Pushing them here and
// not through the layout translation is also what keeps Alt+F4 whole --
// it returns before the Alt-prefixes-with-ESC path below, so the key
// arrives once, with KEY_MOD_ALT set, rather than as ESC + something.
#define SC_F2 0x3C
#define SC_F3 0x3D
#define SC_F4  0x3E
#define SC_F10 0x44

// Processes one byte already read from the 8042 by i8042_poll(). This
// must NOT read port 0x60 itself -- see i8042.h for why.
void keyboard_feed_byte(uint8_t sc) {

    if (sc == 0xE0) {
        extended_prefix = 1;
        return;
    }

    if (extended_prefix) {
        extended_prefix = 0;
        if (sc == RIGHT_ALT_PRESS) { altgr_pressed = 1; return; }
        if (sc == RIGHT_ALT_RELEASE) { altgr_pressed = 0; return; }
        if (sc == CTRL_PRESS) { ctrl_pressed = 1; return; }   // right Ctrl
        if (sc == CTRL_RELEASE) { ctrl_pressed = 0; return; }
        if (!(sc & 0x80)) { // key press, not release
            // Ctrl+Left/Right are word motion in every readline-ish
            // line editor, so they get their own codes -- exactly the
            // KEY_SHIFT_ARROW_* precedent right below, resolved here
            // from live modifier state at keypress time for the same
            // reason (see docs/decisions.md).
            if (ctrl_pressed && sc == SC_ARROW_LEFT) { ring_push(KEY_CTRL_ARROW_LEFT); return; }
            if (ctrl_pressed && sc == SC_ARROW_RIGHT) { ring_push(KEY_CTRL_ARROW_RIGHT); return; }
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
    if (sc == CTRL_PRESS) { ctrl_pressed = 1; return; }       // left Ctrl
    if (sc == CTRL_RELEASE) { ctrl_pressed = 0; return; }
    if (sc == LEFT_ALT_PRESS) { alt_pressed = 1; return; }    // Meta -- not AltGr, see above
    if (sc == LEFT_ALT_RELEASE) { alt_pressed = 0; return; }
    if (sc & 0x80) return; // other key releases ignored

    if (sc == SC_F2) { ring_push(KEY_F2); return; }
    if (sc == SC_F3) { ring_push(KEY_F3); return; }
    if (sc == SC_F4) { ring_push(KEY_F4); return; }
    if (sc == SC_F10) { ring_push(KEY_F10); return; }

    if (sc >= 128) return;
    char c = keyboard_layout_translate(sc, shift_pressed, altgr_pressed);
    if (!c) return;

    // Ctrl and Alt are encoded the way a real terminal encodes them --
    // see keyboard.h's "Ctrl and Alt" comment for the full reasoning.
    //
    // Ctrl folds a letter to its control code (Ctrl-A -> 0x01), which
    // is why Ctrl-H/I/J/M come out as backspace/tab/newline/return with
    // no special cases: in this encoding they ARE those keys, exactly as
    // in bash. Ctrl with anything that isn't a letter is dropped rather
    // than guessed at -- Ctrl-[ really is Esc on a physical terminal,
    // but nothing here wants that, and inventing codes for the rest
    // would be making up an encoding instead of following one.
    if (ctrl_pressed) {
        char lower = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        if (lower >= 'a' && lower <= 'z') ring_push((uint16_t)(lower - 'a' + 1));
        return;
    }

    // Alt (Meta) prefixes the key with ESC, so Alt-B arrives as the two
    // bytes 0x1B 'b'. Two pushes rather than one combined code: this is
    // what every terminal emulator sends, so the line editor's decoder
    // is the same one it would need for a real serial terminal anyway.
    if (alt_pressed) {
        ring_push(0x1B);
    }
    ring_push((uint8_t)c);
}

int keyboard_getchar(void) { return keyboard_getchar_mods(0); }

int keyboard_getchar_mods(uint8_t *out_mods) {
    uint32_t ev;
    for (;;) {
    while (!ring_pop(&ev)) {
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

    // PageUp/PageDown scroll the console's history rather than reaching
    // the caller (see vga.h's scrollback section). Handled here, in the
    // BLOCKING reader, so it works at the shell prompt, in the CLI
    // editor, anywhere the kernel waits for a key -- and deliberately
    // NOT in keyboard_try_getchar(), which is what the window manager
    // polls: the GUI Terminal and Notepad have their own PageUp/PageDown
    // scrolling of their own widgets, and swallowing the keys here would
    // break both.
    int c = (int)(ev & 0xFFFF);
    if (c == KEY_PAGE_UP || c == KEY_PAGE_DOWN) {
        uint32_t page = vga_rows() > 2 ? vga_rows() - 2 : 1; // keep two lines of overlap
        if (c == KEY_PAGE_UP) vga_scroll_back((int)page);
        else vga_scroll_forward((int)page);
        continue; // keep waiting for a key the caller actually wants
    }
    if (out_mods) *out_mods = (uint8_t)(ev >> 16);
    return c;
    }
}

int keyboard_try_getchar(void) { return keyboard_try_getchar_mods(0); }

int keyboard_try_getchar_mods(uint8_t *out_mods) {
    uint32_t ev;
    if (!ring_pop(&ev)) return -1;
    if (out_mods) *out_mods = (uint8_t)(ev >> 16);
    return (int)(ev & 0xFFFF);
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
