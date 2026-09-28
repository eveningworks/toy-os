// System Settings: one page -- its slots, their controls, staging and Apply.
#include "settings/settings_internal.h"

char g_status[160];
int  g_loaded;
int  g_show_sysinfo;

// Cleared on every PAGE CHANGE, set once that page's prose labels have
// been re-fitted to the widths the layout gave them.
//
// ONCE PER PAGE, not once per app and not once per frame. Once per app
// was the second version and fitted only whichever page happened to be
// open first -- every later page's labels are laid out for the first
// time when it opens, so fill_slot() sees width 0, reserves one row,
// and a long description ellipsises. Every frame was the first version
// and relayouts under the user, resetting the scroll position so a long
// page could not be scrolled at all.
int  g_prose_fitted;
// The label width the last fit was computed at. A CHANGE is what asks
// for another one -- see on_draw().
int  g_prose_fit_w;
int  g_show_advanced;
int  g_page_group = -1;
// The sidebar node the open page came from. A release the sidebar took
// for a SCROLLBAR drag names the sidebar just like a row click does
// (uui_route.c reports the grab's id whatever the press was for), so
// without this a thumb drag re-opens the page and discards what the
// user had staged on it. Same guard the File Manager's ID_TREE already
// makes against its current directory.
int  g_page_node = -1;
// Does the current page have anything the toggle would reveal?
int  g_advanced_has;
// The last control geometry reported, so a CHANGE is what triggers the
// next report -- a page change and a SCROLL alike.
//
// It was a flag set on page change, which reported the previous page's
// rects (on_draw runs before the layout places a new page) and said
// nothing at all when the page scrolled. A tool driving a control below
// the fold then had no idea where it had moved to, and a scroll view
// correctly refuses to route a press to a child outside its viewport --
// so the control was simply unreachable and looked dead.
int g_last_y[PAGE_MAX];
int g_last_reported_count = -1;

struct slot g_slot[PAGE_MAX];
int g_slot_count;

struct uui_label     g_page_title;
struct uui_label     g_page_desc;
struct uui_checkbox  g_advanced_cb;
// THE SCREENSAVER'S "Test" BUTTON, which is Windows' Preview by another
// name. Page-specific, like the System Information page above it: a
// setting is a value and this is an ACTION, and the registry has no way
// to declare one. Shown only where it means something -- `g_test_has`.
//
// It spawns the saver and nothing else. The compositor recognises a
// client out of the savers directory and dismisses it on the first
// input exactly as it dismisses its own (wm_idle.h), so this needs no
// protocol of its own and cannot stick.
struct uui_button    g_test_btn;

int g_test_has;

char g_page_title_text[SETTING_ABI_LABEL_MAX];
char g_page_desc_text[SETTING_ABI_DESC_MAX];

// --- building a page --------------------------------------------------

// Fills slot `s` for setting `idx`: its caption, its description, its
// choices, and which control shows them.
// How many rows of PROSE to reserve, from the text and the width the
// label actually has. One row for almost everything; two when the text
// will not fit.
//
// WHY NOT JUST RESERVE TWO. That was the first version, and it cost a
// row on the nine descriptions out of ten that fit on one -- enough
// extra height to push the last control of the Mouse page BELOW THE
// SCROLL FOLD, where a control is not merely hard to click but
// unreachable (CLAUDE.md). Wrapping is meant to save the reader a
// resize, not spend the space it saved.
//
// This does NOT break the natural_size rule. That forbids a widget
// measuring itself from where it currently is, DURING layout; this is
// the app deciding what to ask for BEFORE layout runs -- the same thing
// it already did by writing `rows` by hand, computed instead of
// guessed. `l->w` is the previous layout's width, which is the current
// one on every frame but the first; zero there reserves one row and the
// next frame corrects it.
#define PROSE_ROWS_MAX 4
void fit_rows(struct uui_label *l, const char *text) {
    int need = 1;
    if (text && text[0] && l->w > 0) {
        // RUN THE REAL WRAPPER AND COUNT, rather than dividing the
        // text's width by the label's.
        //
        // Two versions got this wrong in the same direction. "One row or
        // two" clipped anything needing three. Then `ceil(text / width)`
        // looked exact and is a LOWER BOUND, not the answer: wrapping
        // breaks at spaces, so every line ends somewhere short of the
        // edge and the leftovers add up -- a sentence whose ratio is
        // 2.0 routinely needs three lines. Reserving two then ellipsised
        // it, which reads as "wrapping does not work" rather than as an
        // off-by-one.
        //
        // uui_label_wrap_next() is the function draw() itself uses, so
        // the count cannot disagree with the drawing by construction --
        // which is the only way to be sure, given that the answer
        // depends on where the spaces fall.
        char line[128];
        const char *p = text;
        need = 0;
        while (*p && need < PROSE_ROWS_MAX) {
            p = uui_label_wrap_next(p, l->w, line, (int)sizeof line);
            need++;
        }
        if (need < 1) need = 1;
    }
    uui_label_set_wrap(l, need);
}

