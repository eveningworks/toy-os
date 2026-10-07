// fileview -- a directory listing, as a widget. See ui/uui_fileview.h.
#include "ui/uui_fileview.h"
#include <time.h>
#include "ui/uui_widget.h"
#include "rt/sys.h"     // sys_listdir(), sys_ticks()
#include "ui/uui_anim.h" // uui_wheel_step_px()
#include "kpath.h"      // k_path_join/_dirname -- the KERNEL's, linked into ring 3
#include "lib/human.h"  // human_size()
#include "lib/udate.h"  // the Modified column, in the locale's spelling
#include "lib/icon_cache.h" // icon_get() -- the icons view's artwork
#include "lib/ufiletype.h"  // a row's type, in words and as an icon
#include "icon_grid.h"      // cell math, shared with the desktop
#include <string.h>
#include <stdio.h>
#include "keyboard.h"   // KEY_* codes, as delivered by WIN_EV_KEY
#include "ui/uui_label.h" // uui_label_wrap_next -- the icon caption
#include "lib/uclip.h"     // the drag slot -- a drag that leaves the window
#include "ui/uui_route.h" // UUI_NOWHERE -- a drag_over's leave

// Column indices in DETAILS mode. LIST mode declares only the first.
// Dolphin's order, and chosen for how it NARROWS: a pane too narrow for
// every column drops them from the end -- Type, then Modified -- so the
// name keeps its room and no column's index ever moves (the sort key IS
// the index; see uui_fileview_set_sort).
#define FV_COL_NAME 0
#define FV_COL_SIZE 1
#define FV_COL_TIME 2
#define FV_COL_TYPE 3

// Widths in CHARACTERS, per docs/gui-guidelines.md -- font-derived, so
// the whole view reflows when the font size changes. Name stretches.
static const struct uui_table_column fv_cols_details[] = {
    { "Name",     0,  UUI_TALIGN_LEFT  },
    { "Size",     9,  UUI_TALIGN_RIGHT },
    // Room for the widest region's "12/31/2026 12:59 PM" (lib/udate.h).
    { "Modified", 20, UUI_TALIGN_LEFT  },
    { "Type",     13, UUI_TALIGN_LEFT  },
};
#define FV_NAME_MIN_CHARS 16   // the name column is never squeezed below this

// How many of `cols` fit `w`: all of them, or fewer from the end --
// the name, first, keeps at least FV_NAME_MIN_CHARS.
static int fv_fit_cols(const struct uui_table_column *cols, int count, int w) {
    int per = ugfx_char_advance('0');
    if (per <= 0) per = ugfx_char_w();
    int n = count, fixed = 0;
    for (int i = 1; i < count; i++) fixed += cols[i].width_chars;
    while (n > 2 && w < (fixed + FV_NAME_MIN_CHARS) * per) {
        n--;
        fixed -= cols[n].width_chars;
    }
    return n;
}

static int fv_fit_columns(int w) { return fv_fit_cols(fv_cols_details, 4, w); }
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

// An entry's real path: the source's answer in a virtual folder, else
// the directory joined with its name.
static int fv_entry_path(const struct uui_fileview *fv, const struct sys_dirent *e,
                         char *out, int cap) {
    if (fv->src) return fv->src->path(fv->src->ctx, (int)(e - fv->entries), out, cap);
    return k_path_join(fv->dir, e->name, out, (size_t)cap);
}

static void ic_reveal(struct uui_fileview *fv); // icons mode, below
static void fv_details_cols(struct uui_fileview *fv);
static void fv_apply_groups(struct uui_fileview *fv);
static void ic_clamp(struct uui_fileview *fv);   // ...and its range check

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
// A folder's counted size, or -1 (not counted, or nobody counts them).
static long long fv_dir_bytes(const struct uui_fileview *fv, const struct sys_dirent *e) {
    char p[UUI_FILEVIEW_PATH_MAX];
    if (!fv->dirsize || !fv_entry_path(fv, e, p, sizeof p)) return -1;
    return fv->dirsize(fv->dirsize_ctx, p);
}

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

    if (fv->src && col > 0) {
        int ia = (int)(a - fv->entries), ib = (int)(b - fv->entries);
        if (fv->src->compare) return fv->src->compare(fv->src->ctx, ia, ib, col);
        char ta[64], tb[64];
        fv->src->cell(fv->src->ctx, ia, col, ta, sizeof ta);
        fv->src->cell(fv->src->ctx, ib, col, tb, sizeof tb);
        return strcmp(ta, tb);
    }

    int negate = 0;
    enum dirsort_key key = fv_col_key(col, &negate);
    // A directory has no meaningful size (SYS_LISTDIR reports 0), so
    // sorting directories by it would order them arbitrarily where the
    // name order is the only one a reader can predict.
    if (key == DIRSORT_SIZE && a->is_dir && b->is_dir) {
        // ...unless they are counted: then by the count, an uncounted
        // folder as the smallest.
        long long sa = fv_dir_bytes(fv, a), sb = fv_dir_bytes(fv, b);
        if (fv->dirsize && (sa >= 0 || sb >= 0)) return -(sa < sb ? -1 : sa > sb);
        key = DIRSORT_NAME;
    }

    if (col == FV_COL_TYPE) {
        int r = strcmp(ufiletype_name(a->name, a->is_dir), ufiletype_name(b->name, b->is_dir));
        if (r) return r;
    }
    int r = dirsort_cmp(a, b, key);
    if (negate && key != DIRSORT_NAME) r = -r;
    return r;
}

// --- cells ------------------------------------------------------------

