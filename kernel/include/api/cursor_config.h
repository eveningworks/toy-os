#ifndef CURSOR_CONFIG_H
#define CURSOR_CONFIG_H

// Persists the console's cursor style across reboots, exactly the way
// font_config.c persists the font size and tz.c the timezone: one
// `cursor_style=<name>` key inside the shared /etc/toyos.conf, read and
// applied once at boot, written back when the shell's `cursor` command
// changes it.
//
// The style names and the enum live in vga.h (VGA_CURSOR_STYLE_NAMES /
// vga_cursor_style_parse()), not here -- this file is only the /etc
// plumbing, so adding a cursor style needs no change to it at all.
#include "vga.h"

// Called from kernel_main() after fs_init()/fs_mkdir("/etc"), next to
// font_config_init(). Leaves the built-in default in place if the key
// is absent or names a style that doesn't exist.
void cursor_config_init(void);

// Persists `style` as /etc/toyos.conf's "cursor_style=<name>" key.
// Returns an `enum setting_result` (etc_config.h): SETTING_INVALID for
// an out-of-range style, SETTING_SAVED if written, SETTING_UNSAVED if
// the write failed.
int cursor_config_save(enum vga_cursor_style style);

#endif
