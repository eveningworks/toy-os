// File Manager -- Windows 11 Explorer's shape, with a commander inside.
//
// The window is a command bar, a breadcrumb and a search box, Places
// and Devices down the left, the listing, and a details pane; a second
// pane (Norton Commander's, with its F5/F6/F7/F8, Tab and Enter) is one
// View row away and keeps its whole keymap. docs/decisions/gui.md ("The
// file manager is a commander, and it opens as an Explorer") has why
// each part is the shape it is.
//
// THE FILE OPERATIONS RUN HERE, over lib/ufileop.h -- which /bin/cp,
// /bin/mv and /bin/rm are front ends over too, so there is still one
// implementation of what copying means and it is still testable as text
// at a prompt (tools/fileop_test.py). They used to be spawned children;
// what that could never do is report progress, be cancelled, or ask
// anything when a destination already exists.
//
// The panes are uui_fileview (ui/uui_fileview.h), which is where the
// listing, the ordering, ".." and descend-on-activate live -- shared
// with Image Viewer, Notepad's dialog and the WM's file picker, so this
// app contains no directory-reading code at all.
//
// THIS FILE IS THE APP: the menus, the commands, the input, main(). The
// rest of it is in userland/fm/, split by concern -- see fm_internal.h,
// which names what is where. It is not a library and the units are not
// modules; it is one event loop and one pile of state, filed so that
// finding a part of it is quick. `userland/wm/` is arranged the same
// way and for the same reason.
#include "fm/fm_internal.h"
#include <string.h>
#include <stdio.h>
#include <strings.h>  // strncasecmp -- the search filter
#include <stdlib.h>   // atoi -- the saved divider positions
#include "kpath.h"
#include "lib/uconf.h"
#include "lib/human.h"
#include "lib/udate.h"
#include "lib/uopen.h"
#include "lib/uclip.h"
#include "ui/uui.h"
#include "ui/utheme.h"
#include "ui/uui_dialog.h"
#include "lib/ufileop.h"
#include "ui/ulog.h"
#include "keyboard.h"

#define WIN_W 960
#define WIN_H 560

// Each pane's directory is remembered across runs in FILES_CONF -- the
// per-app `/etc/<app>.conf` convention, whose second user this was.

// The listings. 256 entries x 80 bytes = 20 KB per pane, which is why
// these are file-scope: a ring-3 frame is capped at 2 KiB
// (USERLAND_CFLAGS), and uui_fileview does not own its storage.
static struct sys_dirent g_left_entries[PANE_FILES];
static struct sys_dirent g_right_entries[PANE_FILES];

struct uui_fileview g_pane[2];
int g_active;            // 0 = left, 1 = right

// The two dividers. Both are persisted as a FRACTION rather than a
// pixel column (ui/uui_splitter.h), so a window opened wider than the
// one they were dragged in keeps the proportion instead of stranding a
// pane at its old width.
//
// The defaults reproduce what these were before they could move: a tree
// about eighteen columns wide, and two panes of equal width.
#define TREE_SPLIT_DEFAULT 160
#define PANE_SPLIT_DEFAULT (UUI_SPLIT_SCALE / 2)

struct uui_splitter g_tree_split;   // tree | panes
struct uui_splitter g_pane_split;   // left | right

// View options, all persisted in FILES_CONF. `g_single` shows only the
// ACTIVE pane (Tab still swaps which one that is, and F5/F6 still act
// toward the hidden one's directory -- the pane keeps existing, it just
// is not shown).
//
// **THE DEFAULT IS ONE PANE WITH THE TREE BESIDE IT** -- Explorer's
// shape, and Dolphin's and Nautilus's out of the box. The commander
// layout is still here and is one toolbar click away; what changed is
// which of the two a person who has never opened this app gets. Two
// panes is a power user's arrangement and reads as cluttered to
// everyone else, and the tree is how most people navigate.
//
// Both are PERSISTED, so this only decides a machine's first run --
// after that the file wins, which is why changing it is safe.
int g_single = 1;
// OFF by default now: the places column is how most people move around,
// and the tree is one View-menu row away -- Explorer's navigation pane
// with its folder tree collapsed.
int g_tree_on = 0;

struct uui_menubar g_ctx;   // the context menu and the drop-downs -- no bar of its own
struct uui_toolbar g_nav;
struct uui_pathbar g_path;
struct uui_textbox g_search;
int g_search_on;
static char g_search_hint[48];
struct uui_places g_places;
struct uui_toolbar g_viewbar;
int g_dpane = 1;
struct uui_button g_dp_open, g_dp_props;
struct uapp *g_app;         // for the widget callbacks, which carry none
struct uui_toolbar g_toolbar;
struct uui_statusbar g_status;

// CANCEL, and it exists only while an operation is running. Hidden the
// rest of the time rather than disabled: a permanently greyed button in
// the status bar is chrome that never does anything, and Explorer's
// stop button appears with the progress it stops.
struct uui_button g_cancel_btn;
struct uui_button g_toast_btn;   // over the undo toast's action slot (fm_view.c)
struct uui_button g_toast_x;     // ...and over its dismiss slot

// A secondary press ARMS; the release OPENS. Not a preference: the
// router delivers a release for every button, so a menu opened on the
// press would be handed that same gesture's release and commit whatever
// row landed under the cursor (uui_menubar.h says it once, in full).
static int g_ctx_armed;
static int g_ctx_x, g_ctx_y;

char g_stat_dir[PATH_MAX_LEN + 8];
char g_stat_items[48];
char g_stat_note[64];

// --- live refresh -------------------------------------------------------
//
// SYS_FS_GENERATION, the desktop's idiom: one integer compare per tick
// and no disk I/O. A copy finishing in another process shows up here
// without anyone pressing anything, which is the whole reason this app
// does not have to be told when its own child is done either.
unsigned long long g_seen_generation;
// Each pane's folder counter as of its last listing (pane_listed()).
static long long g_pane_gen[2];

// WHAT IS ON THE CLIPBOARD, cached. The menus ask per item on every
// draw and hit test (`item_flags` is a query, by design), and a syscall
// per item per frame to answer "is Paste greyed" would be absurd. Kept
// current by on_clipboard(), which is why the event exists.
static int g_clip_op = UCLIP_NONE;

// --- the conflict dialog ----------------------------------------------
//
// Raised when the worker finds a destination that exists. The rows
// point at these buffers, which is why they are file-scope and not on a
// frame: uui_dialog does not own its text (ui/uui_dialog.h).
struct uui_dialog g_dialog;
static char g_dlg_rows[3][PATH_MAX_LEN + 40];
static const char *g_dlg_row_ptr[3] = { g_dlg_rows[0], g_dlg_rows[1], g_dlg_rows[2] };
static char g_dlg_rename[PATH_MAX_LEN];

// APPLY-TO-ALL. Without it a paste of two hundred files asks two
// hundred times, which is the difference between a dialog and an
// obstacle. Remembered for one operation and cleared when it ends.
static int g_apply_all = -1;

static void answer_conflict(int code);   // defined with the dialog
static void answer_dialog(int code);
enum dialog_kind g_dialog_kind;

// The address bars (fm_internal.h says what they are). The text is
// re-synced from the pane on every layout while nobody is editing.
struct uui_textbox g_addr[2];
int g_addr_edit = -1;

struct uui_fileview *active(void)  { return &g_pane[g_active]; }
struct uui_fileview *other(void)   { return &g_pane[!g_active]; }

// THE COMMAND BAR'S DROP-DOWNS. There is no menu bar: Windows 11's
// Explorer took it away, and what it held is here, in the context menu,
// or on a key -- and every drop-down row names its key, so the F-keys
// are still discoverable.
static const struct uui_menu_item new_items[] = {
    UUI_MENU("Folder",         CMD_MKDIR,    "Ctrl+Shift+N"),
    UUI_MENU("Text file",      CMD_NEW_FILE, 0),
};

static const struct uui_menu_item sort_items[] = {
    UUI_MENU("Name",           CMD_SORT_NAME,     0),
    UUI_MENU("Date modified",  CMD_SORT_MODIFIED, 0),
    UUI_MENU("Type",           CMD_SORT_TYPE,     0),
    UUI_MENU("Size",           CMD_SORT_SIZE,     0),
    UUI_MENU_SEP,
    UUI_MENU("Ascending",      CMD_SORT_ASC,      0),
    UUI_MENU("Descending",     CMD_SORT_DESC,     0),
};

static const struct uui_menu_item view_items[] = {
    UUI_MENU("Large icons",    CMD_VIEW_LARGE,   0),
    UUI_MENU("Icons",          CMD_VIEW_ICONS,   0),
    UUI_MENU("Details",        CMD_VIEW_DETAILS, 0),
    UUI_MENU_SEP,
    UUI_MENU("Details pane",   CMD_VIEW_DPANE,   0),
    UUI_MENU("Second pane",    CMD_VIEW_PANES,   0),
    UUI_MENU("Folder tree",    CMD_VIEW_TREE,    0),
};

// "See more": what Explorer puts behind its "..." -- the rarer verbs.
static const struct uui_menu_item more_items[] = {
    UUI_MENU("Undo",               CMD_UNDO,       "Ctrl+Z"),
    UUI_MENU("Redo",               CMD_REDO,       "Ctrl+Y"),
    UUI_MENU_SEP,
    UUI_MENU("Select all",         CMD_SELECT_ALL, "Ctrl+A"),
    UUI_MENU("Refresh",            CMD_REFRESH,    "Ctrl+R"),
    UUI_MENU_SEP,
    UUI_MENU("Copy to other pane", CMD_COPY,       "F5"),
    UUI_MENU("Move to other pane", CMD_MOVE,       "F6"),
    UUI_MENU("Other pane",         CMD_SWAP,       "Tab"),
    UUI_MENU_SEP,
    UUI_MENU("Properties",         CMD_PROPERTIES, 0),
    UUI_MENU("Options...",         CMD_OPTIONS,    0),
    UUI_MENU_SEP,
    UUI_MENU("Close",              CMD_EXIT,       "Alt+F4"),
};

// COLOUR-CODED BY WHAT EACH COMMAND DOES (the File Manager design chosen
// 2026-10-01), through the theme's action roles: moving about and the
// clipboard, refresh and sort, new, rename, delete, the view mode.
#define TINT_NAV    UTHEME_ACT_NAV
#define TINT_TEAL   UTHEME_ACT_VIEW
#define TINT_GREEN  UTHEME_ACT_CREATE
#define TINT_VIOLET UTHEME_ACT_EDIT
#define TINT_RED    UTHEME_ACT_DANGER
#define TINT_ORANGE UTHEME_ACT_ARRANGE

// Back, Forward, Up and Refresh, beside the breadcrumb -- every browser's
// and every file manager's place for them.
static const struct uui_toolbar_item nav_items[] = {
    { "tb-back",    "Back (Alt+Left)",     CMD_BACK, 0, 0, 0, TINT_NAV },
    { "tb-forward", "Forward (Alt+Right)", CMD_FORWARD, 0, 0, 0, TINT_NAV },
    { "tb-up",      "Up (Backspace)",      CMD_UP, 0, 0, 0, TINT_NAV },
    { "tb-refresh", "Refresh (Ctrl+R)",    CMD_REFRESH, 0, 0, 0, TINT_TEAL },
};

