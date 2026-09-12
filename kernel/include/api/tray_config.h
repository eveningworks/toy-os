#ifndef TRAY_CONFIG_H
#define TRAY_CONFIG_H

// Registers the notification area's per-item visibility settings
// (`desktop.tray_brightness`, ...), each `auto` | `always` | `never`.
// Persist-only: the tray is drawn by a ring-3 process. See the .c file.
void tray_setting_register(void);

// What the WM compares a setting's value against. `auto` is the only
// one that consults the hardware.
#define TRAY_SHOW_AUTO   "auto"
#define TRAY_SHOW_ALWAYS "always"
#define TRAY_SHOW_NEVER  "never"

#endif
