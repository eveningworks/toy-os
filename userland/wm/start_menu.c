// See start_menu.h.
#include "start_menu.h"
#include "ui/uui_popup.h"
#include "wm_shadow.h"
#include "start_store.h"
#include "wm_tooltip.h"
#include "wm_overlay.h"
#include "lib/icon_cache.h"
#include "wm_internal.h"
#include "wm_taskbar.h"   // taskbar_start_rect()
#include "confirm_dialog.h"
#include "context_menu.h"
#include "lib/ubootmenu.h"
#include "ui/uui.h"
#include "ui/utheme.h"
#include "kapi.h"
#include "keyboard.h"   // KEY_ARROW_* / KEY_MOD_*
#include "rt/sys.h"

int start_menu_open = 0;

// System actions shown under a divider at the foot of the sidebar --
// these don't open a window like a real gui_app_registry entry does,
// they trigger a WM-level action directly. "Exit to shell" replaces the
// old hardcoded "Esc always exits the window manager" shortcut: it's
// discoverable now instead of a hidden key.
//
// All three go through confirm_dialog.h's reusable Yes/No popup instead
// of acting directly -- each drops every open window's unsaved state
// (no session restore exists), so a stray click landing on one
// shouldn't be irreversible.
static void do_exit_to_shell(void) { wm_exit_requested = 1; }
static void action_exit_to_shell(void) {
    confirm_dialog_open_with("Exit to shell? Unsaved changes will be lost.", do_exit_to_shell, 0);
}

// Shutdown -- a real poweroff through the machine's own ACPI tables
// (kernel/acpi/). Unlike do_exit_to_shell(), system_poweroff() never
// returns, so there is no wm_exit_requested-style flag to set here.
static void do_shutdown(void) { sys_poweroff(0); }
static void action_shutdown(void) {
    confirm_dialog_open_with("Shut down? Unsaved changes will be lost.", do_shutdown, 0);
}

// Restart -- the same syscall with the other argument (the FADT's reset
// register). Before Shutdown, as KDE orders them.
static void do_restart(void) { sys_poweroff(1); }

// RESTART INTO A BOOT ENTRY: with more than one GRUB entry, Restart opens
// a flyout of them -- KDE's Leave dialog and Windows' power menu both
// offer this -- and a pick is confirmed like a plain restart, naming the
// entry. The choice is one boot only; lib/ubootmenu.h has why that is
// GRUB's to enforce. Read when the menu OPENS, not per draw: it is a
// file on /boot.
static struct ubootmenu g_boot;
static int g_boot_menu;           // two entries or more: Restart has a flyout
static int g_from_key;            // the context menu takes no keys; see action_restart
static int g_boot_pick;
static char g_boot_label[UBOOTMENU_MAX][UBOOTMENU_TITLE + 12];
static struct context_menu_item g_boot_item[UBOOTMENU_MAX];
static char g_boot_msg[UBOOTMENU_TITLE + 64];

static void do_restart_into(void) {
    // The default needs no choice, but one left pending is cleared or
    // it would win instead.
    const char *title = g_boot_pick == g_boot.def ? 0 : g_boot.title[g_boot_pick];
    if (ubootmenu_set_next(title) < 0) {
        confirm_dialog_open_with("Could not save the boot choice. Restart normally?",
                                 do_restart, 0);
        return;
    }
    sys_poweroff(1);
}

static void pick_boot_entry(void *ctx) {
    g_boot_pick = (int)(intptr_t)ctx;
    k_snprintf(g_boot_msg, sizeof g_boot_msg, "Restart into %s? Unsaved changes will be lost.",
             g_boot.title[g_boot_pick]);
    start_menu_close();
    confirm_dialog_open_with(g_boot_msg, do_restart_into, 0);
}

static void action_restart(void) {
    // From the keyboard, the plain confirm: the flyout could not be
    // driven by the keys that opened it.
    if (!g_boot_menu || g_from_key) {
        confirm_dialog_open_with("Restart? Unsaved changes will be lost.", do_restart, 0);
        return;
    }
    int x = 0, y = 0, w = 0;
    for (int n = 0; n < start_menu_row_count(); n++) {
        const char *label; int kind, h;
        if (!start_menu_row_info(n, &label, &kind, &x, &y, &w, &h, 0)) break;
        if (kind == START_ROW_ACTION && !k_strcmp(label, "Restart")) break;
    }
    for (int i = 0; i < g_boot.count; i++) {
        k_snprintf(g_boot_label[i], sizeof g_boot_label[i], "%s%s", g_boot.title[i],
                 i == g_boot.def ? " (default)" : "");
        k_memset(&g_boot_item[i], 0, sizeof g_boot_item[i]);
        g_boot_item[i].label = g_boot_label[i];
        g_boot_item[i].on_select = pick_boot_entry;
        g_boot_item[i].ctx = (void *)(intptr_t)i;
    }
    // A CHILD OF THE START MENU, as a row's right-click menu is: naming
    // the parent keeps Start up under it (wm_input.c).
    wm_overlay_set_parent("start");
    context_menu_open_at(x + w, y, g_boot_item, g_boot.count);
}

const struct start_action wm_system_actions[] = {
    { "Exit to shell", action_exit_to_shell },
    { "Restart", action_restart },
    { "Shutdown", action_shutdown },
};
const int wm_system_action_count = sizeof(wm_system_actions) / sizeof(wm_system_actions[0]);

// --- state ------------------------------------------------------------

// A click used to close the menu in the same frame it ran the row's
// action -- no visible confirmation the click landed. The clicked row
// shows a brief flash first; its action still runs immediately, only
// closing is deferred. `flash_row` is a row index in the walk order
// below, -1 when idle.
static int flash_row = -1;
static uint64_t flash_until = 0;
#define START_MENU_FLASH_TICKS 10 // ~100ms at the PIT's 100Hz

// WHICH CONTROL IS HOVERED, tracked rather than derived inside the draw
// -- the draw used to compute it from (mx, my) on every full repaint,
// which forced a whole-screen repaint on every mouse move while the
// menu was open (60 ms a move at 1280x720).
static int hover_token = 0;

static int sel_cat = 0;   // the open folder, an index into the folder list
static int sel_row = -1;  // the keyboard-highlighted app row, -1 for none
// The first app row SHOWN. The list is taller than the pane now, so a
// row index and a screen position are different things -- every place
// that had conflated them is the reason this is named rather than
// derived.
static int scroll;

#define START_QUERY_MAX 24
static char query[START_QUERY_MAX];
static int query_len;

// Hover tokens. Opaque to wm_overlay.c, which only compares them, so
// all that matters is that no two controls share one.
#define TOK_CAT(i)    (1 + (i))
#define TOK_ACTION(i) (100 + (i))
#define TOK_APP(i)    (200 + (i))
#define TOK_SEARCH    999
#define TOK_SETTINGS  998
#define TOK_SCROLLBAR 997

// THE APP COLUMN'S SCROLLBAR: thin at rest, widened while the pointer is
// on it or a drag holds it (an overlay bar, Windows 11's and Breeze's).
// Drawn and hit-tested by the shared uui_scrollbar helpers; the drag and
// the held-click paging run from the overlay's press op, every tick.
static int sb_drag;               // 1 while the thumb is held
static int sb_grab;               // where in the thumb the press landed
static int sb_page;               // -1 / +1 while a track click repeats, else 0
static uint64_t sb_next;          // when that repeat fires next, in ticks
#define SB_REPEAT_DELAY 40        // 400 ms, then
#define SB_REPEAT_EVERY 8         // every 80 ms -- Windows' typematic shape

