// Pointer routing -- see ui/uui_route.h for why this exists and the two
// rules it implements.
#include "ui/uui_route.h"
#include "ui/uui_layout.h"
#include <stddef.h>

void uui_router_init(struct uui_router *r, struct uui_item *items, int count) {
    r->items = items;
    r->count = count;
    uui_router_reset(r);
}

void uui_router_reset(struct uui_router *r) {
    r->grab = NULL;
    r->grab_ops = NULL;
    r->grab_id = 0;
}

// A nested layout is routed by recursing into ITS items, so a container
// needs no input code of its own -- it is a placement device, not a
// widget with behaviour.
static struct uui_item *nested(struct uui_item *it, int *out_count) {
    if (it->ops != &uui_layout_ops) return NULL;
    struct uui_layout *l = (struct uui_layout *)it->widget;
    *out_count = l->count;
    return l->items;
}

// Deliver a press to one item; returns the consuming item, or NULL.
static struct uui_item *press_item(struct uui_item *it, int cx, int cy,
                                   int *changed) {
    int n = 0;
    struct uui_item *sub = nested(it, &n);
    if (sub) {
        // Back to front within the nested container too.
        for (int i = n - 1; i >= 0; i--) {
            struct uui_item *hit = press_item(&sub[i], cx, cy, changed);
            if (hit) return hit;
        }
        return NULL;
    }
    if (!it->ops || !it->ops->press) return NULL;
    // A widget with an active overlay is asked WITHOUT a hit test: its
    // popup is drawn outside its own rect (see uui_route.h).
    int overlay = it->ops->overlay_active && it->ops->overlay_active(it->widget);
    if (!overlay && it->ops->hit && !it->ops->hit(it->widget, cx, cy)) return NULL;
    if (!it->ops->press(it->widget, cx, cy)) return NULL;
    *changed = 1;
    return it;
}

// Any item claiming an overlay, searched depth-first. Offered every
// press first, whatever the coordinates.
static struct uui_item *overlay_owner(struct uui_item *items, int count) {
    for (int i = count - 1; i >= 0; i--) {
        int n = 0;
        struct uui_item *sub = nested(&items[i], &n);
        if (sub) {
            struct uui_item *o = overlay_owner(sub, n);
            if (o) return o;
            continue;
        }
        const struct uui_widget_ops *ops = items[i].ops;
        if (ops && ops->overlay_active && ops->overlay_active(items[i].widget)) {
            return &items[i];
        }
    }
    return NULL;
}

int uui_router_press(struct uui_router *r, int cx, int cy, int *out_changed) {
    int changed = 0;
    struct uui_item *taken = NULL;

    struct uui_item *ov = overlay_owner(r->items, r->count);
    if (ov && ov->ops->press && ov->ops->press(ov->widget, cx, cy)) {
        changed = 1;
        taken = ov;
    }

    if (!taken) {
        for (int i = r->count - 1; i >= 0 && !taken; i--) {
            taken = press_item(&r->items[i], cx, cy, &changed);
        }
    }

    if (taken) {
        r->grab = taken->widget;
        r->grab_ops = taken->ops;
        r->grab_id = taken->id;
    }
    if (out_changed) *out_changed = changed;
    return taken ? taken->id : 0;
}

int uui_router_motion(struct uui_router *r, int cx, int cy, unsigned buttons,
                       int *out_changed) {
    int changed = 0;
    int id = 0;

    // The GRAB: while a button is held, motion belongs to whoever took
    // the press, even far outside its rect. Without this a thumb drag
    // stops the moment the cursor leaves the scrollbar.
    if (r->grab && r->grab_ops && r->grab_ops->motion) {
        if (r->grab_ops->motion(r->grab, cx, cy, buttons)) changed = 1;
        id = r->grab_id;
        if (out_changed) *out_changed = changed;
        return id;
    }

    // Otherwise it is a hover: every widget hears it, because more than
    // one may need to CLEAR a highlight the cursor has left.
    for (int i = 0; i < r->count; i++) {
        int n = 0;
        struct uui_item *sub = nested(&r->items[i], &n);
        struct uui_item *list = sub ? sub : &r->items[i];
        int listn = sub ? n : 1;
        for (int j = 0; j < listn; j++) {
            const struct uui_widget_ops *ops = list[j].ops;
            if (!ops || !ops->motion) continue;
            if (ops->motion(list[j].widget, cx, cy, buttons)) {
                changed = 1;
                id = list[j].id;
            }
        }
    }
    if (out_changed) *out_changed = changed;
    return id;
}

int uui_router_release(struct uui_router *r, int cx, int cy, int *out_changed) {
    int changed = 0;
    int id = 0;
    if (r->grab && r->grab_ops && r->grab_ops->release) {
        if (r->grab_ops->release(r->grab, cx, cy)) changed = 1;
        id = r->grab_id;
    }
    // The grab always ends, even if the widget declined the release --
    // a grab that outlives the button is a widget that keeps scrolling
    // after you let go.
    uui_router_reset(r);
    if (out_changed) *out_changed = changed;
    return id;
}

static int wheel_item(struct uui_item *it, int cx, int cy, int notches,
                      int *changed, int *id) {
    int n = 0;
    struct uui_item *sub = nested(it, &n);
    if (sub) {
        for (int i = n - 1; i >= 0; i--) {
            if (wheel_item(&sub[i], cx, cy, notches, changed, id)) return 1;
        }
        return 0;
    }
    if (!it->ops || !it->ops->wheel) return 0;
    int overlay = it->ops->overlay_active && it->ops->overlay_active(it->widget);
    if (!overlay && it->ops->hit && !it->ops->hit(it->widget, cx, cy)) return 0;
    if (!it->ops->wheel(it->widget, notches)) return 0;
    *changed = 1;
    *id = it->id;
    return 1;
}

int uui_router_wheel(struct uui_router *r, int cx, int cy, int notches,
                      int *out_changed) {
    int changed = 0, id = 0;

    // Under the cursor, not down a fixed chain. An app that tried each
    // scrollable widget in turn scrolled whichever came first in its own
    // list, so the wheel over one control moved another.
    struct uui_item *ov = overlay_owner(r->items, r->count);
    if (ov && ov->ops->wheel && ov->ops->wheel(ov->widget, notches)) {
        changed = 1;
        id = ov->id;
    } else {
        for (int i = r->count - 1; i >= 0; i--) {
            if (wheel_item(&r->items[i], cx, cy, notches, &changed, &id)) break;
        }
    }
    if (out_changed) *out_changed = changed;
    return id;
}
