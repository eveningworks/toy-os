#ifndef UUI_FOCUS_H
#define UUI_FOCUS_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"


// Split out of the single uwidgets.c/.h this used to be, one file per
// widget -- the same shape as apps/ui/, so a widget's kernel-side and
// ring-3 versions live at matching paths. See ui/uui.h.

// --- keyboard focus ring ----------------------------------------------
//
// ROUTE KEYS THROUGH uui_focus_key(), never by trying each widget in
// turn: the first widget tried swallows every key it recognises, which
// is how a listbox next to a dropdown became unreachable from the
// keyboard in the kernel version.

struct uui_focus_ops {
    int  (*key)(void *w, int key);
    int  (*hit)(const void *w, int cx, int cy);
    void (*set_focused)(void *w, int focused);
};

struct uui_focusable {
    void *widget;
    const struct uui_focus_ops *ops;
};

struct uui_focus {
    struct uui_focusable *items; // caller-owned; tab order = array order
    int count;
    int current; // or -1
};

void uui_focus_init(struct uui_focus *f, struct uui_focusable *items, int count);
void uui_focus_set(struct uui_focus *f, int index);
int  uui_focus_next(struct uui_focus *f);
int  uui_focus_prev(struct uui_focus *f);

// Handles Tab/Shift-Tab itself, then forwards to the focused widget.
// Returns 1 if consumed.
int  uui_focus_key(struct uui_focus *f, int key, unsigned mods);

// Moves focus to whatever was clicked. Returns 1 if focus changed.
int  uui_focus_click(struct uui_focus *f, int cx, int cy);

#endif
