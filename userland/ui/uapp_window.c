// uapp's toplevels: what the main window and a dialog window share --
// input routing and painting -- and dialog windows. See ui/uapp_internal.h.
#include "ui/uapp_internal.h"

// ONE FRAME'S PAINTING, for either kind of toplevel. ORDER, and it is
// load-bearing: clear, then the APP's own painting, then the widgets, then
// overlays. The app paints UNDER its widgets, which makes the failure that
// produced this structurally impossible: an app whose on_draw began by
// clearing the surface -- the natural first line -- wiped everything the
// toolkit had drawn, and UI Demo came up blank with a 35-check suite
// passing it. An app that must paint OVER a widget says so with
// on_draw_over. `a` is the main window's app, for its draw hooks; a
// dialog window has none.
void top_paint(struct uapp_top *t, struct ugfx_surface *s, struct uui_layout *layout,
                      struct uapp *a) {
    if (layout || t->router.count) ugfx_fill(s, UTHEME_PANEL_BG);
    if (a && a->desc->on_draw) {
        struct uapp_draw d = { s, UTHEME_TEXT, UTHEME_PANEL_BG };
        a->desc->on_draw(a, &d);
    }
    if (layout) uui_layout_draw(s, layout);
    if (top_routes(t)) uui_router_draw(&t->router, s);
    if (a && a->desc->on_draw_over) {
        struct uapp_draw d = { s, UTHEME_TEXT, UTHEME_PANEL_BG };
        a->desc->on_draw_over(a, &d);
    }
}

// --- dialog windows: a second toplevel (ui/uapp.h) -------------------
//
// The same per-slot surface machinery a popup uses, with the compositor
// told to give this one chrome instead (WIN_REQ_DIALOG). What is NOT
// shared with the toplevel is the content: its own item array, its own
// router and its own focus ring, because it is a different window --
// a second view of the first one's widgets would be a split view, not
// a dialog.
struct uapp_window g_dlg[WIN_CLIENT_MAX];

struct uapp_window *dlg_for(uint32_t slot) {
    if (slot == 0 || slot >= WIN_CLIENT_MAX) return 0;
    return g_dlg[slot].top.slot ? &g_dlg[slot] : 0;
}

// Places the content and paints it. The same order flush() uses for the
// toplevel, minus the app hooks a dialog has no equivalent of.
void dlg_flush(struct uapp_window *w) {
    if (!w->top.slot || !w->top.dirty) return;
    w->top.dirty = 0;
    struct uapp_surf *s = &g_surf[w->top.slot];
    if (!surf_back(s)) { w->top.dirty = 1; return; }   // drawn once a buffer is back
    if (w->desc.layout) uui_layout_run(w->desc.layout, 0, 0, s->w, s->h);
    if (w->desc.log_prefix && layout_log_enabled()) {
        g_log_seen_n = 0;
        if (w->desc.layout) log_items(w->desc.log_prefix, w->desc.layout->items,
                                      w->desc.layout->count);
        if (w->top.router.count) log_items(w->desc.log_prefix, w->top.router.items, w->top.router.count);
        if (w->top.router.extra && uui_editmenu_is_open()) log_items(w->desc.log_prefix, w->top.router.extra, 1);
        if (w->desc.on_log_layout) w->desc.on_log_layout(w);
    }
    top_paint(&w->top, &s->surface, w->desc.layout, 0);
    layout_log_flush(1);
    surf_present(s);
    present_popups((uint32_t)w->top.slot);
}

int ids_unique(const struct uui_router *r, const char *who);   // below
int buttons_heard(const struct uui_router *r, int has_on_action, const char *who);

struct uapp_window *uapp_window_open(struct uapp *a, const struct uapp_window_desc *desc) {
    if (!a || !desc || desc->w <= 0 || desc->h <= 0 || comp_pid() <= 0) return 0;
    int w = desc->w, h = desc->h;
    clamp_to_screen(&w, &h);
    struct uapp_surf *s = 0;
    for (int i = 1; i < WIN_CLIENT_MAX; i++)
        if (!g_surf[i].used) { s = &g_surf[i]; break; }
    if (!s) return 0;
    if (!bufs_create(s, w, h)) return 0;
    int slot = slot_of(s);

