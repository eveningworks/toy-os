#ifndef WINDOW_EFFECTS_CONFIG_H
#define WINDOW_EFFECTS_CONFIG_H

// The desktop's visual effects: `desktop.shadows` (drop shadows under
// windows and popups). Persist-only; the window manager adopts them on
// its generation poll (userland/wm/wm_shadow.c).
void window_effects_setting_register(void);

#endif // WINDOW_EFFECTS_CONFIG_H
