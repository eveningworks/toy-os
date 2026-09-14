// splitter -- see ui/uui_splitter.h for the fraction/delta model.
#include "ui/uui_splitter.h"
#include "ui/utheme.h"
#include "rt/sys.h"     // sys_ticks() -- the double-click window
#include "keyboard.h"
#include <stddef.h>

// Two double clicks of the same handle within this many ticks resets
// it. The same window uui_fileview uses, and for the same reason: one
// number for "that was a double click" across the toolkit.
#define SPLIT_DOUBLE_TICKS 30

int uui_splitter_thickness(void) {
    int t = ugfx_char_w() / 2;
    return t < 4 ? 4 : t;
}

void uui_splitter_init(struct uui_splitter *sp, int horizontal, int frac) {
    sp->x = sp->y = sp->w = sp->h = 0;
    sp->horizontal = horizontal ? 1 : 0;
    sp->lo = sp->hi = sp->min_before = sp->min_after = 0;
    sp->hovered = sp->dragging = sp->focused = sp->disabled = 0;
    sp->anchor_c = sp->anchor_frac = 0;
    sp->last_click_tick = 0;
    sp->def_frac = frac;
    sp->frac = 0;
    uui_splitter_set_frac(sp, frac);
}

void uui_splitter_set_track(struct uui_splitter *sp, int lo, int hi,
                             int min_before, int min_after) {
    sp->lo = lo;
    sp->hi = hi > lo ? hi : lo;
    sp->min_before = min_before > 0 ? min_before : 0;
    sp->min_after = min_after > 0 ? min_after : 0;
}

void uui_splitter_set_frac(struct uui_splitter *sp, int frac) {
    if (frac < 0) frac = 0;
    if (frac > UUI_SPLIT_SCALE) frac = UUI_SPLIT_SCALE;
    sp->frac = frac;
}

int uui_splitter_frac(const struct uui_splitter *sp) { return sp->frac; }

// How many pixels the fraction actually maps onto. Negative when the
// track cannot hold both minima and the band -- a window dragged
// narrower than its own content -- which every reader below treats as
// "no travel" rather than as an error.
static int travel_of(const struct uui_splitter *sp) {
    return (sp->hi - sp->lo) - uui_splitter_thickness()
            - sp->min_before - sp->min_after;
}

int uui_splitter_before(const struct uui_splitter *sp) {
    int travel = travel_of(sp);
    if (travel <= 0) {
        // Nothing to give: split what there is down the middle rather
        // than handing one side every pixel and the other none.
        int room = (sp->hi - sp->lo) - uui_splitter_thickness();
        return room > 0 ? room / 2 : 0;
    }
    return sp->min_before + (int)(((long)sp->frac * travel) / UUI_SPLIT_SCALE);
}

int uui_splitter_pos(const struct uui_splitter *sp) {
    return sp->lo + uui_splitter_before(sp);
}

int uui_splitter_after(const struct uui_splitter *sp) {
    int after = (sp->hi - sp->lo) - uui_splitter_before(sp)
                 - uui_splitter_thickness();
    return after > 0 ? after : 0;
}

void uui_splitter_natural_size(const struct uui_splitter *sp, int *out_w, int *out_h) {
    int t = uui_splitter_thickness();
    // 0 on the cross axis is "no preference" (uui_primitives.h): a
    // divider is as long as whatever it divides.
    if (out_w) *out_w = sp->horizontal ? t : 0;
    if (out_h) *out_h = sp->horizontal ? 0 : t;
}

void uui_splitter_set_geometry(struct uui_splitter *sp, int x, int y, int w, int h) {
    int t = uui_splitter_thickness();
    sp->x = x; sp->y = y;
    sp->w = sp->horizontal ? t : w;
    sp->h = sp->horizontal ? h : t;
    (void)w; (void)h;
}

// --- drawing ----------------------------------------------------------

void uui_splitter_draw(struct ugfx_surface *s, const struct uui_splitter *sp) {
    if (sp->w <= 0 || sp->h <= 0) return;

    enum uui_state st = sp->disabled ? UUI_STATE_DISABLED
                       : sp->dragging ? UUI_STATE_PRESSED
                       : sp->hovered  ? UUI_STATE_HOVER
                                       : UUI_STATE_REST;
    // At rest the band is the window's own colour and only the hairline
    // shows: a permanently grey bar between two panes reads as a third
    // pane. The tint is what says "this one moves", and it comes from
    // the panel colour rather than being hand-picked, so it darkens on
    // a light theme and lightens on a dark one (uui_primitives.h).
    if (st != UUI_STATE_REST)
        ugfx_fill_rect(s, sp->x, sp->y, sp->w, sp->h,
                        uui_state_bg(UTHEME_PANEL_BG, st));

    int t = uui_splitter_thickness();
    if (sp->horizontal)
        ugfx_fill_rect(s, sp->x + t / 2, sp->y, 1, sp->h, UTHEME_BORDER);
    else
        ugfx_fill_rect(s, sp->x, sp->y + t / 2, sp->w, 1, UTHEME_BORDER);

    if (sp->focused && !sp->disabled)
        uui_focus_ring(s, sp->x, sp->y, sp->w, sp->h);
}

// --- input ------------------------------------------------------------

int uui_splitter_hit(const struct uui_splitter *sp, int cx, int cy) {
    return uui_hit(sp->x, sp->y, sp->w, sp->h, cx, cy);
}

// The coordinate the drag runs along.
static int along(const struct uui_splitter *sp, int cx, int cy) {
    return sp->horizontal ? cx : cy;
}

