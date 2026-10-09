#ifndef ULIB_UKEYMAP_H
#define ULIB_UKEYMAP_H

// A keyboard layout read from /usr/share/kbs/<name>: which character
// each key gives on each level, and which keys are dead -- a snapshot
// for SHOWING a layout. Parsed by the kernel's own code, compiled into
// ring 3 (keyboard_layout.c), so a picture needs no syscall.
//
// **LOADING ONE ALSO MAKES IT THIS PROCESS'S TRANSLATOR**: after
// ukeymap_load(), keyboard_layout_translate() and _compose() (api/
// keyboard_layout.h) type with that layout -- which is how the
// on-screen keyboard types. A process showing several layouts
// translates with whichever it loaded last.
//
// The file format is the kernel's (keyboard_layout.c's top comment):
//   kc_<keycode>[_shift][_altgr]=<char|0xNN|dead:<accent>>
//   dead:<accent>=<what it types alone>
// Keycodes are Linux evdev numbers; a character is one Latin-1 byte.
#include <stdint.h>

#define UKEYMAP_KEYS 256   // the kernel's KB_KEYCODE_MAX

enum ukeymap_level { UKEYMAP_BASE, UKEYMAP_SHIFT, UKEYMAP_ALTGR, UKEYMAP_SHIFT_ALTGR };

struct ukeymap {
    // Per keycode and level: the Latin-1 character, 0 for none. A dead
    // key holds its accent as typed alone, with its bit set in `dead`.
    uint8_t ch[UKEYMAP_KEYS][4];
    uint8_t dead[UKEYMAP_KEYS];   // bit `level` set: that level is dead
};

// Parses a layout from memory. Returns 1, or 0 for no key at all; a line
// it cannot read is skipped, as the kernel's parser skips it.
int ukeymap_parse(struct ukeymap *m, const char *text, uint32_t len);

// Copies whatever this process's translator holds now -- after
// keyboard_layout_use_fallback(), say, when no layout file can be read.
void ukeymap_snapshot(struct ukeymap *m);

// Reads /usr/share/kbs/<name>. Returns 1, or 0 when it is missing or
// unreadable; `m` is cleared either way.
int ukeymap_load(struct ukeymap *m, const char *name);

// The character key `kc` gives on `level` (0 for none), and whether it
// is a dead key there.
int ukeymap_char(const struct ukeymap *m, int kc, enum ukeymap_level level, int *is_dead);

#endif
