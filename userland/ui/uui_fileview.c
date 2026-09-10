// fileview -- a directory listing, as a widget. See ui/uui_fileview.h.
#include "ui/uui_fileview.h"
#include "ui/uui_widget.h"
#include "rt/sys.h"     // sys_listdir(), sys_ticks()
#include "kpath.h"      // k_path_join/_dirname -- the KERNEL's, linked into ring 3
#include "lib/human.h"  // human_size()
#include "lib/icon_cache.h" // icon_get() -- the icons view's artwork
#include "icon_grid.h"      // cell math, shared with the desktop
#include <string.h>
#include <stdio.h>
#include "keyboard.h"   // KEY_* codes, as delivered by WIN_EV_KEY
#include "ui/uui_route.h" // UUI_NOWHERE -- a drag_over's leave

// Column indices in DETAILS mode. LIST mode declares only the first.
#define FV_COL_NAME 0
#define FV_COL_SIZE 1
#define FV_COL_TIME 2

// Widths in CHARACTERS, per docs/gui-guidelines.md -- font-derived, so
// the whole view reflows when the font size changes. Name stretches.
static const struct uui_table_column fv_cols_details[] = {
    { "Name",     0,  UUI_TALIGN_LEFT  },
    { "Size",     9,  UUI_TALIGN_RIGHT },
    { "Modified", 13, UUI_TALIGN_RIGHT },
};
static const struct uui_table_column fv_cols_list[] = {
    { "Name", 0, UUI_TALIGN_LEFT },
};

// --- the row model ----------------------------------------------------
//
// Row 0 is a SYNTHETIC ".." whenever the view is below the root; the
// real entries follow. It is not in `entries` because it is not a
// directory entry -- the filesystem never reported it, and a caller
// asking for the selected path must not be handed the parent.

static int fv_entry_index(const struct uui_fileview *fv, int row) {
    return fv->has_up ? row - 1 : row;
}

static const struct sys_dirent *fv_entry(const struct uui_fileview *fv, int row) {
    int i = fv_entry_index(fv, row);
    if (i < 0 || i >= fv->count) return 0;
    return &fv->entries[i];
}

static int fv_is_up_row(const struct uui_fileview *fv, int row) {
    return fv->has_up && row == 0;
}

static void ic_reveal(struct uui_fileview *fv); // icons mode, below

int uui_fileview_row_count(const struct uui_fileview *fv) {
    return fv->count + (fv->has_up ? 1 : 0);
}

int uui_fileview_count(const struct uui_fileview *fv) { return fv->count; }
int uui_fileview_truncated(const struct uui_fileview *fv) { return fv->truncated; }
const char *uui_fileview_dir(const struct uui_fileview *fv) { return fv->dir; }

unsigned long long uui_fileview_total_bytes(const struct uui_fileview *fv) {
    unsigned long long n = 0;
    for (int i = 0; i < fv->count; i++)
        if (!fv->entries[i].is_dir) n += fv->entries[i].size;
    return n;
}

// --- ordering ---------------------------------------------------------
//
// THE GROUPS ARE PRE-MULTIPLIED BY THE SORT DIRECTION, AND THAT IS NOT A
// TRICK FOR ITS OWN SAKE. uui_table multiplies whatever this returns by
// `sort_dir` (uui_table.c's order_rebuild), which is exactly right for
// comparing two files and exactly wrong for ".." and for directories:
// those lead the list under EVERY sort, as they do in Explorer, Finder
// and every commander. Multiplying the group difference by `sort_dir`
// here means the table's own multiplication cancels it back out, so the
// groups hold while the rows within them reverse.
//
// The alternative -- sorting the array with dirsort() and giving the
// table no comparator -- costs the click-to-sort header, and would put
// a second ordering beside the one lib/dirsort.h exists to keep single.
static int fv_group(const struct uui_fileview *fv, int row) {
    if (fv_is_up_row(fv, row)) return 0;
    const struct sys_dirent *e = fv_entry(fv, row);
    if (!e) return 3;
    return e->is_dir ? 1 : 2;
}

// Which dirsort key a column sorts by, and whether ASCENDING has to
// negate it. dirsort's directions are coreutils' (`ls -S` largest
// first, `ls -t` newest first); a table header's up-arrow means
// smallest and oldest first. Negating here rather than adding a third
// convention keeps the comparison itself in one place (lib/dirsort.h).
static enum dirsort_key fv_col_key(int col, int *out_negate) {
    switch (col) {
    case FV_COL_SIZE: *out_negate = 1; return DIRSORT_SIZE;
    case FV_COL_TIME: *out_negate = 1; return DIRSORT_TIME;
    default:          *out_negate = 0; return DIRSORT_NAME;
    }
}

static int fv_compare(void *ctx, int row_a, int row_b, int col) {
    struct uui_fileview *fv = (struct uui_fileview *)ctx;
    int dir = fv->table.sort_dir ? fv->table.sort_dir : 1;

    int ga = fv_group(fv, row_a), gb = fv_group(fv, row_b);
    if (ga != gb) return (ga - gb) * dir;

    const struct sys_dirent *a = fv_entry(fv, row_a);
    const struct sys_dirent *b = fv_entry(fv, row_b);
    if (!a || !b) return 0;

    int negate = 0;
    enum dirsort_key key = fv_col_key(col, &negate);
    // A directory has no meaningful size (SYS_LISTDIR reports 0), so
    // sorting directories by it would order them arbitrarily where the
    // name order is the only one a reader can predict.
    if (key == DIRSORT_SIZE && a->is_dir && b->is_dir) key = DIRSORT_NAME;

    int r = dirsort_cmp(a, b, key);
    if (negate && key != DIRSORT_NAME) r = -r;
    return r;
}

// --- cells ------------------------------------------------------------

static void fv_cell(void *ctx, int row, int col, char *out, int cap) {
    const struct uui_fileview *fv = (const struct uui_fileview *)ctx;

    if (fv_is_up_row(fv, row)) {
        // Nothing but the name: a size or a date against ".." would be
        // a fact about a row that is not a file.
        if (col == FV_COL_NAME) snprintf(out, (size_t)cap, "..");
        else out[0] = '\0';
        return;
    }

    const struct sys_dirent *e = fv_entry(fv, row);
    if (!e) { out[0] = '\0'; return; }

    switch (col) {
    case FV_COL_NAME:
        // A trailing '/' marks a directory: `ls -F`'s answer, kept in
        // the table modes even though the icons view draws artwork --
        // a text row with no glyph column still has to say which rows
        // descend.
        if (e->is_dir) snprintf(out, (size_t)cap, "%s/", e->name);
        else           snprintf(out, (size_t)cap, "%s", e->name);
        break;
    case FV_COL_SIZE:
        if (e->is_dir) snprintf(out, (size_t)cap, "<DIR>");
        else           human_size(out, (unsigned long)cap, e->size);
        break;
    case FV_COL_TIME: {
        const struct rtc_time *t = &e->modified;
        // Zeroed when the kernel's per-entry stat failed
        // (syscall_abi.h), and a "00-00 00:00" is a worse answer than
        // an empty cell.
        if (!t->year && !t->month && !t->day) { out[0] = '\0'; break; }
        snprintf(out, (size_t)cap, "%02u-%02u %02u:%02u",
                  (unsigned)t->month, (unsigned)t->day,
                  (unsigned)t->hour, (unsigned)t->minute);
        break;
    }
    default:
        out[0] = '\0';
        break;
    }
}

// --- marks ------------------------------------------------------------

int uui_fileview_is_marked(const struct uui_fileview *fv, int row) {
    if (row < 0 || row >= uui_fileview_row_count(fv)) return 0;
    if (fv_is_up_row(fv, row)) return 0;
    return (fv->marks[row / 32] >> (row % 32)) & 1u;
}

