#ifndef FM_INTERNAL_H
#define FM_INTERNAL_H

#include <stdint.h>
#include "rt/sys.h"
#include "ui/ugfx.h"
#include "ui/uapp.h"
#include "ui/uui_fileview.h"
#include "ui/uui_tree.h"
#include "ui/uui_toolbar.h"
#include "ui/uui_menubar.h"
#include "ui/uui_statusbar.h"
#include "ui/uui_splitter.h"
#include "ui/uui_textbox.h"
#include "ui/uui_widget.h"
#include "ui/uui_pathbar.h"
#include "ui/uui_places.h"
#include "ui/uui_button.h"
#include "lib/uimg.h"

// The File Manager's own header, shared by the files it is split across
// and by nothing else.
//
// **THESE ARE NOT INDEPENDENT MODULES.** They are one app, one event
// loop and one pile of mutable state, organised by concern so that
// finding the right part of it is quick -- the same arrangement
// `userland/wm/` has, and this header is `wm_internal.h`'s counterpart.
// The units share the state below directly rather than through
// accessors, which is honest about what they are.
//
// Where things live:
//   files.c      the app -- the menus, the commands, input, main()
//   fm_view.c    layout, drawing, and the geometry a test asserts on
//   fm_jobs.c    the operation queue and its spawned children
//   fm_tree.c    the lazy folder tree
//   fm_thumbs.c  the lazy thumbnail cache
//   fm_modal.c   Rename / New folder (the two prompts with a text field)
//   fm_details.c the details pane: a preview and the selection's facts
//   fm_trash.c   the Recycle Bin's folder (trash:/) and its verbs

#define PATH_MAX_LEN 256         // NOT FS_PATH_MAX -- this app's own
                                 // buffers; the shared widgets it uses
                                 // (uui_fileview, uui_places, ufileop,
                                 // uopen) carry the same bound.
#define PANE_FILES  SYS_LISTDIR_MAX

// The Properties window is a PROCESS, not a dialog in this one. See
// userland/gui/apps/properties.c.
#define PROPERTIES_EXEC "/bin/wm/apps/properties"
// "Edit in Notepad" on the context menu. Named here rather than
// resolved through Handles=, because the item exists for the files
// Notepad does NOT claim -- a README, a .desktop -- and is what "open
// this as text anyway" means.
#define NOTEPAD_EXEC "/bin/wm/apps/notepad"

// The router's ids. A widget reports its own (ui/uui_widget.h).
#define ID_LEFT  1
#define ID_RIGHT 2
#define ID_TREE  3
#define ID_MENU  4
#define ID_TOOLBAR 5
#define ID_TREE_SPLIT 6
#define ID_PANE_SPLIT 7
#define ID_CTX   8
#define ID_DIALOG 9
#define ID_CANCEL 10
#define ID_ADDR_L 11
#define ID_ADDR_R 12
#define ID_NAV     13   // Back / Forward / Up / Refresh
#define ID_PATH    14   // the breadcrumb
#define ID_SEARCH  15
#define ID_PLACES  16
#define ID_VIEWBAR 17   // the status bar's view switch
#define ID_DP_OPEN  18  // the details pane's buttons
#define ID_DP_PROPS 19
#define ID_TOAST    20  // the undo toast's button
#define ID_TOAST_X  21  // ...and its dismiss

// The commands, shared by the menu bar, the toolbar, the context menu
// and the function keys -- one code per act, so those four cannot
// disagree about what a command does.
enum {
    CMD_COPY = 1, CMD_MOVE, CMD_MKDIR, CMD_RENAME, CMD_DELETE,
    CMD_REFRESH, CMD_SWAP, CMD_EXIT,
    CMD_VIEW_DETAILS, CMD_VIEW_ICONS, CMD_VIEW_PANES, CMD_VIEW_TREE,
    CMD_UP, CMD_OPEN, CMD_PROPERTIES,
    CMD_CLIP_COPY, CMD_CLIP_CUT, CMD_CLIP_PASTE,
    CMD_EDIT,
    CMD_BACK, CMD_FORWARD,
    // The command bar's drop-downs (they open a menu, they do nothing
    // themselves) and what is in them.
    CMD_MENU_NEW, CMD_MENU_SORT, CMD_MENU_VIEW, CMD_MENU_MORE,
    CMD_NEW_FILE,
    CMD_SORT_NAME, CMD_SORT_MODIFIED, CMD_SORT_TYPE, CMD_SORT_SIZE,
    CMD_SORT_ASC, CMD_SORT_DESC,
    CMD_VIEW_LARGE, CMD_VIEW_DPANE,
    CMD_SELECT_ALL, CMD_OPTIONS,
    // The Recycle Bin (fm_trash.c). DELETE moves to it; DELETE_FOREVER
    // (Shift+Delete, and Delete inside the bin) does not.
    CMD_DELETE_FOREVER, CMD_RESTORE, CMD_RESTORE_ALL, CMD_EMPTY_BIN,
    // The worker's own operations, never a user's command.
    CMD_TRASH, CMD_PURGE,
    // Undo and Redo (fm_undo.c): commands, and CMD_UNDO the worker's op.
    CMD_UNDO, CMD_REDO, CMD_TOAST,
};

