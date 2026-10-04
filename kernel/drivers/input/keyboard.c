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
#include "irqflags.h"     // irq_save() -- keyboard_events_attach() races IRQ1

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
// CAPS LOCK: the one lock STATE this driver keeps (Num Lock stays always-
// numeric, see the keypad below). `caps_held` is so a typematic repeat of
// the key -- another press with no release -- does not toggle it again.
static int caps_lock, caps_held;
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

// The modifiers physically held RIGHT NOW. Sampled by emit() at
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

// A key has been decoded (emit()). Hand it to the console terminal,
// which runs the line discipline over it and queues what a reader should
// see -- or, while a compositor holds the keyboard, to the event stream
// below instead.
//
// EVERY CHARACTER THIS DRIVER PRODUCES FITS IN A BYTE (Latin-1); a
// KEY_* special does not (api/keyboard.h), and the terminal turns it
// into an ANSI sequence before it reaches fd 0 -- which is what lets a
// terminal be a byte stream, as SYS_READ's fd-0 contract states.
//
// The wake that used to be here is tty_enqueue()'s now, for the same
// reason and with the same restraint: this runs in the IRQ1 handler, so
// only scheduler state and an already-saved trapframe may be touched.

// --- THE KEY EVENT STREAMS: what a compositor reads ------------------
//
// See keyboard.h (keyboard_try_get_key(), keyboard_try_get_physical()).
// While a compositor is ATTACHED (keyboard_events_attach()), every key
// event goes to these two streams and nowhere else, in the order it
// happened: `kev` carries each press as its translated code --
// autorepeats and dead-key output included -- and each release; `phys`
// carries the same keys by position, both edges, no repeats. Linux's
// evdev shape. While none is attached, a held screen drops keys and a
// console takes them (emit()).
//
// **THE INVARIANT, FOR BOTH: A PRESS THAT IS QUEUED ALWAYS HAS ROOM FOR
// ITS RELEASE.** A press is admitted only while the free slots after it
// still cover the release of every key whose press is queued and not yet
// released (`owed`), so a release is never refused and never evicts
// anything. Under overflow the NEWEST press is refused, whole -- and a
// key whose press was refused has its release dropped too, so a client
// never sees half of a key. Nothing queued is ever evicted.
//
// **AND OWED IS BOUNDED BY THE KEYS TRACKED, WHICH EACH RING EXCEEDS.**
// A release that never comes (an i8042 overrun) leaves its key owed: one
// slot until that key is pressed and released again, and never more than
// the tracked keycodes in all -- each ring is twice that, so presses keep
// fitting whatever is lost. A role change forgets every owed release.
//
// **EVERY READ AND WRITE IS WITH INTERRUPTS OFF.** The producers are
// IRQ handlers (PS/2, virtio-input) and polled sources run from
// scheduler_idle() (USB HID), the reader is win_input_poll() and the
// reset comes from a syscall -- all on one CPU (docs/smp-design.md), so
// interrupts off is what serialises them. No lock: an IRQ cannot take one.
struct edge_rec {
    uint16_t code;
    uint8_t  down;
    uint8_t  mods;
};
struct edge_ring {
    struct edge_rec *buf;
    unsigned size, head, tail;
    int owed;                 // presses queued whose release is still to come
};

static int er_free(const struct edge_ring *r) {
    return (int)((r->tail + r->size - r->head - 1) % r->size);
}
static void er_put(struct edge_ring *r, uint16_t code, int down) {
    r->buf[r->head] = (struct edge_rec){ code, (uint8_t)(down ? 1 : 0), current_mods() };
    r->head = (r->head + 1) % r->size;
}
// Room for a press: itself, plus every owed release, plus its own if it
// starts a hold.
static int er_admits(const struct edge_ring *r, int new_hold) {
    return er_free(r) >= r->owed + (new_hold ? 2 : 1);
}
static int er_get(struct edge_ring *r, struct edge_rec *out) {
    uint64_t f = irq_save();
    int got = r->tail != r->head;
    if (got) {
        *out = r->buf[r->tail];
        r->tail = (r->tail + 1) % r->size;
    }
    irq_restore(f);
    return got;
}
static void er_reset(struct edge_ring *r) { r->head = r->tail = 0; r->owed = 0; }