// The breathing room above the first row and below the last, which a
// menu whose rows sit flush against its border does not have.
#define SM_INSET (ugfx_char_h() * 2 / 3)
// The icon on an APP row (two lines tall: the name and its Comment=),
// and the column it reserves -- stated once, so the width the layout
// books and the pixels the draw puts there cannot drift apart.
#define SM_ICON_SZ(app_h)  ((app_h) * 5 / 8)
#define SM_ICON_COL(app_h) (SM_ICON_SZ(app_h) + 34)  // the pill's inset and pad, the icon, a gap
// A FOLDER'S icon is smaller than an app's: it labels a list rather
// than standing for a program, and Kickoff and Whisker both draw it
// that way.
#define SM_FOLDER_SZ(item_h) ((item_h) / 2)

// HOW TALL THE APP COLUMN IS ALLOWED TO GET, in rows. Beyond this it
// SCROLLS: a menu as tall as its biggest folder was fine at nineteen
// apps and is not a design, and "All Applications" is unbounded by
// construction. Eight two-line rows is about half a 720p screen with
// the default font, and it is font-derived like everything else here.
#define SM_MAX_ROWS 8

// How many apps the Recent folder remembers. A history nobody can scan
// is not a shortcut -- Kickoff shows a handful for the same reason.
#define SM_RECENT_MAX 8

// --- FOLDERS: the sidebar's list, which is not just the categories ----
//
// Three pseudo-folders come before the `Category=` ones, each existing
// only when it has something in it -- the same rule the categories
// follow, and the reason an empty folder is unrepresentable here:
//
//   Favourites  what has been pinned, in PIN order (start_store.h)
//   Recent      what has been launched, newest first
//   All         every app, which is what the folders are a view of
//
// Kickoff opens on Favorites and keeps Recent beside it; Windows 11's
// whole Start is those two. `All Applications` is Kickoff's too, and it
// is what makes the categories a convenience rather than a maze: there
// is always one place everything is.
enum folder_kind { FOLDER_FAV = 0, FOLDER_RECENT, FOLDER_ALL, FOLDER_CAT };

static int have_favourites(void) {
    for (int i = 0; i < start_store_pin_count(); i++)
        if (gui_app_by_id(GUI_SHOW_STARTMENU, start_store_pin_at(i))) return 1;
    return 0;   // a pin whose app is gone is not a folder
}

// RECENT AS IT WAS WHEN THE MENU OPENED. A launch records itself while
// the menu is still on screen, and a live order moved the launched row
// to the top under the pointer mid-click -- Windows' Recommended and
// Kickoff's Recent both change on the next open, not this one. App ids,
// not pointers, so an app removed while the menu is up is skipped.
static char g_recent[SM_RECENT_MAX][GUI_APP_ICON_MAX];
static int g_recent_n;

// A SELECTION SORT over the visible apps, newest sequence first. A few
// dozen apps, once per open.
static void snapshot_recent(void) {
    uint32_t ceiling = 0xFFFFFFFFu;
    int apps = gui_app_visible_count(GUI_SHOW_STARTMENU);
    for (g_recent_n = 0; g_recent_n < SM_RECENT_MAX; g_recent_n++) {
        uint32_t best = 0;
        struct gui_app *pick = 0;
        for (int i = 0; i < apps; i++) {
            struct gui_app *a = gui_app_visible_at(GUI_SHOW_STARTMENU, i);
            uint32_t seq = start_store_last_seq(a->app_id);
            if (!seq || seq >= ceiling || seq <= best) continue;
            best = seq;
            pick = a;
        }
        if (!pick) break;
        k_strlcpy(g_recent[g_recent_n], pick->app_id, sizeof g_recent[g_recent_n]);
        ceiling = best;
    }
}

static struct gui_app *recent_at(int n) {
    // Frozen only while OPEN: a closed menu reports what the next open
    // will show, which is what a caller reading its geometry expects.
    if (!start_menu_open) snapshot_recent();
    for (int i = 0; i < g_recent_n; i++) {
        struct gui_app *a = gui_app_by_id(GUI_SHOW_STARTMENU, g_recent[i]);
        if (a && n-- == 0) return a;
    }
    return 0;
}

static int have_recent(void) { return recent_at(0) != 0; }

// The pseudo-folders that exist right now, in order. Returned as a
// count plus a kind-for-index rather than a table, because which ones
// exist changes with a pin and with a launch.
static int pseudo_count(void) {
    return (have_favourites() ? 1 : 0) + (have_recent() ? 1 : 0) + 1;  // All always
}

static enum folder_kind folder_kind_at(int n) {
    if (have_favourites()) {
        if (n == 0) return FOLDER_FAV;
        n--;
    }
    if (have_recent()) {
        if (n == 0) return FOLDER_RECENT;
        n--;
    }
    if (n == 0) return FOLDER_ALL;
    return FOLDER_CAT;
}

// Which CATEGORY a folder index means, or -1 when it is a pseudo-folder.
static int folder_cat_index(int n) {
    int p = pseudo_count();
    return n >= p ? n - p : -1;
}

static int folder_count(void) {
    return pseudo_count() + gui_app_cat_count(GUI_SHOW_STARTMENU);
}

static const char *folder_label_at(int n) {
    switch (folder_kind_at(n)) {
    case FOLDER_FAV:    return "Favourites";
    case FOLDER_RECENT: return "Recent";
    case FOLDER_ALL:    return "All Apps";
    default: break;
    }
    const char *l = gui_app_cat_label(GUI_SHOW_STARTMENU, folder_cat_index(n));
    return l ? l : "";
}

// The icon a folder draws. A pseudo-folder gets its own; a category
// gets `cat-<key>`, which is the name gen_icons.py writes -- and a
// folder whose icon is missing simply draws none, since the label
// carries the meaning either way.
static const char *folder_icon_at(int n) {
    switch (folder_kind_at(n)) {
    case FOLDER_FAV:    return "cat-favourites";
    case FOLDER_RECENT: return "cat-recent";
    case FOLDER_ALL:    return "cat-all";
    default: break;
    }
    static char buf[GUI_APP_ICON_MAX];
    const char *key = gui_app_cat_key(GUI_SHOW_STARTMENU, folder_cat_index(n));
    if (!key) return "";
    k_snprintf(buf, sizeof buf, "cat-%s", key);
    return buf;
}

// The n'th app of folder `f`, or NULL past its end. Only Recent has to
// ORDER anything, and snapshot_recent() did that at open: the registry
// is already sorted by (category, name), and the pins carry the order
// they were made in.
static struct gui_app *folder_app_at(int f, int n) {
    if (n < 0) return 0;
    switch (folder_kind_at(f)) {
    case FOLDER_FAV: {
        for (int i = 0; i < start_store_pin_count(); i++) {
            struct gui_app *a = gui_app_by_id(GUI_SHOW_STARTMENU, start_store_pin_at(i));
            if (!a) continue;           // pinned, but not installed today
            if (n-- == 0) return a;
        }
        return 0;
    }
    case FOLDER_RECENT:
        return recent_at(n);
    case FOLDER_ALL:
        return gui_app_visible_at(GUI_SHOW_STARTMENU, n);
    default:
        return gui_app_cat_at(GUI_SHOW_STARTMENU,
                              gui_app_cat_key(GUI_SHOW_STARTMENU, folder_cat_index(f)), n);
    }
}

// --- layout -----------------------------------------------------------

// The gap between the card and the strip, and the screen's edge.
static int sm_gap(void) { return ugfx_char_h() / 2 + 2; }
// A row pill's radius and its inset from the column -- the card's.
static int sm_pill_inset(void) { return ugfx_char_h() / 3; }

// KICKOFF'S SHAPE, refined (chosen from mockups 2026-10-01): a header
// with the search field and a settings button, the folder rail and the
// app column under it, a footer with the system actions as labelled
// buttons. App rows are TWO lines -- the name and its Comment= -- so the
// description lives on the row it describes rather than in a strip.
struct sm_layout {
    int x, y, w, h;   // the whole popup
    int item_h;       // a folder row
    int app_h;        // an app row: two lines of text and air
    int side_w;       // the folder rail
    int pane_x, pane_w, pane_h, pane_rows; // the app column; rows it can SHOW
    int cats;         // folders in the rail
    int head_h, foot_h; // the header band and the footer band
    int main_y, main_h; // the rail and the column between them
    int btn;          // the header's square settings button
};

