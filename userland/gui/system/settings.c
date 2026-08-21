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
#include <stdint.h>
#include <stdarg.h>
#include "rt/sys.h"
#include "lib/string.h"
#include "lib/stdio.h"
#include "ui/uapp.h"
#include "ui/uui.h"
#include "ui/uui_layout.h"
#include "ui/uui_sidebar.h"
#include "ui/uui_label.h"
#include "ui/uui_radio_list.h"
#include "ui/uui_dropdown.h"
#include "ui/uui_slider.h"
#include "ui/uui_checkbox.h"
#include "ui/uui_button.h"
#include "ui/uui_button_group.h"
#include "ui/uui_statusbar.h"
#include "ui/uui_scrollview.h"
#include "ui/utheme.h"
#include "setting_abi.h"
#include "cpuinfo.h"
#include "version.h"

#define MAX_SETTINGS   SETTING_ABI_MAX
// Room for every timezone the kernel ships plus hand-added rows.
#define MAX_CHOICES    128
#define MAX_CATEGORIES 12
#define MAX_GROUPS     16
// Controls on one page. A group larger than this would be a page nobody
// can take in anyway; the overflow is REPORTED rather than silently cut.
#define PAGE_MAX       6

// Above this many choices a page uses a DROPDOWN rather than radio
// buttons, unless /etc/settings.d says otherwise. Few mutually-exclusive
// options are better all visible; ninety-two timezones are not.
#define CHOICES_DROPDOWN_MIN 7

// struct slot's `kind`.
enum { CTRL_RADIO = 0, CTRL_COMBO, CTRL_SLIDER };

enum { ID_TREE = 1, ID_BODY, ID_PAGE, ID_ADVANCED, ID_BUTTONS, ID_STATUS,
       ID_CONTROL_BASE = 100 }; // + slot, so a control names its own row

// NON-ZERO on purpose: uui_button_group_take_activated() returns 0 for
// "nothing committed", so a button coded 0 could never be told apart
// from a press that was dragged off.
enum { BTN_OK = 201, BTN_APPLY = 202, BTN_CANCEL = 203 };

#define NODE_SYSINFO       1
#define NODE_CATEGORY_BASE 1000
#define NODE_GROUP_BASE    2000

// --- what the registry says ------------------------------------------

static char     g_label[MAX_SETTINGS][SETTING_ABI_LABEL_MAX];
static char     g_desc[MAX_SETTINGS][SETTING_ABI_DESC_MAX];
// The QUALIFIED name ("system.font_size"): a setting's identity is
// (namespace, name), so the bare key is not necessarily usable alone.
static char     g_name[MAX_SETTINGS][SETTING_ABI_QUALIFIED_MAX];
static char     g_ns[MAX_SETTINGS][SETTING_ABI_NS_MAX];
static char     g_file[MAX_SETTINGS][SETTING_ABI_FILE_MAX];
static char     g_value[MAX_SETTINGS][SETTING_ABI_VALUE_MAX];
static char     g_cat_of[MAX_SETTINGS][SETTING_ABI_CATEGORY_MAX];
static char     g_group_of[MAX_SETTINGS][SETTING_ABI_CATEGORY_MAX];
static uint32_t g_type[MAX_SETTINGS];
static uint32_t g_widget[MAX_SETTINGS];
static uint32_t g_sflags[MAX_SETTINGS];
static int      g_order[MAX_SETTINGS];
static int      g_setting_count;
static uint32_t g_generation;

// The sidebar: categories, and the pages under them.
static char g_cat[MAX_CATEGORIES][SETTING_ABI_CATEGORY_MAX];
static int  g_cat_count;
static char g_group_cat[MAX_GROUPS][SETTING_ABI_CATEGORY_MAX];
static char g_group_key[MAX_GROUPS][SETTING_ABI_CATEGORY_MAX];
static char g_group_label[MAX_GROUPS][SETTING_ABI_LABEL_MAX];
static int  g_group_count;

static struct uui_sidebar_row g_nodes[MAX_CATEGORIES + MAX_GROUPS + 1];
static int g_node_count;

