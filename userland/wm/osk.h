#ifndef OSK_H
#define OSK_H

#include <stdint.h>

// The on-screen keyboard: a panel of keycaps above the taskbar that
// types into the focused client. An overlay rather than an app, which
// is GNOME's shape (its OSK is part of the shell) rather than Windows'
// osk.exe or KDE's maliit. Two things follow from that and neither is
// incidental: an overlay is not a window, so clicking a keycap cannot
// take focus away from the window being typed into; and the keystroke
// is a direct wm_client_send_key() call, so no ring-3 program gains the
// ability to type into another one -- the hole X11's XTEST leaves open
// and Wayland's virtual-keyboard-v1 was written to close.
//
// IT ENCODES KEYS THE WAY THE PHYSICAL KEYBOARD DOES, and that is the
// part to preserve. Ctrl-C is the control code 0x03, not 'c' with a
// modifier bit; Alt-B is ESC then 'b'. See api/keyboard.h's "Ctrl and
// Alt" comment -- an OSK that sent the modifier bits instead would
// leave Ctrl-C doing nothing in the Terminal, which is most of what a
// machine with no keyboard needs it for.
//
// Its overlay row has NO `close` op, which is what stops another popup
// dismissing it (wm_overlay.h): a keyboard has to survive the click
// that puts the caret in the text field it is typing into.
extern int osk_open;

// Registers the tray item. Call once from wm_run()'s setup, beside
// tray_init().
void osk_init(void);

// What `gui osk` reports. A test asks for a keycap BY LABEL rather than
// working out where it is: the panel is font-derived, so a test that
// computed a cap's centre would be a second layout implementation and
// would drift from this one (docs/conventions/gui.md).
struct osk_report {
    int x, y, w, h;                      // the panel
    int tray_x, tray_y, tray_w, tray_h;  // the item that toggles it
    unsigned mods;                       // armed sticky modifiers
    int docked;                          // full width, or floating
    int bar_h;                           // the top bar it is dragged by
    int dock_cx, dock_cy, close_cx, close_cy;   // the bar's two buttons
};
void osk_report(struct osk_report *r);

// One keycap's box, found by its label ("a", "Enter", "Ctrl"). 0 when
// there is no such cap. Valid whether or not the panel is open.
int osk_key_box(const char *cap, int *x, int *y, int *w, int *h);

// The overlay registry's ops -- see wm_overlay.h.
void osk_draw(int mx, int my);
int  osk_handle_click(int mx, int my);
int  osk_hover_at(int mx, int my);
void osk_damage(void);
// Where it is, for wm_overlay.h's automatic damage. 0 when it has
// no rect to report.
int osk_rect(int *x, int *y, int *w, int *h);
void osk_update_press(int mx, int my, uint8_t buttons);

#endif