    struct uapp_window *d = &g_dlg[slot];
    memset(d, 0, sizeof *d);
    d->desc = *desc;
    strlcpy(d->title, desc->title ? desc->title : "Dialog", sizeof d->title);

    struct wmchan_msg m, r;
    memset(&m, 0, sizeof m);
    m.type = WIN_REQ_DIALOG;
    m.window = (uint32_t)slot;
    m.a = w;
    m.b = h;
    m.c = WIN_DIALOG_C(a->window, (desc->flags & UAPP_WIN_MODAL) ? WIN_DIALOG_MODAL : 0);
    strlcpy(m.text, d->title, sizeof m.text);
    if (!wmchan() ||
        uchan_call(&g_wmchan, &m, sizeof m, &r, sizeof r, UAPP_CALL_TIMEOUT_MS) < 0 ||
        r.a < 0) {
        bufs_release(s);
        d->top.slot = 0;
        return 0;
    }

    d->top.slot = slot;
    d->top.ops = &WIN_OPS;
    d->top.focus = desc->focus;
    d->top.hears = desc->on_widget != 0;
    d->app = a;
    d->top.cursor = WIN_CURSOR_DEFAULT;
    if (desc->widgets && desc->widget_count > 0) {
        uui_router_init(&d->top.router, desc->widgets, desc->widget_count);
        // Too late to refuse the app, so the window routes nothing.
        if (!ids_unique(&d->top.router, desc->title) ||
            !buttons_heard(&d->top.router, desc->on_action != 0, desc->title))
            d->top.router.count = 0;
    }
    d->top.dirty = 1;
    dlg_flush(d);
    return d;
}

void uapp_window_close(struct uapp_window *w) {
    if (!w || !w->top.slot) return;
    int slot = w->top.slot;
    wmchan_send(WIN_REQ_DESTROY, (uint32_t)slot, 0, 0, 0, 0);
    bufs_release(&g_surf[slot]);
    w->top.slot = 0;
    if (w->app) w->app->top.dirty = 1;   // the owner repaints, now unblocked
}

int   uapp_window_is_open(const struct uapp_window *w) { return w && w->top.slot != 0; }
void  uapp_window_redraw(struct uapp_window *w)  { if (w && w->top.slot) w->top.dirty = 1; }
void *uapp_window_state(struct uapp_window *w)   { return w ? w->desc.state : 0; }
struct uapp *uapp_window_app(struct uapp_window *w) { return w ? w->app : 0; }
int uapp_window_width(const struct uapp_window *w)  { return w && w->top.slot ? g_surf[w->top.slot].w : 0; }
int uapp_window_height(const struct uapp_window *w) { return w && w->top.slot ? g_surf[w->top.slot].h : 0; }

void uapp_window_set_title(struct uapp_window *w, const char *title) {
    if (!w || !w->top.slot || !title) return;
    strlcpy(w->title, title, sizeof w->title);
    wmchan_send(WIN_REQ_TITLE, (uint32_t)w->top.slot, 0, 0, 0, w->title);
}

// One event, in the dialog's OWN coordinate space -- it is a toplevel,
// so nothing is translated. Its widgets are routed exactly as the
// toplevel's are; what a dialog has no equivalent of (scanout, resize,
// drag brokering) is simply not offered.
// --- telling the app what a widget did ------------------------------------
//
// A LONE BUTTON'S COMMIT IS A COMMAND: on_action, with the button's code.
// Its press, hover and a press dragged off reach the app not at all -- the
// button drew them. Every other widget's change is on_widget's. One number
// space for both is how Crash Reports' table (id 1) arrived as its Open
// command (1) on every hover. `committed` is the router's word for it: a
// release while armed, or a key the button took.
static const struct uui_button *lone_button(const struct uui_router *r, int id) {
    const struct uui_item *it = uui_router_item(r, id);
    return it && it->ops == &uui_button_ops ? it->widget : 0;
}

