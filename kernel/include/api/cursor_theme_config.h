#ifndef CURSOR_THEME_CONFIG_H
#define CURSOR_THEME_CONFIG_H

// The cursor THEME and SIZE settings' registry descriptors. See
// kernel/lib/cursor_theme_config.c for why the kernel owns the
// description of a setting whose behaviour belongs to the compositor.
//
// Called from settings_init() alongside the other four, so `config`,
// Control Panel and `SYS_SETTING` all see these two whether or not a
// desktop is running -- which is the point: a setting that only exists
// while its owner is up cannot be listed, changed or documented from
// anywhere else.
void cursor_theme_setting_register(void);

#endif