#define KEY_DOWN_MAX 128     // evdev keycodes tracked by `kev`; INPUT_KEY_COMPOSE is 127
#define PHYS_KEYCODES 256    // ...and by `phys`
static struct edge_rec kev_buf[2 * KEY_DOWN_MAX], phys_buf[2 * PHYS_KEYCODES];
static struct edge_ring kev  = { kev_buf,  2 * KEY_DOWN_MAX, 0, 0, 0 };
static struct edge_ring phys = { phys_buf, 2 * PHYS_KEYCODES, 0, 0, 0 };
static uint8_t  kev_delivered[KEY_DOWN_MAX];   // this key's press is queued
static uint16_t kev_code[KEY_DOWN_MAX];        // ...as this code: its release's
static uint8_t  phys_held[PHYS_KEYCODES / 8];  // `phys`'s owed set, and its repeat filter
static int g_attached;                         // a compositor reads these streams

// A character with no key of its own (a dead key's accent; a key past
// the tracked range): down and up together, or neither.
static void kev_synthetic(uint16_t code) {
    uint64_t f = irq_save();
    if (er_admits(&kev, 1)) {
        er_put(&kev, code, 1);
        er_put(&kev, code, 0);
    }
    irq_restore(f);
}

// A press of `keycode` that produced `code`. A REPEAT (the key is
// already delivered) starts no hold. A RELEASE REPORTS WHAT THE FIRST
// PRESS PRODUCED: hold W ('w'), press Shift, and the repeats say 'W' --
// the release must still say 'w', or a client that saw 'w' go down holds
// it forever.
static void kev_press(uint16_t keycode, uint16_t code) {
    if (!code) return;
    if (keycode >= KEY_DOWN_MAX) { kev_synthetic(code); return; }
    uint64_t f = irq_save();
    int repeat = kev_delivered[keycode];
    if (er_admits(&kev, !repeat)) {           // else refused, whole
        er_put(&kev, code, 1);
        if (!repeat) {
            kev_delivered[keycode] = 1;
            kev_code[keycode] = code;
            kev.owed++;
        }
    }
    irq_restore(f);
}

static void kev_release(uint16_t keycode) {
    if (keycode >= KEY_DOWN_MAX) return;
    uint64_t f = irq_save();
    if (kev_delivered[keycode]) {
        kev_delivered[keycode] = 0;
        kev.owed--;
        er_put(&kev, kev_code[keycode], 0);
    }
    irq_restore(f);
}

int keyboard_try_get_key(int *out_code, int *out_down, uint8_t *out_mods) {
    struct edge_rec e;
    if (!er_get(&kev, &e)) return 0;
    if (out_code) *out_code = e.code;
    if (out_down) *out_down = e.down;
    if (out_mods) *out_mods = e.mods;
    return 1;
}

// The same keys by position. `phys_held` is what makes a PS/2 typematic
// repeat -- another make code for a key already down -- NOT an edge (USB
// and virtio report no repeats), and is the owed set: a refused press
// leaves its bit clear, so its release is dropped. A keycode past the
// bitmap is left out rather than wrapped.
static void phys_push(uint16_t keycode, int down) {
    if (!g_attached || keycode >= PHYS_KEYCODES) return;
    uint8_t bit = (uint8_t)(1u << (keycode & 7));
    uint64_t f = irq_save();
    int held = (phys_held[keycode >> 3] & bit) != 0;
    if (held != !!down) {                     // else a repeat, or a release of nothing
        if (!down) {
            phys_held[keycode >> 3] &= (uint8_t)~bit;
            phys.owed--;
            er_put(&phys, keycode, 0);
        } else if (er_admits(&phys, 1)) {     // else refused, whole
            phys_held[keycode >> 3] |= bit;
            phys.owed++;
            er_put(&phys, keycode, 1);
        }
    }
    irq_restore(f);
}