// The footer's button for system action `i`: its width, from its label
// plus Shut down's glyph and Restart's chevron when it has a boot menu.
static int sm_action_w(int i) {
    const char *lb = wm_system_actions[i].label;
    int w = ugfx_text_width(lb) + 2 * ugfx_char_h();
    if (!k_strcmp(lb, "Shutdown")) w += ugfx_char_h() + 9;
    if (!k_strcmp(lb, "Restart") && g_boot_menu) w += ugfx_char_h();
    return w;
}

static void layout(struct sm_layout *L) {
    int ch = ugfx_char_h();
    L->item_h = ch * 2 + 10;
    L->app_h = ch * 4;
    L->cats = folder_count();
    L->head_h = ch * 4 + 6;
    L->foot_h = ch * 3 + 9;
    L->btn = ch * 2 + 8;

    int side_label = 0;
    for (int i = 0; i < L->cats; i++) {
        int n = ugfx_text_width(folder_label_at(i));
        if (n > side_label) side_label = n;
    }
    // The folder icon's column, on the same terms as the app column's:
    // booked whether or not every folder has artwork, so a missing icon
    // file cannot move the labels.
    L->side_w = side_label + 36 + SM_FOLDER_SZ(L->item_h) + 6;
    // A FLOOR as well as the measure: sized to its labels alone the card
    // came out a narrow strip. Windows 11's Start and Kickoff are wider
    // still; this keeps the rail a comfortable column at any font.
    if (L->side_w < ch * 15) L->side_w = ch * 15;

    // MEASURED, not counted: the column is as wide as its widest label
    // DRAWS, which on a proportional face is not its longest label times
    // the widest advance -- that came out ~1.8x too wide. Measured over
    // EVERY app rather than the open folder's, so switching folders
    // never moves the menu's edge. The Comment= line is elided, so it
    // is not measured.
    int app_label = 0;
    int apps = gui_app_visible_count(GUI_SHOW_STARTMENU);
    for (int i = 0; i < apps; i++) {
        int n = ugfx_text_width(gui_app_visible_at(GUI_SHOW_STARTMENU, i)->name);
        if (n > app_label) app_label = n;
    }
    L->pane_w = app_label + 16 + SM_ICON_COL(L->app_h);
    if (L->pane_w < ch * 34) L->pane_w = ch * 34;   // the same floor, room for the line under it

    // THE FOOTER MUST HOLD ITS BUTTONS and the version beside them.
    int foot_need = ugfx_text_width("toy-os " TOYOS_VERSION) + 3 * ch;
    for (int i = 0; i < wm_system_action_count; i++) foot_need += sm_action_w(i) + 4;
    if (L->side_w + 1 + L->pane_w < foot_need) L->pane_w = foot_need - L->side_w - 1;

    // AS TALL AS THE RAIL, OR AS THE BIGGEST FOLDER, WHICHEVER IS
    // SMALLER-BUT-ENOUGH -- and never past SM_MAX_ROWS, which is what
    // the scrolling is for. The rail's own height is a floor rather
    // than a target: a column shorter than the folders beside it would
    // leave the divider running past the bottom of the list.
    int side_h = L->cats * L->item_h;
    int biggest = 0;
    for (int i = 0; i < L->cats; i++) {
        int n = 0;
        while (folder_app_at(i, n)) n++;
        if (n > biggest) biggest = n;
    }
    if (biggest > SM_MAX_ROWS) biggest = SM_MAX_ROWS;
    int list_h = biggest * L->app_h;
    int content_h = side_h > list_h ? side_h : list_h;
    L->main_h = content_h + 2 * SM_INSET;
    L->pane_h = L->main_h;
    L->pane_rows = L->app_h ? (L->main_h - 2 * SM_INSET) / L->app_h : 0;

    L->w = L->side_w + 1 + L->pane_w;
    L->h = L->head_h + L->main_h + L->foot_h;
    // Over the Start button, wherever the style put it -- clamped to the
    // screen, so a centred button's menu cannot hang off the right edge.
    {
        int sx, sy, sw, sh;
        taskbar_start_rect(&sx, &sy, &sw, &sh);
        L->x = sx;
        // Centred Start, centred menu -- Windows 11's.
        if (taskbar_start_centered())
            L->x = screen_w / 2 - L->w / 2;
        if (L->x + L->w > screen_w - sm_gap()) L->x = screen_w - sm_gap() - L->w;
        if (L->x < sm_gap()) L->x = sm_gap();
    }
    // FLOATING, a gap above the strip as Windows 11's Start is.
    L->y = (screen_h - taskbar_h) - L->h - sm_gap();
    L->main_y = L->y + L->head_h;
    L->pane_x = L->x + L->side_w + 1;
}

static int pane_count(void);

// The bar's rect (the WIDE one, which is also its hit zone), and whether
// the column needs one at all. uui_scrollbar's vertical offset counts
// from the BOTTOM (a scrollback), this list from the top: sb_offset()
// is the one conversion.
static int sb_rect(const struct sm_layout *L, int *x, int *y, int *w, int *h) {
    int bw = ugfx_char_h() - 1;
    *w = bw;
    *x = L->x + L->w - 1 - sm_pill_inset() - bw;
    *y = L->main_y + SM_INSET;
    *h = L->main_h - 2 * SM_INSET;
    return pane_count() > L->pane_rows;
}

static int sb_max_first(const struct sm_layout *L) {
    int m = pane_count() - L->pane_rows;
    return m > 0 ? m : 0;
}

static int sb_offset(const struct sm_layout *L) { return sb_max_first(L) - scroll; }

static void sb_scroll_to(const struct sm_layout *L, int first) {
    int m = sb_max_first(L);
    if (first > m) first = m;
    if (first < 0) first = 0;
    if (first == scroll) return;
    scroll = first;
    start_menu_damage();
    redraw_pending = 1;
}

void start_menu_geometry(int *out_x, int *out_y, int *out_w, int *out_h,
                         int *out_item_h) {
    struct sm_layout L;
    layout(&L);
    if (out_x) *out_x = L.x;
    if (out_y) *out_y = L.y;
    if (out_w) *out_w = L.w;
    if (out_h) *out_h = L.h;
    if (out_item_h) *out_item_h = L.item_h;
}

int start_menu_rect(int *x, int *y, int *w, int *h) {
    struct sm_layout L;
    layout(&L);
    *x = L.x; *y = L.y; *w = L.w; *h = L.h;
    return 1;
}

void start_menu_damage(void) { wm_overlay_damage("start"); }

// The Start BUTTON is drawn lit while the menu is up (wm_render.c's
// draw_taskbar), so opening and closing change it too. With no window
// to change focus and damage the strip for it, the button kept its lit
// look after a dismiss -- damage_sweep.py found it the first time it
// ran on an empty desktop. The button alone, and only on open and
// close: start_menu_damage() runs on every hover change, and a whole
// strip repaint per hover made every Start-menu frame heavier.
static void damage_start_button(void) {
    int x, y, w, h;
    taskbar_start_hit_rect(&x, &y, &w, &h);
    wm_damage_rect(x, y, w, h);
}

// --- what the app column holds ----------------------------------------

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

