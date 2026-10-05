#ifndef ULIB_UKEYMAP_H
#define ULIB_UKEYMAP_H

// A keyboard layout read from /usr/share/kbs/<name>, for SHOWING it --
// which character each key gives on each level, and which keys are
// dead. Typing goes through the kernel's own tables
// (kernel/lib/keyboard_layout.c); this is the same file read again in
// ring 3 so a picture of a layout needs no syscall and no switch.
//
// The file format is the kernel's (keyboard_layout.c's top comment):
//   kc_<keycode>[_shift][_altgr]=<char|0xNN|dead:<accent>>
//   dead:<accent>=<what it types alone>
// Keycodes are Linux evdev numbers; a character is one Latin-1 byte.
#include <stdint.h>

#define UKEYMAP_KEYS 128

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

// Reads /usr/share/kbs/<name>. Returns 1, or 0 when it is missing or
// unreadable; `m` is cleared either way.
int ukeymap_load(struct ukeymap *m, const char *name);

// The character key `kc` gives on `level` (0 for none), and whether it
// is a dead key there.
int ukeymap_char(const struct ukeymap *m, int kc, enum ukeymap_level level, int *is_dead);

#endif
