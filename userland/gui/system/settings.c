// System Settings -- a RING-3 process.
//
// Named for what it shows, and deliberately NOT "Control Panel": that is
// Windows' name, and this shows exactly the SETTINGS registry -- not
// facts, not tunables (docs/settings-and-queries.md's "The vocabulary").
//
// THE THING WORTH KNOWING ABOUT THIS APP: it contains no list of
// settings, no list of categories, no list of pages, and no captions.
// All of it comes from the registry and /etc/settings.d, so a setting
// registered anywhere appears here, on the right page, with a
// description and readable choice names, with no edit to this code.
//
// THE SHAPE: a searchable sidebar of categories and their pages, and the
// page as a column of cards -- one per setting, its control on the right
// (Windows 11, GNOME, Plasma 6). CHANGES ARE STAGED, KDE's model: the
// footer counts what is not applied yet and holds Reset and Apply, and
// leaving a page with changes asks first. docs/decisions/gui.md has why.
//
// This file is the app -- callbacks, navigation, the root layout; the
// rest is userland/settings/ (settings_internal.h names the parts).
#include "settings/settings_internal.h"

struct uui_sidebar g_tree;   // set_registry.c rebuilds its rows

// --- following changes made elsewhere ---------------------------------
//
// A SETTING CHANGED WHILE THIS WINDOW IS OPEN -- by the tray, by `config`,
// by another program -- is re-read and the open page redrawn from it
// (KDE's KConfigWatcher shape). A page with edits pending is left alone
// and marked stale, as is one under an open dialog; it is refreshed once
// those are applied, reset or closed.
static int g_stale;

static struct uui_dialog g_ask;

static int on_tick(struct uapp *a) {
    (void)a;
    if (registry_generation() == g_generation && !g_stale) return 0;
    if (page_dirty() || uui_dialog_is_open(&g_ask) ||
        (g_opts_win && uapp_window_is_open(g_opts_win))) {
        g_stale = 1;
        return 0;
    }
    g_stale = 0;
    int keep_node = uui_sidebar_selected_id(&g_tree);
    reload_settings();
    uui_sidebar_select_id(&g_tree, keep_node);
    if (!g_show_sysinfo && g_page_group >= 0 && g_page_group < g_group_count)
        open_group(g_page_group);
    return 1;
}

// --- layout -----------------------------------------------------------
//
//   +----------------+-------------------------------------------+
//   | [Find a set..] |  Mouse                                    |
//   | INPUT          |  +-------------------------------------+  |
//   |   Mouse        |  | Acceleration           [==o] On     |  |
//   |   Keyboard     |  +-------------------------------------+  |
//   | APPEARANCE     |  | Cursor size   [Normal|Large|Huge]   |  |
//   |   Fonts        |  +-------------------------------------+  |
//   +----------------+-------------------------------------------+
//   | 1 change not applied                      [Reset] [Apply] |
//   +------------------------------------------------------------+
//
// THE PAGE SCROLLS; THE SIDEBAR AND THE FOOTER DO NOT. uui_layout
// overflows rather than shrinking, so anything that must stay reachable
// stays outside the scrolled page (CLAUDE.md). The sidebar scrolls
// itself: one scroll region per page.

static struct uui_textbox g_search;
static struct uui_item ITEMS_LEFT[2];
static struct uui_layout LEFT_LAYOUT;
static struct uui_item ITEMS_BODY[3];
static struct uui_layout BODY_LAYOUT;

static struct uui_label  g_footer;
static char g_footer_text[sizeof g_status];
static struct uui_button g_reset, g_apply;
static struct uui_item ITEMS_FOOTER[3];
static struct uui_layout FOOTER_LAYOUT;

// The sidebar's width is the user's, dragged. Persisted in this app's
// own /etc/<app_id>.conf -- this window's furniture, not a setting.
#define SETTINGS_CONF "/etc/settings.conf"
#define SIDE_SPLIT_DEFAULT 200

static struct uui_splitter g_side_split;
static struct uui_item ITEMS[2];
static struct uui_layout LAYOUT;
// What the router sees: the laid-out window, and the question on top of
// it -- LAST, because the router asks the last item first.
static struct uui_item ROOT[2];

