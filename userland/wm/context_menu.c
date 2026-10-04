// See context_menu.h for the design writeup.
#include "context_menu.h"
#include "wm_shadow.h"
#include "wm_internal.h"
#include "wm_overlay.h"
#include "ui/uui.h"
#include "ui/uui_menubar.h"
#include "ui/uui_popup.h"
#include "ui/utheme.h"
#include "kapi.h"

int context_menu_open = 0;

// The menu itself is uui_menubar's, opened as a free-floating popup
// (uui_menubar_open_at) with no bar strip behind it -- `count == 0`, so
// nothing walks a title. The compositor installs no uui_popup provider
// for its own panel, so every level falls through to the in-window path
// and is drawn straight into wm_surface(), clamped against the bounds
// set below.
static struct uui_menubar g_menu;

// THE CALLER'S ITEM MODEL IS KEPT, and translated here. A row commits a
// CODE, so the code indexes g_src back to the caller's own item and its
// on_select/ctx pair -- which is what lets desktop.c go on packing a
// gui_app pointer or a window index into a row.
// Past these a menu is TRUNCATED rather than overrunning: the rows that
// fit still work, which is the failure a WM menu can survive.
#define CM_MAX_ROWS 32
#define CM_MAX_SUB  96   // every row of every submenu, both levels together
#define CM_CODES    (CM_MAX_ROWS + CM_MAX_SUB)

static struct uui_menu_item g_items[CM_MAX_ROWS];
static struct uui_menu_item g_subs[CM_MAX_SUB];
static const struct context_menu_item *g_src[CM_CODES];
static int g_ncodes, g_subn;

static int add_code(const struct context_menu_item *it) {
    if (g_ncodes >= CM_CODES) return -1;
    g_src[g_ncodes] = it;
    return g_ncodes++;
}

// One row, and its submenu below it to `depth` more levels.
static void translate_row(const struct context_menu_item *s, struct uui_menu_item *d,
                          int depth) {
    *d = (struct uui_menu_item){ 0 };
    d->label = s->separator ? 0 : s->label;   // a NULL label IS the separator
    d->accel = s->accel;
    d->code = add_code(s);
    d->icon = s->icon;
    d->tint = s->tint;
    d->style = s->strip ? UUI_MIS_STRIP : 0;
    if (!s->sub || s->sub_count <= 0 || depth <= 0 || g_subn >= CM_MAX_SUB) return;
    int k = s->sub_count;
    if (g_subn + k > CM_MAX_SUB) k = CM_MAX_SUB - g_subn;
    struct uui_menu_item *rows = &g_subs[g_subn];
    g_subn += k;   // claimed before recursing, so a nested level packs after it
    d->sub = rows;
    d->sub_count = k;
    for (int j = 0; j < k; j++) translate_row(&s->sub[j], &rows[j], depth - 1);
}

static int build(const struct context_menu_item *src, int count) {
    g_ncodes = 0;
    g_subn = 0;
    int n = count > CM_MAX_ROWS ? CM_MAX_ROWS : count;
    for (int i = 0; i < n; i++) translate_row(&src[i], &g_items[i], 2);
    return n;
}

// The widget asks per item rather than reading a flag off the tree, so
// a tick follows the caller's live struct with nothing to keep in sync.
static unsigned item_flags(int code) {
    if (code < 0 || code >= g_ncodes || !g_src[code]) return 0;
    return (g_src[code]->checked ? UUI_MI_CHECKED : 0) |
           (g_src[code]->disabled ? UUI_MI_DISABLED : 0);
}

// --- geometry, all asked of the widget ---------------------------------

// A separator is half a row, so "the item height" is the first real
// row's -- asked of the widget rather than recomputed here, which is
// what stops this file owning a second copy of row_height().
static int level_item_h(int level) {
    const struct uui_menu_level *lv = &g_menu.level[level];
    for (int i = 0; i < lv->count; i++) {
        int x, y, w, h;
        if (!uui_menubar_item_rect(&g_menu, level, i, &x, &y, &w, &h)) break;
        if (lv->items[i].label) return h;
    }
    return ugfx_char_h();
}

static void damage_level(int level) {
    int x, y, w, h;
    if (uui_menubar_popup_rect(&g_menu, level, &x, &y, &w, &h))
        wm_damage_window_rect(x, y, w, h);   // plus its shadow (wm_shadow.h)
}

