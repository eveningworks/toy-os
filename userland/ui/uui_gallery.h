#ifndef UUI_GALLERY_H
#define UUI_GALLERY_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_widget.h"

// uui_gallery -- one choice out of a few, each shown as a CARD: a picture
// the caller paints and a label under it. KDE's Cursors and Global Theme
// pages, Windows 11's Themes grid. A radio list whose options are seen
// rather than read.
//
// THE PICTURE IS A CALLBACK, NOT AN IMAGE: `draw_tile` paints card `i`'s
// tile into the given rect. What a choice looks like is the caller's
// knowledge -- five cursors on a split backdrop, a wallpaper thumbnail --
// and a list of pre-made images would make the caller render and own
// them anyway, at a size only the widget knows.
//
// Behaves as a radio group: one selected; Left/Right step, Up/Down step a
// row, Home/End the ends; a press arms and the RELEASE over the same card
// commits (dragged off, it puts the selection back). The current card is
// the soft accent selection (docs/gui-guidelines.md), hover a quieter grey.

struct uui_gallery {
    int x, y, w, h;              // content-relative
    const char *const *labels;   // caller-owned, `count` of them
    int count;
    int selected;
    int hovered;                 // OWNED, from motion
    int focused;                 // OWNED, from the focus ring
    // OWNED: draw the focus ring. Set by Tab or an arrow key, cleared by a
    // click -- Windows' focus-visible rule: a pointer user sees the soft
    // selection alone, a keyboard user also sees where the keys will act.
    int ring;
    int disabled;
    int armed_prev;              // the selection before the press in flight, -1 none
    int min_w;                   // a card's least width, px; 0 for the default (13 lines)

    // The tile's painter, and what it is handed back. NULL draws no picture.
    void (*draw_tile)(struct ugfx_surface *s, int index, int x, int y, int w, int h,
                      void *ctx);
    void *ctx;
};

void uui_gallery_init(struct uui_gallery *g, const char *const *labels, int count,
                      int selected);
// Columns at the current width: as many minimum-width cards as fit, 3
// before any width is known.
int uui_gallery_cols(const struct uui_gallery *g);
// Card `i`'s rect and its tile's, content-relative; 0 when out of range.
int uui_gallery_card_rect(const struct uui_gallery *g, int i, int *x, int *y, int *w, int *h);
int uui_gallery_tile_rect(const struct uui_gallery *g, int i, int *x, int *y, int *w, int *h);
// The card under a point, or -1.
int uui_gallery_hit(const struct uui_gallery *g, int cx, int cy);
// 1 when the key moved the selection.
int uui_gallery_key(struct uui_gallery *g, int key);

// Routed: a stacked FILL_W control in a uui_setting_row, or any layout.
// `describe` reports `card i` and `tile i` and `selected`.
extern const struct uui_widget_ops uui_gallery_ops;

#endif
