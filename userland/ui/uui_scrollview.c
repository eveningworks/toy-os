// uui_scrollview -- see uui_scrollview.h for what it is and why.
//
// The whole widget rests on one decision: SCROLLING RE-RUNS THE CONTENT
// LAYOUT AT A SHIFTED ORIGIN. Everything else follows from it.
//
//   * A child's rect is its real on-screen rect, so hit-testing,
//     focus and drawing need no coordinate translation anywhere.
//   * A child scrolled out of view is genuinely elsewhere, not hidden
//     -- so input MUST be clipped to the viewport, or a child above the
//     top would still take clicks. That is the one thing this file has
//     to remember that a non-scrolling container does not.
//   * Re-laying out on every wheel notch is not the extravagance it
//     sounds: uui_layout is not retained mode (see its header), a
//     re-run is arithmetic over a handful of items, and a resize
//     already does exactly this.
#include "ui/uui_scrollview.h"
#include "ui/uui_scrollbar.h"
#include "ui/utheme.h"
#include "ui/ugfx.h"

static int row_px(const struct uui_scrollview *sv) {
    if (sv->step > 0) return sv->step;
    int r = ugfx_char_h();
    return r > 0 ? r : 8; // a font that has not loaded must not divide by zero
}

static int bar_px(const struct uui_scrollview *sv) {
    if (sv->bar_w > 0) return sv->bar_w;
    int w = 0, h = 0;
    uui_scrollbar_natural_size(&w, &h);
    (void)h;
    return w > 0 ? w : 8;
}

static int max_offset(const struct uui_scrollview *sv) {
    int over = sv->content_h - sv->h;
    return over > 0 ? over : 0;
}

// THE SCROLLBAR IS DRIVEN IN PIXELS HERE, NOT IN ROWS, and that is what
// makes the thumb reach the top. Its maths is a pure ratio -- thumb size
// is `h * visible / total` and its position `track_range * off /
// max_scroll` -- so any consistent unit works, and rows were not
// consistent: `total` rounded UP, `visible` rounded DOWN and the offset
// rounded DOWN again, so a view scrolled fully to the top reported an
// offset SHORT of the bar's own maximum and the thumb was drawn a few
// pixels below the track. The bottom was exact, which is why it read as
// "it will not go all the way up". In pixels both ends land exactly and
// a drag is smooth instead of snapping to a row.
//
// The three callers -- draw, hit and drag -- must pass the SAME three
// numbers, which is why they come from one place.
static void sb_units(const struct uui_scrollview *sv,
                     int *total, int *vis, int *off) {
    *total = sv->content_h > 0 ? sv->content_h : 1;
    *vis = sv->h;
    *off = max_offset(sv) - sv->offset;   // the bar counts from the BOTTOM
}

static int clamp_offset(struct uui_scrollview *sv) {
    int before = sv->offset;
    int max = max_offset(sv);
    if (sv->offset > max) sv->offset = max;
    if (sv->offset < 0) sv->offset = 0;
    return sv->offset != before;
}