static char g_status[160];
static int  g_loaded;
static int  g_show_sysinfo;

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
static int  g_prose_fitted;
static int  g_show_advanced;
static int  g_page_group = -1;
// Does the current page have anything the toggle would reveal?
static int  g_advanced_has;
// The last control geometry reported, so a CHANGE is what triggers the
// next report -- a page change and a SCROLL alike.
//
// It was a flag set on page change, which reported the previous page's
// rects (on_draw runs before the layout places a new page) and said
// nothing at all when the page scrolled. A tool driving a control below
// the fold then had no idea where it had moved to, and a scroll view
// correctly refuses to route a press to a child outside its viewport --
// so the control was simply unreachable and looked dead.
static int g_last_y[PAGE_MAX];
static int g_last_reported_count = -1;

// --- the page's controls ---------------------------------------------
//
// One SLOT per control, each holding both presentations; `hidden` picks
// one. Declaring both and hiding one beats rewriting an item's ops at
// runtime -- `hidden` already removes a widget from drawing AND
// hit-testing, and swapping an ops pointer underneath a router that may
// hold a pointer grab is a different kind of problem.
struct slot {
    int setting;               // index into the arrays above, or -1
    struct uui_label     caption;
    struct uui_label     explain;
    struct uui_radio_list radio;
    struct uui_dropdown   combo;
    struct uui_slider     slider;
    // Which of the three is showing. A KIND rather than a pair of flags:
    // two booleans can express "both" and "neither", and neither is a
    // state this page has.
    int kind;
    // The staged selection, and the value the page opened with. The
    // baseline is what the "(current)" marker names, which is what makes
    // an accidental click visible before it is committed.
    int staged;
    int baseline;
    int choice_count;
    char choice[MAX_CHOICES][SETTING_ABI_VALUE_MAX];      // display names
    char choice_raw[MAX_CHOICES][SETTING_ABI_VALUE_MAX];  // what gets stored
    const char *choice_ptr[MAX_CHOICES];
};
static struct slot g_slot[PAGE_MAX];
static int g_slot_count;

static struct uui_label     g_page_title;
static struct uui_label     g_page_desc;
static struct uui_checkbox  g_advanced_cb;
static struct uui_sidebar   g_tree;
static struct uui_button    g_btn[3];
static struct uui_button_group g_buttons;
static struct uui_statusbar g_status_bar;

static char g_page_title_text[SETTING_ABI_LABEL_MAX];
static char g_page_desc_text[SETTING_ABI_DESC_MAX];

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...) {
    // stderr, not stdout: a windowed client has no terminal, and the
    // kernel routes stderr to the log where a test tool can read it.
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    sys_eprint(buf);
}

// --- reading the registry --------------------------------------------

// The PAGE a setting belongs to. A setting with no group gets one of its
// own, named by its label -- which is what every setting did before
// groups existed, so nothing had to be edited to keep working.
static const char *group_key_of(int i) {
    return g_group_of[i][0] ? g_group_of[i] : g_label[i];
}

