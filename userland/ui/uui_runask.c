// uui_runask -- see uui_runask.h. One card, two hosts: a modal window
// over an app (the File Manager) or an app's own main window
// (/bin/wm/system/runask, for the desktop and `open`).
#include "ui/uui_runask.h"
#include "ui/ugfx.h"
#include "ui/umonofont.h"
#include "ui/utheme.h"
#include "ui/ulog.h"
#include "ui/uui_button.h"
#include "ui/uui_checkbox.h"
#include "ui/uui_focus.h"
#include "ui/uui_image.h"
#include "ui/uui_label.h"
#include "ui/uui_layout.h"
#include "ui/uui_route.h"
#include "lib/icon_cache.h"
#include "kpath.h"
#include "rt/sys.h"
#include <stdio.h>
#include <string.h>

enum { ID_TERMINAL = 1, ID_RUN, ID_EDIT, ID_CANCEL, ID_ALLOW, ID_ALWAYS };

static char g_path[256];
static struct ulaunch_info g_info;
static uui_runask_fn g_done;
static void *g_ctx;
static struct uapp_window *g_win;   // the modal host, when it is one
static struct uapp *g_app;          // the app host, when it is one
static int g_key_took;              // a focused button took this Enter

static char g_title[300], g_what[128], g_sub[300], g_warn[160], g_always_text[40];
static struct uui_image g_icon;
static struct uui_label g_l_what, g_l_sub, g_l_warn, g_spacer, g_gap;
static struct uui_checkbox g_always;
static struct uui_button g_b_edit, g_b_term, g_b_run, g_b_cancel, g_b_allow;
static struct umonofont g_mono;

static struct uui_item g_text_items[3], g_hero_items[2], g_btn_items[5], g_root_items[5];
static struct uui_layout g_text_l, g_hero_l, g_btn_l, g_root_l;
static struct uui_focusable g_focusables[6];
static struct uui_focus g_focus;

static int is_x_face(void) { return !g_info.runnable; }

// --- the script's first lines ----------------------------------------------
//
// A field's ground and border with the lines in the terminal's face: a
// label's `bg` only blends its glyphs, it fills nothing.
#define PEEK_PAD 6
static struct { int x, y, w, h, lines; } g_peek;

static const struct ugfx_font *peek_font(void) { return umonofont_get(&g_mono, 0, "runask"); }

static void peek_natural(const void *w, int *ow, int *oh) {
    (void)w;
    const struct ugfx_font *was = ugfx_set_font(peek_font());
    *ow = ugfx_char_advance('n') * 20;
    *oh = g_peek.lines * ugfx_char_h() + 2 * PEEK_PAD;
    ugfx_set_font(was);
}
static void peek_geometry(void *w, int x, int y, int ww, int hh) {
    (void)w;
    g_peek.x = x; g_peek.y = y; g_peek.w = ww; g_peek.h = hh;
}
static void peek_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    (void)w;
    *x = g_peek.x; *y = g_peek.y; *ow = g_peek.w; *oh = g_peek.h;
}
static void peek_draw(struct ugfx_surface *s, const void *w) {
    (void)w;
    ugfx_fill_rect(s, g_peek.x, g_peek.y, g_peek.w, g_peek.h, UTHEME_WHITE);
    ugfx_draw_rect(s, g_peek.x, g_peek.y, g_peek.w, g_peek.h, UTHEME_BORDER);
    const struct ugfx_font *was = ugfx_set_font(peek_font());
    int ch = ugfx_char_h();
    for (int i = 0; i < g_peek.lines; i++)
        ugfx_draw_string_clipped(s, g_peek.x + PEEK_PAD + 2, g_peek.y + PEEK_PAD + i * ch,
                                 g_peek.w - 2 * PEEK_PAD - 2, g_info.head[i], UTHEME_TEXT, UTHEME_WHITE);
    ugfx_set_font(was);
}
static const struct uui_widget_ops g_peek_ops = {
    .natural_size = peek_natural,
    .set_geometry = peek_geometry,
    .bounds = peek_bounds,
    .draw = peek_draw,
};