// The one place the content is positioned. Called whenever anything it
// depends on moves: the viewport's rect, the offset, or -- since
// 2026-08-19 -- the content's ITEM LIST (uui_scrollview_content_changed).
//
// That third dependency was missing, and it was invisible for as long as
// no app changed a scroll view's contents at runtime. System Settings
// rebuilds its page's item list when you pick a different page, and
// without a re-layout the new widgets kept x/y/w/h at zero while the
// ones carried over from the previous page kept the PREVIOUS page's
// rects. It reads exactly like a layout that stops after four children.
static void place_content(struct uui_scrollview *sv) {
    if (!sv->content) { sv->content_h = 0; return; }

    // **THE CONTENT IS A NESTED LAYOUT, AND NOTHING ELSE WAS SAYING SO.**
    // `nested` is set by uui_layout_ops when a layout is placed as a
    // child, and a scroll view runs its content DIRECTLY -- so the page
    // inside one kept the outermost layout's default margin and paid it
    // a second time, on top of the window's own. Task Manager's Overview
    // was inset twice: 14 px from the window, then another 14 from a
    // page that had asked for `.margin = 0` and been given the default
    // anyway, because uui_layout_margin() reads 0 as "unset".
    // BEFORE the measure below, which spends the margin twice over.
    sv->content->nested = 1;

    int nw = 0, nh = 0;
    uui_layout_natural_size(sv->content, &nw, &nh);
    sv->content_h = nh;
    clamp_offset(sv);

    // Width: the viewport minus the scrollbar strip, when there is one.
    // Measured AFTER content_h, because whether the bar exists depends
    // on it.
    int inner_w = sv->w - (sv->content_h > sv->h ? bar_px(sv) : 0);
    if (inner_w < 0) inner_w = 0;

    // Height: the CONTENT's natural height, not the viewport's. That is
    // what makes this a scroll view rather than a squashed one -- the
    // children are laid out at the size they asked for and the viewport
    // shows a window onto them.
    uui_layout_run(sv->content, sv->x, sv->y - sv->offset, inner_w, sv->content_h);
    sv->seen_items = sv->content->items;
    sv->seen_count = sv->content->count;
}

void uui_scrollview_init(struct uui_scrollview *sv, struct uui_layout *content) {
    sv->x = sv->y = sv->w = sv->h = 0;
    sv->content = content;
    sv->offset = 0;
    sv->content_h = 0;
    sv->thumb_grab = -1;
    sv->seen_items = 0;
    sv->seen_count = -1; // -1, not 0: an EMPTY content list is a real
                          // state, and 0 would read as "already seen".
    sv->pref_rows = 0;
    sv->step = 0;
    sv->bar_w = 0;
    sv->bg = UTHEME_WINDOW_BG;
    // The same two values uui_listbox uses, so a scrollbar looks the
    // same whichever widget is showing one.
    sv->track_bg = UTHEME_BUTTON_BG;
    sv->thumb_bg = ugfx_rgb(150, 155, 165);
    if (content) uui_router_init(&sv->router, content->items, content->count);
    else uui_router_init(&sv->router, 0, 0);
}

void uui_scrollview_set_preferred_rows(struct uui_scrollview *sv, int rows) {
    sv->pref_rows = rows > 0 ? rows : 0;
}

void uui_scrollview_set_step(struct uui_scrollview *sv, int px) {
    sv->step = px > 0 ? px : 0;
}

int uui_scrollview_offset(const struct uui_scrollview *sv) { return sv->offset; }
int uui_scrollview_max_offset(const struct uui_scrollview *sv) { return max_offset(sv); }

int uui_scrollview_scrollable(const struct uui_scrollview *sv) {
    return sv->content_h > sv->h;
}

int uui_scrollview_set_offset(struct uui_scrollview *sv, int px) {
    int before = sv->offset;
    sv->offset = px;
    clamp_offset(sv);
    if (sv->offset == before) return 0;
    place_content(sv);
    return 1;
}

int uui_scrollview_reveal(struct uui_scrollview *sv, int y, int h) {
    if (h <= 0) return 0;
    // Content coordinates are screen coordinates (see this file's top
    // comment), so "where is this band relative to the viewport" is a
    // plain subtraction.
    if (y < sv->y) return uui_scrollview_set_offset(sv, sv->offset - (sv->y - y));
    if (y + h > sv->y + sv->h)
        return uui_scrollview_set_offset(sv, sv->offset + (y + h) - (sv->y + sv->h));
    return 0; // already visible -- scrolling would be the surprise
}

// --- the widget ops ---------------------------------------------------

