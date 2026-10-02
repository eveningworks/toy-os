// sidebar -- see ui/uui_sidebar.h for the contract, and for why a
// navigation sidebar is not an outline view however alike they look.
//
// The whole widget is one asymmetry: a HEADING is drawn and nothing
// else, an ITEM is everything else. That distinction is applied in five
// separate places below -- hit testing, hover, selection, the arrow
// keys and the focus ring -- and every one of them has to honour it or
// a heading becomes reachable by some route the others closed. They are
// all written against `is_item()` for exactly that reason: one
// predicate, so they cannot drift apart.
#include "ui/uui_sidebar.h"
#include "ui/uui_describe.h"
#include "ui/uui_widget.h"
#include "ui/uui_scrollbar.h"
#include "lib/icon_cache.h" // icon_get() -- a heading may carry an icon
#include "keyboard.h" // KEY_* codes, as delivered by WIN_EV_KEY

#define UUI_SIDEBAR_PAD_X   6  // left inset for a heading
#define UUI_SIDEBAR_INDENT  10 // extra inset for an item under one

static int is_item(const struct uui_sidebar *s, int row) {
    return row >= 0 && row < s->count &&
           s->rows[row].kind != UUI_SIDEBAR_HEADING &&
           s->rows[row].kind != UUI_SIDEBAR_SEP;
}

// The first selectable row, or -1 when the sidebar is all headings --
// which is a legal (if useless) state and must not select one of them.
static int first_item(const struct uui_sidebar *s) {
    for (int i = 0; i < s->count; i++)
        if (is_item(s, i)) return i;
    return -1;
}

void uui_sidebar_init(struct uui_sidebar *s, int x, int y, int w, int h,
                       const struct uui_sidebar_row *rows, int count) {
    s->x = x; s->y = y; s->w = w; s->h = h;
    s->rows = rows;
    s->count = count;
    s->selected = -1;
    s->hovered = -1;
    s->top = 0;
    s->row_h = 0; // derive from the font
    s->bar_w = 8;
    s->thumb_grab = -1;
    s->bg = ugfx_rgb(246, 246, 248);
    s->fg = ugfx_rgb(20, 20, 20);
    // Slightly dimmer than an item, but only slightly -- and the first
    // attempt at this got the balance wrong in a way worth recording.
    // At (110,112,120) the grey cancelled the bold: on a monospace face
    // bold changes stroke weight and nothing else, so a caption that is
    // both lighter AND the same width simply reads as regular text in
    // grey, and the hierarchy the bold was for disappeared. Measured
    // rather than eyeballed -- ink per character came out LOWER for a
    // heading than for an item beside it.
    //
    // So: dim enough to sit behind the destinations, dark enough that
    // the weight still shows. KDE and GNOME grey their headers too, but
    // both do it on proportional faces where bold also widens.
    s->heading_fg = ugfx_rgb(64, 66, 74);
    s->sel_bg = ugfx_rgb(205, 220, 240);
    s->sel_fg = ugfx_rgb(20, 20, 20);
    s->track_bg = UTHEME_BUTTON_BG;
    s->thumb_bg = ugfx_rgb(150, 155, 165);
    s->selected = first_item(s);
}

void uui_sidebar_set_rows(struct uui_sidebar *s,
                           const struct uui_sidebar_row *rows, int count) {
    // KEEP THE SELECTION BY ID ACROSS A REBUILD. The rows are the app's
    // array and it may hand over a completely different one; what the
    // user cares about is the page they were on, which is an id. System
    // Settings rebuilds this whenever the setting registry changes, and
    // a rebuild that jumped back to the top would move them off the page
    // they were editing.
    int keep = uui_sidebar_selected_id(s);
    s->rows = rows;
    s->count = count;
    s->hovered = -1;
    s->top = 0;
    s->selected = -1;
    if (keep < 0 || !uui_sidebar_select_id(s, keep))
        s->selected = first_item(s);
}

int uui_sidebar_selected_id(const struct uui_sidebar *s) {
    if (!is_item(s, s->selected)) return -1;
    return s->rows[s->selected].id;
}

int uui_sidebar_select_id(struct uui_sidebar *s, int id) {
    for (int i = 0; i < s->count; i++) {
        // is_item() first: a heading's id is never matched, so an app
        // cannot navigate to one by handing back an id it read off the
        // array itself.
        if (is_item(s, i) && s->rows[i].id == id) {
            s->selected = i;
            return 1;
        }
    }
    return 0;
}

