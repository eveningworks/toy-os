#ifndef KEYBOARD_TAP_H
#define KEYBOARD_TAP_H

#include <stdint.h>

// The keyboard TAP: a rolling record of the last few hundred key events,
// as the driver saw them, read from ring 3 through QUERY_KBDTAP and
// printed by /bin/kbd.
//
// WHY IT EXISTS
// -------------
// A keypress passes through four encodings before anything acts on it --
// a PS/2 scancode, an evdev keycode, a layout lookup, and the modifiers
// held at that instant (kernel/input.h has the stages) -- and a keyboard
// bug is nearly always one stage disagreeing with the next. From outside
// they are indistinguishable: a key does the wrong thing, or nothing.
// Every such hunt here has started by hand-writing a probe that prints
// all four at once. This is that probe, made permanent.
//
// **IT IS OFF UNTIL SOMEBODY TURNS IT ON, AND THAT IS A PRIVACY
// DECISION RATHER THAN A PERFORMANCE ONE.** A ring holding the last
// couple of hundred keystrokes is a keylogger by any honest description,
// and this kernel has NO privilege model: `SYS_QUERY` checks nothing, so
// while the tap is on, any ring-3 process can read what was typed --
// including at a prompt. The cost of recording is trivial (~5 KB and a
// bounded copy in the IRQ1 handler); the reason it is not on by default
// is that "kernel keylogger, enabled out of the box" is not a thing to
// ship in a system anyone else might run.
//
// So: `kernel.kbdtap` is a tunable, off by default, persisted like every
// other one -- and **turning it off WIPES the ring**, so "off" means
// there are no keystrokes in kernel memory rather than merely no new
// ones. `/bin/kbd`'s live mode arms it while it runs and disarms it on
// the way out, which is what makes the common case one command.
//
// The cost of being off by default is real and worth stating: the
// question this was built for -- "what did the key I just pressed do?"
// -- can only be answered after the fact if the tap was already on. Left
// off, every use of it starts by reproducing the bug. That is the trade
// this file used to make in the other direction.
//
// **LINUX KEEPS NO KEYPRESS HISTORY EITHER, and for a different
// reason.** evdev allocates its ring PER OPEN CLIENT, in evdev_open(),
// so with nobody holding /dev/input/eventN there is no buffer to fill
// and evtest sees only what arrives after it starts; the nearest thing
// the input core retains is a CURRENT STATE bitmap (EVIOCGKEY), which is
// state, not history. Its reason is scale rather than privacy -- a ring
// per device per client -- but the resulting posture is the same one,
// and worth landing on deliberately rather than by accident.
//
// KEYBOARD ONLY, deliberately. Pointer motion arrives hundreds of times
// a second and would evict every keypress from a ring this size before
// anyone could read it; a pointer tap wants its own buffer and its own
// filtering, and is not this.
//
// EVERYTHING HERE RUNS IN AN INTERRUPT HANDLER. No allocation, no wake,
// no lock -- the same restraint the key event stream beside it observes
// (api/keyboard.h). Recording is a bounded copy into a static ring.

// Whether the tap is recording, and the switch behind `kernel.kbdtap`.
//
// **DISABLING WIPES**, which is the half that makes the switch mean
// something: stopping new records while leaving the last 128 keystrokes
// readable by anything that asks is not "off". Enabling wipes too, so a
// session always starts clean.
//
// The sequence numbers do NOT restart -- they are a counter, not
// keystroke data, and keeping them monotonic preserves the "never
// reused" property a reader relies on to tell a repeat from a gap.
void kbdtap_set_enabled(int on);
int  kbdtap_enabled(void);

// Open a record for one key event. A no-op while the tap is off. `wire` is the PS/2 byte INCLUDING its
// release bit, or 0 for a key that did not arrive over PS/2 (0x00 is not
// a scancode, so zero is unambiguous); `extended` says it followed an
// 0xE0 prefix. `mods` is sampled by the caller AFTER a modifier key has
// updated the state, so a Shift press reports Shift held -- the same
// rule the key event stream follows, and for the same reason.
void kbdtap_key(uint16_t wire, int extended, uint16_t keycode, int down,
                uint8_t mods);

// Attach a code this event put into the console byte stream to the
// record kbdtap_key() opened most recently. Called once for an ordinary
// key and twice for Alt-<key>, which is encoded as ESC then the key.
// Silently ignored past QUERY_KBDTAP_PRODUCED_MAX, and a no-op before
// any record has been opened.
void kbdtap_produced(uint16_t code);

// The provider's two halves, callable directly: record `index` into a
// struct query_kbdtap, and how many are retained. A KTEST that holds
// the preemption guard reads through these, never query_read() -- the
// registry's lock may have a sleeping holder (query.c).
int kbdtap_count(void);
int kbdtap_fill(int index, void *out);

// The QUERY_KBDTAP provider that reads this ring is declared with every
// other one in api/query.h, and lives in keyboard_tap.c beside the code
// that fills it.

#endif