static void sv_natural_size(const void *w, int *out_w, int *out_h) {
    const struct uui_scrollview *sv = w;
    int nw = 0, nh = 0;
    if (sv->content) uui_layout_natural_size(sv->content, &nw, &nh);

    // Width: the content's, plus room for a scrollbar. Asking for it
    // unconditionally costs one strip in the rare case it never appears
    // and avoids the content reflowing the moment it does.
    if (out_w) *out_w = nw + bar_px(sv);

    // Height: what the app asked for, capped at what there is to show.
    // The cap matters -- reserving twenty rows for a five-row page
    // gives a window a band of empty space nobody asked for.
    int rows = sv->pref_rows > 0 ? sv->pref_rows : 10;
    int want = rows * row_px(sv);
    if (out_h) *out_h = (nh > 0 && want > nh) ? nh : want;
}

// Has the content's item list been swapped since it was last laid out?
static int content_moved(const struct uui_scrollview *sv) {
    if (!sv->content) return 0;
    return sv->content->items != sv->seen_items ||
           sv->content->count != sv->seen_count;
}

void uui_scrollview_content_changed(struct uui_scrollview *sv) {
    // The offset is CLAMPED rather than reset: a page whose content grew
    // should not jump to the top, and place_content() clamps for us.
    // A caller that wants the top asks for it separately.
    place_content(sv);
}

static void sv_set_geometry(void *w, int x, int y, int width, int height) {
    struct uui_scrollview *sv = w;
    sv->x = x; sv->y = y; sv->w = width; sv->h = height;
    place_content(sv);
}

// The children are painted by the ROUTER, between these two -- see
// uui_widget.h. That is why the clip is set in one and cleared in the
// other rather than around a draw call here: a container cannot clip
// what it does not paint. The background goes here too, for the same
// reason -- a container's own `draw` is deliberately not called when it
// has children, or uui_layout would paint its items twice.
static void sv_children_begin(struct ugfx_surface *s, void *w) {
    struct uui_scrollview *sv = w;
    ugfx_fill_rect(s, sv->x, sv->y, sv->w, sv->h, sv->bg);
    // A non-positive w/h would set an EMPTY clip rather than none
    // (ugfx.h), which is exactly right for a zero-sized viewport.
    ugfx_set_clip_rect(s, sv->x, sv->y,
                        sv->w - (uui_scrollview_scrollable(sv) ? bar_px(sv) : 0), sv->h);
}

static void sv_children_end(struct ugfx_surface *s, void *w) {
    struct uui_scrollview *sv = w;
    ugfx_clear_clip_rect(s);

    if (!uui_scrollview_scrollable(sv)) return;

    int total, vis, off;
    sb_units(sv, &total, &vis, &off);
    uui_scrollbar_draw(s, sv->x + sv->w - bar_px(sv), sv->y, bar_px(sv), sv->h,
                        total, vis, off, sv->track_bg, sv->thumb_bg, 0);
}

// THE ONE ACCESSOR for the children, which is why the staleness check
// lives here: the router calls this before it routes input into them and
// before it paints them, so an item list swapped since the last layout
// is re-positioned before anything can use a zero rect.
//
// Making it AUTOMATIC rather than a call an app must remember is
// deliberate. The explicit uui_scrollview_content_changed() still
// exists and is still the honest thing to call, but an app that forgets
// it now gets a correctly laid-out page instead of a page whose new
// widgets are invisible and unclickable -- a failure that reads as a
// broken LAYOUT and cost a long hunt to trace back to a missing call.
// This project's own rule: an init step reachable by only one entry
// point is a bug waiting for a second entry point.
static struct uui_item *sv_children(void *w, int *out_count) {
    struct uui_scrollview *sv = w;
    if (!sv->content) { *out_count = 0; return 0; }
    if (content_moved(sv)) place_content(sv);
    *out_count = sv->content->count;
    return sv->content->items;
}