static void rebuild_sidebar(void) {
    g_cat_count = 0;
    g_group_count = 0;
    g_node_count = 0;

    // Categories and pages in FIRST-SEEN order. Registration order is
    // the kernel's boot order, which groups related settings already; a
    // sort would put "Appearance" above "Startup" on one boot and leave
    // a ring-3 program's category wherever the alphabet says on the
    // next, and a sidebar that reorders itself is one nobody builds
    // muscle memory for.
    for (int i = 0; i < g_setting_count; i++) {
        int c = -1;
        for (int j = 0; j < g_cat_count; j++)
            if (strcmp(g_cat[j], g_cat_of[i]) == 0) { c = j; break; }
        if (c < 0 && g_cat_count < MAX_CATEGORIES) {
            c = g_cat_count++;
            strlcpy(g_cat[c], g_cat_of[i], sizeof g_cat[c]);
        }
        const char *key = group_key_of(i);
        int g = -1;
        for (int j = 0; j < g_group_count; j++)
            if (strcmp(g_group_cat[j], g_cat_of[i]) == 0 &&
                strcmp(g_group_key[j], key) == 0) { g = j; break; }
        if (g < 0 && g_group_count < MAX_GROUPS) {
            g = g_group_count++;
            strlcpy(g_group_cat[g], g_cat_of[i], sizeof g_group_cat[g]);
            strlcpy(g_group_key[g], key, sizeof g_group_key[g]);
            // The page's own label, if /etc/settings.d gives it one --
            // otherwise the group key, which is already a human word.
            struct setting_msg m;
            memset(&m, 0, sizeof m);
            m.op = SETTING_OP_GROUP_TEXT;
            snprintf(m.name, sizeof m.name, "%s/%s", g_cat_of[i], key);
            if (sys_setting(&m) == 0 && m.label[0])
                strlcpy(g_group_label[g], m.label, sizeof g_group_label[g]);
            else
                strlcpy(g_group_label[g], key, sizeof g_group_label[g]);
        }
    }

    for (int c = 0; c < g_cat_count; c++) {
        if (g_node_count >= (int)(sizeof g_nodes / sizeof g_nodes[0])) break;
        // A HEADING, not a row: a category is a caption over the pages
        // beneath it and is not itself a destination, so it cannot be
        // selected and the arrow keys step over it. That is the whole
        // reason this is a uui_sidebar and not a uui_tree -- see that
        // header. It keeps its id anyway, purely so the debug dump
        // below can name it.
        g_nodes[g_node_count++] = (struct uui_sidebar_row){
            .label = g_cat[c], .kind = UUI_SIDEBAR_HEADING,
            .id = NODE_CATEGORY_BASE + c
        };
        for (int g = 0; g < g_group_count; g++) {
            if (strcmp(g_group_cat[g], g_cat[c]) != 0) continue;
            if (g_node_count >= (int)(sizeof g_nodes / sizeof g_nodes[0])) break;
            g_nodes[g_node_count++] = (struct uui_sidebar_row){
                .label = g_group_label[g], .kind = UUI_SIDEBAR_ITEM,
                .id = NODE_GROUP_BASE + g
            };
        }
    }
    // AN ITEM WITH NO HEADING OVER IT. It is a page, so it has to be
    // reachable -- a heading could not be selected and the page would
    // become dead. It sits unindented-looking at the end rather than
    // inventing a one-page category to hold it.
    if (g_node_count < (int)(sizeof g_nodes / sizeof g_nodes[0]))
        g_nodes[g_node_count++] = (struct uui_sidebar_row){
            .label = "System Information", .kind = UUI_SIDEBAR_ITEM,
            .id = NODE_SYSINFO
        };

    uui_sidebar_set_rows(&g_tree, g_nodes, g_node_count);
}

static int reload_settings(void) {
    struct setting_msg m;
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_COUNT;
    if (sys_setting(&m) != 0) { g_setting_count = 0; return 0; }

    int n = m.count;
    if (n > MAX_SETTINGS) n = MAX_SETTINGS;
    g_setting_count = 0;

    for (int i = 0; i < n; i++) {
        memset(&m, 0, sizeof m);
        m.op = SETTING_OP_INFO;
        m.index = i;
        if (sys_setting(&m) != 0) continue;

        int k = g_setting_count;
        strlcpy(g_label[k], m.label, sizeof g_label[k]);
        strlcpy(g_desc[k],  m.description, sizeof g_desc[k]);
        strlcpy(g_ns[k],    m.ns,    sizeof g_ns[k]);
        if (m.ns[0]) snprintf(g_name[k], sizeof g_name[k], "%s.%s", m.ns, m.name);
        else         strlcpy(g_name[k], m.name, sizeof g_name[k]);
        strlcpy(g_file[k],  m.file,  sizeof g_file[k]);
        strlcpy(g_value[k], m.value, sizeof g_value[k]);
        strlcpy(g_cat_of[k],   m.category, sizeof g_cat_of[k]);
        strlcpy(g_group_of[k], m.group,    sizeof g_group_of[k]);
        g_type[k]   = m.type;
        g_widget[k] = m.widget;
        g_sflags[k] = m.sflags;
        g_order[k]  = m.order;
        g_setting_count++;
    }
    g_generation = m.generation;

    // The NAMESPACE is appended only where it is needed to tell two rows
    // apart. Every label used to carry it unconditionally, which put the
    // same word on every row -- distinguishing nothing while costing
    // width. It cannot simply be dropped: two programs may own settings
    // with the same human label, and identical rows would be unusable.
    //
    // TWO PASSES, because appending while still comparing would suffix
    // only the FIRST of a clashing pair.
    int clash[MAX_SETTINGS];
    for (int i = 0; i < g_setting_count; i++) {
        clash[i] = 0;
        for (int j = 0; j < g_setting_count && !clash[i]; j++)
            if (j != i && strcmp(g_label[i], g_label[j]) == 0) clash[i] = 1;
    }
    for (int i = 0; i < g_setting_count; i++) {
        if (!clash[i] || !g_ns[i][0]) continue;
        char q[SETTING_ABI_LABEL_MAX];
        snprintf(q, sizeof q, "%s  (%s)", g_label[i], g_ns[i]);
        strlcpy(g_label[i], q, sizeof g_label[i]);
    }

    rebuild_sidebar();
    return g_setting_count;
}

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
static void fit_rows(struct uui_label *l, const char *text) {
    int need = (text && text[0] && l->w > 0 && ugfx_text_width(text) > l->w) ? 2 : 1;
    uui_label_set_wrap(l, need);
}