static void finish(int act) {
    if (act != ULAUNCH_ASK && g_always.checked && !is_x_face())
        if (!ulaunch_set_policy(g_info.kind, act)) ulog("runask: could not save the choice\n");
    uui_runask_fn done = g_done;
    g_done = 0;
    if (g_win) { uapp_window_close(g_win); g_win = 0; }
    ulogf("runask: %s -> %d\n", g_path, act);
    if (done) done(g_ctx, g_path, g_info.kind, act);
}

static void act_code(int code) {
    switch (code) {
    case ID_TERMINAL: finish(ULAUNCH_TERMINAL); break;
    case ID_RUN:      finish(ULAUNCH_RUN); break;
    case ID_EDIT:     finish(ULAUNCH_EDIT); break;
    case ID_CANCEL:   finish(ULAUNCH_ASK); break;
    case ID_ALLOW:
        if (!ulaunch_allow(g_path)) {
            // REFUSED, and said on the card: FAT32 keeps no permissions.
            snprintf(g_warn, sizeof g_warn, "Its permissions could not be changed here.");
            ulogf("runask: allow %s FAILED\n", g_path);
            if (g_win) uapp_window_redraw(g_win);
            if (g_app) uapp_redraw(g_app);
            return;
        }
        g_info.runnable = 1;
        finish(ULAUNCH_TERMINAL);
        break;
    default:
        break;
    }
}

// The default act: what Enter does when no button has the focus.
static int default_code(void) {
    if (is_x_face()) return ID_ALLOW;
    return g_b_term.disabled ? ID_CANCEL : ID_TERMINAL;
}

static void key(int k) {
    if (k == 0x1B) { finish(ULAUNCH_ASK); return; }
    if ((k == '\n' || k == '\r') && !g_key_took) act_code(default_code());
    g_key_took = 0;
}

static void widget(int id, int reason) {
    if (reason == UUI_REASON_KEY && id != ID_ALWAYS) {
        g_key_took = 1;
        act_code(id);
    }
}

// --- the window host ------------------------------------------------------

static void w_action(struct uapp_window *w, int code) { (void)w; act_code(code); }
static void w_widget(struct uapp_window *w, int id, int reason) {
    widget(id, reason);
    if (g_win) uapp_window_redraw(w);
}
static void w_key(struct uapp_window *w, int k, unsigned mods) {
    (void)w; (void)mods;
    key(k);
}
static void w_close(struct uapp_window *w) { (void)w; finish(ULAUNCH_ASK); }

// --- the app host ---------------------------------------------------------

static void a_action(struct uapp *a, int code) {
    act_code(code);
    if (!g_done) uapp_quit(a, 0);
}
static void a_widget(struct uapp *a, int id, int reason) {
    widget(id, reason);
    if (!g_done) uapp_quit(a, 0);
    else uapp_redraw(a);
}
static void a_key(struct uapp *a, int k, unsigned mods) {
    (void)mods;
    key(k);
    if (!g_done) uapp_quit(a, 0);
}
static int a_close(struct uapp *a) {
    if (g_done) finish(ULAUNCH_ASK);
    uapp_quit(a, 0);
    return 1;
}

// --- the content ----------------------------------------------------------

static void describe(void) {
    const char *name = k_path_basename(g_path);
    char dir[256];
    snprintf(dir, sizeof dir, "%s", g_path);
    char *slash = strrchr(dir, '/');
    if (slash) *(slash == dir ? slash + 1 : slash) = '\0';
    struct sys_stat st;
    unsigned long long size = sys_stat(g_path, &st) == 0 ? st.size : 0;
    char sz[24];
    if (size < 1024) snprintf(sz, sizeof sz, "%llu B", size);
    else snprintf(sz, sizeof sz, "%llu.%llu KB", size / 1024, (size % 1024) * 10 / 1024);

    snprintf(g_title, sizeof g_title, "Run %s?", name);
    g_warn[0] = '\0';
    if (g_info.kind == ULAUNCH_SCRIPT) {
        snprintf(g_what, sizeof g_what, is_x_face() ? "%s is not marked as a program" : "%s is a script", name);
        snprintf(g_sub, sizeof g_sub, "Run by %s \xb7 %s \xb7 %s", g_info.interp, sz, dir);
        snprintf(g_always_text, sizeof g_always_text, "Always do this for scripts");
    } else {
        snprintf(g_what, sizeof g_what, is_x_face() ? "%s is not marked as a program" : "%s is a program", name);
        snprintf(g_sub, sizeof g_sub, "%s \xb7 %s", sz, dir);
        snprintf(g_always_text, sizeof g_always_text, "Always do this for programs");
    }
    if (is_x_face())
        snprintf(g_warn, sizeof g_warn, "Its permissions do not allow running it. Allowing it lets the shell run it too.");
    else if (!g_info.interp_found)
        snprintf(g_warn, sizeof g_warn, "It cannot run: %s is not installed.", g_info.interp);
    else
        snprintf(g_warn, sizeof g_warn, "A %s can change or delete files. Run it only if you trust it.",
                 g_info.kind == ULAUNCH_SCRIPT ? "script" : "program");
}