// THE PROSE LINE A SLOT SHOWS -- the `unavailable` reason when there is
// one, otherwise the description.
//
// ONE FUNCTION BECAUSE TWO PLACES NEED THE SAME ANSWER, and they got
// different ones: load_slot() set the label to the reason while
// refit_prose() re-measured the DESCRIPTION, so an unavailable setting's
// sentence was wrapped to fit a string it was not -- one row, and the
// reason ellipsised at "...has no DMA...". The rows a label reserves and
// the text it draws have to be computed from the same string, and the
// only way to guarantee that is for there to be one place that decides.
const char *slot_prose(int idx) {
    if (idx < 0) return "";
    return g_unavail[idx][0] ? g_unavail[idx] : g_desc[idx];
}

// A setting the REGISTRY says cannot be changed here gets every one of
// its controls disabled -- all four, not just the one showing, because
// /etc/settings.d can swap which one that is with a `Widget=` line and
// a control that was live only under one presentation is the kind of
// gap that is found by a user rather than by a test.
//
// The widgets go dim and refuse input; the SENTENCE is drawn by
// relayout_page() in place of the description, since a disabled control
// with no reason beside it reads as a broken one.
// **THE COMPOSITOR HAS TO STAND DOWN WHILE A CAPTURE IS ARMED.** Global
// shortcuts are matched before any client sees a key, so without this
// pressing Super+E to bind it would launch a file manager and the
// control could never record the combinations it exists for
// (abi/win_proto.h). It lapses if this window loses the focus, so a
// crash mid-capture cannot leave the desktop with no shortcuts.
struct uapp *g_app;

void keycap_armed(void *ctx, int armed) {
    (void)ctx;
    if (g_app) uapp_inhibit_shortcuts(g_app, armed);
}

// A combination was captured, or capture was cancelled (`text` NULL).
// Nothing is WRITTEN here: the page stages it like every other control
// and Apply commits, so a mis-press is undone by Cancel rather than by
// pressing the old shortcut again.
void keycap_done(void *ctx, const char *text) {
    (void)ctx; (void)text;
    if (g_app) uapp_redraw(g_app);
}

void set_slot_enabled(struct slot *sl, int idx) {
    int off = idx >= 0 && g_unavail[idx][0] != '\0';
    sl->radio.disabled  = off;
    sl->combo.disabled  = off;
    sl->slider.disabled = off;
    sl->spin.disabled   = off;
    sl->text.disabled   = off;
}

