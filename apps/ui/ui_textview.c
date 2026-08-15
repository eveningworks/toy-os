// See ui_textview.h for the design writeup -- in particular why
// scrolling belongs to the control rather than to each app.
#include "ui_textview.h"
#include "kapi.h"

#define DEFAULT_WHEEL_LINES 3
// Font-derived, not a fixed pixel count: a constant here would be right
// at exactly one font size. It used to spell the derivation out
// (`gfx_char_w() + 4`, matching the TERM_SCROLLBAR_W / NOTEPAD_SCROLLBAR_W
// this control replaced) -- and so did ui_listbox.c, so widening the bar
// meant finding both. It asks the scrollbar itself now.
static int default_bar_w(void) {
    int w;
    widget_scrollbar_natural_size(&w, NULL);
    return w;
}
#define DEFAULT_BAR_W      default_bar_w()
// Below this much room for text, AUTO hides the bar entirely -- the
// hide-when-narrow rule both apps also had, as bar_w * 3.
#define DEFAULT_MIN_TEXT_W (DEFAULT_BAR_W * 3)

void ui_textview_init(struct ui_textview *tv, int x, int y, int w, int h,
                       uint32_t bg, uint32_t track_bg, uint32_t thumb_bg,
                       uint32_t sel_bg) {
    widget_scrollback_init(&tv->tb);
    tv->x = x; tv->y = y; tv->w = w; tv->h = h;
    tv->policy = UI_SCROLLBAR_AUTO;
    tv->body = UI_TEXTVIEW_BODY_APP;
    tv->bar_w = DEFAULT_BAR_W;
    tv->min_text_w = DEFAULT_MIN_TEXT_W;
    tv->wheel_lines = DEFAULT_WHEEL_LINES;
    tv->page_overlap = 1;
    tv->show_caret = 0;
    tv->bg = bg;
    tv->track_bg = track_bg;
    tv->thumb_bg = thumb_bg;
    tv->sel_bg = sel_bg;
    tv->thumb_grab = -1;
    tv->panning = 0;
    tv->pan_last_y = 0;
    tv->pan_remainder = 0;
}

void ui_textview_natural_size(const struct ui_textview *tv, int *out_w, int *out_h) {
    (void)tv;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
}

void ui_textview_set_geometry(struct ui_textview *tv, int x, int y, int w, int h) {
    tv->x = x; tv->y = y; tv->w = w; tv->h = h;
}

// Text width assuming the bar IS shown -- needed to ask
// widget_scrollback_metrics() how many lines the content wraps to,
// which is itself what decides whether the bar shows. Resolving that
// circularity by measuring against the narrower width is deliberate: it
// means the bar never appears and then immediately disappears because
// reserving its strip made the text re-wrap to fit.
static int text_w_with_bar(const struct ui_textview *tv) {
    int tw = tv->w - tv->bar_w;
    return tw > 0 ? tw : 0;
}

int ui_textview_scrollbar_visible(const struct ui_textview *tv) {
    if (tv->policy == UI_SCROLLBAR_NEVER) return 0;
    if (text_w_with_bar(tv) < tv->min_text_w) return 0; // too narrow to be worth it
    if (tv->policy == UI_SCROLLBAR_ALWAYS) return 1;

    int total = 0, visible = 0;
    widget_scrollback_metrics((struct text_scrollback *)&tv->tb,
                               text_w_with_bar(tv), tv->h, &total, &visible);
    return total > visible;
}

int ui_textview_text_w(const struct ui_textview *tv) {
    return ui_textview_scrollbar_visible(tv) ? text_w_with_bar(tv) : tv->w;
}

int ui_textview_hit(const struct ui_textview *tv, int cx, int cy) {
    return widget_hit(tv->x, tv->y, tv->w, tv->h, cx, cy);
}

static void metrics(struct ui_textview *tv, int *total, int *visible) {
    widget_scrollback_metrics(&tv->tb, ui_textview_text_w(tv), tv->h, total, visible);
}

void ui_textview_draw(struct ui_textview *tv, int origin_x, int origin_y) {
    int sx = origin_x + tv->x, sy = origin_y + tv->y;
    int tw = ui_textview_text_w(tv);

    gfx_fill_rect(sx, sy, tv->w, tv->h, tv->bg);
    widget_scrollback_draw(&tv->tb, sx, sy, tw, tv->h, tv->bg, tv->sel_bg, tv->show_caret);

    if (!ui_textview_scrollbar_visible(tv)) return;
    int total, visible;
    metrics(tv, &total, &visible);
    widget_scrollbar_draw(sx + tw, sy, tv->bar_w, tv->h, total, visible,
                           tv->tb.scroll_offset, tv->track_bg, tv->thumb_bg);
}

