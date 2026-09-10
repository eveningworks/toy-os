// Pointer routing -- see ui/uui_route.h for why this exists and the two
// rules it implements.
#include "ui/uui_route.h"
#include "ui/uui_layout.h"
#include "keyboard.h"   // KEY_MOD_CTRL -- the drag's copy/move bit
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
    r->dragging = 0;
    r->drag.kind = UUI_DRAG_NONE;
    r->target = NULL;
    r->target_ops = NULL;
    r->target_id = 0;
    // NOT drop_id: it is set by the release that resets everything
    // else, and read by uapp right after.
}

// A nested layout is routed by recursing into ITS items, so a container
// needs no input code of its own -- it is a placement device, not a
// widget with behaviour.
static struct uui_item *nested(struct uui_item *it, int *out_count) {
    if (!it->ops || !it->ops->children) return NULL;
    return it->ops->children(it->widget, out_count);
}

// May the pointer reach this container's children at all? A container
// with a `hit` clips them to itself -- a scroll view's children are
// laid out past its edges and must not be clickable out there. One
// without (a plain layout) never clips, which is what it wants.
static int container_admits(struct uui_item *it, int cx, int cy) {
    if (!it->ops->hit) return 1;
    return it->ops->hit(it->widget, cx, cy);
}

// Deliver a press to one item; returns the consuming item, or NULL.
static struct uui_item *press_item(struct uui_item *it, int cx, int cy,
                                   unsigned mods, int *changed) {
    if (it->hidden) return NULL;   // hidden is hidden from the mouse too
    int n = 0;
    struct uui_item *sub = nested(it, &n);
    if (sub) {
        if (!container_admits(it, cx, cy)) return NULL;
        // Back to front within the nested container too.
        for (int i = n - 1; i >= 0; i--) {
            struct uui_item *hit = press_item(&sub[i], cx, cy, mods, changed);
            if (hit) return hit;
        }
        // No child took it -- fall through to the container's OWN press.
        // That is how a scroll view's scrollbar gets its clicks: it is
        // part of the container, not one of the children.
    }
    if (!it->ops || !it->ops->press) return NULL;
    // A widget with an active overlay is asked WITHOUT a hit test: its
    // popup is drawn outside its own rect (see uui_route.h).
    int overlay = it->ops->overlay_active && it->ops->overlay_active(it->widget);
    if (!overlay && it->ops->hit && !it->ops->hit(it->widget, cx, cy)) return NULL;
    if (!it->ops->press(it->widget, cx, cy, mods)) return NULL;
    *changed = 1;
    return it;
}

// Any item claiming an overlay, searched depth-first. Offered every
// press first, whatever the coordinates.
//
// A CONTAINER MAY BE THE OWNER: a modal dialog with a body is one, and
// it is asked before its children are searched, so the whole subtree is
// then routed through it (press_item/motion_item/wheel_item recurse).
static struct uui_item *overlay_owner(struct uui_item *items, int count) {
    for (int i = count - 1; i >= 0; i--) {
        if (items[i].hidden) continue;
        const struct uui_widget_ops *ops = items[i].ops;
        if (ops && ops->overlay_active && ops->overlay_active(items[i].widget)) {
            return &items[i];
        }
        int n = 0;
        struct uui_item *sub = nested(&items[i], &n);
        if (sub) {
            struct uui_item *o = overlay_owner(sub, n);
            if (o) return o;
        }
    }
    return NULL;
}

int uui_router_press(struct uui_router *r, int cx, int cy, unsigned mods,
                      int *out_changed) {
    int changed = 0;
    struct uui_item *taken = NULL;

    // Through press_item(), not the owner's press directly: an owner
    // with children (a dialog's body) offers them the press first and
    // its own press only takes what they declined.
    struct uui_item *ov = overlay_owner(r->items, r->count);
    if (ov) taken = press_item(ov, cx, cy, mods, &changed);

    if (!taken) {
        for (int i = r->count - 1; i >= 0 && !taken; i--) {
            taken = press_item(&r->items[i], cx, cy, mods, &changed);
        }
    }

    if (taken) {
        r->grab = taken->widget;
        r->grab_ops = taken->ops;
        r->grab_id = taken->id;
        r->press_x = cx;
        r->press_y = cy;
    }
    if (out_changed) *out_changed = changed;
    return taken ? taken->id : 0;
}

// UUI_NOWHERE (uui_route.h): a point no widget can contain. A widget
// the cursor has left still has to HEAR the move, or its highlight
// stays lit after the pointer has gone; this is what a subtree is told
// when the cursor is outside the container that clips it. uui_hit() is
// a half-open range test, so any coordinate this far negative misses
// everything without a widget needing to know about the convention.

// --- the drag session (rule 3) ----------------------------------------