void context_menu_damage(void) {
    if (!context_menu_open) return;
    for (int l = 0; l < uui_menubar_depth(&g_menu); l++) damage_level(l);
    redraw_pending = 1;
}

int context_menu_showing(const struct context_menu_item *items) {
    return context_menu_open && g_ncodes > 0 && g_src[0] == &items[0];
}

void context_menu_open_at(int x, int y, const struct context_menu_item *items, int count) {
    int n = build(items, count);

    uui_menubar_init(&g_menu, 0, 0);   // no bar strip: a context menu is level 0 alone
    g_menu.item_flags = item_flags;
    // The widget's own card, with no colours of the panel's: a desktop
    // menu and an app's are one design (docs/gui-guidelines.md, "Menus").
    // The same usable rectangle wm_popup_place() clamps into, handed to
    // the widget's own flip/slide/clamp -- so a menu opened near the
    // taskbar or an edge stays whole, and one with no room below the
    // pointer flips above it as Windows' and KDE's do.
    uui_menubar_set_bounds(&g_menu, WM_POPUP_MARGIN, 0,
                           screen_w - 2 * WM_POPUP_MARGIN,
                           screen_h - taskbar_h - WM_POPUP_MARGIN);
    uui_menubar_open_at(&g_menu, g_items, n, x, y);

    wm_overlay_close_others("context");
    context_menu_open = 1;
    redraw_pending = 1;
}

int context_menu_geometry(int *x, int *y, int *w, int *item_h_out) {
    if (!context_menu_open) return 0;
    int lx, ly, lw, lh;
    if (!uui_menubar_popup_rect(&g_menu, 0, &lx, &ly, &lw, &lh)) return 0;
    if (x) *x = lx;
    if (y) *y = ly;
    if (w) *w = lw;
    if (item_h_out) *item_h_out = level_item_h(0);
    return g_menu.level[0].count;
}

int context_menu_contains(int mx, int my) {
    if (!context_menu_open) return 0;
    for (int lvl = 0; lvl < uui_menubar_depth(&g_menu); lvl++) {
        int lx, ly, lw, lh;
        if (uui_menubar_popup_rect(&g_menu, lvl, &lx, &ly, &lw, &lh) &&
            uui_hit(lx, ly, lw, lh, mx, my))
            return 1;
    }
    return 0;
}

int context_menu_sub_geometry(int *x, int *y, int *w, int *item_h_out) {
    if (!context_menu_open || uui_menubar_depth(&g_menu) < 2) return 0;
    int lx, ly, lw, lh;
    if (!uui_menubar_popup_rect(&g_menu, 1, &lx, &ly, &lw, &lh)) return 0;
    if (x) *x = lx;
    if (y) *y = ly;
    if (w) *w = lw;
    if (item_h_out) *item_h_out = level_item_h(1);
    return g_menu.level[1].count;
}

static const char *row_label(int level, int index) {
    if (!context_menu_open || level >= uui_menubar_depth(&g_menu)) return 0;
    const struct uui_menu_level *lv = &g_menu.level[level];
    if (index < 0 || index >= lv->count) return 0;
    return lv->items[index].label ? lv->items[index].label : "-";
}

const char *context_menu_row_label(int index) { return row_label(0, index); }
const char *context_menu_sub_row_label(int index) { return row_label(1, index); }

static int row_top(int level, int index) {
    int x, y, w, h;
    if (!uui_menubar_item_rect(&g_menu, level, index, &x, &y, &w, &h)) return 0;
    return y;
}

int context_menu_row_top(int index) { return row_top(0, index); }

int context_menu_row_rect(int level, int index, int *x, int *y, int *w, int *h) {
    if (!context_menu_open) return 0;
    return uui_menubar_item_rect(&g_menu, level, index, x, y, w, h);
}

int context_menu_level_rows(int level, int *x, int *y, int *w) {
    int h;
    if (!context_menu_open || !uui_menubar_popup_rect(&g_menu, level, x, y, w, &h)) return 0;
    return g_menu.level[level].count;
}

const char *context_menu_level_label(int level, int index) { return row_label(level, index); }

int context_menu_row_disabled(int level, int index) {
    if (!context_menu_open || level >= uui_menubar_depth(&g_menu)) return 0;
    const struct uui_menu_level *lv = &g_menu.level[level];
    if (index < 0 || index >= lv->count || !lv->items[index].label) return 0;
    return (item_flags(lv->items[index].code) & UUI_MI_DISABLED) != 0;
}
int context_menu_sub_row_top(int index) { return row_top(1, index); }

