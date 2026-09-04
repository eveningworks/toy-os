#ifndef TRAY_SLIDER_POPUP_H
#define TRAY_SLIDER_POPUP_H

// One tray flyout that is a slider over a registered setting -- the
// shared half of volume_popup.c and brightness_popup.c, which were the
// same file twice: a panel anchored above its tray item, a `uui_scale`
// with an icon cell to its left and a "NN%" caption to its right, a
// debounced-then-committed setting write, the drag, the wheel, and the
// open/close/damage of an overlay-table row.
//
// The OWNER keeps what is only its own -- the volume flyout's mute
// toggle and device rows, the brightness flyout's `unavailable`
// sentence -- below the slider row, starting at `geom.below_y`, and
// it keeps its own `*_geometry()` as the one answer drawing,
// hit-testing and `gui <name> --json` all ask.
//
// THE WRITE IS DEBOUNCED, and that is the one thing to know before
// editing. A setting write validates, applies AND persists -- one /etc
// write per call -- so a dragged slider without the delay is a hundred
// filesystem writes. The level is written TRAY_SLIDER_COMMIT_MS after
// the last movement, and at once on release; until then `level` is the
// popup's own state, which is why drawing reads it and not the setting.
//
// A setting the registry answers `unavailable` for (brightness with no
// backlight) DISABLES the scale and every path writes nothing; the
// wheel is still consumed over the tray item, since the notch was
// aimed here.

#include <stdint.h>
#include "ui/uui_scale.h"
#include "setting_abi.h"

#define TRAY_SLIDER_COMMIT_MS 250

struct tray_slider_popup {
    // Set by the owner before tray_slider_init(). `name` is the
    // overlay-table row (wm_overlay.h), which is how opening this one
    // closes the others.
    const char *name;
    const char *setting;          // qualified: "system.volume"
    int step;                     // one wheel notch

    // Optional. `on_level` runs after the level changed by any path
    // (a drag, the wheel, a reload); `on_reload` after a generation
    // change re-read the setting; `damage` is the owner's overlay
    // damage op, and is what open/close/set_level call.
    void (*on_level)(void);
    void (*on_reload)(void);
    void (*damage)(void);

    // Owned here. `level` is clamped into [min, max] from the INFO
    // record; `unavailable` is empty when the setting can be written.
    int open;
    int tray_id;
    int level, min, max;
    char unavailable[SETTING_ABI_DESC_MAX];
    int pending;                  // a write is owed
    unsigned long long pending_at;
    uint32_t seen_generation;
    struct uui_scale scale;       // placed by tray_slider_geometry()
};

// The slider row's geometry -- the SAME numbers drawing, hit-testing
// and the debug console use. Valid whether or not the popup is open.
struct tray_slider_geom {
    int x, y, w, h;                     // the panel
    int icon_x, icon_y, icon_w, icon_h; // the square left of the track
    int slider_x, slider_y, slider_w, slider_h;   // the scale's rect
    int tray_x, tray_y, tray_w, tray_h; // the item that opens this
    int pad, row_h;
    int below_y;                        // where the owner's rows start
};

// Registers the tray item and reads the setting once.
void tray_slider_init(struct tray_slider_popup *p, const char *icon);

// Computes the panel: at least `want_w` wide (the owner's widest
// content), `extra_h` taller than the slider row, right-aligned to the
// tray item and placed by wm_popup_place(). Sets the scale's rect as
// a side effect, which is what lets every other call take `g`.
void tray_slider_geometry(struct tray_slider_popup *p, int want_w, int extra_h,
                          struct tray_slider_geom *g);

// The overlay-table verbs. Open closes every other dismissable overlay.
void tray_slider_open(struct tray_slider_popup *p);
void tray_slider_close(struct tray_slider_popup *p);
void tray_slider_damage(const struct tray_slider_geom *g);

// Once per frame: flushes a settled write, or re-reads the setting when
// something else changed one.
void tray_slider_poll(struct tray_slider_popup *p);

// Hover tokens for the overlay registry; the owner's own controls start
// at TRAY_SLIDER_HOVER_OWNER. Also drives the scale's thumb highlight.
#define TRAY_SLIDER_HOVER_NONE  0
#define TRAY_SLIDER_HOVER_TRACK 1
#define TRAY_SLIDER_HOVER_ICON  2
#define TRAY_SLIDER_HOVER_OWNER 3
int tray_slider_hover_at(struct tray_slider_popup *p, const struct tray_slider_geom *g,
                         int mx, int my);

// What a left click meant. DISMISSED is a click outside an open panel:
// the popup has closed, and the owner returns `my < screen_h -
// taskbar_h` so a taskbar click falls through (the rule every popup
// here follows). ICON and INSIDE are the owner's to act on.
enum tray_slider_click {
    TRAY_SLIDER_CLICK_NONE,       // closed, and not on the tray item
    TRAY_SLIDER_CLICK_OPENED,
    TRAY_SLIDER_CLICK_DISMISSED,
    TRAY_SLIDER_CLICK_TRACK,      // pressed the scale; a drag has begun
    TRAY_SLIDER_CLICK_ICON,
    TRAY_SLIDER_CLICK_INSIDE,
};
enum tray_slider_click tray_slider_click(struct tray_slider_popup *p,
                                         const struct tray_slider_geom *g,
                                         int mx, int my);

// Live press tracking, every tick, so the scale can be DRAGGED; the
// release commits at once rather than waiting out the debounce.
void tray_slider_update_press(struct tray_slider_popup *p, int mx, uint8_t buttons);

// A wheel notch. Consumed only over the tray item or the open panel --
// anywhere else it would eat every scroll in every app.
int tray_slider_wheel(struct tray_slider_popup *p, const struct tray_slider_geom *g,
                      int mx, int my, int notches);

// Applies a level to the popup and owes the write; `commit_now` skips
// the debounce (mute, release). Clamped; a no-op when unavailable.
void tray_slider_set_level(struct tray_slider_popup *p, int level, int commit_now);

// The panel background, the icon cell (hover-washed when `icon_hot`),
// the scale, the caption and the border. The owner draws its rows
// after, inside the panel.
void tray_slider_draw(const struct tray_slider_popup *p, const struct tray_slider_geom *g,
                      const char *icon, int icon_hot);

#endif
