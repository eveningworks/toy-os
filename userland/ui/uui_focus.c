// focus. Split out of uwidgets.c -- see ui/uui_focus.h.
#include "ui/uui_focus.h"
#include "keyboard.h" // KEY_* codes, as delivered by WIN_EV_KEY

// ---------------------------------------------------------------------
// focus ring
// ---------------------------------------------------------------------

void uui_focus_init(struct uui_focus *f, struct uui_focusable *items, int count) {
    f->items = items;
    f->count = count;
    f->current = -1;
}

void uui_focus_set(struct uui_focus *f, int index) {
    if (f->current >= 0 && f->current < f->count) {
        const struct uui_focusable *it = &f->items[f->current];
        if (it->ops->set_focused) it->ops->set_focused(it->widget, 0);
    }
    f->current = (index >= 0 && index < f->count) ? index : -1;
    if (f->current >= 0) {
        const struct uui_focusable *it = &f->items[f->current];
        if (it->ops->set_focused) it->ops->set_focused(it->widget, 1);
    }
}

int uui_focus_next(struct uui_focus *f) {
    if (f->count <= 0) return 0;
    uui_focus_set(f, (f->current + 1) % f->count);
    return 1;
}

int uui_focus_prev(struct uui_focus *f) {
    if (f->count <= 0) return 0;
    uui_focus_set(f, (f->current - 1 + f->count) % f->count);
    return 1;
}

int uui_focus_key(struct uui_focus *f, int key, unsigned mods) {
    if (key == '\t') return (mods & KEY_MOD_SHIFT) ? uui_focus_prev(f) : uui_focus_next(f);
    if (f->current < 0 || f->current >= f->count) return 0;
    const struct uui_focusable *it = &f->items[f->current];
    // mods forwarded now that this shares uui_widget_ops with layout --
    // a widget that cares about Shift can see it rather than having the
    // focus ring silently swallow the distinction.
    return it->ops->key ? it->ops->key(it->widget, key, mods) : 0;
}

int uui_focus_click(struct uui_focus *f, int cx, int cy) {
    for (int i = 0; i < f->count; i++) {
        const struct uui_focusable *it = &f->items[i];
        if (it->ops->hit && it->ops->hit(it->widget, cx, cy)) {
            if (i == f->current) return 0;
            uui_focus_set(f, i);
            return 1;
        }
    }
    return 0;
}
