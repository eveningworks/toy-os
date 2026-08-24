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
// **IT RECORDS WHETHER OR NOT ANYTHING IS READING**, which is the design
// decision worth defending, because it costs a ring buffer and a handful
// of instructions in the IRQ1 handler forever. What it buys is that the
// question can be asked AFTER the fact: press the key that misbehaved,
// then run `kbd --last`. An arm-and-drain tap can only watch keys
// pressed from now on, so every use of it begins by reproducing the bug
// with the tool already open -- and an intermittent one may not oblige.
// dmesg makes exactly this trade, and Linux's evdev buffers every event
// whether or not a client has the node open.
//
// KEYBOARD ONLY, deliberately. Pointer motion arrives hundreds of times
// a second and would evict every keypress from a ring this size before
// anyone could read it; a pointer tap wants its own buffer and its own
// filtering, and is not this.
//
// EVERYTHING HERE RUNS IN AN INTERRUPT HANDLER. No allocation, no wake,
// no lock -- the same restraint the transition queue beside it observes
// (api/keyboard.h). Recording is a bounded copy into a static ring.

// Open a record for one key event. `wire` is the PS/2 byte INCLUDING its
// release bit, or 0 for a key that did not arrive over PS/2 (0x00 is not
// a scancode, so zero is unambiguous); `extended` says it followed an
// 0xE0 prefix. `mods` is sampled by the caller AFTER a modifier key has
// updated the state, so a Shift press reports Shift held -- the same
// rule the transition queue follows, and for the same reason.
void kbdtap_key(uint16_t wire, int extended, uint16_t keycode, int down,
                uint8_t mods);

// Attach a code this event put into the console byte stream to the
// record kbdtap_key() opened most recently. Called once for an ordinary
// key and twice for Alt-<key>, which is encoded as ESC then the key.
// Silently ignored past QUERY_KBDTAP_PRODUCED_MAX, and a no-op before
// any record has been opened.
void kbdtap_produced(uint16_t code);

// The QUERY_KBDTAP provider that reads this ring is declared with every
// other one in api/query.h, and lives in keyboard_tap.c beside the code
// that fills it.

#endif