// THE COMMAND BAR: the verbs with words where an icon alone would be a
// guess. Same codes and the same item_flags as the menus, so a latched
// button and a ticked menu row cannot disagree.
static const struct uui_toolbar_item toolbar_items[] = {
    { "tb-new",     "Create a folder or a file", CMD_MENU_NEW, "New", UUI_TB_MENU, 0, TINT_GREEN },
    UUI_TOOLBAR_SEP,
    { "tb-cut",     "Cut (Ctrl+X)",     CMD_CLIP_CUT,   0, 0, 0, TINT_NAV },
    { "tb-copy",    "Copy (Ctrl+C)",    CMD_CLIP_COPY,  0, 0, 0, TINT_NAV },
    { "tb-paste",   "Paste (Ctrl+V)",   CMD_CLIP_PASTE, 0, 0, 0, TINT_NAV },
    { "tb-rename",  "Rename (F2)",      CMD_RENAME,     0, 0, 0, TINT_VIOLET },
    { "tb-delete",  "Delete (Del)",     CMD_DELETE,     0, 0, 0, TINT_RED },
    UUI_TOOLBAR_SEP,
    { "tb-sort",    0,                  CMD_MENU_SORT,  "Sort", UUI_TB_MENU, 0, TINT_TEAL },
    { "tb-view",    0,                  CMD_MENU_VIEW,  "View", UUI_TB_MENU, 0, TINT_ORANGE },
    { "tb-more",    "See more",         CMD_MENU_MORE,  0, UUI_TB_MENU, 0, 0 },
    { "tb-pane",    "Show the details pane", CMD_VIEW_DPANE, "Details", UUI_TB_END, 0, TINT_NAV },
};

// THE RECYCLE BIN'S COMMAND BAR, swapped in while the active pane shows
// it (path_sync): its verbs are not the folder's -- nothing is created,
// pasted or renamed in a bin -- and Explorer's bin swaps its bar too.
static const struct uui_toolbar_item bin_toolbar_items[] = {
    { "tb-bin-restore", "Put it back where it was deleted from", CMD_RESTORE, "Restore", 0, 0, TINT_GREEN },
    { "tb-bin-restore", "Put everything back", CMD_RESTORE_ALL, "Restore all", 0, 0, TINT_GREEN },
    UUI_TOOLBAR_SEP,
    { "tb-delete",    "Delete for good (Del)", CMD_DELETE_FOREVER, "Delete permanently", 0, 0, TINT_RED },
    { "tb-bin-empty", "Delete everything here for good", CMD_EMPTY_BIN, "Empty Recycle Bin", 0, 0, TINT_RED },
    UUI_TOOLBAR_SEP,
    { "tb-sort",    0,                  CMD_MENU_SORT,  "Sort", UUI_TB_MENU, 0, TINT_TEAL },
    { "tb-view",    0,                  CMD_MENU_VIEW,  "View", UUI_TB_MENU, 0, TINT_ORANGE },
    { "tb-pane",    "Show the details pane", CMD_VIEW_DPANE, "Details", UUI_TB_END, 0, TINT_NAV },
};

// The status bar's view switch, at its right end, as in Explorer.
static const struct uui_toolbar_item viewbar_items[] = {
    { "tb-details", "Details", CMD_VIEW_DETAILS, 0, 0, 0, TINT_NAV },
    { "tb-icons",   "Icons",   CMD_VIEW_ICONS, 0, 0, 0, TINT_NAV },
};

// THE CONTEXT MENU, on a secondary click inside a pane. A separate
// uui_menubar with NO bar of its own -- see uui_menubar.h's context-menu
// note for why that is the arrangement rather than a second widget type.
// The order is Explorer's and Dolphin's: open first, the verbs, then
// Properties last with a separator before it.
static const struct uui_menu_item ctx_items[] = {
    UUI_MENU("Open",        CMD_OPEN,     "Enter"),
    UUI_MENU("Edit in Notepad", CMD_EDIT, 0),   // dropped for anything but a text file
    UUI_MENU_SEP,
    UUI_MENU("Cut",         CMD_CLIP_CUT,   "Ctrl+X"),
    UUI_MENU("Copy",        CMD_CLIP_COPY,  "Ctrl+C"),
    UUI_MENU("Paste",       CMD_CLIP_PASTE, "Ctrl+V"),
    UUI_MENU_SEP,
    UUI_MENU("Copy to other pane", CMD_COPY, "F5"),
    UUI_MENU("Move to other pane", CMD_MOVE, "F6"),
    UUI_MENU("Rename",      CMD_RENAME,   "F2"),
    UUI_MENU("Delete",      CMD_DELETE,   "F8"),
    UUI_MENU_SEP,
    UUI_MENU("New folder",  CMD_MKDIR,    "F7"),
    UUI_MENU_SEP,
    UUI_MENU("Properties",  CMD_PROPERTIES, 0),
};

// ...and inside the Recycle Bin: what a deleted item can have done to it.
static const struct uui_menu_item bin_ctx_items[] = {
    UUI_MENU("Restore",            CMD_RESTORE,        0),
    UUI_MENU_SEP,
    UUI_MENU("Copy",               CMD_CLIP_COPY,      "Ctrl+C"),
    UUI_MENU("Delete permanently", CMD_DELETE_FOREVER, "Del"),
    UUI_MENU_SEP,
    UUI_MENU("Properties",         CMD_PROPERTIES,     0),
};

// The context menu as OPENED: ctx_items minus the rows that do not
// apply to what was clicked. "Edit in Notepad" is absent -- not greyed
// -- for a folder or a binary, since a row that can never apply here
// is noise rather than a hint. Static because the popup keeps the
// pointer while it is up.
static struct uui_menu_item g_ctx_built[sizeof ctx_items / sizeof ctx_items[0]];
int g_ctx_rows;   // how many of them the open popup holds (fm_view.c logs it)

// Text or binary, git's rule: a NUL in the first bytes says binary. A
// short read (an empty file, a permission problem) counts as text --
// the item then opens Notepad on it, which is the safe wrong answer.
static int looks_like_text(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 1;
    unsigned char buf[512];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    for (size_t i = 0; i < n; i++)
        if (buf[i] == 0) return 0;
    return 1;
}

static int build_ctx_items(void) {
    int can_edit = 0;
    char path[PATH_MAX_LEN];
    if (uui_fileview_selected_path(active(), path, sizeof path) &&
        !uui_fileview_selected_is_dir(active()))
        can_edit = looks_like_text(path);
    int n = 0;
    if (in_bin(active())) {
        for (int i = 0; i < (int)(sizeof bin_ctx_items / sizeof bin_ctx_items[0]); i++)
            g_ctx_built[n++] = bin_ctx_items[i];
        return n;
    }
    for (int i = 0; i < (int)(sizeof ctx_items / sizeof ctx_items[0]); i++) {
        if (ctx_items[i].code == CMD_EDIT && !can_edit) continue;
        g_ctx_built[n++] = ctx_items[i];
    }
    return n;
}

static enum uui_fileview_sort sort_key_of(int code) {
    switch (code) {
    case CMD_SORT_MODIFIED: return UUI_FILEVIEW_SORT_MODIFIED;
    case CMD_SORT_TYPE:     return UUI_FILEVIEW_SORT_TYPE;
    case CMD_SORT_SIZE:     return UUI_FILEVIEW_SORT_SIZE;
    default:                return UUI_FILEVIEW_SORT_NAME;
    }
}

// dispatch-ok: ONE CASE PER COMMAND WHOSE ROW OR BUTTON HAS A STATE, all
// from this app's own CMD_* enum -- bounded by the menus it draws, and
// each answer reads different state, which a table would only hide.
// Details/Icons tick as a pair (the ACTIVE pane's current mode) and the
// two toggles tick when their thing is SHOWN -- so "Second pane" is
// checked in the default two-pane state, not when the option was used.
static unsigned menu_item_flags(int code) {
    unsigned bin;
    if (bin_item_flags(code, &bin)) return bin;   // the Recycle Bin's own rules
    if (search_item_flags(code, &bin)) return bin;   // ...and a search's
    if (code == CMD_UNDO || code == CMD_REDO)
        return undo_can(code == CMD_REDO) ? 0 : UUI_MI_DISABLED;
    switch (code) {
    case CMD_UP: {
        const char *d = uui_fileview_dir(&g_pane[g_active]);
        return (d[0] == '/' && d[1] == '\0') ? UUI_MI_DISABLED : 0;
    }
    // GREYED AT THE ENDS, which is how a person reads "this is as far
    // back as it goes" without pressing it and getting a status note.
    case CMD_BACK:
        return fm_history_can_back(g_active) ? 0 : UUI_MI_DISABLED;
    case CMD_FORWARD:
        return fm_history_can_forward(g_active) ? 0 : UUI_MI_DISABLED;
    // Both act on ONE row, so both are dead with nothing selected --
    // said by greying them rather than by a status-bar complaint after
    // the click, which is the difference between a menu that tells you
    // and one that scolds you.
    case CMD_OPEN:
    case CMD_PROPERTIES:
    case CMD_RENAME:
        return uui_fileview_selected_name(&g_pane[g_active]) ? 0 : UUI_MI_DISABLED;
    case CMD_CLIP_COPY:
    case CMD_CLIP_CUT:
    case CMD_DELETE:
    case CMD_DELETE_FOREVER:
        return operand_count() > 0 ? 0 : UUI_MI_DISABLED;
    case CMD_CLIP_PASTE:
        // Greyed when there is nothing to paste, which is what tells a
        // user the Ctrl+C in the other window did not take.
        return g_clip_op != UCLIP_NONE ? 0 : UUI_MI_DISABLED;
    case CMD_VIEW_DETAILS:
        return g_pane[g_active].mode == UUI_FILEVIEW_DETAILS ? UUI_MI_CHECKED : 0;
    case CMD_VIEW_ICONS:
        return g_pane[g_active].mode == UUI_FILEVIEW_ICONS && !g_pane[g_active].icon_px
               ? UUI_MI_CHECKED : 0;
    case CMD_VIEW_LARGE:
        return g_pane[g_active].mode == UUI_FILEVIEW_ICONS && g_pane[g_active].icon_px
               ? UUI_MI_CHECKED : 0;
    case CMD_VIEW_DPANE:
        return g_dpane ? UUI_MI_CHECKED : 0;
    case CMD_SORT_NAME: case CMD_SORT_MODIFIED: case CMD_SORT_TYPE: case CMD_SORT_SIZE:
        return uui_fileview_sort(active(), 0) == sort_key_of(code) ? UUI_MI_CHECKED : 0;
    case CMD_SORT_ASC: case CMD_SORT_DESC: {
        int dir;
        uui_fileview_sort(active(), &dir);
        return (dir > 0) == (code == CMD_SORT_ASC) ? UUI_MI_CHECKED : 0;
    }
    case CMD_VIEW_PANES:
        return g_single ? 0 : UUI_MI_CHECKED;
    case CMD_VIEW_TREE:
        return g_tree_on ? UUI_MI_CHECKED : 0;
    default:
        return 0;
    }
}

