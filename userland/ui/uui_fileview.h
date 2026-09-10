#ifndef UUI_FILEVIEW_H
#define UUI_FILEVIEW_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_table.h"
#include "syscall_abi.h"  // struct sys_dirent
#include "lib/dirsort.h"  // enum dirsort_key, dirsort_cmp()
#include "rubberband.h"   // the icons view's drag selection

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
// How many marked NAMES a reload carries across (see uui_fileview.c's
// reload); a larger set keeps its first this many.
#define UUI_FILEVIEW_KEEP_MARKS 64

enum uui_fileview_mode {
    // Names only, no header -- a narrow sidebar or a picker's list.
    UUI_FILEVIEW_LIST,
    // Name / Size / Modified, with the sortable header. LVS_REPORT vs
    // LVS_LIST on Win32: one widget, and the difference is columns.
    UUI_FILEVIEW_DETAILS,
    // An icon grid (LVS_ICON). The FIRST mode that is not the table:
    // drawing, hit-testing, scrolling and keyboard motion are the
    // widget's own here, over icon_grid.h's cell math -- but the
    // SELECTION, the marks, the sort order and every path accessor stay
    // the table's state, so a caller sees one widget whatever the mode.
    UUI_FILEVIEW_ICONS,
};

// Return 1 to list this entry, 0 to hide it. Directories are offered
// too -- a filter that hides them makes a view nobody can navigate, so
// most callers accept every `e->is_dir`.
typedef int (*uui_fileview_filter_fn)(void *ctx, const char *dir,
                                       const struct sys_dirent *e);

// The ICONS view's thumbnail source: the image to draw for this entry
// at (up to) `px` x `px`, or NULL for the generic file icon. Called
// during DRAW for visible cells only, so it must be a CACHE LOOKUP,
// never a decode -- a JPEG decoded in the draw path freezes the window
// for as long as the folder has photos, which is why every desktop
// thumbnails asynchronously. The caller owns the pixels and their
// lifetime; the widget only blits what it is handed this frame.
struct uimg;
typedef const struct uimg *(*uui_fileview_thumb_fn)(void *ctx, const char *dir,
                                                     const struct sys_dirent *e,
                                                     int px);

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

    uui_fileview_thumb_fn thumb; // NULL = every file gets the generic icon
    void *thumb_ctx;

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
    // RE-APPLIED BY NAME ON EVERY RELOAD: a mark names a ROW, the rows
    // are re-read from the filesystem, and a mark kept as a row would
    // point at whatever landed in that slot -- which is how a delete
    // ends up pointed at the wrong file. So reload() remembers the
    // names (up to UUI_FILEVIEW_KEEP_MARKS) and marks those rows again.
    // A caller acting on marks still SNAPSHOTS the paths before it
    // starts: the set can change under a long operation.
    uint32_t marks[(SYS_LISTDIR_MAX + 1 + 31) / 32];
    int mark_count;
    uint32_t mark_bg;

    // WHERE A SHIFT-RANGE STARTS. A source row, or -1. Set by a plain
    // or Ctrl click and by Insert/Space, exactly as Explorer, Dolphin
    // and every list view since the Finder: Shift extends from the last
    // row you touched WITHOUT Shift, not from the selection's edge.
    int anchor;

    // DIMMED rows -- present but pending, which is what a cut looks
    // like. A separate bitmap from `marks` because the two are
    // independent: a staged file can also be marked. Same lifetime rule
    // as the marks, and for the same reason -- see the note above.
    uint32_t dimmed[(SYS_LISTDIR_MAX + 1 + 31) / 32];
    int dim_count;

    // Double-click state, in sys_ticks(). OWNED.
    int last_click_row;
    unsigned long last_click_tick;
    // THE PRESSED ROW, kept until the release: a plain press on a row
    // that is already MARKED must not clear the set on the way down, or
    // a drag could never carry more than one file -- the clear is
    // deferred to a release that turned out to be a click (Explorer's
    // and Dolphin's rule). A source row, or -1.
    int press_row;
    int deferred_clear;
    // --- a drop target's state (ui/uui_widget.h's drag ops) ----------
    // -2 nothing hovering; -1 the directory itself; a source row that
    // is a directory. Drawn as an accent outline.
    int drop_row;
    char drop_target[UUI_FILEVIEW_PATH_MAX]; // resolved at the drop
    char drag_label[UUI_FILEVIEW_PATH_MAX];  // what the ghost says

    // --- icons mode only (see enum uui_fileview_mode) ---------------
    int icon_top;            // first visible grid ROW; OWNED
    // A drag from EMPTY SPACE sweeps a rubber band that MARKS what it
    // covers, in every view (rubberband.h's second caller); a plain
    // click there clears the selection, which is the band's own rule.
    struct rubberband band;  // OWNED

    // A 2px border in `active_mark_color`, drawn by the WIDGET as the
    // last step of its own draw -- which is what keeps it under a menu
    // popup. It lived in the File Manager's on_draw_over, which runs
    // AFTER the router's overlay pass, so the active-pane outline
    // painted straight across an open menu. Dolphin's split view marks
    // its active pane inside the view for the same z-order reason.
    int active_mark;
    uint32_t active_mark_color;

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
// See `active_mark` above. `on` 0 turns it off; the colour is kept.
void uui_fileview_set_active_mark(struct uui_fileview *fv, int on, uint32_t color);
// See `navigable` above. Takes effect on the next reload.
void uui_fileview_set_navigable(struct uui_fileview *fv, int navigable);

