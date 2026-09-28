// System Settings -- a RING-3 process.
//
// Named for what it shows, and deliberately NOT "Control Panel": that is
// Windows' name, and this shows exactly the SETTINGS registry -- not
// facts, not tunables (docs/settings-and-queries.md's "The vocabulary").
//
// THE THING WORTH KNOWING ABOUT THIS FILE: it contains no list of
// settings, no list of categories, no list of pages, and no captions.
// All of it comes from the kernel -- the registry supplies the settings
// and their grouping, and /etc/settings.d supplies the prose. So a
// setting registered anywhere appears here, on the right page, with a
// description and readable choice names, with no edit to this file.
// The kernel-space version this replaced had one hand-written applet per
// setting, which is a second source of truth that drifts.
//
// THE SHAPE is KDE System Settings': category -> group in a tree on the
// left, and the group's whole PAGE on the right -- several related
// controls together, not one control per page. GNOME, Windows Settings
// and macOS Ventura all converged on sidebar-plus-pane.
//
// CHANGES ARE STAGED. Selecting a choice does not apply it: Apply
// commits, OK commits and closes, Cancel discards. Windows' and KDE's
// model. (This app used to be instant-apply, GNOME's model, which is why
// the row you arrived on is marked "current" -- with staging, that is
// what makes an accidental click visible.)
#include "settings/settings_internal.h"

struct uui_sidebar g_tree;   // set_registry.c rebuilds its rows
static struct uui_button    g_btn[3];
static struct uui_button_group g_buttons;
static struct uui_statusbar g_status_bar;

// --- following changes made elsewhere ---------------------------------
//
// A SETTING CHANGED WHILE THIS WINDOW IS OPEN -- by the tray, by `config`,
// by another program -- is re-read and the open page redrawn from it,
// which is KDE's KConfigWatcher shape for a window that stages edits.
// Settings used to read the registry at start and after its own Apply
// only, so a page kept showing a value that had long since changed.
//
// A PAGE WITH EDITS PENDING IS LEFT ALONE: rebuilding it would throw the
// user's staged values away. It is marked stale and refreshed as soon as
// those edits are applied or cancelled. The same holds while an options
// dialog is open over it.
static int g_stale;