void load_slot(struct slot *sl, int idx) {
    sl->setting = idx;
    sl->choice_count = 0;
    sl->staged = -1;
    sl->baseline = -1;

    uui_label_set_text(&sl->caption, g_label[idx]);
    // Empty is fine and common: /etc/settings.d is optional, and a
    // setting with no file simply has no description. The label reserves
    // its row either way, so a page does not reflow when text appears.
    // THE REASON REPLACES THE DESCRIPTION when there is one. Both would
    // be better, but the row budget is real (see fit_rows below, and the
    // control that once fell below the scroll fold), and of the two the
    // reason is the one the user needs: it explains a control that will
    // not respond, where the description explains one that would.
    uui_label_set_text(&sl->explain, slot_prose(idx));
    // ROWS FROM THE TEXT, at the width this page actually has. A flat
    // two rows for every explanation was the first version, and it cost
    // a row on the nine descriptions out of ten that fit on one --
    // enough extra height to push the last control of the Mouse page
    // BELOW THE SCROLL FOLD, where it is not merely hard to click but
    // unreachable (CLAUDE.md).
    //
    // This does NOT break the natural_size rule. That rule forbids a
    // widget MEASURING itself from where it currently is, during layout;
    // this is the app deciding, before layout runs, how many rows to
    // ask for -- the same thing it already does by writing `rows` by
    // hand, just computed instead of guessed. PAGE_SCROLL.w is the
    // previous layout's width, which is the current one on every frame
    // but the first; a zero there simply reserves one row and the next
    // frame corrects it.
    fit_rows(&sl->explain, slot_prose(idx));

    if (g_type[idx] == SETTING_ABI_TYPE_INT) {
        // A NUMBER GETS A SPINBOX. The range comes from the REGISTRY
        // (g_imin/g_imax/g_istep), not from anything this app decides --
        // so a setting whose bounds change in the kernel needs no edit
        // here, and a control can never offer a value setting_set()
        // would refuse.
        sl->kind = CTRL_SPIN;
        int cur = 0;
        for (const char *p = g_value[idx]; *p >= '0' && *p <= '9'; p++)
            cur = cur * 10 + (*p - '0');
        uui_spinbox_init(&sl->spin, cur, g_imin[idx], g_imax[idx],
                          g_istep[idx], g_unit[idx][0] ? g_unit[idx] : 0);
        // The NUMBER, not an index -- see struct slot.
        sl->baseline = uui_spinbox_value(&sl->spin);
        sl->staged = sl->baseline;
        sl->choice_count = 0;
        set_slot_enabled(sl, idx);
        return;
    }

    if (g_type[idx] == SETTING_ABI_TYPE_KEYCOMBO) {
        // A KEY COMBINATION GETS A CAPTURE CONTROL -- you press the keys
        // rather than spelling them. That is the whole reason KEYCOMBO
        // is a type of its own and not a STRING (abi/setting_abi.h).
        sl->kind = CTRL_KEYCAP;
        uui_keycapture_init(&sl->keycap, g_value[idx]);
        sl->keycap.on_arm = keycap_armed;
        sl->keycap.on_done = keycap_done;
        sl->keycap.ctx = sl;
        strlcpy(sl->baseline_buf, g_value[idx], sizeof sl->baseline_buf);
        sl->baseline = 0;
        sl->staged = 0;
        sl->choice_count = 0;
        set_slot_enabled(sl, idx);
        return;
    }

    if (g_type[idx] != SETTING_ABI_TYPE_ENUM) {
        // FREE TEXT GETS A FIELD. There are no choices to enumerate, so
        // the value is edited directly; `staged` is a CHANGED FLAG here
        // rather than an index (see struct slot).
        sl->kind = CTRL_TEXT;
        uui_textbox_init(&sl->text, g_value[idx]);
        strlcpy(sl->baseline_buf, g_value[idx], sizeof sl->baseline_buf);
        sl->baseline = 0;
        sl->staged = 0;
        sl->choice_count = 0;
        set_slot_enabled(sl, idx);
        return;
    }

    const struct usaver_opt *o = opt_of(idx);
    for (int c = 0; c < MAX_CHOICES; c++) {
        // THE RAW VALUE IS WHAT GETS STORED; the label is what is shown.
        // The registry's own label falls back to the value, and a saver
        // option has no display names at all (usaver_display() just
        // capitalises), so neither branch has to decide -- applying the
        // displayed string would try to set the timezone to "Los
        // Angeles".
        char raw[SETTING_ABI_VALUE_MAX], disp[SETTING_ABI_LABEL_MAX];
        if (o) {
            if (c >= o->choice_count) break;
            strlcpy(raw, o->choice[c], sizeof raw);
            usaver_display(raw, disp, sizeof disp);
        } else {
            struct setting_msg m;
            memset(&m, 0, sizeof m);
            m.op = SETTING_OP_CHOICE;
            m.index = idx;
            m.choice = c;
            if (usetting_dispatch(&m) != 0) break; // past the last one
            strlcpy(raw, m.value, sizeof raw);
            strlcpy(disp, m.label, sizeof disp);
        }

        strlcpy(sl->choice_raw[c], raw, sizeof sl->choice_raw[c]);
        if (strcmp(raw, g_value[idx]) == 0) {
            // THE MARKER, baked into the option's text: uui_radio_list
            // and uui_dropdown both take plain strings, and a "mark this
            // row" hook on each would be a widget feature with one
            // caller. If a second app wants it, that is when it earns a
            // place in the widgets.
            snprintf(sl->choice[c], sizeof sl->choice[c], "%s   (current)", disp);
            sl->baseline = c;
            sl->staged = c;
        } else {
            strlcpy(sl->choice[c], disp, sizeof sl->choice[c]);
        }
        sl->choice_ptr[c] = sl->choice[c];
        sl->choice_count = c + 1;
    }

    // WHICH CONTROL. /etc/settings.d may say; otherwise the count
    // decides. The file's word wins because it knows things the count
    // cannot -- a setting with five choices today that will have fifty.
    if (g_widget[idx] == SETTING_ABI_WIDGET_DROPDOWN) sl->kind = CTRL_COMBO;
    else if (g_widget[idx] == SETTING_ABI_WIDGET_RADIO) sl->kind = CTRL_RADIO;
    else if (g_widget[idx] == SETTING_ABI_WIDGET_SLIDER) sl->kind = CTRL_SLIDER;
    else sl->kind = sl->choice_count >= CHOICES_DROPDOWN_MIN ? CTRL_COMBO : CTRL_RADIO;

    sl->radio.options = sl->choice_ptr;
    sl->radio.count = sl->choice_count;
    sl->radio.cols = 1;
    sl->radio.selected = sl->staged;
    // No set_items on a dropdown -- init IS the setter, and re-initing
    // also resets its popup's scroll, which is what you want on a page
    // that just changed.
    uui_dropdown_init(&sl->combo, 0, 0, 0, 0, sl->choice_ptr, sl->choice_count);
    sl->combo.list.selected = sl->staged;
    uui_slider_set_options(&sl->slider, sl->choice_ptr, sl->choice_count);
    sl->slider.selected = sl->staged;
    set_slot_enabled(sl, idx);
}

// Does this page have anything staged but not yet applied?
int page_dirty(void) {
    for (int i = 0; i < g_slot_count; i++)
        if (g_slot[i].setting >= 0 && g_slot[i].staged != g_slot[i].baseline)
            return 1;
    return 0;
}

// Declared here; defined below with the rest of the layout.
void relayout_page(void);
// How many per-setting captions the last relayout_page() emitted. Only
// a test reads it (see the `settings: page` line) -- a suppressed
// caption and an absent one are indistinguishable in a screendump.
int g_page_captions;