// MOTION reaches the app while a button is held (a drag, a thumb), or
// for an item that asked to hear hovers (UUI_TRACK_HOVER).
static int motion_wanted(const struct uui_router *r, int id, unsigned held) {
    if (held) return 1;
    const struct uui_item *it = uui_router_item(r, id);
    return it && (it->flags & UUI_TRACK_HOVER);
}

void tell_app(struct uapp *a, int id, int reason, int committed) {
    const struct uapp_desc *d = a->desc;
    if (!id) return;
    const struct uui_button *b = lone_button(&a->top.router, id);
    if (b) { if (committed && d->on_action) d->on_action(a, b->code); return; }
    if (d->on_widget) d->on_widget(a, id, reason);
}

static void tell_window(struct uapp_window *w, int id, int reason, int committed) {
    const struct uapp_window_desc *d = &w->desc;
    if (!id) return;
    const struct uui_button *b = lone_button(&w->top.router, id);
    if (b) { if (committed && d->on_action) d->on_action(w, b->code); return; }
    if (d->on_widget) d->on_widget(w, id, reason);
}

// --- one toplevel's input, routed the same way in either kind -------------

static struct uapp *uapp_of(struct uapp_top *t) { return (struct uapp *)t; }
static struct uapp_window *window_of(struct uapp_top *t) { return (struct uapp_window *)t; }

static void app_tell(struct uapp_top *t, int id, int reason, int committed) {
    tell_app(uapp_of(t), id, reason, committed);
}
static void app_key(struct uapp_top *t, int key, unsigned mods) {
    struct uapp *a = uapp_of(t);
    if (a->desc->on_key) a->desc->on_key(a, key, mods);
}
static void win_tell(struct uapp_top *t, int id, int reason, int committed) {
    tell_window(window_of(t), id, reason, committed);
}
static void win_key(struct uapp_top *t, int key, unsigned mods) {
    struct uapp_window *w = window_of(t);
    if (w->desc.on_key) w->desc.on_key(w, key, mods);
}
const struct uapp_top_ops APP_OPS = { app_tell, app_key };
const struct uapp_top_ops WIN_OPS = { win_tell, win_key };

// --- the shared edit menu (ui/uui_editmenu.h) ----------------------------
//
// THE ROUTER WHOSE `extra` IT HANGS ON, so a second window's router stops
// offering it presses once it has opened somewhere else.
static struct uui_router *g_menu_router;

// Is there anything to route? The app's widgets, or the menu, which an
// app that hand-routes everything (no widgets at all) still gets.
int top_routes(const struct uapp_top *t) {
    return t->router.count || (t->router.extra && uui_editmenu_is_open());
}

static void top_size(const struct uapp_top *t, int *w, int *h) {
    if (t == &g_app.top) { *w = g_app.w; *h = g_app.h; return; }
    *w = g_surf[t->slot].w;
    *h = g_surf[t->slot].h;
}

static void menu_open(struct uapp_top *t, const struct uui_edit_target *tg, int id, int x, int y) {
    if (g_menu_router && g_menu_router != &t->router) g_menu_router->extra = 0;
    g_menu_router = &t->router;
    t->router.extra = uui_editmenu_item();
    int w, h;
    top_size(t, &w, &h);
    uui_editmenu_open(tg, id, x, y, w, h);
    t->dirty = 1;
}

// The menu changed a widget's text: the app hears it as a typed change,
// so a search field filters and a setting field applies as if typed.
static void menu_changed(struct uapp_top *t, int id) {
    t->dirty = 1;
    if (id) t->ops->tell(t, id, UUI_REASON_KEY, 1);
}

