#ifndef START_BUTTON_CONFIG_H
#define START_BUTTON_CONFIG_H

// Registers `desktop.start_button` (text | icon | both). Persist-only:
// the taskbar that reads it is a ring-3 process. See the .c file.
void start_button_setting_register(void);

#endif