int uui_fileview_toggle_mark(struct uui_fileview *fv, int row) {
    if (row < 0 || row >= uui_fileview_row_count(fv)) return 0;
    if (fv_is_up_row(fv, row)) return 0;
    uint32_t bit = 1u << (row % 32);
    if (fv->marks[row / 32] & bit) {
        fv->marks[row / 32] &= ~bit;
        fv->mark_count--;
    } else {
        fv->marks[row / 32] |= bit;
        fv->mark_count++;
    }
    return 1;
}

int uui_fileview_mark_count(const struct uui_fileview *fv) { return fv->mark_count; }

void uui_fileview_clear_marks(struct uui_fileview *fv) {
    for (int i = 0; i < (int)(sizeof fv->marks / sizeof fv->marks[0]); i++)
        fv->marks[i] = 0;
    fv->mark_count = 0;
}

// Row order, not selection order: an operation over a marked set should
// run top to bottom, which is the order the user can see.
static int fv_marked_row(const struct uui_fileview *fv, int n) {
    int rows = uui_fileview_row_count(fv);
    for (int r = 0; r < rows; r++) {
        if (!uui_fileview_is_marked(fv, r)) continue;
        if (n-- == 0) return r;
    }
    return -1;
}

int uui_fileview_marked_path(const struct uui_fileview *fv, int n, char *out, int cap) {
    const struct sys_dirent *e = fv_entry(fv, fv_marked_row(fv, n));
    if (!e) return 0;
    return k_path_join(fv->dir, e->name, out, (size_t)cap);
}

int uui_fileview_marked_is_dir(const struct uui_fileview *fv, int n) {
    const struct sys_dirent *e = fv_entry(fv, fv_marked_row(fv, n));
    return e ? (int)e->is_dir : 0;
}

void uui_fileview_set_dimmed(struct uui_fileview *fv, int row, int on) {
    if (row < 0 || row >= (int)(sizeof fv->dimmed * 8)) return;
    uint32_t bit = 1u << (row & 31);
    int was = (fv->dimmed[row >> 5] & bit) != 0;
    if (was == !!on) return;
    if (on) { fv->dimmed[row >> 5] |= bit;  fv->dim_count++; }
    else    { fv->dimmed[row >> 5] &= ~bit; fv->dim_count--; }
}

int uui_fileview_is_dimmed(const struct uui_fileview *fv, int row) {
    if (row < 0 || row >= (int)(sizeof fv->dimmed * 8)) return 0;
    return (fv->dimmed[row >> 5] & (1u << (row & 31))) != 0;
}

void uui_fileview_clear_dimmed(struct uui_fileview *fv) {
    for (int i = 0; i < (int)(sizeof fv->dimmed / sizeof fv->dimmed[0]); i++)
        fv->dimmed[i] = 0;
    fv->dim_count = 0;
}

int uui_fileview_row_of(const struct uui_fileview *fv, const char *name) {
    if (!name) return -1;
    for (int r = 0; r < uui_fileview_row_count(fv); r++) {
        const struct sys_dirent *e = fv_entry(fv, r);
        if (e && strcmp(e->name, name) == 0) return r;
    }
    return -1;
}

// Faded rows -- see uui_table_fade_fn.
static int fv_fade(void *ctx, int row) {
    return uui_fileview_is_dimmed((const struct uui_fileview *)ctx, row);
}

// The marked rows' own background. Selection and hover outrank it (see
// uui_table.h): a mark says what an operation will act on, and it must
// not hide where the keyboard is.
static uint32_t fv_tint(void *ctx, int row) {
    const struct uui_fileview *fv = (const struct uui_fileview *)ctx;
    return uui_fileview_is_marked(fv, row) ? fv->mark_bg : 0;
}

// --- listing ----------------------------------------------------------

void uui_fileview_init(struct uui_fileview *fv, int x, int y, int w, int h,
                        struct sys_dirent *storage, int cap) {
    memset(fv, 0, sizeof *fv);
    fv->anchor = -1;
    fv->press_row = -1;
    fv->drop_row = -2;
    fv->entries = storage;
    fv->cap = cap;
    fv->last_click_row = -1;
    fv->navigable = 1;
    fv->dir[0] = '/';
    fv->dir[1] = '\0';
    fv->mode = UUI_FILEVIEW_DETAILS;

    uui_table_init(&fv->table, x, y, w, h,
                    fv_cols_details, 3, fv_cell, fv);
    uui_table_set_compare(&fv->table, fv_compare);
    uui_table_set_tint(&fv->table, fv_tint);
    uui_table_set_fade(&fv->table, fv_fade);
    // Marked rows wear the SELECTION colour, as in Explorer and Dolphin:
    // a multi-selection is one selection with several rows, not a
    // second kind of highlight. The cursor row is told apart by its
    // focus ring, which is what the ring is for.
    fv->mark_bg = fv->table.sel_bg;
    uui_table_set_sort(&fv->table, FV_COL_NAME, 1);
    // Typing a letter seeks by NAME, in both modes. Stated rather than
    // left to the default, so a reordered column list moves it too.
    uui_table_set_seek_col(&fv->table, FV_COL_NAME);
}

void uui_fileview_set_mode(struct uui_fileview *fv, enum uui_fileview_mode mode) {
    fv->mode = mode;
    if (mode == UUI_FILEVIEW_ICONS) {
        // The table keeps its columns and sort: the grid displays the
        // same order, and switching back finds the header as it was.
        fv->icon_top = 0;
        rb_clear(&fv->band);
        return;
    }
    if (mode == UUI_FILEVIEW_LIST) {
        fv->table.cols = fv_cols_list;
        fv->table.col_count = 1;
        uui_table_set_header(&fv->table, 0);
        // A one-column view has nothing to sort BY, so it stays on the
        // name -- not "unsorted", which would show the filesystem's own
        // walk order and differ from `ls` (lib/dirsort.h's whole point).
        uui_table_set_sort(&fv->table, FV_COL_NAME, 1);
    } else {
        fv->table.cols = fv_cols_details;
        fv->table.col_count = 3;
        uui_table_set_header(&fv->table, 1);
    }
}

void uui_fileview_set_navigable(struct uui_fileview *fv, int navigable) {
    fv->navigable = navigable ? 1 : 0;
}

void uui_fileview_set_filter(struct uui_fileview *fv,
                              uui_fileview_filter_fn fn, void *ctx) {
    fv->filter = fn;
    fv->filter_ctx = ctx;
}

void uui_fileview_set_thumb(struct uui_fileview *fv,
                             uui_fileview_thumb_fn fn, void *ctx) {
    fv->thumb = fn;
    fv->thumb_ctx = ctx;
}

static int fv_at_root(const struct uui_fileview *fv) {
    return fv->dir[0] == '/' && fv->dir[1] == '\0';
}

