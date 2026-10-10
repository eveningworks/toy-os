#ifndef FONT_CONFIG_H
#define FONT_CONFIG_H

#include "font_ttf.h" // enum font_size

// Font size persistence, layered on top of gfx_set_font_size()
// (kernel/drivers/gfx.c) -- a "font_size=<n>" key in the shared
// /etc/toyos.conf every setting lives in by default (see
// etc_config.h), read/applied once at boot. See
// kernel/lib/font_config.c's top comment for the exact key.

// Call once at boot, after fs_init()/fs_mkdir("/etc") (same ordering as
// an INIT_CONFIG initcall, see kernel.c) -- loads the persisted font_size key if
// present and applies it via gfx_set_font_size(). Does nothing (keeps
// gfx.c's compiled-in default) if no config exists yet or its value
// isn't one of the recognized size names (the point-size numbers
// font_ttf.h bakes in -- see its own top comment).
void font_config_init(void);

// Persists `size` as /etc/toyos.conf's "font_size=<n>" key so it
// survives a reboot. Does NOT call gfx_set_font_size() itself -- same
// split as a setting's apply (selects + persists) vs a plain read
// (applies); callers that want to change the active size call
// gfx_set_font_size() themselves and this separately (see the shell's
// `fontsize` command).
// Returns an `enum setting_result` (etc_config.h): SETTING_SAVED if
// the key was written, SETTING_UNSAVED if `size` was accepted but the
// write failed -- callers are expected to SAY SO rather than report an
// unqualified success, see the shell's `fontsize`.
int font_config_save(enum font_size size);

// Persists an arbitrary point size, and the selected face's name (an
// empty name meaning the baked font). Both write into the same
// /etc/toyos.conf the size has always lived in.
// APPLY: set the font, persist it, and notify every GUI client that its
// cached metrics are stale. The entry point the shell and the settings
// registry both use -- see font_config.c for why the notification lives
// here rather than at each caller. Returns a SETTING_* code.
int font_config_apply_px(int px);
int font_config_apply_face(const char *name);

int font_config_save_px(int px);

// The size the desktop draws at, as `system.font_size` reports it --
// any size with a face loaded, a baked one without (gfx_font_px() is
// always baked: the console's).
int font_config_px(void);
int font_config_save_face(const char *name);

// Announces this setting to the registry (setting.h), so it appears in
// `settings` and in the ring-3 Control Panel. Called from
// settings_init(); the registry's `apply` does what `fontsize` does --
// validate, apply live, persist -- so the two cannot drift.
void font_config_setting_register(void);

#endif