// The ring's two ends: search and sidebar before the page's controls,
// the footer's buttons after them (set_page.c fills the middle).
//
// FOCUS SURVIVES THE REBUILD when its widget is still in the ring: the
// ring is rebuilt on every relayout, and typing into the search box
// relayouts on every key -- a reset ring dropped the second letter. The
// focused WIDGET is taken before FOCUS is rewritten, since the ring's
// index would afterwards name whatever took its place.
static void *g_ring_was;

void focus_ring_open(void) {
    g_ring_was = PAGE_FOCUS.items && PAGE_FOCUS.current >= 0 &&
                 PAGE_FOCUS.current < PAGE_FOCUS.count
                 ? PAGE_FOCUS.items[PAGE_FOCUS.current].widget : 0;
}

void focus_ring_close(void) {
    FOCUS[0] = (struct uui_focusable){ &g_search, &uui_textbox_ops };
    FOCUS[1] = (struct uui_focusable){ &g_tree, &uui_sidebar_ops };
    FOCUS[FOCUS_COUNT++] = (struct uui_focusable){ &g_reset, &uui_button_ops };
    FOCUS[FOCUS_COUNT++] = (struct uui_focusable){ &g_apply, &uui_button_ops };
    uui_focus_init(&PAGE_FOCUS, FOCUS, FOCUS_COUNT);
    for (int i = 0; g_ring_was && i < FOCUS_COUNT; i++)
        if (FOCUS[i].widget == g_ring_was) { uui_focus_set(&PAGE_FOCUS, i); break; }
    g_ring_was = 0;
}

// --- navigation and events -------------------------------------------

static void report_rows(void);

static void navigate(int node_id) {
    g_page_node = node_id;
    // New cards: the previous fit says nothing about them.
    g_prose_fitted = 0;
    g_prose_fit_w = 0;
    if (node_id == NODE_SYSINFO) {
        g_show_sysinfo = 1;
        g_page_group = -1;
        g_slot_count = 0;
        strlcpy(g_page_title_text, "System Information", sizeof g_page_title_text);
        g_page_desc_text[0] = '\0';
        strlcpy(g_status, "About this machine", sizeof g_status);
        relayout_page();
        // Reported like any other page, or a test that opened it has
        // nothing to confirm it by.
        ulogf("settings: page %s slots 0 advanced 0 captions 0 disabled 0\n",
              g_page_title_text);
        return;
    }
    if (node_id >= NODE_GROUP_BASE) open_group(node_id - NODE_GROUP_BASE);
}

// LEAVING A PAGE WITH CHANGES ASKS -- Apply, Discard or stay -- which is
// KDE's System Settings prompt. Silently discarding was the old rule, and
// a change vanishing with only a status line to say so is how a user
// learns not to trust the app. `node` is where to go, or -1 to quit.
enum { ASK_APPLY = 1, ASK_DISCARD, ASK_STAY };
static int g_pending_node = -1, g_pending_quit;
static char g_ask_line[2][SETTING_ABI_LABEL_MAX + 48];
static const char *g_ask_rows[2];

static void ask_leave(struct uapp *a, int node) {
    g_pending_node = node;
    g_pending_quit = node < 0;
    int n = page_changes();
    snprintf(g_ask_line[0], sizeof g_ask_line[0], "%d change%s on %s %s not applied.",
             n, n == 1 ? "" : "s", g_page_title_text, n == 1 ? "is" : "are");
    snprintf(g_ask_line[1], sizeof g_ask_line[1], "Apply %s before leaving?",
             n == 1 ? "it" : "them");
    g_ask_rows[0] = g_ask_line[0];
    g_ask_rows[1] = g_ask_line[1];
    static const struct uui_dialog_button btns[] = {
        { "Apply", ASK_APPLY }, { "Discard", ASK_DISCARD }, { "Cancel", ASK_STAY },
    };
    uui_dialog_set_bounds(&g_ask, 0, 0, uapp_width(a), uapp_height(a));
    uui_dialog_open(&g_ask, "Unapplied changes", g_ask_rows, 2, btns, 3, 0, ASK_STAY);
    ulogf("settings: ask leave %s changes %d\n", g_pending_quit ? "quit" : "page", n);
}

static void leave(struct uapp *a) {
    if (g_pending_quit) { uapp_quit(a, 0); return; }
    uui_sidebar_select_id(&g_tree, g_pending_node);
    navigate(g_pending_node);
}

static int on_close(struct uapp *a) {
    if (!page_dirty()) return 1;
    if (!uui_dialog_is_open(&g_ask)) ask_leave(a, -1);
    uapp_redraw(a);
    return 0;
}