// The editable text at (x, y): a routed widget, else one the app draws
// itself but registered with its focus ring. The router's id, or 0.
static int edit_at(struct uapp_top *t, int x, int y, struct uui_edit_target *tg) {
    int id = 0;
    if (uui_router_edit_at(&t->router, x, y, tg, &id)) return id ? id : -1;
    if (!t->focus) return 0;
    for (int i = 0; i < t->focus->count; i++) {
        const struct uui_focusable *f = &t->focus->items[i];
        if (f->ops && f->ops->edit_target && f->ops->edit_target(f->widget, x, y, tg)) {
            id = uui_router_id_of(&t->router, f->widget);
            return id ? id : -1;
        }
    }
    return 0;
}

// A SECONDARY PRESS OVER EDITABLE TEXT IS THE TOOLKIT'S: it focuses the
// field and arms; the RELEASE opens the menu (uui_menubar_open_at()'s
// rule -- a menu opened on the press would be handed that release).
// Returns 1 when taken, and then the app's on_press/on_release do not
// hear it -- QLineEdit accepts its contextMenuEvent the same way, so a
// window's own right-click menu does not open on top of this one.
static struct uapp_top *g_armed;
static struct uui_edit_target g_armed_tg;
static int g_armed_id;

int top_secondary(struct uapp_top *t, int x, int y, int up) {
    if (!up) {
        g_armed = 0;
        if (uui_editmenu_is_open()) uui_editmenu_close();
        int id = edit_at(t, x, y, &g_armed_tg);
        if (!id) return 0;
        if (t->focus && uui_focus_click(t->focus, x, y)) t->dirty = 1;
        g_armed = t;
        g_armed_id = id > 0 ? id : 0;
        return 1;
    }
    if (g_armed != t) return 0;
    g_armed = 0;
    // Re-asked at the release: the press may have moved focus, and a
    // widget answers again with the state it is in now.
    struct uui_edit_target tg;
    int id = edit_at(t, x, y, &tg);
    if (id) menu_open(t, &tg, id > 0 ? id : 0, x, y);
    else menu_open(t, &g_armed_tg, g_armed_id, x, y);   // released off it: still the menu it asked for
    return 1;
}

// The Menu key, or Shift+F10: the menu at the FOCUSED field's caret.
static int menu_key(struct uapp_top *t) {
    if (!t->focus || t->focus->current < 0) return 0;
    const struct uui_focusable *f = &t->focus->items[t->focus->current];
    struct uui_edit_target tg;
    if (!f->ops || !f->ops->edit_target ||
        !f->ops->edit_target(f->widget, UUI_NOWHERE, UUI_NOWHERE, &tg))
        return 0;
    menu_open(t, &tg, uui_router_id_of(&t->router, f->widget), tg.x, tg.y);
    return 1;
}

// A SHORTCUT SKIPS THE WIDGETS (uui_widget.h): Ctrl+1 goes to the app, and
// a focused field does not type a 1. Then AN OPEN POPUP OUTRANKS the focus
// ring -- it is drawn over everything and is what the user is looking at,
// the keyboard's half of the overlay rule (ui/uui_route.h) -- then the
// ring, then the app, which still hears a key the ring took so it can
// re-read the widget whose value just changed.
void top_key(struct uapp_top *t, const struct win_event *ev) {
    // The edit menu outranks everything while it is up: it is the
    // grabbing popup the compositor sends keys for.
    if (g_menu_router == &t->router && uui_editmenu_is_open()) {
        int tid = 0;
        int r = uui_editmenu_key(ev->a, &tid);
        if (r) {
            t->dirty = 1;
            if (r == 2) menu_changed(t, tid);
            return;
        }
    }
    if ((ev->a == KEY_MENU || (ev->a == KEY_F10 && (ev->mods & KEY_MOD_SHIFT))) && menu_key(t))
        return;
    if (uui_key_is_shortcut(ev->a, ev->mods)) { t->ops->key(t, ev->a, ev->mods); return; }
    if (t->router.count) {
        int changed = 0;
        int id = uui_router_overlay_key(&t->router, ev->a, ev->mods, &changed);
        if (changed) t->dirty = 1;
        if (id) { t->ops->tell(t, id, UUI_REASON_KEY, 0); return; }
    }
    if (t->focus && uui_focus_key(t->focus, ev->a, ev->mods)) {
        t->dirty = 1;
        // AND THE APP IS TOLD, by the same id a click on that widget
        // reports: a value the keyboard changed is as much a change as a
        // clicked one. Not Tab, which moved focus and changed no value.
        if (ev->a != '\t' && t->hears && t->focus->current >= 0) {
            int id = uui_router_id_of(&t->router, t->focus->items[t->focus->current].widget);
            t->ops->tell(t, id, UUI_REASON_KEY, 1);
        }
    }
    t->ops->key(t, ev->a, ev->mods);
}

