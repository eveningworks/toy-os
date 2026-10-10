// System Settings: one page -- its slots, their controls, staging and Apply.
//
// A PAGE IS A COLUMN OF CARDS (ui/uui_setting_row.h): each setting's name
// and description on the left, its control on the right, or under the
// text when the control is a list. Which control a setting gets is
// decided here from what its values ARE -- see pick_kind().
#include "settings/settings_internal.h"
#include "lib/usetting_schema.h" // WhenUnmet= and Requires=, read from the declaration
#include "query_abi.h"
#include <stdlib.h>

char g_status[160];
int  g_loaded;
int  g_show_sysinfo;

// Set once the open page's cards have been fitted at the width they were
// laid out at; a CHANGE of that width asks again (on_draw). Refitting on
// every frame relayouts under the user and resets the scroll position.
int  g_prose_fitted;
int  g_prose_fit_w;
int  g_show_advanced;
int  g_page_group = -1;
// The sidebar node the open page came from. A release the sidebar took
// for a SCROLLBAR drag names the sidebar just like a row click does, so
// without this a thumb drag re-opens the page and discards what was
// staged on it.
int  g_page_node = -1;
int  g_advanced_has;
// The last control geometry reported, so a CHANGE -- a page change or a
// scroll -- is what triggers the next report.
int g_last_y[PAGE_MAX];
int g_last_reported_count = -1;

struct slot g_slot[PAGE_MAX];
int g_slot_count;

struct uui_label     g_page_title;
struct uui_label     g_page_desc;
struct uui_checkbox  g_advanced_cb;
// THE SCREENSAVER'S "Test" BUTTON -- Windows' Preview. An ACTION, which
// the registry has no way to declare, so it is the page's own; shown only
// where `g_test_has` says it means something.
struct uui_button    g_test_btn;
int g_test_has;

char g_page_title_text[SETTING_ABI_LABEL_MAX];
char g_page_desc_text[SETTING_ABI_DESC_MAX];

// --- prose ------------------------------------------------------------