static void build(void) {
    describe();
    int ch = ugfx_char_h();
    uint32_t bg = UTHEME_PANEL_BG, fg = UTHEME_TEXT;

    uui_image_init(&g_icon, icon_get(g_info.kind == ULAUNCH_SCRIPT ? "file-text" : "terminal", ch * 2 + 8),
                   UIMG_FIT_CONTAIN);
    uui_label_init(&g_l_what, g_what);
    g_l_what.font = ugfx_font_session(UGFX_FONT_BOLD);
    uui_label_init(&g_l_sub, g_sub);
    uui_label_init(&g_l_warn, g_warn);
    g_l_warn.wrap = 1;
    g_l_warn.rows = 2;
    uui_label_init(&g_spacer, "");
    uui_label_init(&g_gap, "");
    g_text_items[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_l_what, .flags = UUI_FILL_W, .name = "what" };
    g_text_items[1] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_l_sub, .flags = UUI_FILL_W, .name = "sub" };
    g_text_items[2] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_l_warn, .flags = UUI_FILL_W, .name = "warn" };
    g_text_l = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_text_items, .count = 3, .gap = 4 };
    g_hero_items[0] = (struct uui_item){ .ops = &uui_image_ops, .widget = &g_icon, .name = "icon",
                                         .main_size = ch * 2 + 8 };
    g_hero_items[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_text_l, .flags = UUI_FILL_W };
    g_hero_l = (struct uui_layout){ .dir = UUI_ROW, .items = g_hero_items, .count = 2, .gap = 12 };

    int nhead = is_x_face() ? 0 : g_info.head_lines;
    g_peek.lines = nhead;

    uui_checkbox_init(&g_always, 0, 0, 0, g_always_text, bg, fg);
    g_always.checked = 0;

    uui_button_init(&g_b_edit, 0, 0, 0, 0, "Open in Notepad", UTHEME_BUTTON_BG, fg, ID_EDIT);
    uui_button_init(&g_b_term, 0, 0, 0, 0, "Run in Terminal", UTHEME_ACCENT, UTHEME_ACCENT_TEXT, ID_TERMINAL);
    uui_button_init(&g_b_run, 0, 0, 0, 0, "Run", UTHEME_BUTTON_BG, fg, ID_RUN);
    uui_button_init(&g_b_cancel, 0, 0, 0, 0, "Cancel", UTHEME_BUTTON_BG, fg, ID_CANCEL);
    uui_button_init(&g_b_allow, 0, 0, 0, 0, "Allow running, and run", UTHEME_ACCENT, UTHEME_ACCENT_TEXT, ID_ALLOW);
    g_b_term.disabled = g_b_run.disabled = !g_info.interp_found;

    int nb = 0, nf = 0;
    if (g_info.kind == ULAUNCH_SCRIPT) {
        g_btn_items[nb++] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_b_edit, .id = ID_EDIT, .name = "edit" };
    }
    g_btn_items[nb++] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_spacer, .flags = UUI_FILL_W };
    if (is_x_face()) {
        g_btn_items[nb++] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_b_allow, .id = ID_ALLOW, .name = "allow" };
        g_focusables[nf++] = (struct uui_focusable){ &g_b_allow, &uui_button_ops };
    } else {
        g_btn_items[nb++] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_b_term, .id = ID_TERMINAL, .name = "terminal" };
        g_btn_items[nb++] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_b_run, .id = ID_RUN, .name = "run" };
        g_focusables[nf++] = (struct uui_focusable){ &g_b_term, &uui_button_ops };
        g_focusables[nf++] = (struct uui_focusable){ &g_b_run, &uui_button_ops };
    }
    g_btn_items[nb++] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_b_cancel, .id = ID_CANCEL, .name = "cancel" };
    int cancel_at = nf;
    g_focusables[nf++] = (struct uui_focusable){ &g_b_cancel, &uui_button_ops };
    if (g_info.kind == ULAUNCH_SCRIPT)
        g_focusables[nf++] = (struct uui_focusable){ &g_b_edit, &uui_button_ops };
    g_btn_l = (struct uui_layout){ .dir = UUI_ROW, .items = g_btn_items, .count = nb, .gap = 8 };

    int nr = 0;
    g_root_items[nr++] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_hero_l, .flags = UUI_FILL_W };
    if (nhead)
        g_root_items[nr++] = (struct uui_item){ .ops = &g_peek_ops, .widget = &g_peek, .flags = UUI_FILL_W, .name = "head" };
    if (!is_x_face()) {
        g_root_items[nr++] = (struct uui_item){ .ops = &uui_checkbox_ops, .widget = &g_always, .id = ID_ALWAYS, .name = "always" };
        g_focusables[nf++] = (struct uui_focusable){ &g_always, &uui_checkbox_ops };
    }
    g_root_items[nr++] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_gap, .flags = UUI_FILL_H };
    g_root_items[nr++] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_btn_l, .flags = UUI_FILL_W };
    g_root_l = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_root_items, .count = nr, .margin = 14, .gap = 12 };

    uui_focus_init(&g_focus, g_focusables, nf);
    // The DEFAULT has the focus, so Enter is its answer -- unless it is
    // disabled, and then Cancel is.
    uui_focus_set(&g_focus, g_b_term.disabled && !is_x_face() ? cancel_at : 0);
}

