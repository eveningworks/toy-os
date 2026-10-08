#ifndef UUI_CLIP_H
#define UUI_CLIP_H

// uui_clip -- one child, drawn and clickable only inside a VIEWPORT the
// caller names: a panel placed in a scrolling pane that is not itself a
// uui_scrollview (Device Manager's uui_props), so it scrolls with the
// pane and is cut at its edges instead of disappearing whole. The
// scroll view's clipping, without the scrolling: the caller places the
// child wherever the pane puts it, in or out of view, and says where
// the view is.
#include "ui/uui_widget.h"

struct uui_clip {
    struct uui_item child;          // caller-owned widget, any ops
    int vx, vy, vw, vh;             // the viewport, window-relative
    int x, y, w, h;                 // the child's rect, as last placed
};

void uui_clip_init(struct uui_clip *c, struct uui_item child);
void uui_clip_set_viewport(struct uui_clip *c, int x, int y, int w, int h);

// Is any of the child in the viewport? An app hides the item when not.
int uui_clip_visible(const struct uui_clip *c);

extern const struct uui_widget_ops uui_clip_ops;

#endif