// THE MENU BAR IS ROUTED, not hand-dispatched: this app has routed
// widgets, and the router runs before on_press -- hand-routing the
// popup made a click on a View item ALSO select the folder-tree row
// under it (CLAUDE.md's exact rule; the tree made it visible).
struct uui_item g_widgets[] = {
    { .ops = &uui_toolbar_ops, .widget = &g_nav, .id = ID_NAV, .name = "nav" },
    { .ops = &uui_pathbar_ops, .widget = &g_path, .id = ID_PATH, .name = "path" },
    { .ops = &uui_textbox_ops, .widget = &g_search, .id = ID_SEARCH, .name = "search" },
    { .ops = &uui_toolbar_ops, .widget = &g_toolbar, .id = ID_TOOLBAR, .name = "toolbar" },
    { .ops = &uui_places_ops, .widget = &g_places, .id = ID_PLACES, .name = "places" },
    { .ops = &uui_toolbar_ops, .widget = &g_viewbar, .id = ID_VIEWBAR, .name = "viewbar" },
    { .ops = &uui_button_ops, .widget = &g_dp_open, .id = ID_DP_OPEN, .name = "dpopen", .hidden = 1 },
    { .ops = &uui_button_ops, .widget = &g_dp_props, .id = ID_DP_PROPS, .name = "dpprops", .hidden = 1 },
    { .ops = &uui_fileview_ops, .widget = &g_pane[0], .id = ID_LEFT, .name = "left" },
    { .ops = &uui_fileview_ops, .widget = &g_pane[1], .id = ID_RIGHT, .name = "right" },
    { .ops = &uui_tree_ops, .widget = &g_tree, .id = ID_TREE, .name = "tree" },
    { .ops = &uui_splitter_ops, .widget = &g_tree_split, .id = ID_TREE_SPLIT, .name = "treesplit" },
    { .ops = &uui_splitter_ops, .widget = &g_pane_split, .id = ID_PANE_SPLIT, .name = "panesplit" },
    { .ops = &uui_button_ops, .widget = &g_cancel_btn, .id = ID_CANCEL, .hidden = 1 },
    { .ops = &uui_button_ops, .widget = &g_toast_btn, .id = ID_TOAST, .hidden = 1 },
    { .ops = &uui_button_ops, .widget = &g_toast_x, .id = ID_TOAST_X, .hidden = 1 },
    { .ops = &uui_segmented_ops, .widget = &g_scope_seg, .id = ID_SCOPE, .hidden = 1, .name = "scope" },
    { .ops = &uui_button_ops, .widget = &g_search_stop, .id = ID_SEARCH_STOP, .hidden = 1 },
    { .ops = &uui_textbox_ops, .widget = &g_addr[0], .id = ID_ADDR_L, .name = "addr0" },
    { .ops = &uui_textbox_ops, .widget = &g_addr[1], .id = ID_ADDR_R, .name = "addr1" },
    // LAST, so it is hit-tested FIRST: input order is the reverse of
    // draw order, and its popup covers whatever is under it.
    { .ops = &uui_menubar_ops, .widget = &g_ctx, .id = ID_CTX, .name = "ctxmenu" },  // not "ctx": the app's own `ctx <open>` line keeps that key
    // LAST OF ALL: a modal has to be offered every press before
    // anything else, and it draws over everything.
    { .ops = &uui_dialog_ops, .widget = &g_dialog, .id = ID_DIALOG },
};
const int g_widget_count = (int)(sizeof g_widgets / sizeof g_widgets[0]);

// A widget by its ID. Linear over a dozen entries, called a handful of
// times per layout -- and it cannot go stale when the array grows,
// which the position-derived indices it replaced could and did.
struct uui_item *widget_by_id(int id) {
    for (int i = 0; i < g_widget_count; i++)
        if (g_widgets[i].id == id) return &g_widgets[i];
    // Never NULL for a compiled-in id; returning slot 0 rather than
    // faulting keeps a typo a visual bug instead of a crash.
    return &g_widgets[0];
}

void set_note(const char *s) { strlcpy(g_stat_note, s, sizeof g_stat_note); }

// --- the address bars ---------------------------------------------------

void addr_begin_edit(int pane) {
    if (g_addr_edit >= 0 && g_addr_edit != pane) addr_end_edit(0);
    g_active = pane;
    g_addr_edit = pane;
    uui_textbox_init(&g_addr[pane], uui_fileview_dir(&g_pane[pane]));
    uui_textbox_set_active(&g_addr[pane], 1);
    uui_textbox_key(&g_addr[pane], 0x01);   // Ctrl-A: typing replaces the path
    refresh_status();
}

// `commit` navigates the pane to what was typed -- resolved against the
// pane's own directory, so "sub" and "../etc" both mean what a shell
// would take them to mean. A directory that does not exist is said so
// and the field stays up, holding what was typed, for a second try.
void addr_end_edit(int commit) {
    int pane = g_addr_edit;
    if (pane < 0) return;
    if (commit) {
        char path[PATH_MAX_LEN], was[PATH_MAX_LEN];
        strlcpy(was, uui_fileview_dir(&g_pane[pane]), sizeof was);
        const char *typed = uui_textbox_text(&g_addr[pane]);
        // The scratch is the caller's now (kpath.h) and is twice a
        // path; static rather than a local because a GUI client's
        // event loop is one thread and this does not recurse.
        static char scratch[KPATH_SCRATCH_FOR(PATH_MAX_LEN)];
        struct kpath_scratch sc = { scratch, sizeof scratch };
        if (!k_path_resolve(was, typed, path, sizeof path, &sc)) {
            set_note("path too long");
            return;
        }
        if (!fm_goto(pane, path)) {
            // The failed listing left the pane EMPTY (ui/uui_fileview.h);
            // put the directory it had back rather than show a hole.
            // DELIBERATELY NOT fm_goto(): this is undoing a navigation
            // that did not happen, and recording it would put the place
            // you already were into the history a second time, where
            // Back would then have to be pressed twice to leave it.
            uui_fileview_set_dir(&g_pane[pane], was);
            snprintf(g_stat_note, sizeof g_stat_note, "no such directory: %s",
                      k_path_basename(path));
            return;
        }
    }
    g_addr_edit = -1;
    uui_textbox_set_active(&g_addr[pane], 0);
    refresh_status();
}

// A divider's position, written when the drag ENDS rather than per
// motion: a drag is hundreds of events and every one of them would be a
// whole-file rewrite.
void save_split(const char *key, const struct uui_splitter *sp) {
    char v[12];
    snprintf(v, sizeof v, "%d", uui_splitter_frac(sp));
    uconf_set(FILES_CONF, key, v);
}

// --- status -----------------------------------------------------------

void refresh_status(void) {
    // WHILE A JOB RUNS the note is the job -- "Copy 3/7  notes.txt  62%"
    // -- which is the thing a spawned child could never report, and the
    // reason the operation moved in here.
    if (fm_job_running()) {
        fm_job_status(g_stat_note, sizeof g_stat_note);
    }
    path_sync();

    // Explorer's two facts: how many things are here, and what is
    // chosen -- with its size, since that is what a copy will cost.
    struct uui_fileview *fv = active();
    int n = uui_fileview_count(fv);
    snprintf(g_stat_dir, sizeof g_stat_dir, "%d item%s%s", n, n == 1 ? "" : "s",
             uui_fileview_truncated(fv) ? " (more)" : "");
    char size[24];
    int marks = uui_fileview_mark_count(fv);
    const struct sys_dirent *e = uui_fileview_selected_entry(fv);
    if (marks > 1) {
        unsigned long long bytes = 0;
        char path[PATH_MAX_LEN];
        for (int i = 0; i < marks; i++) {
            struct sys_stat st;
            if (uui_fileview_marked_path(fv, i, path, sizeof path) &&
                !uui_fileview_marked_is_dir(fv, i) && sys_stat(path, &st) == 0)
                bytes += st.size;
        }
        human_size(size, sizeof size, bytes);
        snprintf(g_stat_items, sizeof g_stat_items, "%d items selected, %s", marks, size);
    } else if (e && !e->is_dir) {
        human_size(size, sizeof size, e->size);
        snprintf(g_stat_items, sizeof g_stat_items, "1 item selected, %s", size);
    } else if (e) {
        snprintf(g_stat_items, sizeof g_stat_items, "1 item selected");
    } else {
        g_stat_items[0] = '\0';
    }
}

// --- the breadcrumb, the search box and the places ------------------------

// ONE SEARCH QUERY PER PANE, snapshotted as it is typed: the filter is
// re-run on every reload, and reading the shared box from it let a search
// typed for one pane re-filter the other the next time the disk changed.
static char g_query[2][UUI_TEXTBOX_MAX];
// THE SEARCH STRIP (fm_search.c's results): Look in this folder, its
// subfolders, or the whole computer; a Stop while the walk runs. Up
// whenever the active pane has a query or shows results.
struct uui_segmented g_scope_seg;
struct uui_button g_search_stop;
static const char *const g_scope_names[] = { "This folder", "Subfolders too", "Whole computer" };
static char g_scope[2][PATH_MAX_LEN];   // the real folder a pane's search is about
static int g_keep_query;                // the next navigation keeps the query (a scope switch)

int search_strip_shown(void) {
    return g_query[g_active][0] || in_search(active());
}

// A pane's virtual folders: the Recycle Bin, and a search's results.
static const struct uui_fileview_source *resolve_virtual(void *ctx, const char *dir) {
    (void)ctx;
    if (strcmp(dir, FM_BIN) == 0) return bin_source();
    if (search_scope(dir)) return search_source();
    return 0;
}

// Search below `scope` for the active pane's query: the results become
// the pane's folder ("search:" + scope), and on_pane_dir starts the walk.
static void search_into(const char *scope) {
    if (!g_query[g_active][0]) { set_note("type what to look for, then Enter"); return; }
    char dir[PATH_MAX_LEN + 8];
    snprintf(dir, sizeof dir, "%s%s", FM_SEARCH, scope);
    strlcpy(g_scope[g_active], scope, sizeof g_scope[g_active]);
    g_keep_query = 1;
    if (!strcmp(uui_fileview_dir(active()), dir)) {   // already there: search again
        search_start(scope, g_query[g_active]);
        reload_pane(active());
    } else {
        fm_goto(g_active, dir);
    }
    g_keep_query = 0;
}

// The strip's switch, chosen: back to filtering the folder, or into its
// subfolders, or everything from the root.
static void scope_chosen(int sel) {
    const char *real = search_scope(uui_fileview_dir(active()));
    const char *scope = g_scope[g_active][0] ? g_scope[g_active] : (real ? real : uui_fileview_dir(active()));
    if (sel == 0) {
        search_stop();
        g_keep_query = 1;
        fm_goto(g_active, scope);
        g_keep_query = 0;
        reload_pane(active());   // the filter applies again
    } else {
        search_into(sel == 2 ? "/" : scope);
        if (sel == 2) strlcpy(g_scope[g_active], scope, sizeof g_scope[g_active]);
    }
}