// How many rows the PAGE description needs at the width its label had in
// the last layout -- the app deciding before layout, which the
// natural_size rule allows (the cards do the same in uui_setting_row_fit).
#define PROSE_ROWS_MAX 4
void fit_rows(struct uui_label *l, const char *text) {
    int need = 1;
    if (text && text[0] && l->w > 0) {
        // The real wrapper, counted: a width ratio is only a lower bound,
        // since every wrapped line ends short of the edge.
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

// The `unavailable` reason when there is one, otherwise the description.
// ONE place decides, because the rows reserved and the text drawn must be
// measured from the same string.
const char *slot_prose(int idx) {
    if (idx < 0) return "";
    return g_unavail[idx][0] ? g_unavail[idx] : g_desc[idx];
}

// **THE COMPOSITOR HAS TO STAND DOWN WHILE A CAPTURE IS ARMED.** Global
// shortcuts are matched before any client sees a key, so without this
// pressing Super+E to bind it would launch a file manager. It lapses if
// this window loses the focus, so a crash mid-capture cannot leave the
// desktop without shortcuts.
struct uapp *g_app;

void keycap_armed(void *ctx, int armed) {
    (void)ctx;
    if (g_app) uapp_inhibit_shortcuts(g_app, armed);
}

// Captured or cancelled. Nothing is WRITTEN: it stages like every other
// control, and Apply commits.
void keycap_done(void *ctx, const char *text) {
    (void)ctx; (void)text;
    if (g_app) uapp_redraw(g_app);
}

// Every presentation disabled together, not just the one showing: a
// `Widget=` line can swap which one that is.
void set_slot_enabled(struct slot *sl, int idx) {
    int off = idx >= 0 && g_unavail[idx][0] != '\0';
    sl->radio.disabled  = off;
    sl->combo.disabled  = off;
    sl->slider.disabled = off;
    sl->spin.disabled   = off;
    sl->text.disabled   = off;
    sl->sw.disabled     = off;
    sl->seg.disabled    = off;
    sl->gallery.disabled = off;
    sl->row.disabled    = off;
}

// --- which control ------------------------------------------------------

// A two-valued setting whose values are a state and its absence gets a
// SWITCH. Returns the index of the "on" value, or -1.
static int onoff_index(const struct slot *sl) {
    static const char *const PAIRS[][2] = {
        { "on", "off" }, { "yes", "no" }, { "true", "false" },
        { "enabled", "disabled" }, { "1", "0" },
    };
    if (sl->choice_count != 2) return -1;
    for (unsigned p = 0; p < sizeof PAIRS / sizeof PAIRS[0]; p++)
        for (int on = 0; on < 2; on++)
            if (!strcmp(sl->choice_raw[on], PAIRS[p][0]) &&
                !strcmp(sl->choice_raw[1 - on], PAIRS[p][1]))
                return on;
    return -1;
}

// Few choices with SHORT names sit side by side as one segmented control;
// the budget is font-derived, so a bigger font tips a setting into a list
// rather than overflowing its card.
static int fits_segmented(const struct slot *sl) {
    if (sl->choice_count < 2 || sl->choice_count > 4) return 0;
    int widest = 0;
    for (int c = 0; c < sl->choice_count; c++) {
        int w = ugfx_text_width(sl->choice[c]);
        if (w > widest) widest = w;
    }
    return widest * sl->choice_count <= ugfx_char_w() * 24;
}

// A CHOICE setting's control, from what its values ARE. /etc/settings.d's
// `Widget=dropdown` and `Widget=slider` are honoured as they always were;
// `radio` and no word at all both mean "pick by the values": a switch for
// on/off, a segmented control for a few short names, a list inside the
// card for a few long ones, a dropdown for many.
static int pick_kind(struct slot *sl, int idx) {
    sl->on_idx = onoff_index(sl);
    if (g_widget[idx] == SETTING_ABI_WIDGET_DROPDOWN) return CTRL_COMBO;
    if (g_widget[idx] == SETTING_ABI_WIDGET_SLIDER)   return CTRL_SLIDER;
    if (g_widget[idx] == SETTING_ABI_WIDGET_GALLERY)  return CTRL_GALLERY;
    if (sl->on_idx >= 0)   return CTRL_SWITCH;
    if (fits_segmented(sl)) return CTRL_SEGMENTED;
    return sl->choice_count >= CHOICES_DROPDOWN_MIN ? CTRL_COMBO : CTRL_RADIO;
}

void load_slot(struct slot *sl, int idx) {
    sl->setting = idx;
    sl->choice_count = 0;
    sl->staged = -1;
    sl->baseline = -1;
    sl->on_idx = -1;
    // A new page: the card's fit starts over.
    sl->row.desc_rows = 1;
    sl->row.stacked_auto = 0;
    sl->row.changed = 0;

    if (g_type[idx] == SETTING_ABI_TYPE_INT) {
        // A NUMBER GETS A SPINBOX bounded by the REGISTRY's range, so it
        // can never offer a value setting_set() would refuse.
        sl->kind = CTRL_SPIN;
        int cur = 0;
        for (const char *p = g_value[idx]; *p >= '0' && *p <= '9'; p++)
            cur = cur * 10 + (*p - '0');
        uui_spinbox_init(&sl->spin, cur, g_imin[idx], g_imax[idx],
                          g_istep[idx], g_unit[idx][0] ? g_unit[idx] : 0);
        sl->baseline = uui_spinbox_value(&sl->spin);   // the NUMBER -- see struct slot
        sl->staged = sl->baseline;
        set_slot_enabled(sl, idx);
        return;
    }

    if (g_type[idx] == SETTING_ABI_TYPE_KEYCOMBO) {
        // Pressed, not spelled -- why KEYCOMBO is a type of its own.
        sl->kind = CTRL_KEYCAP;
        uui_keycapture_init(&sl->keycap, g_value[idx]);
        sl->keycap.on_arm = keycap_armed;
        sl->keycap.on_done = keycap_done;
        sl->keycap.ctx = sl;
        strlcpy(sl->baseline_buf, g_value[idx], sizeof sl->baseline_buf);
        sl->baseline = 0;
        sl->staged = 0;
        set_slot_enabled(sl, idx);
        return;
    }

    if (g_type[idx] != SETTING_ABI_TYPE_ENUM) {
        // FREE TEXT: `staged` is a changed FLAG here (see struct slot).
        sl->kind = CTRL_TEXT;
        uui_textbox_init(&sl->text, g_value[idx]);
        strlcpy(sl->baseline_buf, g_value[idx], sizeof sl->baseline_buf);
        sl->baseline = 0;
        sl->staged = 0;
        set_slot_enabled(sl, idx);
        return;
    }

    const struct usaver_opt *o = opt_of(idx);
    for (int c = 0; c < MAX_CHOICES; c++) {
        // THE RAW VALUE IS WHAT GETS STORED; the label is what is shown.
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
        strlcpy(sl->choice[c], disp, sizeof sl->choice[c]);
        if (strcmp(raw, g_value[idx]) == 0) {
            sl->baseline = c;
            sl->staged = c;
        }
        sl->choice_ptr[c] = sl->choice[c];
        sl->choice_count = c + 1;
    }
    sl->kind = pick_kind(sl, idx);

    sl->radio.options = sl->choice_ptr;
    sl->radio.count = sl->choice_count;
    sl->radio.cols = 1;
    sl->radio.selected = sl->staged;
    // init IS a dropdown's setter, and re-initing resets its popup scroll.
    uui_dropdown_init(&sl->combo, 0, 0, 0, 0, sl->choice_ptr, sl->choice_count);
    sl->combo.list.selected = sl->staged;
    uui_slider_set_options(&sl->slider, sl->choice_ptr, sl->choice_count);
    sl->slider.selected = sl->staged;
    uui_segmented_init(&sl->seg, sl->choice_ptr, sl->choice_count, sl->staged);
    uui_switch_init(&sl->sw, sl->on_idx >= 0 && sl->staged == sl->on_idx);
    uui_gallery_init(&sl->gallery, sl->choice_ptr, sl->choice_count, sl->staged);
    if (sl->kind == CTRL_GALLERY) preview_attach(sl, idx);
    set_slot_enabled(sl, idx);
}

// The value this slot would store, whichever control it is showing -- the
// one function the commit, its log line and the status text all ask.
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

// How many of the page's settings are staged but not yet applied.
int page_changes(void) {
    int n = 0;
    for (int i = 0; i < g_slot_count; i++)
        if (g_slot[i].setting >= 0 && g_slot[i].staged != g_slot[i].baseline) n++;
    return n;
}
int page_dirty(void) { return page_changes() > 0; }

int g_page_captions;

// Shows the page for group `g`: its settings in Order= then registration
// order, with advanced ones held back unless asked for.
void open_group(int g) {
    g_page_group = g;
    g_prose_fitted = 0;   // load_slot() resets every card's fit: ask again
    g_show_sysinfo = 0;
    g_show_startup = 0;
    g_show_adapters = 0;
    g_show_remote = 0;
    g_slot_count = 0;
    g_saver_slot = -1;
    g_saver_base = -1;
    preview_reset();   // a new page: the last one's decoded pictures go

    int hidden_advanced = 0, page_advanced = 0;
    // A selection sort over the group's settings: the registry's own
    // order has to survive as the tie-break.
    static int *taken;
    static int taken_cap;
    if (taken_cap < g_cap) {
        int *t = realloc(taken, (size_t)g_cap * sizeof *t);
        if (!t) return;
        taken = t;
        taken_cap = g_cap;
    }
    for (int i = 0; i < g_setting_count; i++) taken[i] = 0;
    for (;;) {
        int best = -1;
        for (int i = 0; i < g_setting_count; i++) {
            if (taken[i]) continue;
            if (strcmp(g_cat_of[i], g_group_cat[g]) != 0) continue;
            if (strcmp(group_key_of(i), g_group_key[g]) != 0) continue;
            if (g_sflags[i] & SETTING_ABI_SF_ADVANCED) {
                page_advanced++;
                if (!g_show_advanced) {
                    hidden_advanced++;
                    taken[i] = 1;
                    continue;
                }
            }
            if (best < 0 || g_order[i] < g_order[best]) best = i;
        }
        if (best < 0) break;
        taken[best] = 1;
        if (g_slot_count >= PAGE_MAX) {
            // REPORTED, never silently cut.
            snprintf(g_status, sizeof g_status,
                     "%s: too many settings for one page -- use `config list`",
                     g_group_label[g]);
            break;
        }
        load_slot(&g_slot[g_slot_count++], best);
    }

    strlcpy(g_page_title_text, g_group_label[g], sizeof g_page_title_text);
    g_page_desc_text[0] = '\0';
    struct setting_msg m;
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_GROUP_TEXT;
    snprintf(m.name, sizeof m.name, "%s/%s", g_group_cat[g], g_group_key[g]);
    if (usetting_dispatch(&m) == 0 && m.description[0])
        strlcpy(g_page_desc_text, m.description, sizeof g_page_desc_text);
    fit_rows(&g_page_desc, g_page_desc_text);

    find_requirements();

    // Found by the setting it carries, not the page's NAME, so a page
    // renamed in /etc/settings.d keeps it.
    g_test_has = 0;
    for (int i = 0; i < g_slot_count; i++)
        if (g_slot[i].setting >= 0 && strcmp(g_name[g_slot[i].setting], OWNER_SAVER) == 0)
            g_test_has = 1;

    // The chosen saver's or effect's own options, after the settings that
    // select it (set_owner.c).
    const char *own_kind = 0;
    const char *own_name = page_owner(&own_kind);
    g_page_owner_kind = own_kind;
    if (own_kind) {
        g_saver_slot = g_slot_count;
        rebuild_owner_options(own_kind, own_name);
    } else {
        // NO OWNER MEANS NO OPTIONS, and g_saver must say so -- the count
        // is read from it, and the previous page may have filled it.
        g_saver.opt_count = 0;
        g_saver.name[0] = '\0';
    }
    ulogf("settings: owner %s %s opts %d\n",
          own_kind ? own_kind : "-", own_name ? own_name : "-", g_saver.opt_count);

    g_advanced_cb.checked = g_show_advanced;
    g_advanced_cb.label = "Show advanced settings";
    // Only on a page that HAS an advanced setting. Not `|| g_show_advanced`:
    // the flag is global, so that put the toggle on every page once checked.
    g_advanced_has = page_advanced > 0;

    clock_page_opened();
    kbd_page_opened();
    snd_page_opened();
    snprintf(g_status, sizeof g_status, "%s", g_page_title_text);
    relayout_page();
    // `captions` and `disabled` are otherwise INVISIBLE to a test.
    int off = 0;
    for (int i = 0; i < g_slot_count; i++)
        if (g_slot[i].setting >= 0 && g_unavail[g_slot[i].setting][0]) off++;
    ulogf("settings: page %s/%s slots %d advanced %d captions %d disabled %d\n",
          g_group_cat[g], g_group_key[g], g_slot_count, hidden_advanced,
          g_page_captions, off);
}

// Applies every staged change on the page, and reports the OUTCOME:
// SETTING_UNSAVED is live but will not survive a reboot, which a settings
// UI must never report as plain success.
int apply_page(void) {
    int changed = 0, failed = 0, unsaved = 0, needs_reboot = 0;

    for (int i = 0; i < g_slot_count; i++) {
        struct slot *sl = &g_slot[i];
        if (sl->setting < 0 || sl->staged < 0) continue;
        if (sl->staged == sl->baseline) continue;

        // A SAVER OPTION IS A LINE IN A FILE: the write is the change.
        const struct usaver_opt *o = opt_of(sl->setting);
        if (o) {
            int ok = uconf_set(g_file[sl->setting], o->key, staged_value(sl));
            if (ok) changed++; else failed++;
            ulogf("settings: set %s %s result %s\n", g_name[sl->setting],
                  staged_value(sl), ok ? "saved" : "FAILED");
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
        snprintf(g_status, sizeof g_status, "%d change(s) were refused", failed);
    else if (unsaved)
        snprintf(g_status, sizeof g_status,
                 "Applied, but %d did not save -- they revert at the next boot", unsaved);
    else if (needs_reboot)
        snprintf(g_status, sizeof g_status,
                 "Saved -- %d change(s) take effect at the next boot", needs_reboot);
    else if (changed)
        snprintf(g_status, sizeof g_status, "Saved %d change(s)", changed);
    else
        strlcpy(g_status, "Nothing to apply", sizeof g_status);

    // Re-read: what is in effect is what the registry now says, not what
    // was asked for.
    if (changed) {
        char keep[sizeof g_status];
        strlcpy(keep, g_status, sizeof keep);
        int keep_node = uui_sidebar_selected_id(&g_tree);
        reload_settings();
        uui_sidebar_select_id(&g_tree, keep_node);
        if (g_page_group >= 0 && g_page_group < g_group_count) open_group(g_page_group);
        strlcpy(g_status, keep, sizeof g_status);   // open_group() titled it
    }
    return failed == 0;
}

// --- rows that follow another row's STAGED value -------------------------
//
// `WhenUnmet=hide` beside a Requires= that names a setting ON THE SAME
// PAGE: the row is shown only while that setting's STAGED value matches,
// so the Wallpaper page's Picture / Live / Plain colour switch swaps the
// rows under it as it is clicked, before Apply -- Windows' and KDE's
// background pages. Without the key a Requires= row greys, as before.
//
// Apply writes slots in page order and the controller sorts first, so
// the requirement is met by the time a shown row is written.
// The controlling slot PLUS ONE, so the zeroed array before any page
// opens reads as "follows nothing" rather than "follows slot 0".
static int g_req_slot[PAGE_MAX];
static char g_req_value[PAGE_MAX][SETTING_ABI_VALUE_MAX];

void find_requirements(void) {
    for (int i = 0; i < PAGE_MAX; i++) g_req_slot[i] = 0;
    for (int i = 0; i < g_slot_count; i++) {
        int k = g_slot[i].setting;
        if (k < 0 || opt_of(k)) continue;
        const char *name = g_name[k];
        size_t nl = strlen(g_ns[k]);
        if (nl && !strncmp(name, g_ns[k], nl) && name[nl] == '.') name += nl + 1;
        char word[16];
        if (!uschema_text_word(g_ns[k], name, "WhenUnmet", word, sizeof word) || strcmp(word, "hide"))
            continue;
        struct uschema s;
        if (!uschema_find(g_name[k], &s) || !s.req_name[0]) continue;
        for (int j = 0; j < g_slot_count; j++) {
            int c = g_slot[j].setting;
            if (j == i || c < 0 || strcmp(g_name[c], s.req_name)) continue;
            g_req_slot[i] = j + 1;
            strlcpy(g_req_value[i], s.req_value, sizeof g_req_value[i]);
            // ITS VISIBILITY SAYS IT. Greyed as well, a row shown because
            // the switch was just moved would be shown disabled until Apply.
            g_unavail[k][0] = '\0';
            set_slot_enabled(&g_slot[i], k);
        }
    }
}

int slot_hidden(int i) {
    if (i < 0 || i >= g_slot_count) return 0;
    // An owner's synthesised options go wherever the owner goes.
    if (g_saver_slot >= 0 && i >= g_saver_slot) {
        for (int o = 0; o < g_saver_slot; o++)
            if (g_slot[o].setting >= 0 && owner_kind(g_slot[o].setting)) return slot_hidden(o);
        return 0;
    }
    int j = g_req_slot[i] - 1;
    return j >= 0 && strcmp(staged_value(&g_slot[j]), g_req_value[i]) != 0;
}

static int controls_rows(int slot_index) {
    for (int i = 0; i < g_slot_count; i++)
        if (g_req_slot[i] == slot_index + 1) return 1;
    return 0;
}

// --- the page's items ---------------------------------------------------

struct uui_item PAGE[PAGE_ITEMS];
int PAGE_COUNT;
struct uui_layout PAGE_LAYOUT;
struct uui_scrollview PAGE_SCROLL;

// THE FOCUS RING: search, sidebar, the page's controls, then the footer's
// buttons -- everything, in reading order (settings.c fills the ends).
struct uui_focusable FOCUS[FOCUS_MAX];
int FOCUS_COUNT;
struct uui_focus PAGE_FOCUS;

// The control item a slot shows, and its focus entry.
static struct uui_item slot_control(struct slot *sl, int i, struct uui_focusable *f) {
    struct uui_item it = { .id = ID_CONTROL_BASE + i };
    switch (sl->kind) {
    case CTRL_COMBO:     it.ops = &uui_dropdown_ops;    it.widget = &sl->combo;  break;
    case CTRL_SLIDER:    it.ops = &uui_slider_ops;      it.widget = &sl->slider;
                         it.flags = UUI_FILL_W; break;
    // A field has no natural width at all, so it is stretched.
    case CTRL_TEXT:      it.ops = &uui_textbox_ops;     it.widget = &sl->text;
                         it.flags = UUI_FILL_W; break;
    case CTRL_KEYCAP:    it.ops = &uui_keycapture_ops;  it.widget = &sl->keycap; break;
    case CTRL_SPIN:      it.ops = &uui_spinbox_ops;     it.widget = &sl->spin;   break;
    case CTRL_SWITCH:    it.ops = &uui_switch_ops;      it.widget = &sl->sw;     break;
    case CTRL_SEGMENTED: it.ops = &uui_segmented_ops;   it.widget = &sl->seg;    break;
    // Cards across the card's whole width, under its text.
    case CTRL_GALLERY:   it.ops = &uui_gallery_ops;     it.widget = &sl->gallery;
                         it.flags = UUI_FILL_W; break;
    default:             it.ops = &uui_radio_list_ops;  it.widget = &sl->radio;  break;
    }
    *f = (struct uui_focusable){ it.widget, it.ops };
    return it;
}

// ONE SLOT'S CARD, written into a caller's array -- the page and the
// options dialog build the same cards from the same slots.
//
// The card's fit state (desc_rows, stacked_auto) is NOT reset here: this
// runs on every relayout, and resetting it would undo the fit that asked
// for the relayout.
int emit_slot(struct uui_item *out, int n, int i,
              struct uui_focusable *focus, int *nfocus) {
    struct slot *sl = &g_slot[i];
    struct uui_setting_row *r = &sl->row;
    r->control = slot_control(sl, i, &focus[(*nfocus)++]);
    r->title = sl->setting >= 0 ? g_label[sl->setting] : "";
    r->desc = slot_prose(sl->setting);
    r->stacked = sl->kind == CTRL_RADIO || sl->kind == CTRL_GALLERY;
    r->changed = sl->setting >= 0 && sl->staged != sl->baseline;
    out[n++] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = r,
                                  .flags = UUI_FILL_W };
    g_page_captions++;
    return n;
}

static struct uui_custom g_monitor;
static void draw_monitor(struct ugfx_surface *s, const struct uui_custom *c) {
    preview_monitor(s, c->x, c->y, c->w, c->h, staged_value((struct slot *)c->state));
}

// DISPLAY > SCREEN'S MONITOR, above Resolution (mockup D1): the staged
// mode, placed on the panel as the staged Scaling places it. A scaling
// row the kernel calls unavailable means no scaler.
#define SET_RESOLUTION "system.resolution"
#define SET_SCALING    "system.scaling"
static struct uui_custom g_screen;
static struct preview_screen g_screen_ps;

static struct slot *slot_named(const char *name) {
    for (int i = 0; i < g_slot_count; i++)
        if (g_slot[i].setting >= 0 && !strcmp(g_name[g_slot[i].setting], name)) return &g_slot[i];
    return 0;
}

static void draw_screen(struct ugfx_surface *s, const struct uui_custom *c) {
    struct slot *res = slot_named(SET_RESOLUTION), *sc = slot_named(SET_SCALING);
    if (!res) return;
    char *e;
    const char *v = staged_value(res);
    long mw = strtol(v, &e, 10);
    if (*e != 'x') return;
    long mh = strtol(e + 1, &e, 10);
    if (*e || mw <= 0 || mh <= 0) return;
    g_screen_ps.mode_w = (int)mw;
    g_screen_ps.mode_h = (int)mh;
    g_screen_ps.can_scale = sc && !slot_disabled(sc);
    g_screen_ps.scaling = sc ? staged_value(sc) : "full";
    preview_screen(s, c->x, c->y, c->w, c->h, &g_screen_ps);
    uapp_logf_layout("settings: screen %s\n", g_screen_ps.caption);
}

static void emit_screen(struct uui_item *out, int *n) {
    struct query_display d;
    memset(&d, 0, sizeof d);
    sys_query_record(QUERY_DISPLAY, 0, &d, sizeof d);
    g_screen_ps.panel_w = (int)d.native_width;
    g_screen_ps.panel_h = (int)d.native_height;
    g_screen = (struct uui_custom){ .w = 0, .h = ugfx_char_h() * 12, .draw = draw_screen };
    out[(*n)++] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &g_screen,
                                     .flags = UUI_FILL_W, .name = "screen_monitor" };
}

void relayout_page(void) {
    int n = 0;
    focus_ring_open();          // settings.c: remember what has focus
    FOCUS_COUNT = FOCUS_LEAD;   // settings.c's search and sidebar go first
    PAGE[n++] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_page_title,
                                   .flags = UUI_FILL_W };
    if (g_page_desc_text[0])
        PAGE[n++] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_page_desc,
                                       .flags = UUI_FILL_W };

    g_page_captions = 0;
    if (!g_show_sysinfo && !g_show_startup && !g_show_adapters && !g_show_remote)
        n = clock_emit_top(PAGE, n);
    if (g_show_startup) n = startup_emit(PAGE, n, FOCUS, &FOCUS_COUNT);
    if (g_show_adapters) n = adapters_emit(PAGE, n, FOCUS, &FOCUS_COUNT);
    if (g_show_remote) n = remote_emit(PAGE, n, FOCUS, &FOCUS_COUNT);
    // SYSTEM INFORMATION is one drawn item and two buttons (set_sysinfo.c).
    if (g_show_sysinfo) {
        static struct uui_item si_btns[2];
        static struct uui_layout si_row;
        g_si_view.h = sysinfo_height();
        g_si_view.w = 0;
        PAGE[n++] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &g_si_view,
                                       .flags = UUI_FILL_W };
        si_btns[0] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_si_devmgr,
                                        .id = ID_SI_DEVMGR, .name = "si_devmgr" };
        si_btns[1] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_si_copy,
                                        .id = ID_SI_COPY, .name = "si_copy" };
        si_row = (struct uui_layout){ .dir = UUI_ROW, .items = si_btns, .count = 2 };
        PAGE[n++] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &si_row };
        FOCUS[FOCUS_COUNT++] = (struct uui_focusable){ &g_si_devmgr, &uui_button_ops };
        FOCUS[FOCUS_COUNT++] = (struct uui_focusable){ &g_si_copy, &uui_button_ops };
        g_si_debug_cb.checked = g_show_debug;
        PAGE[n++] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_si_debug,
                                       .flags = UUI_FILL_W };
        FOCUS[FOCUS_COUNT++] = (struct uui_focusable){ &g_si_debug_cb, &uui_checkbox_ops };
    }
    // An effect's options live in its Settings... dialog, not on the page.
    int page_slots = (g_saver_slot >= 0 && owner_uses_dialog(g_page_owner_kind))
                     ? g_saver_slot : g_slot_count;
    if (slot_named(SET_RESOLUTION)) emit_screen(PAGE, &n);   // the page's top, as D1
    for (int i = 0; i < page_slots; i++) {
        struct slot *sl = &g_slot[i];
        if (slot_hidden(i)) continue;
        // THE SAVER GALLERY'S MONITOR, above it: the chosen saver, big.
        if (sl->setting >= 0 && sl->kind == CTRL_GALLERY && !strcmp(g_name[sl->setting], OWNER_SAVER)) {
            g_monitor = (struct uui_custom){ .w = 0, .h = ugfx_char_h() * 11, .draw = draw_monitor, .state = sl };
            PAGE[n++] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &g_monitor,
                                           .flags = UUI_FILL_W, .name = "saver_monitor" };
        }
        int kn = kbd_emit_slot(PAGE, n, i, FOCUS, &FOCUS_COUNT);
        n = kn >= 0 ? kn : emit_slot(PAGE, n, i, FOCUS, &FOCUS_COUNT);
        n = clock_emit_after(PAGE, n, i, FOCUS, &FOCUS_COUNT);
        n = kbd_emit_after(PAGE, n, i, FOCUS, &FOCUS_COUNT);
        n = snd_emit_after(PAGE, n, i, FOCUS, &FOCUS_COUNT);
        // Test, under the saver it previews (Windows puts Preview there).
        if (g_test_has && sl->setting >= 0 && strcmp(g_name[sl->setting], OWNER_SAVER) == 0) {
            PAGE[n++] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_test_btn,
                                           .id = ID_TEST };
            FOCUS[FOCUS_COUNT++] = (struct uui_focusable){ &g_test_btn, &uui_button_ops };
        }
        // Settings..., under the effect it configures -- only when there
        // is something to configure.
        if (sl->setting >= 0 && owner_uses_dialog(owner_kind(sl->setting)) &&
            g_saver.opt_count > 0) {
            PAGE[n++] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_opts_btn,
                                           .id = ID_OPTS };
            FOCUS[FOCUS_COUNT++] = (struct uui_focusable){ &g_opts_btn, &uui_button_ops };
        }
    }
    PAGE[n++] = (struct uui_item){ .ops = &uui_checkbox_ops, .widget = &g_advanced_cb,
                                   .id = ID_ADVANCED, .hidden = !g_advanced_has };
    if (g_advanced_has)
        FOCUS[FOCUS_COUNT++] = (struct uui_focusable){ &g_advanced_cb, &uui_checkbox_ops };
    PAGE_COUNT = n;
    PAGE_LAYOUT.count = n;
    focus_ring_close();   // settings.c: the footer's buttons, then init
    uui_scrollview_content_changed(&PAGE_SCROLL);
}

