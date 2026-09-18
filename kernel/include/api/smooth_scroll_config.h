#ifndef SMOOTH_SCROLL_CONFIG_H
#define SMOOTH_SCROLL_CONFIG_H

// Whether a wheel or keyboard scroll glides to its destination or lands
// there at once. One persist-only setting; the ring-3 toolkit reads it
// at each scroll (userland/ui/uui_anim.c) and does the work.
void smooth_scroll_setting_register(void);

#endif // SMOOTH_SCROLL_CONFIG_H
