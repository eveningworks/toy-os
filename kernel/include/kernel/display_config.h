#ifndef KERNEL_DISPLAY_CONFIG_H
#define KERNEL_DISPLAY_CONFIG_H

// The panel backlight as a registered setting (`system.brightness`),
// persisted to /etc and applied through display_backlight_set().
// sound_config.h's shape.
void display_config_init(void);
void display_config_setting_register(void);

#endif
