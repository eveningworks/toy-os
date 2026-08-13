#ifndef KEYBOARD_LAYOUT_H
#define KEYBOARD_LAYOUT_H

#include <stdint.h>

// Data-driven keyboard scancode->character translation -- deliberately
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

// Translates one scancode (0-127) under the given shift/AltGr state to
// the character the active layout produces, or 0 if that combination
// doesn't produce a character in this layout (an unmapped key -- same
// "0 means nothing" convention keyboard.c's own tables always used).
// Values above ASCII are the same Latin-1 codepoints keyboard.h's
// CHAR_A_RING/CHAR_A_DIAERESIS/etc already use. `altgr` takes priority
// over `shift` when both are set (this is XKB "level 3" -- AltGr alone
// -- not "level 4" -- Shift+AltGr, which isn't tracked as a separate
// combination; a real Shift+AltGr press just reads as AltGr here,
// same simplification tools/gen_kbs.py's generator makes on the data
// side by only emitting levels 1-3, not 4). No AltGr entry for the
// pressed key falls through to whatever shift/base would have
// produced, exactly like an unmapped scancode always has.
char keyboard_layout_translate(uint8_t scancode, int shift, int altgr);

#endif
