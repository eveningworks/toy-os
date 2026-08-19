// tree -- see ui/uui_tree.h for the contract and why the nodes are the
// app's flat const array.
#include "ui/uui_tree.h"
#include "ui/uui_widget.h"
#include "keyboard.h" // KEY_* codes, as delivered by WIN_EV_KEY

#define UUI_TREE_PAD_X   4  // left inset before the first expander
#define UUI_TREE_INDENT  12 // per depth level
#define UUI_TREE_EXP_W   10 // the expander's hit width

void uui_tree_init(struct uui_tree *t, int x, int y, int w, int h,
                    const struct uui_tree_node *nodes, int count) {
    t->x = x; t->y = y; t->w = w; t->h = h;
    t->nodes = nodes;
    t->count = count;
    t->selected = count > 0 ? 0 : -1;
    t->hovered = -1;
    t->top = 0;
    t->row_h = 0; // derive from the font
    t->bar_w = 8;
    t->thumb_grab = -1;
    t->collapsed = 0; // EXPANDED by default -- see the header
    t->bg = ugfx_rgb(246, 246, 248);
    t->fg = ugfx_rgb(20, 20, 20);
    t->sel_bg = ugfx_rgb(205, 220, 240);
    t->sel_fg = ugfx_rgb(20, 20, 20);
    t->track_bg = ugfx_rgb(225, 225, 230);
    t->thumb_bg = ugfx_rgb(150, 155, 165);
    t->guide = ugfx_rgb(200, 200, 208);
}

void uui_tree_set_nodes(struct uui_tree *t, const struct uui_tree_node *nodes, int count) {
    t->nodes = nodes;
    t->count = count;
    if (t->selected >= count) t->selected = count > 0 ? count - 1 : -1;
    t->hovered = -1;
    t->top = 0;
    // Collapsed state is DROPPED with the nodes it described: bit 3 of
    // an old tree means nothing about a new one, and keeping it would
    // silently hide an unrelated row.
    t->collapsed = 0;
}

// --- structure, derived from the depth run ----------------------------

int uui_tree_is_parent(const struct uui_tree *t, int node) {
    if (node < 0 || node >= t->count - 1) return 0;
    return t->nodes[node + 1].depth > t->nodes[node].depth;
}

int uui_tree_is_collapsed(const struct uui_tree *t, int node) {
    if (node < 0 || node >= UUI_TREE_MAX_NODES) return 0;
    return (t->collapsed >> node) & 1u;
}

// Is `node` hidden by a COLLAPSED ANCESTOR? Walks forward tracking the
// shallowest collapsed depth still in effect, rather than searching
// backwards per node -- the same single pass every other loop here uses,
// so they cannot disagree about what is visible.
static int node_hidden(const struct uui_tree *t, int node) {
    int hide_below = -1; // depth at or under which everything is hidden
    for (int i = 0; i <= node && i < t->count; i++) {
        int d = t->nodes[i].depth;
        if (hide_below >= 0 && d <= hide_below) hide_below = -1; // out of it
        if (i == node) return hide_below >= 0;
        if (hide_below < 0 && uui_tree_is_parent(t, i) && uui_tree_is_collapsed(t, i))
            hide_below = d;
    }
    return 0;
}

int uui_tree_visible_count(const struct uui_tree *t) {
    int n = 0;
    for (int i = 0; i < t->count; i++) if (!node_hidden(t, i)) n++;
    return n;
}

int uui_tree_node_at_row(const struct uui_tree *t, int row) {
    if (row < 0) return -1;
    int n = 0;
    for (int i = 0; i < t->count; i++) {
        if (node_hidden(t, i)) continue;
        if (n == row) return i;
        n++;
    }
    return -1;
}

// The visible ROW a node sits on, or -1 when it is hidden.
static int row_of_node(const struct uui_tree *t, int node) {
    int n = 0;
    for (int i = 0; i < t->count; i++) {
        if (node_hidden(t, i)) continue;
        if (i == node) return n;
        n++;
    }
    return -1;
}