void uui_fileview_set_filter(struct uui_fileview *fv,
                              uui_fileview_filter_fn fn, void *ctx);
void uui_fileview_set_thumb(struct uui_fileview *fv,
                             uui_fileview_thumb_fn fn, void *ctx);

// The icons-mode cell rectangle for a VIEW position, content-relative.
// For tests and layout logs (docs/gui-guidelines.md: a test asks the
// app where things are). Returns 0 outside icons mode or the range.
int  uui_fileview_cell_rect(const struct uui_fileview *fv, int view,
                             int *x, int *y, int *w, int *h);

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

// --- dimmed rows; see the struct ------------------------------------
//
// CLEARED BY EVERY RELOAD, like the marks. An app that dims a set held
// somewhere else (the clipboard, say) re-applies it from the on_dir
// callback, by NAME -- a row index does not survive a re-read.
void uui_fileview_set_dimmed(struct uui_fileview *fv, int row, int on);
int  uui_fileview_is_dimmed(const struct uui_fileview *fv, int row);
void uui_fileview_clear_dimmed(struct uui_fileview *fv);

// The row a given name is on, or -1. What an app dimming a set of paths
// walks, since it holds names and this holds rows.
int  uui_fileview_row_of(const struct uui_fileview *fv, const char *name);

// A rubber-band drag is in progress. A caller that reloads
// on a timer must skip the reload while this is set: a reload clears the
// marks the band is mid-way through choosing (the desktop's
// desktop_drag_active() rule).
int  uui_fileview_band_active(const struct uui_fileview *fv);

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
// `mods` is the KEY_MOD_* bits held. Ctrl toggles the row's mark and
// moves the anchor; Shift marks the range from the anchor; neither
// clears the set, and a PLAIN click does -- the selection and the
// marked set are one thing here, as in Explorer and Dolphin.
int  uui_fileview_press(struct uui_fileview *fv, int cx, int cy, unsigned mods);

// Selects whatever row is at (cx, cy), and NOTHING else -- no double
// click, no rubber band, no scrollbar. What a SECONDARY click needs: a
// context menu acts on the selection, so right-clicking a row the user
// has not selected must move the selection there first (Explorer's and
// Dolphin's rule) without any chance of a second right-click counting
// as a double and opening the file. Returns 1 if the selection moved.
int  uui_fileview_select_at(struct uui_fileview *fv, int cx, int cy);
int  uui_fileview_drag(struct uui_fileview *fv, int cx, int cy);
void uui_fileview_drag_end(struct uui_fileview *fv);
int  uui_fileview_wheel(struct uui_fileview *fv, int notches);
// Arrows/Home/End/PageUp/PageDown, plus Enter (activate), Backspace
// (up) and Insert/Space (toggle a mark and step down). Returns 1 if the
// view consumed the key.
int  uui_fileview_key(struct uui_fileview *fv, int key);

// --- drag and drop (routed: ui/uui_route.h's third rule) -------------
//
// AS A SOURCE: a press on a row that moves past the threshold drags
// the marked set if the row is in it, else that row alone, with the
// selection made to match -- so the app's usual "marks, else the
// selection" operand rule names exactly what was carried.
// AS A TARGET: a directory row (".." included) is a drop onto it; a
// file row or empty space is a drop into this directory. Refused when
// that is the items' own directory or one of the items themselves.
// After a drop, the resolved directory:
const char *uui_fileview_drop_target(const struct uui_fileview *fv);

struct uui_widget_ops;
extern const struct uui_widget_ops uui_fileview_ops;

#endif
