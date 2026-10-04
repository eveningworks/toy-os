#ifndef KEYBOARD_LAYOUT_H
#define KEYBOARD_LAYOUT_H

#include <stdint.h>

// Data-driven keyboard keycode->character translation -- deliberately
// split out of kernel/drivers/keyboard.c (the driver): keyboard.c's job
// is turning raw 8042 bytes into scancodes/shift-state/extended-prefix
// handling, not owning per-region character tables. This file owns the
// tables (loaded from /etc/kbs/<name> -- see kernel/core/etc_config.h's
// sibling convention for /etc files, though this one is its own small
// parser, not etc_config_get/set -- see keyboard_layout.c's top comment
// for why) and the one lookup keyboard.c's keyboard_feed_byte() calls
// into. See docs/decisions.md for the full reasoning and
// tools/gen_kbs.py for how the on-disk files are generated.

#define KB_LAYOUT_NAME_MAX 8

// Loads scancode->character mappings for the named layout from
// /etc/kbs/<name>, replacing whatever was previously loaded. Falls
// back automatically (see keyboard_layout.c) to /etc/kbs/us, and if
// even that's missing, to a small compiled-in US table -- so this
// never leaves the keyboard producing nothing, no matter what's
// actually on disk. Returns 1 if /etc/kbs/<name> itself was found and
// loaded, 0 if a fallback was used instead (callers -- e.g. the
// shell's `keyboard` command -- can use this to tell the user their
// requested layout wasn't found, rather than silently pretending it
// was applied).
int keyboard_layout_load(const char *name);

// The name of whatever layout actually ended up active (which may not
// be what was last requested, if that request fell back -- see
// keyboard_layout_load()'s return value). Always a valid, non-null,
// NUL-terminated string.
const char *keyboard_layout_current(void);

// A SYMBOL: what a key means on the active layout -- 0 for nothing, a
// Latin-1 character 1..0xFF, or a DEAD KEY at KB_SYM_DEAD_BASE + n. An
// int rather than a char: Latin-1 above 0x7F is negative in a `char`.
#define KB_SYM_DEAD_BASE 0x100
#define KB_SYM_DEAD(n)   (KB_SYM_DEAD_BASE + (n))
#define KB_SYM_IS_DEAD(s) ((s) >= KB_SYM_DEAD_BASE)

// Translates one LINUX EVDEV KEYCODE under the given Shift/AltGr state
// to a symbol (above). A keycode, not an AT scancode: every non-PS/2
// keyboard reports evdev natively, and the PS/2 driver translates its
// wire once on the way in, as Linux's atkbd does (docs/decisions.md).
//
// Shift+AltGr is XKB level 4; a key with no level-4 symbol falls back to
// level 3, and a key with no AltGr symbol at all falls through to
// Shift/base -- so AltGr over an ordinary key still types it rather than
// eating the keystroke.
int keyboard_layout_translate(uint16_t keycode, int shift, int altgr);

// The same, under CAPS LOCK: xkb's rule for an "alphabetic" key -- Caps
// inverts Shift on a key whose unshifted symbol is a lowercase letter
// and whose shifted one is its capital (ASCII or Latin-1, so e-acute
// capitalises and sharp s does not). Digits and punctuation are
// untouched and Caps+Shift types lowercase, as on Windows and Linux.
int keyboard_layout_translate_caps(uint16_t keycode, int shift, int altgr, int caps);

// --- dead keys -------------------------------------------------------
//
// **THE STATE LIVES HERE, BEHIND EVERY DRIVER.** PS/2, virtio-input and
// USB HID all reach keyboard.c's key_event(), which is the one caller --
// so a dead key typed on one keyboard composes with a letter typed on
// another, as on Linux, where the console's accent table (KDSKBDIACR)
// sits in the keyboard driver above every device.
//
// Feed each translated symbol through keyboard_layout_compose(); it
// writes the 0, 1 or 2 characters to emit into `out`:
//   dead key                  -> nothing; the accent is pending
//   pending + composable char -> the composed character (dead acute, e)
//   pending + Space           -> the accent alone
//   pending + the same dead   -> the accent alone
//   pending + another dead    -> the first accent; the second pends
//   pending + Backspace/Esc   -> nothing; the accent is taken back
//   pending + anything else   -> the accent, then the key (Windows)
// Loading a layout drops a pending accent.
int keyboard_layout_compose(int sym, uint8_t out[2]);

// What a symbol types with no composition: a character is itself, a
// dead key its accent alone (0 if the layout gave it none). For a key
// pressed with Ctrl or Alt, which is a shortcut rather than text.
int keyboard_layout_spacing(int sym);

// The pending dead key's symbol, or 0; and a way to drop it. For tests.
int keyboard_layout_dead_pending(void);
void keyboard_layout_compose_reset(void);

// Parses a layout from memory instead of /etc/kbs, replacing the active
// tables; 1 if it mapped anything. For KTESTs, which restore the real
// layout with keyboard_layout_load() afterwards. Does not change
// keyboard_layout_current().
int keyboard_layout_load_text(const char *data, uint32_t size);

#endif