int uui_tree_set_collapsed(struct uui_tree *t, int node, int collapsed) {
    if (!uui_tree_is_parent(t, node)) return 0; // a leaf: no-op, not an error
    if (node >= UUI_TREE_MAX_NODES) return 0;
    int was = uui_tree_is_collapsed(t, node);
    if (was == !!collapsed) return 0;
    if (collapsed) t->collapsed |= (uint64_t)1 << node;
    else t->collapsed &= ~((uint64_t)1 << node);
    // Collapsing over the selection would leave a selected row nobody
    // can see, and an app reading uui_tree_selected_id() would keep
    // getting an id whose row is gone. Move it to the parent, which is
    // what every desktop tree does.
    if (collapsed && t->selected >= 0 && node_hidden(t, t->selected))
        t->selected = node;
    return 1;
}

void uui_tree_expand_all(struct uui_tree *t) { t->collapsed = 0; }

void uui_tree_collapse_all(struct uui_tree *t) {
    for (int i = 0; i < t->count && i < UUI_TREE_MAX_NODES; i++)
        if (uui_tree_is_parent(t, i)) t->collapsed |= (uint64_t)1 << i;
    if (t->selected >= 0 && node_hidden(t, t->selected)) {
        // Same rule as above, applied to whatever ancestor survives.
        for (int i = t->selected; i >= 0; i--)
            if (!node_hidden(t, i)) { t->selected = i; break; }
    }
}

// --- ids --------------------------------------------------------------

int uui_tree_selected_id(const struct uui_tree *t) {
    if (t->selected < 0 || t->selected >= t->count) return -1;
    return t->nodes[t->selected].id;
}

int uui_tree_select_id(struct uui_tree *t, int id) {
    for (int i = 0; i < t->count; i++) {
        if (t->nodes[i].id != id) continue;
        if (t->selected == i && !node_hidden(t, i)) return 0;
        // EXPAND WHATEVER IS HIDING IT. Selecting a node and leaving it
        // invisible looks to a user exactly like the call did nothing.
        while (node_hidden(t, i)) {
            for (int j = i - 1; j >= 0; j--) {
                if (t->nodes[j].depth < t->nodes[i].depth &&
                    uui_tree_is_collapsed(t, j)) {
                    uui_tree_set_collapsed(t, j, 0);
                    break;
                }
            }
        }
        t->selected = i;
        return 1;
    }
    return 0;
}

// --- geometry ---------------------------------------------------------

int uui_tree_row_h(const struct uui_tree *t) {
    return t->row_h > 0 ? t->row_h : ugfx_char_h() + 6;
}

int uui_tree_visible_rows(const struct uui_tree *t) {
    int rh = uui_tree_row_h(t);
    int n = rh > 0 ? t->h / rh : 0;
    return n > 0 ? n : 1;
}

int uui_tree_scrollbar_visible(const struct uui_tree *t) {
    return uui_tree_visible_count(t) > uui_tree_visible_rows(t);
}

// The scrollbar counts from the BOTTOM (offset 0 = scrolled to the end)
// while `top` counts from the start. Two helpers, lifted verbatim from
// uui_listbox, so the two widgets cannot disagree about which way a
// thumb moves -- getting this backwards is invisible until somebody
// drags.
static int tree_bar_x(const struct uui_tree *t) { return t->x + t->w - t->bar_w; }

static int tree_offset(const struct uui_tree *t) {
    return uui_tree_visible_count(t) - uui_tree_visible_rows(t) - t->top;
}

static void tree_clamp(struct uui_tree *t) {
    int vis = uui_tree_visible_rows(t);
    int total = uui_tree_visible_count(t);
    int max_top = total - vis;
    if (max_top < 0) max_top = 0;
    if (t->top > max_top) t->top = max_top;
    if (t->top < 0) t->top = 0;
}