// The Recycle Bin's folder, as uui_fileview and the breadcrumb see it --
// KIO's trash:/.
#define FM_BIN "trash:/"

// The dialog's answers. ONE widget serves both questions the app asks
// (a conflict, a delete), so `g_dialog_kind` says which one is up and
// the codes never overlap.
enum {
    DLG_OVERWRITE = 1, DLG_OVERWRITE_ALL, DLG_SKIP, DLG_SKIP_ALL,
    DLG_RENAME, DLG_RENAME_ALL, DLG_CANCEL,
    DLG_DELETE, DLG_OK,
};
enum dialog_kind { DIALOG_NONE, DIALOG_CONFLICT, DIALOG_DELETE, DIALOG_PROMPT };
extern enum dialog_kind g_dialog_kind;

// --- navigation history, per pane (fm_history.c) ---------------------
//
// EVERY directory change goes through fm_goto()/fm_history_record(), or
// the history silently stops matching where the panes actually are --
// which is worse than having none, because Back then goes somewhere the
// user has never been.
void fm_history_record(int pane, const char *dir);
// Record wherever the panes are NOW -- the backstop for a navigation the
// widget made by itself (a double click). Call it per frame; see
// fm_history.c for why that is cheaper than keeping a list of call
// sites complete.
void fm_history_sync(void);
int  fm_history_back(int pane);      // 1 moved, 0 nowhere to go, -1 all gone
int  fm_history_forward(int pane);
int  fm_history_can_back(int pane);
int  fm_history_can_forward(int pane);

// The choke point: set a pane's directory AND record it. Returns what
// uui_fileview_set_dir() returned.
int  fm_goto(int pane, const char *dir);
int  fm_goto_up(int pane);        // the fileview's own "up"
int  fm_goto_activate(int pane);  // ...and its "open what is selected"

// --- the app's own state (files.c) -----------------------------------

extern struct uui_fileview g_pane[2];
extern int g_active;             // 0 = left, 1 = right
extern struct uui_splitter g_tree_split;   // tree | panes
extern struct uui_splitter g_pane_split;   // left | right
extern int g_single;             // one pane shown, not two
extern int g_tree_on;
extern struct uui_menubar g_ctx; // the context menu -- no bar of its own

// THIS APP, for the code that has no `struct uapp *` of its own: the
// fileview's callbacks are the widget's and carry only `ctx`, and the
// worker thread has no call stack from the event loop at all. Set once
// in on_open().
extern struct uapp *g_app;
extern int g_ctx_rows;           // rows in the open popup, separators included
struct uui_dialog;
extern struct uui_dialog g_dialog;
extern struct uui_toolbar g_toolbar;
extern struct uui_statusbar g_status;
extern struct uui_button g_cancel_btn;
extern struct uui_button g_toast_btn, g_toast_x;

// THE ADDRESS BARS. Each pane's path strip is a text field: read-only
// looking until it is clicked (or Ctrl+L), then edited in place, Enter
// navigates and Esc puts the path back -- Dolphin's split view. Only
// one can be editing, `g_addr_edit` (-1 for none).
extern struct uui_textbox g_addr[2];
extern int g_addr_edit;
void addr_begin_edit(int pane);
void addr_end_edit(int commit);
// THE WINDOW'S CHROME, top to bottom: Back/Forward/Up beside the
// breadcrumb and the search box; the command bar; the places column,
// the panes and the details pane; the status bar with the view switch.
// Windows 11 Explorer's arrangement (docs/decisions.md has why).
extern struct uui_toolbar g_nav;
extern struct uui_pathbar g_path;
extern struct uui_textbox g_search;
extern int g_search_on;          // the search box has the keyboard
extern struct uui_places g_places;
extern struct uui_toolbar g_viewbar;
extern int g_dpane;              // the details pane is shown
extern struct uui_button g_dp_open, g_dp_props;
void search_apply(void);         // re-filter the active pane by g_search
void search_clear(void);
void path_sync(void);            // breadcrumb, places and placeholder follow the active pane