// Shows the page for group `g`. Its settings, in Order= then
// registration order, with advanced ones held back unless asked for.
void open_group(int g) {
    g_page_group = g;
    g_show_sysinfo = 0;
    g_slot_count = 0;
    g_saver_slot = -1;
    g_saver_base = -1;

    int hidden_advanced = 0;
    // A SELECTION SORT over the group's settings rather than sorting the
    // whole table: the page is at most PAGE_MAX long, and the registry's
    // own order has to survive as the tie-break -- a stable sort of six
    // items by hand is cheaper than keeping a permutation of everything.
    int taken[MAX_SETTINGS];
    for (int i = 0; i < g_setting_count; i++) taken[i] = 0;

    for (;;) {
        int best = -1;
        for (int i = 0; i < g_setting_count; i++) {
            if (taken[i]) continue;
            if (strcmp(g_cat_of[i], g_group_cat[g]) != 0) continue;
            if (strcmp(group_key_of(i), g_group_key[g]) != 0) continue;
            if ((g_sflags[i] & SETTING_ABI_SF_ADVANCED) && !g_show_advanced) {
                hidden_advanced++;
                taken[i] = 1;
                continue;
            }
            if (best < 0 || g_order[i] < g_order[best]) best = i;
        }
        if (best < 0) break;
        taken[best] = 1;
        if (g_slot_count >= PAGE_MAX) {
            // REPORTED, never silently cut: a page showing five of seven
            // settings with nothing saying so is the failure mode this
            // project keeps writing rules about.
            snprintf(g_status, sizeof g_status,
                     "%s: too many settings for one page -- use `config list`",
                     g_group_label[g]);
            break;
        }
        load_slot(&g_slot[g_slot_count++], best);
    }

    // The page's own title and description, from /etc/settings.d.
    strlcpy(g_page_title_text, g_group_label[g], sizeof g_page_title_text);
    fit_rows(&g_page_desc, g_page_desc_text);
    g_page_desc_text[0] = '\0';
    struct setting_msg m;
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_GROUP_TEXT;
    snprintf(m.name, sizeof m.name, "%s/%s", g_group_cat[g], g_group_key[g]);
    if (usetting_dispatch(&m) == 0 && m.description[0])
        strlcpy(g_page_desc_text, m.description, sizeof g_page_desc_text);

    // THE TEST BUTTON BELONGS TO THE PAGE THAT CARRIES THE SAVER, found
    // by the setting it holds rather than by the page's NAME: a page
    // renamed in /etc/settings.d would otherwise silently lose it.
    g_test_has = 0;
    for (int i = 0; i < g_slot_count; i++)
        if (g_slot[i].setting >= 0 &&
            strcmp(g_name[g_slot[i].setting], "desktop.screensaver") == 0)
            g_test_has = 1;

    // ...and so do the chosen saver's own options, below the settings
    // that select it. One page rather than a Configure button: the
    // options are meaningless without the saver they belong to, and a
    // dialog would hide the thing Test is there to preview.
    const char *own_kind = 0;
    const char *own_name = page_owner(&own_kind);
    g_page_owner_kind = own_kind;
    if (own_kind) {
        g_saver_slot = g_slot_count;
        rebuild_owner_options(own_kind, own_name);
    } else {
        // NO OWNER ON THIS PAGE MEANS NO OPTIONS, and g_saver must be
        // emptied to say so. Leaving it is how the Effects page came to
        // report the SCREENSAVER's three options and offer a button
        // that opened nothing: the count is read from g_saver, and the
        // previous page had filled it.
        g_saver.opt_count = 0;
        g_saver.name[0] = '\0';
    }
    ulogf("settings: owner %s %s opts %d\n",
          own_kind ? own_kind : "-", own_name ? own_name : "-",
          g_saver.opt_count);

    g_advanced_cb.checked = g_show_advanced;
    // The checkbox appears only where there is something to reveal --
    // an "advanced" toggle on a page with no advanced settings is a
    // control that does nothing.
    g_advanced_cb.label = "Show advanced settings";
    g_advanced_has = hidden_advanced > 0 || g_show_advanced;

    if (!g_status[0] || g_page_group == g)
        snprintf(g_status, sizeof g_status, "%s", g_page_title_text);
    relayout_page();
    // `captions` and `disabled` ride along because both are otherwise
    // INVISIBLE to a test: a caption suppressed as a duplicate of the
    // page title and a caption that was never there look identical in a
    // screendump, and a greyed control differs from a live one by a few
    // units of colour. Reported facts, not pixels -- CLAUDE.md.
    int captions = 0, off = 0;
    for (int i = 0; i < g_slot_count; i++) {
        if (g_slot[i].setting < 0) continue;
        if (g_unavail[g_slot[i].setting][0]) off++;
    }
    captions = g_page_captions; // set by relayout_page() just above
    ulogf("settings: page %s/%s slots %d advanced %d captions %d disabled %d\n",
          g_group_cat[g], g_group_key[g], g_slot_count, hidden_advanced,
          captions, off);
}

// Applies every staged change on the page. Reports the OUTCOME rather
// than assuming it worked: SETTING_UNSAVED means the change is live but
// will not survive a reboot, which is the one thing a settings UI must
// never report as plain success.
// The value this slot would store, whichever control it is showing.
//
// ONE FUNCTION, because the alternative is a branch at each of the four
// places that used to write `choice_raw[staged]` -- the commit, its log
// line, and the two status-bar messages -- and a kind added later would
// have to find all of them.
const char *staged_value(struct slot *sl) {
    if (sl->kind == CTRL_TEXT) return uui_textbox_text(&sl->text);
    if (sl->kind == CTRL_KEYCAP) return sl->keycap.text;
    if (sl->kind == CTRL_SPIN) {
        snprintf(sl->staged_buf, sizeof sl->staged_buf, "%d", sl->staged);
        return sl->staged_buf;
    }
    if (sl->staged < 0 || sl->staged >= sl->choice_count) return "";
    return sl->choice_raw[sl->staged];
}