// The search box changed: filter the sidebar. If the open page no longer
// matches and nothing is staged on it, the first match opens instead.
static void search_changed(void) {
    const char *q = uui_textbox_text(&g_search);
    if (!strcmp(q, g_filter)) return;
    strlcpy(g_filter, q, sizeof g_filter);
    rebuild_sidebar();
    int sel = uui_sidebar_selected_id(&g_tree);
    if (sel >= 0 && sel != g_page_node && !page_dirty()) navigate(sel);
    else if (g_page_node >= 0) uui_sidebar_select_id(&g_tree, g_page_node);
    ulogf("settings: filter \"%s\" rows %d\n", g_filter, g_node_count);
    report_rows();
}

// The sidebar's width, re-derived from the divider whenever the body
// could have moved. The track leaves out the layout's own margins and
// the two gaps around the band, which belong to neither child.
static void apply_split(struct uapp *a) {
    int m = uui_layout_margin(&BODY_LAYOUT), g = uui_layout_gap(&BODY_LAYOUT);
    int lo = BODY_LAYOUT.x + m;
    int hi = lo + BODY_LAYOUT.w - 2 * m - 2 * g;
    uui_splitter_set_track(&g_side_split, lo, hi,
                            ugfx_char_w() * 10, ugfx_char_w() * 24);
    ITEMS_BODY[0].main_size = uui_splitter_before(&g_side_split);
    uui_layout_run(&LAYOUT, 0, 0, uapp_width(a), uapp_height(a));
    uui_dialog_set_bounds(&g_ask, 0, 0, uapp_width(a), uapp_height(a));
}

static void save_split(void) {
    char v[12];
    snprintf(v, sizeof v, "%d", uui_splitter_frac(&g_side_split));
    uconf_set(SETTINGS_CONF, "sidebar", v);
}

static void on_widget(struct uapp *a, int id, int reason) {
    // COMMIT ON RELEASE (docs/gui-guidelines.md), and a KEY is a deliberate
    // act too; a motion never stages -- hovering across a list once wrote
    // /etc on every pixel. The divider is the exception: it follows the
    // drag live, and only its /etc write waits for the release.
    if (id != ID_SIDE_SPLIT &&
        reason != UUI_REASON_RELEASE && reason != UUI_REASON_KEY) return;

    if (id >= ID_CONTROL_BASE && id < ID_CONTROL_BASE + PAGE_MAX) {
        if (control_changed(id - ID_CONTROL_BASE)) relayout_page();
        uapp_redraw(a);
        return;
    }

    switch (id) {
    case ID_SIDE_SPLIT:
        apply_split(a);
        if (reason == UUI_REASON_RELEASE) save_split();
        break;
    case ID_SEARCH:
        search_changed();
        break;
    case ID_TREE: {
        // The sidebar hands back the APP's id, not a row.
        int to = uui_sidebar_selected_id(&g_tree);
        if (to < 0 || to == g_page_node) break;
        if (page_dirty()) {
            // The selection stays on the page being left until answered.
            uui_sidebar_select_id(&g_tree, g_page_node);
            ask_leave(a, to);
            break;
        }
        navigate(to);
        break;
    }
    case ID_ASK: {
        int code = uui_dialog_take_code(&g_ask);   // -1 on every press
        if (code == ASK_APPLY) {
            if (apply_page()) leave(a);
        } else if (code == ASK_DISCARD) {
            leave(a);
        }
        break;
    }
    case ID_RESET:
        if (page_dirty() && g_page_group >= 0) {
            open_group(g_page_group);
            strlcpy(g_status, "Changes discarded", sizeof g_status);
            ulogf("settings: reset\n");
        }
        break;
    case ID_APPLY:
        apply_page();
        break;
    case ID_ADVANCED:
        g_show_advanced = g_advanced_cb.checked;
        if (g_page_group >= 0) open_group(g_page_group);
        break;
    case ID_OPTS:
        open_options_dialog(a);
        return;
    case ID_TEST: {
        // The saver AS STAGED -- previewing what is on disk would answer a
        // question nobody asked.
        const char *name = 0;
        for (int i = 0; i < g_slot_count && !name; i++) {
            struct slot *sl = &g_slot[i];
            if (sl->setting >= 0 && strcmp(g_name[sl->setting], OWNER_SAVER) == 0)
                name = staged_value(sl);
        }
        if (!name || !name[0]) break;
        // THE STAGED OPTIONS ARE WRITTEN FIRST -- the one place this page
        // commits unasked. A saver reads its file at startup, so a Test
        // that skipped this would preview the options you did NOT pick.
        int wrote = 0;
        for (int i = 0; i < g_slot_count; i++) {
            struct slot *sl = &g_slot[i];
            const struct usaver_opt *o = sl->setting >= 0 ? opt_of(sl->setting) : 0;
            if (!o || sl->staged == sl->baseline) continue;
            if (uconf_set(g_file[sl->setting], o->key, staged_value(sl))) {
                sl->baseline = sl->staged;
                sl->row.changed = 0;
                wrote++;
            }
        }
        char path[128];
        snprintf(path, sizeof path, "%s/%s", SCREENSAVER_DIR, name);
        int pid = sys_spawn(path, 0, -1);
        if (pid > 0 && wrote)
            snprintf(g_status, sizeof g_status, "Testing %s -- %d option(s) saved", name, wrote);
        else if (pid > 0) snprintf(g_status, sizeof g_status, "Testing %s", name);
        else snprintf(g_status, sizeof g_status, "Could not start %s", name);
        ulogf("settings: screensaver test %s pid %d options %d\n", name, pid, wrote);
        break;
    }
    default:
        break;
    }
    uapp_redraw(a);
}