int keyboard_try_get_physical(uint16_t *out_keycode, int *out_down, uint8_t *out_mods) {
    struct edge_rec e;
    if (!er_get(&phys, &e)) return 0;
    if (out_keycode) *out_keycode = e.code;
    if (out_down) *out_down = e.down;
    if (out_mods) *out_mods = e.mods;
    return 1;
}

// EVERY compositor role change lands here (win_role.c): both streams and
// every owed release are emptied, so a new compositor -- a restart, a
// handoff -- is never handed the last one's edges, nor a release for a
// press it never saw.
void keyboard_events_attach(int on) {
    uint64_t f = irq_save();
    er_reset(&kev);
    er_reset(&phys);
    k_memset(kev_delivered, 0, sizeof kev_delivered);
    k_memset(phys_held, 0, sizeof phys_held);
    g_attached = on ? 1 : 0;
    irq_restore(f);
}

int keyboard_events_attached(void) { return g_attached; }

// THE KEYCODE CURRENTLY BEING TRANSLATED, so emit() can tell the key
// stream which key a character came from without every one of its ~20
// call sites passing it. Set at the top of key_event() and read only
// beneath it -- all of it with interrupts off (key_event()), since a
// polled source runs it from scheduler_idle() where IRQ1 could cut in.
static uint16_t emitting_keycode;

// Bytes still to drop from a Pause sequence -- see keyboard_feed_byte().
static int pause_swallow;

// A key produced `c`: to the compositor's event stream while one is
// attached, to the console terminal otherwise -- never both, so the
// terminal holds nothing stale when the keyboard comes back. `synthetic`
// is a character with no key of its own (a dead key's accent), which goes
// down and up at once.
// What one key sends the console terminal, held until interrupts are
// back on (key_event()): the line discipline echoes, and an echo can
// scroll a 1080p console for milliseconds. The most one key produces is
// a dead key's accent and the key; a few more slots cost nothing.
#define TTY_DEFER_MAX 4
static struct { uint16_t c; uint8_t mods; } g_tty_defer[TTY_DEFER_MAX];
static int g_tty_defer_n;
static int g_led_defer = -1;   // an LED mask to set after, or -1