// The breadcrumb and the places follow the ACTIVE pane; the search box's
// hint names the folder it would search, as Explorer's does.
void path_sync(void) {
    const char *dir = uui_fileview_dir(active());
    const struct uui_toolbar_item *want = in_bin(active()) ? bin_toolbar_items : toolbar_items;
    if (g_toolbar.items != want) {
        g_toolbar.items = want;
        g_toolbar.count = want == bin_toolbar_items
                        ? (int)(sizeof bin_toolbar_items / sizeof bin_toolbar_items[0])
                        : (int)(sizeof toolbar_items / sizeof toolbar_items[0]);
        g_toolbar.hot = g_toolbar.armed = -1;
    }
    if (!uui_pathbar_is_editing(&g_path) && strcmp(g_path.path, dir) != 0)
        uui_pathbar_set_path(&g_path, dir);
    uui_places_select_path(&g_places, dir);
    // With the folder tree off the pane is flat, so a place or a volume
    // follows the directory here; with it on, a navigation reveals it
    // (on_pane_dir), and doing it here too would reopen a branch the
    // user collapsed.
    if (!g_tree_on) tree_select_path(dir);
    if (!g_search_on && strcmp(uui_textbox_text(&g_search), g_query[g_active]) != 0)
        uui_textbox_set_text(&g_search, g_query[g_active]);
    const char *sc = search_scope(dir);
    g_scope_seg.selected = !sc ? 0 : !strcmp(sc, "/") ? 2 : 1;
    const char *base = in_bin(active()) ? "Recycle Bin"
                     : (dir[0] == '/' && !dir[1]) ? "System" : k_path_basename(dir);
    snprintf(g_search_hint, sizeof g_search_hint, "Search %s", base);
}

// EACH PANE'S ONE FILTER: hidden names (Options), then the search --
// a filter on the folder you are in, by name, Dolphin's filter bar
// rather than Explorer's recursive search, which would walk the disk on
// every keystroke. Case-insensitive, anywhere in the name.
static int pane_filter(void *ctx, const char *dir, const struct sys_dirent *e) {
    (void)dir;
    if (!g_opt.hidden && e->name[0] == '.') return 0;
    const char *q = g_query[(int)(intptr_t)ctx];
    int qn = (int)strlen(q), n = (int)strlen(e->name);
    for (int i = 0; i + qn <= n; i++)
        if (!strncasecmp(e->name + i, q, (size_t)qn)) return 1;
    return 0;
}

void search_apply(void) {
    strlcpy(g_query[g_active], uui_textbox_text(&g_search), sizeof g_query[g_active]);
    reload_pane(active());
    ulogf("files: search \"%s\" rows %d\n", uui_textbox_text(&g_search),
          uui_fileview_count(active()));
}

void search_clear(void) {
    uui_textbox_set_text(&g_search, "");
    g_search_on = 0;
    uui_textbox_set_active(&g_search, 0);
    for (int i = 0; i < 2; i++)
        if (g_query[i][0]) {
            g_query[i][0] = '\0';
            reload_pane(&g_pane[i]);
        }
}

// A path from the breadcrumb: a segment's (absolute) or one typed there
// (resolved against the pane, so "sub" and "../etc" mean what a shell
// takes them to mean). A place that does not exist is said so, and the
// field stays up holding what was typed.
static void path_navigate(const char *typed) {
    char path[PATH_MAX_LEN], was[PATH_MAX_LEN];
    strlcpy(was, uui_fileview_dir(active()), sizeof was);
    static char scratch[KPATH_SCRATCH_FOR(PATH_MAX_LEN)];
    struct kpath_scratch sc = { scratch, sizeof scratch };
    if (strcmp(typed, FM_BIN) == 0) strlcpy(path, typed, sizeof path);   // not a relative path
    else if (!k_path_resolve(was, typed, path, sizeof path, &sc)) { set_note("path too long"); return; }
    if (!fm_goto(g_active, path)) {
        uui_fileview_set_dir(active(), was);   // see addr_end_edit on why not fm_goto
        snprintf(g_stat_note, sizeof g_stat_note, "no such folder: %s", k_path_basename(path));
        return;
    }
    uui_pathbar_set_path(&g_path, uui_fileview_dir(active()));
}

// A drop-down, under the command-bar button that asked for it.
static void open_dropdown(int code) {
    const struct uui_menu_item *items = 0;
    int n = 0;
    switch (code) {
    case CMD_MENU_NEW:  items = new_items;  n = (int)(sizeof new_items / sizeof new_items[0]); break;
    case CMD_MENU_SORT: items = sort_items; n = (int)(sizeof sort_items / sizeof sort_items[0]); break;
    case CMD_MENU_VIEW: items = view_items; n = (int)(sizeof view_items / sizeof view_items[0]); break;
    default:            items = more_items; n = (int)(sizeof more_items / sizeof more_items[0]); break;
    }
    int x = 0, y = 0, w = 0, h = 0;
    for (int i = 0; i < g_toolbar.count; i++)
        if (g_toolbar.items[i].code == code) uui_toolbar_item_rect(&g_toolbar, i, &x, &y, &w, &h);
    g_ctx_rows = n;
    uui_menubar_open_at(&g_ctx, items, n, x, y + h);
}

// NEW, IN PLACE: the item is created under a free name and its name is
// then edited where it stands -- Explorer's New folder, Dolphin's too.
static void create_and_rename(int cmd) {
    char name[PATH_MAX_LEN], path[PATH_MAX_LEN];
    const char *base = cmd == CMD_MKDIR ? "New folder" : "New text file.txt";
    struct sys_stat st;
    strlcpy(name, base, sizeof name);
    if (k_path_join(uui_fileview_dir(active()), base, path, sizeof path) &&
        sys_stat(path, &st) == 0 &&
        !ufileop_unique_name(uui_fileview_dir(active()), base, name, sizeof name)) {
        set_note("no free name");
        return;
    }
    if (cmd == CMD_MKDIR) commit_mkdir(name);
    else commit_newfile(name);
    if (uui_fileview_select_name(active(), name)) uui_fileview_begin_rename(active());
}

// A rename the pane finished (Enter, or a click elsewhere): do it.
static void rename_poll(void) {
    char from[PATH_MAX_LEN], to[PATH_MAX_LEN];
    for (int i = 0; i < 2; i++)
        if (uui_fileview_take_rename(&g_pane[i], from, to, sizeof from)) {
            int was = g_active;
            g_active = i;
            commit_rename_named(from, to);
            g_active = was;
        }
}

static void set_view(int code) {
    struct uui_fileview *fv = active();
    fv->icon_px = code == CMD_VIEW_LARGE ? ugfx_char_h() * 6 : 0;
    uui_fileview_set_mode(fv, code == CMD_VIEW_DETAILS ? UUI_FILEVIEW_DETAILS
                                                        : UUI_FILEVIEW_ICONS);
    uconf_set(FILES_CONF, g_active ? "right_view" : "left_view",
              code == CMD_VIEW_DETAILS ? "details" : code == CMD_VIEW_LARGE ? "large" : "icons");
}

void do_command(struct uapp *a, int code) {
    // dispatch-ok: THE SET IS THE APP'S OWN MENUS and nothing else can
    // extend it -- every branch is one entry of the CMD_* enum in
    // fm_internal.h, which exists so the menu bar, the toolbar, the
    // context menu and the function keys cannot disagree about what a
    // command does. That is the opposite of the case this check is for:
    // syscall_table.c is a table because its entries are UNIFORM and the
    // set is open-ended. These bodies are not uniform -- some are one
    // call, some take a selection and bail, some spawn a process -- so a
    // {code, handler} table would mean 21 one-line functions each needing
    // the uapp and the locals this one already has, which is more code
    // saying less. It crossed 20 branches when Back and Forward were
    // added on 2026-09-19.
    // A command the bin greys is refused here too: a key reaches this
    // without asking menu_item_flags().
    if ((in_bin(active()) || in_search(active())) && (menu_item_flags(code) & UUI_MI_DISABLED) &&
        code != CMD_UP) {
        set_note(in_bin(active()) ? "not in the Recycle Bin" : "not in search results");
        return;
    }
    switch (code) {
    case CMD_COPY:   do_copy(); break;
    case CMD_MOVE:   do_move(); break;
    case CMD_DELETE:         do_delete(0); break;
    case CMD_DELETE_FOREVER: do_delete(1); break;
    case CMD_RESTORE:        bin_restore(0); break;
    case CMD_RESTORE_ALL:    bin_restore(1); break;
    case CMD_EMPTY_BIN:      do_empty_bin(); break;
    case CMD_UNDO:           do_undo(0); break;
    case CMD_REDO:           do_undo(1); break;
    case CMD_MKDIR:
        if (g_opt.rename_dialog) open_prompt(CMD_MKDIR, "New folder", "New folder");
        else create_and_rename(CMD_MKDIR);
        break;
    case CMD_RENAME: {
        const char *name = uui_fileview_selected_name(active());
        if (!name) { set_note("nothing selected"); break; }
        // Options: the name edited where it is (Explorer's F2), or asked
        // for in a dialog.
        if (g_opt.rename_dialog) open_prompt(CMD_RENAME, "Rename", name);
        else uui_fileview_begin_rename(active());
        break;
    }
    case CMD_OPTIONS:
        options_open(a);
        break;
    case CMD_BACK:
        if (fm_history_back(g_active) == 0) set_note("nothing to go back to");
        break;
    case CMD_FORWARD:
        if (fm_history_forward(g_active) == 0) set_note("nothing to go forward to");
        break;
    case CMD_UP:
        fm_goto_up(g_active);
        break;
    case CMD_CLIP_COPY:  clip_copy();  break;
    case CMD_CLIP_CUT:   clip_cut();   break;
    case CMD_CLIP_PASTE: clip_paste(); break;
    case CMD_OPEN:
        // The same act as Enter or a double click, so a directory
        // descends and a file goes to whatever /etc/mimeapps.conf and
        // the Handles= declarations resolve to (lib/uopen.h).
        if (!fm_goto_activate(g_active)) set_note("nothing selected");
        break;
    case CMD_EDIT: {
        // A launch, like on_pane_open(): not waited for.
        char path[PATH_MAX_LEN];
        if (!uui_fileview_selected_path(active(), path, sizeof path)) {
            set_note("nothing selected");
            break;
        }
        if (uapp_spawn(a, NOTEPAD_EXEC, path) < 0) {
            set_note("could not start Notepad");
            ulogf("files: edit %s -- spawn %s FAILED\n", path, NOTEPAD_EXEC);
        } else {
            snprintf(g_stat_note, sizeof g_stat_note, "editing %s", k_path_basename(path));
        }
        break;
    }
    case CMD_PROPERTIES: {
        char path[PATH_MAX_LEN];
        if (!uui_fileview_selected_path(active(), path, sizeof path)) {
            set_note("nothing selected");
            break;
        }
        // A WINDOW OF ITS OWN, spawned. It walks a directory tree to
        // total it up, which is work no event loop should be doing --
        // and a Properties window you can leave open beside the listing
        // is what Explorer and Dolphin both give you.
        if (uapp_spawn(a, PROPERTIES_EXEC, path) < 0) {
            set_note("could not open Properties");
            ulogf("files: spawn %s FAILED\n", PROPERTIES_EXEC);
        }
        break;
    }
    case CMD_REFRESH:
        reload_panes();
        set_note("refreshed");
        break;
    case CMD_SWAP:
        g_active = !g_active;
        break;
    case CMD_VIEW_DETAILS:
    case CMD_VIEW_ICONS:
    case CMD_VIEW_LARGE:
        // The ACTIVE pane's, not the window's: two panes with two modes
        // is normal in every commander that grew a thumbnail view.
        set_view(code);
        break;
    case CMD_VIEW_DPANE:
        g_dpane = !g_dpane;
        uconf_set(FILES_CONF, "details_pane", g_dpane ? "1" : "0");
        break;
    case CMD_MENU_NEW: case CMD_MENU_SORT: case CMD_MENU_VIEW: case CMD_MENU_MORE:
        open_dropdown(code);
        break;
    case CMD_NEW_FILE:
        if (g_opt.rename_dialog) open_prompt(CMD_NEW_FILE, "New text file", "New text file.txt");
        else create_and_rename(CMD_NEW_FILE);
        break;
    case CMD_SORT_NAME: case CMD_SORT_MODIFIED: case CMD_SORT_TYPE: case CMD_SORT_SIZE: {
        int dir;
        uui_fileview_sort(active(), &dir);
        uui_fileview_set_sort(active(), sort_key_of(code), dir);
        break;
    }
    case CMD_SORT_ASC: case CMD_SORT_DESC:
        uui_fileview_set_sort(active(), uui_fileview_sort(active(), 0),
                              code == CMD_SORT_ASC ? 1 : -1);
        break;
    case CMD_SELECT_ALL: {
        struct uui_fileview *fv = active();
        for (int r = 0; r < uui_fileview_row_count(fv); r++)
            if (!uui_fileview_is_marked(fv, r)) uui_fileview_toggle_mark(fv, r);
        break;
    }
    case CMD_VIEW_PANES:
        g_single = !g_single;
        uconf_set(FILES_CONF, "panes", g_single ? "1" : "2");
        break;
    case CMD_VIEW_TREE:
        g_tree_on = !g_tree_on;
        if (g_tree_on) tree_reveal_path(uui_fileview_dir(active()));
        else { tree_rebuild(); tree_select_path(uui_fileview_dir(active())); }
        uconf_set(FILES_CONF, "tree", g_tree_on ? "1" : "0");
        break;
    case CMD_EXIT:
        uapp_quit(a, 0);
        return;
    default:
        return;
    }
    refresh_status();
    uapp_redraw(a);
}

