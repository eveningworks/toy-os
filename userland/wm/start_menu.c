// See start_menu.h.
#include "start_menu.h"
#include "wm_shadow.h"
#include "start_store.h"
#include "wm_tooltip.h"
#include "wm_overlay.h"
#include "lib/icon_cache.h"
#include "wm_internal.h"
#include "wm_taskbar.h"   // taskbar_start_rect()
#include "confirm_dialog.h"
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
static void action_restart(void) {
    confirm_dialog_open_with("Restart? Unsaved changes will be lost.", do_restart, 0);
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

// The breathing room above the first row and below the last, which a
// menu whose rows sit flush against its border does not have.
#define SM_INSET 4
// The icon on an app row, and the column it reserves -- stated once, so
// the width the layout books and the pixels the draw puts there cannot
// drift apart.
#define SM_ICON_SZ(item_h)  ((item_h) - 6)
#define SM_ICON_COL(item_h) (SM_ICON_SZ(item_h) + 10)
// A FOLDER'S icon is smaller than an app's: it labels a list rather
// than standing for a program, and Kickoff and Whisker both draw it
// that way.
#define SM_FOLDER_SZ(item_h) ((item_h) - 10)

// HOW TALL THE APP COLUMN IS ALLOWED TO GET, in rows. Beyond this it
// SCROLLS: a menu as tall as its biggest folder was fine at nineteen
// apps and is not a design, and "All Applications" is unbounded by
// construction. Ten rows is about a third of a 720p screen with the
// default font, and it is font-derived like everything else here.
#define SM_MAX_ROWS 10

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

struct sm_layout {
    int x, y, w, h;   // the whole popup
    int item_h;
    int side_w;       // the category column
    int pane_x, pane_w, pane_h, pane_rows; // the app column; rows it can SHOW
    int cats;         // folders in the sidebar
    int sep_h;        // the divider between folders and actions
    int desc_h;       // the strip that says what the hovered app IS
    int search_h;
};

static void layout(struct sm_layout *L) {
    // A ROW IS TALLER THAN A MENU BAR'S, because this is a launcher a
    // pointer aims at rather than a list being scanned -- Breeze and
    // Windows 11 both give a launcher row about twice a text line's
    // leading. Font-derived, so it follows `font_size`.
    L->item_h = ugfx_char_h() + 10;
    L->cats = folder_count();

    int side_label = 0;
    for (int i = 0; i < L->cats; i++) {
        int n = ugfx_text_width(folder_label_at(i));
        if (n > side_label) side_label = n;
    }
    for (int i = 0; i < wm_system_action_count; i++) {
        int n = ugfx_text_width(wm_system_actions[i].label);
        if (n > side_label) side_label = n;
    }
    // The folder icon's column, on the same terms as the app column's:
    // booked whether or not every folder has artwork, so a missing icon
    // file cannot move the labels.
    L->side_w = side_label + 20 + SM_FOLDER_SZ(L->item_h) + 6;

    // MEASURED, not counted: the column is as wide as its widest label
    // DRAWS, which on a proportional face is not its longest label times
    // the widest advance -- that came out ~1.8x too wide. Measured over
    // EVERY app rather than the open folder's, so switching folders
    // never moves the menu's edge.
    int app_label = 0;
    int apps = gui_app_visible_count(GUI_SHOW_STARTMENU);
    for (int i = 0; i < apps; i++) {
        int n = ugfx_text_width(gui_app_visible_at(GUI_SHOW_STARTMENU, i)->name);
        if (n > app_label) app_label = n;
    }
    // ROOM FOR THE ICON COLUMN, whether or not every row has one: rows
    // with an icon indent their label by it, and a width that ignored
    // it would clip the longest label the moment artwork arrived.
    L->pane_w = app_label + 16 + SM_ICON_COL(L->item_h);

    L->sep_h = L->item_h / 2;
    int side_h = L->cats * L->item_h + L->sep_h + wm_system_action_count * L->item_h;
    // AS TALL AS THE SIDEBAR, OR AS THE BIGGEST FOLDER, WHICHEVER IS
    // SMALLER-BUT-ENOUGH -- and never past SM_MAX_ROWS, which is what
    // the scrolling is for. The sidebar's own height is a floor rather
    // than a target: a pane shorter than the folders beside it would
    // leave the divider running past the bottom of the list.
    int biggest = 0;
    for (int i = 0; i < L->cats; i++) {
        int n = 0;
        while (folder_app_at(i, n)) n++;
        if (n > biggest) biggest = n;
    }
    if (biggest > SM_MAX_ROWS) biggest = SM_MAX_ROWS;
    int content_h = side_h > biggest * L->item_h ? side_h : biggest * L->item_h;
    L->pane_h = content_h + 2 * SM_INSET;
    L->pane_rows = L->item_h ? content_h / L->item_h : 0;
    L->search_h = L->item_h + 8;
    // ONE LINE ABOUT THE ROW UNDER THE POINTER, from `Comment=`. A
    // strip rather than a second line per row: two-line rows would make
    // the menu half as tall again for text that is only useful about
    // ONE row at a time, which is what a status line is for. The strip
    // is always there, empty or not -- a menu whose height changed as
    // the pointer moved would be worse than a blank line.
    L->desc_h = ugfx_char_h() + 6;

    L->w = L->side_w + 1 + L->pane_w;
    L->h = L->pane_h + L->desc_h + L->search_h;
    // Over the Start button, wherever the style put it -- clamped to the
    // screen, so a centred button's menu cannot hang off the right edge.
    {
        int sx, sy, sw, sh;
        taskbar_start_rect(&sx, &sy, &sw, &sh);
        L->x = sx;
        // Centred Start, centred menu -- Windows 11's.
        if (taskbar_start_centered())
            L->x = screen_w / 2 - L->w / 2;
        if (L->x + L->w > screen_w - 4) L->x = screen_w - 4 - L->w;
        if (L->x < 4) L->x = 4;
    }
    L->y = (screen_h - taskbar_h) - L->h;
    L->pane_x = L->x + L->side_w + 1;
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
        rx = L->x; ry = L->y + SM_INSET + n * L->item_h;
        rw = L->side_w; rh = L->item_h;
        sel = (n == sel_cat && !query_len);
    } else if (n < L->cats + acts) {
        int i = n - L->cats;
        lb = wm_system_actions[i].label;
        k = START_ROW_ACTION;
        rx = L->x;
        ry = L->y + SM_INSET + L->cats * L->item_h + L->sep_h + i * L->item_h;
        rw = L->side_w; rh = L->item_h;
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
        rx = L->pane_x; ry = L->y + SM_INSET + i * L->item_h;
        rw = L->pane_w; rh = L->item_h;
        sel = (scroll + i == sel_row);
    } else if (n == L->cats + acts + apps) {
        // The description strip. Reported as a row so a test can find
        // it and read what it says, and never hit-testable: it is a
        // status line, and a click there belongs to nothing.
        lb = start_menu_description();
        k = START_ROW_DESC;
        rx = L->x; ry = L->y + L->pane_h; rw = L->w; rh = L->desc_h;
    } else if (n == L->cats + acts + apps + 1) {
        lb = "Search";
        k = START_ROW_SEARCH;
        rx = L->x; ry = L->y + L->pane_h + L->desc_h; rw = L->w; rh = L->search_h;
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
    return L.cats + wm_system_action_count + pane_visible(&L, 0) + 2;
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
    if (x) *x = rx + 6;
    if (y) *y = ry + 3;
    if (sz) *sz = SM_ICON_SZ(L.item_h);
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
    int total = L.cats + wm_system_action_count + apps + 2;
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
    wm_tooltip_track(a->comment, x, y, w, h);
}

int start_menu_hover_at(int mx, int my) {
    if (!start_menu_open) { hover_token = 0; wm_tooltip_cancel(); return 0; }
    struct sm_layout L;
    layout(&L);
    int n = row_at(mx, my);
    track_tooltip(&L, n);
    if (n < 0) { hover_token = 0; return 0; }
    int acts = wm_system_action_count;
    if (n < L.cats) hover_token = TOK_CAT(n);
    else if (n < L.cats + acts) hover_token = TOK_ACTION(n - L.cats);
    else if (n < L.cats + acts + pane_visible(&L, 0)) hover_token = TOK_APP(n - L.cats - acts);
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

void start_menu_draw(int mx, int my) {
    if (!start_menu_open) return;
    struct sm_layout L;
    layout(&L);
    wm_shadow_draw(L.x, L.y, L.w, L.h, 0, WM_SHADOW_POPUP);

    uint32_t bg = UTHEME_PANEL_BG, border = UTHEME_BORDER, fg = UTHEME_TEXT;
    // Hover comes from uui_state_bg(), derived from the row's OWN
    // colour, rather than a hand-picked tint: docs/gui-guidelines.md
    // says not to pick tints (a fixed "lighter" wash is what made hover
    // invisible on this near-white theme once), and a fixed dark blue
    // also forced the label to white, so the menu had a second text
    // colour nothing else used.
    uint32_t side_hover = uui_state_bg(bg, UUI_STATE_HOVER);
    uint32_t pane_bg = UTHEME_WHITE;
    uint32_t pane_hover = uui_state_bg(pane_bg, UUI_STATE_HOVER);
    // The click flash stays a deliberately DISTINCT warm colour, not a
    // ui_state derivation: it is not an interaction state, it is a
    // momentary confirmation that a row was chosen.
    uint32_t flash_bg = ugfx_rgb(230, 190, 90);
    uint32_t flash_fg = UTHEME_WHITE;

    // TRACKED, not derived: see hover_token. (mx, my) still arrive so
    // the signature matches the other overlays.
    (void)mx; (void)my;

    ugfx_fill_rect(wm_surface(), L.x, L.y, L.w, L.h, bg);
    ugfx_fill_rect(wm_surface(), L.pane_x, L.y, L.pane_w, L.pane_h, pane_bg);
    ugfx_fill_rect(wm_surface(), L.x + L.side_w, L.y, 1, L.pane_h, UTHEME_SEPARATOR);
    ugfx_fill_rect(wm_surface(), L.x + 6,
                   L.y + SM_INSET + L.cats * L.item_h + L.sep_h / 2,
                   L.side_w - 12, 1, UTHEME_SEPARATOR);

    int apps = pane_visible(&L, 0);
    int total = L.cats + wm_system_action_count + apps + 2;
    int icon_sz = SM_ICON_SZ(L.item_h);
    for (int i = 0; i < total; i++) {
        const char *label; int kind, x, y, w, h, selected;
        if (!row_rect(&L, apps, i, &label, &kind, &x, &y, &w, &h, &selected)) break;
        int side = (kind == START_ROW_CATEGORY || kind == START_ROW_ACTION);
        uint32_t row_bg = side ? bg : pane_bg, row_fg = fg;

        if (kind == START_ROW_SEARCH) {
            // A sunken field with the query in it, always focused --
            // there is nothing else here that takes text, so a caret is
            // drawn unconditionally rather than following a focus that
            // cannot move.
            int fx = x + 4, fy = y + 4, fw = w - 8, fh = h - 8;
            ugfx_fill_rect(wm_surface(), fx, fy, fw, fh, UTHEME_WHITE);
            ugfx_draw_rect(wm_surface(), fx, fy, fw, fh, UTHEME_OUTLINE);
            int r = fh / 4;
            draw_magnifier(fx + 4 + r, fy + fh / 2 - 1, r, UTHEME_OUTLINE);
            int tx = fx + 8 + r * 3;
            int ty = fy + (fh - ugfx_char_h()) / 2;
            int tw = fw - (tx - fx) - 6;
            if (query_len) {
                ugfx_draw_string_clipped(wm_surface(), tx, ty, tw, query, fg, UTHEME_WHITE);
                int caret = tx + ugfx_text_width(query);
                if (caret < fx + fw - 3)
                    ugfx_fill_rect(wm_surface(), caret + 1, ty, 1, ugfx_char_h(), fg);
            } else {
                ugfx_draw_string_clipped(wm_surface(), tx, ty, tw, "Search apps",
                                         UTHEME_OUTLINE, UTHEME_WHITE);
                ugfx_fill_rect(wm_surface(), tx - 1, ty, 1, ugfx_char_h(), fg);
            }
            continue;
        }

        int hot = 0;
        if (kind == START_ROW_CATEGORY) hot = (hover_token == TOK_CAT(i));
        else if (kind == START_ROW_ACTION) hot = (hover_token == TOK_ACTION(i - L.cats));
        else hot = (hover_token == TOK_APP(i - L.cats - wm_system_action_count));

        if (i == flash_row) {
            row_bg = flash_bg; row_fg = flash_fg;
        } else if (selected) {
            // SELECTION OUTRANKS HOVER -- a pale wash with ordinary
            // text, Explorer's treatment, plus the accent bar below.
            row_bg = UTHEME_SELECTION;
        } else if (hot) {
            row_bg = side ? side_hover : pane_hover;
        }
        if (row_bg != (uint32_t)(side ? bg : pane_bg))
            ugfx_fill_rect(wm_surface(), x, y, w, h, row_bg);
        if (selected && kind == START_ROW_CATEGORY)
            ugfx_fill_rect(wm_surface(), x, y + 2, 3, h - 4, UTHEME_ACCENT);

        int text_x = x + 10;
        if (kind == START_ROW_APP) {
            // THE INDENT IS UNCONDITIONAL, the icon is not. A row whose
            // app has no artwork (Crash Test ships without any, on
            // purpose) still starts its label in the same column, so
            // the list reads straight rather than ragged.
            struct gui_app *a; int act;
            pane_row(scroll + i - L.cats - wm_system_action_count, &a, &act);
            text_x = x + 6 + icon_sz + 4;
            const struct uimg *ico = a ? icon_get(a->icon_name, icon_sz) : 0;
            if (ico)
                ugfx_blit_alpha(wm_surface(), x + 6, y + 3,
                                ico->w, ico->h, ico->px, ico->w);
        } else if (kind == START_ROW_CATEGORY) {
            // The folder's own icon, on the same terms: the column is
            // booked whether or not the file exists, so a theme missing
            // one folder's artwork does not move the other labels.
            int fsz = SM_FOLDER_SZ(L.item_h);
            text_x = x + 8 + fsz + 6;
            const struct uimg *ico = icon_get(folder_icon_at(i), fsz);
            if (ico)
                ugfx_blit_alpha(wm_surface(), x + 8, y + (h - ico->h) / 2,
                                ico->w, ico->h, ico->px, ico->w);
        }
        // Elided, for the same reason the strip is -- and a row label
        // that ran past its column used to be drawn through the border
        // before it was even clipped.
        ugfx_draw_string_elided(wm_surface(), text_x,
                                y + (L.item_h - ugfx_char_h()) / 2,
                                x + w - 8 - text_x, label, row_fg, row_bg);
    }

    // WHERE IN THE LIST THIS IS, when the list is taller than the pane.
    // An INDICATOR, not a scrollbar: there is no track to click and no
    // thumb to drag, because the pane is scrolled by the wheel and the
    // keyboard and adding a draggable control to an overlay the panel
    // hand-draws would be a second implementation of `uui_scrollbar`
    // (docs/decisions.md). It says where you are; it does not take
    // input, and nothing about it invites a click.
    int listed = pane_count();
    if (listed > L.pane_rows) {
        int track_h = L.pane_h - 2 * SM_INSET;
        int thumb_h = track_h * L.pane_rows / listed;
        if (thumb_h < 8) thumb_h = 8;
        int span = track_h - thumb_h;
        int max_first = listed - L.pane_rows;
        int thumb_y = L.y + SM_INSET + (max_first ? span * scroll / max_first : 0);
        int tx = L.pane_x + L.pane_w - 5;
        ugfx_fill_rect(wm_surface(), tx, L.y + SM_INSET, 3, track_h,
                       uui_state_bg(pane_bg, UUI_STATE_HOVER));
        ugfx_fill_rect(wm_surface(), tx, thumb_y, 3, thumb_h, UTHEME_OUTLINE);
    }

    // The description strip: a rule, then one line about the current
    // row. Drawn even when empty, because its HEIGHT is part of the
    // layout -- a menu that changed size as the pointer moved would be
    // worse than a blank line (see layout()).
    {
        int dy = L.y + L.pane_h;
        ugfx_fill_rect(wm_surface(), L.x, dy, L.w, L.desc_h, bg);
        ugfx_fill_rect(wm_surface(), L.x + 6, dy, L.w - 12, 1, UTHEME_SEPARATOR);
        const char *d = start_menu_description();
        if (d && d[0])
            // ELIDED, not clipped: a description that simply stopped at
            // the menu's edge read as a complete shorter sentence, and
            // the strip is the one place a person reads a whole line.
            // The full text is the tooltip's job (wm_tooltip.h).
            ugfx_draw_string_elided(wm_surface(), L.x + 10,
                                    dy + (L.desc_h - ugfx_char_h()) / 2,
                                    L.w - 20, d, UTHEME_OUTLINE, bg);
    }

    // Border last, after every row fill -- a hover/selection band spans
    // the full column width, the same columns the border's edges sit on,
    // so drawing the border first would get overpainted wherever a
    // highlighted row touches it.
    ugfx_draw_rect(wm_surface(), L.x, L.y, L.w, L.h, border);
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
        return 1;
    }
    if (n < L.cats + acts + pane_visible(&L, 0)) {
        struct gui_app *a; int act;
        pane_row(scroll + n - L.cats - acts, &a, &act);
        if (a) open_app(a);
        else if (act >= 0) wm_system_actions[act].on_select();
        return 1;
    }
    return 0;   // the search field: clicking it changes nothing, it is always focused
}

int start_menu_handle_click(int mx, int my) {
    if (!start_menu_open) return 0;

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
            if (activate(n)) flash(n);
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