int uui_fileview_reload(struct uui_fileview *fv) {
    // Remember the selected NAME, not the row: a reload can insert or
    // remove entries above it, and a row index would then point at a
    // different file with nothing to say it moved.
    char keep[UUI_FILEVIEW_PATH_MAX];
    keep[0] = '\0';
    const char *sel = uui_fileview_selected_name(fv);
    if (sel) snprintf(keep, sizeof keep, "%s", sel);
    // NOTHING selected stays nothing: an empty-space click deselects,
    // and a refresh half a second later must not undo it.
    int had_none = fv->table.selected < 0;

    // THE MARKS SURVIVE BY NAME TOO. A mark names a ROW and the rows
    // are re-read, so the bitmap cannot be kept -- but the NAMES can,
    // and re-marking by name is how Explorer and Dolphin keep a
    // selection across a refresh. Without this, any write anywhere on
    // the volume (the app's own config file included) had the next
    // tick's reload silently unmark everything -- which is what made
    // "Insert marks files" flaky for weeks. STATIC scratch: the list is
    // larger than a ring-3 frame allows, and reload is not re-entrant.
    static char kept_marks[UUI_FILEVIEW_KEEP_MARKS][64];
    int nkept = 0;
    for (int n = 0; n < fv->mark_count && nkept < UUI_FILEVIEW_KEEP_MARKS; n++) {
        char path[UUI_FILEVIEW_PATH_MAX];
        if (!uui_fileview_marked_path(fv, n, path, sizeof path)) continue;
        snprintf(kept_marks[nkept++], sizeof kept_marks[0], "%s", k_path_basename(path));
    }

    fv->count = 0;
    fv->truncated = 0;
    fv->failed = 0;
    uui_fileview_clear_marks(fv); // see uui_fileview.h -- a mark names a ROW
    uui_fileview_clear_dimmed(fv); // ...and so do the dim bits and the anchor
    fv->anchor = -1;
    rb_clear(&fv->band);           // its selection is those rows
    fv->has_up = fv->navigable && !fv_at_root(fv);

    int n = sys_listdir(fv->dir, fv->entries, fv->cap);
    if (n < 0) {
        // A directory that cannot be read is NOT an empty one, and a
        // caller conflating the two reports a missing path as empty
        // (wm_fs.h learned this first).
        fv->failed = 1;
        n = 0;
    }
    // A full array means "there may be more" -- SYS_LISTDIR has no
    // offset argument, so this is a truncation point the view SAYS
    // rather than hides, exactly as /bin/ls does.
    fv->truncated = (n >= fv->cap);

    if (fv->filter) {
        int kept = 0;
        for (int i = 0; i < n; i++) {
            if (!fv->filter(fv->filter_ctx, fv->dir, &fv->entries[i])) continue;
            if (kept != i) fv->entries[kept] = fv->entries[i];
            kept++;
        }
        n = kept;
    }
    fv->count = n;

    // The table permutes; nothing sorts the array itself. set_rows()
    // rebuilds the order and clamps the scroll and the selection.
    uui_table_set_rows(&fv->table, uui_fileview_row_count(fv));

    if (keep[0] && uui_fileview_select_name(fv, keep)) {
        /* kept */
    } else if (had_none) {
        fv->table.selected = -1;
    } else {
        fv->table.selected = uui_fileview_row_count(fv) > 0
                              ? uui_table_source_row(&fv->table, 0) : -1;
    }
    for (int i = 0; i < nkept; i++) {
        int row = uui_fileview_row_of(fv, kept_marks[i]);
        if (row >= 0) uui_fileview_toggle_mark(fv, row);
    }
    fv->last_click_row = -1;
    if (fv->mode == UUI_FILEVIEW_ICONS) ic_reveal(fv);
    return !fv->failed;
}

int uui_fileview_set_dir(struct uui_fileview *fv, const char *dir) {
    if (!dir || !dir[0]) return 0;
    snprintf(fv->dir, sizeof fv->dir, "%s", dir);
    // Nothing to keep across a directory CHANGE -- reload()'s
    // preserve-by-name is for a refresh of the same directory.
    fv->table.selected = -1;
    fv->table.top = 0;
    fv->icon_top = 0;
    int ok = uui_fileview_reload(fv);
    // A directory just entered starts on its first row (reload keeps
    // "nothing selected" only for a refresh of the same one).
    if (fv->table.selected < 0 && uui_fileview_row_count(fv) > 0)
        fv->table.selected = uui_table_source_row(&fv->table, 0);
    if (fv->on_dir_changed) fv->on_dir_changed(fv->ctx, fv->dir);
    return ok;
}

int uui_fileview_up(struct uui_fileview *fv) {
    if (!fv->navigable || fv_at_root(fv)) return 0;

    // The directory being left, so it can be selected on arrival --
    // Backspace then Enter returns where you were, which is what every
    // file manager does and what makes Backspace safe to press.
    const char *leaving = k_path_basename(fv->dir);
    char was[UUI_FILEVIEW_PATH_MAX];
    snprintf(was, sizeof was, "%s", leaving ? leaving : "");

    char parent[UUI_FILEVIEW_PATH_MAX];
    if (!k_path_dirname(fv->dir, parent, sizeof parent)) return 0;
    if (!uui_fileview_set_dir(fv, parent)) return 0;
    if (was[0]) uui_fileview_select_name(fv, was);
    return 1;
}

// --- the selection ----------------------------------------------------

const char *uui_fileview_selected_name(const struct uui_fileview *fv) {
    const struct sys_dirent *e = fv_entry(fv, fv->table.selected);
    return e ? e->name : 0;
}

int uui_fileview_selected_is_dir(const struct uui_fileview *fv) {
    const struct sys_dirent *e = fv_entry(fv, fv->table.selected);
    return e ? (int)e->is_dir : 0;
}

int uui_fileview_selected_path(const struct uui_fileview *fv, char *out, int cap) {
    const struct sys_dirent *e = fv_entry(fv, fv->table.selected);
    if (!e) return 0; // nothing selected, or the ".." row -- see the header
    return k_path_join(fv->dir, e->name, out, (size_t)cap);
}

int uui_fileview_select_name(struct uui_fileview *fv, const char *name) {
    if (!name || !name[0]) return 0;
    for (int i = 0; i < fv->count; i++) {
        if (strcmp(fv->entries[i].name, name) != 0) continue;
        fv->table.selected = fv->has_up ? i + 1 : i;
        // Scroll it into view: a selection the user cannot see is a
        // selection they will act on by accident.
        if (fv->mode == UUI_FILEVIEW_ICONS) { ic_reveal(fv); return 1; }
        int view = uui_table_view_row(&fv->table, fv->table.selected);
        int vis = uui_table_visible_rows(&fv->table);
        if (view >= 0) {
            if (view < fv->table.top) fv->table.top = view;
            else if (view >= fv->table.top + vis) fv->table.top = view - vis + 1;
        }
        return 1;
    }
    return 0;
}

static void fv_report_select(struct uui_fileview *fv) {
    if (!fv->on_select) return;
    char p[UUI_FILEVIEW_PATH_MAX];
    if (uui_fileview_selected_path(fv, p, sizeof p))
        fv->on_select(fv->ctx, p, uui_fileview_selected_is_dir(fv));
}

int uui_fileview_activate(struct uui_fileview *fv) {
    int row = fv->table.selected;
    if (row < 0) return 0;

    if (fv_is_up_row(fv, row)) return uui_fileview_up(fv);

    const struct sys_dirent *e = fv_entry(fv, row);
    if (!e) return 0;

    if (e->is_dir) {
        if (!fv->navigable) return 0;
        char next[UUI_FILEVIEW_PATH_MAX];
        if (!k_path_join(fv->dir, e->name, next, sizeof next)) return 0;
        return uui_fileview_set_dir(fv, next);
    }

    if (fv->on_open) {
        char p[UUI_FILEVIEW_PATH_MAX];
        if (k_path_join(fv->dir, e->name, p, sizeof p)) fv->on_open(fv->ctx, p);
    }
    return 1;
}

// --- icons mode -------------------------------------------------------
//
// The cell math is icon_grid.h's (the desktop's, compiled into ring 3);
// the artwork is icon_cache's "folder"/"file" at a font-derived size.
// Everything below speaks VIEW positions -- the table's sorted order --
// and converts through uui_table_source_row()/_view_row() at the edges,
// so selection, marks and sorting are ONE state across all three modes.

static int ic_px(void)   { return ugfx_char_h() * 2; }  // the icon box
static int ic_pad(void)  { return 4; }

