#ifndef SETTINGS_INTERNAL_H
#define SETTINGS_INTERNAL_H

// SYSTEM SETTINGS' SHARED STATE, split by concern the way userland/wm/ is:
// the registry and sidebar (set_registry.c), one page (set_page.c), an
// owner's options and their dialog (set_owner.c), System Information
// (set_sysinfo.c); the app itself -- callbacks and the root layout -- is
// userland/gui/system/settings.c. One process, so these are plain globals.

#include <stdint.h>
#include "ui/ulog.h"
#include <stdarg.h>
#include "rt/sys.h"
#include <string.h>
#include <strings.h>      // strncasecmp
#include <stdio.h>
#include "ui/uapp.h"
#include "ui/uui.h"
#include "ui/uui_layout.h"
#include "ui/uui_sidebar.h"
#include "ui/uui_splitter.h"
#include "ui/uui_label.h"
#include "ui/uui_radio_list.h"
#include "ui/uui_dropdown.h"
#include "ui/uui_focus.h"
#include "ui/uui_slider.h"
#include "ui/uui_spinbox.h"
#include "ui/uui_checkbox.h"
#include "ui/uui_textbox.h"
#include "ui/uui_keycapture.h"
#include "ui/uui_button.h"
#include "ui/uui_switch.h"
#include "ui/uui_segmented.h"
#include "ui/uui_gallery.h"
#include "ui/uui_setting_row.h"
#include "ui/uui_dialog.h"
#include "lib/usaver.h"
#include "lib/ueffect.h" // an EFFECT declares options the same way
#include "ui/uui_scrollview.h"
#include "ui/utheme.h"
#include "lib/uconf.h"   // the sidebar width, remembered
#include <stdlib.h>       // atoi
#include "setting_abi.h"
#include "cpuinfo.h"
#include "version.h"
#include "lib/usetting.h" // the MERGED registry -- see the header


_Static_assert(UUI_TEXTBOX_MAX >= SETTING_ABI_VALUE_MAX,
               "a text field must hold a whole setting value, or editing one "
               "silently truncates it");

// THE TABLES ARE SIZED FROM THE REGISTRY'S OWN COUNT, at every reload
// (reload_settings()), so there is no cap here for the registry to
// outgrow -- one fixed size ran out three times, each time dropping the
// LAST settings without a word. `g_cap` rows: the registry's settings,
// then room for the selected screensaver's options, which are not
// registered anywhere (see g_saver_base) and are synthesised at indices
// past the registry's so that one code path draws both. A category and a
// page are both keyed off settings, so neither can outnumber them.
extern int g_cap;
#define MAX_SETTINGS   g_cap
// Room for every timezone the kernel ships plus hand-added rows.
#define MAX_CHOICES    128
#define MAX_CATEGORIES g_cap
#define MAX_GROUPS     g_cap
// Controls on one page. A group larger than this would be a page nobody
// can take in anyway; the overflow is REPORTED rather than silently cut.
//
// **IT COUNTS THE OPTIONS TOO, and that is why it is not 2 +
// USAVER_OPT_MAX any more.** It was sized by the Screensaver page --
// two registry settings plus what the saver declares -- and then
// Appearance -> Effects gained an owner of its own with FIVE settings
// above it, so the second of shatter's two options did not fit and
// vanished with nothing on screen saying so. The ceiling has to cover
// the biggest PAGE plus the most options an owner can declare.
#define PAGE_MAX       (12 + USAVER_OPT_MAX)

// From this many choices a page uses a DROPDOWN rather than a list, unless
// /etc/settings.d says otherwise. Few mutually-exclusive options are
// better all visible; ninety-two timezones are not.
#define CHOICES_DROPDOWN_MIN 7

// struct slot's `kind`.
enum { CTRL_RADIO = 0, CTRL_COMBO, CTRL_SLIDER, CTRL_SPIN, CTRL_TEXT,
       CTRL_KEYCAP, CTRL_SWITCH, CTRL_SEGMENTED, CTRL_GALLERY };