static void load_slot(struct slot *sl, int idx) {
    sl->setting = idx;
    sl->choice_count = 0;
    sl->staged = -1;
    sl->baseline = -1;

    uui_label_set_text(&sl->caption, g_label[idx]);
    // Empty is fine and common: /etc/settings.d is optional, and a
    // setting with no file simply has no description. The label reserves
    // its row either way, so a page does not reflow when text appears.
    uui_label_set_text(&sl->explain, g_desc[idx]);
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
    fit_rows(&sl->explain, g_desc[idx]);

    if (g_type[idx] != SETTING_ABI_TYPE_ENUM) {
        sl->kind = CTRL_RADIO;
        // Free text has no choices to offer. Shown as an empty control
        // with an explanation rather than omitted, so the row does not
        // look broken -- editing one needs a text field, which is why
        // `config set` exists for these.
        sl->radio.options = 0;
        sl->radio.count = 0;
        return;
    }

    for (int c = 0; c < MAX_CHOICES; c++) {
        struct setting_msg m;
        memset(&m, 0, sizeof m);
        m.op = SETTING_OP_CHOICE;
        m.index = idx;
        m.choice = c;
        if (sys_setting(&m) != 0) break; // past the last one

        // The RAW value is what gets stored; `label` is what is shown,
        // and the kernel falls it back to the value, so this never has
        // to decide. Applying the displayed string would try to set the
        // timezone to "Los Angeles".
        strlcpy(sl->choice_raw[c], m.value, sizeof sl->choice_raw[c]);
        if (strcmp(m.value, g_value[idx]) == 0) {
            // THE MARKER, baked into the option's text: uui_radio_list
            // and uui_dropdown both take plain strings, and a "mark this
            // row" hook on each would be a widget feature with one
            // caller. If a second app wants it, that is when it earns a
            // place in the widgets.
            snprintf(sl->choice[c], sizeof sl->choice[c], "%s   (current)", m.label);
            sl->baseline = c;
            sl->staged = c;
        } else {
            strlcpy(sl->choice[c], m.label, sizeof sl->choice[c]);
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
}

// Does this page have anything staged but not yet applied?
static int page_dirty(void) {
    for (int i = 0; i < g_slot_count; i++)
        if (g_slot[i].setting >= 0 && g_slot[i].staged != g_slot[i].baseline)
            return 1;
    return 0;
}

// Declared here; defined below with the rest of the layout.
static void relayout_page(void);

// Shows the page for group `g`. Its settings, in Order= then
// registration order, with advanced ones held back unless asked for.
static void open_group(int g) {
    g_page_group = g;
    g_show_sysinfo = 0;
    g_slot_count = 0;

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
    if (sys_setting(&m) == 0 && m.description[0])
        strlcpy(g_page_desc_text, m.description, sizeof g_page_desc_text);

    g_advanced_cb.checked = g_show_advanced;
    // The checkbox appears only where there is something to reveal --
    // an "advanced" toggle on a page with no advanced settings is a
    // control that does nothing.
    g_advanced_cb.label = "Show advanced settings";
    g_advanced_has = hidden_advanced > 0 || g_show_advanced;

    if (!g_status[0] || g_page_group == g)
        snprintf(g_status, sizeof g_status, "%s", g_page_title_text);
    relayout_page();
    logf_("settings: page %s/%s slots %d advanced %d\n",
          g_group_cat[g], g_group_key[g], g_slot_count, hidden_advanced);
}

// Applies every staged change on the page. Reports the OUTCOME rather
// than assuming it worked: SETTING_UNSAVED means the change is live but
// will not survive a reboot, which is the one thing a settings UI must
// never report as plain success.
static int apply_page(void) {
    int changed = 0, failed = 0, unsaved = 0, needs_reboot = 0;

    for (int i = 0; i < g_slot_count; i++) {
        struct slot *sl = &g_slot[i];
        if (sl->setting < 0 || sl->staged < 0) continue;
        if (sl->staged == sl->baseline) continue;

        struct setting_msg m;
        memset(&m, 0, sizeof m);
        m.op = SETTING_OP_SET;
        strlcpy(m.name, g_name[sl->setting], sizeof m.name);
        strlcpy(m.value, sl->choice_raw[sl->staged], sizeof m.value);
        if (sys_setting(&m) != 0) { failed++; continue; }

        logf_("settings: set %s %s result %u\n", g_name[sl->setting],
              sl->choice_raw[sl->staged], m.result);
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

// --- the System Information page --------------------------------------

static struct cpu_info g_cpu;
static int g_cpu_loaded;

static void draw_sysinfo(struct ugfx_surface *s, int x, int y, int w, int h) {
    int line_h = ugfx_char_h() + 6;
    int row = 0;
    char line[128];

    if (!g_cpu_loaded) { sys_cpu_info(&g_cpu); g_cpu_loaded = 1; }

    struct sys_info si;
    memset(&si, 0, sizeof si);
    sys_sysinfo(&si);

    // Two budgets, and the second is easy to forget: the clipped draw
    // bounds WIDTH only, so a row past the bottom would simply be drawn
    // wherever it landed. Stop on a whole line rather than one sliced
    // through its glyphs.
    #define INFO_LINE(str) do { \
        int ly = y + row * line_h; \
        if (ly + ugfx_char_h() > y + h) break; \
        ugfx_draw_string_clipped(s, x, ly, w, (str), UTHEME_TEXT, UTHEME_PANEL_BG); \
        row++; \
    } while (0)

    INFO_LINE("toy-os v" TOYOS_VERSION_FULL);

    const char *brand = g_cpu.brand;
    while (*brand == ' ') brand++;
    if (!*brand) brand = g_cpu.vendor;
    snprintf(line, sizeof line, "CPU:     %s", brand);
    INFO_LINE(line);
    snprintf(line, sizeof line, "Vendor:  %s", g_cpu.vendor);
    INFO_LINE(line);
    snprintf(line, sizeof line, "Ident:   family %u, model %u, stepping %u",
             (unsigned)g_cpu.family, (unsigned)g_cpu.model, (unsigned)g_cpu.stepping);
    INFO_LINE(line);
    if (g_cpu.mhz) {
        snprintf(line, sizeof line, "Speed:   ~%u MHz (%s)", (unsigned)g_cpu.mhz,
                 g_cpu.mhz_source == CPU_MHZ_CPUID_16H ? "CPUID" : "measured");
    } else {
        snprintf(line, sizeof line, "Speed:   unknown");
    }
    INFO_LINE(line);
    snprintf(line, sizeof line, "Enabled: SSE %s  NX %s  SMEP %s  SMAP %s",
             (g_cpu.enabled & CPU_EN_SSE) ? "on" : "off",
             (g_cpu.enabled & CPU_EN_NX) ? "on" : "off",
             (g_cpu.enabled & CPU_EN_SMEP) ? "on" : "off",
             (g_cpu.enabled & CPU_EN_SMAP) ? "on" : "off");
    INFO_LINE(line);
    snprintf(line, sizeof line, "Memory:  %u KB free of %u KB",
             (unsigned)si.mem_free_kb, (unsigned)si.mem_total_kb);
    INFO_LINE(line);
    if (si.flags & SYS_INFO_DISK_VALID) {
        snprintf(line, sizeof line, "Disk:    %u MB used of %u MB",
                 (unsigned)(si.disk_used_bytes / (1024 * 1024)),
                 (unsigned)(si.disk_total_bytes / (1024 * 1024)));
    } else {
        snprintf(line, sizeof line, "Disk:    unavailable");
    }
    INFO_LINE(line);
    snprintf(line, sizeof line, "PCI:     %u device(s)", (unsigned)sys_pci_count());
    INFO_LINE(line);
    snprintf(line, sizeof line, "Uptime:  %u s",
             (unsigned)(sys_monotonic_ns() / 1000000000ull));
    INFO_LINE(line);

    #undef INFO_LINE
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

// title, description, then caption/explain/radio/combo per slot, then
// the advanced toggle.
#define PAGE_ITEMS (2 + PAGE_MAX * 4 + 1)
static struct uui_item PAGE[PAGE_ITEMS];
static int PAGE_COUNT;
static struct uui_layout PAGE_LAYOUT;
static struct uui_scrollview PAGE_SCROLL;

static struct uui_item ITEMS_BODY[2];
static struct uui_layout BODY_LAYOUT;
static struct uui_item ITEMS[3];
static struct uui_layout LAYOUT;

// Rebuilds the page's item list for whatever is currently on it. The
// count is DERIVED from what was added, never written out: a literal
// count that disagreed with its array once walked the router one item
// past the end into a garbage ops table.
static void relayout_page(void) {
    int n = 0;
    PAGE[n++] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_page_title,
                                    .id = 0, .flags = UUI_FILL_W };
    PAGE[n++] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_page_desc,
                                    .id = 0, .flags = UUI_FILL_W };
    for (int i = 0; i < g_slot_count; i++) {
        struct slot *sl = &g_slot[i];
        PAGE[n++] = (struct uui_item){ .ops = &uui_label_ops, .widget = &sl->caption,
                                        .id = 0, .flags = UUI_FILL_W };
        PAGE[n++] = (struct uui_item){ .ops = &uui_label_ops, .widget = &sl->explain,
                                        .id = 0, .flags = UUI_FILL_W };
        // ONLY THE CONTROL IN USE is declared. Declaring both and hiding
        // one was the first version, and `hidden` is honoured -- but a
        // hidden widget is still a layout child, and the pair left this
        // page carrying twice the items it needed with one of every two
        // contributing nothing. Emitting one keeps the item list
        // describing exactly what is on screen, which is also what makes
        // the reported geometry mean something.
        if (sl->kind == CTRL_COMBO) {
            PAGE[n++] = (struct uui_item){ .ops = &uui_dropdown_ops,
                                            .widget = &sl->combo,
                                            .id = ID_CONTROL_BASE + i };
        } else if (sl->kind == CTRL_SLIDER) {
            PAGE[n++] = (struct uui_item){ .ops = &uui_slider_ops,
                                            .widget = &sl->slider,
                                            .id = ID_CONTROL_BASE + i,
                                            .flags = UUI_FILL_W };
        } else {
            PAGE[n++] = (struct uui_item){ .ops = &uui_radio_list_ops,
                                            .widget = &sl->radio,
                                            .id = ID_CONTROL_BASE + i,
                                            .flags = UUI_FILL_W };
        }
    }
    PAGE[n++] = (struct uui_item){ .ops = &uui_checkbox_ops, .widget = &g_advanced_cb,
                                    .id = ID_ADVANCED, .hidden = !g_advanced_has };
    PAGE_COUNT = n;
    PAGE_LAYOUT.count = n;
    // The item list just changed. The scroll view now NOTICES this by
    // itself (uui_scrollview.h), so this is belt-and-braces rather than
    // load-bearing -- kept because saying so at the point of change is
    // clearer than relying on a check somewhere else.
    uui_scrollview_content_changed(&PAGE_SCROLL);
}

// --- navigation and events -------------------------------------------

static void navigate(int node_id) {
    // A new page means new labels at new widths -- see g_prose_fitted.
    g_prose_fitted = 0;
    if (node_id == NODE_SYSINFO) {
        g_show_sysinfo = 1;
        g_page_group = -1;
        g_slot_count = 0;
        strlcpy(g_page_title_text, "System Information", sizeof g_page_title_text);
        g_page_desc_text[0] = '\0';
        strlcpy(g_status, "About this machine", sizeof g_status);
        relayout_page();
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

static void on_widget(struct uapp *a, int id, int reason) {
    // COMMIT ON RELEASE, docs/gui-guidelines.md's rule for every control
    // here -- and this file used to discard `reason` entirely. The
    // router delivers press, MOTION, release and wheel, so acting on all
    // of them meant merely moving the pointer across the choice list
    // applied a setting: each motion wrote /etc, bumped fs_generation()
    // and made the desktop re-read every .desktop file. Hovering froze
    // the machine for seconds.
    if (reason != UUI_REASON_RELEASE) return;

    if (id >= ID_CONTROL_BASE && id < ID_CONTROL_BASE + PAGE_MAX) {
        // STAGED, not applied. The selection is remembered; Apply or OK
        // is what writes it.
        struct slot *sl = &g_slot[id - ID_CONTROL_BASE];
        sl->staged = sl->kind == CTRL_COMBO   ? uui_dropdown_selected(&sl->combo)
                    : sl->kind == CTRL_SLIDER ? sl->slider.selected
                                              : sl->radio.selected;
        if (sl->setting >= 0 && sl->staged >= 0) {
            snprintf(g_status, sizeof g_status, "%s -> %s   (not applied yet)",
                     g_label[sl->setting], sl->choice_raw[sl->staged]);
            // LOGGED as well as shown. The status bar is pixels; a test
            // asserting on staging needs a fact, and reading the bar
            // back from a screenshot would be asserting the wrong thing
            // anyway -- what matters is that the change was staged and
            // NOT written.
            logf_("settings: staged %s %s\n", g_name[sl->setting],
                  sl->choice_raw[sl->staged]);
        }
        uapp_redraw(a);
        return;
    }

    switch (id) {
    case ID_TREE: {
        // The tree hands back the APP's id, not a row -- rows move as
        // categories collapse, ids do not.
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
static int refit_prose(void) {
    int changed = 0;
    int was = g_page_desc.rows;
    fit_rows(&g_page_desc, g_page_desc_text);
    if (g_page_desc.rows != was) changed = 1;
    for (int i = 0; i < g_slot_count; i++) {
        struct slot *sl = &g_slot[i];
        if (sl->setting < 0) continue;
        was = sl->explain.rows;
        fit_rows(&sl->explain, g_desc[sl->setting]);
        if (sl->explain.rows != was) changed = 1;
    }
    if (changed) relayout_page();
    return changed;
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    if (!g_prose_fitted && g_page_desc.w > 0) {
        g_prose_fitted = 1;
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

    // WHERE EACH CONTROL ENDED UP, reported whenever it MOVES -- which
    // covers a page change and a scroll with one rule. Geometry does not
    // exist until the layout has run, and on_draw is the first hook that
    // is reliably after it.
    int moved = g_slot_count != g_last_reported_count;
    for (int i = 0; i < g_slot_count && !moved; i++) {
        struct slot *sl = &g_slot[i];
        int y = sl->kind == CTRL_COMBO ? sl->combo.y
                : sl->kind == CTRL_SLIDER ? sl->slider.y : sl->radio.y;
        if (y != g_last_y[i]) moved = 1;
    }
    if (moved) {
        g_last_reported_count = g_slot_count;
        for (int i = 0; i < g_slot_count; i++) {
            struct slot *sl = &g_slot[i];
            int x, y, w, hh;
            if (sl->kind == CTRL_COMBO) {
                x = sl->combo.x; y = sl->combo.y; w = sl->combo.w; hh = sl->combo.h;
            } else if (sl->kind == CTRL_SLIDER) {
                x = sl->slider.x; y = sl->slider.y; w = sl->slider.w; hh = sl->slider.h;
            } else {
                x = sl->radio.x; y = sl->radio.y; w = sl->radio.w; hh = sl->radio.h;
            }
            g_last_y[i] = y;
            // The description's ROW COUNT, beside the control's
            // geometry and for the same reason: it is the only
            // observable difference between a wrapped explanation and a
            // truncated one, and settings_test asserts that at least
            // one description on a page actually took two rows.
            logf_("settings: prose %d %s rows %d width %d text %d\n", i,
                  sl->setting >= 0 ? g_name[sl->setting] : "-",
                  sl->explain.rows, sl->explain.w,
                  sl->setting >= 0 ? ugfx_text_width(g_desc[sl->setting]) : 0);
            logf_("settings: control %d %s %d %d %d %d rows %d kind %s\n", i,
                  sl->setting >= 0 ? g_name[sl->setting] : "-", x, y, w, hh,
                  sl->choice_count,
                  sl->kind == CTRL_COMBO ? "combo"
                    : sl->kind == CTRL_SLIDER ? "slider" : "radio");
        }
        logf_("settings: advanced_toggle %d %d %d %d shown %d\n",
              g_advanced_cb.x, g_advanced_cb.y, g_advanced_cb.w, g_advanced_cb.h,
              g_advanced_has);
    }
}

static void on_open(struct uapp *a) {
    (void)a;
    // The lines tools/ asserts on. Kept in the app rather than derived
    // from a screenshot because a layout is a fact, and a number a test
    // can read beats a picture it has to interpret.
    logf_("settings: settings %d\n", g_setting_count);
    logf_("settings: categories %d\n", g_cat_count);
    logf_("settings: groups %d\n", g_group_count);
    logf_("settings: nodes %d\n", g_node_count);
    logf_("settings: layout tree %d %d %d %d\n",
          g_tree.x, g_tree.y, g_tree.w, g_tree.h);
    logf_("settings: layout page %d %d %d %d\n",
          PAGE_SCROLL.x, PAGE_SCROLL.y, PAGE_SCROLL.w, PAGE_SCROLL.h);
    // The button group holds its buttons' geometry, not its own -- so
    // report the first button's, which is what a test clicks anyway.
    logf_("settings: layout buttons %d %d %d %d\n",
          g_btn[0].x, g_btn[0].y, g_btn[0].w, g_btn[0].h);
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
        logf_("settings: row %d id %d y %d depth %d %s\n",
              r, g_nodes[r].id, g_tree.y + r * rh + rh / 2,
              g_nodes[r].kind == UUI_SIDEBAR_HEADING ? 0 : 1,
              g_nodes[r].label);
    }
    // NO per-slot dump here: on_open runs ONCE, so it would describe the
    // first page forever and a tool reading it while looking at another
    // page gets a confident wrong answer. The `control` lines in
    // on_draw are reported per page change, where the geometry also
    // exists.
}

static void on_size(int *w, int *h) {
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
    *w = ugfx_char_w() * 74;
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
        uui_label_init(&g_slot[i].explain, 0);
        // One row until a description arrives that needs two -- the row
        // count is recomputed per text in fill_slot(), see there.
        uui_label_set_wrap(&g_slot[i].explain, 1);
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
                                        .id = ID_TREE, .flags = UUI_FILL_H };
    ITEMS_BODY[1] = (struct uui_item){ .ops = &uui_scrollview_ops, .widget = &PAGE_SCROLL,
                                        .id = ID_PAGE,
                                        .flags = UUI_FILL_W | UUI_FILL_H };
    BODY_LAYOUT = (struct uui_layout){ .dir = UUI_ROW, .items = ITEMS_BODY,
                                        .count = 2, .margin = 0 };

    ITEMS[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &BODY_LAYOUT,
                                   .id = ID_BODY, .flags = UUI_FILL_W | UUI_FILL_H };
    ITEMS[1] = (struct uui_item){ .ops = &uui_button_group_ops, .widget = &g_buttons,
                                   .id = ID_BUTTONS };
    ITEMS[2] = (struct uui_item){ .ops = &uui_statusbar_ops, .widget = &g_status_bar,
                                   .id = ID_STATUS, .flags = UUI_FILL_W };
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
        .on_draw = on_draw,
        .on_draw_over = on_draw_over,
        .on_open = on_open,
        .on_size = on_size,
    };
    return uapp_run(&desc);
}
