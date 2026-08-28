#ifndef KERNEL_SOUND_CONFIG_H
#define KERNEL_SOUND_CONFIG_H

// The output volume, persisted to /etc and applied through
// sound_set_volume(). Same shape as mouse_config.h.
void sound_config_init(void);
void sound_config_setting_register(void);

#endif
