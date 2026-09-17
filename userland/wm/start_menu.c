// See start_menu.h.
#include "start_menu.h"
#include "wm_overlay.h"
#include "lib/icon_cache.h"
#include "wm_internal.h"
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

static int sel_cat = 0;   // the open folder, an index into the category list
static int sel_row = -1;  // the keyboard-highlighted app row, -1 for none

#define START_QUERY_MAX 24
static char query[START_QUERY_MAX];
static int query_len;

// Hover tokens. Opaque to wm_overlay.c, which only compares them, so
// all that matters is that no two controls share one.
#define TOK_CAT(i)    (1 + (i))
#define TOK_ACTION(i) (100 + (i))
#define TOK_APP(i)    (200 + (i))
#define TOK_SEARCH    999

// --- layout -----------------------------------------------------------

// The breathing room above the first row and below the last, which a
// menu whose rows sit flush against its border does not have.
#define SM_INSET 4
// The icon on an app row, and the column it reserves -- stated once, so
// the width the layout books and the pixels the draw puts there cannot
// drift apart.
#define SM_ICON_SZ(item_h)  ((item_h) - 6)
#define SM_ICON_COL(item_h) (SM_ICON_SZ(item_h) + 10)

struct sm_layout {
    int x, y, w, h;   // the whole popup
    int item_h;
    int side_w;       // the category column
    int pane_x, pane_w, pane_h, pane_rows; // the app column; rows it can SHOW
    int cats;         // folders in the sidebar
    int sep_h;        // the divider between folders and actions
    int search_h;
};