static int on_tick(struct uapp *a) {
    (void)a;
    if (registry_generation() == g_generation && !g_stale) return 0;
    if (page_dirty() || (g_opts_win && uapp_window_is_open(g_opts_win))) {
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
//   +----------------+---------------------------+
//   | Appearance     |  Mouse                    |
//   |   Fonts        |  How the pointer looks... |
//   | Input          |                           |
//   |   Mouse        |  Pointer speed            |
//   |   Keyboard     |  How far the pointer...   |
//   | System Info    |   ( ) Slow  (o) Normal    |
//   +----------------+---------------------------+
//   |                        [OK] [Apply] [Cancel]|
//   | Mouse                                       |
//   +---------------------------------------------+
//
// THE PAGE SCROLLS; THE SIDEBAR, THE BUTTONS AND THE STATUS BAR DO NOT.
// uui_layout does not shrink children below their natural size -- it
// OVERFLOWS -- so anything inside the scrolled page that must stay
// reachable would be pushed off the bottom instead. The chrome stays
// outside it, per CLAUDE.md.
//
// The sidebar is not in the scroll view either: it scrolls itself, and
// one scroll region per page is the rule.


static struct uui_item ITEMS_BODY[3];
static struct uui_layout BODY_LAYOUT;

// The sidebar's width is the user's, dragged. Persisted in this app's
// own /etc/<app_id>.conf -- the desktop's and File Manager's convention
// -- and NOT as a registered setting: it is this window's furniture,
// and this is the app that would then have to list it among the
// settings.
#define SETTINGS_CONF "/etc/settings.conf"
#define SIDE_SPLIT_DEFAULT 200

static struct uui_splitter g_side_split;
static struct uui_item ITEMS[3];
static struct uui_layout LAYOUT;


// --- navigation and events -------------------------------------------

static void navigate(int node_id) {
    g_page_node = node_id;
    // A new page means new labels, so the previous fit says nothing
    // about them even at the same width -- see g_prose_fitted.
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
        // REPORTED LIKE ANY OTHER PAGE. This one carries no settings, so
        // it emits no control lines either -- a test that opened it had
        // nothing to confirm it by, and measured the previous page's
        // pixels instead. Same shape as open_group()'s line below.
        ulogf("settings: page %s slots 0 advanced 0 captions 0 disabled 0\n",
              g_page_title_text);
        return;
    }
    if (node_id >= NODE_GROUP_BASE) {
        open_group(node_id - NODE_GROUP_BASE);
        return;
    }
    if (node_id >= NODE_CATEGORY_BASE) {
        // A CATEGORY opens its first page rather than showing an empty
        // pane -- clicking a heading and getting nothing reads as a
        // broken app, and every desktop settings tree does this.
        int c = node_id - NODE_CATEGORY_BASE;
        for (int g = 0; g < g_group_count; g++)
            if (strcmp(g_group_cat[g], g_cat[c]) == 0) { open_group(g); return; }
    }
}

// The sidebar's width, re-derived from the divider whenever the body
// could have moved. The track leaves out the layout's own margins and
// the two gaps it puts around the band -- those pixels belong to
// neither child, and counting them would offset every drag by one gap.
static void apply_split(struct uapp *a) {
    int m = uui_layout_margin(&BODY_LAYOUT), g = uui_layout_gap(&BODY_LAYOUT);
    int lo = BODY_LAYOUT.x + m;
    int hi = lo + BODY_LAYOUT.w - 2 * m - 2 * g;
    uui_splitter_set_track(&g_side_split, lo, hi,
                            ugfx_char_w() * 10, ugfx_char_w() * 24);
    ITEMS_BODY[0].main_size = uui_splitter_before(&g_side_split);
    uui_layout_run(&LAYOUT, 0, 0, uapp_width(a), uapp_height(a));
}

static void save_split(void) {
    char v[12];
    snprintf(v, sizeof v, "%d", uui_splitter_frac(&g_side_split));
    uconf_set(SETTINGS_CONF, "sidebar", v);
}

static void on_widget(struct uapp *a, int id, int reason) {
    // COMMIT ON RELEASE, docs/gui-guidelines.md's rule for every control
    // here -- and this file used to discard `reason` entirely. The
    // router delivers press, MOTION, release and wheel, so acting on all
    // of them meant merely moving the pointer across the choice list
    // applied a setting: each motion wrote /etc, bumped fs_generation()
    // and made the desktop re-read every .desktop file. Hovering froze
    // the machine for seconds.
    // A KEY IS A DELIBERATE ACT AND STAGES; a motion is not. What the
    // rule above is really about is not acting on a pointer merely
    // crossing a control -- typing into a focused dropdown, or arrowing
    // through an open one, is the user choosing a value, and it would
    // otherwise change on screen and be silently dropped by Apply.
    //
    // **THE DIVIDER IS THE ONE EXCEPTION, and it has to be exempted
    // HERE or its own "live" comment below is a lie** -- which it was:
    // the layout re-ran only on release, so the columns jumped at the
    // end of a drag instead of following the handle. Every real toolkit
    // resizes panes during the drag (Qt's QSplitter `opaqueResize`,
    // GtkPaned), and the reason motion is dangerous in this app does
    // not apply to it: relaying out is free, and the /etc write it
    // would be dangerous to repeat still waits for the release.
    if (id != ID_SIDE_SPLIT &&
        reason != UUI_REASON_RELEASE && reason != UUI_REASON_KEY) return;

    if (id >= ID_CONTROL_BASE && id < ID_CONTROL_BASE + PAGE_MAX) {
        if (control_changed(id - ID_CONTROL_BASE)) relayout_page();
        uapp_redraw(a);
        return;
    }

    switch (id) {
    case ID_SIDE_SPLIT:
        // Live: the layout re-runs on every motion, so the sidebar
        // follows the handle rather than jumping on release.
        apply_split(a);
        if (reason == UUI_REASON_RELEASE) save_split();
        uapp_redraw(a);
        return;
    case ID_TREE: {
        // The tree hands back the APP's id, not a row -- rows move as
        // categories collapse, ids do not.
        if (uui_sidebar_selected_id(&g_tree) == g_page_node) break;
        if (page_dirty()) {
            // SAID, not silently dropped. Discarding is the safe choice
            // (Cancel's behaviour), but a change vanishing with no word
            // is how a user learns not to trust the app.
            strlcpy(g_status, "Unapplied changes were discarded", sizeof g_status);
        }
        navigate(uui_sidebar_selected_id(&g_tree));
        break;
    }
    case ID_ADVANCED:
        g_show_advanced = g_advanced_cb.checked;
        if (g_page_group >= 0) open_group(g_page_group);
        break;
    case ID_OPTS:
        open_options_dialog(a);
        return;

    case ID_TEST: {
        // THE SAVER AS IT IS CONFIGURED RIGHT NOW, staged value and all
        // -- previewing what is on disk rather than what is on screen
        // would answer a question nobody asked.
        const char *name = 0;
        for (int i = 0; i < g_slot_count && !name; i++) {
            struct slot *sl = &g_slot[i];
            if (sl->setting >= 0 &&
                strcmp(g_name[sl->setting], "desktop.screensaver") == 0)
                name = staged_value(sl);
        }
        if (!name || !name[0]) break;
        // THE STAGED OPTIONS ARE WRITTEN FIRST, and that is the one
        // place this page commits without being told to. A saver is a
        // separate process that reads a file at startup: there is no
        // channel for an unwritten value, so a Test that skipped this
        // would preview the options you did NOT pick. Only this saver's
        // own options are written -- the settings above them still wait
        // for Apply.
        int wrote = 0;
        for (int i = 0; i < g_slot_count; i++) {
            struct slot *sl = &g_slot[i];
            const struct usaver_opt *o = sl->setting >= 0 ? opt_of(sl->setting) : 0;
            if (!o || sl->staged == sl->baseline) continue;
            if (uconf_set(g_file[sl->setting], o->key, staged_value(sl))) {
                sl->baseline = sl->staged;
                wrote++;
            }
        }
        char path[128];
        snprintf(path, sizeof path, "%s/%s", SCREENSAVER_DIR, name);
        int pid = sys_spawn(path, 0, -1);
        if (pid > 0 && wrote)
            snprintf(g_status, sizeof g_status,
                     "Testing %s -- %d option(s) saved", name, wrote);
        else if (pid > 0) snprintf(g_status, sizeof g_status, "Testing %s", name);
        else snprintf(g_status, sizeof g_status, "Could not start %s", name);
        ulogf("settings: screensaver test %s pid %d options %d\n",
              name, pid, wrote);
        break;
    }
    case ID_BUTTONS: {
        // The router names the WIDGET, and the group is one widget
        // holding all three -- so which one committed is collected from
        // the group. 0 means a press that was dragged off.
        int code = uui_button_group_take_activated(&g_buttons);
        if (code == BTN_APPLY) {
            apply_page();
        } else if (code == BTN_OK) {
            if (apply_page()) uapp_quit(a, 0);
        } else if (code == BTN_CANCEL) {
            uapp_quit(a, 0);
        }
        break;
    }
    default:
        break;
    }
    uapp_redraw(a);
}

// Painted OVER the widgets, which is the only way anything reaches the
// page area: uui_scrollview fills its rect, so the two things this app
// draws itself -- the System Information page and the empty-registry
// notice -- would otherwise be covered the moment they were drawn. See
// uapp.c's note on the ordering and why on_draw_over exists.
static void on_draw_over(struct uapp *a, struct uapp_draw *d) {
    struct ugfx_surface *s = uapp_surface(d);
    (void)a;
    int pad = ugfx_char_w();
    // BELOW THE TITLE, and the position is ASKED OF THE LAYOUT rather
    // than computed from the page rect. The title and description are
    // real widgets at the top of the page (relayout_page() adds them
    // first, on every page including this one), so drawing from the
    // page's own top lands on top of them -- which is exactly what the
    // first version did, printing the version string through the words
    // "System Information". `g_page_desc` is the lower of the two and
    // is present-but-empty here, so its bottom edge is the first free
    // row whether or not a description was set.
    int top = g_page_desc.y + g_page_desc.h;
    if (top < PAGE_SCROLL.y + pad) top = PAGE_SCROLL.y + pad; // before the first layout
    int avail = PAGE_SCROLL.y + PAGE_SCROLL.h - top - pad;

    if (g_show_sysinfo) {
        draw_sysinfo(s, PAGE_SCROLL.x + pad, top,
                     PAGE_SCROLL.w - 2 * pad, avail);
        return;
    }
    if (g_setting_count == 0)
        ugfx_draw_string_clipped(s, PAGE_SCROLL.x + pad, top,
                                  PAGE_SCROLL.w - 2 * pad,
                                  "No settings are registered.",
                                  UTHEME_TEXT, UTHEME_PANEL_BG);
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    uapp_log_layout(a, "settings");   // tree, split (+frac), page, by name
    // WHICH LABEL'S WIDTH DECIDES, and when to do this again.
    //
    // It used to be "once per page, as soon as g_page_desc has a width",
    // and both halves were wrong. The page description is laid out
    // before the slots' explanations, so its width could be real while
    // theirs were still 0 -- and fitting at width 0 reserves one row and
    // then LOCKS it, which is a description ellipsised for the life of
    // the page. And a window RESIZE changes every width with nothing
    // asking for a re-fit, so widening the window made the text no
    // longer wrap and narrowing it clipped.
    //
    // Watching the width the fit was DONE at fixes both, and keeps the
    // property the once-per-page rule was protecting: on a steady frame
    // the width is unchanged, so nothing relayouts and the scroll
    // position stays put. (Re-fitting every frame was the first version
    // and reset the scroll under the user, so a long page could not be
    // scrolled at all.)
    int fit_w = g_slot_count > 0 ? g_slot[0].explain.w : g_page_desc.w;
    if (fit_w > 0 && (!g_prose_fitted || fit_w != g_prose_fit_w)) {
        g_prose_fitted = 1;
        g_prose_fit_w = fit_w;
        if (refit_prose()) uapp_redraw(a);
    }
    struct ugfx_surface *s = uapp_surface(d);
    (void)a;
    // THE PAGE AREA IS THE SCROLL VIEW'S RECT, asked of the widget
    // rather than recomputed from the window size: the layout owns where
    // things ended up, and a second calculation here would be a second
    // answer.
    (void)s;
    // NOTHING IS PAINTED HERE, and the reason is the opposite of what
    // this comment used to claim. It said "on_draw runs AFTER the
    // widgets", so painting here was safe; uapp.c says the order is
    // "clear, then the APP's own painting, then the widgets, then
    // overlays" -- on_draw runs FIRST, deliberately, so that an app
    // whose first line clears the surface can only ever wipe its own
    // backdrop.
    //
    // That wrong comment cost the System Information page: it drew its
    // text here, the scroll view then painted its background over the
    // whole page area, and the page came up EMPTY with the title and
    // status bar still correct -- so it looked like a data problem
    // rather than a paint-order one. Anything that must appear ON TOP
    // of a widget goes in on_draw_over(), below.

    // WHERE THE SIDEBAR IS SCROLLED TO, reported on a CHANGE. It is
    // not derivable from anything else the app logs -- the row dump is
    // taken once, at the top -- so a test asking "did the wheel reach
    // it?" would otherwise have to read pixels.
    static int last_top = -1;
    if (g_tree.top != last_top) {
        last_top = g_tree.top;
        ulogf("settings: sidebar top %d visible %d rows %d\n", g_tree.top,
              uui_sidebar_visible_rows(&g_tree), g_node_count);
    }

    // WHERE EACH CONTROL ENDED UP, reported whenever it MOVES -- which
    // covers a page change and a scroll with one rule. Geometry does not
    // exist until the layout has run, and on_draw is the first hook that
    // is reliably after it.
    int moved = g_slot_count != g_last_reported_count;
    for (int i = 0; i < g_slot_count && !moved; i++) {
        struct slot *sl = &g_slot[i];
        int x, y, w, hh;
        slot_rect(sl, &x, &y, &w, &hh);
        if (y != g_last_y[i]) moved = 1;
    }
    if (moved) {
        g_last_reported_count = g_slot_count;
        for (int i = 0; i < g_slot_count; i++) {
            struct slot *sl = &g_slot[i];
            int x, y, w, hh;
            slot_rect(sl, &x, &y, &w, &hh);
            g_last_y[i] = y;
            // The description's ROW COUNT, beside the control's
            // geometry and for the same reason: it is the only
            // observable difference between a wrapped explanation and a
            // truncated one, and settings_test asserts that at least
            // one description on a page actually took two rows.
            ulogf("settings: prose %d %s rows %d width %d text %d\n", i,
                  sl->setting >= 0 ? g_name[sl->setting] : "-",
                  sl->explain.rows, sl->explain.w,
                  sl->setting >= 0 ? ugfx_text_width(g_desc[sl->setting]) : 0);
            ulogf("settings: control %d %s %d %d %d %d rows %d kind %s\n", i,
                  sl->setting >= 0 ? g_name[sl->setting] : "-", x, y, w, hh,
                  sl->choice_count, slot_kind_name(sl));
            // READ FROM THE CONTROL THAT IS SHOWING. This asked the
            // radio whatever kind was on screen, which happened to be
            // right only because set_slot_enabled() sets all of them
            // together -- a fact one edit away from being false.
            int shown_off = slot_disabled(sl);
            ulogf("settings: enabled %d %s %d\n", i,
                  sl->setting >= 0 ? g_name[sl->setting] : "-",
                  shown_off ? 0 : 1);
            // WHAT IS STORED AND WHAT IS SHOWN, side by side. They are
            // different strings for a setting whose choices carry
            // display names ("losangeles" / "Los Angeles"), and a
            // screendump cannot tell a missing display name from a
            // value that happens to look like one. `shown` goes LAST
            // because it contains spaces.
            if (sl->kind != CTRL_SPIN && sl->kind != CTRL_TEXT &&
                sl->staged >= 0 && sl->staged < sl->choice_count) {
                ulogf("settings: choice %d %s raw %s shown %s\n", i,
                      sl->setting >= 0 ? g_name[sl->setting] : "-",
                      sl->choice_raw[sl->staged], sl->choice[sl->staged]);
            }
        }
        // The Test button's rect, on the same terms as the toggle below
        // it: a test aims at what the app reports, never at arithmetic
        // of its own (docs/gui-guidelines.md).
        ulogf("settings: test_button %d %d %d %d shown %d\n",
              g_test_btn.x, g_test_btn.y, g_test_btn.w, g_test_btn.h, g_test_has);
        // The options button, on the same terms: a test clicks what the
        // app reports, and `opts` is how it learns whether this page
        // has one at all.
        ulogf("settings: opts_button %d %d %d %d opts %d\n",
              g_opts_btn.x, g_opts_btn.y, g_opts_btn.w, g_opts_btn.h,
              g_saver.opt_count);
        ulogf("settings: advanced_toggle %d %d %d %d shown %d\n",
              g_advanced_cb.x, g_advanced_cb.y, g_advanced_cb.w, g_advanced_cb.h,
              g_advanced_has);
    }
}

// The divider is a FRACTION, so the sidebar keeps its share of a window
// that got wider -- which means re-deriving the pin whenever the room
// changes. A font change changes it too: every natural size is measured
// from the cell.
static void on_resize(struct uapp *a, int w, int h) {
    (void)w; (void)h;
    apply_split(a);
}

static void on_font(struct uapp *a) { apply_split(a); }

static void on_open(struct uapp *a) {
    g_app = a;
    apply_split(a);
    // The lines tools/ asserts on. Kept in the app rather than derived
    // from a screenshot because a layout is a fact, and a number a test
    // can read beats a picture it has to interpret.
    ulogf("settings: settings %d\n", g_setting_count);
    ulogf("settings: categories %d\n", g_cat_count);
    ulogf("settings: groups %d\n", g_group_count);
    ulogf("settings: nodes %d\n", g_node_count);
    // The button group holds its buttons' geometry, not its own -- so
    // report the first button's, which is what a test clicks anyway.
    uapp_logf_layout("settings: layout buttons %d %d %d %d\n",
          g_btn[0].x, g_btn[0].y, g_btn[0].w, g_btn[0].h);
    // And each by its label, since a group's members have no names of
    // their own and a test that means Apply must not click OK.
    static const char *const BTN_LABELS[] = { "ok", "apply", "cancel" };
    for (int b = 0; b < 3; b++)
        uapp_logf_layout("settings: layout button %s %d %d %d %d\n", BTN_LABELS[b],
                         g_btn[b].x, g_btn[b].y, g_btn[b].w, g_btn[b].h);
    // Every visible sidebar row, with the y a click should land on --
    // reported by the app rather than re-derived in Python, for the
    // reason DebugConsole.menu_row() exists.
    int rh = uui_sidebar_row_h(&g_tree);
    for (int r = 0; r < g_node_count; r++) {
        // A FLAT LIST: the row at screen position r IS row r, because a
        // sidebar hides nothing (a tree needed an indirection here only
        // because collapsing could).
        //
        // `depth` is still reported, as 0 for a heading and 1 for an
        // item, because that is what the structure looks like and what
        // tools/settings_test.py has always parsed. The KIND is the
        // thing that actually decides behaviour now, so it is named too.
        // GRAMMAR UNCHANGED, and the label stays LAST. `depth` is 0 for
        // a heading and 1 for an item, which is what the structure looks
        // like and what tools/settings_test.py parses; inserting a field
        // before the label instead swallowed it into the label, because
        // a label may contain spaces and is therefore captured as the
        // rest of the line.
        ulogf("settings: row %d id %d y %d depth %d %s\n",
              r, g_nodes[r].id, g_tree.y + r * rh + rh / 2,
              // **DEPTH IS ABOUT INDENT, NOT ABOUT SELECTABILITY.** A
              // collapsed category (UUI_SIDEBAR_TOP) is a top-level row
              // that happens to be a destination, so it reports 0 like
              // the heading it replaced -- reporting 1 would tell a
              // test it is a page of whatever came before it, which is
              // exactly what it is not.
              g_nodes[r].kind == UUI_SIDEBAR_ITEM ? 1 : 0,
              g_nodes[r].kind == UUI_SIDEBAR_SEP ? "-" :
              g_nodes[r].label ? g_nodes[r].label : "-");
    }
    // NO per-slot dump here: on_open runs ONCE, so it would describe the
    // first page forever and a tool reading it while looking at another
    // page gets a confident wrong answer. The `control` lines in
    // on_draw are reported per page change, where the geometry also
    // exists.
}

static void on_size(int *w, int *h) {
    // **ONE CELL'S MARGIN, SHARED BETWEEN THE WINDOW EDGE AND THE PAGE.**
    // The page is in a scroll view, and a scroll view's content used to
    // pay the full default margin on top of the window's own -- two
    // whole character cells before the first control. Spending none
    // inside instead put the page flush against the view's edge. Half
    // each: closer to the frame, still not touching the panel. The same
    // split as Task Manager's, for the same reason.
    // Font-derived and re-run on every resize, so it reflows with
    // `fontsize` (docs/gui-guidelines.md).
    LAYOUT.margin      = ugfx_char_w() / 2;
    PAGE_LAYOUT.margin = ugfx_char_w() - ugfx_char_w() / 2;

    // THE REGISTRY IS READ HERE, not in on_open. on_size is the first
    // hook with a font, and it runs BEFORE the layout, which sizes each
    // widget from its natural_size -- so loading in on_open would lay
    // the page out around empty widgets and then fill them.
    //
    // Guarded, because on_size runs again on every resize and a reload
    // there would throw away the user's staged changes mid-drag.
    if (!g_loaded) {
        g_loaded = 1;
        reload_settings();
        // THE FIRST ITEM, NOT ROW 0. With a sidebar, row 0 is a category
        // HEADING -- not selectable, and its id names no page. Setting
        // `selected = 0` by hand would put the widget in a state it will
        // not draw and navigate to nothing. uui_sidebar_set_rows() has
        // already chosen the first item; ask it what that was.
        if (g_node_count > 0)
            navigate(uui_sidebar_selected_id(&g_tree));
    }
    // FONT-DERIVED, and sized HERE rather than in main(): ugfx_char_w()
    // is 0 until uapp_run() has fetched the font, so a checkbox sized
    // there comes out a zero-pixel box -- drawing as nothing and
    // hit-testing as nothing, which looks exactly like a dead control.
    // That is the same trap the buttons below carry a note about, and it
    // caught this one too.
    g_advanced_cb.size = ugfx_char_h();

    // EACH SETTING'S NAME IN BOLD, so a page of four settings reads as
    // four blocks rather than eight interchangeable lines of text. The
    // caption and its explanation were the same weight, and with a
    // description under every one the page had no visual structure --
    // the same hierarchy the sidebar's headings give the navigation,
    // applied to the page.
    //
    // Bold rather than a larger size or a rule: everything is laid out
    // on ONE line pitch, so this changes the letterforms and moves
    // nothing, where a bigger caption would reflow the page.
    //
    // HERE AND NOT IN main(), for the reason the comment above gives
    // about ugfx_char_h(): nothing font-related is valid until uapp_run()
    // has fetched the font. The handle would in fact survive it (it is a
    // pointer to a struct filled in later), but a rule with one silent
    // exception is worse than no exception -- and on_size runs again
    // after a font change, so this re-attaches for free.
    for (int i = 0; i < PAGE_MAX; i++)
        g_slot[i].caption.font = ugfx_font_session(UGFX_FONT_BOLD);
    // The page's own title too, or the hierarchy comes out INVERTED: a
    // regular-weight heading sitting above four bold ones reads as the
    // least important thing on the page.
    g_page_title.font = ugfx_font_session(UGFX_FONT_BOLD);

    int bw = ugfx_char_w() * 9, bh = ugfx_char_h() + 10;
    for (int i = 0; i < 3; i++) {
        g_btn[i].x = i * (bw + 6);
        g_btn[i].y = 0;
        g_btn[i].w = bw;
        g_btn[i].h = bh;
    }
    // FONT-DERIVED, and TALL ENOUGH FOR THE DENSEST PAGE. 26 rows left
    // the Mouse page's speed control below the fold of its own scroll
    // view -- reachable by scrolling, but a control you have to go
    // looking for on the default window size is a control most people
    // will not find (docs/conventions/gui.md). The page still scrolls,
    // because a page CAN always outgrow any window; this is about where
    // the default sits, not about removing the scroll view.
    // WIDE ENOUGH THAT THE SIDEBAR'S GUTTER DOES NOT COME OUT OF THE
    // PAGE. The two share one row, and the sidebar takes its natural
    // width -- so when it grew an icon column, the page silently lost
    // exactly that much and a description that had fitted on one line
    // started wrapping onto two. The window is the thing that should
    // absorb a wider sidebar, not the content beside it.
    *w = ugfx_char_w() * 82;
    *h = ugfx_char_h() * 32;
}

int main(void) {
    uui_sidebar_init(&g_tree, 0, 0, 0, 0, g_nodes, 0);
    g_tree.bg = UTHEME_PANEL_BG;
    g_tree.fg = UTHEME_TEXT;

    uui_label_init(&g_page_title, g_page_title_text);
    uui_label_init(&g_page_desc, g_page_desc_text);
    // THE PROSE WRAPS; THE HEADINGS DO NOT. A page description and a
    // setting's explanation are sentences written by whoever registered
    // the setting, and there is no length they are promised to fit --
    // so they were being clipped mid-word and the only way to read one
    // was to widen the window. A title and a caption are short by
    // construction and wrapping one would look broken.
    //
    // Two rows, not one: `rows` is what a wrapping label reserves and
    // cannot exceed (see uui_label.h), and two lines of this page's
    // width holds every description the kernel currently registers with
    // room to spare. A longer one ellipsises rather than vanishing.
    uui_label_set_wrap(&g_page_desc, 1); // grown per text by fit_rows()
    for (int i = 0; i < PAGE_MAX; i++) {
        g_slot[i].setting = -1;
        uui_label_init(&g_slot[i].caption, 0);
        // (The bold weight is attached in on_size, not here -- see there.)
        uui_label_init(&g_slot[i].explain, 0);
        // One row until a description arrives that needs two -- the row
        // count is recomputed per text in fill_slot(), see there.
        uui_label_set_wrap(&g_slot[i].explain, 1);
        // The separator between one setting and the next: one empty
        // row, always. See the field's comment for why the space goes
        // here rather than where it used to be.
        uui_label_init(&g_slot[i].spacer, "");
        uui_label_set_wrap(&g_slot[i].spacer, 1);
        g_slot[i].radio.cols = 1;
        g_slot[i].radio.selected = -1;
        g_slot[i].radio.hovered = -1;
        g_slot[i].radio.bg = UTHEME_PANEL_BG;
        g_slot[i].radio.fg = UTHEME_TEXT;
        uui_dropdown_init(&g_slot[i].combo, 0, 0, 0, 0, 0, 0);
        uui_slider_init(&g_slot[i].slider, 0, 0);
        g_slot[i].slider.bg = UTHEME_PANEL_BG;
        g_slot[i].slider.fg = UTHEME_TEXT;
    }
    uui_button_init(&g_test_btn, 0, 0, 0, 0, "Test", UTHEME_BUTTON_BG, UTHEME_TEXT, 1);
    uui_button_init(&g_opts_btn, 0, 0, 0, 0, "Settings...", UTHEME_BUTTON_BG, UTHEME_TEXT, 1);
    uui_button_init(&g_opts_ok, 0, 0, 0, 0, "OK", UTHEME_BUTTON_BG, UTHEME_TEXT, 1);
    uui_button_init(&g_opts_cancel, 0, 0, 0, 0, "Cancel", UTHEME_BUTTON_BG, UTHEME_TEXT, 1);
    uui_checkbox_init(&g_advanced_cb, 0, 0, 0, "Show advanced settings",
                       UTHEME_PANEL_BG, UTHEME_TEXT);

    uui_button_init(&g_btn[0], 0, 0, 0, 0, "OK", UTHEME_BUTTON_BG, UTHEME_TEXT, BTN_OK);
    uui_button_init(&g_btn[1], 0, 0, 0, 0, "Apply", UTHEME_BUTTON_BG, UTHEME_TEXT, BTN_APPLY);
    uui_button_init(&g_btn[2], 0, 0, 0, 0, "Cancel", UTHEME_BUTTON_BG, UTHEME_TEXT, BTN_CANCEL);
    uui_button_group_init(&g_buttons, g_btn, 3);

    uui_statusbar_init(&g_status_bar);
    g_status_bar.panes[0].text = g_status;
    g_status_bar.panes[0].chars = 0;
    g_status_bar.count = 1;

    PAGE_LAYOUT = (struct uui_layout){ .dir = UUI_COLUMN, .items = PAGE,
                                        .count = 0, .margin = 0 };
    relayout_page();
    uui_scrollview_init(&PAGE_SCROLL, &PAGE_LAYOUT);
    uui_scrollview_set_preferred_rows(&PAGE_SCROLL, 14);

    ITEMS_BODY[0] = (struct uui_item){ .ops = &uui_sidebar_ops, .widget = &g_tree,
                                        .id = ID_TREE, .flags = UUI_FILL_H, .name = "tree" };
    ITEMS_BODY[1] = (struct uui_item){ .ops = &uui_splitter_ops, .widget = &g_side_split,
                                        .id = ID_SIDE_SPLIT, .flags = UUI_FILL_H, .name = "split" };
    ITEMS_BODY[2] = (struct uui_item){ .ops = &uui_scrollview_ops, .widget = &PAGE_SCROLL,
                                        .id = ID_PAGE, .name = "page",
                                        .flags = UUI_FILL_W | UUI_FILL_H };
    BODY_LAYOUT = (struct uui_layout){ .dir = UUI_ROW, .items = ITEMS_BODY,
                                        .count = 3, .margin = 0 };

    ITEMS[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &BODY_LAYOUT,
                                   .id = ID_BODY, .flags = UUI_FILL_W | UUI_FILL_H };
    ITEMS[1] = (struct uui_item){ .ops = &uui_button_group_ops, .widget = &g_buttons,
                                   .id = ID_BUTTONS };
    ITEMS[2] = (struct uui_item){ .ops = &uui_statusbar_ops, .widget = &g_status_bar,
                                   .id = ID_STATUS, .flags = UUI_FILL_W };
    uui_splitter_init(&g_side_split, 1, SIDE_SPLIT_DEFAULT);
    {
        char v[12];
        if (uconf_get(SETTINGS_CONF, "sidebar", v, sizeof v))
            uui_splitter_set_frac(&g_side_split, atoi(v));
    }

    LAYOUT = (struct uui_layout){ .dir = UUI_COLUMN, .items = ITEMS,
                                  .count = (int)(sizeof ITEMS / sizeof ITEMS[0]) };

    struct uapp_desc desc = {
        .title = "System Settings",
        // One is enough, and two would show the same registry while each
        // believed its own cached copy -- a second window is the fastest
        // way to see a stale value.
        .app_id = "settings",
        .layout = &LAYOUT,
        .flags = UAPP_RESIZABLE | UAPP_SINGLE_INSTANCE,
        .widgets = ITEMS,
        .widget_count = (int)(sizeof ITEMS / sizeof ITEMS[0]),
        .on_widget = on_widget,
        // Tab moves between the page's controls; the toolkit owns the
        // ring (docs/conventions/gui.md). relayout_page() refills it.
        .focus = &PAGE_FOCUS,
        .on_draw = on_draw,
        .on_draw_over = on_draw_over,
        .on_open = on_open,
        .on_size = on_size,
        .on_resize = on_resize,
        .on_font = on_font,
        .on_tick = on_tick,
        .tick_ms = 500,
    };
    return uapp_run(&desc);
}
