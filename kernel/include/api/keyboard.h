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

// Ctrl+Left/Right -- word motion in any readline-style line editor
// (kernel/lib/klineedit.c). Distinct codes for the same reason the
// Shift+arrow family above has them.
#define KEY_CTRL_ARROW_LEFT   0xA2
#define KEY_CTRL_ARROW_RIGHT  0xA3

// F10 -- focuses an application's menu bar (ui/uui_menubar.h), which is
// what it does on Windows and in KDE. It exists rather than Alt+letter
// mnemonics because Alt is encoded terminal-style as an ESC PREFIX (see
// "Ctrl and Alt" below), so Alt-F arrives as ESC then 'f' and cannot be
// told apart from the Esc that has to close the menu. F10 has no such
// ambiguity, and is the binding both of those desktops offer anyway.
#define KEY_F10               0xA4

// F4 -- exists for Alt+F4, which CLOSES the focused window. That is a
// window-manager shortcut (apps/wm/wm.c intercepts it before routing
// keys to the focused window), the way it is in Windows and KDE, not a
// key an app handles. Matched as KEY_F4 plus KEY_MOD_ALT rather than
// given a combined KEY_ALT_F4 code the way the Shift+arrow family was:
// nothing is folded for a function key, so the modifier bits are usable
// here, and this generalises to a future Alt+F<n> for free.
#define KEY_F4                0xA5

// ---- Ctrl and Alt ----
//
// These do NOT get KEY_* codes of their own. They're encoded the way a
// real terminal encodes them, which is what bash and every other
// readline program already expect:
//
//   Ctrl-<letter>  ->  the control code, 0x01-0x1A. Ctrl-A is 0x01,
//                      Ctrl-E is 0x05, Ctrl-W is 0x17.
//   Alt-<key>      ->  ESC (0x1B) followed by the key itself, so
//                      Alt-B arrives as the two-byte sequence 0x1B 'b'.
//                      This is readline's "meta prefix".
//
// Two consequences worth knowing before adding a binding:
//
// 1. Ctrl-H, Ctrl-I, Ctrl-J and Ctrl-M are indistinguishable from
//    backspace, Tab, newline and Return -- because in this encoding
//    they ARE those keys. That's correct, not a collision to work
//    around: it's exactly how they behave in a terminal, so
//    Ctrl-H-as-backspace and Ctrl-I-as-completion come out right with
//    no code at all.
// 2. A lone Esc and the start of an Alt sequence look identical at
//    this layer, which is a real ambiguity a physical terminal has
//    too. The line editor resolves it by holding the ESC and deciding
//    on the NEXT key (see klineedit.h); anything that needs a bare Esc
//    -- leaving GUI mode, exiting the editor -- sees it unchanged
//    because those consumers never sit inside a line edit.
//
// Ctrl with a non-letter is dropped rather than assigned a made-up
// code, and AltGr is deliberately NOT Meta: it stays a layout modifier
// so a Nordic layout's third-level characters keep working (see
// keyboard.c's comment where the two Alt keys are told apart).

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

// ---- Modifier bits ----
//
// Which modifiers were physically held when a key was produced. These
// ride ALONGSIDE the key, they don't replace its encoding: Ctrl-A is
// still 0x01 and Alt-B is still ESC then 'b', exactly as the section
// above describes, so every CLI consumer is unaffected and the terminal
// encoding stays canonical.
//
// They exist because that encoding genuinely cannot express some things
// a GUI needs. **Shift-Tab is the motivating case**: Shift only swaps
// the layout's character table, and Tab has no shifted variant, so
// Shift-Tab and Tab arrive as the same 0x09 and a focus ring has no way
// to cycle backwards. The alternative was another discrete KEY_* code,
// as the KEY_SHIFT_ARROW_* family got -- fine once, but it doesn't
// scale, and there are only ~32 free codes before the Nordic block at
// 0xC4.
//
// Sampled at scancode-processing time, the same instant the layout
// table picks between 'a' and 'A' -- NOT queryable as live state
// afterwards. That is the same timing rule the Shift+arrow codes
// follow, and for the same reason: a modifier release racing a keypress
// must resolve one way, not two. See docs/decisions.md.
//
// Note KEY_MOD_CTRL and KEY_MOD_ALT are reported for completeness, but a
// GUI generally should NOT act on them for letter keys -- by the time
// the key arrives, Ctrl-A has already become 0x01, so `key=='a' &&
// (mods & KEY_MOD_CTRL)` is never true. Match the control code itself.
// Shift is the useful one, because it does not fold the key away.
#define KEY_MOD_SHIFT 0x01
#define KEY_MOD_CTRL  0x02
#define KEY_MOD_ALT   0x04 // LEFT Alt (Meta) only -- AltGr is separate, see above
#define KEY_MOD_ALTGR 0x08

// Blocking read of a single byte from the input stream: either an ASCII
// char or one of the KEY_* codes above.
int keyboard_getchar(void);

// Same as keyboard_getchar but returns -1 immediately if nothing is
// waiting, instead of blocking. Used by the GUI event loop.
int keyboard_try_getchar(void);

// The same two reads, but also reporting the KEY_MOD_* bits held when
// the key was produced. `out_mods` may be NULL, in which case these are
// exactly the two functions above -- which is how those are implemented.
int keyboard_getchar_mods(uint8_t *out_mods);
int keyboard_try_getchar_mods(uint8_t *out_mods);

// The modifiers held RIGHT NOW (KEY_MOD_*), for a caller that has no key
// event to read them off. A mouse click is the case: it carries no
// modifier state of its own, and Ctrl/Shift-click is a real gesture.
//
// A LIVE sample, not a latched one -- it answers "what is held at this
// instant". That makes it the wrong tool for keyboard input, where what
// matters is the modifiers held when the KEY was pressed; those ride
// with the key (keyboard_try_getchar_mods()) precisely so a modifier
// released a moment later cannot change how an already-typed character
// is interpreted.
uint8_t keyboard_mods_now(void);

// Blocking read of one line into buf (max len-1 chars + null terminator).
// Echoes typed characters to the VGA console and handles backspace.
void keyboard_read_line(char *buf, unsigned int len);

#endif