static int ic_cell_w(void) {
    // A label's worth of pitch, never narrower than the icon -- the
    // desktop's icon_col_w() tradeoff (fixed pitch, clipped labels).
    int w = 14 * ugfx_char_w();
    int m = ic_px() + 8;
    return w < m ? m : w;
}

static int ic_cell_h(void) { return ic_px() + ugfx_char_h() + 10; }

static int ic_cols(const struct uui_fileview *fv) {
    int n = (fv->table.w - fv->table.bar_w - 2 * ic_pad()) / ic_cell_w();
    return n > 0 ? n : 1;
}

static int ic_total_rows(const struct uui_fileview *fv) {
    int cols = ic_cols(fv);
    return (uui_fileview_row_count(fv) + cols - 1) / cols;
}

static int ic_vis_rows(const struct uui_fileview *fv) {
    int n = (fv->table.h - 2 * ic_pad()) / ic_cell_h();
    return n > 0 ? n : 1;
}

static int ic_bar_visible(const struct uui_fileview *fv) {
    return ic_total_rows(fv) > ic_vis_rows(fv);
}

// Bottom-anchored offset, uui_scrollbar's convention (see uui_tree.c).
static int ic_offset(const struct uui_fileview *fv) {
    return ic_total_rows(fv) - ic_vis_rows(fv) - fv->icon_top;
}

static void ic_clamp(struct uui_fileview *fv) {
    int max_top = ic_total_rows(fv) - ic_vis_rows(fv);
    if (max_top < 0) max_top = 0;
    if (fv->icon_top > max_top) fv->icon_top = max_top;
    if (fv->icon_top < 0) fv->icon_top = 0;
}

static int ic_set_offset(struct uui_fileview *fv, int offset) {
    int before = fv->icon_top;
    fv->icon_top = ic_total_rows(fv) - ic_vis_rows(fv) - offset;
    ic_clamp(fv);
    return fv->icon_top != before;
}

static struct icon_grid ic_grid(const struct uui_fileview *fv) {
    struct icon_grid g;
    g.origin_x = fv->table.x + ic_pad();
    // Scrolled: grid row `icon_top` lands on the widget's first row.
    g.origin_y = fv->table.y + ic_pad() - fv->icon_top * ic_cell_h();
    g.cell_w = ic_cell_w();
    g.cell_h = ic_cell_h();
    g.cols = ic_cols(fv);
    return g;
}

// A VIEW position's cell rectangle, in content coordinates.
static void ic_cell_rect(const struct uui_fileview *fv, int view,
                          int *out_x, int *out_y) {
    struct icon_grid g = ic_grid(fv);
    icon_grid_cell_rect(&g, view % g.cols, view / g.cols, out_x, out_y);
}

int uui_fileview_cell_rect(const struct uui_fileview *fv, int view,
                            int *x, int *y, int *w, int *h) {
    if (fv->mode != UUI_FILEVIEW_ICONS) return 0;
    if (view < 0 || view >= uui_fileview_row_count(fv)) return 0;
    ic_cell_rect(fv, view, x, y);
    if (w) *w = ic_cell_w();
    if (h) *h = ic_cell_h();
    return 1;
}

// VIEW position at (cx, cy), or -1. Exact, not nearest-cell:
// icon_grid_nearest_cell() clamps, and "empty space" must stay
// answerable here or the rubber band could never begin.
static int ic_hit_view(const struct uui_fileview *fv, int cx, int cy) {
    const struct uui_table *t = &fv->table;
    if (!uui_hit(t->x, t->y, t->w, t->h, cx, cy)) return -1;
    if (ic_bar_visible(fv) && cx >= t->x + t->w - t->bar_w) return -1;
    int lx = cx - (t->x + ic_pad());
    int ly = cy - (t->y + ic_pad()) + fv->icon_top * ic_cell_h();
    if (lx < 0 || ly < 0) return -1;
    int col = lx / ic_cell_w();
    if (col >= ic_cols(fv)) return -1;
    int view = (ly / ic_cell_h()) * ic_cols(fv) + col;
    return view < uui_fileview_row_count(fv) ? view : -1;
}

static void ic_reveal(struct uui_fileview *fv) {
    int view = uui_table_view_row(&fv->table, fv->table.selected);
    if (view < 0) { ic_clamp(fv); return; }
    int grow = view / ic_cols(fv);
    int vis = ic_vis_rows(fv);
    if (grow < fv->icon_top) fv->icon_top = grow;
    else if (grow >= fv->icon_top + vis) fv->icon_top = grow - vis + 1;
    ic_clamp(fv);
}

// Move the selection by `delta` VIEW positions, clamped to the ends.
static int ic_step(struct uui_fileview *fv, int delta) {
    int rows = uui_fileview_row_count(fv);
    if (rows <= 0) return 0;
    int view = uui_table_view_row(&fv->table, fv->table.selected);
    int next = view < 0 ? 0 : view + delta;
    if (next < 0) next = 0;
    if (next >= rows) next = rows - 1;
    if (view == next) return 0;
    fv->table.selected = uui_table_source_row(&fv->table, next);
    ic_reveal(fv);
    fv_report_select(fv);
    return 1;
}

// The band's items are view positions; rubberband.h asks geometry only.
static int ic_rb_count(void *ctx) {
    return uui_fileview_row_count((const struct uui_fileview *)ctx);
}

static void ic_rb_rect(void *ctx, int index, int *x, int *y, int *w, int *h) {
    const struct uui_fileview *fv = (const struct uui_fileview *)ctx;
    ic_cell_rect(fv, index, x, y);
    *w = ic_cell_w();
    *h = ic_cell_h();
}

// Make the MARKS agree with the band's selection. Toggle-by-difference,
// so mark_count stays right and ".." (never markable) stays out.
static void ic_apply_band(struct uui_fileview *fv) {
    int rows = uui_fileview_row_count(fv);
    for (int view = 0; view < rows; view++) {
        int src = uui_table_source_row(&fv->table, view);
        if (fv_is_up_row(fv, src)) continue;
        int want = rb_is_selected(&fv->band, view);
        if (want != uui_fileview_is_marked(fv, src))
            uui_fileview_toggle_mark(fv, src);
    }
}

int uui_fileview_band_active(const struct uui_fileview *fv) {
    return fv->band.armed;
}

// The band's rects in the TABLE modes: a view row is the full row
// width at its screen y, so the same rubberband.h engine marks rows.
static void tb_rb_rect(void *ctx, int index, int *x, int *y, int *w, int *h) {
    const struct uui_fileview *fv = (const struct uui_fileview *)ctx;
    const struct uui_table *t = &fv->table;
    int bar = uui_table_scrollbar_visible(t) ? t->bar_w : 0;
    *x = t->x;
    *y = t->y + uui_table_header_h(t) + (index - t->top) * uui_table_row_h(t);
    *w = t->w - bar;
    *h = uui_table_row_h(t);
}

// A PRESS ON EMPTY SPACE, any view: the selection goes -- marks and the
// cursor row both, Explorer's and Dolphin's rule -- unless Ctrl/Shift
// says the band that may follow should ADD (what KDE, Windows and this
// desktop's own icons all do on that gesture). Deciding the deselect
// here rather than at rb_end() is what makes it the same in the table
// modes, which have no band-clear to fall out of.
static void fv_empty_press(struct uui_fileview *fv, int cx, int cy, unsigned mods) {
    int add = (mods & (KEY_MOD_CTRL | KEY_MOD_SHIFT)) != 0;
    if (!add) {
        uui_fileview_clear_marks(fv);
        fv->anchor = -1;
        fv->table.selected = -1;
    }
    rb_begin(&fv->band, cx, cy, add ? RB_ADD : RB_REPLACE);
}