// The widget WANTS room for its deepest, longest row -- measured with
// every node counted, collapsed or not, because natural size must not
// depend on the widget's current state any more than on where it is.
// A tree that shrank when collapsed would make a layout twitch as the
// user clicked. See CLAUDE.md on natural_size.
void uui_tree_natural_size(const struct uui_tree *t, int *out_w, int *out_h) {
    int widest = 0;
    for (int i = 0; i < t->count; i++) {
        int w = t->nodes[i].depth * UUI_TREE_INDENT + UUI_TREE_EXP_W +
                ugfx_text_width(t->nodes[i].label);
        if (w > widest) widest = w;
    }
    if (out_w) *out_w = UUI_TREE_PAD_X * 2 + widest + t->bar_w;
    if (out_h) *out_h = uui_tree_row_h(t) * (t->count > 0 ? t->count : 1);
}

// --- drawing ----------------------------------------------------------

static int row_text_x(const struct uui_tree *t, int node) {
    return t->x + UUI_TREE_PAD_X + t->nodes[node].depth * UUI_TREE_INDENT +
           UUI_TREE_EXP_W;
}

// A small filled triangle: right when collapsed, down when expanded --
// the disclosure shape every desktop tree uses, drawn rather than
// spelled with characters so it does not depend on the font.
static void draw_expander(struct ugfx_surface *s, int cx, int cy, int open,
                           uint32_t col) {
    for (int i = 0; i < 4; i++) {
        if (open) ugfx_fill_rect(s, cx - 3 + i, cy - 1 + i, 7 - 2 * i, 1, col);
        else      ugfx_fill_rect(s, cx - 1 + i, cy - 3 + i, 1, 7 - 2 * i, col);
    }
}

void uui_tree_draw(struct ugfx_surface *s, const struct uui_tree *t) {
    int rh = uui_tree_row_h(t);
    int vis = uui_tree_visible_rows(t);
    int total = uui_tree_visible_count(t);
    int bar = uui_tree_scrollbar_visible(t) ? t->bar_w : 0;

    ugfx_fill_rect(s, t->x, t->y, t->w, t->h, t->bg);

    for (int r = 0; r < vis; r++) {
        int node = uui_tree_node_at_row(t, t->top + r);
        if (node < 0) break;
        int ry = t->y + r * rh;
        int selected = (node == t->selected);
        if (selected)
            ugfx_fill_rect(s, t->x, ry, t->w - bar, rh, t->sel_bg);
        else if (node == t->hovered)
            ugfx_fill_rect(s, t->x, ry, t->w - bar, rh,
                            uui_state_bg(t->bg, UUI_STATE_HOVER));

        if (uui_tree_is_parent(t, node)) {
            draw_expander(s, t->x + UUI_TREE_PAD_X +
                              t->nodes[node].depth * UUI_TREE_INDENT + 4,
                          ry + rh / 2, !uui_tree_is_collapsed(t, node), t->fg);
        } else if (t->nodes[node].depth > 0) {
            // A guide tick for a child row, so depth reads at a glance
            // without an expander to anchor it.
            ugfx_fill_rect(s, t->x + UUI_TREE_PAD_X +
                               t->nodes[node].depth * UUI_TREE_INDENT + 3,
                           ry + rh / 2, 3, 1, t->guide);
        }

        // CLIPPED, always: a label longer than the pane must not run
        // into the page beside it. gfx_draw_string does not clip, and
        // that has caused the identical overlap bug twice already.
        int tx = row_text_x(t, node);
        int avail = t->x + t->w - bar - tx - UUI_TREE_PAD_X;
        if (avail > 0)
            ugfx_draw_string_clipped(s, tx, ry + (rh - ugfx_char_h()) / 2, avail,
                                      t->nodes[node].label,
                                      selected ? t->sel_fg : t->fg,
                                      selected ? t->sel_bg : t->bg);
    }

    if (bar)
        uui_scrollbar_draw(s, t->x + t->w - bar, t->y, bar, t->h,
                            total, vis, tree_offset(t), t->track_bg, t->thumb_bg, 0);
}

// --- input ------------------------------------------------------------

int uui_tree_hit(const struct uui_tree *t, int cx, int cy) {
    if (cx < t->x || cx >= t->x + t->w || cy < t->y || cy >= t->y + t->h) return -1;
    if (uui_tree_scrollbar_visible(t) && cx >= tree_bar_x(t)) return -1;
    int r = (cy - t->y) / uui_tree_row_h(t);
    return uui_tree_node_at_row(t, t->top + r);
}