int apply_page(void) {
    int changed = 0, failed = 0, unsaved = 0, needs_reboot = 0;

    for (int i = 0; i < g_slot_count; i++) {
        struct slot *sl = &g_slot[i];
        if (sl->setting < 0 || sl->staged < 0) continue;
        if (sl->staged == sl->baseline) continue;

        // A SAVER OPTION IS A LINE IN A FILE, and there is nothing to
        // apply it to: no subsystem holds the value and the program that
        // reads it is not running. So the write IS the change, and it
        // cannot come back UNSAVED -- the two outcomes here are a
        // rewritten document and a failed write.
        const struct usaver_opt *o = opt_of(sl->setting);
        if (o) {
            if (uconf_set(g_file[sl->setting], o->key, staged_value(sl))) {
                changed++;
                ulogf("settings: set %s %s result saved\n",
                      g_name[sl->setting], staged_value(sl));
            } else {
                failed++;
                ulogf("settings: set %s %s result FAILED\n",
                      g_name[sl->setting], staged_value(sl));
            }
            continue;
        }

        struct setting_msg m;
        memset(&m, 0, sizeof m);
        m.op = SETTING_OP_SET;
        strlcpy(m.name, g_name[sl->setting], sizeof m.name);
        strlcpy(m.value, staged_value(sl), sizeof m.value);
        if (usetting_dispatch(&m) != 0) { failed++; continue; }

        ulogf("settings: set %s %s result %u\n", g_name[sl->setting],
              staged_value(sl), m.result);
        if (m.result == SETTING_SAVED) changed++;
        else if (m.result == SETTING_UNSAVED) { changed++; unsaved++; }
        else failed++;
        if (g_sflags[sl->setting] & SETTING_ABI_SF_REBOOT) needs_reboot++;
    }

    if (failed)
        snprintf(g_status, sizeof g_status, "%d change(s) were REFUSED", failed);
    else if (unsaved)
        snprintf(g_status, sizeof g_status,
                 "Applied, but %d did NOT save -- they revert at the next boot", unsaved);
    else if (needs_reboot)
        // The one thing `result` cannot say: the value persisted and yet
        // nothing visibly happened, because init reads it at boot.
        snprintf(g_status, sizeof g_status,
                 "Saved -- %d change(s) take effect at the next boot", needs_reboot);
    else if (changed)
        snprintf(g_status, sizeof g_status, "Saved %d change(s)", changed);
    else
        strlcpy(g_status, "Nothing to apply", sizeof g_status);

    // Re-read: what is in effect is whatever the registry now says, not
    // what was asked for. Believing the request instead of the answer is
    // how a UI shows a setting that was refused.
    if (changed) {
        int keep_node = uui_sidebar_selected_id(&g_tree);
        reload_settings();
        uui_sidebar_select_id(&g_tree, keep_node);
        if (g_page_group >= 0 && g_page_group < g_group_count) open_group(g_page_group);
    }
    return failed == 0;
}

struct uui_item PAGE[PAGE_ITEMS];
int PAGE_COUNT;
struct uui_layout PAGE_LAYOUT;
struct uui_scrollview PAGE_SCROLL;

// THE PAGE'S CONTROLS ARE A FOCUS RING, rebuilt with the page. Tab
// moves between the controls of whatever page is open, and that is the
// whole ring on purpose: the sidebar is reached by clicking, and the
// buttons commit what the controls staged. Without it a dropdown could
// never hold keyboard focus, so typing a city name -- which is the
// point of a 92-item list -- had nowhere to arrive.
//
// The FULL ops tables, not the `_focus_ops` ones: they carry `key`,
// `set_focused` and `accepts_focus` already (ui/uui_widget.h), so a
// second table per widget would be a second place to drift.
struct uui_focusable FOCUS[PAGE_MAX];
int FOCUS_COUNT;
struct uui_focus PAGE_FOCUS;

