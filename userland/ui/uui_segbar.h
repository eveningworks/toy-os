#ifndef UUI_SEGBAR_H
#define UUI_SEGBAR_H

// uui_segbar -- a WHOLE split into SEGMENTS side by side, each as wide
// as its share and one selected: a disk's partitions and its free
// space (Disks), the way GNOME Disks' volume map and Windows' Disk
// Management strip draw one. A segment is a rounded card: a title, an
// optional corner note (where it is mounted), an optional usage meter,
// and a detail line.
//
// WIDTH IS PROPORTIONAL, WITH A FLOOR: every segment gets `min_w` first
// and the rest is shared by size, so a 1 MB boot partition beside a
// 100 GB one is still a card that can be read and clicked. When even
// the floors do not fit, the segments share the width equally.
//
// A click selects (committed on the release, over the same segment);
// Left/Right/Home/End move the selection. The app reads `selected` in
// on_widget.
#include <stdint.h>
#include "ui/uui_widget.h"

#define UUI_SEGBAR_MAX 16

struct uui_segbar_seg {
    const char *title;    // "toyos", "Free space"
    const char *corner;   // top right ("/boot"), or NULL
    const char *detail;   // bottom line ("119.1 GB tfs3"), or NULL
    uint64_t size;        // any unit, the same for every segment
    int used_pct;         // 0-100 draws a meter; -1 none
    uint32_t fill, edge;  // the card's colours
    int empty;            // unallocated: a dashed edge, no fill
};

struct uui_segbar {
    int x, y, w, h;
    const struct uui_segbar_seg *segs;   // caller-owned
    int count;
    int selected;                        // -1 for none
    int min_w;                           // the floor; 0 = font-derived
    // OWNED
    int hot, armed, focused;
    int sx[UUI_SEGBAR_MAX], sw[UUI_SEGBAR_MAX];
};

void uui_segbar_init(struct uui_segbar *b);
// Points it at `segs` (at most UUI_SEGBAR_MAX) and keeps the selection
// when it is still in range.
void uui_segbar_set(struct uui_segbar *b, const struct uui_segbar_seg *segs, int count);
// Where segment `i` was placed; 0 if there is none.
int  uui_segbar_rect(const struct uui_segbar *b, int i, int *x, int *y, int *w, int *h);

extern const struct uui_widget_ops uui_segbar_ops;

#endif
