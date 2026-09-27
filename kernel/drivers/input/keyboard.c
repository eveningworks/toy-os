#include "clockevent.h" // clockevent_idle_halt()
#include "keyboard.h"
#include "keyboard_layout.h"
#include "io.h"
#include "vga.h"
#include "klog.h"
#include "scheduler.h" // scheduler_idle(), and the fd-0 reader wake below
#include "syscall_abi.h" // SYS_RETRY -- the wake value a parked fd-0 read gets
#include "string.h" // k_tolower() -- the Ctrl-key fold
#include "input.h" // INPUT_KEY_* -- the evdev keycodes everything above the wire uses
#include "tty.h" // the console terminal -- keys go to its line discipline
#include "keyboard_tap.h" // the rolling key log /bin/kbd reads

// driver-none: the keymap and key ring above the input core

#define KBD_DATA_PORT 0x60

// **THE RING USED TO BE HERE AND IS NOW tty0's.** This driver produces
// keystrokes; deciding what one MEANS -- whether it ends a line, echoes,
// or interrupts a job -- belongs to a terminal, and there is one now
// (kernel/tty.h). What is left here is the wire and the layout.
//
// The queue still carries (mods << 16) | key: the KEY is unchanged from
// what this driver has always pushed (terminal-encoded: Ctrl-A is 0x01,
// Alt-B is ESC then 'b'), and the mods half is for callers that need to
// tell Shift-Tab from Tab, which the terminal encoding cannot express.
static int shift_pressed = 0;
static int altgr_pressed = 0;
static int ctrl_pressed = 0;
static int alt_pressed = 0;   // LEFT Alt only -- right Alt is AltGr, see below
static int super_pressed = 0; // Super/Win, a modifier since shortcuts landed
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
// Exposed as keyboard_mods_now() below. Static here because everything
// in this file wants the live value; the accessor exists for callers
// that have no KEY event to read modifiers off -- see keyboard.h.
static uint8_t current_mods(void) {
    uint8_t m = 0;
    if (shift_pressed) m |= KEY_MOD_SHIFT;
    if (ctrl_pressed) m |= KEY_MOD_CTRL;
    if (alt_pressed) m |= KEY_MOD_ALT;
    if (altgr_pressed) m |= KEY_MOD_ALTGR;
    if (super_pressed) m |= KEY_MOD_SUPER;
    return m;
}

// A key has been decoded. Hand it to the console terminal, which runs
// the line discipline over it and queues what a reader should see.
//
// EVERY CODE THIS DRIVER PRODUCES FITS IN A BYTE -- the specials
// included (KEY_ARROW_* and friends are 0x91-0xA6) -- which is what lets
// a terminal be a byte stream and is the same fact SYS_READ's fd-0
// contract already states.
//
// The wake that used to be here is tty_enqueue()'s now, for the same
// reason and with the same restraint: this runs in the IRQ1 handler, so
// only scheduler state and an already-saved trapframe may be touched.
// --- key transitions: what the byte stream cannot carry --------------
//
// See keyboard.h for the rule and the reasoning. Everything here runs in
// the IRQ1 handler, so it does no more than the tty push beside it does:
// no allocation, no wake, no lock.

// What code each evdev keycode produced when it went DOWN, so its
// release can carry the SAME one. Zero means "that key produced nothing
// on the way down", which is the common case for a key with no glyph on
// this layout -- and a release with nothing to report is dropped rather
// than guessed at.
//
// Indexed by evdev keycode. 128 covers every code this kernel reports
// (INPUT_KEY_COMPOSE, 127, is the highest); anything above it is
// range-checked away rather than wrapped, because a driver reporting an
// unexpected keycode must not be able to write past this array.
#define KEY_DOWN_MAX 128
static uint8_t down_code[KEY_DOWN_MAX];

// Sized so it cannot realistically fill: a human cannot have 32 keys
// down, and the compositor drains this every frame. On overflow the
// OLDEST is dropped, which is stated rather than silent -- a dropped
// release is a key that stays down forever from the client's point of
// view, so the choice matters even though nothing should reach it.
#define TRANS_MAX 64
struct key_transition {
    uint16_t code;
    uint8_t  down;
    uint8_t  mods;
};
static struct key_transition trans[TRANS_MAX];
static unsigned trans_head, trans_tail;

