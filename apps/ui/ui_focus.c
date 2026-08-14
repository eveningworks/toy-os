// See ui_focus.h for the design writeup -- in particular why routing
// keys by focus replaced trying each widget in turn, and why a widget
// joins by exporting an ops table rather than by being added to a switch.
#include "ui_focus.h"
#include "kapi.h"

#define RING_INSET 2

void ui_focus_init(struct ui_focus *f, struct ui_focusable *items, int count,
                    uint32_t ring) {
    f->items = items;
    f->count = count;
    f->current = -1; // nothing focused until clicked or tabbed to -- see the header
    f->ring = ring;
}

void *ui_focus_current(const struct ui_focus *f) {
    if (f->current < 0 || f->current >= f->count) return 0;
    return f->items[f->current].widget;
}

int ui_focus_index(const struct ui_focus *f) { return f->current; }

static int accepts(const struct ui_focusable *it) {
    if (!it->ops) return 0;
    if (!it->ops->accepts_focus) return 1; // NULL means "always" -- see the header
    return it->ops->accepts_focus(it->widget);
}

static void tell(struct ui_focus *f, int index, int focused) {
    if (index < 0 || index >= f->count) return;
    const struct ui_focusable *it = &f->items[index];
    if (it->ops && it->ops->set_focused) it->ops->set_focused(it->widget, focused);
}

void ui_focus_set(struct ui_focus *f, int index) {
    if (index < 0 || index >= f->count) index = -1;
    if (index == f->current) return;
    tell(f, f->current, 0);
    f->current = index;
    tell(f, f->current, 1);
}

// Shared by next/prev: step `dir` from the current index, wrapping, and
// stop at the first widget that will take focus. Bounded by `count` so a
// ring in which NOTHING accepts focus terminates instead of spinning --
// that state is reachable (every widget disabled) and a hang there would
// be a frozen desktop, not a missing highlight.
static int step(struct ui_focus *f, int dir) {
    if (f->count <= 0) return 0;
    int start = f->current;
    int i = start;
    for (int n = 0; n < f->count; n++) {
        i = (i < 0)
            ? (dir > 0 ? 0 : f->count - 1)
            : (i + dir + f->count) % f->count;
        if (accepts(&f->items[i])) {
            if (i == start) return 0; // already there; nothing moved
            ui_focus_set(f, i);
            return 1;
        }
    }
    return 0;
}

int ui_focus_next(struct ui_focus *f) { return step(f, +1); }
int ui_focus_prev(struct ui_focus *f) { return step(f, -1); }

int ui_focus_key(struct ui_focus *f, int key, uint8_t mods) {
    // Tab is 0x09 whether or not Shift is held -- Tab has no shifted
    // character, so the modifier bit is the ONLY thing distinguishing
    // them. See api/keyboard.h's "Modifier bits".
    if (key == '\t') {
        return (mods & KEY_MOD_SHIFT) ? ui_focus_prev(f) : ui_focus_next(f);
    }

    if (f->current < 0 || f->current >= f->count) return 0;
    const struct ui_focusable *it = &f->items[f->current];
    if (!it->ops || !it->ops->key) return 0;
    return it->ops->key(it->widget, key, mods);
}

int ui_focus_click(struct ui_focus *f, int cx, int cy) {
    int before = f->current;
    for (int i = 0; i < f->count; i++) {
        const struct ui_focusable *it = &f->items[i];
        if (!it->ops || !it->ops->hit) continue;
        if (!it->ops->hit(it->widget, cx, cy)) continue;
        if (!accepts(it)) continue; // a disabled widget doesn't take focus off the last one
        ui_focus_set(f, i);
        return f->current != before;
    }
    // Clicked nothing focusable. Focus is dropped rather than left
    // behind: keeping it would mean the next keystroke goes somewhere
    // the user has visibly clicked away from.
    ui_focus_set(f, -1);
    return f->current != before;
}

void ui_focus_ring_rect(int x, int y, int w, int h,
                         int origin_x, int origin_y, uint32_t color) {
    gfx_draw_rect(origin_x + x + RING_INSET, origin_y + y + RING_INSET,
                   w - RING_INSET * 2, h - RING_INSET * 2, color);
}

void ui_focus_draw_ring(const struct ui_focus *f, int origin_x, int origin_y) {
    if (f->current < 0 || f->current >= f->count) return;
    const struct ui_focusable *it = &f->items[f->current];
    if (!it->ops || !it->ops->draw_ring) return;
    it->ops->draw_ring(it->widget, origin_x, origin_y, f->ring);
}