// --- geometry -----------------------------------------------------------

int uui_sidebar_row_h(const struct uui_sidebar *s) {
    if (s->row_h > 0) return s->row_h;
    int h = ugfx_char_h() + 6;
    return h > 0 ? h : 1;
}

int uui_sidebar_visible_rows(const struct uui_sidebar *s) {
    int rh = uui_sidebar_row_h(s);
    int n = rh > 0 ? s->h / rh : 0;
    return n > 0 ? n : 1;
}

#define row_h(s)        uui_sidebar_row_h(s)
#define visible_rows(s) uui_sidebar_visible_rows(s)

static int scrollbar_visible(const struct uui_sidebar *s) {
    return s->count > visible_rows(s);
}

// The scrollbar counts from the BOTTOM (offset 0 = scrolled to the end)
// while `top` counts from the start. Same pair as uui_tree and
// uui_listbox, deliberately identical, so the three cannot disagree
// about which way a thumb moves -- getting it backwards is invisible
// until somebody drags.
static int bar_x(const struct uui_sidebar *s) { return s->x + s->w - s->bar_w; }

static int offset_of(const struct uui_sidebar *s) {
    return s->count - visible_rows(s) - s->top;
}

static void clamp_top(struct uui_sidebar *s) {
    int max_top = s->count - visible_rows(s);
    if (max_top < 0) max_top = 0;
    if (s->top > max_top) s->top = max_top;
    if (s->top < 0) s->top = 0;
}

// The inverse of offset_of(): the scrollbar speaks offsets, `top` is
// what this widget stores.
static int set_offset(struct uui_sidebar *s, int offset) {
    int before = s->top;
    s->top = s->count - visible_rows(s) - offset;
    clamp_top(s);
    return s->top != before;
}

// Scrolls until `row` is on screen. Called after any keyboard move, so
// arrowing past the bottom follows rather than silently moving a
// selection nobody can see.
static void reveal(struct uui_sidebar *s, int row) {
    if (row < 0) return;
    int vis = visible_rows(s);
    if (row < s->top) s->top = row;
    else if (row >= s->top + vis) s->top = row - vis + 1;
    clamp_top(s);
}

// A heading's icon is square and sized to the text, so it scales with
// the font rather than pinning a pixel count -- the rule everything
// drawn here follows (docs/gui-guidelines.md: layout is FONT-DERIVED).
static int icon_px(void) { return ugfx_char_h(); }

// THE ICON GUTTER: a column every row reserves, whether or not it has
// an icon, and 0 for a sidebar where no row does.
//
// **PER SIDEBAR, NOT PER ROW, and that is the whole point.** The first
// version indented only the rows that HAD an icon, which pushed the
// headings right while their child items stayed where they were -- so
// the headings ended up further right than the rows beneath them and
// the hierarchy read backwards. An icon that only some rows carry
// cannot also be the thing that sets their indent. Every sidebar in
// every system that has icons does it this way for exactly that reason.
static int icon_gutter(const struct uui_sidebar *s) {
    for (int i = 0; i < s->count; i++)
        if (s->rows[i].kind != UUI_SIDEBAR_ITEM &&
            s->rows[i].kind != UUI_SIDEBAR_SEP && s->rows[i].icon)
            return icon_px() + UUI_SIDEBAR_PAD_X;
    return 0;
}

static int text_x(const struct uui_sidebar *s, int row) {
    // An item is indented under its heading; a heading is not. That is
    // the entire visual grouping -- the icon below decorates a heading,
    // it does not do the grouping.
    return s->x + UUI_SIDEBAR_PAD_X + icon_gutter(s) +
           (s->rows[row].kind == UUI_SIDEBAR_ITEM ? UUI_SIDEBAR_INDENT : 0);
}