static int sv_hit(const void *w, int cx, int cy) {
    const struct uui_scrollview *sv = w;
    // The WHOLE widget, scrollbar strip included. A hit that covered
    // only the content would leave the bar unreachable -- the trap
    // uui_table shipped and CLAUDE.md records.
    return cx >= sv->x && cx < sv->x + sv->w && cy >= sv->y && cy < sv->y + sv->h;
}

// Is (cx, cy) on the scrollbar strip rather than the content?
static int on_bar(const struct uui_scrollview *sv, int cx) {
    if (!uui_scrollview_scrollable(sv)) return 0;
    return cx >= sv->x + sv->w - bar_px(sv);
}

static int bar_press(struct uui_scrollview *sv, int cx, int cy) {
    int total, vis, off;
    sb_units(sv, &total, &vis, &off);
    int bx = sv->x + sv->w - bar_px(sv);

    enum uui_scrollbar_zone zone =
        uui_scrollbar_hit(bx, sv->y, bar_px(sv), sv->h, total, vis, off, cx, cy, 0);

    if (zone == UUI_SB_THUMB) {
        int ty, th;
        uui_scrollbar_thumb_rect(sv->y, sv->h, total, vis, off, &ty, &th, bar_px(sv), 0);
        // The grab offset WITHIN the thumb, so it tracks the cursor
        // instead of snapping its top to it -- the bug the ring-3
        // Notepad shipped by passing 0 here.
        sv->thumb_grab = cy - ty;
        return 1;
    }

    // Page toward the click, keeping one row of overlap, as every real
    // toolkit does and as uui_listbox already does here. In pixels now,
    // like everything else the bar is driven with (sb_units()).
    int row = row_px(sv);
    int page = sv->h > row ? sv->h - row : row;
    if (zone == UUI_SB_ABOVE) return uui_scrollview_set_offset(sv, sv->offset - page);
    if (zone == UUI_SB_BELOW) return uui_scrollview_set_offset(sv, sv->offset + page);
    return 0;
}

// Reached only when no CHILD took the press (uui_route.c falls through
// to the container), so this is the scrollbar's and nothing else's.
static int sv_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_scrollview *sv = w;
    if (!sv_hit(sv, cx, cy)) return 0;
    if (on_bar(sv, cx)) return bar_press(sv, cx, cy);
    return 0; // inside the viewport but on nothing -- not ours to consume
}

static int sv_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_scrollview *sv = w;
    if (sv->thumb_grab >= 0) {
        int total, vis, off;
        sb_units(sv, &total, &vis, &off);
        off = uui_scrollbar_offset_for_drag(sv->y, sv->h, total, vis,
                                            cy, sv->thumb_grab, bar_px(sv), 0);
        return uui_scrollview_set_offset(sv, max_offset(sv) - off);
    }
    (void)cx; (void)cy; (void)buttons;
    return 0;
}

static int sv_release(void *w, int cx, int cy) {
    struct uui_scrollview *sv = w;
    (void)cx; (void)cy;
    if (sv->thumb_grab >= 0) { sv->thumb_grab = -1; return 1; }
    return 0;
}

static int sv_wheel(void *w, int notches) {
    struct uui_scrollview *sv = w;
    if (!uui_scrollview_scrollable(sv)) return 0;
    // Three rows a notch, matching uui_listbox so a wheel feels the same
    // wherever it is used.
    return uui_scrollview_set_offset(sv, sv->offset - notches * 3 * row_px(sv));
}

static void sv_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_scrollview *s = (const struct uui_scrollview *)w;
    *x = s->x; *y = s->y; *ow = s->w; *oh = s->h;
}

const struct uui_widget_ops uui_scrollview_ops = {
    .natural_size = sv_natural_size,
    .set_geometry = sv_set_geometry,
    .children       = sv_children,
    .children_begin = sv_children_begin,
    .children_end   = sv_children_end,
    .hit          = sv_hit,
    .press        = sv_press,
    .motion       = sv_motion,
    .release      = sv_release,
    .wheel        = sv_wheel,
    .bounds = sv_bounds,
};