static int fv_apply_mods(struct uui_fileview *fv, int row, unsigned mods);

// The press half of fv_apply_mods(): a PLAIN press on a row already in
// the marked set leaves the set alone until the release says it was a
// click (see `deferred_clear`), so a drag from it carries the set.
static int fv_press_mods(struct uui_fileview *fv, int row, unsigned mods) {
    if (!(mods & (KEY_MOD_CTRL | KEY_MOD_SHIFT)) &&
        uui_fileview_is_marked(fv, row)) {
        fv->deferred_clear = 1;
        return 0;
    }
    return fv_apply_mods(fv, row, mods);
}

// A staged cut's artwork, taken HALFWAY TO THE BACKGROUND -- Explorer's
// and Dolphin's translucent icon. The table fades its TEXT instead
// (uui_table.c's fade), because a text row has no artwork to fade.
static void ic_wash(struct ugfx_surface *s, int x, int y, int w, int h,
                    uint32_t bg) {
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            ugfx_blend_pixel(s, xx, yy, bg, 128);
}

static void ic_draw(struct ugfx_surface *s, const struct uui_fileview *fv) {
    const struct uui_table *t = &fv->table;
    ugfx_fill_rect(s, t->x, t->y, t->w, t->h, t->bg);

    // THE BOTTOM ROW IS DELIBERATELY PARTIAL (`last`, below), so this
    // must clip or that row paints over whatever follows the pane -- the
    // File Manager's status bar, which is what it did.
    ugfx_set_clip_rect(s, t->x, t->y, t->w, t->h);

    int rows = uui_fileview_row_count(fv);
    int cols = ic_cols(fv), vis = ic_vis_rows(fv);
    int px = ic_px(), cw = ic_cell_w(), chh = ic_cell_h();
    int first = fv->icon_top * cols;
    int last = first + (vis + 1) * cols; // +1: the partial row at the bottom
    if (last > rows) last = rows;

    int sel_x = -1, sel_y = -1; // the selected cell, for the focus ring

    for (int view = first; view < last; view++) {
        int src = uui_table_source_row(t, view);
        int x, y;
        ic_cell_rect(fv, view, &x, &y);

        // Same precedence as the table's rows: selection, hover, tint.
        uint32_t bg = t->bg;
        int selected = (src == t->selected);
        if (selected) { bg = t->sel_bg; sel_x = x; sel_y = y; }
        else if (src == t->hovered) bg = uui_state_bg(t->bg, UUI_STATE_HOVER);
        else if (uui_fileview_is_marked(fv, src)) bg = fv->mark_bg;
        if (bg != t->bg) ugfx_fill_rect(s, x, y, cw - 2, chh - 2, bg);

        int is_up = fv_is_up_row(fv, src);
        const struct sys_dirent *e = fv_entry(fv, src);
        const char *name = is_up ? ".." : (e ? e->name : "");
        int is_dir = is_up || (e && e->is_dir);

        // A thumbnail if the app has one READY (see uui_fileview_thumb_fn
        // -- this is a lookup, never a decode), else the generic icon.
        const struct uimg *thumb = 0;
        if (!is_dir && e && fv->thumb)
            thumb = fv->thumb(fv->thumb_ctx, fv->dir, e, px);
        int ax = x, ay = y + 2, aw = cw, ah = px; // the artwork, for ic_wash
        if (thumb) {
            int tx2 = x + (cw - thumb->w) / 2;
            int ty2 = y + 2 + (px - thumb->h) / 2;
            ax = tx2; ay = ty2; aw = thumb->w; ah = thumb->h;
            if (thumb->has_alpha)
                ugfx_blit_alpha(s, tx2, ty2, thumb->w, thumb->h, thumb->px, thumb->w);
            else
                ugfx_blit(s, tx2, ty2, thumb->w, thumb->h, thumb->px, thumb->w);
            // A hairline frame: a photo's edge can match the pane and a
            // frameless thumbnail reads as a rendering glitch.
            ugfx_draw_rect(s, tx2 - 1, ty2 - 1, thumb->w + 2, thumb->h + 2,
                            t->grid);
        } else {
            const struct uimg *ico = icon_get(is_dir ? "folder" : "file", px);
            int ix = x + (cw - px) / 2;
            ax = ix; ay = y + 2; aw = px; ah = px;
            if (ico) {
                ugfx_blit_alpha(s, ix, y + 2, ico->w, ico->h, ico->px, ico->w);
            } else {
                // The desktop's letter tile, for a build whose icon
                // files are missing rather than merely unthemed.
                ugfx_fill_rect(s, ix, y + 2, px, px, ugfx_rgb(60, 90, 130));
                char initial[2] = { name[0] ? name[0] : '?', 0 };
                ugfx_draw_string(s, ix + (px - ugfx_char_w()) / 2,
                                  y + 2 + (px - ugfx_char_h()) / 2, initial,
                                  ugfx_rgb(230, 230, 235), ugfx_rgb(60, 90, 130));
            }
        }

        // The LABEL stays at full strength: it is how the file is
        // identified, and Explorer and Dolphin both fade only the icon.
        if (uui_fileview_is_dimmed(fv, src)) ic_wash(s, ax, ay, aw, ah, bg);

        int max_w = cw - 6;
        int tw = ugfx_text_width(name);
        int lx = tw < max_w ? x + (cw - tw) / 2 : x + 3;
        ugfx_draw_string_clipped(s, lx, y + 2 + px + 2, max_w, name,
                                  selected ? t->sel_fg : t->fg, bg);
    }

    if (ic_bar_visible(fv))
        uui_scrollbar_draw(s, t->x + t->w - t->bar_w, t->y, t->bar_w, t->h,
                            ic_total_rows(fv), vis, ic_offset(fv),
                            t->track_bg, t->thumb_bg, 0);

    // The band, above everything it crosses. An outline, not a fill --
    // the same call the desktop makes (no alpha blend to fill with).
    int bx, by, bw, bh;
    if (rb_rect(&fv->band, &bx, &by, &bw, &bh))
        ugfx_draw_rect(s, bx, by, bw, bh, t->fg);

    if (t->focused) {
        if (sel_x >= 0) uui_focus_ring(s, sel_x, sel_y, cw - 2, chh - 2);
        else            uui_focus_ring(s, t->x, t->y, t->w, t->h);
    }

    ugfx_clear_clip_rect(s);
}

static int ic_hover(struct uui_fileview *fv, int cx, int cy) {
    int view = ic_hit_view(fv, cx, cy);
    int src = view >= 0 ? uui_table_source_row(&fv->table, view) : -1;
    if (src == fv->table.hovered) return 0;
    fv->table.hovered = src;
    return 1;
}