static void trans_push(uint16_t code, int down) {
    if (!code) return;
    unsigned next = (trans_head + 1) % TRANS_MAX;
    if (next == trans_tail) trans_tail = (trans_tail + 1) % TRANS_MAX; // drop oldest
    trans[trans_head].code = code;
    trans[trans_head].down = (uint8_t)(down ? 1 : 0);
    trans[trans_head].mods = current_mods();
    trans_head = next;
}

int keyboard_try_get_transition(uint16_t *out_code, int *out_down,
                                 uint8_t *out_mods) {
    if (trans_tail == trans_head) return 0;
    struct key_transition t = trans[trans_tail];
    trans_tail = (trans_tail + 1) % TRANS_MAX;
    if (out_code) *out_code = t.code;
    if (out_down) *out_down = t.down;
    if (out_mods) *out_mods = t.mods;
    return 1;
}

// THE KEYCODE CURRENTLY BEING TRANSLATED, so ring_push() can record what
// this key produced without every one of its ~20 call sites having to
// pass it. Set once at the top of keyboard_key_event() and read only
// from there, both in the same non-preemptible IRQ handler.
static uint16_t emitting_keycode;

// Bytes still to drop from a Pause sequence -- see keyboard_feed_byte().
static int pause_swallow;

// Whether ring_push() has already run for THIS edge, which is what lets
// the rule above be "first press, but last push within a press".
static int first_push_done;

static void ring_push(uint16_t c) {
    // REMEMBERED BEFORE IT IS SENT, so the release of this key can carry
    // the same code.
    //
    // **THE FIRST PRESS OF A HELD KEY WINS, NOT THE LAST**, and the
    // difference is autorepeat. Hold W (which reports 'w'), then press
    // Shift: the repeats that follow report 'W', and recording each of
    // them would make the eventual release report 'W' for a key the
    // client watched go down as 'w' -- so it would clear nothing and
    // hold 'w' forever. Keeping the first means the release always ends
    // the hold it started. A non-zero entry IS "this key is already
    // down", so no separate held flag is needed.
    //
    // Within a single press the LAST push still wins, which is what
    // makes Alt-B (pushed as ESC then 'b') release as 'b' rather than as
    // ESC -- those two pushes share one keycode and one edge.
    if (emitting_keycode < KEY_DOWN_MAX &&
        (!down_code[emitting_keycode] || !first_push_done)) {
        down_code[emitting_keycode] = (uint8_t)c;
    }
    first_push_done = 1;
    // The tap's view of the SAME push, so `kbd` can show a keycode and
    // the character it turned into on one line. Here rather than at the
    // ~20 call sites for the reason `emitting_keycode` is here.
    kbdtap_produced(c);
    tty_input(tty_console(), (uint8_t)c, current_mods());
}

// Defined below, beneath keyboard_key_event() which is its public face.
// Declared here because the PS/2 path is the one caller that has a wire
// byte to hand it.
static void key_event(uint16_t keycode, int down, uint16_t wire, int extended);

