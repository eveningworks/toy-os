// fileview -- a directory listing, as a widget. See ui/uui_fileview.h.
#include "ui/uui_fileview.h"
#include "ui/uui_widget.h"
#include "rt/sys.h"     // sys_listdir(), sys_ticks()
#include "kpath.h"      // k_path_join/_dirname -- the KERNEL's, linked into ring 3
#include "lib/human.h"  // human_size()
#include <string.h>
#include <stdio.h>
#include "keyboard.h"   // KEY_* codes, as delivered by WIN_EV_KEY

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
        // A trailing '/' marks a directory, which is `ls -F`'s answer
        // and needs no artwork -- icon_get() is WM-internal today (see
        // docs/filemanager-design.md's open questions).
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
    // Marked rows read as marked on this near-white theme by being
    // WARMER, not lighter -- the same reasoning ui_state_bg() applies
    // to hover (docs/gui-guidelines.md).
    fv->mark_bg = ugfx_rgb(250, 232, 190);
    uui_table_set_sort(&fv->table, FV_COL_NAME, 1);
    // Typing a letter seeks by NAME, in both modes. Stated rather than
    // left to the default, so a reordered column list moves it too.
    uui_table_set_seek_col(&fv->table, FV_COL_NAME);
}

void uui_fileview_set_mode(struct uui_fileview *fv, enum uui_fileview_mode mode) {
    fv->mode = mode;
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

    fv->count = 0;
    fv->truncated = 0;
    fv->failed = 0;
    uui_fileview_clear_marks(fv); // see uui_fileview.h -- a mark names a ROW
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
    } else {
        fv->table.selected = uui_fileview_row_count(fv) > 0
                              ? uui_table_source_row(&fv->table, 0) : -1;
    }
    fv->last_click_row = -1;
    return !fv->failed;
}

int uui_fileview_set_dir(struct uui_fileview *fv, const char *dir) {
    if (!dir || !dir[0]) return 0;
    snprintf(fv->dir, sizeof fv->dir, "%s", dir);
    // Nothing to keep across a directory CHANGE -- reload()'s
    // preserve-by-name is for a refresh of the same directory.
    fv->table.selected = -1;
    fv->table.top = 0;
    int ok = uui_fileview_reload(fv);
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

// --- the direct interface ---------------------------------------------

void uui_fileview_set_geometry(struct uui_fileview *fv, int x, int y, int w, int h) {
    fv->table.x = x; fv->table.y = y; fv->table.w = w; fv->table.h = h;
    uui_table_set_rows(&fv->table, uui_fileview_row_count(fv));
}

void uui_fileview_draw(struct ugfx_surface *s, const struct uui_fileview *fv) {
    uui_table_draw(s, &fv->table);
}

void uui_fileview_natural_size(const struct uui_fileview *fv, int *out_w, int *out_h) {
    uui_table_natural_size(&fv->table, out_w, out_h);
}

int uui_fileview_hit(const struct uui_fileview *fv, int cx, int cy) {
    return uui_table_hit(&fv->table, cx, cy);
}

int uui_fileview_hover(struct uui_fileview *fv, int cx, int cy) {
    return uui_table_hover(&fv->table, cx, cy);
}

int uui_fileview_press(struct uui_fileview *fv, int cx, int cy) {
    // A PRESS ANYWHERE INSIDE IS CONSUMED, even on the empty space below
    // the last row. Returning 0 there does two things a caller cannot
    // work around: the router does not take the pointer grab, so no
    // release is delivered, and it never NAMES this widget to the app
    // (ui/uui_route.c) -- which is how clicking the blank part of a file
    // manager pane failed to make that pane the active one, while
    // clicking a row worked.
    int inside = uui_hit(fv->table.x, fv->table.y, fv->table.w, fv->table.h, cx, cy);

    if (uui_table_press(&fv->table, cx, cy)) return 1; // the scrollbar

    int row = uui_table_hit(&fv->table, cx, cy);
    int changed = uui_table_click(&fv->table, cx, cy);
    if (row < 0) return changed || inside;

    unsigned long now = sys_ticks();
    int is_double = (row == fv->last_click_row &&
                      now - fv->last_click_tick <= UUI_FILEVIEW_DOUBLE_CLICK_TICKS);
    fv->last_click_tick = now;
    // A third click inside the window must not re-trigger as a second
    // double -- the same guard the desktop's icons carry.
    fv->last_click_row = is_double ? -1 : row;

    if (is_double) return uui_fileview_activate(fv) || changed;
    if (changed) fv_report_select(fv);
    return changed || inside;
}

int uui_fileview_drag(struct uui_fileview *fv, int cx, int cy) {
    return uui_table_drag(&fv->table, cx, cy);
}

void uui_fileview_drag_end(struct uui_fileview *fv) {
    uui_table_drag_end(&fv->table);
}

int uui_fileview_wheel(struct uui_fileview *fv, int notches) {
    return uui_table_wheel(&fv->table, notches);
}

int uui_fileview_key(struct uui_fileview *fv, int key) {
    if (key == '\n' || key == '\r') return uui_fileview_activate(fv);
    if (key == '\b') return uui_fileview_up(fv);
    if (key == KEY_INSERT || key == ' ') {
        // Toggle and STEP DOWN, which is what makes marking a run of
        // files one repeated keystroke -- every commander does this, and
        // a toggle that stayed put would need two keys per file.
        if (!uui_fileview_toggle_mark(fv, fv->table.selected)) return 0;
        uui_table_key(&fv->table, KEY_ARROW_DOWN);
        return 1;
    }
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

static int fv_ops_press(void *w, int cx, int cy) {
    return uui_fileview_press((struct uui_fileview *)w, cx, cy);
}

static int fv_ops_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_fileview *fv = (struct uui_fileview *)w;
    if (buttons & 1) return uui_fileview_drag(fv, cx, cy);
    return uui_fileview_hover(fv, cx, cy);
}

static int fv_ops_release(void *w, int cx, int cy) {
    (void)cx; (void)cy;
    uui_fileview_drag_end((struct uui_fileview *)w);
    return 0;
}

static int fv_ops_wheel(void *w, int notches) {
    return uui_fileview_wheel((struct uui_fileview *)w, notches);
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
};