// The name as SHOWN: without its extension when the view hides them and
// the type is one the table knows -- an unknown extension is part of
// what the name says.
static void fv_display_name(const struct uui_fileview *fv, const struct sys_dirent *e,
                            char *out, int cap) {
    snprintf(out, (size_t)cap, "%s", e->name);
    if (!fv->hide_ext || e->is_dir || !strcmp(ufiletype_name(e->name, 0), "File")) return;
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = '\0';
}

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
    if (fv->src && col > 0) {
        fv->src->cell(fv->src->ctx, (int)(e - fv->entries), col, out, cap);
        return;
    }

    switch (col) {
    case FV_COL_NAME:
        // Bare: the row's icon says which rows are folders.
        fv_display_name(fv, e, out, cap);
        break;
    case FV_COL_TYPE:
        snprintf(out, (size_t)cap, "%s", ufiletype_name(e->name, e->is_dir));
        break;
    case FV_COL_SIZE:
        // A folder has no size of its own (SYS_LISTDIR reports 0), and
        // a blank says so better than a zero -- unless the app counts
        // them, and then "..." until its count arrives.
        if (e->is_dir) {
            long long b = fv_dir_bytes(fv, e);
            if (b >= 0) human_size(out, (unsigned long)cap, (unsigned long long)b);
            else if (fv->dirsize) snprintf(out, (size_t)cap, "...");
            else out[0] = '\0';
        } else {
            human_size(out, (unsigned long)cap, e->size);
        }
        break;
    case FV_COL_TIME: {
        // Zeroed when the kernel's per-entry stat failed
        // (syscall_abi.h), and a "00-00 00:00" is a worse answer than
        // an empty cell.
        if (!e->modified.year && !e->modified.month && !e->modified.day) {
            out[0] = '\0';
            break;
        }
        udate_format(out, (unsigned long)cap, &e->modified, UDATE_DATE | UDATE_TIME);
        break;
    }
    default:
        out[0] = '\0';
        break;
    }
}

// The row's type icon, before its name: the table asks, the icon cache
// answers (a lookup, never a decode).
static const struct uimg *fv_icon(void *ctx, int row, int px) {
    const struct uui_fileview *fv = (const struct uui_fileview *)ctx;
    if (fv_is_up_row(fv, row)) return icon_get("folder", px);
    const struct sys_dirent *e = fv_entry(fv, row);
    return e ? icon_get(ufiletype_icon(e->name, e->is_dir), px) : 0;
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
    return fv_entry_path(fv, e, out, cap);
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
    return uui_fileview_is_marked(fv, row)
           ? UUI_COLOR(fv->mark_bg, uui_table_c_sel_bg(&fv->table)) : 0;
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
                    fv_cols_details, 4, fv_cell, fv);
    uui_table_set_icon(&fv->table, fv_icon);
    uui_table_set_compare(&fv->table, fv_compare);
    uui_table_set_tint(&fv->table, fv_tint);
    uui_table_set_fade(&fv->table, fv_fade);
    // Marked rows wear the SELECTION colour, as in Explorer and Dolphin:
    // a multi-selection is one selection with several rows, not a
    // second kind of highlight. The cursor row is told apart by its
    // focus ring, which is what the ring is for.
    // UNSET, not resolved here: this runs at construction, and taking
    // the colour now would freeze today's palette into every file view
    // ever built -- the exact thing the sentinel exists to stop.
    fv->mark_bg = UUI_COLOR_UNSET;
    uui_table_set_sort(&fv->table, FV_COL_NAME, 1);
    // Typing a letter seeks by NAME, in both modes. Stated rather than
    // left to the default, so a reordered column list moves it too.
    uui_table_set_seek_col(&fv->table, FV_COL_NAME);
}

// The enum IS the column order in details mode; stated so a reordered
// column list cannot silently re-map the menu.
_Static_assert(UUI_FILEVIEW_SORT_NAME == FV_COL_NAME && UUI_FILEVIEW_SORT_MODIFIED == FV_COL_TIME &&
               UUI_FILEVIEW_SORT_TYPE == FV_COL_TYPE && UUI_FILEVIEW_SORT_SIZE == FV_COL_SIZE,
               "the sort keys are the details columns");

// Sorting by a column a narrow pane is not showing is still sorting:
// the comparator reads the entries, not the drawn cells.
void uui_fileview_set_sort(struct uui_fileview *fv, enum uui_fileview_sort key, int dir) {
    int shown = fv->table.col_count;
    if (fv->table.cols == fv_cols_details) fv->table.col_count = 4;
    else if (fv->src && fv->table.cols == fv->src->cols) fv->table.col_count = fv->src->ncols;
    uui_table_set_sort(&fv->table, (int)key, dir);
    fv->table.col_count = shown;
}

enum uui_fileview_sort uui_fileview_sort(const struct uui_fileview *fv, int *dir) {
    if (dir) *dir = fv->table.sort_dir < 0 ? -1 : 1;
    return fv->table.sort_col >= 0 ? (enum uui_fileview_sort)fv->table.sort_col
                                   : UUI_FILEVIEW_SORT_NAME;
}