// --- the PS/2 WIRE, and nothing above it -----------------------------
//
// **THE ONE PLACE AN AT SCANCODE EXISTS IN THIS KERNEL.** Everything
// above keyboard_key_event() speaks Linux evdev keycodes, which is what
// virtio-input and a USB keyboard report natively and what /etc/kbs is
// keyed on. This driver is the legacy one, so the legacy encoding stops
// here -- exactly where Linux keeps it (`atkbd` translates set 1 into
// keycodes and nothing above it ever sees a scancode).
//
// It used to be the other way round: the layout was keyed on scancodes,
// so the INPUT CORE translated evdev DOWN into set 1 for every non-PS/2
// device. That table duly grew a hole -- KEY_102ND, the ISO key that
// carries `|` on every Nordic layout, was missing -- and `|` could be
// typed on PS/2 and not on virtio-input. Pointing the translation the
// other way deletes the table rather than fixing it.
//
// For the unprefixed block the mapping is the IDENTITY, and that is not
// luck: evdev's numbering was taken from AT set 1 (KEY_1 = 2 = 0x02, up
// to KEY_F12 = 88 = 0x58). Only the 0xE0-prefixed keys need a table,
// because those are the ones evdev renumbered.
static uint16_t ext_keycode(uint8_t sc) {
    switch (sc) { // dispatch-ok: bounded by the 0xE0 codes a PS/2 keyboard emits
    case 0x1C: return INPUT_KEY_KPENTER;
    case 0x1D: return INPUT_KEY_RIGHTCTRL;
    case 0x35: return INPUT_KEY_KPSLASH;
    case 0x38: return INPUT_KEY_RIGHTALT;   // AltGr -- see keyboard_key_event
    case 0x47: return INPUT_KEY_HOME;
    case 0x48: return INPUT_KEY_UP;
    case 0x49: return INPUT_KEY_PAGEUP;
    case 0x4B: return INPUT_KEY_LEFT;
    case 0x4D: return INPUT_KEY_RIGHT;
    case 0x4F: return INPUT_KEY_END;
    case 0x50: return INPUT_KEY_DOWN;
    case 0x51: return INPUT_KEY_PAGEDOWN;
    case 0x52: return INPUT_KEY_INSERT;
    case 0x53: return INPUT_KEY_DELETE;
    case 0x5B: return INPUT_KEY_LEFTMETA;
    case 0x5C: return INPUT_KEY_RIGHTMETA;
    case 0x5D: return INPUT_KEY_COMPOSE;
    case 0x37: return INPUT_KEY_SYSRQ;      // Print Screen: E0 2A E0 37
    // **THE FAKE SHIFTS AROUND PRINT SCREEN ARE DROPPED.** A PS/2
    // keyboard brackets PrtSc with E0 2A / E0 AA so that a DOS-era
    // reader saw a shifted key; taking them at face value here would
    // report a Shift press that nobody made, and leave `shift_pressed`
    // set if the release half were ever missed.
    case 0x2A: case 0x36: return 0;
    default:   return 0;                    // a key this kernel has no name for
    }
}

int keyboard_wire_keycode(uint8_t sc, int extended, uint16_t *out) {
    uint16_t kc = extended ? ext_keycode((uint8_t)(sc & 0x7F))
                            : (uint16_t)(sc & 0x7F);
    if (!kc) return 0;
    if (out) *out = kc;
    return 1;
}

// Processes one byte already read from the 8042 by i8042_poll(). This
// must NOT read port 0x60 itself -- see i8042.h for why.
//
// THE WIRE ONLY: an 0xE0 prefix, a release bit, and a scancode. What the
// key MEANS is keyboard_key_event()'s, which every driver reaches --
// this function is what makes PS/2 one of them rather than the one the
// others have to imitate.
void keyboard_feed_byte(uint8_t sc) {
    // **PAUSE IS SIX BYTES AND HAS NO RELEASE.** It arrives as
    // E1 1D 45 E1 9D C5 and nothing else uses the E1 prefix, so the
    // whole sequence is swallowed by counting: report the press when the
    // prefix arrives and drop the five bytes behind it. There is no
    // break code to report, which is why this is the one key that
    // reports a press with no matching release -- documented rather
    // than smoothed over, because a client tracking held keys has to
    // tolerate it (and already must, see win_proto.h).
    if (sc == 0xE1) {
        pause_swallow = 5;
        key_event(INPUT_KEY_PAUSE, 1, 0xE1, 0);
        return;
    }
    if (pause_swallow) {
        pause_swallow--;
        return;
    }

    if (sc == 0xE0) {
        extended_prefix = 1;
        return;
    }

    int down = !(sc & 0x80);
    uint8_t code = sc & 0x7F;

    uint16_t keycode;
    int extended = extended_prefix;
    if (extended_prefix) {
        extended_prefix = 0;
        keycode = ext_keycode(code);
    } else {
        // The identity, for the reason ext_keycode() states.
        keycode = code;
    }
    if (!keycode) return;
    // THE WIRE BYTE IS PASSED DOWN, not stashed in a static for the tap
    // to pick up. A static would be written by whichever driver reported
    // last, so a virtio keypress arriving between an 8042 byte and its
    // decode would be logged carrying somebody else's scancode -- a
    // wrong number in a tool whose whole job is being trusted about
    // numbers. `sc`, not `code`: the release bit is what the wire said.
    key_event(keycode, down, sc, extended);
}