int ui_textview_wheel(struct ui_textview *tv, int delta) {
    if (delta == 0) return 0;
    widget_scrollback_scroll(&tv->tb, delta * tv->wheel_lines);
    return 1;
}

// Which zone is this point in? Returns the scrollbar zone, or
// SCROLLBAR_ZONE_NONE for anything that isn't on the bar.
static enum scrollbar_zone bar_zone(struct ui_textview *tv, int cx, int cy) {
    if (!ui_textview_scrollbar_visible(tv)) return SCROLLBAR_ZONE_NONE;
    int tw = ui_textview_text_w(tv);
    if (cx < tv->x + tw) return SCROLLBAR_ZONE_NONE; // in the text body
    int total, visible;
    metrics(tv, &total, &visible);
    return widget_scrollbar_hit(tv->x + tw, tv->y, tv->bar_w, tv->h,
                                 total, visible, tv->tb.scroll_offset, cx, cy);
}

int ui_textview_click(struct ui_textview *tv, int cx, int cy) {
    if (!ui_textview_hit(tv, cx, cy)) return 0;
    enum scrollbar_zone zone = bar_zone(tv, cx, cy);
    if (zone != SCROLLBAR_ZONE_ABOVE && zone != SCROLLBAR_ZONE_BELOW) return 0;

    int total, visible;
    metrics(tv, &total, &visible);
    int page = visible > tv->page_overlap ? visible - tv->page_overlap : 1;
    widget_scrollback_scroll(&tv->tb, zone == SCROLLBAR_ZONE_ABOVE ? page : -page);
    return 1;
}

int ui_textview_drag_start(struct ui_textview *tv, int cx, int cy) {
    if (!ui_textview_hit(tv, cx, cy)) return 0;

    enum scrollbar_zone zone = bar_zone(tv, cx, cy);
    if (zone == SCROLLBAR_ZONE_THUMB) {
        int total, visible;
        metrics(tv, &total, &visible);
        int thumb_y, thumb_h;
        widget_scrollbar_thumb_rect(tv->y, tv->h, total, visible,
                                     tv->tb.scroll_offset, &thumb_y, &thumb_h);
        tv->thumb_grab = cy - thumb_y;
        return 1;
    }

    // A press anywhere ELSE on the bar is the track: decline it, so it
    // arrives at on_click and pages. Checked before the body case below
    // and not after -- with PAN enabled the first version fell straight
    // through and swallowed every track click as a pan, so the bar
    // stopped paging the moment panning was switched on. The bar always
    // outranks the body.
    if (zone != SCROLLBAR_ZONE_NONE) return 0;

    // A body press. Only claimed in PAN mode -- otherwise it belongs to
    // the app, which is what keeps Notepad's click-to-position and
    // drag-select working through this control unchanged.
    if (tv->body == UI_TEXTVIEW_BODY_PAN) {
        tv->thumb_grab = -1;
        tv->panning = 1;
        tv->pan_last_y = cy;
        tv->pan_remainder = 0;
        return 1;
    }
    return 0;
}

void ui_textview_drag(struct ui_textview *tv, int cx, int cy) {
    (void)cx;
    int total, visible;
    metrics(tv, &total, &visible);

    if (tv->thumb_grab >= 0) {
        tv->tb.scroll_offset =
            widget_scrollbar_offset_for_drag(tv->y, tv->h, total, visible,
                                              cy, tv->thumb_grab);
        return;
    }

    if (!tv->panning) return;
    // Pan: dragging DOWN reveals older content, the direction a hand
    // pushing paper would move it. The remainder carries sub-line
    // movement between ticks -- without it a slow drag truncates to
    // zero lines every tick and the view never moves at all.
    int line_h = gfx_char_h();
    if (line_h <= 0) return;
    int dy = (cy - tv->pan_last_y) + tv->pan_remainder;
    int lines = dy / line_h;
    tv->pan_remainder = dy - lines * line_h;
    tv->pan_last_y = cy;
    if (lines) widget_scrollback_scroll(&tv->tb, lines);
}

void ui_textview_drag_end(struct ui_textview *tv) {
    tv->thumb_grab = -1;
    tv->panning = 0;
    tv->pan_remainder = 0;
}
