#ifndef ICON_SIZE_CONFIG_H
#define ICON_SIZE_CONFIG_H

// Registers `desktop.icon_size` (small | medium | large). Persist-only:
// the desktop that reads it is a ring-3 process. See the .c file.
void icon_size_setting_register(void);

#endif