// The widget WANTS room for its longest row at its own indent, measured
// over every row -- natural size must not depend on which row happens to
// be selected or scrolled to. See CLAUDE.md on natural_size.
//
// Measured with the HEADING FONT for headings, because bold is wider:
// measuring everything in regular makes a sidebar exactly too narrow for
// its own captions, which is the bug that would be blamed on clipping.
void uui_sidebar_natural_size(const struct uui_sidebar *s, int *out_w, int *out_h) {
    int widest = 0;
    for (int i = 0; i < s->count; i++) {
        if (s->rows[i].kind == UUI_SIDEBAR_SEP) continue;
        int heading = s->rows[i].kind != UUI_SIDEBAR_ITEM;
        const struct ugfx_font *was =
            ugfx_set_font(heading ? ugfx_font_session(UGFX_FONT_BOLD) : 0);
        // THE GUTTER COUNTS TOWARDS THE WIDTH. It is added to every
        // row's text_x, so leaving it out here makes the widget ask for
        // exactly the gutter's width less than it needs -- which shows
        // up as the longest label clipped ("System Informati"), and
        // gets blamed on the clipping rather than on the measurement.
        int w = icon_gutter(s) + (heading ? 0 : UUI_SIDEBAR_INDENT) +
                ugfx_text_width(s->rows[i].label);
        ugfx_set_font(was);
        if (w > widest) widest = w;
    }
    if (out_w) *out_w = UUI_SIDEBAR_PAD_X * 2 + widest + s->bar_w;
    if (out_h) *out_h = row_h(s) * (s->count > 0 ? s->count : 1);
}

void uui_sidebar_set_geometry(struct uui_sidebar *s, int x, int y, int w, int h) {
    s->x = x; s->y = y; s->w = w; s->h = h;
    clamp_top(s);
}

// --- drawing ------------------------------------------------------------

void uui_sidebar_draw(struct ugfx_surface *surf, const struct uui_sidebar *s) {
    int rh = row_h(s);
    int vis = visible_rows(s);
    int bar = scrollbar_visible(s) ? s->bar_w : 0;

    ugfx_fill_rect(surf, s->x, s->y, s->w, s->h, s->bg);

    int sel_ry = -1; // the selected row's screen y, or -1 if off-screen

    for (int r = 0; r < vis; r++) {
        int row = s->top + r;
        if (row >= s->count) break;
        int ry = s->y + r * rh;
        // TWO QUESTIONS, NOT ONE. `heading` is about WEIGHT and indent
        // -- true of a caption and of a collapsed top-level row alike --
        // while `inert` is about whether it can be chosen. Conflating
        // them is why a selected TOP row painted no highlight the first
        // time this was written.
        // A RULE AND NOTHING ELSE -- before the weight/indent questions
        // below, none of which a separator has an answer to.
        if (s->rows[row].kind == UUI_SIDEBAR_SEP) {
            int inset = UUI_SIDEBAR_PAD_X * 2;
            ugfx_fill_rect(surf, s->x + inset, ry + rh / 2,
                            s->w - bar - inset * 2, 1, s->heading_fg);
            continue;
        }
        int heading = s->rows[row].kind != UUI_SIDEBAR_ITEM;
        int inert = s->rows[row].kind == UUI_SIDEBAR_HEADING;
        int selected = (row == s->selected) && !inert;
        if (selected) sel_ry = ry;

        // NO HOVER AND NO SELECTION BOX ON A HEADING. A caption that
        // lights up under the pointer is telling the user it can be
        // clicked, and it cannot -- which is the specific lie this
        // widget exists to avoid.
        if (selected)
            ugfx_fill_rect(surf, s->x, ry, s->w - bar, rh, s->sel_bg);
        else if (!inert && row == s->hovered)
            ugfx_fill_rect(surf, s->x, ry, s->w - bar, rh,
                            uui_state_bg(s->bg, UUI_STATE_HOVER));

        // THE HEADING'S ICON, before the label and at the label's own
        // height. ugfx_blit_alpha(), never ugfx_blit(): an icon's
        // corners are transparent and a plain blit lands them as black
        // squares (docs/conventions/gui.md).
        if (heading && s->rows[row].icon) {
            int isz = icon_px();
            const struct uimg *ic = icon_get(s->rows[row].icon, isz);
            if (ic)
                ugfx_blit_alpha(surf, s->x + UUI_SIDEBAR_PAD_X,
                                 ry + (rh - isz) / 2, ic->w, ic->h, ic->px, ic->w);
        }

        const struct ugfx_font *was =
            ugfx_set_font(heading ? ugfx_font_session(UGFX_FONT_BOLD) : 0);

        int tx = text_x(s, row);
        int avail = s->x + s->w - bar - tx - UUI_SIDEBAR_PAD_X;
        if (avail > 0)
            // CLIPPED, always: a label longer than the pane must not run
            // into the page beside it (docs/gui-guidelines.md -- the
            // identical overlap bug has shipped twice).
            ugfx_draw_string_clipped(surf, tx, ry + (rh - ugfx_char_h()) / 2,
                                      avail, s->rows[row].label,
                                      // SELECTED WINS over the heading colour,
                                      // or a selectable heading row reads dark
                                      // on an accent fill.
                                      selected ? s->sel_fg
                                               : (heading ? s->heading_fg : s->fg),
                                      selected ? s->sel_bg : s->bg);
        ugfx_set_font(was);
    }

    if (bar)
        uui_scrollbar_draw(surf, s->x + s->w - bar, s->y, bar, s->h,
                            s->count, vis, offset_of(s),
                            s->track_bg, s->thumb_bg, 0);

    // On the selected row, or round the pane when it is scrolled off --
    // see uui_listbox.c for why an indicator that can vanish is not one.
    if (s->focused) {
        if (sel_ry >= 0) uui_focus_ring(surf, s->x, sel_ry, s->w - bar, rh);
        else             uui_focus_ring(surf, s->x, s->y, s->w, s->h);
    }
}

