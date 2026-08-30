#ifndef VOLUME_POPUP_H
#define VOLUME_POPUP_H

// The taskbar's volume flyout: a level slider, a mute toggle and the
// list of output devices, in a panel anchored above its tray icon.
// Same peer-file pattern as calendar_popup.h -- state, drawing and
// hit-testing for one popup, sharing the window manager's globals
// through wm_internal.h.
//
// WHY THE PANEL OWNS IT. Windows' volume flyout, KDE Plasma's audio
// applet and GNOME's quick settings are all drawn by the SHELL from its
// tray icon, not by an application window; adjusting the volume should
// not cost a process spawn and a taskbar button. (In Windows and KDE
// the applet is a separate process from the shell -- here it is not,
// because the tray API carries no click callback and inventing one to
// host a single panel would be the larger change. See docs/decisions.md.)
//
// WHAT IT TALKS TO. Nothing about audio: it reads and writes the two
// registered settings, `system.volume` and `system.audio_device`, over
// SYS_SETTING. The device list is that setting's own CHOICE list, so a
// card plugged in after boot appears here with no code in this file
// knowing what a card is.
//
// THE WRITE IS DEBOUNCED, and that is the one thing to know before
// editing. A setting write is validated, applied AND persisted -- one
// /etc write per call -- so a dragged slider would be a hundred of
// them. The level is applied on a short delay after the last movement
// (VOLUME_COMMIT_MS); the number under the pointer is the popup's own
// state until then, which is why drawing reads a local and not the
// setting.

#include <stdint.h>

// Whether the popup is open -- read by wm_render.c and wm_input.c.
extern int volume_open;

// Opens it, closing the Start menu, the calendar and the context menu:
// the four are mutually exclusive, as on every desktop.
void volume_open_now(void);
void volume_close(void);

// Registers the tray item. Called once from wm_run()'s setup, after
// tray_init() so the clock keeps slot 0.
void volume_tray_init(void);

// Refreshes the tray icon from the current level -- called once per
// frame beside tray_update_clock(). Cheap: a no-op unless the icon
// actually changes.
void volume_tray_update(void);

// Re-reads the settings if anything on the filesystem changed, and
// flushes a debounced level change once the pointer has settled. Called
// once per frame from wm.c, like calendar_poll_config().
void volume_poll_config(void);

// Draws the panel, using the live cursor for hover. A no-op when closed.
void volume_draw(int mx, int my);

// A left click at (mx, my). Returns 1 when wm_input.c should stop
// there. A dismissing click on the TASKBAR falls through, so the Start
// button acts on the same click that closed the popup -- the rule
// calendar_handle_click() already follows.
int volume_handle_click(int mx, int my);

// Live press tracking, every tick, so the slider can be DRAGGED --
// the same shape confirm_dialog_update_press() uses and for the same
// reason: a control that only sees the button-down edge cannot follow
// the pointer. Returns 1 when the level changed (i.e. redraw).
int volume_update_press(int mx, int my, uint8_t buttons);

// A wheel notch at (mx, my). Consumed -- and the volume stepped by
// VOLUME_STEP -- only when the pointer is over the tray item or over
// the open panel, which is where KDE, GNOME and Windows all take it.
// Returns 1 when consumed, so wm.c can fall through to the focused
// window otherwise.
int volume_handle_wheel(int mx, int my, int notches);

// The panel's live geometry, for the debug console (`gui volume`) and
// therefore for tests -- the SAME numbers drawing and hit-testing use.
// Valid whether or not the popup is open.
#define VOLUME_MAX_DEVICES 6

struct volume_geom {
    int x, y, w, h;                  // the panel
    int mute_x, mute_y, mute_w, mute_h;
    int slider_x, slider_y, slider_w, slider_h;
    int list_x, list_y, row_h;       // the device list; rows start at list_y
    int rows;                        // how many device rows, "auto" included
    int level;                       // 0..100, what the slider draws
    int muted;                       // 1 while the level is held at 0
    int selected_row;                // which row carries the tick
    int tray_x, tray_y, tray_w, tray_h;   // the item that opens this
};
void volume_geometry(struct volume_geom *out);

// One device row's value and label, for the debug console. `index` 0 is
// always "auto". Returns 0 past the last row.
int volume_row(int index, char *value, uint32_t value_size,
               char *label, uint32_t label_size);

#endif
