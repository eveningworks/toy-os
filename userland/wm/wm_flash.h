#ifndef WM_FLASH_H
#define WM_FLASH_H

// THE SHUTTER FLASH: a white wash over the whole screen that fades out
// in a quarter of a second, when a screenshot is saved with the flash
// asked for (WIN_NOTICE_F_FLASH). GNOME Shell's Flashspot. The
// compositor draws it because the tool may have no window by then -- a
// Shift+PrtSc capture never has one.
//
// A PASSIVE overlay that repaints the screen every frame while it is up
// (wm_overlay.h), so it costs nothing once faded.

void wm_flash_start(void);
int  wm_flash_open(void);
void wm_flash_draw(int mx, int my);
// Milliseconds until the next frame it needs, or -1 when it is idle.
int  wm_flash_wait_ms(void);

#endif
