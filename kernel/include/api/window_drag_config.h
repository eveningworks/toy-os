#ifndef WINDOW_DRAG_CONFIG_H
#define WINDOW_DRAG_CONFIG_H

// What a window shows while it is being moved or resized: the window
// itself, or an outline. Two persist-only settings; the window manager
// reads them and does the work (userland/wm/wm_input.c).
void window_drag_setting_register(void);

#endif // WINDOW_DRAG_CONFIG_H