static void layout(struct sm_layout *L) {
    // A ROW IS TALLER THAN A MENU BAR'S, because this is a launcher a
    // pointer aims at rather than a list being scanned -- Breeze and
    // Windows 11 both give a launcher row about twice a text line's
    // leading. Font-derived, so it follows `font_size`.
    L->item_h = ugfx_char_h() + 10;
    L->cats = gui_app_cat_count(GUI_SHOW_STARTMENU);

    int side_label = 0;
    for (int i = 0; i < L->cats; i++) {
        int n = ugfx_text_width(gui_app_cat_label(GUI_SHOW_STARTMENU, i));
        if (n > side_label) side_label = n;
    }
    for (int i = 0; i < wm_system_action_count; i++) {
        int n = ugfx_text_width(wm_system_actions[i].label);
        if (n > side_label) side_label = n;
    }
    L->side_w = side_label + 20;

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
    int biggest = 0;
    for (int i = 0; i < L->cats; i++) {
        int n = gui_app_cat_size(GUI_SHOW_STARTMENU, gui_app_cat_key(GUI_SHOW_STARTMENU, i));
        if (n > biggest) biggest = n;
    }
    int content_h = side_h > biggest * L->item_h ? side_h : biggest * L->item_h;
    L->pane_h = content_h + 2 * SM_INSET;
    L->pane_rows = L->item_h ? content_h / L->item_h : 0;
    L->search_h = L->item_h + 8;

    L->w = L->side_w + 1 + L->pane_w;
    L->h = L->pane_h + L->search_h;
    L->x = 4;
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

void start_menu_damage(void) {
    struct sm_layout L;
    layout(&L);
    wm_damage_rect(L.x, L.y, L.w, L.h);
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

static const char *cur_cat_key(void) {
    return gui_app_cat_key(GUI_SHOW_STARTMENU, sel_cat);
}

// The n'th row of the app column: an app, or a system action matched by
// the query (typing "shut" finds Shutdown, as it does on KDE and
// Windows). Exactly one of *app / *action is set; *action is -1 for an
// app row. Returns 0 past the end.
static int pane_row(int n, struct gui_app **app, int *action) {
    if (app) *app = 0;
    if (action) *action = -1;
    if (n < 0) return 0;

    if (!query_len) {
        struct gui_app *a = gui_app_cat_at(GUI_SHOW_STARTMENU, cur_cat_key(), n);
        if (!a) return 0;
        if (app) *app = a;
        return 1;
    }

    int apps = gui_app_visible_count(GUI_SHOW_STARTMENU);
    for (int i = 0; i < apps; i++) {
        struct gui_app *a = gui_app_visible_at(GUI_SHOW_STARTMENU, i);
        if (!matches(a->name, query)) continue;
        if (n-- == 0) { if (app) *app = a; return 1; }
    }
    for (int i = 0; i < wm_system_action_count; i++) {
        if (!matches(wm_system_actions[i].label, query)) continue;
        if (n-- == 0) { if (action) *action = i; return 1; }
    }
    return 0;
}

// How many rows the column SHOWS -- capped at what fits, so the draw,
// the hit test, the keyboard and `gui menu --json` cannot disagree
// about which rows exist. A search with more matches than that shows
// the first of them; nothing scrolls here yet.
static int pane_count(void) {
    struct sm_layout L;
    layout(&L);
    int n = 0;
    while (n < L.pane_rows && pane_row(n, 0, 0)) n++;
    return n;
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
        lb = gui_app_cat_label(GUI_SHOW_STARTMENU, n);
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
        int i = n - L->cats - acts;
        struct gui_app *a; int act;
        pane_row(i, &a, &act);
        lb = a ? a->name : (act >= 0 ? wm_system_actions[act].label : "");
        k = START_ROW_APP;
        rx = L->pane_x; ry = L->y + SM_INSET + i * L->item_h;
        rw = L->pane_w; rh = L->item_h;
        sel = (i == sel_row);
    } else if (n == L->cats + acts + apps) {
        lb = "Search";
        k = START_ROW_SEARCH;
        rx = L->x; ry = L->y + L->pane_h; rw = L->w; rh = L->search_h;
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
    return L.cats + wm_system_action_count + pane_count() + 1;
}

int start_menu_row_info(int n, const char **label, int *kind,
                        int *x, int *y, int *w, int *h, int *selected) {
    struct sm_layout L;
    layout(&L);
    return row_rect(&L, pane_count(), n, label, kind, x, y, w, h, selected);
}

// The icon box on an app row, for a test that compares those pixels
// against the source artwork. Exported rather than re-derived: the
// offsets moved once and icons_test.py's own copy of them went on
// sampling the old place, which reads as a wrong icon.
int start_menu_row_icon(int n, int *x, int *y, int *sz) {
    struct sm_layout L;
    layout(&L);
    int apps = pane_count();
    int kind, rx, ry, rw, rh;
    if (!row_rect(&L, apps, n, 0, &kind, &rx, &ry, &rw, &rh, 0)) return 0;
    if (kind != START_ROW_APP) return 0;
    if (x) *x = rx + 6;
    if (y) *y = ry + 3;
    if (sz) *sz = SM_ICON_SZ(L.item_h);
    return 1;
}

const char *start_menu_query(void) { return query; }

const char *start_menu_category(void) {
    const char *l = gui_app_cat_label(GUI_SHOW_STARTMENU, sel_cat);
    return l ? l : "";
}

// Which row (mx, my) lands on, or -1 -- THE ONE COPY of that
// arithmetic, walked rather than divided, because the rows are no
// longer one uniform column.
static int row_at(int mx, int my) {
    if (!start_menu_open) return -1;
    struct sm_layout L;
    layout(&L);
    int apps = pane_count();
    int total = L.cats + wm_system_action_count + apps + 1;
    for (int i = 0; i < total; i++) {
        int x, y, w, h;
        if (!row_rect(&L, apps, i, 0, 0, &x, &y, &w, &h, 0)) break;
        if (uui_hit(x, y, w, h, mx, my)) return i;
    }
    return -1;
}

struct gui_app *start_menu_app_at(int mx, int my) {
    struct sm_layout L;
    layout(&L);
    int n = row_at(mx, my);
    if (n < L.cats + wm_system_action_count) return 0;
    struct gui_app *a = 0;
    pane_row(n - L.cats - wm_system_action_count, &a, 0);
    return a;
}

int start_menu_hover_at(int mx, int my) {
    if (!start_menu_open) { hover_token = 0; return 0; }
    struct sm_layout L;
    layout(&L);
    int n = row_at(mx, my);
    if (n < 0) { hover_token = 0; return 0; }
    int acts = wm_system_action_count;
    if (n < L.cats) hover_token = TOK_CAT(n);
    else if (n < L.cats + acts) hover_token = TOK_ACTION(n - L.cats);
    else if (n < L.cats + acts + pane_count()) hover_token = TOK_APP(n - L.cats - acts);
    else hover_token = TOK_SEARCH;
    return hover_token;
}

// --- open / close -----------------------------------------------------

void start_menu_open_now(void) {
    wm_overlay_close_others("start");
    start_menu_open = 1;
    flash_row = -1;
    hover_token = 0;
    sel_cat = 0;
    sel_row = -1;
    query[0] = '\0';
    query_len = 0;
    start_menu_damage();
}

void start_menu_close(void) {
    if (!start_menu_open) return;
    start_menu_open = 0;
    flash_row = -1;
    hover_token = 0;
    start_menu_damage(); // the rows it just vacated
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

    int apps = pane_count();
    int total = L.cats + wm_system_action_count + apps + 1;
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
            pane_row(i - L.cats - wm_system_action_count, &a, &act);
            text_x = x + 6 + icon_sz + 4;
            const struct uimg *ico = a ? icon_get(a->icon_name, icon_sz) : 0;
            if (ico)
                ugfx_blit_alpha(wm_surface(), x + 6, y + 3,
                                ico->w, ico->h, ico->px, ico->w);
        }
        // Clipped: a label longer than its column is wide would
        // otherwise be drawn through the border.
        ugfx_draw_string_clipped(wm_surface(), text_x,
                                 y + (L.item_h - ugfx_char_h()) / 2,
                                 x + w - 8 - text_x, label, row_fg, row_bg);
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
    if (n < L.cats + acts + pane_count()) {
        struct gui_app *a; int act;
        pane_row(n - L.cats - acts, &a, &act);
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
        start_menu_open = 0;
        start_menu_damage(); // the rows it just vacated
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
    start_menu_damage();
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
            int row = sel_row >= 0 ? sel_row : 0;
            int n = L.cats + wm_system_action_count + row;
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
        start_menu_damage();
        redraw_pending = 1;
        return 1;
    case KEY_ARROW_UP:
        if (rows > 0) sel_row = (sel_row <= 0) ? rows - 1 : sel_row - 1;
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
        flash_row = -1;
        start_menu_open = 0;
        hover_token = 0;
        start_menu_damage(); // the rows it just vacated
        redraw_pending = 1;
    }
}