// --- input --------------------------------------------------------------

// Double-click tracking for the folder tree. The window is the
// fileview's, not a second constant: two double-click speeds in one
// window is a thing a user feels and cannot name.
// A LONGER WINDOW THAN THE FILEVIEW'S, because the first click of the
// pair does real work -- it navigates a pane, which lists a directory
// and relayouts the window -- and the second click is not looked at
// until that frame is done. Measured at 60-90 ticks in the emulator
// against the fileview's 30, where both clicks are cheap. ~900 ms is
// also Windows' own maximum double-click time, so it is not out of
// band for a person either.
#define TREE_DOUBLE_CLICK_TICKS 90
static int g_tree_click_id = -1;
static uint64_t g_tree_click_tick = 0;
static int g_tree_click_collapsed = 0;

static void on_widget(struct uapp *a, int id, int reason) {
    // The modal owns the window: a routed widget can still be clicked
    // under it, and acting on that could open a second modal over the
    // first. The menu's parked code is TAKEN so it cannot replay later.
    char taken[PATH_MAX_LEN];
    // The dialog IS the modal when it asks for a name, so it answers
    // before the gate below -- behind it, Esc and both buttons were
    // parked and never taken, and the window stayed modal for good.
    if (id == ID_DIALOG) {
        int code = uui_dialog_take_code(&g_dialog);
        if (code > 0) answer_dialog(code);
        uapp_redraw(a);
        return;
    }
    if (g_modal != MODAL_NONE) {
        if (id == ID_CTX) (void)uui_menubar_take_code(&g_ctx);
        if (id == ID_TOOLBAR) (void)uui_toolbar_take_code(&g_toolbar);
        if (id == ID_NAV) (void)uui_toolbar_take_code(&g_nav);
        if (id == ID_VIEWBAR) (void)uui_toolbar_take_code(&g_viewbar);
        if (id == ID_PATH) (void)uui_pathbar_take(&g_path, taken, sizeof taken);
        if (id == ID_PLACES) (void)uui_places_take(&g_places, taken, sizeof taken);
        return;
    }
    if (id == ID_TOOLBAR || id == ID_NAV || id == ID_VIEWBAR) {
        // The commit is PARKED in the widget (ui/uui_toolbar.h): the
        // ops release slot can only say "changed", not which item.
        struct uui_toolbar *tb = id == ID_NAV ? &g_nav : id == ID_VIEWBAR ? &g_viewbar : &g_toolbar;
        int code = uui_toolbar_take_code(tb);
        if (code >= 0) do_command(a, code);
        return;
    }
    if (id == ID_PATH) {
        if (uui_pathbar_take(&g_path, taken, sizeof taken)) path_navigate(taken);
        refresh_status();
        uapp_redraw(a);
        return;
    }
    if (id == ID_PLACES) {
        if (uui_places_take(&g_places, taken, sizeof taken) &&
            strcmp(taken, uui_fileview_dir(active())) != 0 && !fm_goto(g_active, taken))
            snprintf(g_stat_note, sizeof g_stat_note, "cannot open %s", taken);
        refresh_status();
        uapp_redraw(a);
        return;
    }
    if (id == ID_SCOPE) {   // the Look in switch moved
        scope_chosen(g_scope_seg.selected);
        refresh_status();
        uapp_redraw(a);
        return;
    }
    if (id == ID_SEARCH) {
        if (reason == UUI_REASON_PRESS) {
            g_search_on = 1;
            uui_textbox_set_active(&g_search, 1);
        }
        uapp_redraw(a);
        return;
    }
    if (id == ID_ADDR_L || id == ID_ADDR_R) {
        // A click in a path strip starts editing it (and makes that
        // pane the active one, as clicking anywhere in a pane does).
        if (reason == UUI_REASON_PRESS) addr_begin_edit(id == ID_ADDR_R);
        uapp_redraw(a);
        return;
    }
    if (id == ID_CTX) {
        int code = uui_menubar_take_code(&g_ctx);
        if (code >= 0) do_command(a, code);
        uapp_redraw(a);
        return;
    }
    if (id == ID_TREE_SPLIT || id == ID_PANE_SPLIT) {
        // Every motion repaints -- the divider follows the pointer live,
        // as every splitter since Qt 3 has; only the release is saved.
        if (reason == UUI_REASON_RELEASE)
            save_split(id == ID_TREE_SPLIT ? "tree_split" : "pane_split",
                        id == ID_TREE_SPLIT ? &g_tree_split : &g_pane_split);
        uapp_redraw(a);
        return;
    }
    if (reason == UUI_REASON_DROP) {
        // Files dropped on a pane or a tree row: the TARGET widget says
        // where, the payload says from which pane and whether Ctrl
        // asked for a copy. The source pane stays the active one.
        const struct uui_drag *d = uapp_drag(a);
        if (!d || d->kind != UUI_DRAG_FILES) return;
        // No source widget: the drag came from ANOTHER window (or the
        // desktop), and its files are in the drag slot.
        int extern_drop = d->source == 0;
        struct uui_fileview *src = d->source == &g_pane[1] ? &g_pane[1] : &g_pane[0];
        const char *dest = 0;
        if (id == ID_LEFT || id == ID_RIGHT) {
            dest = uui_fileview_drop_target(&g_pane[id == ID_RIGHT]);
        } else if (id == ID_TREE) {
            int nid = uui_tree_drop_id(&g_tree);
            if (nid >= 0 && nid < g_tree_count) dest = g_tree_path[nid];
        }
        if (dest && dest[0]) {
            if (extern_drop) do_drop_extern(dest, d->copy);
            else do_drop(src, dest, d->copy);
        }
        refresh_status();
        uapp_redraw(a);
        return;
    }
    if (reason != UUI_REASON_RELEASE) return;

    if (id == ID_LEFT || id == ID_RIGHT) {
        // Clicking a pane makes it the active one, which is what makes
        // "the other pane" a thing the mouse can choose.
        g_active = (id == ID_RIGHT);
        refresh_status();
        uapp_redraw(a);
        return;
    }
    if (id == ID_TREE) {
        // A SECOND CLICK ON THE SAME ROW EXPANDS OR COLLAPSES IT, which
        // is what Explorer and Dolphin do and what the expander column
        // alone made a small target for. The first click still
        // navigates; the node index IS the id here (fm_tree.c numbers
        // them by slot), so the toggle needs no lookup.
        int nid = uui_tree_selected_id(&g_tree);
        uint64_t now = sys_ticks();
        if (nid >= 0 && nid == g_tree_click_id &&
            now - g_tree_click_tick <= TREE_DOUBLE_CLICK_TICKS) {
            // THE STATE THE FIRST CLICK SAW, not the state now: that
            // click navigated, and navigating REVEALS the node's path,
            // so a blind toggle here reads an already-expanded node and
            // closes the folder the user just asked to open.
            uui_tree_set_collapsed(&g_tree, nid, !g_tree_click_collapsed);
            g_tree_click_id = -1;   // a third click is not a second
            refresh_status();
            uapp_redraw(a);
            return;
        }
        g_tree_click_id = nid;
        g_tree_click_collapsed = uui_tree_is_collapsed(&g_tree, nid);
        // Navigate the ACTIVE pane there. Guarded against the selection
        // that did not move -- an expander click also releases here, and
        // re-entering the same directory would reset its selection.
        if (nid >= 0 && nid < g_tree_count &&
            strcmp(g_tree_path[nid], uui_fileview_dir(active())) != 0)
            fm_goto(g_active, g_tree_path[nid]);
        // AFTER the navigation, which lists a directory: the window is
        // meant to measure the gap between the user's two clicks, not
        // the work the first one caused.
        g_tree_click_tick = sys_ticks();
        refresh_status();
        uapp_redraw(a);
        return;
    }
}

// The buttons: the details pane's Open / Properties and a job's Cancel.
// Under a modal they do nothing, as every routed widget does.
static void on_action(struct uapp *a, int code) {
    if (g_modal != MODAL_NONE) return;
    if (code == ID_DP_OPEN)  do_command(a, CMD_OPEN);
    if (code == ID_DP_PROPS) do_command(a, CMD_PROPERTIES);
    if (code == ID_SEARCH_STOP) {
        search_stop();
        uapp_redraw(a);
    }
    if (code == ID_TOAST_X) {
        undo_toast_hide();
        uapp_redraw(a);
    }
    if (code == ID_TOAST) {
        undo_toast_action();
        uapp_redraw(a);
    }
    if (code == ID_CANCEL) {
        fm_job_cancel();
        set_note("cancelling");
        uapp_redraw(a);
    }
}