// Rebuilds the page's item list for whatever is currently on it. The
// count is DERIVED from what was added, never written out: a literal
// count that disagreed with its array once walked the router one item
// past the end into a garbage ops table.
// ONE SLOT'S ROWS -- its caption, its explanation and the one control
// it actually uses -- written into a CALLER'S array.
//
// Split out of relayout_page() so the options dialog can build the same
// rows from the same slots (`relayout_dialog()`): a control is a
// control wherever it is drawn, and two copies of this switch would
// drift the first time a widget kind was added.
int emit_slot(struct uui_item *out, int n, int i,
                     struct uui_focusable *focus, int *nfocus) {
        struct slot *sl = &g_slot[i];
        int drop_caption = (sl->setting >= 0 &&
                            strcasecmp(g_label[sl->setting], g_page_title_text) == 0);
        if (!drop_caption) {
            out[n++] = (struct uui_item){ .ops = &uui_label_ops, .widget = &sl->caption,
                                            .id = 0, .flags = UUI_FILL_W };
            g_page_captions++;
        }
        // **ONLY WHEN IT SAYS SOMETHING.** An empty explanation used to
        // be declared anyway, reserving a row so that a description
        // appearing later could not reflow the page -- and the cost was
        // a blank row between every caption and its own control, which
        // is the gap that made this page look wrong. relayout_page()
        // rebuilds the item list whenever the page changes, so a reason
        // arriving simply adds its rows then; there is nothing to
        // reserve against.
        if (slot_prose(sl->setting)[0]) {
            out[n++] = (struct uui_item){ .ops = &uui_label_ops, .widget = &sl->explain,
                                            .id = 0, .flags = UUI_FILL_W };
        }
        // ONLY THE CONTROL IN USE is declared. Declaring both and hiding
        // one was the first version, and `hidden` is honoured -- but a
        // hidden widget is still a layout child, and the pair left this
        // page carrying twice the items it needed with one of every two
        // contributing nothing. Emitting one keeps the item list
        // describing exactly what is on screen, which is also what makes
        // the reported geometry mean something.
        if (sl->kind == CTRL_COMBO) {
            out[n++] = (struct uui_item){ .ops = &uui_dropdown_ops,
                                            .widget = &sl->combo,
                                            .id = ID_CONTROL_BASE + i };
            focus[(*nfocus)++] = (struct uui_focusable){ &sl->combo, &uui_dropdown_ops };
        } else if (sl->kind == CTRL_SLIDER) {
            out[n++] = (struct uui_item){ .ops = &uui_slider_ops,
                                            .widget = &sl->slider,
                                            .id = ID_CONTROL_BASE + i,
                                            .flags = UUI_FILL_W };
            focus[(*nfocus)++] = (struct uui_focusable){ &sl->slider, &uui_slider_ops };
        } else if (sl->kind == CTRL_TEXT) {
            // FILL_W, unlike the spinbox: a field has no natural width
            // at all (uui_textbox_natural_size reports 0, meaning "no
            // preference"), so an unstretched one would be invisible.
            out[n++] = (struct uui_item){ .ops = &uui_textbox_ops,
                                            .widget = &sl->text,
                                            .id = ID_CONTROL_BASE + i,
                                            .flags = UUI_FILL_W };
            focus[(*nfocus)++] = (struct uui_focusable){ &sl->text, &uui_textbox_ops };
        } else if (sl->kind == CTRL_KEYCAP) {
            // NOT UUI_FILL_W: its natural size is measured from the
            // longest thing it ever shows (the prompt), so stretching it
            // across the page would leave a shortcut floating in a box
            // four times its width.
            out[n++] = (struct uui_item){ .ops = &uui_keycapture_ops,
                                            .widget = &sl->keycap,
                                            .id = ID_CONTROL_BASE + i };
            focus[(*nfocus)++] = (struct uui_focusable){ &sl->keycap, &uui_keycapture_ops };
        } else if (sl->kind == CTRL_SPIN) {
            // NOT UUI_FILL_W: a spinbox wants exactly the width of its
            // widest number plus its steppers, and stretching it across
            // the page would put the arrows an inch from the digits.
            // Its natural size is the right size.
            out[n++] = (struct uui_item){ .ops = &uui_spinbox_ops,
                                            .widget = &sl->spin,
                                            .id = ID_CONTROL_BASE + i };
            focus[(*nfocus)++] = (struct uui_focusable){ &sl->spin, &uui_spinbox_ops };
        } else {
            out[n++] = (struct uui_item){ .ops = &uui_radio_list_ops,
                                            .widget = &sl->radio,
                                            .id = ID_CONTROL_BASE + i,
                                            .flags = UUI_FILL_W };
            focus[(*nfocus)++] = (struct uui_focusable){ &sl->radio, &uui_radio_list_ops };
        }
    return n;
}