// Painted OVER the widgets -- the scroll view fills its rect, so the two
// things this app draws itself (System Information, the empty-registry
// notice) would otherwise be covered. Positioned under the title the
// LAYOUT placed, never from the page's own top.
static void on_draw_over(struct uapp *a, struct uapp_draw *d) {
    struct ugfx_surface *s = uapp_surface(d);
    (void)a;
    int pad = ugfx_char_w();
    int top = g_page_title.y + g_page_title.h + pad / 2;
    if (top < PAGE_SCROLL.y + pad) top = PAGE_SCROLL.y + pad; // before the first layout
    int avail = PAGE_SCROLL.y + PAGE_SCROLL.h - top - pad;

    if (g_show_sysinfo) {
        draw_sysinfo(s, PAGE_SCROLL.x + pad, top, PAGE_SCROLL.w - 2 * pad, avail);
        return;
    }
    if (g_setting_count == 0)
        ugfx_draw_string_clipped(s, PAGE_SCROLL.x + pad, top, PAGE_SCROLL.w - 2 * pad,
                                  "No settings are registered.", UTHEME_TEXT, UTHEME_PANEL_BG);
}

// The footer says what is pending, or what last happened; Reset and Apply
// are live only while something is staged.
static void update_footer(void) {
    int n = page_changes();
    if (n) snprintf(g_footer_text, sizeof g_footer_text, "%d change%s not applied",
                    n, n == 1 ? "" : "s");
    else strlcpy(g_footer_text, g_status, sizeof g_footer_text);
    g_reset.disabled = g_apply.disabled = !n;
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    (void)d;
    update_footer();
    uapp_log_layout(a, "settings");   // search, tree, split, page, reset, apply
    // Fit the cards at the width they were laid out at, again whenever
    // that width CHANGES (a resize) -- and not on a steady frame, which
    // would relayout under the user and lose the scroll position.
    int fit_w = g_slot_count > 0 ? g_slot[0].row.w : g_page_desc.w;
    //
    // ONE MORE FRAME AFTER ANY FIT, changed or not: this frame's layout
    // runs AFTER on_draw and can move the width again (the page gaining a
    // scrollbar), and a fit that asked for no frame would never see it.
    if (fit_w > 0 && (!g_prose_fitted || fit_w != g_prose_fit_w)) {
        g_prose_fitted = 1;
        g_prose_fit_w = fit_w;
        refit_prose();
        uapp_redraw(a);
    }
    // NOTHING IS PAINTED HERE: on_draw runs BEFORE the widgets, which
    // would cover it. What must appear on top is on_draw_over's.

    // TAB SCROLLS THE PAGE TO WHAT IT FOCUSED -- the whole card, not just
    // its control -- once per focus change, so the wheel is still free
    // to scroll away from it afterwards.
    static int last_focus = -1;
    if (PAGE_FOCUS.current != last_focus) {
        last_focus = PAGE_FOCUS.current;
        const struct uui_focusable *f = last_focus >= FOCUS_LEAD &&
            last_focus < PAGE_FOCUS.count ? &PAGE_FOCUS.items[last_focus] : 0;
        int x = 0, y = 0, w = 0, h = 0;
        if (f && f->ops->bounds) f->ops->bounds(f->widget, &x, &y, &w, &h);
        for (int i = 0; f && i < g_slot_count; i++)
            if (g_slot[i].row.control.widget == f->widget) {
                y = g_slot[i].row.y;
                h = g_slot[i].row.h;
            }
        if (f && f->widget != &g_reset && f->widget != &g_apply && h > 0 &&
            uui_scrollview_reveal(&PAGE_SCROLL, y, h))
            uapp_redraw(a);
    }

    // Where the sidebar is scrolled to, on a CHANGE -- nothing else says.
    static int last_top = -1;
    if (g_tree.top != last_top) {
        last_top = g_tree.top;
        ulogf("settings: sidebar top %d visible %d rows %d\n", g_tree.top,
              uui_sidebar_visible_rows(&g_tree), g_node_count);
    }

    // WHERE EACH CONTROL ENDED UP, whenever one MOVES -- a page change
    // and a scroll alike. A control below the fold is unreachable to a
    // test that does not know where it went.
    int moved = g_slot_count != g_last_reported_count;
    for (int i = 0; i < g_slot_count && !moved; i++) {
        int x, y, w, hh;
        slot_rect(&g_slot[i], &x, &y, &w, &hh);
        if (y != g_last_y[i]) moved = 1;
    }
    if (!moved) return;
    g_last_reported_count = g_slot_count;
    for (int i = 0; i < g_slot_count; i++) {
        struct slot *sl = &g_slot[i];
        const char *nm = sl->setting >= 0 ? g_name[sl->setting] : "-";
        int x, y, w, hh;
        slot_rect(sl, &x, &y, &w, &hh);
        g_last_y[i] = y;
        // The description's ROW COUNT: the only observable difference
        // between wrapped and truncated prose.
        ulogf("settings: prose %d %s rows %d width %d text %d\n", i, nm,
              sl->row.desc_rows, uui_setting_row_text_w(&sl->row),
              sl->setting >= 0 ? ugfx_text_width(g_desc[sl->setting]) : 0);
        ulogf("settings: card %d %s %d %d %d %d stacked %d\n", i, nm,
              sl->row.x, sl->row.y, sl->row.w, sl->row.h,
              sl->row.stacked || sl->row.stacked_auto);
        ulogf("settings: control %d %s %d %d %d %d rows %d kind %s\n", i, nm,
              x, y, w, hh, sl->choice_count, slot_kind_name(sl));
        ulogf("settings: enabled %d %s %d\n", i, nm, slot_disabled(sl) ? 0 : 1);
        // What is stored and what is shown, side by side; `shown` LAST,
        // since it may contain spaces.
        if (sl->kind != CTRL_SPIN && sl->kind != CTRL_TEXT && sl->kind != CTRL_KEYCAP &&
            sl->staged >= 0 && sl->staged < sl->choice_count)
            ulogf("settings: choice %d %s raw %s shown %s\n", i, nm,
                  sl->choice_raw[sl->staged], sl->choice[sl->staged]);
    }
    ulogf("settings: test_button %d %d %d %d shown %d\n",
          g_test_btn.x, g_test_btn.y, g_test_btn.w, g_test_btn.h, g_test_has);
    ulogf("settings: opts_button %d %d %d %d opts %d\n",
          g_opts_btn.x, g_opts_btn.y, g_opts_btn.w, g_opts_btn.h, g_saver.opt_count);
    ulogf("settings: advanced_toggle %d %d %d %d shown %d\n",
          g_advanced_cb.x, g_advanced_cb.y, g_advanced_cb.w, g_advanced_cb.h,
          g_advanced_has);
}