enum { ID_TREE = 1, ID_SIDE_SPLIT, ID_BODY, ID_PAGE, ID_ADVANCED, ID_TEST,
       ID_OPTS, ID_OPTS_OK, ID_OPTS_CANCEL, ID_SEARCH, ID_RESET, ID_APPLY,
       ID_ASK, ID_FOOTER, ID_SI_COPY, ID_SI_DEVMGR, ID_SI_DEBUG, ID_CLOCK_CHANGE,
       ID_SU_DEFAULT, ID_SU_TIMEOUT, ID_SU_NEXT, ID_SU_OPEN,
       ID_KB_LIST, ID_KB_ADD, ID_KB_REMOVE, ID_KB_UP, ID_KB_DOWN, ID_KB_TRY,
       ID_SNDFMT = 60,          // .. + UUI_SNDFORMAT_IDS, Sound > Output's Format
       ID_CONTROL_BASE = 100 }; // + slot, so a control names its own row

// The focus ring: FOCUS_LEAD entries before the page's controls (search,
// sidebar), and room after them for Test, Settings..., Change..., the
// advanced toggle and the footer's two buttons.
#define FOCUS_LEAD 2
#define FOCUS_MAX  (FOCUS_LEAD + PAGE_MAX + 6 + 16)   // 16: the Format panel's

#define NODE_SYSINFO       1
#define NODE_STARTUP       2
#define NODE_CATEGORY_BASE 1000
#define NODE_GROUP_BASE    2000


// Title, description, a card per slot, Test and Settings..., the advanced
// toggle, and set_clock.c's clock, Change... card and preview.
#define PAGE_ITEMS (2 + PAGE_MAX + 2 + 1 + 3)

#define GROUP_DISPLAY_MAX (SETTING_ABI_CATEGORY_MAX + SETTING_ABI_LABEL_MAX + 3)

// THE TWO SETTINGS THAT OWN OPTIONS BELOW THEM -- see set_owner.c.
// OBJECTS, NOT LITERALS: an owner kind is compared by POINTER, and two
// translation units' copies of one string literal need not be the same
// address.
extern const char OWNER_SAVER[], OWNER_EFFECT[];

// --- the page's controls ---------------------------------------------
//
// One SLOT per control, each holding both presentations; `hidden` picks
// one. Declaring both and hiding one beats rewriting an item's ops at
// runtime -- `hidden` already removes a widget from drawing AND
// hit-testing, and swapping an ops pointer underneath a router that may
// hold a pointer grab is a different kind of problem.
struct slot {
    int setting;               // index into the arrays above, or -1
    // The card the page shows it in; its `control` is whichever of the
    // presentations below `kind` names.
    struct uui_setting_row row;
    struct uui_radio_list radio;
    struct uui_dropdown   combo;
    struct uui_slider     slider;
    struct uui_spinbox    spin;
    // FREE TEXT. Until this existed a STRING setting drew an EMPTY radio
    // list -- a row that looks broken and can only be changed with
    // `config set`. Every registry client can now edit one, not just the
    // setting that prompted it.
    struct uui_textbox    text;
    // A KEY COMBINATION, captured by pressing it. Staged the way
    // CTRL_TEXT is -- baseline_buf holds what the page opened with
    // and `staged` is a changed FLAG, because a combination is no
    // more an index than a free string is.
    struct uui_keycapture keycap;
    struct uui_switch     sw;       // a two-valued state (onoff pairs)
    struct uui_segmented  seg;      // a few short names, side by side
    struct uui_gallery    gallery;  // Widget=gallery: a card per choice, Preview= paints it
    int on_idx;                     // CTRL_SWITCH: the "on" value's index
    // Which presentation is showing. A KIND rather than a set of flags:
    // booleans can express "both" and "neither", and neither is a state
    // this page has.
    int kind;
    // The staged selection, and the value the page opened with.
    //
    // **FOR A CHOICE CONTROL THESE ARE INDICES; FOR CTRL_SPIN THEY ARE
    // THE NUMBER ITSELF.** That is deliberate rather than a second pair
    // of fields: everything the page does with them -- "is it different
    // from the baseline", "is anything staged" -- is an equality test
    // that reads the same either way, and only staged_value() below has
    // to know which. Two more fields would mean every one of those
    // tests growing a branch.
    int staged;
    int baseline;
    char staged_buf[SETTING_ABI_VALUE_MAX]; // CTRL_SPIN's staged, as text
    // CTRL_TEXT's opening value. A text slot cannot express "changed" as
    // an index, so it keeps `baseline` at 0 and sets `staged` to 1 when
    // the field differs from this -- which leaves every `staged !=
    // baseline` test on this page reading exactly as it did.
    char baseline_buf[SETTING_ABI_VALUE_MAX];
    int choice_count;
    char choice[MAX_CHOICES][SETTING_ABI_VALUE_MAX];      // display names
    char choice_raw[MAX_CHOICES][SETTING_ABI_VALUE_MAX];  // what gets stored
    const char *choice_ptr[MAX_CHOICES];
};