void relayout_page(void) {
    int n = 0;
    FOCUS_COUNT = 0;
    PAGE[n++] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_page_title,
                                    .id = 0, .flags = UUI_FILL_W };
    PAGE[n++] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_page_desc,
                                    .id = 0, .flags = UUI_FILL_W };
    // A PAGE DOES NOT REPEAT ITS OWN TITLE. A setting that declares no
    // `group` gets a page of its own NAMED BY ITS LABEL (group_key_of),
    // so the heading and that caption are the same string -- "Time zone"
    // printed twice. The title is the one to keep: it is what the
    // sidebar row says, so dropping the caption leaves the page named
    // exactly once and named the same way it was reached.
    //
    // PER SETTING AND CASE-INSENSITIVE, not once per single-control
    // page. Network Time is a page of THREE settings whose first is
    // labelled "Network time", so neither half of the old test fired
    // and the page opened with "Network Time" directly above "Network
    // time" -- a duplicate that differed only in one capital letter,
    // which is exactly the kind a case-sensitive compare cannot see.
    //
    // Compared rather than inferred from "did it declare a group?",
    // because a declared group whose name happens to match one of its
    // settings reads identically to a reader and should behave the same.
    int title_dup = 0;
    for (int i = 0; i < g_slot_count; i++) {
        if (g_slot[i].setting >= 0 &&
            strcasecmp(g_label[g_slot[i].setting], g_page_title_text) == 0) {
            title_dup = 1;
            break;
        }
    }

    // AND THE GROUP BLURB GOES WITH IT. Once a caption is dropped the
    // page reads title -> group description -> that setting's own
    // description -> control: two blocks of prose stacked before the
    // first thing you can click, saying overlapping things. The
    // setting's is the one to keep, because it sits against the control
    // it explains; the group's is orientation the title already gave.
    // Only fires where the duplication is, so a page whose captions all
    // survive keeps its blurb.
    if (title_dup) g_page_desc_text[0] = '\0';

    g_page_captions = 0;
    // THE OPTION ROWS ARE NOT ON THE PAGE. They belong to whichever
    // effect or saver the dropdown names, and they are reached through
    // the Settings... button below it -- see g_opts_btn. g_saver_slot
    // is where they start, so the page stops there.
    int page_slots = (g_saver_slot >= 0 && owner_uses_dialog(g_page_owner_kind))
                     ? g_saver_slot : g_slot_count;
    for (int i = 0; i < page_slots; i++) {
        struct slot *sl = &g_slot[i];
        n = emit_slot(PAGE, n, i, FOCUS, &FOCUS_COUNT);

        // THE TEST BUTTON SITS UNDER THE CONTROL IT PREVIEWS, and it is
        // INSIDE the page for a reason that is not taste: the ROOT
        // layout runs at startup, on a resize and on a font change only
        // (uapp.c), so a root item whose `hidden` follows the open page
        // is placed ONCE, while hidden, at zero size -- and a zero-sized
        // button draws nothing but its label, which reads as a stray
        // caption rather than as a missing control. PAGE is rebuilt and
        // re-laid out on every page change, so a hidden item here works.
        //
        // Under the chooser is also where Windows puts Preview and where
        // KDE puts its screen-locker preview: beside the thing it shows,
        // not among the dialog's own verbs. Being near the TOP of the
        // page is what keeps it clear of the scroll fold whatever the
        // chosen saver declares.
        if (g_test_has && sl->setting >= 0 &&
            strcmp(g_name[sl->setting], "desktop.screensaver") == 0) {
            PAGE[n++] = (struct uui_item){ .ops = &uui_button_ops,
                                            .widget = &g_test_btn,
                                            .id = ID_TEST };
        }
        // SETTINGS... SITS UNDER THE CONTROL IT CONFIGURES, and only
        // where there is something to configure: an effect that
        // declares no options gets no button rather than a dialog that
        // opens empty. Windows shows it greyed; this system has no
        // greyed buttons and an absent one says the same thing.
        if (sl->setting >= 0 && owner_uses_dialog(owner_kind(sl->setting)) &&
            g_saver.opt_count > 0) {
            PAGE[n++] = (struct uui_item){ .ops = &uui_button_ops,
                                            .widget = &g_opts_btn,
                                            .id = ID_OPTS };
        }

        // The gap that separates this setting from the next -- see
        // `spacer`. NOT after the last one: a trailing blank row at the
        // bottom of a scroll view is space you can scroll to and find
        // nothing in.
        if (i + 1 < page_slots) {
            PAGE[n++] = (struct uui_item){ .ops = &uui_label_ops, .widget = &sl->spacer,
                                            .id = 0, .flags = UUI_FILL_W };
        }
    }
    PAGE[n++] = (struct uui_item){ .ops = &uui_checkbox_ops, .widget = &g_advanced_cb,
                                    .id = ID_ADVANCED, .hidden = !g_advanced_has };
    PAGE_COUNT = n;
    PAGE_LAYOUT.count = n;
    // The widgets the ring pointed at may have been re-inited by
    // load_slot(), so it starts again with nothing focused rather than
    // holding an index into the previous page.
    uui_focus_init(&PAGE_FOCUS, FOCUS, FOCUS_COUNT);
    // The item list just changed. The scroll view now NOTICES this by
    // itself (uui_scrollview.h), so this is belt-and-braces rather than
    // load-bearing -- kept because saying so at the point of change is
    // clearer than relying on a check somewhere else.
    uui_scrollview_content_changed(&PAGE_SCROLL);
}

// A CONTROL MOVED: stage its value, say so, and -- when the control is
// the one that OWNS a page's options -- rebuild them.
//
// Shared with the options dialog, which routes the same slot ids
// through its own window: staging is the slot's, not the surface's, so
// a control changed in either place stages identically and one Apply
// writes both.
//
// Returns 1 when the page's own item list has to be rebuilt.
int control_changed(int slot_index) {
    int relayout = 0;
        // STAGED, not applied. The selection is remembered; Apply or OK
        // is what writes it.
        struct slot *sl = &g_slot[slot_index];
        // A SPINBOX REPORTS A NUMBER, and it has already bounded it --
        // the widget cannot produce a value outside the range the
        // registry gave it. `staged` therefore holds the number itself
        // here and an index everywhere else; see struct slot.
        // A FIELD REPORTS WHETHER IT DIFFERS, not an index -- there is
        // no choice list to index into. `baseline` stays 0, so the
        // page's `staged != baseline` tests keep working unchanged.
        sl->staged = sl->kind == CTRL_KEYCAP
                        ? (strcmp(sl->keycap.text, sl->baseline_buf) != 0)
                    : sl->kind == CTRL_TEXT
                        ? (strcmp(uui_textbox_text(&sl->text), sl->baseline_buf) != 0)
                    : sl->kind == CTRL_COMBO   ? uui_dropdown_selected(&sl->combo)
                    : sl->kind == CTRL_SLIDER ? sl->slider.selected
                    : sl->kind == CTRL_SPIN   ? uui_spinbox_value(&sl->spin)
                                              : sl->radio.selected;
        if (sl->setting >= 0 && sl->staged >= 0) {
            snprintf(g_status, sizeof g_status, "%s -> %s   (not applied yet)",
                     g_label[sl->setting], staged_value(sl));
            // LOGGED as well as shown. The status bar is pixels; a test
            // asserting on staging needs a fact, and reading the bar
            // back from a screenshot would be asserting the wrong thing
            // anyway -- what matters is that the change was staged and
            // NOT written.
            ulogf("settings: staged %s %s\n", g_name[sl->setting],
                  staged_value(sl));
        }
        // PICKING A SAVER CHANGES WHAT IS BELOW IT: the rows under the
        // dropdown belong to the saver it names. Rebuilt here rather
        // than at Apply, because a page still showing the previous
        // saver's controls is one where every value and every bound is
        // the wrong saver's.
        if (sl->setting >= 0 && !opt_of(sl->setting) && owner_kind(sl->setting)) {
            rebuild_owner_options(owner_kind(sl->setting), staged_value(sl));
            g_prose_fitted = 0;   // the new rows' text has never been fitted
            relayout = 1;
        }
    return relayout;
}