// The deepest widget under (cx, cy) that declares drag_over, back to
// front through containers -- the same walk as cursor_item(). The grab
// holder is NOT excluded: a pane can be its own target (a file dropped
// on a sub-folder row of the pane it came from).
static struct uui_item *target_item(struct uui_item *it, int cx, int cy) {
    if (it->hidden) return NULL;
    int n = 0;
    struct uui_item *sub = nested(it, &n);
    if (sub) {
        if (!container_admits(it, cx, cy)) return NULL;
        for (int i = n - 1; i >= 0; i--) {
            struct uui_item *hit = target_item(&sub[i], cx, cy);
            if (hit) return hit;
        }
    }
    if (!it->ops || !it->ops->drag_over) return NULL;
    if (it->ops->hit && !it->ops->hit(it->widget, cx, cy)) return NULL;
    return it;
}

static struct uui_item *target_under(struct uui_router *r, int cx, int cy) {
    for (int i = r->count - 1; i >= 0; i--) {
        struct uui_item *hit = target_item(&r->items[i], cx, cy);
        if (hit) return hit;
    }
    return NULL;
}

// One motion while dragging: the target under the pointer is asked, the
// one being left is told so. Always a repaint -- the ghost moved.
static void drag_motion(struct uui_router *r, int cx, int cy, unsigned mods) {
    r->drag.x = cx;
    r->drag.y = cy;
    r->drag.mods = mods;
    r->drag.copy = (mods & KEY_MOD_CTRL) != 0;
    struct uui_item *t = target_under(r, cx, cy);
    void *tw = t ? t->widget : NULL;
    if (r->target && r->target != tw) {
        r->target_ops->drag_over(r->target, UUI_NOWHERE, UUI_NOWHERE, &r->drag);
    }
    r->target = tw;
    r->target_ops = t ? t->ops : NULL;
    r->target_id = t ? t->id : 0;
    r->drag.accepted = t ? t->ops->drag_over(tw, cx, cy, &r->drag) : 0;
}

int uui_router_drag_active(const struct uui_router *r) { return r->dragging; }

const struct uui_drag *uui_router_drag(const struct uui_router *r) {
    return r->dragging ? &r->drag : NULL;
}

const struct uui_drag *uui_router_dropped(const struct uui_router *r) {
    return &r->dropped;
}

int uui_router_take_drop(struct uui_router *r) {
    int id = r->drop_id;
    r->drop_id = 0;
    return id;
}

void uui_router_drag_cancel(struct uui_router *r) {
    if (!r->dragging) return;
    if (r->target && r->target_ops->drag_over)
        r->target_ops->drag_over(r->target, UUI_NOWHERE, UUI_NOWHERE, &r->drag);
    if (r->grab_ops && r->grab_ops->drag_end) r->grab_ops->drag_end(r->grab, 0);
    uui_router_reset(r);
}

// One item, and everything nested inside it. Every widget hears the
// move -- unlike a press, which stops at the first taker -- because
// more than one may need to CLEAR a highlight.
//
// THIS RECURSES, and for a long time it did not: the loop this replaced
// walked exactly ONE level of containers while press and wheel walked
// all of them, so a widget below two containers never received a move
// event at all. Nothing failed loudly. System Settings nests four deep
// (window -> body row -> scroll view -> page column), so every hover
// state on that page was dead -- including the one inside a dropdown's
// popup, which is where it was finally noticed.
static int motion_item(struct uui_item *it, int cx, int cy, unsigned buttons,
                       const struct uui_item *skip, int *id) {
    if (it->hidden) return 0;      // not drawn, so nothing to highlight
    if (it == skip) return 0;      // the overlay owner, already told
    int changed = 0;

    int n = 0;
    struct uui_item *sub = nested(it, &n);
    if (sub) {
        // Children of a container that CLIPS (one with a `hit`) are
        // told "nowhere" rather than skipped when the cursor is outside
        // it -- skipping would leave a row lit under a scroll view the
        // pointer has left, and passing the real point would light a
        // row that is scrolled out of sight.
        int ax = cx, ay = cy;
        if (!container_admits(it, cx, cy)) { ax = UUI_NOWHERE; ay = UUI_NOWHERE; }
        for (int i = 0; i < n; i++)
            if (motion_item(&sub[i], ax, ay, buttons, skip, id)) changed = 1;
    }

    if (it->ops && it->ops->motion && it->ops->motion(it->widget, cx, cy, buttons)) {
        changed = 1;
        *id = it->id;
    }
    return changed;
}

