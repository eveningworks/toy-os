#ifndef UUI_KEYMAP_H
#define UUI_KEYMAP_H
// keymap -- a keyboard drawn from a layout, two ways:
//
// - THE PICTURE (Settings' layout preview, KDE's and GNOME's pickers):
//   the 105-key ISO board's four typing rows, each cap showing what its
//   key types -- base at the bottom left, Shift above it, AltGr at the
//   bottom right in the accent colour. Scales DOWN to the width it is
//   given, never up past its natural size.
// - THE TYPING BOARD (the on-screen keyboard): the same ISO rows plus
//   Esc, Del, AltGr and the arrows, filling the rect it is given. Each
//   cap shows what it types NOW under `levels`, with the key's AltGr
//   character as a small accent hint while AltGr is not held. Keys are
//   hit-tested here, by the same walk that draws them.
//
// On both, a dead key's cap has a dashed accent outline. The layout is a
// struct ukeymap (lib/ukeymap.h) the CALLER owns and keeps alive; NULL
// draws the caps empty.
#include <stdint.h>
#include "lib/ukeymap.h"
#include "ui/ugfx.h"
#include "ui/uui_widget.h"

enum uui_keymap_board { UUI_KEYMAP_PICTURE, UUI_KEYMAP_TYPING };

struct uui_keymap {
    int x, y, w, h;
    const struct ukeymap *map;
    uint32_t bg;            // what it is drawn on; UUI_COLOR_UNSET = the panel
    int board;              // enum uui_keymap_board

    // The typing board's state, all owned by the caller.
    unsigned levels;        // KEY_MOD_SHIFT / KEY_MOD_ALTGR: what the caps show
    unsigned armed;         // KEY_MOD_* bits: those modifier caps drawn selected
    int pending_kc;         // a dead key waiting for its letter, or 0
    int hover, pressed;     // key indices, or -1
};

// One key of the board: a character key has a keycode; a function key
// has a label and either a code it types (`action`: an ASCII control
// character or an api/keyboard.h KEY_* code) or a sticky modifier.
struct uui_keymap_key {
    int kc;                 // evdev keycode, 0 for a function key
    int action;
    unsigned mod;           // KEY_MOD_* bit, 0 for none
    const char *label;      // 0 for a character key
};

void uui_keymap_init(struct uui_keymap *k, const struct ukeymap *map);
// From the font: a cap two text lines tall, the board fifteen-odd caps wide.
void uui_keymap_natural_size(const struct uui_keymap *k, int *out_w, int *out_h);
void uui_keymap_set_geometry(struct uui_keymap *k, int x, int y, int w, int h);
void uui_keymap_draw(struct ugfx_surface *s, const struct uui_keymap *k);

int uui_keymap_key_count(const struct uui_keymap *k);
// Key `i`; 0 when out of range.
int uui_keymap_key(const struct uui_keymap *k, int i, struct uui_keymap_key *out);
// Key `i`'s box, as drawn.
int uui_keymap_key_rect(const struct uui_keymap *k, int i, int *x, int *y, int *w, int *h);
// The key under (mx, my), or -1.
int uui_keymap_key_at(const struct uui_keymap *k, int mx, int my);
// A key by name, for a test: a function key's label ("Enter", "AltGr"),
// a character key's BASE character ("q", "/"), or "kc<decimal>" for any
// key at all (a key whose character is not ASCII). -1 when none.
int uui_keymap_find(const struct uui_keymap *k, const char *name);

extern const struct uui_widget_ops uui_keymap_ops;

#endif