// A CONTROL MOVED: stage its value, mark its card, and -- for the control
// that owns a page's options -- rebuild them. Shared with the options
// dialog: staging is the slot's, not the surface's. Returns 1 when the
// page's item list has to be rebuilt.
int control_changed(int slot_index) {
    int relayout = 0;
    struct slot *sl = &g_slot[slot_index];
    // A spinbox reports the NUMBER and a field or capture whether it
    // DIFFERS; everything else an index (see struct slot).
    switch (sl->kind) {
    case CTRL_KEYCAP:    sl->staged = strcmp(sl->keycap.text, sl->baseline_buf) != 0; break;
    case CTRL_TEXT:      sl->staged = strcmp(uui_textbox_text(&sl->text), sl->baseline_buf) != 0; break;
    case CTRL_COMBO:     sl->staged = uui_dropdown_selected(&sl->combo); break;
    case CTRL_SLIDER:    sl->staged = sl->slider.selected; break;
    case CTRL_SPIN:      sl->staged = uui_spinbox_value(&sl->spin); break;
    case CTRL_SWITCH:    sl->staged = sl->sw.on ? sl->on_idx : 1 - sl->on_idx; break;
    case CTRL_SEGMENTED: sl->staged = sl->seg.selected; break;
    case CTRL_GALLERY:   sl->staged = sl->gallery.selected; break;
    default:             sl->staged = sl->radio.selected; break;
    }
    sl->row.changed = sl->setting >= 0 && sl->staged != sl->baseline;
    region_preview_refresh();   // the preview shows what Apply would give
    if (sl->setting >= 0 && sl->staged >= 0) {
        snprintf(g_status, sizeof g_status, "%s -> %s   (not applied yet)",
                 g_label[sl->setting], staged_value(sl));
        // LOGGED as well as shown: a test asserts on facts, not pixels.
        ulogf("settings: staged %s %s\n", g_name[sl->setting], staged_value(sl));
    }
    // A SWITCH OTHER ROWS FOLLOW (WhenUnmet=hide) changes which are shown.
    if (controls_rows(slot_index)) {
        g_prose_fitted = 0;
        relayout = 1;
    }
    // PICKING A SAVER CHANGES WHAT IS BELOW IT.
    if (sl->setting >= 0 && !opt_of(sl->setting) && owner_kind(sl->setting)) {
        rebuild_owner_options(owner_kind(sl->setting), staged_value(sl));
        g_prose_fitted = 0;   // the new cards have never been fitted
        relayout = 1;
    }
    return relayout;
}