// A SECONDARY CLICK ARMS THE CONTEXT MENU. It opens on the release --
// uui_menubar.h says why in full, and it is not a style choice.
static void on_press(struct uapp *a, int x, int y, unsigned buttons) {
    // A press anywhere but the name being edited in place KEEPS the edit
    // (Explorer's rule) -- a press in the pane itself does this inside
    // the widget; this catches the toolbar, the places, the other pane.
    for (int i = 0; i < 2; i++)
        if (g_pane[i].renaming && !uui_textbox_hit(&g_pane[i].rename_box, x, y)) {
            uui_fileview_finish_rename(&g_pane[i]);
            rename_poll();
            uapp_redraw(a);
        }
    // A press anywhere but the field being edited ends the edit,
    // keeping the path as it was -- Dolphin's and Explorer's rule, and
    // the only one under which a click on a row cannot ALSO navigate.
    if (g_addr_edit >= 0 && !uui_textbox_hit(&g_addr[g_addr_edit], x, y)) {
        addr_end_edit(0);
        uapp_redraw(a);
    }
    if (uui_pathbar_is_editing(&g_path) && !uui_pathbar_ops.hit(&g_path, x, y)) {
        uui_pathbar_end_edit(&g_path);
        uapp_redraw(a);
    }
    // The search box lets go of the keyboard, and keeps what it holds.
    if (g_search_on && !uui_textbox_hit(&g_search, x, y)) {
        g_search_on = 0;
        uui_textbox_set_active(&g_search, 0);
        uapp_redraw(a);
    }
    // THE THUMB BUTTONS ARE NAVIGATION, which is the app's decision and
    // not the compositor's -- it delivers SIDE and EXTRA and says
    // nothing about what they mean (abi/win_proto.h). Explorer, Dolphin
    // and every browser read them this way round: the one nearer the
    // thumb's rest position goes back.
    //
    // Before the modal check on purpose: a dialog is up means the app
    // should not navigate, and falling through to the right-click path
    // below with a thumb bit set would open a context menu instead.
    unsigned b = WIN_MOUSE_BUTTONS(buttons);
    if (b & (WIN_MOUSE_BTN_SIDE | WIN_MOUSE_BTN_EXTRA)) {
        if (g_modal == MODAL_NONE)
            do_command(a, (b & WIN_MOUSE_BTN_SIDE) ? CMD_BACK : CMD_FORWARD);
        uapp_redraw(a);
        return;
    }

    if (!(buttons & 0x2) || g_modal != MODAL_NONE) return;
    if (uui_menubar_is_open(&g_ctx)) {
        uui_menubar_close(&g_ctx);  // a second right-click dismisses
        uapp_redraw(a);
        return;
    }
    int p = pane_at(x, y);
    if (p < 0) return;   // not over a listing: no menu, as on the chrome
    g_ctx_armed = 1;
    g_ctx_x = x; g_ctx_y = y;

    // THE CLICK MOVES THE SELECTION FIRST, Explorer's and Dolphin's
    // rule: a menu acting on a row other than the one you pointed at
    // deletes the wrong file.
    g_active = p;
    uui_fileview_select_at(&g_pane[p], x, y);
    refresh_status();
    uapp_redraw(a);
}

static void on_release(struct uapp *a, int x, int y, unsigned buttons) {
    (void)x; (void)y; (void)buttons;
    if (!g_ctx_armed) return;
    g_ctx_armed = 0;
    if (g_modal != MODAL_NONE) return;
    g_ctx_rows = build_ctx_items();
    uui_menubar_open_at(&g_ctx, g_ctx_built, g_ctx_rows, g_ctx_x, g_ctx_y);
    uapp_redraw(a);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    // THE DIALOG FIRST, and it consumes every key: behind it is a
    // listing where a letter seeks and Delete deletes.
    if (uui_dialog_is_open(&g_dialog)) {
        uui_dialog_key(&g_dialog, key);
        int code = uui_dialog_take_code(&g_dialog);
        if (code > 0) answer_dialog(code);
        uapp_redraw(a);
        return;
    }
    // THE NAME BEING EDITED IN PLACE takes every key -- F2, Delete and
    // Ctrl+C mean text editing inside it, not file operations.
    if (uui_fileview_renaming(active())) {
        if (!uui_key_is_shortcut(key, mods)) uui_fileview_key(active(), key);
        rename_poll();
        refresh_status();
        uapp_redraw(a);
        return;
    }

    // THE SEARCH BOX, while it has the keyboard: every key edits the
    // query and the listing follows it; Esc empties it, Enter hands the
    // keyboard back to the listing with the results still up.
    if (g_search_on) {
        if (key == 0x1B) {
            const char *sc = search_scope(uui_fileview_dir(active()));
            if (sc) scope_chosen(0);   // out of the results, to the folder
            search_clear();
        } else if (key == '\n' || key == '\r') {
            // ENTER SEARCHES THE SUBFOLDERS, with the results up and the
            // keyboard back on the listing; typing alone only filters.
            g_search_on = 0;
            uui_textbox_set_active(&g_search, 0);
            strlcpy(g_query[g_active], uui_textbox_text(&g_search), sizeof g_query[g_active]);
            const char *sc = search_scope(uui_fileview_dir(active()));
            if (g_query[g_active][0]) search_into(sc ? sc : uui_fileview_dir(active()));
        } else if (uui_textbox_key_mods(&g_search, key, mods)) {
            search_apply();
        }
        refresh_status();
        uapp_redraw(a);
        return;
    }
    // THE BREADCRUMB AS TEXT: Enter goes there, Esc puts it back.
    if (uui_pathbar_is_editing(&g_path)) {
        char typed[PATH_MAX_LEN];
        uui_pathbar_ops.key(&g_path, key, mods);
        if (uui_pathbar_take(&g_path, typed, sizeof typed)) path_navigate(typed);
        refresh_status();
        uapp_redraw(a);
        return;
    }
    // THE ADDRESS BAR BEING EDITED takes every key: Enter navigates,
    // Esc puts the path back, the rest is typing.
    if (g_addr_edit >= 0) {
        if (key == '\n' || key == '\r') addr_end_edit(1);
        else if (key == 0x1B) addr_end_edit(0);
        else uui_textbox_key_mods(&g_addr[g_addr_edit], key, mods);
        uapp_redraw(a);
        return;
    }
    // ALT+LEFT / ALT+RIGHT, the binding Explorer, Dolphin and every
    // browser use. Tested before the pane's own key handling, where a
    // bare arrow moves the selection -- the modifier is the whole
    // difference between "move down the list" and "leave this folder".
    if ((mods & KEY_MOD_ALT) && (key == KEY_ARROW_LEFT || key == KEY_ARROW_RIGHT)) {
        do_command(a, key == KEY_ARROW_LEFT ? CMD_BACK : CMD_FORWARD);
        uapp_redraw(a);
        return;
    }
    if (key == 0x0C) {   // Ctrl-L: the path as text, as in every file manager
        uui_pathbar_begin_edit(&g_path);
        uapp_redraw(a);
        return;
    }
    if (key == 0x06) {   // Ctrl-F: to the search box
        g_search_on = 1;
        uui_textbox_set_active(&g_search, 1);
        uapp_redraw(a);
        return;
    }
    if (key == 0x01) { do_command(a, CMD_SELECT_ALL); return; }   // Ctrl-A
    // Ctrl+Shift+N: a new folder, Explorer's key. Ctrl+N arrives as its
    // control code with Shift as a modifier bit (api/keyboard.h).
    if (key == 0x0E && (mods & KEY_MOD_SHIFT)) { do_command(a, CMD_MKDIR); return; }

    // ESC STOPS A RUNNING OPERATION, and only then -- asked AFTER the
    // dialog and the prompts above, so an Esc meant for one of those
    // still closes it rather than killing the copy behind it. The
    // popups below are asked after, which costs nothing: a menu cannot
    // be open while a job runs without the same Esc being wanted here
    // first, and Explorer's stop button and Esc do the same one thing.
    if (key == 0x1B && fm_job_running()) {
        fm_job_cancel();
        set_note("cancelling");
        uapp_redraw(a);
        return;
    }

    int code;
    // AN OPEN POPUP TAKES THE KEY, wherever the app thinks it is
    // (CLAUDE.md). Asked before the bar, which is closed whenever this
    // one is open.
    if (uui_menubar_key(&g_ctx, key, &code)) {
        if (code >= 0) do_command(a, code);
        else uapp_redraw(a);
        return;
    }

    switch (key) {
    case '\t':       do_command(a, CMD_SWAP);    return;
    case KEY_F5:     do_command(a, CMD_COPY);    return;
    case KEY_F6:     do_command(a, CMD_MOVE);    return;
    case KEY_F7:     do_command(a, CMD_MKDIR);   return;
    case KEY_F2:     do_command(a, CMD_RENAME);  return;
    case KEY_F8:
    case KEY_DELETE:
        do_command(a, (mods & KEY_MOD_SHIFT) ? CMD_DELETE_FOREVER : CMD_DELETE);
        return;
    default: break;
    }
    if (key == 0x12 && (mods & KEY_MOD_CTRL)) { do_command(a, CMD_REFRESH); return; } // Ctrl-R

    // THE CLIPBOARD KEYS. Ctrl+<letter> arrives as the control code, not
    // as a letter plus a modifier bit (api/keyboard.h), so these are
    // matched as codes -- 0x03/0x18/0x16 ARE Ctrl-C/X/V. Ctrl+C is an
    // ordinary key here and the WM routes nothing: a terminal needs it
    // for INTR, and a compositor that took it globally would have taken
    // that away.
    if (key == 0x03) { do_command(a, CMD_CLIP_COPY);  return; } // Ctrl-C
    if (key == 0x18) { do_command(a, CMD_CLIP_CUT);   return; } // Ctrl-X
    if (key == 0x16) { do_command(a, CMD_CLIP_PASTE); return; } // Ctrl-V
    if (key == 0x1A) { do_command(a, CMD_UNDO); return; }       // Ctrl-Z
    if (key == 0x19) { do_command(a, CMD_REDO); return; }       // Ctrl-Y

    // MOVING A DIVIDER FROM THE KEYBOARD. GtkPaned focuses its handle
    // and takes the arrows from there; this app cannot, because Tab is
    // the commander's pane swap and there is no focus ring to put a
    // handle in. So the chord names the divider instead: Ctrl for the
    // one between the panes, Ctrl+Shift for the one beside the tree.
    // Shift arrives as a DIFFERENT key code, not as a modifier bit
    // (api/keyboard.h's KEY_SHIFT_ARROW_* family).
    if (mods & KEY_MOD_CTRL) {
        struct uui_splitter *sp = 0;
        const char *save_key = 0;
        int k = key;
        if ((key == KEY_ARROW_LEFT || key == KEY_ARROW_RIGHT) && !g_single) {
            sp = &g_pane_split; save_key = "pane_split";
        } else if ((key == KEY_SHIFT_ARROW_LEFT || key == KEY_SHIFT_ARROW_RIGHT)
                    && g_tree_on) {
            sp = &g_tree_split; save_key = "tree_split";
            k = (key == KEY_SHIFT_ARROW_LEFT) ? KEY_ARROW_LEFT : KEY_ARROW_RIGHT;
        }
        if (sp) {
            if (uui_splitter_key(sp, k)) save_split(save_key, sp);
            uapp_redraw(a);
            return;
        }
    }

    // Everything else is the active pane's: arrows, Home/End, PageUp/
    // PageDown, Enter (descend) and Backspace (up) are all one call,
    // because uui_fileview owns what a directory listing does.
    //
    // **AND THAT INCLUDES NAVIGATING**, which is why the directory is
    // compared across the call rather than recorded by whoever asked.
    // Enter and Backspace move the pane from INSIDE the widget, so they
    // never pass through fm_goto()/fm_goto_up() -- history recorded the
    // descent and not the climb back, and Back then pointed at the
    // directory the pane was already showing and appeared to do
    // nothing. Comparing here catches every navigation the widget does
    // on its own, including any it grows later.
    char before[PATH_MAX_LEN];
    snprintf(before, sizeof before, "%s", uui_fileview_dir(active()));
    if (!uui_key_is_shortcut(key, mods) && uui_fileview_key(active(), key)) {
        const char *now = uui_fileview_dir(active());
        if (strcmp(before, now) != 0) fm_history_record(g_active, now);
        refresh_status();
        uapp_redraw(a);
    }
}