// --- input --------------------------------------------------------------

// Returns the row under the point, or -1 -- INCLUDING for a heading,
// which is not a target. Everything below routes through this, so there
// is exactly one place that decides a heading cannot be hit.
int uui_sidebar_hit(const struct uui_sidebar *s, int cx, int cy) {
    if (cx < s->x || cx >= s->x + s->w || cy < s->y || cy >= s->y + s->h) return -1;
    if (scrollbar_visible(s) && cx >= bar_x(s)) return -1;
    int row = s->top + (cy - s->y) / row_h(s);
    if (!is_item(s, row)) return -1;
    return row;
}

static int hover(struct uui_sidebar *s, int cx, int cy) {
    int row = uui_sidebar_hit(s, cx, cy);
    if (row == s->hovered) return 0;
    s->hovered = row;
    return 1;
}

int uui_sidebar_press(struct uui_sidebar *s, int cx, int cy) {
    if (!uui_hit(s->x, s->y, s->w, s->h, cx, cy)) return 0;
    if (scrollbar_visible(s) && cx >= bar_x(s)) {
        int vis = visible_rows(s);
        int off = offset_of(s);
        enum uui_scrollbar_zone zone =
            uui_scrollbar_hit(bar_x(s), s->y, s->bar_w, s->h, s->count, vis,
                               off, cx, cy, 0);
        if (zone == UUI_SB_THUMB) {
            int ty, th;
            uui_scrollbar_thumb_rect(s->y, s->h, s->count, vis, off,
                                      &ty, &th, s->bar_w, 0);
            // The grab offset WITHIN the thumb, so it tracks the cursor
            // instead of snapping its top to it.
            s->thumb_grab = cy - ty;
            return 1;
        }
        int page = vis > 1 ? vis - 1 : 1;
        if (zone == UUI_SB_ABOVE) set_offset(s, off + page);
        else if (zone == UUI_SB_BELOW) set_offset(s, off - page);
        return 1;
    }
    // ARMED HERE, COMMITTED ON RELEASE (docs/gui-guidelines.md): the
    // press only moves the highlight, so dragging off the row before
    // letting go cancels the navigation. The app is told in release.
    int row = uui_sidebar_hit(s, cx, cy);
    if (row < 0) return 0;
    s->selected = row;
    return 1;
}

int uui_sidebar_motion(struct uui_sidebar *s, int cx, int cy, unsigned buttons) {
    if (s->thumb_grab >= 0) {
        (void)cx; // a thumb drag follows y only, and survives leaving the bar
        int off = uui_scrollbar_offset_for_drag(s->y, s->h, s->count,
                                                 visible_rows(s), cy,
                                                 s->thumb_grab, s->bar_w, 0);
        return set_offset(s, off);
    }
    if (buttons) {
        // A drag with the button down keeps moving the highlight, the
        // way a listbox does, so a press that landed on the wrong row
        // can be corrected without letting go.
        int row = uui_sidebar_hit(s, cx, cy);
        if (row >= 0 && row != s->selected) { s->selected = row; return 1; }
        return 0;
    }
    return hover(s, cx, cy);
}

int uui_sidebar_release(struct uui_sidebar *s, int cx, int cy) {
    (void)cx; (void)cy;
    s->thumb_grab = -1;
    return 1;
}

int uui_sidebar_wheel(struct uui_sidebar *s, int notches) {
    if (!scrollbar_visible(s)) return 0;
    int before = s->top;
    s->top -= notches * 3;
    clamp_top(s);
    return s->top != before;
}

