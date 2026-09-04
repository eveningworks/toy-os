#ifndef UUI_STATUSBAR_H
#define UUI_STATUSBAR_H

#include <stdint.h>
#include "ui/ugfx.h"

// uui_statusbar -- the strip along the bottom of an application window,
// as Windows (the common control, `msctls_statusbar32`) and KDE
// (KStatusBar / QStatusBar) both have it.
//
// The shape both of those settled on, and the reason this is panes
// rather than one string: a status bar carries a MESSAGE that stretches
// and INDICATORS that do not. "saved" wants whatever width is left;
// "Ln 12, Col 5" wants exactly its own width and wants to stay put on
// the right, because an indicator that slides around as the message
// changes is unreadable at a glance. Win32 spells that SB_SETPARTS with
// a -1 last part; Qt spells it addWidget vs. addPermanentWidget. Same
// idea, and this is the smallest form of it.
//
// FLAT, per docs/gui-guidelines.md. Real toolkits draw sunken bevels
// between parts; this GUI does not bevel anything, so the parts are
// separated by a hairline and the strip by a hairline along its top.
//
// The text is the APP's -- pointers to its own buffers, never copied.
// So updating the status is assigning a string, and there is no
// set/get pair to keep in sync.

#define UUI_STATUSBAR_MAX_PANES 4

struct uui_status_pane {
    const char *text; // app-owned; NULL draws an empty pane

    // 0 = stretch: share whatever the fixed panes leave over.
    // > 0 = exactly this many characters wide, at the current font.
    // Character counts rather than pixels, because a pixel width is
    // right at exactly one font size (docs/gui-guidelines.md).
    int chars;
};

struct uui_statusbar {
    int x, y, w, h;
    struct uui_status_pane panes[UUI_STATUSBAR_MAX_PANES];
    int count;
    uint32_t bg, fg, border;
};

// One stretching pane, empty. An app fills in `panes[]` and `count`.
void uui_statusbar_init(struct uui_statusbar *sb);

void uui_statusbar_set_geometry(struct uui_statusbar *sb, int x, int y, int w, int h);

// Height is font-derived and real; width has NO preference (0), since a
// status bar takes whatever its window is wide -- see uui_primitives.h
// on what 0 means and why a caller must handle it.
void uui_statusbar_natural_size(const struct uui_statusbar *sb, int *out_w, int *out_h);

// Its height alone -- what an app laying out by hand asks, and ten
// of them wrapped natural_size() to answer it.
static inline int uui_statusbar_height(const struct uui_statusbar *sb) {
    int h = 0;
    uui_statusbar_natural_size(sb, 0, &h);
    return h;
}

void uui_statusbar_draw(struct ugfx_surface *s, const struct uui_statusbar *sb);

// Where pane `index` ended up. For an app reporting its own layout to a
// test (docs/gui-guidelines.md: a GUI test asks the app where things
// are). 0 if there is no such pane.
int uui_statusbar_pane_rect(const struct uui_statusbar *sb, int index,
                             int *x, int *y, int *w, int *h);

// Declarable in a layout (ui/uui_widget.h), so the layout reserves the
// bar's height instead of each app subtracting it from the content rect
// by hand. Draw-only: a status bar reports, it is not a control, and a
// `hit` slot would make it swallow clicks.
struct uui_widget_ops;
extern const struct uui_widget_ops uui_statusbar_ops;

#endif