void uui_fileview_set_mode(struct uui_fileview *fv, enum uui_fileview_mode mode) {
    fv->mode = mode;
    if (fv->src && fv->src->group) {
        fv_apply_groups(fv);
        uui_table_set_rows(&fv->table, uui_fileview_row_count(fv));
    }
    if (mode == UUI_FILEVIEW_ICONS) {
        // The table keeps its columns and sort: the grid displays the
        // same order, and switching back finds the header as it was.
        fv->icon_scroll = 0;
        uui_scrollanim_cancel(&fv->ic_anim);
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
        fv_details_cols(fv);
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

// Each row's share of the folder, on its Size cell.
static int fv_meter(void *ctx, int row, int col) {
    const struct uui_fileview *fv = (const struct uui_fileview *)ctx;
    if (!fv->dirsize || col != FV_COL_SIZE || !fv->sized_total) return -1;
    const struct sys_dirent *e = fv_entry(fv, row);
    if (!e) return -1;
    long long b = e->is_dir ? fv_dir_bytes(fv, e) : (long long)e->size;
    if (b < 0) return -1;
    return (int)((unsigned long long)b * 1000ull / fv->sized_total);
}

void uui_fileview_sizes_changed(struct uui_fileview *fv) {
    unsigned long long t = 0;
    for (int i = 0; i < fv->count; i++) {
        long long b = fv->entries[i].is_dir ? fv_dir_bytes(fv, &fv->entries[i])
                                            : (long long)fv->entries[i].size;
        if (b > 0) t += (unsigned long long)b;
    }
    fv->sized_total = t;
    uui_table_set_rows(&fv->table, uui_fileview_row_count(fv));   // re-sort by the new sizes
}

void uui_fileview_set_dirsize(struct uui_fileview *fv, uui_fileview_dirsize_fn fn, void *ctx) {
    fv->dirsize = fn;
    fv->dirsize_ctx = ctx;
    uui_table_set_meter(&fv->table, fn ? fv_meter : 0);
    uui_fileview_sizes_changed(fv);
}

void uui_fileview_set_resolver(struct uui_fileview *fv,
                                uui_fileview_resolve_fn fn, void *ctx) {
    fv->resolve = fn;
    fv->resolve_ctx = ctx;
}

const struct uui_fileview_source *uui_fileview_source(const struct uui_fileview *fv) {
    return fv->src;
}

// A source's groups, in the table's row numbers (no ".." row in a
// virtual folder, but asked through fv_entry all the same).
static int fv_group_of(void *ctx, int row) {
    const struct uui_fileview *fv = ctx;
    const struct sys_dirent *e = fv_entry(fv, row);
    return e && fv->src && fv->src->group ? fv->src->group(fv->src->ctx, (int)(e - fv->entries)) : 0;
}

static void fv_group_title(void *ctx, int g, char *out, int cap) {
    const struct uui_fileview *fv = ctx;
    out[0] = '\0';
    if (fv->src && fv->src->group_title) fv->src->group_title(fv->src->ctx, g, out, cap);
}

// Groups in the details view only: the icon grid walks the table's
// view rows, and a caption there would be a blank tile.
static void fv_apply_groups(struct uui_fileview *fv) {
    int on = fv->src && fv->src->group && fv->mode == UUI_FILEVIEW_DETAILS;
    uui_table_set_groups(&fv->table, on ? fv_group_of : 0, on ? fv_group_title : 0);
}

// The details columns for the current listing: a source's, or the four.
static void fv_details_cols(struct uui_fileview *fv) {
    if (fv->src) {
        fv->table.cols = fv->src->cols;
        fv->table.col_count = fv_fit_cols(fv->src->cols, fv->src->ncols, fv->table.w);
    } else {
        fv->table.cols = fv_cols_details;
        fv->table.col_count = fv_fit_columns(fv->table.w);
    }
}

static int fv_at_root(const struct uui_fileview *fv) {
    return fv->dir[0] == '/' && fv->dir[1] == '\0';
}

// Defined below, beside the public wrapper -- see its comment on why
// `reveal` is an argument at all.
static int fv_select_name(struct uui_fileview *fv, const char *name, int reveal);

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

    // A virtual folder, or a directory again: the columns follow, and a
    // sort on a column the new set lacks falls back to the name.
    const struct uui_fileview_source *was = fv->src;
    fv->src = fv->resolve ? fv->resolve(fv->resolve_ctx, fv->dir) : 0;
    if (fv->src != was) {
        if (fv->mode == UUI_FILEVIEW_DETAILS) fv_details_cols(fv);
        int ncols = fv->src ? fv->src->ncols : 4;
        if (fv->table.sort_col >= ncols || (was && was->sort_dir))
            uui_table_set_sort(&fv->table, FV_COL_NAME, 1);
        if (fv->src && fv->src->sort_dir)
            uui_table_set_sort(&fv->table, fv->src->sort_col, fv->src->sort_dir);
        fv_apply_groups(fv);
    }
    fv->has_up = !fv->src && fv->navigable && !fv_at_root(fv);

    int n = fv->src ? fv->src->list(fv->src->ctx, fv->dir, fv->entries, fv->cap)
                    : sys_listdir(fv->dir, fv->entries, fv->cap);
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

    if (fv->filter && !fv->src) {
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

    // NO REVEAL: this is a refresh, and the scroll offset is the
    // user's -- see fv_select_name().
    if (keep[0] && fv_select_name(fv, keep, 0)) {
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
    // **CLAMP, NOT REVEAL.** The grid may have shrunk under the offset,
    // so it has to be brought back into range -- but moving the view to
    // the SELECTION is what made a scrolled pane jump to the top on
    // every refresh, and a refresh happens on any write anywhere on the
    // volume. It only ever bit the icons view because it is the only
    // mode this line ran in, which is why /bin -- no thumbnails, no
    // cache writes -- did it too.
    if (fv->mode == UUI_FILEVIEW_ICONS) ic_clamp(fv);
    if (fv->dirsize) uui_fileview_sizes_changed(fv);   // a new listing, a new total
    return !fv->failed;
}

int uui_fileview_set_dir(struct uui_fileview *fv, const char *dir) {
    if (!dir || !dir[0]) return 0;
    fv->renaming = 0;
    snprintf(fv->dir, sizeof fv->dir, "%s", dir);
    // Nothing to keep across a directory CHANGE -- reload()'s
    // preserve-by-name is for a refresh of the same directory.
    fv->table.selected = -1;
    fv->table.top = 0;
    fv->icon_scroll = 0;
    uui_scrollanim_cancel(&fv->ic_anim);
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
    if (fv->src) {   // a virtual folder says where its Up goes, if anywhere
        if (!fv->src->up || !fv->src->up(fv->src->ctx, fv->dir, parent, sizeof parent)) return 0;
    } else if (!k_path_dirname(fv->dir, parent, sizeof parent)) {
        return 0;
    }
    if (!uui_fileview_set_dir(fv, parent)) return 0;
    if (was[0]) uui_fileview_select_name(fv, was);
    return 1;
}

// --- the selection ----------------------------------------------------

const struct sys_dirent *uui_fileview_selected_entry(const struct uui_fileview *fv) {
    if (fv_is_up_row(fv, fv->table.selected)) return 0;
    return fv_entry(fv, fv->table.selected);
}

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
    return fv_entry_path(fv, e, out, cap);
}

// **A REFRESH MUST NOT MOVE THE VIEW, AND THAT IS WHY `reveal` IS AN
// ARGUMENT.** Restoring the selection after a reload and selecting a
// file because the user asked are the same search and opposite
// intentions: the second should scroll the file into sight, and the
// first must leave the scroll exactly where the user put it.
//
// Conflating them shipped as a file manager that scrolled itself back
// to the top every half second in any directory full of images. The
// loop: decoding a thumbnail WRITES it to /var/cache/thumbnails, which
// bumps sys_fs_generation(), which has files.c's tick reload both
// panes, which restored the selection, which revealed it. Explorer and
// Dolphin both keep the offset across a refresh for this reason -- a
// live view the user cannot scroll is not a live view.
static int fv_select_name(struct uui_fileview *fv, const char *name, int reveal) {
    if (!name || !name[0]) return 0;
    for (int i = 0; i < fv->count; i++) {
        if (strcmp(fv->entries[i].name, name) != 0) continue;
        fv->table.selected = fv->has_up ? i + 1 : i;
        if (!reveal) return 1;
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

int uui_fileview_select_name(struct uui_fileview *fv, const char *name) {
    return fv_select_name(fv, name, 1);
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

    if (e->is_dir && fv->src && fv->src->descends) {
        char next[UUI_FILEVIEW_PATH_MAX];
        if (!fv_entry_path(fv, e, next, sizeof next)) return 0;
        return uui_fileview_set_dir(fv, next);
    }
    if (e->is_dir && !fv->src) {
        if (!fv->navigable) return 0;
        char next[UUI_FILEVIEW_PATH_MAX];
        if (!k_path_join(fv->dir, e->name, next, sizeof next)) return 0;
        return uui_fileview_set_dir(fv, next);
    }

    if (fv->on_open) {
        char p[UUI_FILEVIEW_PATH_MAX];
        if (fv_entry_path(fv, e, p, sizeof p)) fv->on_open(fv->ctx, p);
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

// The icon box: the app's `icon_px`, or two text lines.
static int ic_px(const struct uui_fileview *fv) {
    return fv->icon_px > 0 ? fv->icon_px : ugfx_char_h() * 2;
}
static int ic_pad(void)  { return 4; }

static int ic_cell_w(const struct uui_fileview *fv) {
    // A label's worth of pitch, never narrower than the icon -- the
    // desktop's icon_col_w() tradeoff (fixed pitch, clipped labels),
    // including its reservation in a REPRESENTATIVE glyph. `char_w` is
    // the widest advance in the face, so fourteen of it is ~1.7x the
    // pitch these labels were sized against, and the selection
    // highlight is a cell wide.
    int per = ugfx_char_advance('n');
    if (per <= 0) per = ugfx_char_w();
    int w = 14 * per;
    int m = ic_px(fv) + 8;
    return w < m ? m : w;
}

// Two label lines under the icon, the desktop's DESKTOP_LABEL_LINES and
// KDE's/Windows' default; the second is cut with ".." when a name runs on.
#define IC_LABEL_LINES 2
static int ic_cell_h(const struct uui_fileview *fv) { return ic_px(fv) + IC_LABEL_LINES * (ugfx_char_h() + 1) + 10; }

static int ic_cols(const struct uui_fileview *fv) {
    int n = (fv->table.w - fv->table.bar_w - 2 * ic_pad()) / ic_cell_w(fv);
    return n > 0 ? n : 1;
}

static int ic_total_rows(const struct uui_fileview *fv) {
    int cols = ic_cols(fv);
    return (uui_fileview_row_count(fv) + cols - 1) / cols;
}

static int ic_vis_rows(const struct uui_fileview *fv) {
    int n = (fv->table.h - 2 * ic_pad()) / ic_cell_h(fv);
    return n > 0 ? n : 1;
}

// THE GRID SCROLLS BY PIXEL, NOT BY ROW -- Explorer's and Dolphin's
// icon views do, and a cell here is ~6 text lines tall, so a row-
// stepped thumb had a handful of positions and leapt between them
// under the cursor. The scrollbar helpers are unit-agnostic: they get
// pixels for total, visible and offset alike.
static int ic_view_h(const struct uui_fileview *fv) {
    int h = fv->table.h - 2 * ic_pad();
    return h > 0 ? h : 1;
}

static int ic_content_h(const struct uui_fileview *fv) {
    return ic_total_rows(fv) * ic_cell_h(fv);
}

static int ic_max_scroll(const struct uui_fileview *fv) {
    int m = ic_content_h(fv) - ic_view_h(fv);
    return m > 0 ? m : 0;
}

static int ic_bar_visible(const struct uui_fileview *fv) {
    return ic_max_scroll(fv) > 0;
}

// Bottom-anchored offset, uui_scrollbar's convention (see uui_tree.c).
static int ic_offset(const struct uui_fileview *fv) {
    return ic_max_scroll(fv) - fv->icon_scroll;
}

static void ic_clamp(struct uui_fileview *fv) {
    int max_scroll = ic_max_scroll(fv);
    if (fv->icon_scroll > max_scroll) fv->icon_scroll = max_scroll;
    if (fv->icon_scroll < 0) fv->icon_scroll = 0;
}

// Where the grid is DRAWN this frame: the canonical position less the
// glide's displacement (ui/uui_scrollanim.h). Geometry and hit-testing
// both read this, so a click mid-glide lands on what is on screen.
static int ic_eff_scroll(const struct uui_fileview *fv) {
    return fv->icon_scroll - fv->ic_anim.disp;
}

static int ic_set_offset(struct uui_fileview *fv, int offset) {
    int before = fv->icon_scroll;
    fv->icon_scroll = ic_max_scroll(fv) - offset;
    ic_clamp(fv);
    return fv->icon_scroll != before;
}

static struct icon_grid ic_grid(const struct uui_fileview *fv) {
    struct icon_grid g;
    g.origin_x = fv->table.x + ic_pad();
    g.origin_y = fv->table.y + ic_pad() - ic_eff_scroll(fv);
    g.cell_w = ic_cell_w(fv);
    g.cell_h = ic_cell_h(fv);
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
    if (w) *w = ic_cell_w(fv);
    if (h) *h = ic_cell_h(fv);
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
    int ly = cy - (t->y + ic_pad()) + ic_eff_scroll(fv);
    if (lx < 0 || ly < 0) return -1;
    int col = lx / ic_cell_w(fv);
    if (col >= ic_cols(fv)) return -1;
    int view = (ly / ic_cell_h(fv)) * ic_cols(fv) + col;
    return view < uui_fileview_row_count(fv) ? view : -1;
}

static void ic_reveal(struct uui_fileview *fv) {
    int view = uui_table_view_row(&fv->table, fv->table.selected);
    if (view < 0) { ic_clamp(fv); return; }
    int top = (view / ic_cols(fv)) * ic_cell_h(fv), bottom = top + ic_cell_h(fv);
    int view_h = ic_view_h(fv);
    if (top < fv->icon_scroll) fv->icon_scroll = top;
    else if (bottom > fv->icon_scroll + view_h) fv->icon_scroll = bottom - view_h;
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
    *w = ic_cell_w(fv);
    *h = ic_cell_h(fv);
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

static int fv_apply_mods(struct uui_fileview *fv, int prev, int row, unsigned mods);

// The press half of fv_apply_mods(): a PLAIN press on a row already in
// the marked set leaves the set alone until the release says it was a
// click (see `deferred_clear`), so a drag from it carries the set.
static int fv_press_mods(struct uui_fileview *fv, int prev, int row, unsigned mods) {
    if (!(mods & (KEY_MOD_CTRL | KEY_MOD_SHIFT)) &&
        uui_fileview_is_marked(fv, row)) {
        fv->deferred_clear = 1;
        return 0;
    }
    return fv_apply_mods(fv, prev, row, mods);
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
    ugfx_fill_rect(s, t->x, t->y, t->w, t->h, uui_table_c_bg(t));

    // THE BOTTOM ROW IS DELIBERATELY PARTIAL (`last`, below), so this
    // must clip or that row paints over whatever follows the pane -- the
    // File Manager's status bar, which is what it did.
    ugfx_set_clip_rect(s, t->x, t->y, t->w, t->h);

    int rows = uui_fileview_row_count(fv);
    int cols = ic_cols(fv), vis = ic_vis_rows(fv);
    int px = ic_px(fv), cw = ic_cell_w(fv), chh = ic_cell_h(fv);
    // The glide: the state is the draw's own; the view is const to its
    // caller. Everything below reads ic_eff_scroll(), which is what the
    // displacement moved.
    int disp = uui_scrollanim_sync((struct uui_scrollanim *)&fv->ic_anim, fv->icon_scroll);
    int eff = ic_eff_scroll(fv);
    if (eff < 0) eff = 0;
    int first = (eff / chh) * cols;
    int last = first + (vis + 2) * cols; // +2: a partial row at each end
    if (last > rows) last = rows;

    int cursor_drawn = 0;       // the cursor cell carried the focus edge

    for (int view = first; view < last; view++) {
        int src = uui_table_source_row(t, view);
        int x, y;
        ic_cell_rect(fv, view, &x, &y);

        // Same precedence as the table's rows: selection, hover, tint.
        uint32_t bg = uui_table_c_bg(t);
        int selected = (src == t->selected);
        int marked = uui_fileview_is_marked(fv, src);
        if (selected || marked) bg = UUI_COLOR(fv->mark_bg, uui_table_c_sel_bg(t));
        else if (src == t->hovered) bg = uui_state_bg(uui_table_c_bg(t), UUI_STATE_HOVER);
        // A SELECTED CELL IS A SOFT FILL WITH A 1px EDGE, the label plain
        // -- Windows 11 Explorer's shape, chosen from mockups 2026-10-01.
        // The cursor's edge is the full accent in a focused pane: that is
        // the focus ring, rounded, so no square ring is drawn over it.
        if (selected || marked) {
            int cur = selected && t->focused;
            uint32_t edge = cur ? UTHEME_ACCENT : ugfx_blend(UTHEME_WHITE, UTHEME_ACCENT, 130);
            uui_fill_round_rect(s, x, y, cw - 2, chh - 2, 6, edge);
            uui_fill_round_rect(s, x + 1, y + 1, cw - 4, chh - 4, 5, bg);
            if (cur) cursor_drawn = 1;
        } else if (bg != uui_table_c_bg(t)) {
            uui_fill_round_rect(s, x, y, cw - 2, chh - 2, 8, bg);
        }

        int is_up = fv_is_up_row(fv, src);
        const struct sys_dirent *e = fv_entry(fv, src);
        char shown[UUI_FILEVIEW_PATH_MAX];
        if (e && !is_up) fv_display_name(fv, e, shown, sizeof shown);
        const char *name = is_up ? ".." : (e ? shown : "");
        int is_dir = is_up || (e && e->is_dir);

        // A thumbnail if the app has one READY (see uui_fileview_thumb_fn
        // -- this is a lookup, never a decode), else the generic icon.
        const struct uimg *thumb = 0;
        if (!is_dir && e && fv->thumb) {
            // A virtual folder's file lives somewhere real: its folder.
            char real[UUI_FILEVIEW_PATH_MAX], parent[UUI_FILEVIEW_PATH_MAX];
            if (!fv->src)
                thumb = fv->thumb(fv->thumb_ctx, fv->dir, e, px);
            else if (fv_entry_path(fv, e, real, sizeof real) &&
                     k_path_dirname(real, parent, sizeof parent))
                thumb = fv->thumb(fv->thumb_ctx, parent, e, px);
        }
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
                            uui_table_c_grid(t));
        } else {
            const struct uimg *ico = icon_get(is_up || !e ? "folder" : ufiletype_icon(e->name, is_dir), px);
            int ix = x + (cw - px) / 2;
            ax = ix; ay = y + 2; aw = px; ah = px;
            if (ico) {
                ugfx_blit_alpha(s, ix, y + 2, ico->w, ico->h, ico->px, ico->w);
            } else {
                // The desktop's letter tile, for a build whose icon
                // files are missing rather than merely unthemed.
                ugfx_fill_rect(s, ix, y + 2, px, px, ugfx_rgb(60, 90, 130));
                char initial[2] = { name[0] ? name[0] : '?', 0 };
                ugfx_draw_string(s, ix + (px - ugfx_text_width(initial)) / 2,
                                  y + 2 + (px - ugfx_char_h()) / 2, initial,
                                  ugfx_rgb(230, 230, 235), ugfx_rgb(60, 90, 130));
            }
        }

        // The LABEL stays at full strength: it is how the file is
        // identified, and Explorer and Dolphin both fade only the icon.
        if (uui_fileview_is_dimmed(fv, src)) ic_wash(s, ax, ay, aw, ah, bg);

        // Word-wrapped into IC_LABEL_LINES centred lines, the last cut
        // with ".." -- the desktop's draw_label(), over the toolkit's
        // wrap helper.
        int max_w = cw - 6;
        const char *rest = name;
        char line[64];
        for (int n = 0; n < IC_LABEL_LINES && *rest; n++) {
            rest = uui_label_wrap_next(rest, max_w, line, sizeof line);
            int ly = y + 2 + px + 2 + n * (ugfx_char_h() + 1);
            int cut = (n == IC_LABEL_LINES - 1) && *rest;
            int ell = ugfx_text_width("..");
            int avail = cut ? max_w - ell : max_w;
            if (avail < ell) avail = ell;
            int tw = ugfx_text_width(line);
            if (tw > avail) tw = avail;
            int lx = x + 3 + (max_w - (tw + (cut ? ell : 0))) / 2;
            if (lx < x + 3) lx = x + 3;
            uint32_t lfg = uui_table_c_fg(t), lbg = bg;
            ugfx_draw_string_clipped(s, lx, ly, avail, line, lfg, lbg);
            if (cut) ugfx_draw_string_clipped(s, lx + tw, ly, ell, "..", lfg, lbg);
        }
    }

    if (ic_bar_visible(fv)) {
        // The thumb glides with the grid: the displacement folded into
        // the bottom-anchored offset.
        int off = ic_offset(fv) + disp;
        if (off < 0) off = 0;
        if (off > ic_max_scroll(fv)) off = ic_max_scroll(fv);
        uui_scrollbar_draw(s, t->x + t->w - t->bar_w, t->y, t->bar_w, t->h,
                            ic_content_h(fv), ic_view_h(fv), off,
                            uui_table_c_track_bg(t), uui_table_c_thumb_bg(t), 0);
    }

    // The band, above everything it crosses. An outline, not a fill --
    // the same call the desktop makes (no alpha blend to fill with).
    int bx, by, bw, bh;
    if (rb_rect(&fv->band, &bx, &by, &bw, &bh))
        ugfx_draw_rect(s, bx, by, bw, bh, uui_table_c_fg(t));

    // No cursor cell on screen (none, or scrolled away): the ring goes
    // round the pane instead.
    if (t->focused && !cursor_drawn) uui_focus_ring(s, t->x, t->y, t->w, t->h);

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
    // FORGOTTEN BEFORE THE SCROLLBAR, which returns early: a thumb drag
    // read the LAST click's row as this press's and dragged that file.
    fv->press_row = -1;
    fv->deferred_clear = 0;

    if (ic_bar_visible(fv) && cx >= t->x + t->w - t->bar_w) {
        int vis = ic_view_h(fv), total = ic_content_h(fv);
        int off = ic_offset(fv);
        enum uui_scrollbar_zone zone =
            uui_scrollbar_hit(t->x + t->w - t->bar_w, t->y, t->bar_w, t->h,
                              total, vis, off, cx, cy, 0);
        if (zone == UUI_SB_THUMB) {
            int thumb_y, thumb_h;
            uui_scrollbar_thumb_rect(t->y, t->h, total, vis, off,
                                      &thumb_y, &thumb_h, t->bar_w, 0);
            t->thumb_grab = cy - thumb_y;
            uui_scrollanim_cancel(&fv->ic_anim); // a drag draws where the thumb is, at once
            return 1;
        }
        // A page is the view less one cell, so the last row seen stays
        // in sight as the first -- Explorer's paging.
        int page = vis > ic_cell_h(fv) ? vis - ic_cell_h(fv) : ic_cell_h(fv);
        if (zone == UUI_SB_ABOVE || zone == UUI_SB_BELOW) uui_scrollanim_arm(&fv->ic_anim);
        if (zone == UUI_SB_ABOVE) ic_set_offset(fv, off + page);
        else if (zone == UUI_SB_BELOW) ic_set_offset(fv, off - page);
        return 1;
    }

    int view = ic_hit_view(fv, cx, cy);
    if (view < 0) {
        fv_empty_press(fv, cx, cy, mods);
        return 1;
    }

    int src = uui_table_source_row(t, view);
    fv->press_row = src;
    int prev = t->selected;
    int changed = (prev != src);
    t->selected = src;

    fv_press_mods(fv, prev, src, mods);
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
        int off = uui_scrollbar_offset_for_drag(t->y, t->h, ic_content_h(fv),
                                                 ic_view_h(fv), cy,
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
    int before = fv->icon_scroll;
    // MINUS, like every scrolling widget here: positive notches mean
    // the wheel rolled AWAY (mouse.c negates the raw byte), which
    // scrolls the view UP. This shipped as += and read exactly like an
    // inverted mouse. Three text lines a notch, the toolkit's one step
    // (ui/uui_anim.h), and it glides.
    uui_scrollanim_arm(&fv->ic_anim);
    fv->icon_scroll -= notches * uui_wheel_step_px();
    ic_clamp(fv);
    return fv->icon_scroll != before;
}

static int ic_key(struct uui_fileview *fv, int key) {
    int cols = ic_cols(fv), vis = ic_vis_rows(fv);
    int rows = uui_fileview_row_count(fv);
    if (rows <= 0) return 0;
    uui_scrollanim_arm(&fv->ic_anim); // a key that scrolls the view glides it

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
    if (fv->table.cols == fv_cols_details) fv->table.col_count = fv_fit_columns(w);
    else if (fv->src && fv->table.cols == fv->src->cols)
        fv->table.col_count = fv_fit_cols(fv->src->cols, fv->src->ncols, w);
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

// --- rename in place --------------------------------------------------

// Where the name being edited is drawn now: the label under an icon, or
// the name cell of a details row. 0 when that row is gone or scrolled
// out of sight -- the field is drawn nowhere then, and still edits.
static int fv_rename_rect(const struct uui_fileview *fv, int *x, int *y, int *w, int *h) {
    const struct uui_table *t = &fv->table;
    int row = uui_fileview_row_of(fv, fv->rename_from);
    if (row < 0) return 0;
    int view = uui_table_view_row(t, row);
    if (view < 0) return 0;
    int fh = ugfx_char_h() + 6;
    if (fv->mode == UUI_FILEVIEW_ICONS) {
        int cx, cy;
        ic_cell_rect(fv, view, &cx, &cy);
        *x = cx + 2;
        *y = cy + 2 + ic_px(fv) + 1;
        *w = ic_cell_w(fv) - 6;
        *h = fh;
        return 1;
    }
    int rh = uui_table_row_h(t), hh = uui_table_header_h(t);
    if (view < t->top || (view - t->top + 1) * rh > t->h - hh) return 0;
    int cx, cw;
    uui_table_column_rect(t, 0, &cx, &cw);
    int icon = t->icon ? ugfx_char_h() + UUI_TABLE_PAD_X : 0;
    *x = cx + UUI_TABLE_PAD_X + icon - 3;
    *y = t->y + hh + (view - t->top) * rh + (rh - fh) / 2;
    *w = cw - UUI_TABLE_PAD_X - icon;
    *h = fh;
    return 1;
}

int uui_fileview_begin_rename(struct uui_fileview *fv) {
    const struct sys_dirent *e = uui_fileview_selected_entry(fv);
    if (!e) return 0;
    snprintf(fv->rename_from, sizeof fv->rename_from, "%s", e->name);
    uui_textbox_init(&fv->rename_box, e->name);
    uui_textbox_set_active(&fv->rename_box, 1);
    // The NAME selected, not the extension: typing replaces "dusk" and
    // keeps ".jpg" -- what every desktop does, and what keeps the file
    // opening with the same app.
    int n = (int)strlen(e->name), stem = n;
    const char *dot = strrchr(e->name, '.');
    if (!e->is_dir && dot && dot != e->name) stem = (int)(dot - e->name);
    uui_textbox_select(&fv->rename_box, 0, stem);
    fv->renaming = 1;
    fv->rename_parked = 0;
    if (fv->mode == UUI_FILEVIEW_ICONS) ic_reveal(fv);
    return 1;
}

int uui_fileview_renaming(struct uui_fileview *fv) {
    // The file went (another program, a reload): nothing left to name.
    if (fv->renaming && uui_fileview_row_of(fv, fv->rename_from) < 0) fv->renaming = 0;
    return fv->renaming;
}

void uui_fileview_cancel_rename(struct uui_fileview *fv) {
    fv->renaming = 0;
    uui_textbox_set_active(&fv->rename_box, 0);
}

// Parks the typed name when it is a change; an unchanged or empty one is
// simply the edit ending.
static void fv_rename_commit(struct uui_fileview *fv) {
    const char *to = uui_textbox_text(&fv->rename_box);
    if (to[0] && strcmp(to, fv->rename_from) != 0) {
        snprintf(fv->rename_to, sizeof fv->rename_to, "%s", to);
        fv->rename_parked = 1;
    }
    uui_fileview_cancel_rename(fv);
}

void uui_fileview_finish_rename(struct uui_fileview *fv) {
    if (fv->renaming) fv_rename_commit(fv);
}

int uui_fileview_take_rename(struct uui_fileview *fv, char *from, char *to, int cap) {
    if (!fv->rename_parked) return 0;
    fv->rename_parked = 0;
    snprintf(from, (size_t)cap, "%s", fv->rename_from);
    snprintf(to, (size_t)cap, "%s", fv->rename_to);
    return 1;
}

static void fv_draw_rename(struct ugfx_surface *s, const struct uui_fileview *fv) {
    if (!fv->renaming) return;
    int x, y, w, h;
    if (!fv_rename_rect(fv, &x, &y, &w, &h)) return;
    struct uui_fileview *mut = (struct uui_fileview *)fv;   // the field's geometry is the draw's
    uui_textbox_set_geometry(&mut->rename_box, x, y, w, h);
    mut->rename_box.border = UTHEME_ACCENT;
    ugfx_set_clip_rect(s, fv->table.x, fv->table.y, fv->table.w, fv->table.h);
    uui_textbox_draw(s, &fv->rename_box);
    ugfx_clear_clip_rect(s);
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
            ugfx_draw_rect(s, bx, by, bw, bh, uui_table_c_fg(t));
            ugfx_clear_clip_rect(s);
        }
    }
    fv_draw_drop(s, fv);
    fv_draw_rename(s, fv);
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
// unsorted array happens to hold. `prev` is the cursor BEFORE this
// press moved it (-1 when there was none).
//
// Returns 1 if anything changed. The ".." row is never markable -- it
// is not a file, and an operation over a set containing it would act on
// the parent directory.
static int fv_apply_mods(struct uui_fileview *fv, int prev, int row, unsigned mods) {
    if (fv_is_up_row(fv, row)) { fv->anchor = -1; return 0; }

    if (mods & KEY_MOD_CTRL) {
        // A LONE SELECTION IS THE CURSOR, UNMARKED, and Ctrl ADDS to the
        // selection -- so the cursor joins the set before the new row.
        // Without this the first Ctrl+click dropped the item selected
        // before it.
        if (fv->mark_count == 0 && prev >= 0 && prev != row)
            uui_fileview_toggle_mark(fv, prev);
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
    // A press in the field being edited places the caret there; anywhere
    // else it ENDS the edit, keeping what was typed -- Explorer's rule --
    // and then does what it would have done.
    if (fv->renaming) {
        if (uui_textbox_hit(&fv->rename_box, cx, cy))
            return uui_textbox_ops.press(&fv->rename_box, cx, cy, mods) || 1;
        fv_rename_commit(fv);
    }
    fv->press_plain = !(mods & (KEY_MOD_CTRL | KEY_MOD_SHIFT));
    fv->dragged = 0;
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
    int prev = fv->table.selected;   // before the click moves the cursor
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
    int set_changed = fv_press_mods(fv, prev, row, mods);
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
    // The name being edited takes every key: Enter keeps it, Esc drops it.
    if (fv->renaming) {
        if (key == '\n' || key == '\r') fv_rename_commit(fv);
        else if (key == 0x1B) uui_fileview_cancel_rename(fv);
        else uui_textbox_key(&fv->rename_box, key);
        return 1;
    }
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
    struct uui_fileview *fv = (struct uui_fileview *)w;
    uui_fileview_drag_end(fv);
    // SINGLE-CLICK MODE opens on the release of a plain click that stayed
    // on its row -- not on the press, which would open every file a drag
    // started from.
    if (fv->single_click && fv->press_plain && !fv->dragged && fv->press_row >= 0 &&
        !fv->renaming) {
        int row = fv->mode == UUI_FILEVIEW_ICONS
                      ? uui_table_source_row(&fv->table, ic_hit_view(fv, cx, cy))
                      : uui_table_hit(&fv->table, cx, cy);
        if (row == fv->press_row) {
            fv->deferred_clear = 0;
            fv->press_row = -1;
            return uui_fileview_activate(fv) || 1;
        }
    }
    // The press was a click after all: the deferred plain-click clear.
    if (fv->deferred_clear && fv->press_row >= 0) {
        fv->deferred_clear = 0;
        return fv_apply_mods(fv, -1, fv->press_row, 0);
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
    if (fv->src) return 0;   // a virtual folder's rows are not files to carry

    // This press is a drag now, not a click: no band, no deferred
    // clear, and the next click is not a double.
    rb_clear(&fv->band);
    fv->deferred_clear = 0;
    fv->last_click_row = -1;
    fv->dragged = 1;

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

    // THE DRAG SLOT, for a drop in ANOTHER window: the paths, as the
    // clipboard would carry them. Local targets never read it.
    static struct uclip c;   // 64 KiB, lib/uclip.h says why static
    uclip_drag_begin(&c);
    char path[UUI_FILEVIEW_PATH_MAX];
    if (fv->mark_count) {
        for (int i = 0; i < fv->mark_count; i++)
            if (uui_fileview_marked_path(fv, i, path, sizeof path)) uclip_add(&c, path);
    } else if (uui_fileview_selected_path(fv, path, sizeof path)) {
        uclip_add(&c, path);
    }
    (void)uclip_drag_commit(&c);
    return 1;
}

static void fv_ops_drag_end(void *w, int dropped) {
    (void)dropped;
    ((struct uui_fileview *)w)->deferred_clear = 0;
}

static int fv_ops_drag_over(void *w, int cx, int cy, const struct uui_drag *d) {
    struct uui_fileview *fv = (struct uui_fileview *)w;
    fv->drop_row = -2;
    if (d->kind != UUI_DRAG_FILES || cx == UUI_NOWHERE || fv->src) return 0;

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
    const struct uimg *ico = icon_get(d->count == 1 ? ufiletype_icon(d->label, is_dir)
                                                    : "file", px);
    int tw = ugfx_text_width(d->label);
    int gx = d->x + 12, gy = d->y + 12;
    int gw = px + 6 + tw + 8, gh = px + 6;
    ugfx_fill_rect(s, gx, gy, gw, gh, uui_table_c_sel_bg(t));
    ugfx_draw_rect(s, gx, gy, gw, gh, uui_table_c_fg(t));
    if (ico) ugfx_blit_alpha(s, gx + 3, gy + 3, ico->w, ico->h, ico->px, ico->w);
    ugfx_draw_string(s, gx + px + 6, gy + (gh - ugfx_char_h()) / 2, d->label,
                     uui_table_c_sel_fg(t), uui_table_c_sel_bg(t));
    if (d->copy)
        ugfx_draw_string(s, gx + gw + 2, gy, "+", uui_table_c_fg(t), uui_table_c_bg(t));
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