int uui_tree_hit_expander(const struct uui_tree *t, int cx, int cy) {
    int node = uui_tree_hit(t, cx, cy);
    if (node < 0 || !uui_tree_is_parent(t, node)) return 0;
    int ex = t->x + UUI_TREE_PAD_X + t->nodes[node].depth * UUI_TREE_INDENT;
    return cx >= ex && cx < ex + UUI_TREE_EXP_W;
}

int uui_tree_hover(struct uui_tree *t, int cx, int cy) {
    int n = uui_tree_hit(t, cx, cy);
    if (n == t->hovered) return 0;
    t->hovered = n;
    return 1;
}

int uui_tree_click(struct uui_tree *t, int cx, int cy) {
    int node = uui_tree_hit(t, cx, cy);
    if (node < 0) return 0;
    // THE EXPANDER TOGGLES WITHOUT NAVIGATING. Clicking the triangle to
    // see what is inside a section is not the same gesture as choosing
    // that section, and a tree that conflated them would change the
    // page every time somebody explored.
    if (uui_tree_hit_expander(t, cx, cy))
        return uui_tree_set_collapsed(t, node, !uui_tree_is_collapsed(t, node));
    if (t->selected == node) return 0;
    t->selected = node;
    return 1;
}

int uui_tree_wheel(struct uui_tree *t, int notches) {
    int before = t->top;
    t->top -= notches;
    tree_clamp(t);
    return t->top != before;
}

static int tree_set_offset(struct uui_tree *t, int offset) {
    int before = t->top;
    t->top = uui_tree_visible_count(t) - uui_tree_visible_rows(t) - offset;
    tree_clamp(t);
    return t->top != before;
}

int uui_tree_press(struct uui_tree *t, int cx, int cy) {
    if (cx < t->x || cx >= t->x + t->w || cy < t->y || cy >= t->y + t->h) return 0;
    if (uui_tree_scrollbar_visible(t) && cx >= tree_bar_x(t)) {
        int vis = uui_tree_visible_rows(t);
        int total = uui_tree_visible_count(t);
        int off = tree_offset(t);
        enum uui_scrollbar_zone zone =
            uui_scrollbar_hit(tree_bar_x(t), t->y, t->bar_w, t->h, total, vis,
                              off, cx, cy, 0);
        if (zone == UUI_SB_THUMB) {
            int thumb_y, thumb_h;
            uui_scrollbar_thumb_rect(t->y, t->h, total, vis, off,
                                      &thumb_y, &thumb_h, t->bar_w, 0);
            // The grab offset WITHIN the thumb, so it tracks the cursor
            // instead of snapping its top to it -- the bug the ring-3
            // Notepad shipped by passing 0 here.
            t->thumb_grab = cy - thumb_y;
            return 1;
        }
        int page = vis > 1 ? vis - 1 : 1;
        if (zone == UUI_SB_ABOVE) tree_set_offset(t, off + page);
        else if (zone == UUI_SB_BELOW) tree_set_offset(t, off - page);
        return 1;
    }
    uui_tree_click(t, cx, cy);
    // NON-ZERO ON ANY HIT, even when nothing changed: the router takes
    // its pointer grab only when press does, so returning 0 for a
    // re-click on the selected row would silently cost that one row its
    // release. uui_radio_list shipped exactly that bug.
    return 1;
}

int uui_tree_drag(struct uui_tree *t, int cx, int cy) {
    (void)cx;
    if (t->thumb_grab < 0) return 0;
    int off = uui_scrollbar_offset_for_drag(t->y, t->h, uui_tree_visible_count(t),
                                             uui_tree_visible_rows(t), cy,
                                             t->thumb_grab, t->bar_w, 0);
    return tree_set_offset(t, off);
}

void uui_tree_drag_end(struct uui_tree *t) { t->thumb_grab = -1; }

// Scrolls the view so the selection is on screen. Called after any key
// that moves it -- a selection that moved off the top is indistinguishable
// from a key that did nothing.
static void reveal(struct uui_tree *t) {
    int row = row_of_node(t, t->selected);
    if (row < 0) return;
    int vis = uui_tree_visible_rows(t);
    if (row < t->top) t->top = row;
    else if (row >= t->top + vis) t->top = row - vis + 1;
    tree_clamp(t);
}