static int on_tick(struct uapp *a) {
    (void)a;
    // BEFORE anything that can return early. A double click descends
    // from inside uui_fileview, so a mouse navigation reaches no
    // fm_goto*(); this is where it gets recorded (fm_history.c).
    fm_history_sync();
    int changed = poll_job();
    if (uui_toolbar_tick(&g_toolbar)) changed = 1;
    if (uui_toolbar_tick(&g_nav)) changed = 1;
    if (uui_toolbar_tick(&g_viewbar)) changed = 1;
    if (thumb_tick()) changed = 1;
    if (undo_toast_tick()) changed = 1;

    // Not under a rubber band: a reload clears the marks the band is
    // mid-way through choosing (the desktop's desktop_drag_active() rule).
    if (uui_fileview_band_active(&g_pane[0]) ||
        uui_fileview_band_active(&g_pane[1]))
        return changed;

    unsigned long long gen = sys_fs_generation();
    if (gen != g_seen_generation) {
        g_seen_generation = gen;
        for (int i = 0; i < 2; i++)
            if (sys_fs_generation_of(fm_watch_path(uui_fileview_dir(&g_pane[i]))) != g_pane_gen[i])
                reload_pane(&g_pane[i]);
        int places = g_places.count;
        uui_places_refresh(&g_places);  // free space moved, or a disk came
        if (g_places.count != places) tree_rebuild();
        else if (!tree_poll()) tree_refresh_meters();
        refresh_status();
        changed = 1;
    }
    return changed;
}

// The resolver is lib/uopen.h now -- the user's /etc/mimeapps.conf
// override outranks the .desktop declarations, and /bin/open speaks
// the same one, so a double click here and `open x.txt` at a prompt
// cannot disagree.
static void on_pane_open(void *ctx, const char *path) {
    if (in_search(&g_pane[(int)(intptr_t)ctx])) {   // a folder among results: go there
        struct sys_stat st;
        if (sys_stat(path, &st) == 0 && st.is_dir) { fm_goto((int)(intptr_t)ctx, path); return; }
    }
    if (in_bin(&g_pane[(int)(intptr_t)ctx])) {   // Explorer's rule: a deleted file is not opened
        snprintf(g_stat_note, sizeof g_stat_note, "restore %s to open it",
                 k_path_basename(path));
        return;
    }
    char exec[PATH_MAX_LEN];
    if (!uopen_resolve(path, exec, sizeof exec)) {
        // Said out loud rather than doing nothing: a double click that
        // produces silence reads as a broken app.
        snprintf(g_stat_note, sizeof g_stat_note, "no app for %s",
                  k_path_basename(path));
        ulogf("files: open %s -- no handler\n", path);
        return;
    }
    // NOT a tracked JOB -- this is a launch, not an operation on files,
    // and waiting for a text editor to exit would freeze the manager
    // for as long as somebody was editing. It IS a tracked CHILD, which
    // is a different thing: uapp reaps it when it exits, so opening and
    // closing files does not fill `ps` with zombies (ui/uapp.h).
    if (uapp_spawn(g_app, exec, path) < 0) {
        snprintf(g_stat_note, sizeof g_stat_note, "could not start %s", exec);
        ulogf("files: open %s -- spawn %s FAILED\n", path, exec);
    } else {
        snprintf(g_stat_note, sizeof g_stat_note, "opened %s", k_path_basename(path));
        ulogf("files: open %s -- %s\n", path, exec);
    }
}

// `ctx` is the pane index: the callback is the widget's, so which pane
// it came from has to be carried rather than guessed from g_active,
// which the WIDGET does not know about.
static void on_pane_dir(void *ctx, const char *dir) {
    int i = (int)(intptr_t)ctx;
    undo_toast_hide();   // it was about the folder being left
    // A NEW FOLDER ENDS A SEARCH, Explorer's rule: the query was about
    // the folder you left. Re-listed without the filter. Entering a
    // search's results, or switching its scope, keeps it.
    const char *sc = search_scope(dir);
    if (sc) {
        if (g_keep_query || g_query[i][0]) search_start(sc, g_query[i]);
    } else if (g_keep_query) {
        /* the strip's "This folder": the filter stays */
    } else {
        search_stop();
        g_scope[i][0] = '\0';
    }
    if (g_query[i][0] && !sc && !g_keep_query) {
        g_query[i][0] = '\0';
        if (i == g_active) uui_textbox_set_text(&g_search, "");
        uui_fileview_reload(&g_pane[i]);
    }
    refresh_dim();   // the reload cleared the bits; see refresh_dim()
    // Written on every change rather than at exit, because a window
    // manager can Force Quit this process and an exit-time save is a
    // save that does not happen. Two keys, so the whole document is
    // rewritten twice per navigation -- 512 bytes, and the alternative
    // is a dirty flag that has to be right.
    // A search is not a place to come back to: the folder it searched is.
    uconf_set(FILES_CONF, i ? "right" : "left", sc ? (g_scope[i][0] ? g_scope[i] : sc) : dir);
    // AFTER that write: a pane showing /etc would otherwise read it as a
    // change and repaint a second time on every navigation there.
    pane_listed(i);

    // The tree follows the ACTIVE pane on a NAVIGATION: ancestors
    // opened, the node selected and scrolled to. Never on a toggle, so
    // a branch collapsed while standing in it stays collapsed until the
    // next directory change (fm_tree.c's tree_reveal_path).
    if (g_tree_on && i == g_active) tree_reveal_path(dir);

    refresh_status();
}

// One file's facts for the dialog, so Overwrite is an informed choice
// rather than a guess -- which is exactly what Windows and KDE both put
// in this dialog and why.
static void describe(char *out, int cap, const char *label, const char *path) {
    struct sys_stat st;
    if (sys_stat(path, &st) != 0) { snprintf(out, (size_t)cap, "%s: gone", label); return; }
    char human[24], when[48];
    human_size(human, sizeof human, st.size);
    udate_format(when, sizeof when, &st.modified, UDATE_DATE | UDATE_TIME);
    snprintf(out, (size_t)cap, "%s  %s  %s", label, human, when);
}

// Answers the worker, and remembers the answer when it was an
// apply-to-all.
static void answer_conflict(int code) {
    const char *src = fm_conflict_src(), *dst = fm_conflict_dst();
    switch (code) {
    case DLG_OVERWRITE_ALL: g_apply_all = DLG_OVERWRITE; /* fall through */
    case DLG_OVERWRITE:     fm_conflict_answer(UFILEOP_OVERWRITE, 0); break;
    case DLG_SKIP_ALL:      g_apply_all = DLG_SKIP; /* fall through */
    case DLG_SKIP:          fm_conflict_answer(UFILEOP_SKIP, 0); break;
    case DLG_RENAME_ALL:    g_apply_all = DLG_RENAME; /* fall through */
    case DLG_RENAME: {
        char dir[PATH_MAX_LEN];
        k_path_dirname(dst, dir, sizeof dir);
        // "notes.txt" -> "notes (1).txt", the number before the
        // extension so the copy still opens with the same app.
        int ok = ufileop_unique_name(dir, k_path_basename(src), g_dlg_rename,
                                      sizeof g_dlg_rename);
        if (ok)
            fm_conflict_answer(UFILEOP_RENAME, g_dlg_rename);
        else
            fm_conflict_answer(UFILEOP_SKIP, 0);
        break;
    }
    default:                fm_conflict_answer(UFILEOP_CANCEL, 0); break;
    }
}

// One dialog widget, two questions: which one is up decides what an
// answer means.
static void answer_dialog(int code) {
    enum dialog_kind kind = g_dialog_kind;
    g_dialog_kind = DIALOG_NONE;
    if (kind == DIALOG_PROMPT) {
        answer_prompt(code);
        refresh_status();
        return;
    }
    if (kind == DIALOG_DELETE) {
        if (code == DLG_DELETE) commit_delete();
        else set_note("cancelled");
        refresh_status();
        return;
    }
    answer_conflict(code);
}

static void raise_conflict(struct uapp *a) {
    // ANSWERED WITHOUT ASKING when an apply-to-all is standing. The
    // dialog is not even built, which is the point of the checkbox
    // every real file manager has.
    if (g_apply_all > 0) { answer_conflict(g_apply_all); return; }
    g_dialog_kind = DIALOG_CONFLICT;

    const char *src = fm_conflict_src(), *dst = fm_conflict_dst();
    snprintf(g_dlg_rows[0], sizeof g_dlg_rows[0], "%s already exists.",
              k_path_basename(dst));
    describe(g_dlg_rows[1], sizeof g_dlg_rows[1], "Existing:", dst);
    describe(g_dlg_rows[2], sizeof g_dlg_rows[2], "New:     ", src);

    static const struct uui_dialog_button btns[] = {
        { "Overwrite",     DLG_OVERWRITE, 0 },
        { "Overwrite all", DLG_OVERWRITE_ALL, 0 },
        { "Skip",          DLG_SKIP, 0 },
        { "Skip all",      DLG_SKIP_ALL, 0 },
        { "Rename",        DLG_RENAME, 0 },
        { "Cancel",        DLG_CANCEL, 0 },
    };
    uui_dialog_open(&g_dialog, "File already exists", g_dlg_row_ptr, 3,
                     btns, (int)(sizeof btns / sizeof btns[0]),
                     0, DLG_CANCEL);
    uapp_redraw(a);
}

// The worker posted. Both cases run HERE, on the main thread.
static int on_user(struct uapp *a, int a0, int a1) {
    (void)a1;
    if (a0 == POST_CONFLICT) { raise_conflict(a); return 1; }
    if (a0 == POST_DONE) {
        g_apply_all = -1;       // one operation, one memory
        fm_job_finished();
        return 1;
    }
    if (a0 == POST_SEARCH) {
        for (int i = 0; i < 2; i++)
            if (in_search(&g_pane[i])) uui_fileview_reload(&g_pane[i]);
        if (a1) ulogf("files: search done, %d found\n", uui_fileview_count(active()));
        refresh_status();
        return 1;
    }
    if (a0 == POST_THUMB) {
        int got = thumb_posted();
        if (got) delete_picture();   // the delete card may be waiting on it
        return got;
    }
    return 0;
}

// The clipboard changed -- ours or anyone's.
// A PENDING CUT IS DRAWN FADED, which is what Explorer and Dolphin both
// do and the only thing on screen that says a cut is staged at all.
//
// Applied BY NAME, every time: the clipboard holds paths and a pane
// holds rows, and a row index does not survive a reload (see
// ui/uui_fileview.h). Only a CUT dims -- a copy takes nothing away, so
// fading its source would say something untrue.
void refresh_dim(void) {
    for (int i = 0; i < 2; i++) uui_fileview_clear_dimmed(&g_pane[i]);
    if (g_clip_op != UCLIP_CUT) return;

    // STATIC: struct uclip embeds the whole 64 KiB payload, which is
    // thirty times the ring-3 frame budget (lib/uclip.h).
    static struct uclip c;
    uclip_load(&c);
    if (uclip_op(&c) != UCLIP_CUT) return;

    for (int n = 0; n < uclip_count(&c); n++) {
        const char *path = uclip_path(&c, n);
        if (!path) break;
        char dir[PATH_MAX_LEN];
        k_path_dirname(path, dir, sizeof dir);
        for (int i = 0; i < 2; i++) {
            if (strcmp(uui_fileview_dir(&g_pane[i]), dir) != 0) continue;
            int row = uui_fileview_row_of(&g_pane[i], k_path_basename(path));
            if (row >= 0) uui_fileview_set_dimmed(&g_pane[i], row, 1);
        }
    }
}

