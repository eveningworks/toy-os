#ifndef WM_FLYOUT_H
#define WM_FLYOUT_H

#include <stdint.h>

// THE TRAY FLYOUT CARD, shared by every flyout the panel opens --
// network, volume, brightness so far (the mockups' "N1" language,
// docs/gui-guidelines.md): a rounded card on the menus' ground, a header
// with a round badge, captioned sections between hairlines, and a
// footer band with buttons. Plasma's applet popups and Windows 11's
// flyouts are both one card with these parts.
//
// ONLY THE DRAWING IS SHARED. Each flyout keeps its own *_geometry() as
// the one answer drawing, hit-testing and `gui <name> --json` ask, built
// from the metrics below so every card has the same rhythm.
//
// THE CARD IS GLASS when menus are (wm_glass.h's WM_GLASS_MENU), and its
// text at rest blends against it: draw labels with wm_flyout_ink().
// Every flyout rect must also be in wm_glass_frosted_rects() while open.

struct wm_flyout_metrics {
    int pad;        // the card's side margin
    int vpad;       // a section's top and bottom margin
    int radius;     // the card's corners (the popup radius)
    int badge;      // the header's round badge, a diameter
    int hero_h;     // the header row
    int cap_h;      // a section caption ("DETAILS")
    int row_h;      // a key/value or radio row
    int btn_h;      // a footer button
    int foot_h;     // the footer band
    int sw_w, sw_h; // the on/off switch
};

void wm_flyout_metrics(struct wm_flyout_metrics *m);

// Colours the card is drawn in; the ink a label at rest passes as `bg`.
uint32_t wm_flyout_ground(void);
uint32_t wm_flyout_ink(void);       // UGFX_TRANSPARENT: blend against what is drawn
uint32_t wm_flyout_dim(void);       // a key, a caption, a subtitle

// The card: shadow (hollow on glass), ground, a footer band `foot_h`
// tall at the bottom (0 for none), the hairline round it all.
void wm_flyout_card(int x, int y, int w, int h, int foot_h);

// The header: a round badge in `badge` with `icon` (an icon_cache name)
// tinted white, then the title in bold and the subtitle under it, both
// clipped to end at `x + w`.
void wm_flyout_hero(int x, int y, int w, uint32_t badge, const char *icon,
                    const char *title, const char *sub);

// A hairline across the card at y, full width (x, w are the card's).
void wm_flyout_rule(int x, int y, int w);
// A section caption, clipped to `w`.
void wm_flyout_caption(int x, int y, int w, const char *text);
// A key in the dim ink, its value in the text ink, `key_w` apart.
void wm_flyout_kv(int x, int y, int w, int key_w, const char *key, const char *val);

// A footer button: `outlined` is a white face with a hairline (the
// primary), otherwise only a hover wash; `off` greys it and drops the
// hover. Returns its width; wm_flyout_button_w() measures without
// drawing. `icon` may be NULL.
int wm_flyout_button_w(const char *label, const char *icon);
int wm_flyout_button(int x, int y, const char *label, const char *icon,
                     int outlined, int hot, int off);

// An on/off switch (the accent when on), and a square icon button.
void wm_flyout_switch(int x, int y, int on, int hot);
void wm_flyout_icon_button(int x, int y, int size, const char *icon, int hot);

// A choice row: a radio, the label, and a dim note right-aligned; the
// selected one in the soft accent with its edge (docs/gui-guidelines.md).
void wm_flyout_radio_row(int x, int y, int w, int h, int selected, int hot,
                         const char *label, const char *note);

#endif // WM_FLYOUT_H