static int ic_press(struct uui_fileview *fv, int cx, int cy, unsigned mods) {
    struct uui_table *t = &fv->table;
    if (!uui_hit(t->x, t->y, t->w, t->h, cx, cy)) return 0;

    if (ic_bar_visible(fv) && cx >= t->x + t->w - t->bar_w) {
        int vis = ic_vis_rows(fv), total = ic_total_rows(fv);
        int off = ic_offset(fv);
        enum uui_scrollbar_zone zone =
            uui_scrollbar_hit(t->x + t->w - t->bar_w, t->y, t->bar_w, t->h,
                              total, vis, off, cx, cy, 0);
        if (zone == UUI_SB_THUMB) {
            int thumb_y, thumb_h;
            uui_scrollbar_thumb_rect(t->y, t->h, total, vis, off,
                                      &thumb_y, &thumb_h, t->bar_w, 0);
            t->thumb_grab = cy - thumb_y;
            return 1;
        }
        int page = vis > 1 ? vis - 1 : 1;
        if (zone == UUI_SB_ABOVE) ic_set_offset(fv, off + page);
        else if (zone == UUI_SB_BELOW) ic_set_offset(fv, off - page);
        return 1;
    }

    int view = ic_hit_view(fv, cx, cy);
    fv->press_row = -1;
    fv->deferred_clear = 0;
    if (view < 0) {
        fv_empty_press(fv, cx, cy, mods);
        return 1;
    }

    int src = uui_table_source_row(t, view);
    fv->press_row = src;
    int changed = (t->selected != src);
    t->selected = src;

    fv_press_mods(fv, src, mods);
    if (mods & (KEY_MOD_CTRL | KEY_MOD_SHIFT)) {
        if (changed) fv_report_select(fv);
        fv->last_click_row = -1;
        return 1;
    }

    unsigned long now = sys_ticks();
    int is_double = (src == fv->last_click_row &&
                      now - fv->last_click_tick <= UUI_FILEVIEW_DOUBLE_CLICK_TICKS);
    fv->last_click_tick = now;
    fv->last_click_row = is_double ? -1 : src;

    if (is_double) { fv->deferred_clear = 0; return uui_fileview_activate(fv) || 1; }
    if (changed) fv_report_select(fv);
    return 1;
}

static int ic_drag(struct uui_fileview *fv, int cx, int cy) {
    struct uui_table *t = &fv->table;
    if (t->thumb_grab >= 0) {
        int off = uui_scrollbar_offset_for_drag(t->y, t->h, ic_total_rows(fv),
                                                 ic_vis_rows(fv), cy,
                                                 t->thumb_grab, t->bar_w, 0);
        return ic_set_offset(fv, off);
    }
    if (fv->band.armed) {
        struct rb_ops ops = { ic_rb_count, ic_rb_rect };
        rb_motion(&fv->band, cx, cy, &ops, fv);
        ic_apply_band(fv);
        return 1;
    }
    return 0;
}

static void ic_drag_end(struct uui_fileview *fv) {
    fv->table.thumb_grab = -1;
    if (fv->band.armed) {
        rb_end(&fv->band);
        ic_apply_band(fv);
    }
}

static int ic_wheel(struct uui_fileview *fv, int notches) {
    int before = fv->icon_top;
    // MINUS, like every scrolling widget here: positive notches mean
    // the wheel rolled AWAY (mouse.c negates the raw byte), which
    // scrolls the view UP. This shipped as += and read exactly like an
    // inverted mouse.
    fv->icon_top -= notches;
    ic_clamp(fv);
    return fv->icon_top != before;
}

static int ic_key(struct uui_fileview *fv, int key) {
    int cols = ic_cols(fv), vis = ic_vis_rows(fv);
    int rows = uui_fileview_row_count(fv);
    if (rows <= 0) return 0;

    switch (key) {
    case KEY_ARROW_LEFT:  return ic_step(fv, -1);
    case KEY_ARROW_RIGHT: return ic_step(fv, 1);
    case KEY_ARROW_UP:    return ic_step(fv, -cols);
    case KEY_ARROW_DOWN:  return ic_step(fv, cols);
    case KEY_PAGE_UP:     return ic_step(fv, -vis * cols);
    case KEY_PAGE_DOWN:   return ic_step(fv, vis * cols);
    case KEY_HOME:        return ic_step(fv, -rows);
    case KEY_END:         return ic_step(fv, rows);
    default:
        // Letters: the table's type-ahead moves the selection in the
        // shared sort order; only the scroll state is this mode's.
        if (uui_table_key(&fv->table, key)) {
            ic_reveal(fv);
            fv_report_select(fv);
            return 1;
        }
        return 0;
    }
}

// --- the direct interface ---------------------------------------------

void uui_fileview_set_geometry(struct uui_fileview *fv, int x, int y, int w, int h) {
    fv->table.x = x; fv->table.y = y; fv->table.w = w; fv->table.h = h;
    uui_table_set_rows(&fv->table, uui_fileview_row_count(fv));
}

void uui_fileview_set_active_mark(struct uui_fileview *fv, int on, uint32_t color) {
    fv->active_mark = on ? 1 : 0;
    fv->active_mark_color = color;
}

// The drop target's outline: the directory row under the pointer, or
// the whole pane when the drop is "into here". The focus ring's
// accent, so it reads as "this one" in the theme's own colour.
static void fv_draw_drop(struct ugfx_surface *s, const struct uui_fileview *fv) {
    const struct uui_table *t = &fv->table;
    if (fv->drop_row == -2) return;
    if (fv->drop_row == -1) {
        uui_focus_ring(s, t->x + 2, t->y + 2, t->w - 4, t->h - 4);
        return;
    }
    int view = uui_table_view_row(t, fv->drop_row);
    if (view < 0) return;
    int x, y, w, h;
    if (fv->mode == UUI_FILEVIEW_ICONS) {
        if (!uui_fileview_cell_rect(fv, view, &x, &y, &w, &h)) return;
        uui_focus_ring(s, x, y, w - 2, h - 2);
        return;
    }
    if (view < t->top || view >= t->top + uui_table_visible_rows(t)) return;
    tb_rb_rect((void *)fv, view, &x, &y, &w, &h);
    uui_focus_ring(s, x, y, w, h);
}

void uui_fileview_draw(struct ugfx_surface *s, const struct uui_fileview *fv) {
    if (fv->mode == UUI_FILEVIEW_ICONS) {
        ic_draw(s, fv);
    } else {
        uui_table_draw(s, &fv->table);
        // The band, over the rows and clipped to them (ic_draw does its
        // own inside its clip).
        int bx, by, bw, bh;
        if (rb_rect(&fv->band, &bx, &by, &bw, &bh)) {
            const struct uui_table *t = &fv->table;
            int hh = uui_table_header_h(t);
            ugfx_set_clip_rect(s, t->x, t->y + hh, t->w, t->h - hh);
            ugfx_draw_rect(s, bx, by, bw, bh, t->fg);
            ugfx_clear_clip_rect(s);
        }
    }
    fv_draw_drop(s, fv);
    if (fv->active_mark) {
        const struct uui_table *t = &fv->table;
        ugfx_draw_rect(s, t->x, t->y, t->w, t->h, fv->active_mark_color);
        ugfx_draw_rect(s, t->x + 1, t->y + 1, t->w - 2, t->h - 2,
                        fv->active_mark_color);
    }
}

void uui_fileview_natural_size(const struct uui_fileview *fv, int *out_w, int *out_h) {
    uui_table_natural_size(&fv->table, out_w, out_h);
}

int uui_fileview_hit(const struct uui_fileview *fv, int cx, int cy) {
    if (fv->mode == UUI_FILEVIEW_ICONS) {
        int view = ic_hit_view(fv, cx, cy);
        return view >= 0 ? uui_table_source_row(&fv->table, view) : -1;
    }
    return uui_table_hit(&fv->table, cx, cy);
}

int uui_fileview_hover(struct uui_fileview *fv, int cx, int cy) {
    if (fv->mode == UUI_FILEVIEW_ICONS) return ic_hover(fv, cx, cy);
    return uui_table_hover(&fv->table, cx, cy);
}

