#ifndef UUI_BUTTON_GROUP_H
#define UUI_BUTTON_GROUP_H

#include <stdint.h>
#include "ui/uui_button.h"

// Split out of the single uwidgets.c/.h this used to be, one file per
// widget -- the same shape as apps/ui/, so a widget's kernel-side and
// ring-3 versions live at matching paths. See ui/uui.h.

struct uui_button_group {
    struct uui_button *buttons; // not owned -- caller's array
    int count;

    // The code of the button that last COMMITTED, held until the app
    // collects it with uui_button_group_take_activated(). Needed once
    // input is routed: the router reports which WIDGET changed, and a
    // group is one widget with many buttons. 0 = nothing pending.
    int activated;
};

void uui_button_group_init(struct uui_button_group *g,
                            struct uui_button *buttons, int count);

// The box every button in the group currently occupies -- the union of
// their rects. 0x0 for an empty group.
void uui_button_group_natural_size(const struct uui_button_group *g,
                                    int *out_w, int *out_h);

void uui_button_group_draw(const struct uui_button_group *g,
                            struct ugfx_surface *s);

// Re-hit-tests and updates every button's `pressed` flag. Returns 1 if
// which button is hot changed (so the caller knows to repaint). Dragging
// off one button onto another re-presses correctly because this
// re-tests every button every time rather than remembering one.
int uui_button_group_press(struct uui_button_group *g, int cx, int cy);

// Same for `hovered`, with no button held. Pass (-1, -1) when the
// cursor leaves -- nothing is hit there, so the highlight clears with
// no special case.
int uui_button_group_hover(struct uui_button_group *g, int cx, int cy);

// Clears whichever button was pressed and returns its `code`, or -1 if
// none was. **This is how a button commits.** A press dragged off its
// button already had `pressed` cleared by uui_button_group_press(), so
// it returns -1 here and the action correctly does not happen.
int uui_button_group_release(struct uui_button_group *g);

// Collects the code of a button that committed (0 if none), clearing
// it. The routed path records rather than returns, since the router
// reports widgets rather than decisions.
int uui_button_group_take_activated(struct uui_button_group *g);

// Full table with ROUTED POINTER INPUT (ui/uui_route.h). Arms on press,
// commits on release, and a press dragged off commits nothing -- the
// rule docs/gui-guidelines.md exists for, implemented once here instead
// of in every app.
struct uui_widget_ops;
extern const struct uui_widget_ops uui_button_group_ops;

#endif
