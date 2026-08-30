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
//   fm_modal.c   Rename / New folder / the delete confirmation

#define PATH_MAX_LEN 64          // FS_PATH_MAX
#define PANE_FILES  SYS_LISTDIR_MAX

// The Properties window is a PROCESS, not a dialog in this one. See
// userland/gui/apps/properties.c.
#define PROPERTIES_EXEC "/bin/wm/apps/properties"

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

// The commands, shared by the menu bar, the toolbar, the context menu
// and the function keys -- one code per act, so those four cannot
// disagree about what a command does.
enum {
    CMD_COPY = 1, CMD_MOVE, CMD_MKDIR, CMD_RENAME, CMD_DELETE,
    CMD_REFRESH, CMD_SWAP, CMD_EXIT,
    CMD_VIEW_DETAILS, CMD_VIEW_ICONS, CMD_VIEW_PANES, CMD_VIEW_TREE,
    CMD_UP, CMD_OPEN, CMD_PROPERTIES,
    CMD_CLIP_COPY, CMD_CLIP_CUT, CMD_CLIP_PASTE,
};

// --- the app's own state (files.c) -----------------------------------

extern struct uui_fileview g_pane[2];
extern int g_active;             // 0 = left, 1 = right
extern struct uui_splitter g_tree_split;   // tree | panes
extern struct uui_splitter g_pane_split;   // left | right
extern int g_single;             // one pane shown, not two
extern int g_tree_on;
extern struct uui_menubar g_menu;
extern struct uui_menubar g_ctx; // the context menu -- no bar of its own
struct uui_dialog;
extern struct uui_dialog g_dialog;
extern struct uui_toolbar g_toolbar;
extern struct uui_statusbar g_status;
extern struct uui_button g_cancel_btn;
extern char g_stat_dir[PATH_MAX_LEN + 8];
extern char g_stat_items[48];
extern char g_stat_note[64];

// The filesystem generation this app has already reacted to. An app
// that both watches the filesystem and writes to it must adopt the
// generation its OWN write produced, or it reacts to itself -- see
// on_pane_dir() for the flicker that cost.
extern unsigned long long g_seen_generation;

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
void tree_rebuild(void);
void tree_toggle(void *ctx, int id, int expand);

// --- thumbnails (fm_thumbs.c) ----------------------------------------

// uui_fileview's thumb callback: a LOOKUP, never a decode (see
// ui/uui_fileview.h). The decoding happens on the tick.
const struct uimg *pane_thumb(void *ctx, const char *dir,
                               const struct sys_dirent *e, int px);
int thumb_tick(void);

// --- operations (fm_jobs.c) ------------------------------------------

// How many rows the next operation would act on -- the marks, or the
// selection when nothing is marked.
int operand_count(void);
extern int g_job_at, g_job_count;

void do_copy(void);
void do_move(void);
void do_delete(void);
void commit_delete(void);
void commit_mkdir(const char *name);
void commit_rename(const char *name);
int  poll_job(void);

// --- the worker, and the question it can ask --------------------------
//
// The operation runs on a THREAD (fm_jobs.c). It touches nothing in
// Toykit: it records what it is doing and posts, and every decision is
// made here on the main thread.
#define POST_DONE     1
#define POST_CONFLICT 2

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

enum modal_kind { MODAL_NONE, MODAL_CONFIRM, MODAL_PROMPT };
extern enum modal_kind g_modal;

void open_prompt(int cmd, const char *title, const char *initial);
void open_confirm(int cmd, const char *title, const char *body);
int  modal_key(struct uapp *a, int key);
void draw_modal(struct ugfx_surface *s);

// --- layout and drawing (fm_view.c) ----------------------------------

void layout_all(int cw, int ch);
void log_layout(void);
void on_draw(struct uapp *a, struct uapp_draw *d);
void on_draw_over(struct uapp *a, struct uapp_draw *d);

// Which VISIBLE pane holds this point, or -1.
int pane_at(int x, int y);

#endif // FM_INTERNAL_H