// Re-fits the page description and every card on the page to the widths
// the last layout gave them, and relayouts if any answer changed. Safe
// as a feedback loop: the answers change HEIGHT only, and width never
// depends on height, so the second pass settles.
int refit_prose(void) {
    int changed = 0;
    int was = g_page_desc.rows;
    fit_rows(&g_page_desc, g_page_desc_text);
    if (g_page_desc.rows != was) changed = 1;
    // The page fits what the PAGE shows; option cards in the dialog are
    // fitted by the dialog, at its width.
    int page_slots = (g_saver_slot >= 0 && owner_uses_dialog(g_page_owner_kind))
                     ? g_saver_slot : g_slot_count;
    for (int i = 0; i < page_slots; i++)
        if (g_slot[i].setting >= 0 && uui_setting_row_fit(&g_slot[i].row)) changed = 1;
    if (g_show_sysinfo && uui_setting_row_fit(&g_si_debug)) changed = 1;
    if (g_show_startup && startup_fit()) changed = 1;
    if (g_show_adapters && adapters_fit()) changed = 1;
    if (g_show_remote && remote_fit()) changed = 1;
    if (changed) relayout_page();
    return changed;
}

// --- what a test reads ----------------------------------------------------

static const struct uui_item *slot_item(const struct slot *sl) { return &sl->row.control; }

