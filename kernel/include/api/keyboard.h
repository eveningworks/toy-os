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
// window-manager shortcut (userland/wm/wm.c intercepts it before routing
// keys to the focused window), the way it is in Windows and KDE, not a
// key an app handles. Matched as KEY_F4 plus KEY_MOD_ALT rather than
// given a combined KEY_ALT_F4 code the way the Shift+arrow family was:
// nothing is folded for a function key, so the modifier bits are usable
// here, and this generalises to a future Alt+F<n> for free.
#define KEY_F4                0xA5

// Super (the Windows/Meta key). **IT IS BOTH A MODIFIER AND A KEY**,
// which is the one place this driver reports a key twice over, and the
// reason is that every desktop binds it both ways: Super+E opens a file
// manager on Windows and KDE, and Super ALONE opens the launcher.
//
// So it is a MODIFIER FIRST -- KEY_MOD_SUPER below, and a transition on
// this code, never a byte in the console stream (see the modifier block
// after this). The "Super alone" gesture is then a POLICY the window
// manager applies, not something the driver decides: it acts on the
// RELEASE, and only when no other key was pressed in between. That is
// what Windows and KDE both do, and it is why holding Super to type
// Super+E does not also open the Start menu on the way out.
//
// (This header used to say Super acted on its own and that supporting
// Super+<letter> "means adding a modifier bit beside this". That is
// exactly what happened; the bit is KEY_MOD_SUPER.)
//
// Left and right Super send the SAME code. No desktop distinguishes
// them, and the driver already makes that call for left/right Ctrl.
#define KEY_SUPER             0xA6

// --- THE FOUR MODIFIER KEYS, AS KEYS ---------------------------------
//
// These exist ONLY on the transition path below
// (keyboard_try_get_transition()) and are NEVER pushed into the console
// byte stream. That restriction is the whole reason they can exist at
// all: pressing Shift must not put a byte in front of a shell, and the
// line editor would have to learn to ignore four new codes if it did.
//
// They are here because a modifier produces no character, so it produces
// no ordinary key event either -- and "is Ctrl held?" is a real question
// for a program that is not editing text. Doom's stock controls are the
// worked example: fire is Ctrl, run is Shift, strafe is Alt, and three
// of its five defaults are therefore invisible to the byte stream.
//
// LEFT AND RIGHT ARE THE SAME KEY HERE, exactly as they already are for
// the shift_pressed/ctrl_pressed state these follow -- and for the same
// reason Super does not distinguish sides. AltGr stays separate from
// Alt, which is the one distinction this driver has always made and the
// one that matters on a Nordic layout.
#define KEY_SHIFT             0xA7
#define KEY_CTRL              0xA8
#define KEY_ALT               0xA9 // LEFT Alt (Meta)
#define KEY_ALTGR             0xAA

// --- THE REST OF THE FUNCTION ROW ------------------------------------
//
// F2, F3, F4 and F10 got codes first, one per caller, and this header
// said so: "the four function keys with callers". The row is now
// complete because something wants the whole of it -- Doom binds F1
// through F11 (help, save, load, volume, detail, quicksave, end game,
// messages, quickload, quit, gamma), and half a function row is worse
// than none: F6 and F9 are quicksave and quickload, which are the two
// people actually reach for.
//
// Numbered in key order rather than in the order they were added, so
// the block reads as a row. F12 has no caller here and is included
// anyway -- it is the one key whose absence from a complete-looking run
// of F1-F11 would look like an oversight rather than a decision.
#define KEY_F1                0xAB
#define KEY_F5                0xAC
#define KEY_F6                0xAD
#define KEY_F7                0xAE
#define KEY_F8                0xAF
#define KEY_F9                0xB0
#define KEY_F11               0xB1
#define KEY_F12               0xB2

