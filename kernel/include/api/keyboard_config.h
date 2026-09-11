#ifndef KEYBOARD_CONFIG_H
#define KEYBOARD_CONFIG_H

// Keyboard layout persistence, layered on top of
// keyboard_layout_load() (kernel/lib/keyboard_layout.c) -- a
// "keyboard_layout=<name>" key in the shared /etc/toyos.conf every
// setting lives in by default (see etc_config.h), read/applied once at
// boot. Same split as font_config.c/tz.c: this file only selects +
// persists, it doesn't touch the scancode tables themselves -- that's
// keyboard_layout.c's job, and it works from a plain layout NAME now
// (whatever /etc/kbs/<name> exists), not a fixed compiled-in enum.

// Call once at boot, after fs_init()/fs_mkdir("/etc") (same ordering as
// the other /etc readers -- see kernel.c) -- loads the persisted
// keyboard_layout key if present and applies it via
// keyboard_layout_load(). Falls back to "us" (see
// keyboard_layout_load()'s own fallback chain) if no config exists yet.
void keyboard_config_init(void);

// Persists `name` as /etc/toyos.conf's "keyboard_layout=<name>" key so
// it survives a reboot. Does NOT call keyboard_layout_load() itself --
// same split as font_config_save(); the shell's
// `keyboard` command calls keyboard_layout_load() itself and this
// separately.
// Returns an `enum setting_result` (etc_config.h): SETTING_INVALID for
// an empty name, SETTING_SAVED if written, SETTING_UNSAVED if the write
// failed.
int keyboard_config_save(const char *name);

// Announces this setting to the registry (setting.h). Its choice list
// is read from /etc/kbs at call time -- see keyboard_config.c.
void keyboard_config_setting_register(void);

#endif