// settings.c
extern struct uui_sidebar g_tree;
// Around a rebuild of FOCUS: open() notes the focused widget, close()
// appends the footer's buttons, re-inits the ring and restores it.
void focus_ring_open(void);
void focus_ring_close(void);

// set_registry.c
extern char     (*g_label)[SETTING_ABI_LABEL_MAX];
extern char     (*g_desc)[SETTING_ABI_DESC_MAX];
extern char     (*g_name)[SETTING_ABI_QUALIFIED_MAX];
extern char     (*g_ns)[SETTING_ABI_NS_MAX];
extern char     (*g_file)[SETTING_ABI_FILE_MAX];
extern char     (*g_value)[SETTING_ABI_VALUE_MAX];
extern char     (*g_cat_of)[SETTING_ABI_CATEGORY_MAX];
extern char     (*g_group_of)[SETTING_ABI_CATEGORY_MAX];
extern uint32_t *g_type;
extern uint32_t *g_widget;
extern int32_t  *g_imin, *g_imax, *g_istep;
extern char     (*g_unit)[SETTING_ABI_UNIT_MAX];
extern uint32_t *g_sflags;
extern int      *g_order;
extern char     (*g_unavail)[SETTING_ABI_DESC_MAX];
extern int      g_setting_count;
extern uint32_t g_generation;
extern char (*g_cat)[SETTING_ABI_CATEGORY_MAX];
extern int  g_cat_count;
extern char (*g_group_cat)[SETTING_ABI_CATEGORY_MAX];
extern char (*g_group_key)[SETTING_ABI_CATEGORY_MAX];
extern char (*g_group_label)[SETTING_ABI_LABEL_MAX];
extern int *g_cat_order;
extern int *g_group_order;
extern char (*g_group_display)[GROUP_DISPLAY_MAX];
extern int  g_group_count;
extern struct uui_sidebar_row *g_nodes;   // 2 * g_cap + 1 rows
extern int g_node_count;
const char *group_key_of(int i);
const char *category_icon(const char *cat);
extern char g_filter[UUI_TEXTBOX_MAX];
int group_matches(int g);
extern int g_show_debug;
int group_shown(int g);
void rebuild_sidebar(void);
int reload_settings(void);
uint32_t registry_generation(void);

// set_owner.c
extern struct usaver g_saver;
extern int g_saver_base;   // first synthesised index, or -1 for none
extern int g_saver_slot;   // first option's slot, or -1
extern struct uui_button g_opts_btn;
extern struct uui_layout DLG_ROW;
const struct usaver_opt *opt_of(int idx);
void rebuild_owner_options(const char *kind, const char *saver);
const char *owner_kind(int idx);
const char *page_owner(const char **kind_out);
int owner_uses_dialog(const char *kind);
extern struct uui_button    g_opts_ok, g_opts_cancel;
extern struct uapp_window  *g_opts_win;
extern const char          *g_page_owner_kind;
extern struct uui_item      DLG[PAGE_MAX + 4];
extern int                  DLG_COUNT;
extern struct uui_focusable DFOCUS[PAGE_MAX + 2];
extern int                  DFOCUS_COUNT;
extern struct uui_focus     g_dlg_focus;
extern struct uui_layout    DLG_LAYOUT;
extern struct uui_item      DLG_ROW_ITEMS[2];
void relayout_dialog(int content_w);
void opts_window_size(int *w, int *h);
void dlg_on_widget(struct uapp_window *win, int id, int reason);
void dlg_on_action(struct uapp_window *win, int code);
void dlg_on_close(struct uapp_window *win);
void open_options_dialog(struct uapp *a);
int commit_option(struct slot *sl);   // 1 written, 0 nothing to do, -1 refused

