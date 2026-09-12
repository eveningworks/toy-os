#ifndef BRIGHTNESS_POPUP_H
#define BRIGHTNESS_POPUP_H

// The taskbar's brightness flyout: one slider for the panel backlight,
// anchored above its tray icon. A tray_slider_popup.h instance and
// nothing else -- it talks to nothing but the registered
// `system.brightness` setting over SYS_SETTING.
//
// A DISPLAY WITH NO BACKLIGHT STILL HAS THE ITEM. The setting is
// registered on every machine and answers `unavailable` with a sentence
// where the hardware is missing (a VM, a desktop's monitor); the panel
// shows that sentence over a disabled track rather than hiding, the
// rule setting_abi.h states for every client. This is also what makes
// the flyout testable in QEMU, where no backlight exists.
#include <stdint.h>

extern int brightness_open;

void brightness_open_now(void);
void brightness_close(void);
void brightness_tray_init(void);
void brightness_poll_config(void);
void brightness_draw(int mx, int my);
int  brightness_handle_click(int mx, int my);
int  brightness_hover_at(int mx, int my);
void brightness_damage(void);
void brightness_update_press(int mx, int my, uint8_t buttons);
int  brightness_handle_wheel(int mx, int my, int notches);

// The live geometry, for `gui brightness` and therefore for tests --
// the same numbers drawing and hit-testing use.
struct brightness_geom {
    int x, y, w, h;                   // the panel
    int icon_x, icon_y, icon_w, icon_h;
    int slider_x, slider_y, slider_w, slider_h;
    int level;                        // what the slider draws
    int available;                    // 0 when the setting is unavailable
    int tray_x, tray_y, tray_w, tray_h;
};
void brightness_geometry(struct brightness_geom *out);
// The `unavailable` sentence, empty when the backlight is controllable.
const char *brightness_unavailable_text(void);

// Is the sun currently out of the strip? `desktop.tray_brightness`
// resolved against the backlight -- reported so a test can assert the
// absence, which is the only evidence a hidden item can offer.
int brightness_tray_hidden(void);

#endif
