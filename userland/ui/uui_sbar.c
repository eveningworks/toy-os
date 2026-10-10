// uui_sbar -- a widget's scrollbar state. See uui_sbar.h.
#include "ui/uui_sbar.h"
#include "ui/uui_scrollbar.h"
#include "ui/uui_anim.h"

#define GROW_MS   150
#define LINGER_MS 400

void uui_sbar_init(struct uui_sbar *b) {
    *b = (struct uui_sbar){ .step = 1, .grab = -1 };
}

int uui_sbar_width(void) { return uui_scrollbar_overlay_width(); }

void uui_sbar_place(struct uui_sbar *b, int x, int y, int h) {
    b->x = x;
    b->y = y;
    b->w = uui_sbar_width();
    b->h = h;
}

static int max_top(const struct uui_sbar *b) {
    return b->total > b->visible ? b->total - b->visible : 0;
}

static int clamp_top(const struct uui_sbar *b, int top) {
    if (top > max_top(b)) top = max_top(b);
    return top < 0 ? 0 : top;
}

void uui_sbar_set(struct uui_sbar *b, int total, int visible, int top) {
    b->total = total;
    b->visible = visible;
    b->top = clamp_top(b, top);
}

int uui_sbar_shown(const struct uui_sbar *b) { return b->total > b->visible && b->h > 0; }

int uui_sbar_hit(const struct uui_sbar *b, int cx, int cy) {
    return uui_sbar_shown(b) && cx >= b->x && cx < b->x + b->w &&
           cy >= b->y && cy < b->y + b->h;
}

// How wide the bar is drawn now, 0 (thin) .. 255 (full): eased out
// toward the pointer, held LINGER_MS after it leaves, then eased back.
static int wide_at(const struct uui_sbar *b, unsigned long long now, int *settled) {
    *settled = 1;
    if (b->grab >= 0) return 255;
    unsigned long long dt = now > b->changed_ns ? now - b->changed_ns : 0;
    int to = b->hover ? 255 : 0;
    if (!b->hover) {
        if (dt < (unsigned long long)LINGER_MS * 1000000ull) {
            *settled = b->wide_from == 0;
            return b->wide_from;
        }
        dt -= (unsigned long long)LINGER_MS * 1000000ull;
    }
    unsigned long long span = (unsigned long long)GROW_MS * 1000000ull;
    if (dt >= span) return to;
    *settled = 0;
    int p = (int)(1024 - dt * 1024 / span);          // ease-out cubic
    int e = 1024 - (int)((long long)p * p / 1024 * p / 1024);
    return b->wide_from + (to - b->wide_from) * e / 1024;
}

static void set_hover(struct uui_sbar *b, int on) {
    int settled;
    unsigned long long now = uui_anim_now_ns();
    b->wide_from = wide_at(b, now, &settled);
    b->hover = on;
    b->changed_ns = now;
}

// The painter's offset: a vertical bar counts from the BOTTOM
// (uui_scrollbar.h), this struct from the start.
static int offset(const struct uui_sbar *b) { return max_top(b) - b->top; }

void uui_sbar_draw(const struct uui_sbar *b, struct ugfx_surface *s,
                   uint32_t ground, uint32_t ink) {
    if (!uui_sbar_shown(b)) return;
    int settled, wide = wide_at(b, uui_anim_now_ns(), &settled);
    if (!settled) uui_anim_request();
    uui_scrollbar_draw_overlay(s, b->x, b->y, b->w, b->h, b->total, b->visible,
                               offset(b), ground, ink, wide,
                               b->grab >= 0 ? UUI_SCROLLBAR_HELD : 0);
}

static int move_to(struct uui_sbar *b, int top) {
    top = clamp_top(b, top);
    if (top == b->top) return 0;
    b->top = top;
    return UUI_SBAR_MOVED;
}

int uui_sbar_press(struct uui_sbar *b, int cx, int cy) {
    if (!uui_sbar_hit(b, cx, cy)) return 0;
    int w = b->w;
    enum uui_scrollbar_zone z = uui_scrollbar_hit(b->x, b->y, w, b->h, b->total, b->visible,
                                                  offset(b), cx, cy, 0);
    if (z == UUI_SB_THUMB) {
        int ty, th;
        uui_scrollbar_thumb_rect(b->y, b->h, b->total, b->visible, offset(b), &ty, &th, w, 0);
        b->grab = cy - ty;   // where it was taken, so the thumb does not jump to the cursor
        return UUI_SBAR_TOOK | UUI_SBAR_REDRAW;
    }
    // The trough PAGES (guidelines, point 3), keeping one line of context.
    int page = b->visible - (b->step > 0 ? b->step : 1);
    if (page < 1) page = 1;
    if (z == UUI_SB_ABOVE) return UUI_SBAR_TOOK | move_to(b, b->top - page);
    if (z == UUI_SB_BELOW) return UUI_SBAR_TOOK | move_to(b, b->top + page);
    return UUI_SBAR_TOOK;
}

int uui_sbar_motion(struct uui_sbar *b, int cx, int cy, unsigned buttons) {
    (void)buttons;
    if (b->grab >= 0) {   // follows y only, and survives leaving the strip
        int off = uui_scrollbar_offset_for_drag(b->y, b->h, b->total, b->visible, cy, b->grab,
                                                b->w, 0);
        return UUI_SBAR_TOOK | move_to(b, max_top(b) - off);
    }
    int on = uui_sbar_hit(b, cx, cy);
    int r = 0;
    if (on != b->hover) { set_hover(b, on); r = UUI_SBAR_REDRAW; }
    return r | (on ? UUI_SBAR_TOOK : 0);
}

int uui_sbar_release(struct uui_sbar *b) {
    if (b->grab < 0) return 0;
    b->grab = -1;
    b->wide_from = 255;   // let go off the strip: it lingers, then eases thin
    b->changed_ns = uui_anim_now_ns();
    return UUI_SBAR_TOOK | UUI_SBAR_REDRAW;
}

int uui_sbar_leave(struct uui_sbar *b) {
    if (b->grab >= 0 || !b->hover) return 0;
    set_hover(b, 0);
    return UUI_SBAR_REDRAW;
}
