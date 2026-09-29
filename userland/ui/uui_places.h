#ifndef UUI_PLACES_H
#define UUI_PLACES_H

#include <stdint.h>
#include "ui/ugfx.h"

// PLACES AND DEVICES: the list down the left of a file window -- named
// folders a person goes to (Home, Documents, ...), then every mounted
// filesystem with how full it is. Explorer's navigation pane, Dolphin's
// Places panel and GTK's sidebar are this list, and a file manager and a
// file chooser that each drew their own would disagree about what
// "Documents" is. The File Manager and uui_filedialog both use it.
//
// **A DEVICE IS A MOUNT, READ FROM THE KERNEL** (QUERY_FSINFO, what `df`
// prints), so a newly mounted disk appears on the next
// uui_places_refresh() with nothing registered. Its name comes from
// where it is mounted: "/" is "System", "/boot" is "Boot", "/tmp" is
// "Temporary", anything else its last component.
//
// Like the path bar it navigates nothing: a click parks the row's path
// for uui_places_take(), and the app goes there.

#define UUI_PLACES_MAX       16
#define UUI_PLACES_PATH_MAX  64
#define UUI_PLACES_LABEL_MAX 24

struct uui_place {
    char label[UUI_PLACES_LABEL_MAX];
    const char *icon;             // icon_get() name
    char path[UUI_PLACES_PATH_MAX];
    int device;                   // 1 for a mount, with the fields below
    uint64_t total, used;
    int readonly, persistent;
};

struct uui_places {
    int x, y, w, h;
    struct uui_place row[UUI_PLACES_MAX];
    int count;
    int places;                   // how many of `row` are places; devices follow
    int selected;                 // row, or -1: the one whose path is where you are
    int hot, armed;               // OWNED
    char taken[UUI_PLACES_PATH_MAX];
    int has_taken;
    int focused;                  // OWNED -- the focus ring's set_focused
    int scroll;                   // PIXELS scrolled off the top, when it overflows; OWNED
    uint32_t bg;                  // 0 = derived from the theme at draw
};

void uui_places_init(struct uui_places *p);
// A named folder. Call before uui_places_refresh(), which appends the
// devices after the places.
void uui_places_add(struct uui_places *p, const char *label, const char *icon,
                    const char *path);
// Re-reads the mounts and their free space. Cheap: one query per mount.
void uui_places_refresh(struct uui_places *p);
// Highlights the row that IS this directory, or none -- a folder under
// Home is not Home, which is why this is an exact match.
void uui_places_select_path(struct uui_places *p, const char *dir);
// The path a click (or Enter) asked for: 1, or 0 when none since the
// last call.
int  uui_places_take(struct uui_places *p, char *out, int cap);
// Row `i`'s rect, for tests and layout logs; 0 past the end.
int  uui_places_row_rect(const struct uui_places *p, int i, int *x, int *y, int *w, int *h);

struct uui_widget_ops;
extern const struct uui_widget_ops uui_places_ops;

#endif