// THE SELECTION AND THE MARKED SET ARE ONE THING, which is Explorer's
// and Dolphin's model: a plain click replaces the set, Ctrl toggles one
// row, Shift takes the range from the anchor. `row` is a SOURCE row;
// the range is walked in VIEW order, because what a person means by
// "everything between these two" is what they can see, not what the
// unsorted array happens to hold.
//
// Returns 1 if anything changed. The ".." row is never markable -- it
// is not a file, and an operation over a set containing it would act on
// the parent directory.
static int fv_apply_mods(struct uui_fileview *fv, int row, unsigned mods) {
    if (fv_is_up_row(fv, row)) { fv->anchor = -1; return 0; }

    if (mods & KEY_MOD_CTRL) {
        uui_fileview_toggle_mark(fv, row);
        fv->anchor = row;
        return 1;
    }

    if ((mods & KEY_MOD_SHIFT) && fv->anchor >= 0) {
        int a = uui_table_view_row(&fv->table, fv->anchor);
        int b = uui_table_view_row(&fv->table, row);
        if (a < 0 || b < 0) return 0;
        if (a > b) { int t = a; a = b; b = t; }
        // REPLACED, not added to: Shift means "this range", so dragging
        // the far end back in has to un-mark what it passed over.
        uui_fileview_clear_marks(fv);
        for (int view = a; view <= b; view++) {
            int src = uui_table_source_row(&fv->table, view);
            if (src >= 0 && !fv_is_up_row(fv, src))
                uui_fileview_toggle_mark(fv, src);
        }
        return 1;   // the anchor STAYS: a second Shift-click re-ranges
    }

    // Plain: the set becomes this one row, and this is where a range
    // will start from.
    int had = fv->mark_count > 0;
    uui_fileview_clear_marks(fv);
    fv->anchor = row;
    return had;
}

int uui_fileview_press(struct uui_fileview *fv, int cx, int cy, unsigned mods) {
    // A PRESS ANYWHERE INSIDE IS CONSUMED, even on the empty space below
    // the last row. Returning 0 there does two things a caller cannot
    // work around: the router does not take the pointer grab, so no
    // release is delivered, and it never NAMES this widget to the app
    // (ui/uui_route.c) -- which is how clicking the blank part of a file
    // manager pane failed to make that pane the active one, while
    // clicking a row worked.
    if (fv->mode == UUI_FILEVIEW_ICONS) return ic_press(fv, cx, cy, mods);

    int inside = uui_hit(fv->table.x, fv->table.y, fv->table.w, fv->table.h, cx, cy);
    fv->press_row = -1;
    fv->deferred_clear = 0;

    if (uui_table_press(&fv->table, cx, cy)) return 1; // the scrollbar

    int row = uui_table_hit(&fv->table, cx, cy);
    int changed = uui_table_click(&fv->table, cx, cy);
    if (row < 0) {
        // The header sorts; anywhere else inside is empty space.
        if (uui_table_header_hit(&fv->table, cx, cy) >= 0 || !inside)
            return changed || inside;
        fv_empty_press(fv, cx, cy, mods);
        return 1;
    }
    fv->press_row = row;

    // The modifiers act on the SET; the click still moves the cursor.
    // Ctrl+click deliberately does NOT count toward a double click --
    // toggling a row twice is not an "open".
    int set_changed = fv_press_mods(fv, row, mods);
    if (mods & (KEY_MOD_CTRL | KEY_MOD_SHIFT)) {
        if (changed) fv_report_select(fv);
        fv->last_click_row = -1;
        return set_changed || changed || inside;
    }

    unsigned long now = sys_ticks();
    int is_double = (row == fv->last_click_row &&
                      now - fv->last_click_tick <= UUI_FILEVIEW_DOUBLE_CLICK_TICKS);
    fv->last_click_tick = now;
    // A third click inside the window must not re-trigger as a second
    // double -- the same guard the desktop's icons carry.
    fv->last_click_row = is_double ? -1 : row;

    if (is_double) { fv->deferred_clear = 0; return uui_fileview_activate(fv) || changed; }
    if (changed) fv_report_select(fv);
    return set_changed || changed || inside;
}

int uui_fileview_select_at(struct uui_fileview *fv, int cx, int cy) {
    struct uui_table *t = &fv->table;
    int row = (fv->mode == UUI_FILEVIEW_ICONS)
                  ? uui_table_source_row(t, ic_hit_view(fv, cx, cy))
                  : uui_table_hit(t, cx, cy);
    if (row < 0) return 0;

    // A SECONDARY CLICK INSIDE THE SET KEEPS IT. This is the one place
    // a click does not replace the marks, and every file manager does
    // the same: right-clicking one of five selected files must offer to
    // act on the five, not silently drop four. Outside the set it
    // replaces, which is the plain-click rule.
    if (!uui_fileview_is_marked(fv, row)) {
        uui_fileview_clear_marks(fv);
        fv->anchor = row;
    }

    if (row == t->selected) return 0;
    t->selected = row;
    fv_report_select(fv);
    return 1;
}

int uui_fileview_drag(struct uui_fileview *fv, int cx, int cy) {
    if (fv->mode == UUI_FILEVIEW_ICONS) return ic_drag(fv, cx, cy);
    if (uui_table_drag(&fv->table, cx, cy)) return 1;
    if (fv->band.armed) {
        struct rb_ops ops = { ic_rb_count, tb_rb_rect };
        rb_motion(&fv->band, cx, cy, &ops, fv);
        ic_apply_band(fv);
        return 1;
    }
    return 0;
}

void uui_fileview_drag_end(struct uui_fileview *fv) {
    if (fv->mode == UUI_FILEVIEW_ICONS) { ic_drag_end(fv); return; }
    uui_table_drag_end(&fv->table);
    if (fv->band.armed) {
        rb_end(&fv->band);
        ic_apply_band(fv);
    }
}

int uui_fileview_wheel(struct uui_fileview *fv, int notches) {
    if (fv->mode == UUI_FILEVIEW_ICONS) return ic_wheel(fv, notches);
    return uui_table_wheel(&fv->table, notches);
}

int uui_fileview_key(struct uui_fileview *fv, int key) {
    if (key == '\n' || key == '\r') return uui_fileview_activate(fv);
    if (key == '\b') return uui_fileview_up(fv);
    if (key == KEY_INSERT || key == ' ') {
        // Toggle and STEP ON, which is what makes marking a run of
        // files one repeated keystroke -- every commander does this, and
        // a toggle that stayed put would need two keys per file. "On"
        // is the next row in the table modes and the next CELL in the
        // grid, which is the same view position either way.
        if (!uui_fileview_toggle_mark(fv, fv->table.selected)) return 0;
        if (fv->mode == UUI_FILEVIEW_ICONS) ic_step(fv, 1);
        else uui_table_key(&fv->table, KEY_ARROW_DOWN);
        return 1;
    }
    if (fv->mode == UUI_FILEVIEW_ICONS) return ic_key(fv, key);
    if (uui_table_key(&fv->table, key)) {
        fv_report_select(fv);
        return 1;
    }
    return 0;
}

// --- the generic ops table --------------------------------------------

static void fv_ops_natural_size(const void *w, int *out_w, int *out_h) {
    uui_fileview_natural_size((const struct uui_fileview *)w, out_w, out_h);
}

static void fv_ops_set_geometry(void *w, int x, int y, int width, int height) {
    uui_fileview_set_geometry((struct uui_fileview *)w, x, y, width, height);
}

static void fv_ops_bounds(const void *w, int *x, int *y, int *out_w, int *out_h) {
    const struct uui_fileview *fv = (const struct uui_fileview *)w;
    if (x) *x = fv->table.x;
    if (y) *y = fv->table.y;
    if (out_w) *out_w = fv->table.w;
    if (out_h) *out_h = fv->table.h;
}

static void fv_ops_draw(struct ugfx_surface *s, const void *w) {
    uui_fileview_draw(s, (const struct uui_fileview *)w);
}

// `>= 0`, NOT the row index: the router tests this as a BOOLEAN, so
// returning the index makes row 0 report "not hit" (CLAUDE.md's rule,
// and the bug two widgets here shipped). The WHOLE widget, because a
// press on the header or the scrollbar must reach it too.
static int fv_ops_hit(const void *w, int cx, int cy) {
    const struct uui_fileview *fv = (const struct uui_fileview *)w;
    return uui_hit(fv->table.x, fv->table.y, fv->table.w, fv->table.h, cx, cy);
}

