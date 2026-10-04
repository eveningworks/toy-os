#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

// Special key codes pushed into the input stream alongside characters.
//
// **THEY LIVE AT 0xF791-0xF7B9, ABOVE EVERY CHARACTER THE KEYBOARD
// TYPES**, in Unicode's Private Use Area -- the block macOS uses for its
// function keys (NSUpArrowFunctionKey is 0xF700). They were 0x91-0xB9
// until the layouts reached all of Latin-1, whose 0xA0-0xFF they
// overlapped: AltGr+1 on a Spanish keyboard is 0xA1, which was Shift+End.
// The low byte is the old code, so a key truncated to a byte somewhere
// still lands outside ASCII rather than on Ctrl-C. A KEY IS AN `int` (or
// at least a uint16_t) EVERYWHERE; a `uint8_t` key is a bug.
#define KEY_SPECIAL_FIRST     KEY_ARROW_UP
#define KEY_SPECIAL_LAST      KEY_PRINT_SCREEN
#define IS_SPECIAL_KEY(k)     ((k) >= KEY_SPECIAL_FIRST && (k) <= KEY_SPECIAL_LAST)
#define KEY_ARROW_UP    0xF791
#define KEY_ARROW_DOWN  0xF792
#define KEY_PAGE_UP     0xF793
#define KEY_PAGE_DOWN   0xF794
#define KEY_ARROW_LEFT  0xF795
#define KEY_ARROW_RIGHT 0xF796
#define KEY_HOME        0xF797
#define KEY_END         0xF798
#define KEY_DELETE      0xF799
#define KEY_F2          0xF79A
#define KEY_F3          0xF79B
// Shift+arrow/Home/End -- distinct codes rather than a separate
// "modifier held" query, so apps that want selection (Notepad) just
// switch on one more case, and apps that don't (Terminal, the CLI
// editor) simply never see these and keep working exactly as before.
// Emitted by keyboard.c at the moment the scancode is processed (same
// place shift already picks between scancode_ascii/scancode_ascii_shift
// for a letter key), not derived later from some live "is shift down
// right now" state an app would have to poll itself -- see
// docs/decisions.md for why that timing matters.
#define KEY_SHIFT_ARROW_LEFT  0xF79C
#define KEY_SHIFT_ARROW_RIGHT 0xF79D
#define KEY_SHIFT_ARROW_UP    0xF79E
#define KEY_SHIFT_ARROW_DOWN  0xF79F
#define KEY_SHIFT_HOME        0xF7A0
#define KEY_SHIFT_END         0xF7A1

// Ctrl+Left/Right -- word motion in any readline-style line editor
// (kernel/lib/klineedit.c). Distinct codes for the same reason the
// Shift+arrow family above has them.
#define KEY_CTRL_ARROW_LEFT   0xF7A2
#define KEY_CTRL_ARROW_RIGHT  0xF7A3

// F10 -- focuses an application's menu bar (ui/uui_menubar.h), which is
// what it does on Windows and in KDE. Alt+letter mnemonics are possible
// now that Alt reaches a window as a bit (see "Ctrl and Alt" below) --
// they were not while it was an ESC prefix -- but are not built; F10 is
// the binding both of those desktops offer anyway.
#define KEY_F10               0xF7A4

// F4 -- exists for Alt+F4, which CLOSES the focused window. That is a
// window-manager shortcut (userland/wm/wm.c intercepts it before routing
// keys to the focused window), the way it is in Windows and KDE, not a
// key an app handles. Matched as KEY_F4 plus KEY_MOD_ALT rather than
// given a combined KEY_ALT_F4 code the way the Shift+arrow family was:
// nothing is folded for a function key, so the modifier bits are usable
// here, and this generalises to a future Alt+F<n> for free.
#define KEY_F4                0xF7A5

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
#define KEY_SUPER             0xF7A6

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
#define KEY_SHIFT             0xF7A7
#define KEY_CTRL              0xF7A8
#define KEY_ALT               0xF7A9 // LEFT Alt (Meta)
#define KEY_ALTGR             0xF7AA

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
#define KEY_F1                0xF7AB
#define KEY_F5                0xF7AC
#define KEY_F6                0xF7AD
#define KEY_F7                0xF7AE
#define KEY_F8                0xF7AF
#define KEY_F9                0xF7B0
#define KEY_F11               0xF7B1
#define KEY_F12               0xF7B2

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
// The three LOCK keys report their presses. CAPS LOCK is also a STATE
// the keyboard layer keeps (keyboard.c): it capitalises letter keys and
// lights the PS/2 keyboard's LED. Num Lock and Scroll Lock change
// nothing -- the keypad is always numeric (keyboard.c says why).
#define KEY_INSERT            0xF7B3
#define KEY_MENU              0xF7B4 // the "context menu" key, right of AltGr
#define KEY_CAPS_LOCK         0xF7B5
#define KEY_NUM_LOCK          0xF7B6
#define KEY_SCROLL_LOCK       0xF7B7
#define KEY_PAUSE             0xF7B8
#define KEY_PRINT_SCREEN      0xF7B9