int uui_tree_key(struct uui_tree *t, int key) {
    if (t->count <= 0) return 0;
    int row = row_of_node(t, t->selected);
    int total = uui_tree_visible_count(t);

    switch (key) {
    case KEY_ARROW_UP:
        if (row > 0) { t->selected = uui_tree_node_at_row(t, row - 1); reveal(t); return 1; }
        return 0;
    case KEY_ARROW_DOWN:
        if (row >= 0 && row < total - 1) {
            t->selected = uui_tree_node_at_row(t, row + 1); reveal(t); return 1;
        }
        return 0;
    case KEY_HOME:
        t->selected = uui_tree_node_at_row(t, 0); reveal(t); return 1;
    case KEY_END:
        t->selected = uui_tree_node_at_row(t, total - 1); reveal(t); return 1;
    case KEY_ARROW_LEFT:
        // Collapse; or, if already collapsed (or a leaf), step OUT to
        // the parent. The two-step behaviour every desktop tree has --
        // Left on a closed node meaning "nothing" is the version that
        // feels broken.
        if (uui_tree_is_parent(t, t->selected) && !uui_tree_is_collapsed(t, t->selected))
            return uui_tree_set_collapsed(t, t->selected, 1);
        for (int i = t->selected - 1; i >= 0; i--) {
            if (t->nodes[i].depth < t->nodes[t->selected].depth) {
                t->selected = i; reveal(t); return 1;
            }
        }
        return 0;
    case KEY_ARROW_RIGHT:
        if (!uui_tree_is_parent(t, t->selected)) return 0;
        if (uui_tree_is_collapsed(t, t->selected))
            return uui_tree_set_collapsed(t, t->selected, 0);
        t->selected = uui_tree_node_at_row(t, row + 1); // into the first child
        reveal(t);
        return 1;
    default:
        return 0;
    }
}

// --- the ops tables ---------------------------------------------------

static void tree_draw_op(struct ugfx_surface *s, const void *w) {
    uui_tree_draw(s, (const struct uui_tree *)w);
}
// A BOOLEAN, and this is where uui_listbox and uui_table both got it
// wrong: uui_tree_hit() returns an INDEX, and node 0 is falsey.
static int tree_hit_op(const void *w, int cx, int cy) {
    return uui_tree_hit((const struct uui_tree *)w, cx, cy) >= 0;
}
static int tree_press_op(void *w, int cx, int cy) { return uui_tree_press(w, cx, cy); }
static int tree_motion_op(void *w, int cx, int cy, unsigned buttons) {
    (void)buttons;
    struct uui_tree *t = w;
    if (t->thumb_grab >= 0) return uui_tree_drag(t, cx, cy);
    return uui_tree_hover(t, cx, cy);
}
static int tree_release_op(void *w, int cx, int cy) {
    (void)cx; (void)cy;
    uui_tree_drag_end((struct uui_tree *)w);
    return 1;
}
static int tree_wheel_op(void *w, int notches) { return uui_tree_wheel(w, notches); }
static int tree_key_op(void *w, int key, unsigned mods) {
    (void)mods;
    return uui_tree_key(w, key);
}
static void tree_natural_op(const void *w, int *out_w, int *out_h) {
    uui_tree_natural_size((const struct uui_tree *)w, out_w, out_h);
}
static void tree_set_geometry_op(void *w, int x, int y, int rw, int rh) {
    struct uui_tree *t = w;
    t->x = x; t->y = y; t->w = rw; t->h = rh;
    tree_clamp(t);
}

const struct uui_widget_ops uui_tree_focus_ops = {
    .key = tree_key_op,
};

const struct uui_widget_ops uui_tree_ops = {
    .draw = tree_draw_op,
    .hit = tree_hit_op,
    .press = tree_press_op,
    .motion = tree_motion_op,
    .release = tree_release_op,
    .wheel = tree_wheel_op,
    .key = tree_key_op,
    .natural_size = tree_natural_op,
    .set_geometry = tree_set_geometry_op,
};