static int fv_ops_key(void *w, int key, unsigned mods) {
    (void)mods;
    return uui_fileview_key((struct uui_fileview *)w, key);
}

static int fv_ops_accepts_focus(const void *w) { (void)w; return 1; }

// Forwarded to the table it composes, which is what draws the ring.
static void fv_ops_set_focused(void *w, int focused) {
    ((struct uui_fileview *)w)->table.focused = focused;
}

static int fv_ops_press(void *w, int cx, int cy, unsigned mods) {
    return uui_fileview_press((struct uui_fileview *)w, cx, cy, mods);
}

static int fv_ops_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_fileview *fv = (struct uui_fileview *)w;
    if (buttons & 1) return uui_fileview_drag(fv, cx, cy);
    return uui_fileview_hover(fv, cx, cy);
}

static int fv_ops_release(void *w, int cx, int cy) {
    (void)cx; (void)cy;
    struct uui_fileview *fv = (struct uui_fileview *)w;
    uui_fileview_drag_end(fv);
    // The press was a click after all: the deferred plain-click clear.
    if (fv->deferred_clear && fv->press_row >= 0) {
        fv->deferred_clear = 0;
        return fv_apply_mods(fv, fv->press_row, 0);
    }
    fv->deferred_clear = 0;
    return 0;
}

// --- drag and drop ----------------------------------------------------

static int fv_ops_drag_start(void *w, int cx, int cy, struct uui_drag *d) {
    (void)cx; (void)cy;
    struct uui_fileview *fv = (struct uui_fileview *)w;
    int row = fv->press_row;
    if (row < 0 || fv_is_up_row(fv, row)) return 0;   // empty space: a band

    // This press is a drag now, not a click: no band, no deferred
    // clear, and the next click is not a double.
    rb_clear(&fv->band);
    fv->deferred_clear = 0;
    fv->last_click_row = -1;

    // The set, or the row alone (made the whole selection, so the
    // app's operand rule names exactly what is carried).
    if (!uui_fileview_is_marked(fv, row)) {
        uui_fileview_clear_marks(fv);
        fv->anchor = row;
        fv->table.selected = row;
    }
    int n = fv->mark_count > 0 ? fv->mark_count : 1;
    if (n == 1) {
        char one[UUI_FILEVIEW_PATH_MAX];
        const struct sys_dirent *e = fv_entry(fv, row);
        if (fv->mark_count && uui_fileview_marked_path(fv, 0, one, sizeof one))
            snprintf(fv->drag_label, sizeof fv->drag_label, "%s", k_path_basename(one));
        else
            snprintf(fv->drag_label, sizeof fv->drag_label, "%s", e ? e->name : "?");
    } else {
        snprintf(fv->drag_label, sizeof fv->drag_label, "%d items", n);
    }
    d->kind = UUI_DRAG_FILES;
    d->dir = fv->dir;
    d->count = n;
    d->label = fv->drag_label;
    return 1;
}

static void fv_ops_drag_end(void *w, int dropped) {
    (void)dropped;
    ((struct uui_fileview *)w)->deferred_clear = 0;
}

static int fv_ops_drag_over(void *w, int cx, int cy, const struct uui_drag *d) {
    struct uui_fileview *fv = (struct uui_fileview *)w;
    fv->drop_row = -2;
    if (d->kind != UUI_DRAG_FILES || cx == UUI_NOWHERE) return 0;

    int row = uui_fileview_hit(fv, cx, cy);
    int trow = -1;
    char target[UUI_FILEVIEW_PATH_MAX];
    if (row >= 0 && fv_is_up_row(fv, row)) {
        trow = row;
        if (!k_path_dirname(fv->dir, target, sizeof target)) return 0;
    } else if (row >= 0 && fv_entry(fv, row) && fv_entry(fv, row)->is_dir) {
        trow = row;
        if (!k_path_join(fv->dir, fv_entry(fv, row)->name, target, sizeof target))
            return 0;
    } else {
        snprintf(target, sizeof target, "%s", fv->dir);
    }
    // Their own directory is a no-op, and one of the dragged items is
    // not a place to put them.
    if (strcmp(target, d->dir) == 0) return 0;
    if (d->source == fv && trow >= 0 &&
        (uui_fileview_is_marked(fv, trow) || trow == fv->table.selected))
        return 0;
    fv->drop_row = trow;
    snprintf(fv->drop_target, sizeof fv->drop_target, "%s", target);
    return 1;
}

static int fv_ops_drop(void *w, int cx, int cy, const struct uui_drag *d) {
    (void)cx; (void)cy; (void)d;
    struct uui_fileview *fv = (struct uui_fileview *)w;
    if (fv->drop_row == -2) return 0;
    fv->drop_row = -2;   // the highlight goes; drop_target stays for the app
    return 1;
}

const char *uui_fileview_drop_target(const struct uui_fileview *fv) {
    return fv->drop_target;
}

// The ghost: icon and label beside the pointer, a "+" when Ctrl says
// copy -- the feedback Explorer and Dolphin give, minus their cursor.
static void fv_ops_drag_draw(struct ugfx_surface *s, const void *w, const struct uui_drag *d) {
    const struct uui_fileview *fv = (const struct uui_fileview *)w;
    const struct uui_table *t = &fv->table;
    int px = ugfx_char_h();
    int is_dir = 0;
    if (d->count == 1) {
        if (fv->mark_count) is_dir = uui_fileview_marked_is_dir(fv, 0);
        else if (fv_entry(fv, t->selected)) is_dir = fv_entry(fv, t->selected)->is_dir;
    }
    const struct uimg *ico = icon_get(is_dir ? "folder" : "file", px);
    int tw = ugfx_text_width(d->label);
    int gx = d->x + 12, gy = d->y + 12;
    int gw = px + 6 + tw + 8, gh = px + 6;
    ugfx_fill_rect(s, gx, gy, gw, gh, t->sel_bg);
    ugfx_draw_rect(s, gx, gy, gw, gh, t->fg);
    if (ico) ugfx_blit_alpha(s, gx + 3, gy + 3, ico->w, ico->h, ico->px, ico->w);
    ugfx_draw_string(s, gx + px + 6, gy + (gh - ugfx_char_h()) / 2, d->label,
                     t->sel_fg, t->sel_bg);
    if (d->copy)
        ugfx_draw_string(s, gx + gw + 2, gy, "+", t->fg, t->bg);
}

static int fv_ops_wheel(void *w, int notches) {
    return uui_fileview_wheel((struct uui_fileview *)w, notches);
}

static void ops_describe(const void *w, const struct uui_describe *d) {
    uui_describe_int(d, "selected", ((const struct uui_fileview *)w)->table.selected);
}

const struct uui_widget_ops uui_fileview_ops = {
    .natural_size = fv_ops_natural_size,
    .set_geometry = fv_ops_set_geometry,
    .bounds = fv_ops_bounds,
    .draw = fv_ops_draw,
    .hit = fv_ops_hit,
    .key = fv_ops_key,
    .accepts_focus = fv_ops_accepts_focus,
    .set_focused = fv_ops_set_focused,
    .press = fv_ops_press,
    .motion = fv_ops_motion,
    .release = fv_ops_release,
    .wheel = fv_ops_wheel,
    .describe     = ops_describe,
    .drag_start = fv_ops_drag_start,
    .drag_end   = fv_ops_drag_end,
    .drag_over  = fv_ops_drag_over,
    .drop       = fv_ops_drop,
    .drag_draw  = fv_ops_drag_draw,
};