// --- AND THE REST OF THE KEYBOARD ------------------------------------
//
// **EVERY KEY A PC KEYBOARD HAS NOW PRODUCES SOMETHING.** The set above
// grew one code per caller, which left real keys reporting nothing at
// all: pressing Insert, the Menu key, either lock key or anything on the
// numeric keypad was indistinguishable from not pressing a key. That is
// a bad property for an input layer to have -- an app cannot bind what
// it never sees, and "does this keyboard even work?" had no answer for a
// third of the keys on it.
//
// The keypad is NOT here, because it does not need codes: it emits the
// CHARACTERS on the keycaps (`7`, `+`, `.`) and Keypad Enter emits the
// same `\n` the main Enter does, which is what every OS does and what
// makes a keypad useful for typing numbers without any app knowing it
// exists. NumLock's off-state (keypad as arrows) is deliberately NOT
// modelled -- see keyboard.c.
//
// The three LOCK keys report their presses and nothing else: this
// kernel has no lock STATE, so Caps Lock does not change what a letter
// key produces. Reporting the press is still worth it -- an app that
// wants to know is told -- and it is honest about doing nothing more.
#define KEY_INSERT            0xB3
#define KEY_MENU              0xB4 // the "context menu" key, right of AltGr
#define KEY_CAPS_LOCK         0xB5
#define KEY_NUM_LOCK          0xB6
#define KEY_SCROLL_LOCK       0xB7
#define KEY_PAUSE             0xB8
#define KEY_PRINT_SCREEN      0xB9

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

// A key went down or up, by LINUX EVDEV KEYCODE -- what every driver
// calls, and where a key stops being a wire encoding and starts being a
// character. keyboard_feed_byte() above is the PS/2 wire's adapter onto
// this, and the only place in the kernel an AT scancode exists.
//
// Reached from the input core (kernel/input.h's input_report_key) for
// anything that is not PS/2, with no translation in between -- which is
// the point: a hand-kept evdev-to-scancode table is what let `|` work on
// one keyboard and not another.
void keyboard_key_event(uint16_t keycode, int down);

// The evdev keycode the PS/2 wire byte `sc` means -- `extended` for a
// code that followed an 0xE0 prefix. Returns 1 and fills `*out` for a
// byte this driver can name, 0 for one it drops.
//
// EXPOSED SO THE PARITY CHECK CAN EXIST, and it is the only table left
// that could have a hole: everything else speaks keycodes already.
// input_test.c asserts that every keycode the active layout maps a
// character to is producible from some wire byte -- so a key that works
// on virtio-input and not on PS/2 is a failed build rather than a
// keystroke that silently does nothing.
int keyboard_wire_keycode(uint8_t sc, int extended, uint16_t *out);

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
// Super/Win. Unlike the four above it is ALSO a key in its own right
// (KEY_SUPER) -- see there for why, and for whose job the "Super alone"
// gesture is.
#define KEY_MOD_SUPER 0x10

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

// WHO OWNS THE KEYBOARD. While this is set, the BLOCKING readers above
// never return a key -- they idle instead, exactly as if nobody were
// typing. The non-blocking `keyboard_try_getchar*` below are untouched.
//
// It exists because init started supervising the desktop
// (docs/init-design.md stage 2): the desktop is now up while the
// physical shell is still sitting at a prompt behind it, and both were
// draining the same ring. Whichever polled first won, so keys typed at
// the desktop were being executed by an invisible shell -- measured, not
// theorised. Before this, `gui` blocked the shell inside its own
// spawn-and-wait loop, so the question could not arise.
//
// Set from ONE place -- win_server.c, wherever the compositor role
// changes -- so registering, deregistering, a kill and a fault are the
// same path. THE TRAP: a compositor that registers and then never draws
// leaves a console that is both blank and deaf, which looks exactly like
// a hung machine; `target=text` on the GRUB line is the way back, and
// the role clearing on death is what makes that rare.
//
// This is a placeholder for real console ownership, not the finished
// article -- a per-TTY input queue with a foreground process is the TTY
// milestone's job (docs/init-design.md's R9). What it buys today is that
// exactly one thing reads the keyboard at a time.
void keyboard_suspend_blocking(int on);

