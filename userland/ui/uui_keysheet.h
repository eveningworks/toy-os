#ifndef UUI_KEYSHEET_H
#define UUI_KEYSHEET_H

// keysheet -- an app's keyboard shortcuts as a sheet: groups of rows,
// each an action on the left and its keys on the right, drawn as caps.
// GtkShortcutsWindow's shape, and KDE's and Windows' shortcut pages'.
//
// **THE KEYS ARE ONE STRING, written the way a page writes them**:
// "[Alt]+[Enter]", "hold [Shift]", "[,] [.]". A bracketed word is a key
// cap; anything outside brackets is plain, dimmer text. [Up], [Down],
// [Left] and [Right] draw as arrows -- the font is ASCII-only, and a
// cap reading "Left" is not what the key says on it.
//
// GROUPS FILL A GRID ROW BY ROW, `columns` across, each grid row as tall
// as its tallest group. The data is the caller's and is not copied.
#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_widget.h"

struct uui_keysheet_row {
    const char *action;
    const char *keys;
};

struct uui_keysheet_group {
    const char *title;
    const struct uui_keysheet_row *rows;
    int count;
};

struct uui_keysheet {
    int x, y, w, h;
    const struct uui_keysheet_group *groups;
    int group_count;
    int columns;            // at least 1
    uint32_t bg;            // what it is drawn on; UUI_COLOR_UNSET = the panel
};

void uui_keysheet_init(struct uui_keysheet *k, const struct uui_keysheet_group *groups,
                       int group_count, int columns);
// From the font and the content: every row of every group at its full
// width, so nothing is ever elided at the natural size.
void uui_keysheet_natural_size(const struct uui_keysheet *k, int *out_w, int *out_h);
void uui_keysheet_set_geometry(struct uui_keysheet *k, int x, int y, int w, int h);
void uui_keysheet_draw(struct ugfx_surface *s, const struct uui_keysheet *k);

// The width `keys` draws at -- for a caller laying a row out by hand.
int uui_keysheet_keys_width(const char *keys);

extern const struct uui_widget_ops uui_keysheet_ops;

#endif
