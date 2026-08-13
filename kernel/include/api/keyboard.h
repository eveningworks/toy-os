#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

// Special key codes pushed into the input stream alongside normal ASCII.
// Chosen outside the 0-127 ASCII range so they can't collide with real chars.
#define KEY_ARROW_UP    0x91
#define KEY_ARROW_DOWN  0x92
#define KEY_PAGE_UP     0x93
#define KEY_PAGE_DOWN   0x94
#define KEY_ARROW_LEFT  0x95
#define KEY_ARROW_RIGHT 0x96
#define KEY_HOME        0x97
#define KEY_END         0x98
#define KEY_DELETE      0x99
#define KEY_F2          0x9A
#define KEY_F3          0x9B
// Shift+arrow/Home/End -- distinct codes rather than a separate
// "modifier held" query, so apps that want selection (Notepad) just
// switch on one more case, and apps that don't (Terminal, the CLI
// editor) simply never see these and keep working exactly as before.
// Emitted by keyboard.c at the moment the scancode is processed (same
// place shift already picks between scancode_ascii/scancode_ascii_shift
// for a letter key), not derived later from some live "is shift down
// right now" state an app would have to poll itself -- see
// docs/decisions.md for why that timing matters.
#define KEY_SHIFT_ARROW_LEFT  0x9C
#define KEY_SHIFT_ARROW_RIGHT 0x9D
#define KEY_SHIFT_ARROW_UP    0x9E
#define KEY_SHIFT_ARROW_DOWN  0x9F
#define KEY_SHIFT_HOME        0xA0
#define KEY_SHIFT_END         0xA1

// The six Latin-1 codepoints this build's font (font_ttf.h,
// tools/genttf.py) and `se` keyboard layout (keyboard.c) support --
// uppercase/lowercase Å/Ä/Ö. Comfortably clear of both the ASCII range
// and the KEY_* codes above (0x91-0x9B), so they can travel through the
// same uint16_t input stream as everything else with no collision.
// See docs/decisions.md's Nordic-keyboard entry for why Latin-1 over
// UTF-8, and why this is 6 specific codepoints rather than the full
// 0xA0-0xFF Latin-1 Supplement block.
#define CHAR_A_DIAERESIS      0xC4 // Ä
#define CHAR_O_DIAERESIS      0xD6 // Ö
#define CHAR_A_RING           0xC5 // Å
#define CHAR_A_DIAERESIS_LC   0xE4 // ä
#define CHAR_O_DIAERESIS_LC   0xF6 // ö
#define CHAR_A_RING_LC        0xE5 // å

// True if `k` is one of the six Nordic letters above. A plain
// six-way OR rather than a range check, since these codepoints (0xC4,
// 0xD6, 0xC5, 0xE4, 0xF6, 0xE5) aren't contiguous.
#define IS_NORDIC_CHAR(k) ((k) == CHAR_A_DIAERESIS || (k) == CHAR_O_DIAERESIS || \
                            (k) == CHAR_A_RING || (k) == CHAR_A_DIAERESIS_LC || \
                            (k) == CHAR_O_DIAERESIS_LC || (k) == CHAR_A_RING_LC)

// True if `k` is a character that should be inserted into typed text --
// printable ASCII (32-126) or one of the Nordic letters above. Every
// "is this key a printable char, not a control/arrow/function key"
// gate across apps/ (terminal, notepad, widgets textfield, editor)
// should use this instead of a bare `key >= 32 && key < 127`, which
// silently excludes Nordic letters (and, before this build, would also
// have gone through `char`'s signedness as a landmine -- see
// docs/decisions.md). userland/echo.c can't include this header (it's
// a freestanding ring-3 program with no kernel headers) and keeps its
// own copy of the same check.
#define IS_PRINTABLE_KEY(k) (((k) >= 32 && (k) < 127) || IS_NORDIC_CHAR(k))

// Scancode->character translation itself lives in
// kernel/include/api/keyboard_layout.h / kernel/lib/keyboard_layout.c now
// -- data-driven from /etc/kbs/<name> files rather than a compiled-in
// enum of two hardcoded layouts. See that header's top comment and
// docs/decisions.md. keyboard.c (this driver) only owns raw
// scancode/shift-state/extended-prefix handling; it calls into
// keyboard_layout_translate() for the actual character.

// Called by i8042_poll() with one byte already read from the shared
// PS/2 data port. Don't call this from an IRQ handler directly.
void keyboard_feed_byte(uint8_t sc);

// Blocking read of a single byte from the input stream: either an ASCII
// char or one of the KEY_* codes above.
int keyboard_getchar(void);

// Same as keyboard_getchar but returns -1 immediately if nothing is
// waiting, instead of blocking. Used by the GUI event loop.
int keyboard_try_getchar(void);

// Blocking read of one line into buf (max len-1 chars + null terminator).
// Echoes typed characters to the VGA console and handles backspace.
void keyboard_read_line(char *buf, unsigned int len);

#endif