// ---- Ctrl and Alt ----
//
// These do NOT get KEY_* codes of their own (Ctrl+Left/Right aside,
// above). What a key with them held becomes depends on who reads it --
// the split X11 and Wayland make between a keysym and a terminal's bytes:
//
//   A WINDOW (the compositor holds the keyboard):
//     Ctrl-<letter>  ->  the control code, 0x01-0x1A (Ctrl-A is 0x01,
//                        Ctrl-S 0x13), with KEY_MOD_CTRL set. Every
//                        app's shortcuts are written against these.
//     anything else  ->  THE KEY ITSELF with KEY_MOD_CTRL / KEY_MOD_ALT
//                        in `mods`: Ctrl+1 is '1' + Ctrl, Alt-B is 'b' +
//                        Alt. A character with either held is a SHORTCUT,
//                        never text (ui/uui_widget.h's uui_key_is_shortcut).
//
//   A TERMINAL (kernel/tty/tty.c's tty_input(), and the GUI Terminal for
//   its pty) encodes the way every terminal does:
//     Ctrl-<letter>  ->  the control code, as above.
//     Alt-<key>      ->  ESC (0x1B) then the key: readline's meta prefix,
//                        so Alt-B is the two bytes 0x1B 'b'.
//     Ctrl-<char>    ->  nothing: a terminal has no code for Ctrl+1.
//
// Two consequences worth knowing before adding a binding:
//
// 1. Ctrl-H, Ctrl-I, Ctrl-J and Ctrl-M are indistinguishable from
//    backspace, Tab, newline and Return -- because in this encoding
//    they ARE those keys, exactly as in a terminal.
// 2. On a terminal a lone Esc and the start of an Alt sequence look
//    identical, as on a physical one; the line editor holds the ESC and
//    decides on the NEXT key (see klineedit.h). A window never sees
//    that ambiguity.
//
// AltGr is deliberately NOT Alt: it stays a layout modifier so a Nordic
// layout's third-level characters keep working (see keyboard.c's
// comment where the two Alt keys are told apart). A game that wants keys
// by POSITION, untouched by any modifier, reads keyboard_try_get_physical()'s
// stream through the compositor (abi/win_proto.h's WIN_EV_KEY_PHYS).

// A typed character is ONE BYTE OF LATIN-1 (ISO-8859-1): ASCII, or the
// Latin-1 Supplement 0xA0-0xFF, which is what the font draws
// (font_ttf.h) and what /etc/kbs layouts produce. Not UTF-8 --
// docs/decisions/drivers.md's Nordic-keyboard entry says why, and
// docs/roadmap.md's UTF-8 migration is where that changes. 0x80-0x9F
// (the C1 controls) are not characters.
#define IS_LATIN1_CHAR(k) ((k) >= 0xA0 && (k) <= 0xFF)

// True if `k` is a character that should be inserted into typed text --
// printable ASCII (32-126) or Latin-1 0xA0-0xFF. Every "is this key a
// printable char, not a control/arrow/function key" gate should use
// this rather than a bare `key >= 32 && key < 127`, which silently
// drops every accented letter. Pass the key as an INT: a `char` above
// 0x7F is negative in this build (docs/decisions/drivers.md).
// userland/tests/echo.c cannot include this header and keeps a copy.
#define IS_PRINTABLE_KEY(k) (((k) >= 32 && (k) < 127) || IS_LATIN1_CHAR(k))

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
// KEY_MOD_CTRL is set on a Ctrl-letter too, but by then Ctrl-A has
// become 0x01, so `key=='a' && (mods & KEY_MOD_CTRL)` is never true --
// match the control code. For every other key the Ctrl and Alt bits ARE
// the modifier (see "Ctrl and Alt").
#define KEY_MOD_SHIFT 0x01
#define KEY_MOD_CTRL  0x02
#define KEY_MOD_ALT   0x04 // LEFT Alt (Meta) only -- AltGr is separate, see above
#define KEY_MOD_ALTGR 0x08
// Super/Win. Unlike the four above it is ALSO a key in its own right
// (KEY_SUPER) -- see there for why, and for whose job the "Super alone"
// gesture is.
#define KEY_MOD_SUPER 0x10

// Blocking read of one key from the input stream: a Latin-1 character
// or one of the KEY_* codes above.
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
// is a byte stream: every CHARACTER fits in a byte and a special key is
// ANSI-encoded on the way to fd 0, which is what lets it be read with
// read(). A
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

// EVERY KEY'S EDGES, BY POSITION: the evdev keycode (abi/input_keys.h),
// 1 for a press and 0 for a release, and the KEY_MOD_* state after it.
// Nothing is translated, no modifier changes what is reported, and an
// autorepeat is NOT an edge -- what a game asks ("is the key under my
// ring finger held?"), which the codes above cannot answer once Ctrl
// has turned a letter into a control code. SDL's scancodes, Windows'
// WM_KEYDOWN beside WM_CHAR, Wayland's wl_keyboard.key. The compositor
// drains it for the windows that ask (WIN_EV_KEY_PHYS). Returns 1 and
// fills the outputs, or 0 when nothing is waiting; any may be NULL.
int keyboard_try_get_physical(uint16_t *out_keycode, int *out_down,
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