int uui_router_motion(struct uui_router *r, int cx, int cy, unsigned buttons,
                       unsigned mods, int *out_changed) {
    int changed = 0;
    int id = 0;

    // A DRAG IN PROGRESS: the target under the pointer hears it, not
    // the grab holder (rule 3).
    if (r->dragging) {
        drag_motion(r, cx, cy, mods);
        if (out_changed) *out_changed = 1;
        return r->grab_id;
    }

    // The GRAB: while a button is held, motion belongs to whoever took
    // the press, even far outside its rect. Without this a thumb drag
    // stops the moment the cursor leaves the scrollbar.
    if (r->grab && r->grab_ops) {
        // Past the threshold, a source is asked ONCE whether this is a
        // drag. Asked before its ordinary motion, so a band that would
        // otherwise start on the same motion never does.
        if ((buttons & 1) && r->grab_ops->drag_start &&
            r->drag.kind == UUI_DRAG_NONE) {
            int dx = cx - r->press_x, dy = cy - r->press_y;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dx >= UUI_DRAG_THRESHOLD || dy >= UUI_DRAG_THRESHOLD) {
                r->drag.source = r->grab;
                r->drag.source_id = r->grab_id;
                r->drag.accepted = 0;
                if (r->grab_ops->drag_start(r->grab, r->press_x, r->press_y, &r->drag)) {
                    r->dragging = 1;
                    drag_motion(r, cx, cy, mods);
                    if (out_changed) *out_changed = 1;
                    return r->grab_id;
                }
                // Declined: remembered by `kind` staying NONE... which
                // would ask again. Mark it asked.
                r->drag.kind = -1;
            }
        }
        if (r->grab_ops->motion && r->grab_ops->motion(r->grab, cx, cy, buttons))
            changed = 1;
        id = r->grab_id;
        if (out_changed) *out_changed = changed;
        return id;
    }

    // The overlay owner first, and with the REAL point: a dropdown's
    // popup is drawn outside its own rect and usually outside its
    // container's, so the clipping above would tell it "nowhere" and no
    // row in an open popup would ever highlight. Same precedence press
    // and wheel already give it -- and it is skipped in the walk below,
    // since hearing the move twice would light a row and clear it again.
    struct uui_item *ov = overlay_owner(r->items, r->count);
    if (ov && motion_item(ov, cx, cy, buttons, NULL, &id)) changed = 1;

    // AN OPEN POPUP OWNS THE POINTER: everyone else is told "nowhere",
    // never the real point. Press already stops at the overlay owner;
    // motion walking on with real coordinates lit up icons UNDER an
    // open menu -- visible past the popup's edge, and every desktop's
    // grab forbids it. "Nowhere" rather than skipping, so a highlight
    // lit before the popup opened still clears.
    int wx = ov ? UUI_NOWHERE : cx, wy = ov ? UUI_NOWHERE : cy;
    for (int i = 0; i < r->count; i++)
        if (motion_item(&r->items[i], wx, wy, buttons, ov, &id)) changed = 1;

    if (out_changed) *out_changed = changed;
    return id;
}

// The deepest declaring widget under (cx, cy) wins, back to front.
//
// NOT routed through the grab: a grab is about who gets told, the
// cursor is about what is under the pointer.
static int cursor_item(struct uui_item *it, int cx, int cy) {
    if (it->hidden) return WIN_CURSOR_DEFAULT;
    int n = 0;
    struct uui_item *sub = nested(it, &n);
    if (sub) {
        if (!container_admits(it, cx, cy)) return WIN_CURSOR_DEFAULT;
        for (int i = n - 1; i >= 0; i--) {
            int c = cursor_item(&sub[i], cx, cy);
            if (c != WIN_CURSOR_DEFAULT) return c;
        }
        // Fall through to the container's own answer, same as press.
    }
    if (!it->ops || !it->ops->cursor) return WIN_CURSOR_DEFAULT;
    if (it->ops->hit && !it->ops->hit(it->widget, cx, cy)) return WIN_CURSOR_DEFAULT;
    return it->ops->cursor(it->widget, cx, cy);
}

int uui_router_cursor(const struct uui_router *r, int cx, int cy) {
    // An open popup answers first: its rows are outside its own `hit`,
    // so the walk below would never reach them.
    struct uui_item *ov = overlay_owner(r->items, r->count);
    if (ov) {
        int n = 0;
        struct uui_item *sub = nested(ov, &n);
        if (sub && container_admits(ov, cx, cy)) {
            for (int i = n - 1; i >= 0; i--) {
                int c = cursor_item(&sub[i], cx, cy);
                if (c != WIN_CURSOR_DEFAULT) return c;
            }
        }
        if (ov->ops->cursor) {
            int c = ov->ops->cursor(ov->widget, cx, cy);
            if (c != WIN_CURSOR_DEFAULT) return c;
        }
    }
    for (int i = r->count - 1; i >= 0; i--) {
        int c = cursor_item(&r->items[i], cx, cy);
        if (c != WIN_CURSOR_DEFAULT) return c;
    }
    return WIN_CURSOR_DEFAULT;
}