// A PRIMARY press: the widgets first (one takes the pointer grab), then
// keyboard focus follows the click -- but not for a press on a popup,
// which is its owner's: moving focus there closed a dropdown under the
// press choosing its row.
void top_press(struct uapp_top *t, int x, int y, unsigned kmods) {
    int changed = 0;
    int id = top_routes(t) ? uui_router_press(&t->router, x, y, kmods, &changed) : 0;
    if (changed) t->dirty = 1;
    if (id == UUI_EDITMENU_ID) return;   // a row armed; the release commits
    t->ops->tell(t, id, UUI_REASON_PRESS, 0);
    if (t->focus && !g_press_slot && uui_focus_click(t->focus, x, y)) t->dirty = 1;
}

// With the primary button held this re-hit-tests the press (the GRAB: a
// drag goes to whoever took the press, wherever the cursor is); with none
// it is hover. A hover is not the app's unless it asked (motion_wanted).
void top_motion(struct uapp_top *t, int x, int y, unsigned held, unsigned kmods) {
    if (!top_routes(t)) return;
    int changed = 0;
    int id = uui_router_motion(&t->router, x, y, held, kmods, &changed);
    if (changed) t->dirty = 1;
    if (id == UUI_EDITMENU_ID) return;
    if (motion_wanted(&t->router, id, held)) t->ops->tell(t, id, UUI_REASON_MOTION, 0);
}

void top_release(struct uapp_top *t, int x, int y) {
    if (!top_routes(t)) return;
    int changed = 0;
    int id = uui_router_release(&t->router, x, y, &changed);
    if (changed) t->dirty = 1;
    if (id == UUI_EDITMENU_ID) {
        int tid = 0;
        if (uui_editmenu_take(&tid)) menu_changed(t, tid);
        t->dirty = 1;
        return;
    }
    t->ops->tell(t, id, UUI_REASON_RELEASE, changed);
}

// To the widget UNDER THE CURSOR, not down a fixed chain (ui/uui_route.h).
// The id, or 0 when no widget took it.
int top_wheel(struct uapp_top *t, int notches) {
    if (!top_routes(t)) return 0;
    int changed = 0;
    int id = uui_router_wheel(&t->router, t->mouse_x, t->mouse_y, notches, &changed);
    if (changed) t->dirty = 1;
    if (id == UUI_EDITMENU_ID) return id;   // the menu ate it: nobody else scrolls
    t->ops->tell(t, id, UUI_REASON_WHEEL, 0);
    return id;
}

// IDS ARE UNIQUE, or the lookup above names the wrong widget. Refused
// loudly at startup rather than misrouted later -- a resource compiler's
// duplicate-id warning, made fatal.
int ids_unique(const struct uui_router *r, const char *who) {
    int dup = uui_router_duplicate_id(r);
    if (!dup) return 1;
    ulogf("uapp: BUG: %s declares widget id %d twice -- refusing to start\n",
          who ? who : "an app", dup);
    return 0;
}

