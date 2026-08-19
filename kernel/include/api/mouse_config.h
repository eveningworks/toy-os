#ifndef MOUSE_CONFIG_H
#define MOUSE_CONFIG_H

// Pointer speed and acceleration, persisted to /etc. See
// kernel/lib/mouse_config.c; the behaviour itself is mouse.c's.

// Reads /etc/toyos.conf and applies whatever it finds. Called from
// kernel_main() alongside the other *_config_init()s.
void mouse_config_init(void);

// Announces both settings to the registry (api/setting.h), which is what
// gives them a System Settings page and a `config` entry.
void mouse_config_setting_register(void);

#endif
