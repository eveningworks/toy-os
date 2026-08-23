#ifndef UUI_FILEVIEW_H
#define UUI_FILEVIEW_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_table.h"
#include "syscall_abi.h"  // struct sys_dirent
#include "lib/dirsort.h"  // enum dirsort_key, dirsort_cmp()

// --- fileview: a directory, as a widget -------------------------------
//
// THE FOURTH IMPLEMENTATION IS WHY THIS EXISTS. Before it, three places
// listed a directory and every one of them was written from scratch:
// the WM's file picker (userland/wm/file_picker.c), Notepad's Open/Save
// dialog, and Image Viewer's sidebar. All three did the same four
// things -- sys_listdir(), dirsort(), a synthetic ".." row, and
// descend-on-activate -- and a file manager would have been the fourth.
// No real toolkit ships four: Win32 has one SysListView32, Qt one
// QFileSystemModel, GTK one GtkFileChooser.
//
// **It COMPOSES uui_table rather than reimplementing it.** The table
// already owns rows, columns, scrolling, the clickable sorting header
// and keyboard motion. What is added here is everything specific to
// directories, which is the only part those three copies had in common.
//
// **The caller owns the entry storage.** Toykit has no allocator, and a
// full listing is SYS_LISTDIR_MAX x sizeof(struct sys_dirent) = 20 KB --
// too big for a ring-3 stack frame (2 KiB budget) and wrong to bake
// into the struct, since a sidebar wants 64 entries and a file manager
// wants 256. So init() takes an array and its capacity, the same
// ownership rule uui_listbox's items and uui_table's columns follow.
//
// **Filtering is a CALLBACK, not an extension list.** Image Viewer
// decides what an image is by probing magic bytes, deliberately, so
// that a JPEG named .dat is listed and a text file named photo.jpg is
// not. A widget that filtered by extension would take that away.

#define UUI_FILEVIEW_PATH_MAX 64 // FS_PATH_MAX (kernel/include/api/fs.h)

// ~300ms at the PIT's 100 Hz, the same threshold the desktop's icons and
// the file picker each defined for themselves before this widget was the
// one place to define it.
#define UUI_FILEVIEW_DOUBLE_CLICK_TICKS 30

enum uui_fileview_mode {
    // Names only, no header -- a narrow sidebar or a picker's list.
    UUI_FILEVIEW_LIST,
    // Name / Size / Modified, with the sortable header. LVS_REPORT vs
    // LVS_LIST on Win32: one widget, and the difference is columns.
    UUI_FILEVIEW_DETAILS,
};

// Return 1 to list this entry, 0 to hide it. Directories are offered
// too -- a filter that hides them makes a view nobody can navigate, so
// most callers accept every `e->is_dir`.
typedef int (*uui_fileview_filter_fn)(void *ctx, const char *dir,
                                       const struct sys_dirent *e);

struct uui_fileview {
    struct uui_table table; // the whole visual/input mechanism

    // Caller-owned listing storage; see the header comment.
    struct sys_dirent *entries;
    int cap;
    int count;      // entries actually held, EXCLUDING the ".." row
    int has_up;     // 1 when a synthetic ".." leads the rows
    int truncated;  // the listing filled `cap` -- there may be more
    int failed;     // the directory could not be read AT ALL, which is
                     // not the same as it being empty (wm_fs.h's rule)

    char dir[UUI_FILEVIEW_PATH_MAX];

    enum uui_fileview_mode mode;

    // Can the user LEAVE this directory? 1 (the default) shows the ".."
    // row and descends into a directory when one is activated. 0 pins
    // the view to one directory -- what Image Viewer's sidebar wants,
    // since it filters directories out entirely and a ".." leading into
    // a view with no way back would be a control that strands you.
    int navigable;

    uui_fileview_filter_fn filter;
    void *filter_ctx;

    // --- marks ------------------------------------------------------
    //
    // A SET of files the next operation acts on, which is what Insert
    // (or Space) toggles in every commander -- Midnight Commander,
    // Total Commander and Far all have it, and a file manager that can
    // only ever act on one file is a shell with pictures.
    //
    // A BITMAP rather than a flag per entry, because the entries are
    // the CALLER's array (see the header comment) and this widget must
    // not need a second one sized to match. Sized in ROWS, which is
    // SYS_LISTDIR_MAX entries PLUS the synthetic ".." -- one more than
    // the listing, and the off-by-one that would put the last row's
    // mark one word past the end.
    //
    // CLEARED BY EVERY RELOAD, deliberately: a mark names a ROW, the
    // rows are re-read from the filesystem, and a mark surviving a
    // reload would point at whatever landed in that slot -- which is
    // how a delete ends up pointed at the wrong file. A caller acting
    // on marks must therefore SNAPSHOT the paths before it starts.
    uint32_t marks[(SYS_LISTDIR_MAX + 1 + 31) / 32];
    int mark_count;
    uint32_t mark_bg;

    // Double-click state, in sys_ticks(). OWNED.
    int last_click_row;
    unsigned long last_click_tick;

