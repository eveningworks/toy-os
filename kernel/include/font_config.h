#ifndef FONT_CONFIG_H
#define FONT_CONFIG_H

#include "font_ttf.h" // enum font_size

// Font size persistence, layered on top of gfx_set_font_size()
// (kernel/drivers/gfx.c) -- a "font_size=<n>" key in the shared
// /etc/toyos.conf every setting lives in by default (see
// etc_config.h), read/applied once at boot. See
// kernel/core/font_config.c's top comment for the exact key, the
// legacy /etc/fontsize file it migrates forward from, and why this
// isn't its own hand-rolled parser anymore.

// Call once at boot, after fs_init()/fs_mkdir("/etc") (same ordering as
// tz_init() -- see kernel.c) -- loads the persisted font_size key if
// present and applies it via gfx_set_font_size(). Does nothing (keeps
// gfx.c's compiled-in default) if no config exists yet or its value
// isn't one of the recognized size names (the point-size numbers
// font_ttf.h bakes in -- see its own top comment).
void font_config_init(void);

// Persists `size` as /etc/toyos.conf's "font_size=<n>" key so it
// survives a reboot. Does NOT call gfx_set_font_size() itself -- same
// split as tz_set_index() (selects + persists) vs rtc_read_local()
// (applies); callers that want to change the active size call
// gfx_set_font_size() themselves and this separately (see the shell's
// `fontsize` command).
void font_config_save(enum font_size size);

#endif