// The divider is a FRACTION, so re-derive the pin whenever the room
// changes -- a font change too, since every natural size is the cell's.
static void on_resize(struct uapp *a, int w, int h) {
    (void)w; (void)h;
    apply_split(a);
}

static void on_font(struct uapp *a) { apply_split(a); }

// Every visible sidebar row, with the y a click should land on --
// reported by the app, not re-derived in Python. `depth` is 0 for a
// heading and 1 for a page; the label is LAST, since it may hold spaces.
static void report_rows(void) {
    int rh = uui_sidebar_row_h(&g_tree);
    for (int r = 0; r < g_node_count; r++)
        ulogf("settings: row %d id %d y %d depth %d %s\n",
              r, g_nodes[r].id, g_tree.y + r * rh + rh / 2,
              g_nodes[r].kind == UUI_SIDEBAR_ITEM ? 1 : 0,
              g_nodes[r].label ? g_nodes[r].label : "-");
}

static void on_open(struct uapp *a) {
    g_app = a;
    apply_split(a);
    // Facts for tools/: a layout is a number a test can read.
    ulogf("settings: settings %d\n", g_setting_count);
    ulogf("settings: categories %d\n", g_cat_count);
    ulogf("settings: groups %d\n", g_group_count);
    ulogf("settings: nodes %d\n", g_node_count);
    uapp_logf_layout("settings: layout button apply %d %d %d %d\n",
                     g_apply.x, g_apply.y, g_apply.w, g_apply.h);
    uapp_logf_layout("settings: layout button reset %d %d %d %d\n",
                     g_reset.x, g_reset.y, g_reset.w, g_reset.h);
    report_rows();
}

