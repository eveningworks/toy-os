#ifndef WM_LAYOUT_POPUP_H
#define WM_LAYOUT_POPUP_H

// THE TRAY'S KEYBOARD-LAYOUT ITEM: the active layout's code ("FI"), a
// flyout listing the layouts to switch to, and the Super+Space switcher
// that is the same flyout driven from the keyboard.
//
// HIDDEN WITH ONE LAYOUT, as Windows and KDE hide theirs: an indicator
// for a choice that does not exist is noise. The list and the active
// layout are kernel settings (system.keyboard_layouts / _layout); this
// switches by SETTING the active one, which loads it and is never
// persisted -- a boot starts with the list's first.
//
// SUPER+SPACE: the first press opens the flyout with the NEXT layout
// picked, each further Space (Shift+Space back) moves the pick, and
// releasing Super switches to it -- Windows 11's and GNOME's shape. A
// quick tap is therefore "the next layout". Esc cancels.

#include <stdint.h>

#define LAYOUT_MAX 8

extern int layout_open;

void layout_tray_init(void);
void layout_poll(void);
void layout_draw(int mx, int my);
int  layout_handle_click(int mx, int my);
int  layout_hover_at(int mx, int my);
int  layout_key(int key, uint8_t mods);
void layout_damage(void);
int  layout_rect(int *x, int *y, int *w, int *h);
void layout_close(void);

// The switcher, from wm.c's key loop. step() returns 0 when there is
// nothing to switch between, so the key goes on to its other uses.
int  layout_switch_step(int dir);
int  layout_switching(void);
void layout_switch_commit(void);

// For `gui layout --json` (wm_debug.c).
int  layout_count(void);
int  layout_active(void);
int  layout_pick(void);
const char *layout_code(int i);
int  layout_tray_hidden(void);
// Centres, in screen coordinates, of the tray item, row `i` and the
// footer's button -- what a test clicks. 0 when there is no such thing.
int  layout_tray_center(int *x, int *y);
int  layout_row_center(int i, int *x, int *y);
int  layout_button_center(int *x, int *y);

#endif // WM_LAYOUT_POPUP_H
