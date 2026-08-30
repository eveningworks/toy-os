// See uui_textview.h for the design writeup -- in particular why
// scrolling belongs to the control rather than to each app.
#include "ui/uui_textview.h"
#include "ui/uui_widget.h"  // the ops table at the bottom of this file

#define DEFAULT_WHEEL_LINES 3

// Font-derived, not a fixed pixel count: a constant would be right at
// exactly one font size. Asked of the scrollbar, so widening the bar is
// one edit rather than one per caller.
static int default_bar_w(void) {
    int w = 0;
    uui_scrollbar_natural_size(&w, 0);
    return w;
}

void uui_textview_init(struct uui_textview *tv, int x, int y, int w, int h,
                        uint32_t fg, uint32_t bg, uint32_t track_bg,
                        uint32_t thumb_bg, uint32_t sel_bg) {
    utext_init(&tv->tb);
    tv->x = x; tv->y = y; tv->w = w; tv->h = h;
    tv->policy = UUI_TEXTVIEW_AUTO;
    tv->body = UUI_TEXTVIEW_BODY_APP;
    tv->bar_w = default_bar_w();
    // Below this much room for text, AUTO hides the bar entirely.
    tv->min_text_w = tv->bar_w * 3;
    tv->wheel_lines = DEFAULT_WHEEL_LINES;
    tv->page_overlap = 1;
    tv->show_caret = 0;
    tv->bar_flags = 0;
    tv->fg = fg;
    tv->bg = bg;
    tv->track_bg = track_bg;
    tv->thumb_bg = thumb_bg;
    tv->sel_bg = sel_bg;
    tv->thumb_grab = -1;
    tv->panning = 0;
    tv->pan_last_y = 0;
    tv->pan_remainder = 0;
}

void uui_textview_set_geometry(struct uui_textview *tv, int x, int y, int w, int h) {
    tv->x = x; tv->y = y; tv->w = w; tv->h = h;
}

void uui_textview_natural_size(const struct uui_textview *tv, int *out_w, int *out_h) {
    (void)tv;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
}

// Text width assuming the bar IS shown -- needed to ask utext_metrics()
// how many lines the content wraps to, which is itself what decides
// whether the bar shows. Measuring against the narrower width resolves
// that circularity deliberately: the bar never appears and immediately
// disappears because reserving its strip made the text re-wrap to fit.
static int text_w_with_bar(const struct uui_textview *tv) {
    int tw = tv->w - tv->bar_w;
    return tw > 0 ? tw : 0;
}

int uui_textview_scrollbar_visible(const struct uui_textview *tv) {
    if (tv->policy == UUI_TEXTVIEW_NEVER) return 0;
    if (text_w_with_bar(tv) < tv->min_text_w) return 0; // too narrow to be worth it
    if (tv->policy == UUI_TEXTVIEW_ALWAYS) return 1;

    int total = 0, visible = 0;
    utext_metrics((struct utext *)&tv->tb, text_w_with_bar(tv), tv->h,
                  &total, &visible);
    return total > visible;
}

int uui_textview_text_w(const struct uui_textview *tv) {
    return uui_textview_scrollbar_visible(tv) ? text_w_with_bar(tv) : tv->w;
}

int uui_textview_hit(const struct uui_textview *tv, int cx, int cy) {
    return uui_hit(tv->x, tv->y, tv->w, tv->h, cx, cy);
}

static void metrics(struct uui_textview *tv, int *total, int *visible) {
    utext_metrics(&tv->tb, uui_textview_text_w(tv), tv->h, total, visible);
}

void uui_textview_draw(struct ugfx_surface *s, struct uui_textview *tv) {
    int tw = uui_textview_text_w(tv);

    ugfx_fill_rect(s, tv->x, tv->y, tv->w, tv->h, tv->bg);
    utext_draw(&tv->tb, s, tv->x, tv->y, tw, tv->h,
               tv->fg, tv->bg, tv->sel_bg, tv->show_caret);

    if (!uui_textview_scrollbar_visible(tv)) return;
    int total, visible;
    metrics(tv, &total, &visible);
    uui_scrollbar_draw(s, tv->x + tw, tv->y, tv->bar_w, tv->h,
                       total, visible, tv->tb.scroll_offset,
                       tv->track_bg, tv->thumb_bg, tv->bar_flags);
}

int uui_textview_wheel(struct uui_textview *tv, int delta) {
    if (delta == 0) return 0;
    utext_scroll(&tv->tb, delta * tv->wheel_lines);
    return 1;
}

// Which zone is this point in? UUI_SB_NONE for anything off the bar.
static enum uui_scrollbar_zone bar_zone(struct uui_textview *tv, int cx, int cy) {
    if (!uui_textview_scrollbar_visible(tv)) return UUI_SB_NONE;
    int tw = uui_textview_text_w(tv);
    if (cx < tv->x + tw) return UUI_SB_NONE; // in the text body
    int total, visible;
    metrics(tv, &total, &visible);
    return uui_scrollbar_hit(tv->x + tw, tv->y, tv->bar_w, tv->h,
                             total, visible, tv->tb.scroll_offset,
                             cx, cy, tv->bar_flags);
}

int uui_textview_click(struct uui_textview *tv, int cx, int cy) {
    if (!uui_textview_hit(tv, cx, cy)) return 0;
    enum uui_scrollbar_zone zone = bar_zone(tv, cx, cy);

    int total, visible;
    metrics(tv, &total, &visible);

    // An arrow steps one line; the track pages. Both are act-on-contact,
    // which is correct for a repeat-capable control (see
    // docs/gui-guidelines.md's scrollbar section).
    if (zone == UUI_SB_UP)   { utext_scroll(&tv->tb, 1);  return 1; }
    if (zone == UUI_SB_DOWN) { utext_scroll(&tv->tb, -1); return 1; }
    if (zone != UUI_SB_ABOVE && zone != UUI_SB_BELOW) return 0;

    int page = visible > tv->page_overlap ? visible - tv->page_overlap : 1;
    utext_scroll(&tv->tb, zone == UUI_SB_ABOVE ? page : -page);
    return 1;
}

