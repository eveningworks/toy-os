#ifndef KEYBOARD_CONFIG_H
#define KEYBOARD_CONFIG_H

#include "keyboard.h" // enum keyboard_layout

// Keyboard layout persistence, layered on top of keyboard_set_layout()
// (kernel/drivers/keyboard.c) -- a "keyboard_layout=<us|se>" key in the
// shared /etc/toyos.conf every setting lives in by default (see
// etc_config.h), read/applied once at boot. Same split as
// font_config.c/tz.c: this file only selects + persists, it doesn't
// touch the scancode tables themselves.

// Call once at boot, after fs_init()/fs_mkdir("/etc") (same ordering as
// tz_init()/font_config_init() -- see kernel.c) -- loads the persisted
// keyboard_layout key if present and applies it via
// keyboard_set_layout(). Does nothing (keeps keyboard.c's compiled-in
// KB_LAYOUT_US default) if no config exists yet or its value isn't a
// recognized layout name.
void keyboard_config_init(void);

// Persists `layout` as /etc/toyos.conf's "keyboard_layout=<us|se>" key
// so it survives a reboot. Does NOT call keyboard_set_layout() itself --
// same split as tz_set_index()/font_config_save(); the shell's
// `keyboard` command calls keyboard_set_layout() itself and this
// separately.
void keyboard_config_save(enum keyboard_layout layout);

#endif