    // --- what the app hears about. All optional. --------------------
    //
    // A DIRECTORY IS THE WIDGET'S BUSINESS AND A FILE IS THE APP'S:
    // activating a directory descends into it here and reports
    // `on_dir_changed`, while activating a file only ever reports
    // `on_open`. That split is what let three callers stop writing
    // navigation code and keep their own idea of what opening means.
    void (*on_select)(void *ctx, const char *path, int is_dir);
    void (*on_open)(void *ctx, const char *path);
    void (*on_dir_changed)(void *ctx, const char *dir);
    void *ctx;
};

// `storage`/`cap` are the caller's listing array. The view starts empty:
// call uui_fileview_set_dir() to point it somewhere.
void uui_fileview_init(struct uui_fileview *fv, int x, int y, int w, int h,
                        struct sys_dirent *storage, int cap);

void uui_fileview_set_mode(struct uui_fileview *fv, enum uui_fileview_mode mode);
// See `navigable` above. Takes effect on the next reload.
void uui_fileview_set_navigable(struct uui_fileview *fv, int navigable);

void uui_fileview_set_filter(struct uui_fileview *fv,
                              uui_fileview_filter_fn fn, void *ctx);

// Lists `dir` and shows it. Returns 1 on success; on failure the view
// shows the directory as EMPTY and `failed` is set -- a caller that
// treats the two the same reports a missing directory as an empty one.
int  uui_fileview_set_dir(struct uui_fileview *fv, const char *dir);

// Re-lists the CURRENT directory, keeping the selection on the same
// NAME where it still exists. That is what makes a live refresh
// (SYS_FS_GENERATION) invisible: without it every reload would fling
// the selection back to the first row.
int  uui_fileview_reload(struct uui_fileview *fv);

// Up one level. No-op at the root. Selects the directory just left, the
// way every file manager does, so Backspace-then-Enter returns.
int  uui_fileview_up(struct uui_fileview *fv);

const char *uui_fileview_dir(const struct uui_fileview *fv);

// The selected row's full path, written into `out`. Returns 1, or 0
// when nothing is selected or the ".." row is -- ".." is a navigation
// control, not a file, and handing back its path is how a delete key
// ends up pointed at the parent directory.
int  uui_fileview_selected_path(const struct uui_fileview *fv, char *out, int cap);
int  uui_fileview_selected_is_dir(const struct uui_fileview *fv);
const char *uui_fileview_selected_name(const struct uui_fileview *fv);

// Selects the row with this name, if it is there. Returns 1 if found.
int  uui_fileview_select_name(struct uui_fileview *fv, const char *name);

// --- marks; see the struct ------------------------------------------
//
// Toggling ".." or an out-of-range row does nothing: it is a navigation
// control, not a file.
int  uui_fileview_toggle_mark(struct uui_fileview *fv, int row);
int  uui_fileview_is_marked(const struct uui_fileview *fv, int row);
int  uui_fileview_mark_count(const struct uui_fileview *fv);
void uui_fileview_clear_marks(struct uui_fileview *fv);

// The nth marked row's full path (n counts from 0 in ROW order), or 0
// when there is no such mark. This is what a caller snapshots before
// acting -- see the struct's note on why marks do not survive a reload.
int  uui_fileview_marked_path(const struct uui_fileview *fv, int n, char *out, int cap);
int  uui_fileview_marked_is_dir(const struct uui_fileview *fv, int n);

// Acts on the selection: descends into a directory (reporting
// on_dir_changed), or reports on_open for a file. Returns 1 if anything
// happened.
int  uui_fileview_activate(struct uui_fileview *fv);

// Rows currently shown, INCLUDING ".." -- the table's row count.
int  uui_fileview_row_count(const struct uui_fileview *fv);
// Real entries, excluding "..", and their total size in bytes.
int  uui_fileview_count(const struct uui_fileview *fv);
unsigned long long uui_fileview_total_bytes(const struct uui_fileview *fv);
int  uui_fileview_truncated(const struct uui_fileview *fv);

// The direct interface, for a caller the toolkit router never sees --
// the WM's file picker is a screen-absolute modal and draws itself.
// Same arrangement uui_listbox and uui_table already have.
void uui_fileview_set_geometry(struct uui_fileview *fv, int x, int y, int w, int h);
void uui_fileview_draw(struct ugfx_surface *s, const struct uui_fileview *fv);
void uui_fileview_natural_size(const struct uui_fileview *fv, int *out_w, int *out_h);
int  uui_fileview_hit(const struct uui_fileview *fv, int cx, int cy);
int  uui_fileview_hover(struct uui_fileview *fv, int cx, int cy);
// A press: selects, and ACTIVATES on a double click. Returns 1 if
// anything changed (so the caller repaints).
int  uui_fileview_press(struct uui_fileview *fv, int cx, int cy);
int  uui_fileview_drag(struct uui_fileview *fv, int cx, int cy);
void uui_fileview_drag_end(struct uui_fileview *fv);
int  uui_fileview_wheel(struct uui_fileview *fv, int notches);
// Arrows/Home/End/PageUp/PageDown, plus Enter (activate), Backspace
// (up) and Insert/Space (toggle a mark and step down). Returns 1 if the
// view consumed the key.
int  uui_fileview_key(struct uui_fileview *fv, int key);

struct uui_widget_ops;
extern const struct uui_widget_ops uui_fileview_ops;

#endif