void slot_rect(const struct slot *sl, int *x, int *y, int *w, int *h) {
    const struct uui_item *it = slot_item(sl);
    *x = *y = *w = *h = 0;
    if (it->ops && it->ops->bounds) it->ops->bounds(it->widget, x, y, w, h);
}

const char *slot_kind_name(const struct slot *sl) {
    switch (sl->kind) {
    case CTRL_COMBO:     return "combo";
    case CTRL_SLIDER:    return "slider";
    case CTRL_SPIN:      return "spin";
    case CTRL_TEXT:      return "text";
    case CTRL_KEYCAP:    return "keycap";
    case CTRL_SWITCH:    return "switch";
    case CTRL_SEGMENTED: return "segmented";
    case CTRL_GALLERY:   return "gallery";
    default:             return "radio";
    }
}

// A key-capture control has no disabled state of its own.
int slot_disabled(const struct slot *sl) {
    switch (sl->kind) {
    case CTRL_COMBO:     return sl->combo.disabled;
    case CTRL_SLIDER:    return sl->slider.disabled;
    case CTRL_SPIN:      return sl->spin.disabled;
    case CTRL_TEXT:      return sl->text.disabled;
    case CTRL_KEYCAP:    return 0;
    case CTRL_SWITCH:    return sl->sw.disabled;
    case CTRL_SEGMENTED: return sl->seg.disabled;
    case CTRL_GALLERY:   return sl->gallery.disabled;
    default:             return sl->radio.disabled;
    }
}