// --- Options (fm_options.c) -------------------------------------------------
//
// The File Manager's own preferences, in FILES_CONF beside the rest of its
// state, edited in a window of its own (See more > Options). Explorer's
// Folder Options and Dolphin's Configure dialog are the same idea.
enum { FM_START_LAST, FM_START_HOME, FM_START_ROOT };
enum { FM_VIEW_ICONS, FM_VIEW_LARGE, FM_VIEW_DETAILS };
struct fm_options {
    int start;           // FM_START_*
    int single_click;    // a click opens, not a double click
    int view;            // FM_VIEW_* -- what a new window shows
    int thumbs;          // pictures show themselves
    int hidden;          // names that start with a dot are listed
    int extensions;      // "dusk.jpg", not "dusk"
    int rename_dialog;   // F2 asks in a dialog rather than editing in place
    int confirm_delete;  // Delete asks first
};
extern struct fm_options g_opt;
void options_load(void);           // from FILES_CONF, defaults for what is missing
void options_apply(void);          // to both panes, now
void options_open(struct uapp *a); // the window

// --- the details pane (fm_details.c) --------------------------------------
int  details_width(int cw);
void details_layout(int x, int y, int w, int h);
void details_draw(struct ugfx_surface *s);

extern char g_stat_dir[PATH_MAX_LEN + 8];
extern char g_stat_items[48];
extern char g_stat_note[64];

// WHAT HAS CHANGED, PER FOLDER (on_tick). The volume-wide
// SYS_FS_GENERATION is only the cheap first gate; each pane and the
// tree then compare SYS_FS_GENERATION_OF for the folders they show, so
// a write elsewhere -- this app's own config, its thumbnail cache --
// reloads nothing. A pane's counter is sampled whenever it is listed
// (pane_listed()), which is what keeps the app from reacting to the
// listing it just made.
extern unsigned long long g_seen_generation;
void pane_listed(int pane);

// The widget tree, declared by the app and consumed by the router. The
// indices are here because the layout hides panes and the input code
// hit-tests them, and both need to name the same slots.
extern struct uui_item g_widgets[];
extern const int g_widget_count;

// BY ID, NEVER BY POSITION. These were `g_widget_count - 3` and friends,
// which is a number a different file has to keep true (CLAUDE.md):
// appending one widget to the array silently shifted all three, so
// hiding the tree hid the tree SPLITTER and hiding the pane splitter hid
// the CONTEXT MENU -- a right-click that stopped working in single-pane
// view, and a tree drawn over the menu bar. An id is a name.
struct uui_item *widget_by_id(int id);

struct uui_fileview *active(void);
struct uui_fileview *other(void);
void set_note(const char *s);
void refresh_dim(void);
void refresh_status(void);

// RELOAD THROUGH THESE, never uui_fileview_reload() directly. A reload
// drops the dim bits (ui/uui_fileview.h) and re-applying them is what
// three of the four call sites forgot, so a staged cut stopped being
// drawn the moment anything touched the filesystem.
void reload_pane(struct uui_fileview *fv);
void reload_panes(void);
void do_command(struct uapp *a, int code);

// A divider's position, written when a drag ENDS rather than per motion:
// a drag is hundreds of events and every one would be a whole-file
// rewrite.
void save_split(const char *key, const struct uui_splitter *sp);

#define FILES_CONF "/etc/files.conf"

// --- the folder tree (fm_tree.c) -------------------------------------

#define TREE_MAX 96

extern struct uui_tree g_tree;
extern char g_tree_path[TREE_MAX][PATH_MAX_LEN];
extern int g_tree_count;

void tree_init(void);
void tree_select_path(const char *path);
void tree_reveal_path(const char *path);   // ancestors opened, then selected
void tree_rebuild(void);
int  tree_poll(void);           // rebuild if an open folder changed; 1 if it did
void tree_refresh_meters(void); // the volumes' notes and meters, from g_places
void tree_toggle(void *ctx, int id, int expand);

// --- thumbnails (fm_thumbs.c) ----------------------------------------

