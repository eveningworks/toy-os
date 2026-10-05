#ifndef UUI_KEYMAP_H
#define UUI_KEYMAP_H

// keymap -- a picture of a keyboard layout: the 105-key ISO board's four
// typing rows, each cap showing what its key types. Base at the bottom
// left, Shift above it, AltGr at the bottom right in the accent colour;
// a dead key's cap has a dashed accent outline. The preview KDE's and
// GNOME's layout pickers show.
//
// The layout is a struct ukeymap (lib/ukeymap.h) the CALLER owns and
// keeps alive; NULL draws the caps empty. The board scales DOWN to the
// width it is given, never up past its natural size.
#include <stdint.h>
#include "lib/ukeymap.h"
#include "ui/ugfx.h"
#include "ui/uui_widget.h"

struct uui_keymap {
    int x, y, w, h;
    const struct ukeymap *map;
    uint32_t bg;            // what it is drawn on; UUI_COLOR_UNSET = the panel
};

void uui_keymap_init(struct uui_keymap *k, const struct ukeymap *map);
// From the font: a cap two text lines tall, the board fifteen-odd caps wide.
void uui_keymap_natural_size(const struct uui_keymap *k, int *out_w, int *out_h);
void uui_keymap_set_geometry(struct uui_keymap *k, int x, int y, int w, int h);
void uui_keymap_draw(struct ugfx_surface *s, const struct uui_keymap *k);

extern const struct uui_widget_ops uui_keymap_ops;

#endif
