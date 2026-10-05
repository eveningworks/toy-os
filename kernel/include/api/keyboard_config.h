#ifndef KEYBOARD_CONFIG_H
#define KEYBOARD_CONFIG_H

// Keyboard layout configuration in /etc/toyos.conf, layered on top of
// keyboard_layout_load() (kernel/lib/keyboard_layout.c), which owns the
// tables. Three registry settings, all in Input > Keyboard:
//   system.keyboard_layouts   the list Super+Space cycles, "fi,us,de";
//                             the FIRST is what a boot starts with
//   system.keyboard_layout    the ACTIVE one -- live, never persisted
//   system.keyboard_dead_keys on|off, for every layout at once

// Call once at boot, after the /etc readers' prerequisites (kernel.c):
// the dead-keys switch, then the list (or a pre-list machine's single
// `keyboard_layout` key), then the `kbd=` override or the list's first.
void keyboard_config_init(void);

// The live list, comma-separated. Never NULL or empty after init.
const char *keyboard_config_layouts(void);

// Makes `name` the layout a boot starts with: moved to the front of the
// list, or put there, and persisted. Does NOT load it -- the shell's
// `keyboard` command does that itself. Returns an `enum setting_result`
// (etc_config.h).
int keyboard_config_save(const char *name);

// Announces the three settings to the registry (setting.h).
void keyboard_config_setting_register(void);

#endif