static void on_size(int *w, int *h) {
    // One cell's margin, half at the window edge and half inside the page.
    LAYOUT.margin      = ugfx_char_w() / 2;
    PAGE_LAYOUT.margin = ugfx_char_w() - ugfx_char_w() / 2;

    // THE REGISTRY IS READ HERE: on_size is the first hook with a font and
    // it runs BEFORE the layout, which sizes each widget from its natural
    // size. Guarded, because it runs again on every resize.
    if (!g_loaded) {
        g_loaded = 1;
        reload_settings();
        // The sidebar has already chosen its first ITEM (row 0 is a
        // heading, which names no page).
        if (g_node_count > 0) navigate(uui_sidebar_selected_id(&g_tree));
    }
    // Font-derived, so HERE rather than in main(): ugfx_char_h() is 0
    // until uapp_run() has fetched the font.
    g_advanced_cb.size = ugfx_char_h();
    g_page_title.font = ugfx_font_session(UGFX_FONT_BOLD);

    // Wide enough for a card's text beside its control, tall enough that
    // the densest page opens without scrolling past its first cards.
    *w = ugfx_char_w() * 64;
    *h = ugfx_char_h() * 44;
}

int main(void) {
    uui_sidebar_init(&g_tree, 0, 0, 0, 0, g_nodes, 0);
    g_tree.bg = UTHEME_PANEL_BG;
    g_tree.fg = UTHEME_TEXT;

    uui_textbox_init(&g_search, "");
    g_search.placeholder = "Find a setting";

    uui_label_init(&g_page_title, g_page_title_text);
    uui_label_init(&g_page_desc, g_page_desc_text);
    uui_label_set_wrap(&g_page_desc, 1); // grown per text by fit_rows()
    for (int i = 0; i < PAGE_MAX; i++) {
        g_slot[i].setting = -1;
        g_slot[i].radio.cols = 1;
        g_slot[i].radio.selected = -1;
        g_slot[i].radio.hovered = -1;
        g_slot[i].radio.bg = UUI_COLOR_UNSET;
        g_slot[i].radio.fg = UTHEME_TEXT;
        uui_dropdown_init(&g_slot[i].combo, 0, 0, 0, 0, 0, 0);
        uui_slider_init(&g_slot[i].slider, 0, 0);
        g_slot[i].slider.bg = UUI_COLOR_UNSET;
        g_slot[i].slider.fg = UTHEME_TEXT;
    }
    uui_button_init(&g_test_btn, 0, 0, 0, 0, "Test", UTHEME_BUTTON_BG, UTHEME_TEXT, 1);
    uui_button_init(&g_opts_btn, 0, 0, 0, 0, "Settings...", UTHEME_BUTTON_BG, UTHEME_TEXT, 1);
    uui_button_init(&g_opts_ok, 0, 0, 0, 0, "OK", UTHEME_BUTTON_BG, UTHEME_TEXT, 1);
    uui_button_init(&g_opts_cancel, 0, 0, 0, 0, "Cancel", UTHEME_BUTTON_BG, UTHEME_TEXT, 1);
    uui_checkbox_init(&g_advanced_cb, 0, 0, 0, "Show advanced settings",
                       UTHEME_PANEL_BG, UTHEME_TEXT);
    uui_dialog_init(&g_ask);

    // Apply is the PRIMARY action, so it wears the accent.
    uui_button_init(&g_reset, 0, 0, 0, 0, "Reset", UTHEME_BUTTON_BG, UTHEME_TEXT, 1);
    uui_button_init(&g_apply, 0, 0, 0, 0, "Apply", UTHEME_ACCENT, UTHEME_ACCENT_TEXT, 1);
    uui_label_init(&g_footer, g_footer_text);

    PAGE_LAYOUT = (struct uui_layout){ .dir = UUI_COLUMN, .items = PAGE, .count = 0 };
    relayout_page();
    uui_scrollview_init(&PAGE_SCROLL, &PAGE_LAYOUT);
    uui_scrollview_set_preferred_rows(&PAGE_SCROLL, 14);

    ITEMS_LEFT[0] = (struct uui_item){ .ops = &uui_textbox_ops, .widget = &g_search,
                                       .id = ID_SEARCH, .flags = UUI_FILL_W, .name = "search" };
    ITEMS_LEFT[1] = (struct uui_item){ .ops = &uui_sidebar_ops, .widget = &g_tree,
                                       .id = ID_TREE, .flags = UUI_FILL_W | UUI_FILL_H,
                                       .name = "tree" };
    LEFT_LAYOUT = (struct uui_layout){ .dir = UUI_COLUMN, .items = ITEMS_LEFT, .count = 2 };

    ITEMS_BODY[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &LEFT_LAYOUT,
                                       .flags = UUI_FILL_H };
    ITEMS_BODY[1] = (struct uui_item){ .ops = &uui_splitter_ops, .widget = &g_side_split,
                                       .id = ID_SIDE_SPLIT, .flags = UUI_FILL_H, .name = "split" };
    ITEMS_BODY[2] = (struct uui_item){ .ops = &uui_scrollview_ops, .widget = &PAGE_SCROLL,
                                       .id = ID_PAGE, .name = "page",
                                       .flags = UUI_FILL_W | UUI_FILL_H };
    BODY_LAYOUT = (struct uui_layout){ .dir = UUI_ROW, .items = ITEMS_BODY, .count = 3 };

    ITEMS_FOOTER[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_footer,
                                         .id = ID_FOOTER, .flags = UUI_FILL_W | UUI_FILL_H,
                                         .name = "footer" };
    ITEMS_FOOTER[1] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_reset,
                                         .id = ID_RESET, .name = "reset" };
    ITEMS_FOOTER[2] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_apply,
                                         .id = ID_APPLY, .name = "apply" };
    FOOTER_LAYOUT = (struct uui_layout){ .dir = UUI_ROW, .items = ITEMS_FOOTER, .count = 3 };

    ITEMS[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &BODY_LAYOUT,
                                  .id = ID_BODY, .flags = UUI_FILL_W | UUI_FILL_H };
    ITEMS[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &FOOTER_LAYOUT,
                                  .flags = UUI_FILL_W };
    uui_splitter_init(&g_side_split, 1, SIDE_SPLIT_DEFAULT);
    {
        char v[12];
        if (uconf_get(SETTINGS_CONF, "sidebar", v, sizeof v))
            uui_splitter_set_frac(&g_side_split, atoi(v));
    }
    LAYOUT = (struct uui_layout){ .dir = UUI_COLUMN, .items = ITEMS,
                                  .count = (int)(sizeof ITEMS / sizeof ITEMS[0]) };
    ROOT[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &LAYOUT };
    ROOT[1] = (struct uui_item){ .ops = &uui_dialog_ops, .widget = &g_ask,
                                 .id = ID_ASK, .name = "ask" };

    struct uapp_desc desc = {
        .title = "System Settings",
        // One window: two would each believe their own cached registry.
        .app_id = "settings",
        .layout = &LAYOUT,
        .flags = UAPP_RESIZABLE | UAPP_SINGLE_INSTANCE,
        .widgets = ROOT,
        .widget_count = (int)(sizeof ROOT / sizeof ROOT[0]),
        .on_widget = on_widget,
        // The whole window is one ring (focus_ring_close()).
        .focus = &PAGE_FOCUS,
        .on_draw = on_draw,
        .on_draw_over = on_draw_over,
        .on_open = on_open,
        .on_size = on_size,
        .on_resize = on_resize,
        .on_font = on_font,
        .on_close = on_close,
        .on_tick = on_tick,
        .tick_ms = 500,
    };
    return uapp_run(&desc);
}