static int root_count(void) { return g_root_l.count; }

// The width a sentence wants; the height the layout says it needs at it.
static void size_of(int *w, int *h) {
    int nw;
    *w = ugfx_char_advance('n') * 62;
    uui_layout_natural_size(&g_root_l, &nw, h);
}

static void take(const char *path, const struct ulaunch_info *info, uui_runask_fn done, void *ctx) {
    snprintf(g_path, sizeof g_path, "%s", path);
    g_info = *info;
    g_done = done;
    g_ctx = ctx;
    g_key_took = 0;
    build();
}

int uui_runask_is_open(void) { return g_win != 0; }

int uui_runask_open(struct uapp *a, const char *path, const struct ulaunch_info *info,
                    uui_runask_fn done, void *ctx) {
    if (g_win || !a || !path || !info) return 0;
    take(path, info, done, ctx);
    int w, h;
    size_of(&w, &h);
    struct uapp_window_desc d = {
        .title = g_title,
        .w = w, .h = h,
        .flags = UAPP_WIN_MODAL,
        .widgets = g_root_items,
        .widget_count = root_count(),
        .layout = &g_root_l,
        .focus = &g_focus,
        .on_widget = w_widget,
        .on_action = w_action,
        .on_key = w_key,
        .on_close = w_close,
        .log_prefix = "runask",
    };
    g_win = uapp_window_open(a, &d);
    if (!g_win) g_done = 0;
    return g_win != 0;
}

static void a_size(int *w, int *h) { size_of(w, h); }
static void a_open(struct uapp *a) { g_app = a; }
static void a_draw(struct uapp *a, struct uapp_draw *d) {
    (void)d;
    uapp_log_layout(a, "runask");
}

void uui_runask_app_desc(struct uapp_desc *d, const char *path, const struct ulaunch_info *info,
                         uui_runask_fn done, void *ctx) {
    take(path, info, done, ctx);
    d->title = g_title;
    d->app_id = "runask";
    d->on_size = a_size;
    d->layout = &g_root_l;
    d->widgets = g_root_items;
    d->widget_count = root_count();
    d->focus = &g_focus;
    d->on_open = a_open;
    d->on_draw = a_draw;
    d->on_widget = a_widget;
    d->on_action = a_action;
    d->on_key = a_key;
    d->on_close = a_close;
}

int uui_runask_start(struct uapp *a, const char *path, int kind, int act) {
    char *argv[5];
    if (!ulaunch_argv(path, kind, act, argv)) return -1;
    int pid = sys_spawn_argv(argv[0], argv);
    if (pid > 0 && a) uapp_track_child(a, pid);
    return pid;
}