int uui_textview_drag_start(struct uui_textview *tv, int cx, int cy) {
    if (!uui_textview_hit(tv, cx, cy)) return 0;

    enum uui_scrollbar_zone zone = bar_zone(tv, cx, cy);
    if (zone == UUI_SB_THUMB) {
        int total, visible;
        metrics(tv, &total, &visible);
        int thumb_y, thumb_h;
        uui_scrollbar_thumb_rect(tv->y, tv->h, total, visible,
                                 tv->tb.scroll_offset, &thumb_y, &thumb_h,
                                 tv->bar_w, tv->bar_flags);
        tv->thumb_grab = cy - thumb_y;
        return 1;
    }

    // A press anywhere ELSE on the bar is the track or an arrow: decline
    // it, so it reaches click() and pages. Checked BEFORE the body case
    // -- with PAN enabled, falling through here swallows every track
    // click as a pan, and the bar silently stops paging.
    if (zone != UUI_SB_NONE) return 0;

    if (tv->body == UUI_TEXTVIEW_BODY_PAN) {
        tv->thumb_grab = -1;
        tv->panning = 1;
        tv->pan_last_y = cy;
        tv->pan_remainder = 0;
        return 1;
    }
    return 0;
}

void uui_textview_drag(struct uui_textview *tv, int cx, int cy) {
    (void)cx;
    int total, visible;
    metrics(tv, &total, &visible);

    if (tv->thumb_grab >= 0) {
        tv->tb.scroll_offset =
            uui_scrollbar_offset_for_drag(tv->y, tv->h, total, visible,
                                          cy, tv->thumb_grab,
                                          tv->bar_w, tv->bar_flags);
        return;
    }

    if (!tv->panning) return;
    // Pan: dragging DOWN reveals older content, the direction a hand
    // pushing paper would move it. The remainder carries sub-line
    // movement between ticks -- without it a slow drag truncates to zero
    // lines every tick and the view never moves at all.
    int line_h = ugfx_char_h();
    if (line_h <= 0) return;
    int dy = (cy - tv->pan_last_y) + tv->pan_remainder;
    int lines = dy / line_h;
    tv->pan_remainder = dy - lines * line_h;
    tv->pan_last_y = cy;
    if (lines) utext_scroll(&tv->tb, lines);
}

void uui_textview_drag_end(struct uui_textview *tv) {
    tv->thumb_grab = -1;
    tv->panning = 0;
    tv->pan_remainder = 0;
}

// --- routed pointer input (ui/uui_route.h) ----------------------------
//
// The order below is the whole contract, and it is the same one the
// hand-written version in every app had to get right: a thumb press
// claims the drag, otherwise a track press pages, otherwise (in PAN
// mode) the body pans. Reversing the first two swallows track clicks as
// pans and the bar silently stops paging.
static int tv_ops_hit(const void *w, int cx, int cy) {
    return uui_textview_hit((const struct uui_textview *)w, cx, cy);
}

static int tv_ops_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_textview *tv = (struct uui_textview *)w;
    if (uui_textview_drag_start(tv, cx, cy)) return 1;
    return uui_textview_click(tv, cx, cy);
}

static int tv_ops_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_textview *tv = (struct uui_textview *)w;
    if (!buttons) return 0;
    if (tv->thumb_grab < 0 && !tv->panning) return 0;
    uui_textview_drag(tv, cx, cy);
    return 1;
}

static int tv_ops_release(void *w, int cx, int cy) {
    (void)cx; (void)cy;
    uui_textview_drag_end((struct uui_textview *)w);
    return 0;
}

static int tv_ops_wheel(void *w, int notches) {
    return uui_textview_wheel((struct uui_textview *)w, notches);
}

static void tv_ops_draw(struct ugfx_surface *s, const void *w) {
    uui_textview_draw(s, (struct uui_textview *)(void *)(const void *)w);
}

static void te_ops_natural_size(const void *w, int *out_w, int *out_h) {
    uui_textview_natural_size((const struct uui_textview *)w, out_w, out_h);
}

static void te_ops_set_geometry(void *w, int x, int y, int width, int height) {
    uui_textview_set_geometry((struct uui_textview *)w, x, y, width, height);
}

// NATURAL_SIZE AND SET_GEOMETRY were missing until 2026-08-19, so a
// uui_textview declared in a uui_layout was never positioned or measured
// -- it stayed at a zero rect and the layout could not size it. Both
// functions already existed; only the table was short. Three other
// widgets had the same gap the same day; tools/check_widget_ops.py
// exists to stop a fourth. See docs/decisions.md.
// Uniformly text, like uui_textbox's.
static int tv_ops_cursor(const void *w, int cx, int cy) {
    (void)w; (void)cx; (void)cy;
    return WIN_CURSOR_TEXT;
}

const struct uui_widget_ops uui_textview_ops = {
    .natural_size = te_ops_natural_size,
    .set_geometry = te_ops_set_geometry,
    .draw    = tv_ops_draw,
    .hit     = tv_ops_hit,
    .press   = tv_ops_press,
    .motion  = tv_ops_motion,
    .release = tv_ops_release,
    .wheel   = tv_ops_wheel,
    .cursor  = tv_ops_cursor,
};