// set_page.c
extern char g_status[160];
extern int  g_loaded;
extern int  g_show_sysinfo;
extern int  g_prose_fitted;
extern int  g_prose_fit_w;
extern int  g_show_advanced;
extern int  g_page_group;
extern int  g_page_node;
extern int  g_advanced_has;
extern int g_last_y[PAGE_MAX];
extern int g_last_reported_count;
extern struct slot g_slot[PAGE_MAX];
extern int g_slot_count;
extern struct uui_label     g_page_title;
extern struct uui_label     g_page_desc;
extern struct uui_checkbox  g_advanced_cb;
extern struct uui_button    g_test_btn;
extern int g_test_has;
extern char g_page_title_text[SETTING_ABI_LABEL_MAX];
extern char g_page_desc_text[SETTING_ABI_DESC_MAX];
void fit_rows(struct uui_label *l, const char *text);
const char *slot_prose(int idx);
extern struct uapp *g_app;
void keycap_armed(void *ctx, int armed);
void keycap_done(void *ctx, const char *text);
void set_slot_enabled(struct slot *sl, int idx);
void load_slot(struct slot *sl, int idx);
int page_dirty(void);
int page_changes(void);
extern int g_page_captions;
void open_group(int g);
const char *staged_value(struct slot *sl);
int apply_page(void);
extern struct uui_item PAGE[PAGE_ITEMS];
extern int PAGE_COUNT;
extern struct uui_layout PAGE_LAYOUT;
extern struct uui_scrollview PAGE_SCROLL;
extern struct uui_focusable FOCUS[FOCUS_MAX];
extern int FOCUS_COUNT;
extern struct uui_focus PAGE_FOCUS;
int emit_slot(struct uui_item *out, int n, int i,
                     struct uui_focusable *focus, int *nfocus);
void relayout_page(void);
int control_changed(int slot_index);
int refit_prose(void);
void slot_rect(const struct slot *sl, int *x, int *y, int *w, int *h);
const char *slot_kind_name(const struct slot *sl);

// set_preview.c: a gallery's card pictures, by the setting's Preview= word.
// preview_attach() picks the painter (none for an unknown word);
// preview_reset() frees what the last page decoded.
void preview_attach(struct slot *sl, int idx);
void preview_reset(void);
int slot_disabled(const struct slot *sl);

// set_keyboard.c -- Input > Keyboard's layout list, preview and Try it.
// kbd_emit_slot() returns -1 for a slot it leaves to emit_slot().
extern int g_kbd_page;
void kbd_init(void);
void kbd_page_opened(void);
int  kbd_emit_slot(struct uui_item *out, int n, int i,
                   struct uui_focusable *focus, int *nfocus);
int  kbd_emit_after(struct uui_item *out, int n, int i,
                    struct uui_focusable *focus, int *nfocus);
int  kbd_on_widget(struct uapp *a, int id);
int  kbd_on_action(struct uapp *a, int code);
int  kbd_tick(void);
void kbd_shutdown(void);

// set_sound.c -- Sound > Output's Format card (ui/uui_sndformat.h)
extern int g_snd_page;
void snd_init(void);
void snd_page_opened(void);
int  snd_emit_after(struct uui_item *out, int n, int i,
                    struct uui_focusable *focus, int *nfocus);
int  snd_on_widget(struct uapp *a, int id);
int  snd_tick(void);

// set_clock.c -- Time & Locale's live clock, Change... and preview
extern int g_clock_page, g_region_page;
void clock_init(void);
void clock_page_opened(void);
int  clock_emit_top(struct uui_item *out, int n);
int  clock_emit_after(struct uui_item *out, int n, int i,
                      struct uui_focusable *focus, int *nfocus);
int  clock_tick(void);
void clock_open_dialog(struct uapp *a);
void region_preview_refresh(void);

// set_sysinfo.c
extern struct uui_custom g_si_view;
extern struct uui_button g_si_copy, g_si_devmgr;
extern struct uui_checkbox g_si_debug_cb;
extern struct uui_setting_row g_si_debug;
void sysinfo_init(void);
void sysinfo_load(void);
int  sysinfo_tick(void);
int  sysinfo_height(void);
int  sysinfo_copy(void);

// set_startup.c -- System > Boot menu, GRUB's menu (not registry settings)
extern int g_show_startup;
void startup_init(void);
void startup_load(void);
int  startup_emit(struct uui_item *out, int n, struct uui_focusable *focus, int *nfocus);
void startup_changed(int id);
int  startup_fit(void);

#endif