// Case-insensitive substring, which is what every launcher's search
// does. Deliberately not fuzzy: a match a user cannot explain is worse
// than one they have to spell.
static int matches(const char *name, const char *q) {
    if (!name || !q || !*q) return 1;
    for (const char *s = name; *s; s++) {
        const char *a = s, *b = q;
        while (*a && *b && lower(*a) == lower(*b)) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

// The n'th row of the app column: an app, or a system action matched by
// the query (typing "shut" finds Shutdown, as it does on KDE and
// Windows). Exactly one of *app / *action is set; *action is -1 for an
// app row. Returns 0 past the end.
// HOW WELL A NAME MATCHES, smaller is better. An exact name beats a
// name that starts with the query, which beats one that merely contains
// it -- so "ter" offers Terminal before Crash Test, which is what every
// launcher does and what a person typing three letters means. Ties keep
// the list's own order, which is alphabetical within a folder.
#define SCORE_NONE 4
static int match_score(const char *name, const char *q) {
    if (!name || !q || !*q) return 0;
    const char *a = name, *b = q;
    while (*a && *b && lower(*a) == lower(*b)) { a++; b++; }
    if (!*b) return *a ? 1 : 0;        // prefix, or the whole name
    return matches(name, q) ? 2 : SCORE_NONE;
}

static int pane_row(int n, struct gui_app **app, int *action) {
    if (app) *app = 0;
    if (action) *action = -1;
    if (n < 0) return 0;

    if (!query_len) {
        struct gui_app *a = folder_app_at(sel_cat, n);
        if (!a) return 0;
        if (app) *app = a;
        return 1;
    }

    // BY SCORE, THEN BY ORDER: walk the tiers, and within a tier the
    // apps then the system actions. Re-walked per row rather than
    // sorted into a list, for the reason folder_app_at() gives -- there
    // is no second copy of the ordering to keep true, and the input is
    // a few dozen rows of a menu.
    int apps = gui_app_visible_count(GUI_SHOW_STARTMENU);
    for (int tier = 0; tier < SCORE_NONE; tier++) {
        for (int i = 0; i < apps; i++) {
            struct gui_app *a = gui_app_visible_at(GUI_SHOW_STARTMENU, i);
            if (match_score(a->name, query) != tier) continue;
            if (n-- == 0) { if (app) *app = a; return 1; }
        }
        for (int i = 0; i < wm_system_action_count; i++) {
            if (match_score(wm_system_actions[i].label, query) != tier) continue;
            if (n-- == 0) { if (action) *action = i; return 1; }
        }
    }
    return 0;
}

// How many rows the column SHOWS -- capped at what fits, so the draw,
// the hit test, the keyboard and `gui menu --json` cannot disagree
// about which rows exist. A search with more matches than that shows
// the first of them; nothing scrolls here yet.
// EVERY row the open folder (or the query) has -- not what fits. The
// two were the same number until the pane started scrolling, and every
// place that still conflates them is a bug: `pane_count()` is the list,
// `L.pane_rows` is the window onto it, and `scroll` is where the window
// sits.
static int pane_count(void) {
    int n = 0;
    while (pane_row(n, 0, 0)) n++;
    return n;
}

// The rows actually on screen, and the first of them. Clamped here
// rather than at every caller, and clamping is also what makes a folder
// change safe: the new list is usually shorter than where the old one
// was scrolled to.
static int pane_visible(const struct sm_layout *L, int *first) {
    int total = pane_count();
    int max_first = total - L->pane_rows;
    if (max_first < 0) max_first = 0;
    if (scroll > max_first) scroll = max_first;
    if (scroll < 0) scroll = 0;
    if (first) *first = scroll;
    int n = total - scroll;
    return n < L->pane_rows ? n : L->pane_rows;
}

// WHICH ROW THE STRIP IS ABOUT: the keyboard's selection when there is
// one, else whatever the pointer is over. Selection outranks hover --
// the same rule the row highlights follow, and for the same reason: the
// keyboard's idea of "current" must not be silently overridden by where
// a hand left the mouse.
const char *start_menu_description(void) {
    if (!start_menu_open) return "";
    struct gui_app *a = 0;
    int act = -1;
    if (sel_row >= 0) {
        pane_row(sel_row, &a, &act);
    } else if (hover_token >= TOK_APP(0) && hover_token < TOK_SEARCH) {
        pane_row(scroll + hover_token - TOK_APP(0), &a, &act);
    }
    if (a && a->comment) return a->comment;
    // A system action found by the search has no `Comment=` to show,
    // and inventing one here would put words in a data file's mouth.
    return "";
}

void start_menu_scroll_state(int *out_first, int *out_total) {
    struct sm_layout L;
    layout(&L);
    int first = 0;
    pane_visible(&L, &first);
    if (out_first) *out_first = first;
    if (out_total) *out_total = pane_count();
}

int start_menu_wheel(int mx, int my, int notches) {
    if (!start_menu_open || !notches) return 0;
    struct sm_layout L;
    layout(&L);
    // OVER THE MENU, not just anywhere: a notch somewhere else belongs
    // to whatever is under it, exactly as the tray's wheel is scoped to
    // the tray (wm.c). The whole popup counts, not only the app column
    // -- a pointer resting on the sidebar while the other hand scrolls
    // is still scrolling THIS.
    if (!uui_hit(L.x, L.y, L.w, L.h, mx, my)) return 0;
    int total = pane_count();
    if (total <= L.pane_rows) return 1;   // consumed: nothing behind the menu should scroll
    // Positive notches are "up" (mouse.h), which means EARLIER rows.
    scroll -= notches;
    if (scroll > total - L.pane_rows) scroll = total - L.pane_rows;
    if (scroll < 0) scroll = 0;
    start_menu_damage();
    redraw_pending = 1;
    return 1;
}

// --- rows, as reported and as hit-tested ------------------------------
//
// ONE walk shared by the draw, the hit test, the keyboard and the debug
// console, so no two of them can disagree about where a row is. Order:
// the sidebar's folders, then its actions, then the app column, then the
// search field.

// `apps` is pane_count(), passed in rather than asked for per row: a
// walk of ten rows would otherwise re-scan the registry ten times, and
// the hover op runs this every frame the menu is open.
static int row_rect(const struct sm_layout *L, int apps, int n, const char **label,
                    int *kind, int *x, int *y, int *w, int *h, int *selected) {
    int acts = wm_system_action_count;
    const char *lb = "";
    int k, rx, ry, rw, rh, sel = 0;

    if (n < 0) return 0;
    if (n < L->cats) {
        lb = folder_label_at(n);
        k = START_ROW_CATEGORY;
        rx = L->x; ry = L->main_y + SM_INSET + n * L->item_h;
        rw = L->side_w; rh = L->item_h;
        sel = (n == sel_cat && !query_len);
    } else if (n < L->cats + acts) {
        // THE FOOTER'S BUTTONS, right-aligned in the actions' own order.
        int i = n - L->cats;
        lb = wm_system_actions[i].label;
        k = START_ROW_ACTION;
        int total = 0;
        for (int j = 0; j < acts; j++) total += sm_action_w(j) + 4;
        rx = L->x + L->w - ugfx_char_h() / 2 - total;
        for (int j = 0; j < i; j++) rx += sm_action_w(j) + 4;
        rh = L->btn - 4;
        ry = L->y + L->h - L->foot_h + (L->foot_h - rh) / 2;
        rw = sm_action_w(i);
    } else if (n < L->cats + acts + apps) {
        // `i` IS THE SCREEN ROW; the entry it shows is `scroll + i`.
        // Everything that persists -- the keyboard selection, what a
        // click launches -- is in LIST coordinates, and the two are
        // different numbers the moment the pane scrolls.
        int i = n - L->cats - acts;
        struct gui_app *a; int act;
        pane_row(scroll + i, &a, &act);
        lb = a ? a->name : (act >= 0 ? wm_system_actions[act].label : "");
        k = START_ROW_APP;
        rx = L->pane_x; ry = L->main_y + SM_INSET + i * L->app_h;
        rw = L->pane_w; rh = L->app_h;
        sel = (scroll + i == sel_row);
    } else if (n == L->cats + acts + apps) {
        // The description: reported for its TEXT, with no rect -- it is
        // drawn on the row it describes now, as that row's second line.
        lb = start_menu_description();
        k = START_ROW_DESC;
        rx = L->x; ry = L->main_y; rw = 0; rh = 0;
    } else if (n == L->cats + acts + apps + 1) {
        lb = "Search";
        k = START_ROW_SEARCH;
        rh = ugfx_char_h() + 21;
        rx = L->x + ugfx_char_h();
        ry = L->y + (L->head_h - rh) / 2;
        rw = L->w - 2 * ugfx_char_h() - L->btn - ugfx_char_h() / 2;
    } else if (n == L->cats + acts + apps + 2) {
        lb = "System Settings";
        k = START_ROW_SETTINGS;
        rw = rh = L->btn;
        rx = L->x + L->w - ugfx_char_h() - rw;
        ry = L->y + (L->head_h - rh) / 2;
    } else {
        return 0;
    }

    if (label) *label = lb;
    if (kind) *kind = k;
    if (x) *x = rx;
    if (y) *y = ry;
    if (w) *w = rw;
    if (h) *h = rh;
    if (selected) *selected = sel;
    return 1;
}

int start_menu_row_count(void) {
    struct sm_layout L;
    layout(&L);
    return L.cats + wm_system_action_count + pane_visible(&L, 0) + 3;
}

int start_menu_row_info(int n, const char **label, int *kind,
                        int *x, int *y, int *w, int *h, int *selected) {
    struct sm_layout L;
    layout(&L);
    return row_rect(&L, pane_visible(&L, 0), n, label, kind, x, y, w, h, selected);
}

// The icon box on an app row, for a test that compares those pixels
// against the source artwork. Exported rather than re-derived: the
// offsets moved once and icons_test.py's own copy of them went on
// sampling the old place, which reads as a wrong icon.
int start_menu_row_icon(int n, int *x, int *y, int *sz) {
    struct sm_layout L;
    layout(&L);
    int kind, rx, ry, rw, rh;
    if (!row_rect(&L, pane_visible(&L, 0), n, 0, &kind, &rx, &ry, &rw, &rh, 0)) return 0;
    if (kind != START_ROW_APP) return 0;
    int sz_ = SM_ICON_SZ(L.app_h);
    if (x) *x = rx + sm_pill_inset() + 12;   // start_menu_draw()'s app icon
    if (y) *y = ry + (rh - sz_) / 2;
    if (sz) *sz = sz_;
    return 1;
}

const char *start_menu_query(void) { return query; }

const char *start_menu_category(void) {
    return folder_label_at(sel_cat);
}

// Which row (mx, my) lands on, or -1 -- THE ONE COPY of that
// arithmetic, walked rather than divided, because the rows are no
// longer one uniform column.
static int row_at(int mx, int my) {
    if (!start_menu_open) return -1;
    struct sm_layout L;
    layout(&L);
    int apps = pane_visible(&L, 0);
    int total = L.cats + wm_system_action_count + apps + 3;
    for (int i = 0; i < total; i++) {
        int x, y, w, h, kind;
        if (!row_rect(&L, apps, i, 0, &kind, &x, &y, &w, &h, 0)) break;
        // THE STRIP IS NOT A CONTROL. It sits between the list and the
        // field, so a click passing over it must fall through to the
        // menu's own "clicked nothing" rather than land on a row.
        if (kind == START_ROW_DESC) continue;
        if (uui_hit(x, y, w, h, mx, my)) return i;
    }
    return -1;
}

struct gui_app *start_menu_app_at(int mx, int my) {
    struct sm_layout L;
    layout(&L);
    int n = row_at(mx, my);
    if (n < L.cats + wm_system_action_count) return 0;
    if (n >= L.cats + wm_system_action_count + pane_visible(&L, 0)) return 0;
    struct gui_app *a = 0;
    pane_row(scroll + n - L.cats - wm_system_action_count, &a, 0);
    return a;
}

// WHAT THE POINTER IS RESTING ON, handed to the tooltip every frame.
// Told rather than asked, because the menu is the only thing that knows
// which row is which -- and saying the same thing twice is free
// (wm_tooltip_track() compares before it re-arms).
static void track_tooltip(const struct sm_layout *L, int n) {
    int apps = pane_visible(L, 0);
    int acts = wm_system_action_count;
    if (n < L->cats + acts || n >= L->cats + acts + apps) {
        wm_tooltip_cancel();
        return;
    }
    struct gui_app *a = 0; int act;
    pane_row(scroll + n - L->cats - acts, &a, &act);
    if (!a || !a->comment || !a->comment[0]) { wm_tooltip_cancel(); return; }
    int x, y, w, h;
    if (!row_rect(L, apps, n, 0, 0, &x, &y, &w, &h, 0)) { wm_tooltip_cancel(); return; }
    // ONLY WHEN THE ROW CUT IT: the row's second line already says what
    // the app is, and a tooltip repeating it covers the next row.
    int room = w - 2 * sm_pill_inset() - 10 - (12 + SM_ICON_SZ(L->app_h) + 12);
    if (ugfx_text_width(a->comment) <= room) { wm_tooltip_cancel(); return; }
    wm_tooltip_track(a->comment, x, y, w, h);
}

int start_menu_hover_at(int mx, int my) {
    if (!start_menu_open) { hover_token = 0; wm_tooltip_cancel(); return 0; }
    struct sm_layout L;
    layout(&L);
    int bx, by, bw, bh;
    if (sb_rect(&L, &bx, &by, &bw, &bh) && uui_hit(bx, by, bw, bh, mx, my)) {
        wm_tooltip_cancel();
        hover_token = TOK_SCROLLBAR;
        return hover_token;
    }
    int n = row_at(mx, my);
    track_tooltip(&L, n);
    if (n < 0) { hover_token = 0; return 0; }
    int acts = wm_system_action_count;
    if (n < L.cats) hover_token = TOK_CAT(n);
    else if (n < L.cats + acts) hover_token = TOK_ACTION(n - L.cats);
    else if (n < L.cats + acts + pane_visible(&L, 0)) hover_token = TOK_APP(n - L.cats - acts);
    else if (n == L.cats + acts + pane_visible(&L, 0) + 2) hover_token = TOK_SETTINGS;
    else hover_token = TOK_SEARCH;
    return hover_token;
}

// A pin or unpin that keeps the open folder the SAME folder. sel_cat is
// an index, and Favourites appearing or going shifts every index after
// it -- the pane would swap to another folder under the pointer.
void start_menu_toggle_pin(const char *app_id) {
    enum folder_kind k = folder_kind_at(sel_cat);
    int cat = sel_cat - pseudo_count();          // FOLDER_CAT's offset
    if (start_store_is_pinned(app_id)) start_store_unpin(app_id);
    else start_store_pin(app_id);
    if (k == FOLDER_CAT) {
        sel_cat = pseudo_count() + cat;
    } else {
        sel_cat = 0;                             // it went: open's default
        for (int i = 0; i < pseudo_count(); i++)
            if (folder_kind_at(i) == k) { sel_cat = i; break; }
    }
    if (k == FOLDER_FAV || folder_kind_at(sel_cat) != k) {
        sel_row = -1;                            // a different list now
        scroll = 0;
    }
    start_menu_damage();
    redraw_pending = 1;
}

// --- open / close -----------------------------------------------------

void start_menu_open_now(void) {
    wm_overlay_close_others("start");
    start_menu_open = 1;
    g_boot_menu = ubootmenu_read(&g_boot, 0) >= 2 && g_boot.oneshot;
    snapshot_recent();
    flash_row = -1;
    hover_token = 0;
    // OPENS ON WHAT YOU USE, when there is such a thing: Favourites if
    // anything is pinned, else Recent if anything has been launched,
    // else All -- which is also what a machine on its first boot gets,
    // rather than an arbitrary category. Kickoff opens on Favorites for
    // the same reason. The folder list already puts these first, so
    // this is index 0 either way; the reset is what matters, since a
    // menu that reopened where it was left makes the same click do
    // different things on different days.
    sel_cat = 0;
    sel_row = -1;
    scroll = 0;
    query[0] = '\0';
    query_len = 0;
    start_menu_damage();
    damage_start_button();
}

void start_menu_close(void) {
    if (!start_menu_open) return;
    wm_tooltip_cancel();   // it describes a row that is about to not exist
    start_menu_open = 0;
    flash_row = -1;
    hover_token = 0;
    start_menu_damage(); // the rows it just vacated
    damage_start_button();
    redraw_pending = 1;
}

// --- drawing ----------------------------------------------------------

// A magnifier, drawn rather than blitted: it is three primitives and an
// icon file would be a fourth thing to seed for a glyph this small.
static void draw_magnifier(int cx, int cy, int r, uint32_t c) {
    ugfx_draw_circle(wm_surface(), cx, cy, r, c, GEOM_AA);
    ugfx_draw_line(wm_surface(), cx + r - 1, cy + r - 1,
                   cx + r + r / 2, cy + r + r / 2, c, GEOM_AA);
}

// A stroked chevron, the card's submenu mark (ui/uui_menubar.c).
static void sm_chevron(int cx, int cy, uint32_t c) {
    int r = ugfx_char_h() / 3;
    if (r < 3) r = 3;
    for (int d = 0; d < 2; d++) {
        ugfx_draw_line(wm_surface(), cx - r / 2 + d, cy - r, cx + r / 2 + d, cy, c, GEOM_AA);
        ugfx_draw_line(wm_surface(), cx + r / 2 + d, cy, cx - r / 2 + d, cy + r, c, GEOM_AA);
    }
}

void start_menu_draw(int mx, int my) {
    if (!start_menu_open) return;
    struct sm_layout L;
    layout(&L);
    struct ugfx_surface *s = wm_surface();
    int r = uui_popup_radius();
    int pr = sm_pill_inset();
    int ch = ugfx_char_h();
    wm_shadow_draw(L.x, L.y, L.w, L.h, r, WM_SHADOW_POPUP);

    // THE TOOLKIT'S CARD (ui/uui_popup.h): the header, the rail and the
    // footer on a step of chrome, the app column on the card's lighter
    // ground -- Kickoff's bands in this theme's roles.
    uint32_t edge = uui_popup_border(), fg = UTHEME_TEXT;
    uint32_t pane_bg = uui_popup_bg();
    uint32_t bg = ugfx_blend(pane_bg, UTHEME_CHROME, 128);
    uint32_t dim = ugfx_blend(fg, pane_bg, 110);
    // Hover comes from uui_state_bg(), derived from the row's OWN
    // colour, rather than a hand-picked tint (docs/gui-guidelines.md).
    uint32_t side_hover = uui_state_bg(bg, UUI_STATE_HOVER);
    uint32_t pane_hover = uui_state_bg(pane_bg, UUI_STATE_HOVER);
    // The click flash stays a deliberately DISTINCT warm colour, not a
    // ui_state derivation: it is not an interaction state, it is a
    // momentary confirmation that a row was chosen.
    uint32_t flash_bg = ugfx_rgb(230, 190, 90);
    uint32_t flash_fg = UTHEME_WHITE;
    uint32_t rule = ugfx_blend(edge, pane_bg, 96);

    // TRACKED, not derived: see hover_token. (mx, my) still arrive so
    // the signature matches the other overlays.
    (void)mx; (void)my;

    uui_fill_round_rect(s, L.x, L.y, L.w, L.h, r, edge);
    uui_fill_round_rect(s, L.x + 1, L.y + 1, L.w - 2, L.h - 2, r - 1, bg);
    ugfx_fill_rect(s, L.pane_x, L.main_y, L.x + L.w - 1 - L.pane_x, L.main_h, pane_bg);
    ugfx_fill_rect(s, L.x + 1, L.main_y, L.w - 2, 1, rule);
    ugfx_fill_rect(s, L.x + 1, L.main_y + L.main_h - 1, L.w - 2, 1, rule);
    ugfx_fill_rect(s, L.x + L.side_w, L.main_y, 1, L.main_h, rule);
    {
        // The version, quiet, where Kickoff puts the user's name.
        int fy = L.main_y + L.main_h;
        ugfx_draw_string_clipped(s, L.x + ch, fy + (L.foot_h - ch) / 2, L.w / 3,
                                 "toy-os " TOYOS_VERSION, dim, bg);
    }

    int apps = pane_visible(&L, 0);
    int total = L.cats + wm_system_action_count + apps + 3;
    int icon_sz = SM_ICON_SZ(L.app_h);
    for (int i = 0; i < total; i++) {
        const char *label; int kind, x, y, w, h, selected;
        if (!row_rect(&L, apps, i, &label, &kind, &x, &y, &w, &h, &selected)) break;
        if (kind == START_ROW_DESC) continue;   // each app row draws its own

        if (kind == START_ROW_SEARCH) {
            // A field with the query in it, always focused -- there is
            // nothing else here that takes text, so a caret is drawn
            // unconditionally rather than following a focus that cannot
            // move. Windows 11's and Breeze's: rounded, an accent rule.
            int fr = pr + 2;
            uui_fill_round_rect(s, x, y, w, h, fr, UTHEME_OUTLINE);
            uui_fill_round_rect(s, x + 1, y + 1, w - 2, h - 2, fr - 1, UTHEME_WHITE);
            ugfx_fill_rect(s, x + fr, y + h - 2, w - 2 * fr, 2, UTHEME_ACCENT);
            int mr = h / 5;
            draw_magnifier(x + 10 + mr, y + h / 2 - 1, mr, ugfx_blend(fg, UTHEME_WHITE, 80));
            int tx = x + 16 + mr * 3;
            int ty = y + (h - ch) / 2;
            int tw = w - (tx - x) - 6;
            if (query_len) {
                ugfx_draw_string_clipped(s, tx, ty, tw, query, fg, UTHEME_WHITE);
                int caret = tx + ugfx_text_width(query);
                if (caret < x + w - 3) ugfx_fill_rect(s, caret + 1, ty, 1, ch, fg);
            } else {
                ugfx_draw_string_clipped(s, tx, ty, tw, "Search",
                                         ugfx_blend(fg, UTHEME_WHITE, 120), UTHEME_WHITE);
                ugfx_fill_rect(s, tx - 1, ty, 1, ch, fg);
            }
            continue;
        }

        int hot = 0;
        if (kind == START_ROW_CATEGORY) hot = (hover_token == TOK_CAT(i));
        else if (kind == START_ROW_ACTION) hot = (hover_token == TOK_ACTION(i - L.cats));
        else if (kind == START_ROW_SETTINGS) hot = (hover_token == TOK_SETTINGS);
        else hot = (hover_token == TOK_APP(i - L.cats - wm_system_action_count));
        int on_pane = (kind == START_ROW_APP);
        uint32_t row_bg = on_pane ? pane_bg : bg, row_fg = fg;

        // A ROW IS A PILL, inset from its column -- the card's. Footer and
        // header buttons are their own rects already.
        int px = x, pw = w;
        if (kind == START_ROW_CATEGORY || kind == START_ROW_APP) { px = x + pr; pw = w - 2 * pr; }
        if (i == flash_row) {
            row_bg = flash_bg; row_fg = flash_fg;
            uui_fill_round_rect(s, px, y, pw, h, pr, row_bg);
        } else if (selected) {
            // SELECTION OUTRANKS HOVER -- the soft accent fill with its
            // 1px edge (docs/gui-guidelines.md), and a folder's accent
            // bar, Windows 11's navigation-pane mark.
            row_bg = UTHEME_SELECTION;
            uui_fill_round_rect(s, px, y, pw, h, pr, ugfx_blend(UTHEME_SELECTION, UTHEME_ACCENT, 110));
            uui_fill_round_rect(s, px + 1, y + 1, pw - 2, h - 2, pr - 1, row_bg);
            if (kind == START_ROW_CATEGORY)
                uui_fill_round_rect(s, px + 2, y + h / 4, 3, h / 2, UUI_CAPSULE, UTHEME_ACCENT);
        } else if (hot) {
            row_bg = on_pane ? pane_hover : side_hover;
            uui_fill_round_rect(s, px, y, pw, h, pr, row_bg);
        }

        if (kind == START_ROW_SETTINGS) {
            const struct uimg *ico = icon_get("tb-gear", ch + 3);
            if (ico) ugfx_blit_alpha(s, x + (w - ico->w) / 2, y + (h - ico->h) / 2,
                                     ico->w, ico->h, ico->px, ico->w);
            continue;
        }
        if (kind == START_ROW_ACTION) {
            // A labelled footer button: Shut down with the power glyph,
            // Restart with a chevron when a boot menu is behind it.
            int tx = x + ch;
            if (!k_strcmp(label, "Shutdown")) {
                const struct uimg *ico = icon_get("tb-power", ch + 3);
                if (ico) ugfx_blit_alpha(s, tx, y + (h - ico->h) / 2, ico->w, ico->h, ico->px, ico->w);
                tx += ch + 9;
            }
            ugfx_draw_string_clipped(s, tx, y + (h - ch) / 2, x + w - tx, label, row_fg, row_bg);
            if (!k_strcmp(label, "Restart") && g_boot_menu)
                sm_chevron(x + w - ch, y + h / 2, row_fg);
            continue;
        }

        if (kind == START_ROW_APP) {
            // TWO LINES: the name, and what the app IS under it -- the
            // Comment= the description strip used to show for one row at
            // a time. THE INDENT IS UNCONDITIONAL, the icon is not, so a
            // row with no artwork (Crash Test, on purpose) lines up.
            struct gui_app *a; int act;
            pane_row(scroll + i - L.cats - wm_system_action_count, &a, &act);
            int ix = px + 12;
            int text_x = ix + icon_sz + 12;
            int right = px + pw - 10;
            const struct uimg *ico = a ? icon_get(a->icon_name, icon_sz) : 0;
            if (ico) ugfx_blit_alpha(s, ix, y + (h - ico->h) / 2, ico->w, ico->h, ico->px, ico->w);
            const char *note = a && a->comment ? a->comment : "";
            if (note[0]) {
                int top = y + (h - 2 * ch - 4) / 2;
                ugfx_draw_string_elided(s, text_x, top, right - text_x, label, row_fg, row_bg);
                ugfx_draw_string_elided(s, text_x, top + ch + 4, right - text_x, note,
                                        i == flash_row ? row_fg : ugfx_blend(row_fg, row_bg, 110), row_bg);
            } else {
                ugfx_draw_string_elided(s, text_x, y + (h - ch) / 2, right - text_x, label,
                                        row_fg, row_bg);
            }
            continue;
        }

        // A folder: its own icon, on the same terms -- the column is
        // booked whether or not the file exists, so a theme missing one
        // folder's artwork does not move the other labels.
        int fsz = SM_FOLDER_SZ(L.item_h);
        int text_x = px + 12 + fsz + 10;
        const struct uimg *fico = icon_get(folder_icon_at(i), fsz);
        if (fico)
            ugfx_blit_alpha(s, px + 12, y + (h - fico->h) / 2, fico->w, fico->h, fico->px, fico->w);
        // Elided, for the same reason the strip is -- and a row label
        // that ran past its column used to be drawn through the border
        // before it was even clipped.
        ugfx_draw_string_elided(s, text_x, y + (h - ch) / 2, px + pw - 8 - text_x,
                                label, row_fg, row_bg);
    }

    // THE SCROLLBAR, an overlay over the rows' right end: a thin thumb
    // at rest, the shared uui_scrollbar's groove and thumb while the
    // pointer is on it or holds it -- so it says where you are without
    // costing the rows a gutter, and it can be dragged.
    int bx, by, bw, bh;
    if (sb_rect(&L, &bx, &by, &bw, &bh)) {
        int total_rows = pane_count();
        if (sb_drag || hover_token == TOK_SCROLLBAR) {
            uui_scrollbar_draw(s, bx, by, bw, bh, total_rows, L.pane_rows, sb_offset(&L),
                               ugfx_blend(pane_bg, fg, 24),
                               ugfx_blend(pane_bg, fg, sb_drag ? 170 : 130), 0);
        } else {
            int ty, th;
            uui_scrollbar_thumb_rect(by, bh, total_rows, L.pane_rows, sb_offset(&L), &ty, &th, bw, 0);
            uui_fill_round_rect(s, bx + bw - 4, ty, 3, th, UUI_CAPSULE, UTHEME_OUTLINE);
        }
    }
}

// --- acting on a row ---------------------------------------------------

static void flash(int row) {
    flash_row = row;
    flash_until = sys_ticks() + START_MENU_FLASH_TICKS;
    start_menu_damage();
}

// Runs whatever row `n` is. Returns 1 when the menu should flash and
// close, 0 when it stays up (selecting a folder is not a commit).
static int activate(int n) {
    struct sm_layout L;
    layout(&L);
    int acts = wm_system_action_count;
    if (n < 0) return 0;
    if (n < L.cats) {
        sel_cat = n;
        sel_row = -1;
        scroll = 0;
        query[0] = '\0';
        query_len = 0;
        start_menu_damage();
        redraw_pending = 1;
        return 0;
    }
    if (n < L.cats + acts) {
        wm_system_actions[n - L.cats].on_select();
        return !context_menu_open;      // a flyout keeps Start up under it
    }
    if (n < L.cats + acts + pane_visible(&L, 0)) {
        struct gui_app *a; int act;
        pane_row(scroll + n - L.cats - acts, &a, &act);
        if (a) open_app(a);
        else if (act >= 0) wm_system_actions[act].on_select();
        return !context_menu_open;
    }
    if (n == L.cats + acts + pane_visible(&L, 0) + 2) {
        // The header's settings button: System Settings, by its app id.
        for (int i = 0; i < gui_app_registry_count; i++)
            if (gui_app_registry[i].app_id && !k_strcmp(gui_app_registry[i].app_id, "settings")) {
                open_app(&gui_app_registry[i]);
                return 1;
            }
        return 0;
    }
    return 0;   // the search field: clicking it changes nothing, it is always focused
}

int start_menu_handle_click(int mx, int my) {
    if (!start_menu_open) return 0;

    // THE BAR TAKES ITS CLICK before the rows under its right end: the
    // thumb starts a drag, the groove pages toward the click and keeps
    // paging while the button is held (start_menu_update_press()).
    {
        struct sm_layout L;
        layout(&L);
        int bx, by, bw, bh;
        if (sb_rect(&L, &bx, &by, &bw, &bh) && uui_hit(bx, by, bw, bh, mx, my)) {
            int total = pane_count();
            enum uui_scrollbar_zone z = uui_scrollbar_hit(bx, by, bw, bh, total, L.pane_rows,
                                                          sb_offset(&L), mx, my, 0);
            if (z == UUI_SB_THUMB) {
                int ty, th;
                uui_scrollbar_thumb_rect(by, bh, total, L.pane_rows, sb_offset(&L), &ty, &th, bw, 0);
                sb_drag = 1;
                sb_grab = my - ty;
            } else if (z == UUI_SB_ABOVE || z == UUI_SB_BELOW) {
                sb_page = z == UUI_SB_ABOVE ? -1 : 1;
                sb_scroll_to(&L, scroll + sb_page * L.pane_rows);
                sb_next = sys_ticks() + SB_REPEAT_DELAY;
            }
            hover_token = TOK_SCROLLBAR;
            start_menu_damage();
            redraw_pending = 1;
            return 1;
        }
    }

    int n = row_at(mx, my);
    if (n < 0) {
        // Clicked elsewhere while the menu was open (the desktop, a
        // window) -- no row was selected, so there's nothing to flash.
        start_menu_close();
    } else if (activate(n)) {
        // The row's action already ran -- only closing is deferred, so
        // the click gets a brief visible flash instead of vanishing in
        // the same frame it landed. start_menu_update() closes it.
        flash(n);
    }
    redraw_pending = 1;
    return 1;
}

// --- the keyboard -----------------------------------------------------

static void query_changed(void) {
    sel_row = pane_count() > 0 ? 0 : -1;
    scroll = 0;             // a new list is read from its top
    start_menu_damage();
}

// KEEP THE SELECTION ON SCREEN. The arrows move a LIST index and the
// pane shows a window onto that list, so without this the highlight
// walks off the bottom and the keyboard appears to stop working while
// Enter goes on launching something invisible.
static void scroll_to_selection(const struct sm_layout *L) {
    if (sel_row < 0) return;
    if (sel_row < scroll) scroll = sel_row;
    if (sel_row >= scroll + L->pane_rows) scroll = sel_row - L->pane_rows + 1;
    if (scroll < 0) scroll = 0;
}

int start_menu_key(int key, uint8_t mods) {
    if (!start_menu_open) return 0;
    // A BOUND SHORTCUT IS NEVER SHADOWED BY AN OPEN MENU: anything held
    // with a modifier falls through to wm_shortcut.c and to Alt+F4.
    // Ctrl combinations arrive as control CODES rather than as a mod bit
    // (api/keyboard.h), so those fall out below by not being printable.
    if (mods & (KEY_MOD_CTRL | KEY_MOD_ALT | KEY_MOD_ALTGR | KEY_MOD_SUPER)) return 0;

    struct sm_layout L;
    layout(&L);
    int rows = pane_count();

    switch (key) {
    case 0x1B: // Esc clears the query first and closes only then --
               // one level at a time, as docs/gui-guidelines.md says.
        if (query_len) {
            query[0] = '\0';
            query_len = 0;
            query_changed();
            redraw_pending = 1;
        } else {
            start_menu_close();
        }
        return 1;
    case '\n':
    case '\r':
        if (rows > 0) {
            // activate() takes a SCREEN row and sel_row is a LIST index:
            // bring the selection into view, then subtract the scroll.
            scroll_to_selection(&L);
            int first = 0;
            pane_visible(&L, &first);
            int row = sel_row >= 0 ? sel_row : first;
            int n = L.cats + wm_system_action_count + (row - first);
            g_from_key = 1;
            if (activate(n)) flash(n);
            g_from_key = 0;
            redraw_pending = 1;
        }
        return 1;
    case '\b':
    case 0x7F:
        if (query_len) {
            query[--query_len] = '\0';
            query_changed();
            redraw_pending = 1;
        }
        return 1;
    case KEY_ARROW_DOWN:
        if (rows > 0) sel_row = (sel_row + 1 >= rows) ? 0 : sel_row + 1;
        scroll_to_selection(&L);
        start_menu_damage();
        redraw_pending = 1;
        return 1;
    case KEY_ARROW_UP:
        if (rows > 0) sel_row = (sel_row <= 0) ? rows - 1 : sel_row - 1;
        scroll_to_selection(&L);
        start_menu_damage();
        redraw_pending = 1;
        return 1;
    // A PAGE IS THE PANE, which is what makes Page Down land where the
    // eye already is rather than at some fixed count of rows.
    case KEY_PAGE_DOWN:
    case KEY_PAGE_UP: {
        if (rows <= 0) return 1;
        int step = L.pane_rows > 1 ? L.pane_rows - 1 : 1;
        sel_row = (sel_row < 0 ? 0 : sel_row) +
                  (key == KEY_PAGE_DOWN ? step : -step);
        if (sel_row < 0) sel_row = 0;
        if (sel_row >= rows) sel_row = rows - 1;
        scroll_to_selection(&L);
        start_menu_damage();
        redraw_pending = 1;
        return 1;
    }
    case KEY_HOME:
    case KEY_END:
        if (rows > 0) sel_row = (key == KEY_HOME) ? 0 : rows - 1;
        scroll_to_selection(&L);
        start_menu_damage();
        redraw_pending = 1;
        return 1;
    case KEY_ARROW_LEFT:
    case KEY_ARROW_RIGHT:
        // FOLDERS, not panes: there is no second focusable thing here,
        // so left/right is the cheapest way to reach every folder
        // without a focus model. Inert while searching, where the
        // results span every folder.
        if (!query_len && L.cats > 0) {
            sel_cat += (key == KEY_ARROW_RIGHT) ? 1 : -1;
            if (sel_cat < 0) sel_cat = L.cats - 1;
            if (sel_cat >= L.cats) sel_cat = 0;
            sel_row = -1;
            scroll = 0;   // a different folder is read from its top
            start_menu_damage();
            redraw_pending = 1;
        }
        return 1;
    default:
        break;
    }

    if (key >= 32 && key < 127 && query_len < START_QUERY_MAX - 1) {
        query[query_len++] = (char)key;
        query[query_len] = '\0';
        query_changed();
        redraw_pending = 1;
        return 1;
    }
    return 0;
}

void start_menu_update(void) {
    if (flash_row < 0) return;
    if (sys_ticks() >= flash_until) {
        // THROUGH start_menu_close(), not by clearing the flag here.
        // Three paths closed this menu and two of them did it inline,
        // so anything the close has to tidy up was done in one of them
        // and forgotten in the others -- measured: a launch left the
        // hover TOOLTIP on screen over a menu that had gone.
        flash_row = -1;
        start_menu_close();
    }
}

// The overlay's press op, every tick: a thumb drag follows the pointer,
// and a held groove click repeats its page while the thumb has not yet
// reached the pointer -- a press is a gesture, not an edge.
void start_menu_update_press(int mx, int my, uint8_t buttons) {
    if (!start_menu_open || (!sb_drag && !sb_page)) return;
    struct sm_layout L;
    layout(&L);
    int bx, by, bw, bh;
    if (!(buttons & 0x1) || !sb_rect(&L, &bx, &by, &bw, &bh)) {
        sb_drag = 0;
        sb_page = 0;
        start_menu_damage();
        redraw_pending = 1;
        return;
    }
    int total = pane_count();
    if (sb_drag) {
        int off = uui_scrollbar_offset_for_drag(by, bh, total, L.pane_rows, my, sb_grab, bw, 0);
        sb_scroll_to(&L, sb_max_first(&L) - off);
        return;
    }
    (void)mx;
    if (sys_ticks() < sb_next) return;
    enum uui_scrollbar_zone z = uui_scrollbar_hit(bx, by, bw, bh, total, L.pane_rows,
                                                  sb_offset(&L), bx + bw / 2, my, 0);
    if ((sb_page < 0 && z != UUI_SB_ABOVE) || (sb_page > 0 && z != UUI_SB_BELOW)) return;
    sb_scroll_to(&L, scroll + sb_page * L.pane_rows);
    sb_next = sys_ticks() + SB_REPEAT_EVERY;
}

int start_menu_scrollbar(int *x, int *y, int *w, int *h, int *thumb_y, int *thumb_h, int *wide) {
    if (!start_menu_open) return 0;
    struct sm_layout L;
    layout(&L);
    if (!sb_rect(&L, x, y, w, h)) return 0;
    uui_scrollbar_thumb_rect(*y, *h, pane_count(), L.pane_rows, sb_offset(&L), thumb_y, thumb_h, *w, 0);
    *wide = sb_drag || hover_token == TOK_SCROLLBAR;
    return 1;
}