void context_menu_close(void) {
    if (context_menu_open) context_menu_damage();
    uui_menubar_close(&g_menu);
    context_menu_open = 0;
    wm_overlay_set_parent(0);   // the chain ends with its child
    redraw_pending = 1;
}

void context_menu_draw(int mx, int my) {
    (void)mx; (void)my;   // the hovered row is tracked, not derived here
    if (!context_menu_open) return;
    // Each open level casts a menu's small shadow, under the popups.
    for (int l = 0; l < uui_menubar_depth(&g_menu); l++) {
        int x, y, w, h;
        if (uui_menubar_popup_rect(&g_menu, l, &x, &y, &w, &h))
            (uui_popup_glass() ? wm_shadow_draw_hollow : wm_shadow_draw)(
                x, y, w, h, uui_popup_radius(), WM_SHADOW_POPUP);
    }
    uui_menubar_draw_popup(wm_surface(), &g_menu);
}

// The registry's hover op (wm_overlay.h). Not a pure query: a hovered
// row with a submenu OPENS it, as every desktop's menu does. The rects
// that CHANGE are damaged here, because the core damages only what is
// open after the call and a submenu that just closed is not.
int context_menu_hover_at(int mx, int my) {
    if (!context_menu_open) return 0;

    int ox[UUI_MENU_MAX_DEPTH], oy[UUI_MENU_MAX_DEPTH];
    int ow[UUI_MENU_MAX_DEPTH], oh[UUI_MENU_MAX_DEPTH];
    int old_depth = uui_menubar_depth(&g_menu);
    for (int l = 0; l < old_depth; l++)
        uui_menubar_popup_rect(&g_menu, l, &ox[l], &oy[l], &ow[l], &oh[l]);

    uui_menubar_motion(&g_menu, mx, my);

    for (int l = 0; l < old_depth; l++) {
        int x, y, w, h;
        if (uui_menubar_popup_rect(&g_menu, l, &x, &y, &w, &h) &&
            x == ox[l] && y == oy[l] && w == ow[l] && h == oh[l]) continue;
        wm_damage_rect(ox[l], oy[l], ow[l], oh[l]);
        redraw_pending = 1;
    }

    // The token: any two rows must differ, the same row must repeat.
    for (int l = uui_menubar_depth(&g_menu) - 1; l >= 0; l--)
        if (g_menu.level[l].hot >= 0) return (l + 1) * 1000 + g_menu.level[l].hot + 1;
    return 0;
}

int context_menu_handle_click(int mx, int my) {
    if (!context_menu_open) return 0;

    // Press then release at the same point, because the WM routes a
    // click on its button-DOWN edge (wm.c) and the widget commits on
    // the release. The press is what opens a submenu, switches levels
    // and dismisses an outside click; the release is what commits.
    context_menu_damage();
    uui_menubar_press(&g_menu, mx, my);
    int code = uui_menubar_release(&g_menu, mx, my);
    if (code >= 0 && code < g_ncodes && g_src[code] && g_src[code]->on_select)
        g_src[code]->on_select(g_src[code]->ctx);

    // Unlike start_menu.c's brief post-click flash, this closes as soon
    // as the widget does -- a right-click menu is a short-lived popup by
    // convention, and real desktops don't flash a context-menu selection
    // either. A click on a submenu parent or a separator leaves it open,
    // which every app menu here and on a real desktop already did; the
    // panel's own drawing used to close on a separator.
    context_menu_open = uui_menubar_is_open(&g_menu);
    if (!context_menu_open) wm_overlay_set_parent(0);
    redraw_pending = 1;
    return 1;
}

// The open menu owns the keyboard, as an xdg_popup's grab does: every
// key is the widget's while it is up, and a commit runs the row exactly
// as a click does.
int context_menu_key(int key, uint8_t mods) {
    (void)mods;
    if (!context_menu_open) return 0;
    context_menu_damage();
    int code;
    uui_menubar_key(&g_menu, key, &code);
    if (code >= 0 && code < g_ncodes && g_src[code] && g_src[code]->on_select)
        g_src[code]->on_select(g_src[code]->ctx);
    context_menu_open = uui_menubar_is_open(&g_menu);
    if (!context_menu_open) wm_overlay_set_parent(0);
    context_menu_damage();
    redraw_pending = 1;
    return 1;
}