static void emit(uint16_t c, int synthetic) {
    // The tap's view of the SAME push, so `kbd` can show a keycode and
    // the character it turned into on one line. Here rather than at the
    // ~20 call sites for the reason `emitting_keycode` is here.
    kbdtap_produced(c);
    if (g_attached) {
        if (synthetic) kev_synthetic(c);
        else kev_press(emitting_keycode, c);
    } else if (!tty_bypassed(tty_console())) {
        if (g_tty_defer_n < TTY_DEFER_MAX) {
            g_tty_defer[g_tty_defer_n].c = c;
            g_tty_defer[g_tty_defer_n++].mods = current_mods();
        }
    }
    // else: a held screen with no compositor yet -- the key goes nowhere
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
    // prefix arrives and drop the five bytes behind it. There is no break
    // code, so the release is REPORTED HERE, at once -- the wire is the
    // one place that knows it will never come, and every layer above (the
    // key stream, the positional stream, the tap) then sees an ordinary
    // press and release. USB and virtio report Pause's real release.
    //
    // THE SWALLOW IS CHECKED FIRST: the sequence carries a second E1, and
    // read as a new Pause it reported the key twice and ate three of the
    // bytes after it -- the next keystroke.
    if (pause_swallow) {
        pause_swallow--;
        return;
    }
    if (sc == 0xE1) {
        pause_swallow = 5;
        key_event(INPUT_KEY_PAUSE, 1, 0xE1, 0);
        key_event(INPUT_KEY_PAUSE, 0, 0, 0);   // invented: no wire byte
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
static void key_event_body(uint16_t keycode, int down, uint16_t wire, int extended);

// A KEY'S TRANSLATION WITH INTERRUPTS OFF: the modifier state,
// `emitting_keycode`, the dead-key composer and both streams are shared
// by IRQ producers (PS/2, virtio-input) and polled ones run from
// scheduler_idle() (USB HID); one CPU, so this is what serialises them.
// The SLOW halves -- the terminal's echo, the keyboard LED's i8042 wait --
// are collected inside and done after interrupts come back.
static void key_event(uint16_t keycode, int down, uint16_t wire, int extended) {
    uint64_t f = irq_save();
    g_tty_defer_n = 0;
    g_led_defer = -1;
    key_event_body(keycode, down, wire, extended);
    int n = g_tty_defer_n, led = g_led_defer;
    uint16_t out[TTY_DEFER_MAX];
    uint8_t mods[TTY_DEFER_MAX];
    for (int i = 0; i < n; i++) { out[i] = g_tty_defer[i].c; mods[i] = g_tty_defer[i].mods; }
    irq_restore(f);
    for (int i = 0; i < n; i++) tty_input(tty_console(), out[i], mods[i]);
    if (led >= 0) input_set_leds(led);
}

static void key_event_body(uint16_t keycode, int down, uint16_t wire, int extended) {
    // Modifiers first, and they are the only keys whose RELEASE matters.
    //
    // LEFT ALT AND RIGHT ALT ARE DIFFERENT KEYS HERE, deliberately: left
    // Alt is readline's Meta, while right Alt is AltGr, a LAYOUT
    // modifier that picks a third character. Conflating them would make
    // AltGr-b try to be Meta-b on a Nordic layout. evdev gives them
    // separate keycodes, so this needs no prefix bookkeeping -- which is
    // exactly the kind of thing the scancode encoding made fiddly.
    // The state is set BEFORE the event is queued, so the `mods`
    // riding with a modifier's own event describes the world AFTER that
    // key moved -- a Shift press reports KEY_MOD_SHIFT set. The
    // alternative reports every modifier press with the modifier absent,
    // which reads as a bug at every call site that looks.
    //
    // ONE EXIT rather than a `return` per case, so the tap and the
    // event stream are fed from the same place and cannot drift into
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
    // took it out of the byte stream: it used to push KEY_SUPER on
    // the press, which is why the Start menu opened the instant the key
    // went down. It is a modifier now (api/keyboard.h), so it reaches
    // the compositor as a TRANSITION and the "Super alone" gesture is
    // the compositor's policy rather than this driver's.
    case INPUT_KEY_LEFTMETA:
    case INPUT_KEY_RIGHTMETA:  super_pressed = down; mod_code = KEY_SUPER; break;
    default: break;
    }
    // After the modifier state moved, so a Shift press reports Shift held
    // -- the same "the world after this key" the event stream reports.
    if (keycode == INPUT_KEY_CAPSLOCK) {
        if (down && !caps_held) {
            caps_lock = !caps_lock;
            g_led_defer = caps_lock ? INPUT_LED_CAPS : 0;   // an i8042 wait: after
        }
        caps_held = down;
    }
    phys_push(keycode, down);
    if (mod_code) {
        // A modifier produces NO code in the byte stream, so the tap
        // records the event and nothing produced -- which is the honest
        // answer and the one that makes a Shift line readable: the
        // character column is empty and the modifier column is not.
        kbdtap_key(wire, extended, keycode, down, current_mods());
        // A modifier's autorepeat is not an event (X11 and Wayland do not
        // repeat one either): only its first press is queued.
        if (!g_attached) return;
        if (!down) kev_release(keycode);
        else if (keycode >= KEY_DOWN_MAX || !kev_delivered[keycode]) kev_press(keycode, mod_code);
        return;
    }

    // EVERY OTHER KEY, BOTH EDGES, opened here -- before any of the
    // ~20 paths below can return -- so that a key producing nothing is
    // still logged. "The scancode arrived and the layout gave back
    // nothing" is the single most useful line this tool prints, and a
    // record opened only where a character is emitted could never
    // carry it.
    kbdtap_key(wire, extended, keycode, down, current_mods());

    // A RELEASE REPORTS WHAT THE PRESS PRODUCED (kev_release()) -- and
    // nothing at all for a key whose press produced nothing.
    if (!down) {
        if (g_attached) kev_release(keycode);
        return;
    }

    // Recorded by emit() below, whichever of the many paths out of
    // this function ends up taking it.
    emitting_keycode = keycode;

    // Ctrl+Left/Right are word motion in every readline-ish line editor,
    // so they get their own codes -- exactly the KEY_SHIFT_ARROW_*
    // precedent below, resolved here from live modifier state at
    // keypress time for the same reason (see docs/decisions.md).
    if (ctrl_pressed && keycode == INPUT_KEY_LEFT)  { emit(KEY_CTRL_ARROW_LEFT, 0); return; }
    if (ctrl_pressed && keycode == INPUT_KEY_RIGHT) { emit(KEY_CTRL_ARROW_RIGHT, 0); return; }

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
    int kp = 0;   // a keypad character, composed with the layout's below
    switch (keycode) {
    case INPUT_KEY_UP:       emit(shift_pressed ? KEY_SHIFT_ARROW_UP : KEY_ARROW_UP, 0); return;
    case INPUT_KEY_DOWN:     emit(shift_pressed ? KEY_SHIFT_ARROW_DOWN : KEY_ARROW_DOWN, 0); return;
    case INPUT_KEY_LEFT:     emit(shift_pressed ? KEY_SHIFT_ARROW_LEFT : KEY_ARROW_LEFT, 0); return;
    case INPUT_KEY_RIGHT:    emit(shift_pressed ? KEY_SHIFT_ARROW_RIGHT : KEY_ARROW_RIGHT, 0); return;
    case INPUT_KEY_HOME:     emit(shift_pressed ? KEY_SHIFT_HOME : KEY_HOME, 0); return;
    case INPUT_KEY_END:      emit(shift_pressed ? KEY_SHIFT_END : KEY_END, 0); return;
    case INPUT_KEY_PAGEUP:   emit(KEY_PAGE_UP, 0); return;
    case INPUT_KEY_PAGEDOWN: emit(KEY_PAGE_DOWN, 0); return;
    case INPUT_KEY_DELETE:   emit(KEY_DELETE, 0); return;
    // THE WHOLE FUNCTION ROW. It was four -- F2/F3 (the file manager),
    // F10 (the menu bar) and F4 (Alt+F4) -- added one per caller; Doom
    // binds F1 through F11 and made the rest worth having. A KEY_*
    // special is never meta-prefixed, even on a terminal (tty_input()),
    // so Alt+F4 is always one key with KEY_MOD_ALT set.
    case INPUT_KEY_F1:  emit(KEY_F1, 0); return;
    case INPUT_KEY_F2:  emit(KEY_F2, 0); return;
    case INPUT_KEY_F3:  emit(KEY_F3, 0); return;
    case INPUT_KEY_F4:  emit(KEY_F4, 0); return;
    case INPUT_KEY_F5:  emit(KEY_F5, 0); return;
    case INPUT_KEY_F6:  emit(KEY_F6, 0); return;
    case INPUT_KEY_F7:  emit(KEY_F7, 0); return;
    case INPUT_KEY_F8:  emit(KEY_F8, 0); return;
    case INPUT_KEY_F9:  emit(KEY_F9, 0); return;
    case INPUT_KEY_F10: emit(KEY_F10, 0); return;
    case INPUT_KEY_F11: emit(KEY_F11, 0); return;
    case INPUT_KEY_F12: emit(KEY_F12, 0); return;

    // --- THE KEYS THAT USED TO REPORT NOTHING AT ALL ------------------
    //
    // See keyboard.h. Each of these was silently dropped: the layout
    // has no entry for it, so keyboard_layout_translate() returned 0 and
    // the key was indistinguishable from one that was never pressed.
    case INPUT_KEY_INSERT:     emit(KEY_INSERT, 0); return;
    case INPUT_KEY_COMPOSE:    emit(KEY_MENU, 0); return;
    case INPUT_KEY_CAPSLOCK:   emit(KEY_CAPS_LOCK, 0); return;
    case INPUT_KEY_NUMLOCK:    emit(KEY_NUM_LOCK, 0); return;
    case INPUT_KEY_SCROLLLOCK: emit(KEY_SCROLL_LOCK, 0); return;
    case INPUT_KEY_PAUSE:      emit(KEY_PAUSE, 0); return;
    case INPUT_KEY_SYSRQ:      emit(KEY_PRINT_SCREEN, 0); return;

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
    //
    // A keypad character is TEXT like any other, so it goes through the
    // dead-key composer below rather than being pushed from here -- a
    // pending accent must not survive it.
    case INPUT_KEY_KP0: kp = '0'; break;
    case INPUT_KEY_KP1: kp = '1'; break;
    case INPUT_KEY_KP2: kp = '2'; break;
    case INPUT_KEY_KP3: kp = '3'; break;
    case INPUT_KEY_KP4: kp = '4'; break;
    case INPUT_KEY_KP5: kp = '5'; break;
    case INPUT_KEY_KP6: kp = '6'; break;
    case INPUT_KEY_KP7: kp = '7'; break;
    case INPUT_KEY_KP8: kp = '8'; break;
    case INPUT_KEY_KP9: kp = '9'; break;
    case INPUT_KEY_KPDOT:      kp = '.'; break;
    case INPUT_KEY_KPPLUS:     kp = '+'; break;
    case INPUT_KEY_KPMINUS:    kp = '-'; break;
    case INPUT_KEY_KPASTERISK: kp = '*'; break;
    case INPUT_KEY_KPSLASH:    kp = '/'; break;
    case INPUT_KEY_KPENTER:    kp = '\n'; break;
    default: break;
    }

    int sym = kp ? kp : keyboard_layout_translate_caps(keycode, shift_pressed,
                                                        altgr_pressed, caps_lock);
    if (!sym) return;

    // DEAD KEYS COMPOSE HERE, for every driver at once -- the state is the
    // layout's (keyboard_layout.h). A key with Ctrl or Alt held is a
    // shortcut, not text: it neither composes nor disturbs a pending
    // accent, and a dead key under it is its accent alone.
    if (!ctrl_pressed && !alt_pressed) {
        uint8_t out[2];
        int n = keyboard_layout_compose(sym, out);
        // TWO CHARACTERS FROM ONE PRESS (an accent, then the key): the
        // accent has no key of its own, so it goes down AND up at once,
        // and the key is what this keycode's release reports.
        if (n == 2) emit(out[0], 1);
        if (n > 0) emit(out[n - 1], 0);
        return;
    }
    int c = keyboard_layout_spacing(sym);
    if (!c) return;

    // Ctrl folds a letter to its control code (Ctrl-A -> 0x01), which
    // is why Ctrl-H/I/J/M come out as backspace/tab/newline/return with
    // no special cases: in this encoding they ARE those keys, exactly as
    // in bash -- and every app's Ctrl+S is written against 0x13.
    //
    // **EVERYTHING ELSE IS THE KEY, WITH THE MODIFIER IN `mods`.** Ctrl
    // with a non-letter and Alt with anything arrive as the plain key
    // plus KEY_MOD_CTRL / KEY_MOD_ALT -- an X11 or Wayland keysym and its
    // state. What a TERMINAL wants instead (Alt as an ESC prefix, Ctrl
    // with a digit as nothing) is tty_input()'s job below the bypass, the
    // same split that turns an arrow into `ESC [ A` there and nowhere
    // else. Encoding it here reached windows too: Alt+Space in Doom was
    // an Esc press that never came up.
    if (ctrl_pressed) {
        int lower = c < 0x80 ? k_tolower((unsigned char)c) : c;
        if (lower >= 'a' && lower <= 'z') {
            emit((uint16_t)(lower - 'a' + 1), 0);
            return;
        }
    }
    emit((uint16_t)c, 0);
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
        // A held screen (vga.h) whose desktop never came back: the
        // console takes the screen AND the keyboard.
        if (vga_hold_expired()) keyboard_suspend_blocking(0);
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
        // but let Latin-1 letters through.
        if (c >= 128 && !IS_LATIN1_CHAR(c)) continue;

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
