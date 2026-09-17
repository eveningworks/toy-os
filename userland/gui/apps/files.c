// File Manager -- two directory panes, side by side.
//
// IT IS A COMMANDER, NOT AN EXPLORER, AND THAT IS THE DESIGN DECISION
// WORTH STATING (docs/filemanager-design.md has the long form). This
// system has no clipboard and no drag-and-drop; both are their own
// roadmap milestone. Explorer's two primary verbs are copy/paste and
// drag-onto-a-window, so building that shape first would mean shipping
// a file manager whose main actions are greyed out.
//
// Norton Commander answered this in 1986 and Midnight Commander, Total
// Commander and Krusader have kept the answer: put TWO directories on
// screen and copying needs no transfer mechanism at all -- the source
// is the active pane, the destination is the other one, and F5 is copy.
// Nothing is carried, so nothing needs a carrier. The keymap is theirs
// too (F5/F6/F7/F8, Tab, Enter, Backspace), which is free familiarity.
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

#define WIN_W 720
#define WIN_H 440

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
int g_tree_on = 1;

struct uui_menubar g_menu;
struct uui_menubar g_ctx;   // the context menu -- no bar of its own
struct uapp *g_app;         // for the widget callbacks, which carry none
struct uui_toolbar g_toolbar;
struct uui_statusbar g_status;

// CANCEL, and it exists only while an operation is running. Hidden the
// rest of the time rather than disabled: a permanently greyed button in
// the status bar is chrome that never does anything, and Explorer's
// stop button appears with the progress it stops.
struct uui_button g_cancel_btn;

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

static const struct uui_menu_item file_items[] = {
    UUI_MENU("Open",           CMD_OPEN,    "Enter"),
    UUI_MENU("Properties",     CMD_PROPERTIES, 0),
    UUI_MENU_SEP,
    UUI_MENU("Cut",            CMD_CLIP_CUT,   "Ctrl+X"),
    UUI_MENU("Copy to clipboard", CMD_CLIP_COPY, "Ctrl+C"),
    UUI_MENU("Paste",          CMD_CLIP_PASTE, "Ctrl+V"),
    UUI_MENU_SEP,
    UUI_MENU("Copy",           CMD_COPY,    "F5"),
    UUI_MENU("Move",           CMD_MOVE,    "F6"),
    UUI_MENU("New folder",     CMD_MKDIR,   "F7"),
    UUI_MENU("Rename",         CMD_RENAME,  "F2"),
    UUI_MENU("Delete",         CMD_DELETE,  "F8"),
    UUI_MENU_SEP,
    UUI_MENU("Exit",           CMD_EXIT,    "Alt+F4"),
};

static const struct uui_menu_item view_items[] = {
    UUI_MENU("Details",     CMD_VIEW_DETAILS, 0),
    UUI_MENU("Icons",       CMD_VIEW_ICONS,   0),
    UUI_MENU_SEP,
    UUI_MENU("Second pane", CMD_VIEW_PANES,   0),
    UUI_MENU("Folder tree", CMD_VIEW_TREE,    0),
};

static const struct uui_menu_item go_items[] = {
    UUI_MENU("Up",             CMD_UP,      "Backspace"),
    UUI_MENU("Other pane",     CMD_SWAP,    "Tab"),
    UUI_MENU("Refresh",        CMD_REFRESH, "Ctrl+R"),
};

// THE TOOLBAR CARRIES THE VERBS NOW. They were a row of five buttons
// across the bottom -- Norton Commander's function-key bar, which every
// commander since has kept -- and they moved up here because the same
// five commands were already in the File menu and on F5-F8, so the row
// was a third copy costing a whole row of pane height. The KEYS are
// untouched, and the status bar still names them.
//
// Same codes and the same item_flags as the menus, so a latched button
// and a ticked menu item cannot disagree.
static const struct uui_toolbar_item toolbar_items[] = {
    { "tb-up",      "Up",          CMD_UP },
    { "tb-refresh", "Refresh",     CMD_REFRESH },
    UUI_TOOLBAR_SEP,
    { "tb-copy",    "Copy",        CMD_COPY },
    { "tb-move",    "Move",        CMD_MOVE },
    { "tb-mkdir",   "New folder",  CMD_MKDIR },
    { "tb-rename",  "Rename",      CMD_RENAME },
    { "tb-delete",  "Delete",      CMD_DELETE },
    UUI_TOOLBAR_SEP,
    { "tb-details", "Details",     CMD_VIEW_DETAILS },
    { "tb-icons",   "Icons",       CMD_VIEW_ICONS },
    UUI_TOOLBAR_SEP,
    { "tb-panes",   "Second pane", CMD_VIEW_PANES },
    { "tb-tree",    "Folder tree", CMD_VIEW_TREE },
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
    for (int i = 0; i < (int)(sizeof ctx_items / sizeof ctx_items[0]); i++) {
        if (ctx_items[i].code == CMD_EDIT && !can_edit) continue;
        g_ctx_built[n++] = ctx_items[i];
    }
    return n;
}