// The SECOND reason the ring-0 reader stands down: a ring-3 process is
// reading the physical console through fd 0 (kernel/tty/tty_fd.c). Claimed by
// the first such read and released when that process dies, so the
// kernel shell's prompt comes back on its own if the reader crashes.
//
// A separate flag from the compositor's above rather than the same one:
// both can hold at once, and one boolean would let whichever released
// second hand the keyboard back while the other still owned it.
void keyboard_claim_console(int on);
int  keyboard_console_claimed(void);

// True while a COMPOSITOR owns the console -- the first reason above,
// on its own. The ring-3 fd-0 reader tests this rather than the
// combined predicate below: a desktop owning the screen owns the
// keyboard with it, so a console read must park instead of popping a
// key win_input.c is about to hand the compositor. It must NOT test the
// combined one, which is true of its own claim and would deadlock it.
int  keyboard_compositor_owns(void);

// True when EITHER reason holds. This is what the blocking reader tests
// -- never one of the two flags directly.
int  keyboard_blocking_suspended(void);

// Put the physical console's line discipline in RAW mode, or back to
// POSIX's (kernel/tty.h, abi/tty_abi.h). Raw is ICANON and ECHO off with
// ISIG left on: bytes as typed, nothing echoed by the kernel, Ctrl-C
// still interrupting.
//
// **THIS EXISTS FOR apps/, WHICH CANNOT INCLUDE kernel/tty.h.** The
// kernel's own shell edits for itself (kernel/lib/klineedit.c) and so
// must turn the discipline off, exactly as /bin/tosh does through
// sys_tty_raw(0) -- but `apps/` is deliberately not on the internal
// include path, so the capability gets a function on the app-facing side
// rather than a reach-around (kernel/include/README.md). Ring 3 uses the
// syscall; there is one implementation underneath both.
void keyboard_console_set_raw(int on);
int keyboard_try_getchar_mods(uint8_t *out_mods);

// --- KEY TRANSITIONS: everything the byte stream cannot say -----------
//
// **THE RULE, STATED ONCE: this queue carries every key event the
// console byte stream cannot represent -- that is, ALL RELEASES, and
// both edges of the four modifier keys.** Ordinary presses are not
// duplicated here; they arrive as bytes, the way they always have.
//
// It is a SEPARATE queue rather than a flag on the existing one because
// the existing one is a terminal's input (kernel/tty/), and a terminal
// is a byte stream: `keyboard.c` states that every code it produces
// fits in a byte, which is what lets fd 0 be read with read(). A
// release is not a byte and a line discipline has no use for one --
// nothing in `klineedit.c` would ever ask "has W come up?". Pushing
// releases into that stream would put a byte in front of every shell in
// the system to serve a consumer that is not a shell.
//
// WHO READS IT: the compositor path, via win_input.c, which turns a
// transition into WIN_EV_RAW_KEY (a modifier press) or WIN_EV_RAW_KEY_UP
// (any release) for a registered compositor. Non-blocking and drained
// per frame, like the rest of that path.
//
// **THE CODE ON A RELEASE IS WHAT THE PRESS PRODUCED**, not what the
// same physical key would produce now. Pressing W, holding it, pressing
// Shift and then releasing W reports a release of 'w' -- because 'w' is
// what went down, and a client that saw 'w' go down and 'W' come up
// would hold the key forever. The driver remembers, per evdev keycode,
// which code that key's press emitted; this is the job X11 and Wayland
// give the client by delivering physical keycodes and letting XKB
// translate, and doing it here keeps ONE vocabulary on the wire.
//
// Returns 1 and fills the outputs, or 0 when nothing is waiting. Any
// output pointer may be NULL. `down` is 1 for a press (modifiers only)
// and 0 for a release.
int keyboard_try_get_transition(uint16_t *out_code, int *out_down,
                                 uint8_t *out_mods);

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