// uui_fileview's thumb callback: a LOOKUP, never a decode (see
// ui/uui_fileview.h). The decoding happens on a worker thread; this
// enqueues and posts POST_THUMB.
const struct uimg *pane_thumb(void *ctx, const char *dir,
                               const struct sys_dirent *e, int px);
// POST_THUMB arrived -- a decode finished, or a lookup wants one
// started. 1 if a thumbnail became ready.
int thumb_posted(void);
// The tick's backstop, for a kick that found the post queue full.
int thumb_tick(void);

// --- operations (fm_jobs.c) ------------------------------------------

// How many rows the next operation would act on -- the marks, or the
// selection when nothing is marked.
int operand_count(void);
extern int g_job_at, g_job_count;

void do_copy(void);
void do_move(void);
// A DROP: `src`'s operands (its marks, else its selection) into `dest`,
// moved -- or copied when `copy` (Ctrl held, the toolkit's convention).
void do_drop(struct uui_fileview *src, const char *dest, int copy);
// A drop from ANOTHER window: the files are in the drag slot (lib/uclip.h).
void do_drop_extern(const char *dest, int copy);
void do_delete(int forever); // asks (or not, per Options); commit_delete() acts
void delete_picture(void);   // the delete card's picture, again (a thumbnail landed)
void commit_delete(void);
void do_empty_bin(void);  // asks, then purges every bin

// --- Undo and Redo (fm_undo.c) -----------------------------------------------
struct ufu_op;
extern struct uui_toast g_toast;
void undo_record_job(int op, const char *what, char (*paths)[PATH_MAX_LEN],
                     char (*results)[PATH_MAX_LEN], int n, const char *dest);
void undo_record(int kind, const char *a, const char *b);   // UFU_RENAME / UFU_CREATE
void undo_applied(int redo, int failures);
int  undo_can(int redo);
void do_undo(int redo);
void undo_toast_hide(void);
int  undo_toast_tick(void);        // 1 when it timed out and the window should repaint
void undo_toast_action(void);      // its button
void fm_job_undo(struct ufu_op *op, int redo);

// --- the Recycle Bin (fm_trash.c) -------------------------------------------
void bin_init(struct uui_fileview *fv);      // the trash:/ resolver, per pane
int  in_bin(const struct uui_fileview *fv);  // the pane is showing the bin
int  bin_count(void);
int  bin_item_flags(int code, unsigned *out);  // 1 when the bin decides `code`
unsigned long long bin_bytes(void);
void bin_restore(int all);
int  bin_paths(char (*out)[PATH_MAX_LEN], int cap);
const char *fm_watch_path(const char *dir);  // what a pane on `dir` polls
void commit_mkdir(const char *name);
void commit_newfile(const char *name);
void commit_rename(const char *name);
// The in-place rename's answer: the file called `from` in the active
// pane's folder becomes `to` (names, not paths).
void commit_rename_named(const char *from, const char *to);
int  poll_job(void);

// --- the worker, and the question it can ask --------------------------
//
// The operation runs on a THREAD (fm_jobs.c). It touches nothing in
// Toykit: it records what it is doing and posts, and every decision is
// made here on the main thread.
#define POST_DONE     1
#define POST_CONFLICT 2
#define POST_THUMB    3   // fm_thumbs.c's worker, and its wake-up

int  fm_job_running(void);
void fm_job_cancel(void);
void fm_job_status(char *out, int cap);
void fm_job_finished(void);

int         fm_conflict_pending(void);
const char *fm_conflict_src(void);
const char *fm_conflict_dst(void);
void        fm_conflict_answer(int decision, const char *rename);

// The clipboard's three verbs. A CUT MOVES NOTHING until the paste.
void clip_copy(void);
void clip_cut(void);
void clip_paste(void);

// --- the modal (fm_modal.c) ------------------------------------------

// Rename and New folder only: the delete confirmation is a
// uui_dialog now (files.c's open_delete_dialog), since a question with
// buttons is what that widget is.
enum modal_kind { MODAL_NONE, MODAL_PROMPT };
extern enum modal_kind g_modal;

void open_prompt(int cmd, const char *title, const char *initial);
void answer_prompt(int code);

// --- layout and drawing (fm_view.c) ----------------------------------

void layout_all(int cw, int ch);
void log_layout(void);
void on_draw(struct uapp *a, struct uapp_draw *d);
void on_draw_over(struct uapp *a, struct uapp_draw *d);

// Which VISIBLE pane holds this point, or -1.
int pane_at(int x, int y);

#endif // FM_INTERNAL_H