int uui_splitter_press(struct uui_splitter *sp, int cx, int cy) {
    if (sp->disabled || !uui_splitter_hit(sp, cx, cy)) return 0;

    unsigned long now = sys_ticks();
    int is_double = (now - sp->last_click_tick <= SPLIT_DOUBLE_TICKS);
    sp->last_click_tick = is_double ? 0 : now;
    if (is_double) {
        sp->dragging = 0;
        if (sp->frac == sp->def_frac) return 1;
        uui_splitter_set_frac(sp, sp->def_frac);
        return 1;
    }

    sp->dragging = 1;
    sp->anchor_c = along(sp, cx, cy);
    sp->anchor_frac = sp->frac;
    return 1;
}

int uui_splitter_drag(struct uui_splitter *sp, int cx, int cy) {
    if (sp->disabled || !sp->dragging) return 0;
    int travel = travel_of(sp);
    if (travel <= 0) return 0;

    int delta = along(sp, cx, cy) - sp->anchor_c;
    int want = sp->anchor_frac + (int)(((long)delta * UUI_SPLIT_SCALE) / travel);
    if (want == sp->frac) return 0;
    uui_splitter_set_frac(sp, want);
    return 1;
}

void uui_splitter_drag_end(struct uui_splitter *sp) { sp->dragging = 0; }

int uui_splitter_hover(struct uui_splitter *sp, int cx, int cy) {
    int on = !sp->disabled && uui_splitter_hit(sp, cx, cy);
    if (on == sp->hovered) return 0;
    sp->hovered = on;
    return 1;
}

int uui_splitter_key(struct uui_splitter *sp, int key) {
    if (sp->disabled) return 0;
    int travel = travel_of(sp);
    if (travel <= 0) return 0;

    // One character cell per press, so the nudge tracks the font like
    // everything else -- and at least one unit, or a very wide track
    // would round the step to nothing.
    // text-measure-ok: UUI_SPLIT_SCALE is a fixed-point scale, not a
    // character count -- this moves the divider by one cell of travel.
    int step = (int)(((long)ugfx_char_w() * UUI_SPLIT_SCALE) / travel);
    if (step < 1) step = 1;

    int before = sp->frac;
    switch (key) {
    case KEY_ARROW_LEFT:  case KEY_ARROW_UP:   uui_splitter_set_frac(sp, sp->frac - step); break;
    case KEY_ARROW_RIGHT: case KEY_ARROW_DOWN: uui_splitter_set_frac(sp, sp->frac + step); break;
    case KEY_HOME:        uui_splitter_set_frac(sp, 0); break;
    case KEY_END:         uui_splitter_set_frac(sp, UUI_SPLIT_SCALE); break;
    default: return 0;
    }
    return sp->frac != before;
}

// --- the ops table ----------------------------------------------------

static void sp_natural(const void *w, int *out_w, int *out_h) {
    uui_splitter_natural_size((const struct uui_splitter *)w, out_w, out_h);
}
static void sp_geometry(void *w, int x, int y, int width, int height) {
    uui_splitter_set_geometry((struct uui_splitter *)w, x, y, width, height);
}
static void sp_bounds(const void *w, int *x, int *y, int *out_w, int *out_h) {
    const struct uui_splitter *sp = w;
    if (x) *x = sp->x;
    if (y) *y = sp->y;
    if (out_w) *out_w = sp->w;
    if (out_h) *out_h = sp->h;
}
static void sp_draw(struct ugfx_surface *s, const void *w) {
    uui_splitter_draw(s, (const struct uui_splitter *)w);
}
static int sp_hit(const void *w, int cx, int cy) {
    return uui_splitter_hit((const struct uui_splitter *)w, cx, cy);
}
static int sp_press(void *w, int cx, int cy, unsigned mods) { return uui_splitter_press(w, cx, cy); }
static int sp_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_splitter *sp = w;
    // A DRAG NEEDS THE BUTTON STILL DOWN: the grab lasts from press to
    // release, and a motion inside that window with nothing held would
    // otherwise move the divider (uui_scale.c has the measurement).
    if (sp->dragging && (buttons & 0x1)) return uui_splitter_drag(sp, cx, cy);
    return uui_splitter_hover(sp, cx, cy);
}
static int sp_release(void *w, int cx, int cy) {
    (void)cx; (void)cy;
    uui_splitter_drag_end((struct uui_splitter *)w);
    return 1; // so the router names this widget to the app
}
static int sp_cursor(const void *w, int cx, int cy) {
    const struct uui_splitter *sp = w;
    (void)cx; (void)cy;
    if (sp->disabled) return WIN_CURSOR_DEFAULT;
    return sp->horizontal ? WIN_CURSOR_RESIZE_H : WIN_CURSOR_RESIZE_V;
}
static int sp_key(void *w, int key, unsigned mods) {
    (void)mods;
    return uui_splitter_key(w, key);
}
static int sp_accepts_focus(const void *w) {
    const struct uui_splitter *sp = w;
    return !sp->disabled && travel_of(sp) > 0;
}
static void sp_set_focused(void *w, int focused) {
    ((struct uui_splitter *)w)->focused = focused;
}

// FILLED AGAINST uui_widget.h, not against the widget this was modelled
// on: a copied table inherits its gaps.
static void sp_describe(const void *w, const struct uui_describe *d) {
    uui_describe_int(d, "frac", uui_splitter_frac((const struct uui_splitter *)w));
}

const struct uui_widget_ops uui_splitter_ops = {
    .natural_size  = sp_natural,
    .set_geometry  = sp_geometry,
    .bounds        = sp_bounds,
    .draw          = sp_draw,
    .hit           = sp_hit,
    .press         = sp_press,
    .motion        = sp_motion,
    .release       = sp_release,
    .cursor        = sp_cursor,
    .key           = sp_key,
    .accepts_focus = sp_accepts_focus,
    .set_focused   = sp_set_focused,
    .describe      = sp_describe,
};