static int id_of_item(struct uui_item *it, const void *widget) {
    if (it->widget == widget) return it->id;
    int n = 0;
    struct uui_item *sub = nested(it, &n);
    for (int i = 0; i < n; i++) {
        int id = id_of_item(&sub[i], widget);
        if (id) return id;
    }
    return 0;
}

int uui_router_id_of(const struct uui_router *r, const void *widget) {
    if (!widget) return 0;
    for (int i = 0; i < r->count; i++) {
        int id = id_of_item(&r->items[i], widget);
        if (id) return id;
    }
    return 0;
}

int uui_router_overlay_key(struct uui_router *r, int key, unsigned mods,
                            int *out_changed) {
    int changed = 0;
    int id = 0;
    struct uui_item *ov = overlay_owner(r->items, r->count);
    if (ov && ov->ops->key && ov->ops->key(ov->widget, key, mods)) {
        changed = 1;
        id = ov->id;
    }
    if (out_changed) *out_changed = changed;
    return id;
}

int uui_router_release(struct uui_router *r, int cx, int cy, int *out_changed) {
    int changed = 0;
    int id = 0;
    if (r->dragging) {
        // The DROP: on a target that accepted, then the source is told
        // the session is over. The grab holder gets no `release` -- its
        // press became a drag, not a click -- and the id reported is
        // the target's, through take_drop(), never the source's.
        int dropped = 0;
        if (r->target && r->drag.accepted && r->target_ops->drop &&
            r->target_ops->drop(r->target, cx, cy, &r->drag)) {
            dropped = 1;
            r->drop_id = r->target_id;
            r->dropped = r->drag;
        } else if (r->target && r->target_ops->drag_over) {
            r->target_ops->drag_over(r->target, UUI_NOWHERE, UUI_NOWHERE, &r->drag);
        }
        if (r->grab_ops->drag_end) r->grab_ops->drag_end(r->grab, dropped);
        uui_router_reset(r);
        if (out_changed) *out_changed = 1;
        return 0;
    }
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

static void draw_items(struct ugfx_surface *s, struct uui_item *items, int count) {
    for (int i = 0; i < count; i++) {
        if (items[i].hidden) continue;
        int n = 0;
        struct uui_item *sub = nested(&items[i], &n);
        if (sub) {
            // NOT the container's own `draw`: uui_layout's paints its
            // items itself, for callers that drive a layout directly,
            // and calling it here would paint every child twice. A
            // container that wants a background paints it in
            // children_begin, which runs in the right place anyway.
            if (items[i].ops->children_begin) items[i].ops->children_begin(s, items[i].widget);
            draw_items(s, sub, n);
            if (items[i].ops->children_end) items[i].ops->children_end(s, items[i].widget);
            continue;
        }
        if (items[i].ops && items[i].ops->draw) items[i].ops->draw(s, items[i].widget);
    }
}

static void draw_overlays(struct ugfx_surface *s, struct uui_item *items, int count) {
    for (int i = 0; i < count; i++) {
        if (items[i].hidden) continue;
        int n = 0;
        struct uui_item *sub = nested(&items[i], &n);
        if (sub) { draw_overlays(s, sub, n); continue; }
        if (items[i].ops && items[i].ops->draw_overlay) {
            items[i].ops->draw_overlay(s, items[i].widget);
        }
    }
}

void uui_router_draw(struct uui_router *r, struct ugfx_surface *s) {
    draw_items(s, r->items, r->count);
    draw_overlays(s, r->items, r->count);
    // The ghost, above everything: what is being carried, at the pointer.
    if (r->dragging && r->grab_ops->drag_draw)
        r->grab_ops->drag_draw(s, r->grab, &r->drag);
}

static int wheel_item(struct uui_item *it, int cx, int cy, int notches,
                      int *changed, int *id) {
    if (it->hidden) return 0;
    int n = 0;
    struct uui_item *sub = nested(it, &n);
    if (sub) {
        if (!container_admits(it, cx, cy)) return 0;
        for (int i = n - 1; i >= 0; i--) {
            if (wheel_item(&sub[i], cx, cy, notches, changed, id)) return 1;
        }
        // No child wanted it -- the container scrolls instead. Child
        // first, then the ancestor, as every real toolkit does.
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
    // AN OPEN POPUP OWNS THE WHEEL AS IT OWNS THE POINTER: it (or its
    // subtree) takes the notches or nobody does. Letting the walk run
    // scrolled the list UNDER an open menu, which no desktop's grab
    // allows -- and would scroll the document behind a modal.
    struct uui_item *ov = overlay_owner(r->items, r->count);
    if (ov) {
        wheel_item(ov, cx, cy, notches, &changed, &id);
    } else {
        for (int i = r->count - 1; i >= 0; i--) {
            if (wheel_item(&r->items[i], cx, cy, notches, &changed, &id)) break;
        }
    }
    if (out_changed) *out_changed = changed;
    return id;
}