// A LONE BUTTON WITH NOBODY TO HEAR IT is a button that silently does
// nothing -- the shape check_key_routing.py catches for keys, checked
// here because only the running app knows which files linked together.
int buttons_heard(const struct uui_router *r, int has_on_action, const char *who) {
    if (has_on_action || !uui_router_has_ops(r, &uui_button_ops)) return 1;
    ulogf("uapp: BUG: %s declares a button and no on_action -- refusing to start\n",
          who ? who : "an app");
    return 0;
}

// Entering containers (`children`), as uapp_log_layout() does: a dialog
// may sit inside a layout.
static int dialog_open_in(struct uui_item *items, int count) {
    for (int i = 0; i < count; i++) {
        struct uui_item *it = &items[i];
        if (!it->ops) continue;
        if (it->ops == &uui_dialog_ops &&
            uui_dialog_is_open((const struct uui_dialog *)it->widget))
            return 1;
        if (it->ops->children) {
            int n = 0;
            struct uui_item *sub = it->ops->children(it->widget, &n);
            if (sub && dialog_open_in(sub, n)) return 1;
        }
    }
    return 0;
}

int uapp_inwindow_question_open(struct uapp *a) {
    const struct uapp_desc *d = a ? a->desc : 0;
    if (!d) return 0;
    if (d->layout) {
        struct uui_item root = { .ops = &uui_layout_ops, .widget = (void *)d->layout };
        if (dialog_open_in(&root, 1)) return 1;
    }
    return d->widgets && dialog_open_in(d->widgets, d->widget_count);
}

int uapp_question_open(struct uapp *a) {
    for (int i = 1; i < WIN_CLIENT_MAX; i++)
        if (g_dlg[i].top.slot && (g_dlg[i].desc.flags & UAPP_WIN_MODAL)) return 1;
    return uapp_inwindow_question_open(a);
}

void dlg_dispatch(struct uapp_window *w, const struct win_event *ev) {
    const struct uapp_window_desc *d = &w->desc;
    switch (ev->type) {
    case WIN_EV_PING:
        wmchan_send(WIN_REQ_PONG, (uint32_t)w->top.slot, (int)ev->a, 0, 0, 0);
        return;
    case WIN_EV_CLOSE:
        if (d->on_close) d->on_close(w);
        else uapp_window_close(w);
        return;
    case WIN_EV_KEY:
        top_key(&w->top, ev);
        return;
    case WIN_EV_MOUSE_DOWN:
        w->top.mouse_x = ev->a;
        w->top.mouse_y = ev->b;
        if (WIN_MOUSE_BUTTONS(ev->mods) & 0x1)
            top_press(&w->top, ev->a, ev->b, WIN_MOUSE_MODS(ev->mods));
        else if (WIN_MOUSE_BUTTONS(ev->mods) & 0x2)
            top_secondary(&w->top, ev->a, ev->b, 0);
        return;
    case WIN_EV_MOUSE_MOVE: {
        if (ev->a < 0 || ev->b < 0) return;   // a leave carries no position
        w->top.mouse_x = ev->a;
        w->top.mouse_y = ev->b;
        if (!top_routes(&w->top)) return;
        top_motion(&w->top, ev->a, ev->b, ev->mods & 0x1, WIN_MOUSE_MODS(ev->mods));
        int want = uui_router_cursor(&w->top.router, ev->a, ev->b);
        if (want != w->top.cursor && want >= 0 && want < WIN_CURSOR_COUNT) {
            w->top.cursor = want;
            wmchan_send(WIN_REQ_CURSOR, (uint32_t)w->top.slot, want, 0, 0, 0);
        }
        return;
    }
    case WIN_EV_MOUSE_UP:
        w->top.mouse_x = ev->a;
        w->top.mouse_y = ev->b;
        if (top_secondary(&w->top, ev->a, ev->b, 1)) return;
        top_release(&w->top, ev->a, ev->b);
        return;
    case WIN_EV_WHEEL:
        top_wheel(&w->top, (int)ev->a);
        return;
    default:
        return;
    }
}