// Re-asks fit_rows() for every prose label now that the layout has
// given them real widths, and relayouts if any changed. Returns 1 if it
// did, so the caller can ask for the extra frame.
//
// THIS IS A FEEDBACK LOOP, AND IT IS THE SAFE DIRECTION. `rows` is
// computed from WIDTH and only ever changes HEIGHT; width comes from
// UUI_FILL_W and does not depend on height, so the second pass sees the
// same widths as the first and settles. The dangerous version -- the
// one CLAUDE.md's natural_size rule forbids -- is a widget whose
// measurement depends on its own placement, which oscillates.
//
// It exists because a label's width is 0 until the first layout, so
// fit_rows() at fill_slot() time reserves one row for everything and a
// long description ellipsises where it should have wrapped. Asking
// again once is what makes the answer right on the frame after.
int refit_prose(void) {
    int changed = 0;
    int was = g_page_desc.rows;
    fit_rows(&g_page_desc, g_page_desc_text);
    if (g_page_desc.rows != was) changed = 1;
    // THE PAGE FITS WHAT THE PAGE SHOWS. The option slots live past
    // g_saver_slot and are drawn in the options DIALOG, at its narrower
    // width -- and one `explain` label serves both surfaces, so a page
    // that re-fitted them overwrote the dialog's fit on its next frame
    // and cut an explanation off mid-word while the dialog was open.
    int page_slots = (g_saver_slot >= 0 && owner_uses_dialog(g_page_owner_kind))
                     ? g_saver_slot : g_slot_count;
    for (int i = 0; i < page_slots; i++) {
        struct slot *sl = &g_slot[i];
        if (sl->setting < 0) continue;
        was = sl->explain.rows;
        // slot_prose(), NOT g_desc: see its comment. This line reading
        // the description while load_slot() drew the reason is what
        // ellipsised every `unavailable` sentence to one row.
        fit_rows(&sl->explain, slot_prose(sl->setting));
        if (sl->explain.rows != was) changed = 1;
    }
    if (changed) relayout_page();
    return changed;
}

// THE SHOWING CONTROL'S RECT, KIND AND DISABLED STATE, for the reports
// below. One switch naming every kind: three hand-kept chains used to
// answer these separately, and all three sent a key-capture control
// down the radio branch -- so a shortcut slot reported 0 0 0 0 and a
// test clicking it landed on the window's corner.
void slot_rect(const struct slot *sl, int *x, int *y, int *w, int *h) {
    switch (sl->kind) {
    case CTRL_COMBO:  *x = sl->combo.x;  *y = sl->combo.y;  *w = sl->combo.w;  *h = sl->combo.h;  break;
    case CTRL_SLIDER: *x = sl->slider.x; *y = sl->slider.y; *w = sl->slider.w; *h = sl->slider.h; break;
    case CTRL_SPIN:   *x = sl->spin.x;   *y = sl->spin.y;   *w = sl->spin.w;   *h = sl->spin.h;   break;
    case CTRL_TEXT:   *x = sl->text.x;   *y = sl->text.y;   *w = sl->text.w;   *h = sl->text.h;   break;
    case CTRL_KEYCAP: *x = sl->keycap.x; *y = sl->keycap.y; *w = sl->keycap.w; *h = sl->keycap.h; break;
    default:          *x = sl->radio.x;  *y = sl->radio.y;  *w = sl->radio.w;  *h = sl->radio.h;  break;
    }
}

const char *slot_kind_name(const struct slot *sl) {
    switch (sl->kind) {
    case CTRL_COMBO:  return "combo";
    case CTRL_SLIDER: return "slider";
    case CTRL_SPIN:   return "spin";
    case CTRL_TEXT:   return "text";
    case CTRL_KEYCAP: return "keycap";
    default:          return "radio";
    }
}

// A key-capture control has no disabled state of its own.
int slot_disabled(const struct slot *sl) {
    switch (sl->kind) {
    case CTRL_COMBO:  return sl->combo.disabled;
    case CTRL_SLIDER: return sl->slider.disabled;
    case CTRL_SPIN:   return sl->spin.disabled;
    case CTRL_TEXT:   return sl->text.disabled;
    case CTRL_KEYCAP: return 0;
    default:          return sl->radio.disabled;
    }
}
