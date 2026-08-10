#ifndef FONT_CONFIG_H
#define FONT_CONFIG_H

#include "font_ttf.h" // enum font_size

// Font size persistence, layered on top of gfx_set_font_size()
// (kernel/drivers/gfx.c) -- follows the same /etc convention as tz.c's
// timezone config: a small, single-purpose file read once at boot. It's
// key=value ("font_size=tiny") rather than timezone's bare city name,
// since a config value benefits from being self-describing on its own
// -- `cat /etc/fontsize` reads sensibly without needing to know which
// file you're looking at. See kernel/core/font_config.c's top comment
// for the exact format and why it's not a general key=value parser
// shared across settings (yet).

// Call once at boot, after fs_init()/fs_mkdir("/etc") (same ordering as
// tz_init() -- see kernel.c) -- loads /etc/fontsize if present and
// applies it via gfx_set_font_size(). Does nothing (keeps gfx.c's
// compiled-in default) if the file doesn't exist yet or its value isn't
// one of the recognized size names.
void font_config_init(void);

// Persists `size` to /etc/fontsize as "font_size=<name>\n" so it
// survives a reboot. Does NOT call gfx_set_font_size() itself -- same
// split as tz_set_index() (selects + persists) vs rtc_read_local()
// (applies); callers that want to change the active size call
// gfx_set_font_size() themselves and this separately (see the shell's
// `fontsize` command).
void font_config_save(enum font_size size);

#endif