int uui_sidebar_key(struct uui_sidebar *s, int key, unsigned mods) {
    (void)mods;
    if (s->count <= 0) return 0;

    // **THE ARROWS STEP OVER HEADINGS**, which is the kind distinction
    // reaching the keyboard. A loop rather than +/-1 because two
    // headings can sit together (an empty category), and landing on one
    // would leave the sidebar with a selection it will not draw.
    if (key == KEY_ARROW_UP || key == KEY_ARROW_DOWN) {
        int dir = (key == KEY_ARROW_DOWN) ? 1 : -1;
        int i = s->selected;
        for (int step = 0; step < s->count; step++) {
            i += dir;
            if (i < 0 || i >= s->count) return 0; // ran off the end -- stay put
            if (is_item(s, i)) {
                s->selected = i;
                reveal(s, i);
                return 1;
            }
        }
        return 0;
    }
    if (key == KEY_HOME || key == KEY_END) {
        int target = -1;
        if (key == KEY_HOME) target = first_item(s);
        else for (int i = s->count - 1; i >= 0; i--)
            if (is_item(s, i)) { target = i; break; }
        if (target < 0 || target == s->selected) return 0;
        s->selected = target;
        reveal(s, target);
        return 1;
    }
    return 0;
}

// --- the ops table ------------------------------------------------------

static void draw_op(struct ugfx_surface *surf, const void *w) {
    uui_sidebar_draw(surf, (const struct uui_sidebar *)w);
}
// THE WHOLE CONTROL, headings and scrollbar strip included --
// uui_sidebar_hit() answers "which ITEM row", which is a different
// question. The router gates press AND wheel on this slot
// (uui_route.c), so answering the row question here left the scrollbar
// undraggable and the wheel dead over every heading. Same conflation
// uui_listbox, uui_table and uui_fileview each shipped; a boolean, so
// row 0 is not the falsey trap those three also hit.
static int hit_op(const void *w, int cx, int cy) {
    const struct uui_sidebar *s = (const struct uui_sidebar *)w;
    return uui_hit(s->x, s->y, s->w, s->h, cx, cy);
}
static int press_op(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    return uui_sidebar_press(w, cx, cy);
}
static int motion_op(void *w, int cx, int cy, unsigned buttons) {
    return uui_sidebar_motion(w, cx, cy, buttons);
}
static int release_op(void *w, int cx, int cy) { return uui_sidebar_release(w, cx, cy); }
static int wheel_op(void *w, int notches) { return uui_sidebar_wheel(w, notches); }
static int key_op(void *w, int key, unsigned mods) { return uui_sidebar_key(w, key, mods); }
static void natural_op(const void *w, int *out_w, int *out_h) {
    uui_sidebar_natural_size((const struct uui_sidebar *)w, out_w, out_h);
}
static void set_geometry_op(void *w, int x, int y, int rw, int rh) {
    uui_sidebar_set_geometry(w, x, y, rw, rh);
}

// A sidebar with no ITEMS has nothing to navigate, so it is skipped in
// the tab order rather than being a stop that does nothing. Note it is
// items and not rows: a sidebar of nothing but headings is unreachable,
// which is correct and is the whole point of the distinction.
static void set_focused_op(void *w, int focused) {
    ((struct uui_sidebar *)w)->focused = focused;
}

static int accepts_focus_op(const void *w) {
    return first_item((const struct uui_sidebar *)w) >= 0;
}

static void sb_ops_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_sidebar *s = (const struct uui_sidebar *)w;
    *x = s->x; *y = s->y; *ow = s->w; *oh = s->h;
}

// THE ROW PITCH, THE COUNT AND THE SELECTION. A sidebar reports no
// per-row rect -- its rows are a uniform grid, so the pitch plus the
// strip's own bounds is the whole answer, and a test that had to guess
// the pitch would be re-deriving a font-derived number.
static void sb_describe(const void *w, const struct uui_describe *d) {
    const struct uui_sidebar *s = (const struct uui_sidebar *)w;
    uui_describe_int(d, "row_h", uui_sidebar_row_h(s));
    uui_describe_int(d, "rows", s->count);
    uui_describe_int(d, "top", s->top);
    uui_describe_int(d, "selected", s->selected);
}

const struct uui_widget_ops uui_sidebar_ops = {
    .draw = draw_op,
    .accepts_focus = accepts_focus_op,
    .set_focused = set_focused_op,
    .hit = hit_op,
    .press = press_op,
    .motion = motion_op,
    .release = release_op,
    .wheel = wheel_op,
    .key = key_op,
    .natural_size = natural_op,
    .set_geometry = set_geometry_op,
    .bounds = sb_ops_bounds,
    .describe = sb_describe,
};