static const struct uui_menu_item menu_items[] = {
    UUI_SUBMENU("File", file_items),
    UUI_SUBMENU("View", view_items),
    UUI_SUBMENU("Go",   go_items),
};

// Details/Icons tick as a pair (the ACTIVE pane's current mode) and the
// two toggles tick when their thing is SHOWN -- so "Second pane" is
// checked in the default two-pane state, not when the option was used.
static unsigned menu_item_flags(int code) {
    switch (code) {
    case CMD_UP: {
        const char *d = uui_fileview_dir(&g_pane[g_active]);
        return (d[0] == '/' && d[1] == '\0') ? UUI_MI_DISABLED : 0;
    }
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
        return operand_count() > 0 ? 0 : UUI_MI_DISABLED;
    case CMD_CLIP_PASTE:
        // Greyed when there is nothing to paste, which is what tells a
        // user the Ctrl+C in the other window did not take.
        return g_clip_op != UCLIP_NONE ? 0 : UUI_MI_DISABLED;
    case CMD_VIEW_DETAILS:
        return g_pane[g_active].mode == UUI_FILEVIEW_DETAILS ? UUI_MI_CHECKED : 0;
    case CMD_VIEW_ICONS:
        return g_pane[g_active].mode == UUI_FILEVIEW_ICONS ? UUI_MI_CHECKED : 0;
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
    { .ops = &uui_menubar_ops, .widget = &g_menu, .id = ID_MENU, .name = "menu" },
    { .ops = &uui_toolbar_ops, .widget = &g_toolbar, .id = ID_TOOLBAR, .name = "toolbar" },
    { .ops = &uui_fileview_ops, .widget = &g_pane[0], .id = ID_LEFT, .name = "left" },
    { .ops = &uui_fileview_ops, .widget = &g_pane[1], .id = ID_RIGHT, .name = "right" },
    { .ops = &uui_tree_ops, .widget = &g_tree, .id = ID_TREE, .name = "tree" },
    { .ops = &uui_splitter_ops, .widget = &g_tree_split, .id = ID_TREE_SPLIT, .name = "treesplit" },
    { .ops = &uui_splitter_ops, .widget = &g_pane_split, .id = ID_PANE_SPLIT, .name = "panesplit" },
    { .ops = &uui_button_ops, .widget = &g_cancel_btn, .id = ID_CANCEL, .hidden = 1 },
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
        if (!uui_fileview_set_dir(&g_pane[pane], path)) {
            // The failed listing left the pane EMPTY (ui/uui_fileview.h);
            // put the directory it had back rather than show a hole.
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
// whole-file rewrite. The generation is adopted for the same reason
// on_pane_dir() adopts it -- this app watches the filesystem, and
// without it every save reads back as somebody else changing the disk.
void save_split(const char *key, const struct uui_splitter *sp) {
    char v[12];
    snprintf(v, sizeof v, "%d", uui_splitter_frac(sp));
    uconf_set(FILES_CONF, key, v);
    g_seen_generation = sys_fs_generation();
}

// --- status -----------------------------------------------------------

void refresh_status(void) {
    // WHILE A JOB RUNS the note is the job -- "Copy 3/7  notes.txt  62%"
    // -- which is the thing a spawned child could never report, and the
    // reason the operation moved in here.
    if (fm_job_running()) {
        fm_job_status(g_stat_note, sizeof g_stat_note);
    }

    struct uui_fileview *fv = active();
    snprintf(g_stat_dir, sizeof g_stat_dir, "%s%s",
              uui_fileview_dir(fv), g_active ? "  [right]" : "  [left]");

    char total[24];
    human_size(total, sizeof total, uui_fileview_total_bytes(fv));
    int marks = uui_fileview_mark_count(fv);
    if (marks > 0)
        snprintf(g_stat_items, sizeof g_stat_items, "%d marked of %d, %s",
                  marks, uui_fileview_count(fv), total);
    else
        snprintf(g_stat_items, sizeof g_stat_items, "%d item%s, %s%s",
                  uui_fileview_count(fv), uui_fileview_count(fv) == 1 ? "" : "s",
                  total, uui_fileview_truncated(fv) ? " (more)" : "");
}

void do_command(struct uapp *a, int code) {
    switch (code) {
    case CMD_COPY:   do_copy(); break;
    case CMD_MOVE:   do_move(); break;
    case CMD_DELETE: do_delete(); break;
    case CMD_MKDIR:  open_prompt(CMD_MKDIR, "New folder", ""); break;
    case CMD_RENAME: {
        const char *name = uui_fileview_selected_name(active());
        if (!name) { set_note("nothing selected"); break; }
        open_prompt(CMD_RENAME, "Rename", name);
        break;
    }
    case CMD_UP:
        uui_fileview_up(active());
        break;
    case CMD_CLIP_COPY:  clip_copy();  break;
    case CMD_CLIP_CUT:   clip_cut();   break;
    case CMD_CLIP_PASTE: clip_paste(); break;
    case CMD_OPEN:
        // The same act as Enter or a double click, so a directory
        // descends and a file goes to whatever /etc/mimeapps.conf and
        // the Handles= declarations resolve to (lib/uopen.h).
        if (!uui_fileview_activate(active())) set_note("nothing selected");
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
    case CMD_VIEW_ICONS: {
        // The ACTIVE pane's, not the window's: two panes with two modes
        // is normal in every commander that grew a thumbnail view.
        enum uui_fileview_mode m = code == CMD_VIEW_ICONS ? UUI_FILEVIEW_ICONS
                                                           : UUI_FILEVIEW_DETAILS;
        uui_fileview_set_mode(active(), m);
        uconf_set(FILES_CONF, g_active ? "right_view" : "left_view",
                   m == UUI_FILEVIEW_ICONS ? "icons" : "details");
        g_seen_generation = sys_fs_generation(); // adopt our own write
        break;
    }
    case CMD_VIEW_PANES:
        g_single = !g_single;
        uconf_set(FILES_CONF, "panes", g_single ? "1" : "2");
        g_seen_generation = sys_fs_generation();
        break;
    case CMD_VIEW_TREE:
        g_tree_on = !g_tree_on;
        if (g_tree_on) {
            tree_rebuild();
            tree_select_path(uui_fileview_dir(active()));
        }
        uconf_set(FILES_CONF, "tree", g_tree_on ? "1" : "0");
        g_seen_generation = sys_fs_generation();
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
    if (g_modal != MODAL_NONE) {
        if (id == ID_MENU) (void)uui_menubar_take_code(&g_menu);
        if (id == ID_CTX) (void)uui_menubar_take_code(&g_ctx);
        if (id == ID_TOOLBAR) (void)uui_toolbar_take_code(&g_toolbar);
        return;
    }
    if (id == ID_MENU) {
        // The commit is PARKED in the widget (ui/uui_menubar.h): the
        // ops release slot can only say "changed", not which item.
        int code = uui_menubar_take_code(&g_menu);
        if (code >= 0) do_command(a, code);
        return;
    }
    if (id == ID_TOOLBAR) {
        int code = uui_toolbar_take_code(&g_toolbar);
        if (code >= 0) do_command(a, code);
        return;
    }
    if (id == ID_DIALOG) {
        int code = uui_dialog_take_code(&g_dialog);
        if (code > 0) answer_dialog(code);
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
            uui_fileview_set_dir(active(), g_tree_path[nid]);
        // AFTER the navigation, which lists a directory: the window is
        // meant to measure the gap between the user's two clicks, not
        // the work the first one caused.
        g_tree_click_tick = sys_ticks();
        refresh_status();
        uapp_redraw(a);
        return;
    }
    if (id == ID_CANCEL) {
        fm_job_cancel();
        set_note("cancelling");
        uapp_redraw(a);
        return;
    }
    do_command(a, id); // the function-key buttons carry their command as their id
}

// A SECONDARY CLICK ARMS THE CONTEXT MENU. It opens on the release --
// uui_menubar.h says why in full, and it is not a style choice.
static void on_press(struct uapp *a, int x, int y, unsigned buttons) {
    // A press anywhere but the field being edited ends the edit,
    // keeping the path as it was -- Dolphin's and Explorer's rule, and
    // the only one under which a click on a row cannot ALSO navigate.
    if (g_addr_edit >= 0 && !uui_textbox_hit(&g_addr[g_addr_edit], x, y)) {
        addr_end_edit(0);
        uapp_redraw(a);
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
    if (modal_key(a, key)) return;

    // THE ADDRESS BAR BEING EDITED takes every key: Enter navigates,
    // Esc puts the path back, the rest is typing.
    if (g_addr_edit >= 0) {
        if (key == '\n' || key == '\r') addr_end_edit(1);
        else if (key == 0x1B) addr_end_edit(0);
        else uui_textbox_key_mods(&g_addr[g_addr_edit], key, mods);
        uapp_redraw(a);
        return;
    }
    if (key == 0x0C) {   // Ctrl-L: edit the active pane's path, as in every file manager
        addr_begin_edit(g_active);
        uapp_redraw(a);
        return;
    }

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

    int code = 0;
    // AN OPEN POPUP TAKES THE KEY, wherever the app thinks it is
    // (CLAUDE.md). Asked before the bar, which is closed whenever this
    // one is open.
    if (uui_menubar_key(&g_ctx, key, &code)) {
        if (code > 0) do_command(a, code);
        else uapp_redraw(a);
        return;
    }
    if (uui_menubar_key(&g_menu, key, &code)) {
        if (code > 0) do_command(a, code);
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
    case KEY_DELETE: do_command(a, CMD_DELETE);  return;
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
    if (uui_fileview_key(active(), key)) {
        refresh_status();
        uapp_redraw(a);
    }
}

static int on_tick(struct uapp *a) {
    (void)a;
    int changed = poll_job();
    if (uui_toolbar_tick(&g_toolbar)) changed = 1;
    if (thumb_tick()) changed = 1;

    // Not under a rubber band: a reload clears the marks the band is
    // mid-way through choosing (the desktop's desktop_drag_active() rule).
    if (uui_fileview_band_active(&g_pane[0]) ||
        uui_fileview_band_active(&g_pane[1]))
        return changed;

    unsigned long long gen = sys_fs_generation();
    if (gen != g_seen_generation) {
        g_seen_generation = gen;
        reload_panes();
        if (g_tree_on) tree_rebuild(); // a dir can have appeared or gone
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
    (void)ctx;
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
    refresh_dim();   // the reload cleared the bits; see refresh_dim()
    // Written on every change rather than at exit, because a window
    // manager can Force Quit this process and an exit-time save is a
    // save that does not happen. Two keys, so the whole document is
    // rewritten twice per navigation -- 512 bytes, and the alternative
    // is a dirty flag that has to be right.
    uconf_set(FILES_CONF, i ? "right" : "left", dir);

    // ADOPT THE GENERATION OUR OWN WRITE JUST PRODUCED. Without this the
    // app watches the filesystem, changes it, and then reacts to itself:
    // every navigation wrote this file, the write bumped
    // SYS_FS_GENERATION, and the next tick read that as "somebody
    // changed the disk" and reloaded BOTH panes -- a second full repaint
    // half a second after the first, which is visible as a flicker on
    // every single directory change. Measured at 2 frames per
    // navigation before, 1 after.
    //
    // Every watcher needs this: it is why inotify consumers track their
    // own writes and why a settings daemon ignores the change it just
    // made. The cost is bounded and worth stating -- a write by ANOTHER
    // process landing in the same instant is adopted too and its refresh
    // is skipped, until the next change moves the counter again.
    g_seen_generation = sys_fs_generation();

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
    char human[24], when[32];
    human_size(human, sizeof human, st.size);
    rtc_format_iso(when, sizeof when, &st.modified, 0);
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
        { "Overwrite",     DLG_OVERWRITE },
        { "Overwrite all", DLG_OVERWRITE_ALL },
        { "Skip",          DLG_SKIP },
        { "Skip all",      DLG_SKIP_ALL },
        { "Rename",        DLG_RENAME },
        { "Cancel",        DLG_CANCEL },
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
    if (a0 == POST_THUMB) return thumb_posted();
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

void reload_pane(struct uui_fileview *fv) {
    uui_fileview_reload(fv);
    refresh_dim();
}

void reload_panes(void) {
    uui_fileview_reload(&g_pane[0]);
    uui_fileview_reload(&g_pane[1]);
    refresh_dim();
}

static void on_clipboard(struct uapp *a, int op, unsigned serial) {
    (void)serial;
    g_clip_op = op;
    refresh_dim();
    uapp_redraw(a);   // Paste greys and ungreys with it
}

static void on_open(struct uapp *a) {
    g_app = a;
    layout_all(uapp_width(a), uapp_height(a));
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
    char saved_left[PATH_MAX_LEN], saved_right[PATH_MAX_LEN];
    if (!uconf_get(FILES_CONF, "left", saved_left, sizeof saved_left)) saved_left[0] = '\0';
    if (!uconf_get(FILES_CONF, "right", saved_right, sizeof saved_right)) saved_right[0] = '\0';

    const char *left = (argc > 1 && argv[1][0]) ? argv[1]
                        : (saved_left[0] ? saved_left : "/");
    const char *right = (argc > 2 && argv[2][0]) ? argv[2]
                         : (saved_right[0] ? saved_right : "/");

    uui_menubar_init(&g_menu, menu_items,
                      (int)(sizeof menu_items / sizeof menu_items[0]));
    g_menu.item_flags = menu_item_flags;
    // NO ITEMS: a context menu has no bar strip, so Left/Right have no
    // titles to walk out into (ui/uui_menubar.h).
    uui_menubar_init(&g_ctx, 0, 0);
    g_ctx.item_flags = menu_item_flags;
    uui_toolbar_init(&g_toolbar, toolbar_items,
                      (int)(sizeof toolbar_items / sizeof toolbar_items[0]));
    g_toolbar.item_flags = menu_item_flags; // ONE state source -- see uui_toolbar.h
    uui_button_init(&g_cancel_btn, 0, 0, 0, 0, "Cancel",
                     UTHEME_BUTTON_BG, UTHEME_TEXT, ID_CANCEL);
    uui_statusbar_init(&g_status);
    g_status.panes[0].text = g_stat_dir;
    g_status.panes[0].chars = 0;
    g_status.panes[1].text = g_stat_items;
    g_status.panes[1].chars = 20;
    g_status.panes[2].text = g_stat_note;
    g_status.panes[2].chars = 18;
    g_status.count = 3;

    for (int i = 0; i < 2; i++) {
        uui_fileview_init(&g_pane[i], 0, 0, 100, 100,
                           i ? g_right_entries : g_left_entries, PANE_FILES);
        g_pane[i].on_open = on_pane_open;
        g_pane[i].ctx = (void *)(intptr_t)i;
        uui_fileview_set_thumb(&g_pane[i], pane_thumb, 0);
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
    if (!uconf_get(FILES_CONF, "left_view", opt, sizeof opt) || strcmp(opt, "details") != 0)
        uui_fileview_set_mode(&g_pane[0], UUI_FILEVIEW_ICONS);
    if (!uconf_get(FILES_CONF, "right_view", opt, sizeof opt) || strcmp(opt, "details") != 0)
        uui_fileview_set_mode(&g_pane[1], UUI_FILEVIEW_ICONS);
    for (int i = 0; i < 2; i++) uui_textbox_init(&g_addr[i], "/");

    uui_splitter_init(&g_tree_split, 1, TREE_SPLIT_DEFAULT);
    uui_splitter_init(&g_pane_split, 1, PANE_SPLIT_DEFAULT);
    if (uconf_get(FILES_CONF, "tree_split", opt, sizeof opt))
        uui_splitter_set_frac(&g_tree_split, atoi(opt));
    if (uconf_get(FILES_CONF, "pane_split", opt, sizeof opt))
        uui_splitter_set_frac(&g_pane_split, atoi(opt));

    tree_init();

    uui_fileview_set_dir(&g_pane[0], left);
    uui_fileview_set_dir(&g_pane[1], right);
    // Hooked up AFTER the opening directories are set, so starting the
    // app with an explicit argument does not silently rewrite the
    // remembered pair -- an argument is a statement about this launch.
    for (int i = 0; i < 2; i++) g_pane[i].on_dir_changed = on_pane_dir;
    if (g_tree_on) {
        tree_rebuild();
        tree_select_path(uui_fileview_dir(active()));
    }
    // ASKED ONCE AT STARTUP: the broadcast only fires on a CHANGE, so an
    // app that opens after somebody else copied would show Paste greyed
    // until the next one.
    {
        static struct uclip c;   // see refresh_dim() on why it is static
        uclip_load(&c);
        g_clip_op = uclip_op(&c);
    }
    set_note("F5 copy  F6 move  F7 new  F8 delete");

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