// --- what a key MEANS, for every driver ------------------------------
//
// Keyed on evdev keycodes, so a key behaves identically whether it
// arrived over PS/2, virtio-input or anything added later. That is the
// property the input core exists for, and until the layout was re-keyed
// it was not actually true -- see ext_keycode() above.
void keyboard_key_event(uint16_t keycode, int down) {
    // NO WIRE BYTE: everything that is not PS/2 speaks keycodes, so
    // there is no scancode to report and the tap logs a blank rather
    // than a plausible zero (0x00 is not a scancode).
    key_event(keycode, down, 0, 0);
}

// The whole of the above, plus what the PS/2 wire said if it was the
// PS/2 wire that said it. Split out for the keyboard TAP alone: nothing
// else in this file wants to know which encoding a key arrived in, which
// is the property the input core exists to provide.
static void key_event(uint16_t keycode, int down, uint16_t wire, int extended) {
    // Modifiers first, and they are the only keys whose RELEASE matters.
    //
    // LEFT ALT AND RIGHT ALT ARE DIFFERENT KEYS HERE, deliberately: left
    // Alt is readline's Meta, while right Alt is AltGr, a LAYOUT
    // modifier that picks a third character. Conflating them would make
    // AltGr-b try to be Meta-b on a Nordic layout. evdev gives them
    // separate keycodes, so this needs no prefix bookkeeping -- which is
    // exactly the kind of thing the scancode encoding made fiddly.
    // The state is set BEFORE the transition is pushed, so the `mods`
    // riding with a modifier's own event describes the world AFTER that
    // key moved -- a Shift press reports KEY_MOD_SHIFT set. The
    // alternative reports every modifier press with the modifier absent,
    // which reads as a bug at every call site that looks.
    //
    // ONE EXIT rather than a `return` per case, so the tap and the
    // transition queue are fed from the same place and cannot drift into
    // sampling the modifier state at two different instants.
    uint16_t mod_code = 0;
    switch (keycode) { // dispatch-ok: the modifier set is bounded by the keyboard
    case INPUT_KEY_LEFTSHIFT:
    case INPUT_KEY_RIGHTSHIFT: shift_pressed = down; mod_code = KEY_SHIFT; break;
    case INPUT_KEY_LEFTCTRL:
    case INPUT_KEY_RIGHTCTRL:  ctrl_pressed = down;  mod_code = KEY_CTRL;  break;
    case INPUT_KEY_LEFTALT:    alt_pressed = down;   mod_code = KEY_ALT;   break;
    case INPUT_KEY_RIGHTALT:   altgr_pressed = down; mod_code = KEY_ALTGR; break;
    // SUPER JOINED THIS SWITCH when shortcuts landed, and that is what
    // took it out of the byte stream: it used to ring_push(KEY_SUPER) on
    // the press, which is why the Start menu opened the instant the key
    // went down. It is a modifier now (api/keyboard.h), so it reaches
    // the compositor as a TRANSITION and the "Super alone" gesture is
    // the compositor's policy rather than this driver's.
    case INPUT_KEY_LEFTMETA:
    case INPUT_KEY_RIGHTMETA:  super_pressed = down; mod_code = KEY_SUPER; break;
    default: break;
    }
    if (mod_code) {
        // A modifier produces NO code in the byte stream, so the tap
        // records the event and nothing produced -- which is the honest
        // answer and the one that makes a Shift line readable: the
        // character column is empty and the modifier column is not.
        kbdtap_key(wire, extended, keycode, down, current_mods());
        trans_push(mod_code, down);
        return;
    }

    // EVERY OTHER KEY, BOTH EDGES, opened here -- before any of the
    // ~20 paths below can return -- so that a key producing nothing is
    // still logged. "The scancode arrived and the layout gave back
    // nothing" is the single most useful line this tool prints, and a
    // record opened only where a character is emitted could never
    // carry it.
    kbdtap_key(wire, extended, keycode, down, current_mods());

    // A RELEASE REPORTS WHAT THE PRESS PRODUCED, and then forgets it.
    // Clearing is what stops a key that is pressed, released, and then
    // pressed again on a layout where it now produces nothing from
    // reporting the OLD code on its second release.
    if (!down) {
        if (keycode < KEY_DOWN_MAX) {
            trans_push(down_code[keycode], 0);   // a 0 here is dropped by trans_push
            down_code[keycode] = 0;
        }
        return;
    }

    // Recorded by ring_push() below, whichever of the many paths out of
    // this function ends up taking it.
    emitting_keycode = keycode;
    first_push_done = 0;

    // Ctrl+Left/Right are word motion in every readline-ish line editor,
    // so they get their own codes -- exactly the KEY_SHIFT_ARROW_*
    // precedent below, resolved here from live modifier state at
    // keypress time for the same reason (see docs/decisions.md).
    if (ctrl_pressed && keycode == INPUT_KEY_LEFT)  { ring_push(KEY_CTRL_ARROW_LEFT); return; }
    if (ctrl_pressed && keycode == INPUT_KEY_RIGHT) { ring_push(KEY_CTRL_ARROW_RIGHT); return; }

    // Shift+arrow/Home/End get their own codes, decided right here from
    // the live `shift_pressed` state -- same timing as the layout lookup
    // below for ordinary letter keys, so a shift release racing the
    // arrow keypress resolves the same way either family already does.
    // dispatch-ok: a keymap is bounded BY THE KEYBOARD. This switch names
    // every key that does not produce a character -- navigation, the
    // function row, the locks, the keypad -- and it cannot grow except
    // by somebody attaching a key that does not exist today. It is also
    // the exact case CLAUDE.md gives when it says a bounded branch set
    // should be waived rather than made a table; a table here would be
    // the same data with a level of indirection in front of it.
    switch (keycode) {
    case INPUT_KEY_UP:       ring_push(shift_pressed ? KEY_SHIFT_ARROW_UP : KEY_ARROW_UP); return;
    case INPUT_KEY_DOWN:     ring_push(shift_pressed ? KEY_SHIFT_ARROW_DOWN : KEY_ARROW_DOWN); return;
    case INPUT_KEY_LEFT:     ring_push(shift_pressed ? KEY_SHIFT_ARROW_LEFT : KEY_ARROW_LEFT); return;
    case INPUT_KEY_RIGHT:    ring_push(shift_pressed ? KEY_SHIFT_ARROW_RIGHT : KEY_ARROW_RIGHT); return;
    case INPUT_KEY_HOME:     ring_push(shift_pressed ? KEY_SHIFT_HOME : KEY_HOME); return;
    case INPUT_KEY_END:      ring_push(shift_pressed ? KEY_SHIFT_END : KEY_END); return;
    case INPUT_KEY_PAGEUP:   ring_push(KEY_PAGE_UP); return;
    case INPUT_KEY_PAGEDOWN: ring_push(KEY_PAGE_DOWN); return;
    case INPUT_KEY_DELETE:   ring_push(KEY_DELETE); return;
    // THE WHOLE FUNCTION ROW. It was four -- F2/F3 (the file manager),
    // F10 (the menu bar) and F4 (Alt+F4) -- added one per caller; Doom
    // binds F1 through F11 and made the rest worth having. Pushed
    // here rather than through the layout is also what keeps Alt+F4
    // whole -- it returns before the Alt-prefixes-with-ESC path below,
    // so the key arrives once, with KEY_MOD_ALT set, rather than as ESC
    // followed by something.
    case INPUT_KEY_F1:  ring_push(KEY_F1); return;
    case INPUT_KEY_F2:  ring_push(KEY_F2); return;
    case INPUT_KEY_F3:  ring_push(KEY_F3); return;
    case INPUT_KEY_F4:  ring_push(KEY_F4); return;
    case INPUT_KEY_F5:  ring_push(KEY_F5); return;
    case INPUT_KEY_F6:  ring_push(KEY_F6); return;
    case INPUT_KEY_F7:  ring_push(KEY_F7); return;
    case INPUT_KEY_F8:  ring_push(KEY_F8); return;
    case INPUT_KEY_F9:  ring_push(KEY_F9); return;
    case INPUT_KEY_F10: ring_push(KEY_F10); return;
    case INPUT_KEY_F11: ring_push(KEY_F11); return;
    case INPUT_KEY_F12: ring_push(KEY_F12); return;

    // --- THE KEYS THAT USED TO REPORT NOTHING AT ALL ------------------
    //
    // See keyboard.h. Each of these was silently dropped: the layout
    // has no entry for it, so keyboard_layout_translate() returned 0 and
    // the key was indistinguishable from one that was never pressed.
    case INPUT_KEY_INSERT:     ring_push(KEY_INSERT); return;
    case INPUT_KEY_COMPOSE:    ring_push(KEY_MENU); return;
    case INPUT_KEY_CAPSLOCK:   ring_push(KEY_CAPS_LOCK); return;
    case INPUT_KEY_NUMLOCK:    ring_push(KEY_NUM_LOCK); return;
    case INPUT_KEY_SCROLLLOCK: ring_push(KEY_SCROLL_LOCK); return;
    case INPUT_KEY_PAUSE:      ring_push(KEY_PAUSE); return;
    case INPUT_KEY_SYSRQ:      ring_push(KEY_PRINT_SCREEN); return;

    // --- THE NUMERIC KEYPAD, AS THE CHARACTERS ON ITS KEYCAPS ---------
    //
    // Not KEY_* codes: the keypad's whole point is to type numbers, and
    // an app that had to learn twelve new codes to receive a `7` would
    // be the wrong shape. Keypad Enter is the same `\n` the main Enter
    // sends, as on every OS.
    //
    // **NUMLOCK'S OFF-STATE IS DELIBERATELY NOT MODELLED.** On real
    // hardware NumLock off turns the keypad into a second set of
    // arrows/Home/End. Implementing that means holding lock STATE, which
    // this kernel does not have for Caps Lock either, and the failure
    // mode of getting it wrong is a keypad that types nothing while the
    // light says it should. Always-numeric is what a keypad is for, and
    // the arrows already exist a few inches to the left.
    case INPUT_KEY_KP0: ring_push('0'); return;
    case INPUT_KEY_KP1: ring_push('1'); return;
    case INPUT_KEY_KP2: ring_push('2'); return;
    case INPUT_KEY_KP3: ring_push('3'); return;
    case INPUT_KEY_KP4: ring_push('4'); return;
    case INPUT_KEY_KP5: ring_push('5'); return;
    case INPUT_KEY_KP6: ring_push('6'); return;
    case INPUT_KEY_KP7: ring_push('7'); return;
    case INPUT_KEY_KP8: ring_push('8'); return;
    case INPUT_KEY_KP9: ring_push('9'); return;
    case INPUT_KEY_KPDOT:      ring_push('.'); return;
    case INPUT_KEY_KPPLUS:     ring_push('+'); return;
    case INPUT_KEY_KPMINUS:    ring_push('-'); return;
    case INPUT_KEY_KPASTERISK: ring_push('*'); return;
    case INPUT_KEY_KPSLASH:    ring_push('/'); return;
    case INPUT_KEY_KPENTER:    ring_push('\n'); return;
    default: break;
    }

    char c = keyboard_layout_translate(keycode, shift_pressed, altgr_pressed);
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
        int lower = k_tolower((unsigned char)c);
        if (lower < 'a' || lower > 'z') return;
        uint16_t code = (uint16_t)(lower - 'a' + 1);
        // **INTR IS NOT SPECIAL HERE ANY MORE.** Ctrl-C used to be
        // recognised on this line, with a comment saying it belonged to
        // a line discipline and there was not one yet. There is
        // (kernel/tty/ldisc.c), so 0x03 goes through as an ordinary
        // control code and the terminal decides what it means -- which
        // is what makes a Terminal WINDOW able to have the same Ctrl-C
        // as this keyboard.
        ring_push(code);
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

// See keyboard.h. Not static state the shell can reach around: the
// blocking readers below are the only consumers.
// TWO INDEPENDENT REASONS THE RING-0 BLOCKING READER STANDS DOWN, and
// they must not share one flag: a compositor holds the screen
// (win_server.c), or a ring-3 process is reading the console through
// fd 0 (kernel/tty/tty_fd.c). Both can be true at once, and with a single
// boolean whichever released second would hand the keyboard back while
// the other still owned it -- a shell executing keys typed at somebody
// else's prompt, which is the exact bug keyboard_suspend_blocking() was
// added to fix in the first place.
//
// So: two setters, one predicate. Everything that asks "may I take a
// key?" asks the predicate.
static int g_blocking_suspended;
static int g_console_claimed;

void keyboard_suspend_blocking(int on) {
    g_blocking_suspended = on ? 1 : 0;
    // ...AND THE CONSOLE'S LINE DISCIPLINE IS MUTED WITH IT. A
    // compositor reads raw input itself (win_input.c), so a discipline
    // assembling a line underneath it would swallow keystrokes the
    // desktop is about to be given, and would echo them onto a screen
    // it does not own. This is KD_GRAPHICS + KDSKBMODE/K_OFF on a Linux
    // VT, and it is set from HERE because this is the one place the
    // compositor's hold is recorded -- registering, deregistering, a
    // kill and a fault are all the same call.
    tty_set_bypass(tty_console(), g_blocking_suspended);
}
void keyboard_claim_console(int on)    { g_console_claimed = on ? 1 : 0; }
int  keyboard_console_claimed(void)    { return g_console_claimed; }
int  keyboard_compositor_owns(void)    { return g_blocking_suspended; }
int  keyboard_blocking_suspended(void) { return g_blocking_suspended || g_console_claimed; }

// The blocking reader's pop, in the shape its `while` condition wants:
// a truthy "got one" plus the (mods << 16) | key word it has always
// unpacked. tty0's queue is the ring this driver used to own.
static int tty_pop(uint32_t *out) {
    uint8_t mods = 0;
    int c = tty_read_key(tty_console(), &mods);
    if (c < 0) return 0;
    *out = ((uint32_t)mods << 16) | (uint32_t)c;
    return 1;
}

void keyboard_console_set_raw(int on) {
    struct tty_termios tio;
    tty_get_termios(tty_console(), &tio);
    if (on) {
        tio.lflag &= ~(uint32_t)(TTY_ICANON | TTY_ECHO);
        tio.lflag |= TTY_ISIG;
    } else {
        tio.lflag = TTY_LFLAG_DEFAULT;
    }
    tty_set_termios(tty_console(), &tio);
}

int keyboard_getchar(void) { return keyboard_getchar_mods(0); }

int keyboard_getchar_mods(uint8_t *out_mods) {
    uint32_t ev;
    for (;;) {
    // The suspend check comes FIRST and short-circuits, so a suspended
    // reader never pops -- it must not consume a key the compositor is
    // about to be given (win_input.c drains the same queue, from the
    // scheduler_idle() call below).
    while (keyboard_blocking_suspended() || (ev = 0, !tty_pop(&ev))) {
        // The halt wakes on every interrupt, not just a real keypress --
        // and on the deadlines idle work asks for -- so this is a cheap
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
        // userland/wm/wm.c's loop covers the GUI case separately).
        // ...but ONLY WHEN THE CONSOLE OWNS THE SCREEN. A suspended
        // reader is one whose screen belongs to a compositor, and the
        // console's cursor tick and present both write to the
        // framebuffer -- so doing them anyway paints a blinking text
        // cursor on top of the desktop, at whatever cell the shell's
        // prompt left it. Reported from a screenshot: a blinking block
        // sitting on a desktop icon.
        //
        // This is the invariant scheduler_idle() already states -- that
        // console upkeep belongs to whoever owns the screen, which is
        // why the tick and the present are deliberately NOT part of it.
        // Suspending the READ was not enough; the loop body had to stop
        // drawing too.
        if (!keyboard_blocking_suspended()) {
            vga_cursor_tick();
        }
        // The kernel's idle work (scheduler.h) -- the serial debug
        // console, today. Runs either way: it is the one thing here that
        // is not the console's, and a suspended shell must still drain
        // the debug console and feed raw input to the compositor.
        scheduler_idle();
        // The physical console's flush point: it draws into a back
        // buffer and this is where "output is finished, we are waiting
        // for a human" is true, so it is where the screen catches up.
        // Cheap when nothing changed. See vga.h's vga_present().
        if (!keyboard_blocking_suspended()) {
            vga_present();
        }
        // The machine's main idle point, so the one place the tick may
        // stop (kernel/clockevent.h).
        clockevent_idle_halt();
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
    return tty_read_key(tty_console(), out_mods);
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

// The modifiers held RIGHT NOW, for a caller with no key event to read
// them off -- a mouse click, which carries no modifier state of its own.
// The desktop's Ctrl/Shift-drag is the first caller (rubber-band
// selection, userland/wm/desktop.c).
//
// Deliberately a live sample, not a latched value: it answers "what is
// held at this instant", which is the question a click has. That makes
// it wrong for keyboard input, where the modifiers that matter are the
// ones held when the KEY was pressed -- which is why key events carry
// their own mods (keyboard_try_getchar_mods()) rather than calling this.
uint8_t keyboard_mods_now(void) { return current_mods(); }