void pane_listed(int pane) {
    g_pane_gen[pane] = sys_fs_generation_of(fm_watch_path(uui_fileview_dir(&g_pane[pane])));
}

void reload_pane(struct uui_fileview *fv) {
    int i = fv == &g_pane[1];
    pane_listed(i);   // BEFORE the listing: a change during it shows next tick
    ulogf("files: reload %d %s\n", i, uui_fileview_dir(fv));
    uui_fileview_reload(fv);
    refresh_dim();
}

void reload_panes(void) {
    reload_pane(&g_pane[0]);
    reload_pane(&g_pane[1]);
}

static void on_clipboard(struct uapp *a, int op, unsigned serial) {
    (void)serial;
    g_clip_op = op;
    refresh_dim();
    uapp_redraw(a);   // Paste greys and ungreys with it
}

// The file this launch was handed, selected in main() and scrolled to
// here: main() runs before the first layout, when the view has no size
// to scroll within.
static const char *g_start_select;

static void on_open(struct uapp *a) {
    g_app = a;
    // Large icons are two text lines tall doubled: measured now, with the
    // font up -- in main() every size is 0.
    char opt[16];
    for (int i = 0; i < 2; i++)
        if (uconf_get(FILES_CONF, i ? "right_view" : "left_view", opt, sizeof opt) &&
            !strcmp(opt, "large"))
            g_pane[i].icon_px = ugfx_char_h() * 6;
    layout_all(uapp_width(a), uapp_height(a));
    if (g_start_select) uui_fileview_select_name(&g_pane[0], g_start_select);
    refresh_status();
}

static void on_resize(struct uapp *a, int w, int h) {
    (void)a;
    layout_all(w, h);
}

int main(int argc, char **argv) {
    // Argument first, then the remembered directory, then the root. An
    // explicit argument must win: "open the file manager HERE" is a
    // statement about this launch, not a new preference.
    options_load();
    char saved_left[PATH_MAX_LEN], saved_right[PATH_MAX_LEN];
    if (!uconf_get(FILES_CONF, "left", saved_left, sizeof saved_left)) saved_left[0] = '\0';
    if (!uconf_get(FILES_CONF, "right", saved_right, sizeof saved_right)) saved_right[0] = '\0';

    // Options' "Open in": where the last run left off, Home, or the root.
    if (g_opt.start != FM_START_LAST) {
        strlcpy(saved_left, g_opt.start == FM_START_HOME ? "/home" : "/", sizeof saved_left);
        strlcpy(saved_right, saved_left, sizeof saved_right);
    }
    const char *left = (argc > 1 && argv[1][0]) ? argv[1]
                        : (saved_left[0] ? saved_left : "/");
    const char *right = (argc > 2 && argv[2][0]) ? argv[2]
                         : (saved_right[0] ? saved_right : "/");

    uui_toolbar_init(&g_nav, nav_items, (int)(sizeof nav_items / sizeof nav_items[0]));
    g_nav.item_flags = menu_item_flags;
    uui_toolbar_init(&g_viewbar, viewbar_items,
                      (int)(sizeof viewbar_items / sizeof viewbar_items[0]));
    g_viewbar.item_flags = menu_item_flags;
    g_viewbar.compact = 1;
    g_nav.compact = 0;
    uui_pathbar_init(&g_path, "System", "drive");
    uui_pathbar_set_scheme(&g_path, FM_BIN, "Recycle Bin", "place-trash");
    uui_pathbar_set_scheme(&g_path, "search:/", "Search results", "tb-find");
    uui_segmented_init(&g_scope_seg, g_scope_names, 3, 0);
    uui_button_init(&g_search_stop, 0, 0, 0, 0, "Stop", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_SEARCH_STOP);
    uui_textbox_init(&g_search, "");
    g_search.placeholder = g_search_hint;
    // The places are the file chooser's (ui/uui_filedialog.c) -- one
    // idea of where Documents is.
    uui_places_init(&g_places);
    uui_places_add(&g_places, "Home",      "place-home",      "/home");
    uui_places_add(&g_places, "Desktop",   "place-desktop",   "/home/desktop");
    uui_places_add(&g_places, "Documents", "place-documents", "/usr/share/doc");
    uui_places_add(&g_places, "Music",     "place-music",     "/usr/share/music");
    uui_places_add(&g_places, "Pictures",  "place-pictures",  "/usr/share/wallpapers");
    uui_places_add(&g_places, "Recycle Bin", "place-trash",   FM_BIN);
    uui_places_refresh(&g_places);
    uui_button_init(&g_dp_open, 0, 0, 0, 0, "Open", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_DP_OPEN);
    uui_button_init(&g_dp_props, 0, 0, 0, 0, "Properties", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_DP_PROPS);
    // NO ITEMS: a context menu has no bar strip, so Left/Right have no
    // titles to walk out into (ui/uui_menubar.h).
    uui_menubar_init(&g_ctx, 0, 0);
    g_ctx.item_flags = menu_item_flags;
    uui_toolbar_init(&g_toolbar, toolbar_items,
                      (int)(sizeof toolbar_items / sizeof toolbar_items[0]));
    g_toolbar.item_flags = menu_item_flags; // ONE state source -- see uui_toolbar.h
    uui_button_init(&g_cancel_btn, 0, 0, 0, 0, "Cancel",
                     UTHEME_BUTTON_BG, UTHEME_TEXT, ID_CANCEL);
    // On the toast's light card: the accent button, and a flat dismiss
    // in the card's own colour, so only its hover shows a face.
    uui_button_init(&g_toast_btn, 0, 0, 0, 0, "Undo", UTHEME_ACCENT, UTHEME_WHITE, ID_TOAST);
    g_toast_btn.outlined = 1;
    uui_button_init(&g_toast_x, 0, 0, 0, 0, "\xd7", uui_popup_bg(), UTHEME_TEXT, ID_TOAST_X);
    uui_statusbar_init(&g_status);
    g_status.panes[0].text = g_stat_dir;      // "118 items"
    g_status.panes[0].chars = 14;
    g_status.panes[1].text = g_stat_items;    // "1 item selected, 258 KB"
    g_status.panes[1].chars = 28;
    g_status.panes[2].text = g_stat_note;     // what just happened
    g_status.panes[2].chars = 0;
    g_status.count = 3;

    for (int i = 0; i < 2; i++) {
        uui_fileview_init(&g_pane[i], 0, 0, 100, 100,
                           i ? g_right_entries : g_left_entries, PANE_FILES);
        g_pane[i].on_open = on_pane_open;
        g_pane[i].ctx = (void *)(intptr_t)i;
        uui_fileview_set_resolver(&g_pane[i], resolve_virtual, 0);
        uui_fileview_set_thumb(&g_pane[i], g_opt.thumbs ? pane_thumb : 0, 0);
        uui_fileview_set_filter(&g_pane[i], pane_filter, (void *)(intptr_t)i);
        g_pane[i].single_click = g_opt.single_click;
        g_pane[i].hide_ext = !g_opt.extensions;
    }

    // The remembered view options. Unknown values fall back to the
    // defaults they misspell, deliberately -- a config file is not a
    // place to fail from.
    char opt[16];
    if (uconf_get(FILES_CONF, "panes", opt, sizeof opt))
        g_single = (opt[0] == '1');
    if (uconf_get(FILES_CONF, "tree", opt, sizeof opt))
        g_tree_on = (opt[0] == '1');
    // ICONS unless the file says details: the default every desktop
    // file manager opens in, and what the thumbnails were built for.
    for (int i = 0; i < 2; i++) {
        if (!uconf_get(FILES_CONF, i ? "right_view" : "left_view", opt, sizeof opt))
            opt[0] = '\0';
        if (!strcmp(opt, "details")) continue;
        if (!strcmp(opt, "large")) g_pane[i].icon_px = 0;   // sized in on_open, once the font is up
        uui_fileview_set_mode(&g_pane[i], UUI_FILEVIEW_ICONS);
    }
    if (uconf_get(FILES_CONF, "details_pane", opt, sizeof opt))
        g_dpane = (opt[0] == '1');
    for (int i = 0; i < 2; i++) uui_textbox_init(&g_addr[i], "/");

    uui_splitter_init(&g_tree_split, 1, TREE_SPLIT_DEFAULT);
    uui_splitter_init(&g_pane_split, 1, PANE_SPLIT_DEFAULT);
    if (uconf_get(FILES_CONF, "tree_split", opt, sizeof opt))
        uui_splitter_set_frac(&g_tree_split, atoi(opt));
    if (uconf_get(FILES_CONF, "pane_split", opt, sizeof opt))
        uui_splitter_set_frac(&g_pane_split, atoi(opt));

    tree_init();

    // HANDED A FILE, open its FOLDER with the file selected -- Explorer's
    // `/select,` and Dolphin's --select, which is what Task Manager's
    // "Open file location" means. Opened as a folder, a file listed as
    // an error. The selection goes after the goto (docs/conventions/gui.md,
    // "AN APP HANDED A FILE MUST SELECT IT").
    static char start_dir[PATH_MAX_LEN];
    const char *select_name = 0;
    struct sys_stat st;
    if (argc > 1 && sys_stat(left, &st) == 0 && !st.is_dir &&
        k_path_dirname(left, start_dir, sizeof start_dir)) {
        select_name = k_path_basename(left);
        left = start_dir;
    }

    fm_goto(0, left);
    if (select_name) uui_fileview_select_name(&g_pane[0], select_name);
    g_start_select = select_name;
    fm_goto(1, right);
    // Hooked up AFTER the opening directories are set, so starting the
    // app with an explicit argument does not silently rewrite the
    // remembered pair -- an argument is a statement about this launch.
    for (int i = 0; i < 2; i++) {
        g_pane[i].on_dir_changed = on_pane_dir;
        pane_listed(i);   // the baseline on_tick() compares against
    }
    g_seen_generation = sys_fs_generation();
    if (g_tree_on) tree_reveal_path(uui_fileview_dir(active()));
    else { tree_rebuild(); tree_select_path(uui_fileview_dir(active())); }
    // ASKED ONCE AT STARTUP: the broadcast only fires on a CHANGE, so an
    // app that opens after somebody else copied would show Paste greyed
    // until the next one.
    {
        static struct uclip c;   // see refresh_dim() on why it is static
        uclip_load(&c);
        g_clip_op = uclip_op(&c);
    }
    set_note("");

    // One line, once: which directories this instance opened with and
    // where they came from. It is what turned "the pane is in the wrong
    // place" from a guess into a diagnosis in one run.
    ulogf("files: start left=%s right=%s (saved %s|%s)\n", left, right,
          saved_left, saved_right);

    struct uapp_desc desc = {
        .title        = "File Manager",
        .app_id       = "files",
        .w            = WIN_W,
        .h            = WIN_H,
        .flags        = UAPP_RESIZABLE,
        .min_w        = 420,
        .min_h        = 260,
        .tick_ms      = 500,
        .widgets      = g_widgets,
        .widget_count = (int)(sizeof g_widgets / sizeof g_widgets[0]),
        .on_open      = on_open,
        .on_draw      = on_draw,
        .on_draw_over = on_draw_over,
        .on_widget    = on_widget,
        .on_action    = on_action,
        .on_press     = on_press,
        .on_release   = on_release,
        .on_key       = on_key,
        .on_tick      = on_tick,
        .on_resize    = on_resize,
        .on_clipboard = on_clipboard,
        .on_user      = on_user,
    };
    return uapp_run(&desc);
}
