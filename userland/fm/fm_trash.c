// The Recycle Bin as the File Manager shows it: the "trash:/" virtual
// folder -- a uui_fileview source over lib/utrash -- and its verbs,
// Restore and Empty. Deleting INTO the bin is the job worker's
// (fm_jobs.c); this is the bin's own side.
//
// Every bin on every volume is one folder here, newest first, as
// Explorer's Recycle Bin and KDE's trash:/ both show them.
#include "fm_internal.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <caltime.h>
#include "kpath.h"
#include "lib/utrash.h"
#include "lib/human.h"
#include "lib/udate.h"
#include "ui/uui_table.h"
#include "ui/ulog.h"

// ONE LISTING FOR BOTH PANES: a pane showing the bin lists into it, and
// two panes showing it list the same thing in the same order, so either
// may have written it last. Static: 256 items is ~150 KB.
static struct utrash_item g_items[PANE_FILES];
static int g_count;

// The order is the details view's sort keys: Size and Modified land on
// Size and Date deleted, and the fourth column is where it came from.
static const struct uui_table_column bin_cols[] = {
    { "Name",         0,  UUI_TALIGN_LEFT  },
    { "Size",         9,  UUI_TALIGN_RIGHT },
    { "Date deleted", 20, UUI_TALIGN_LEFT  },
    { "Deleted from", 22, UUI_TALIGN_LEFT  },
};

static int bin_list(void *ctx, const char *dir, struct sys_dirent *out, int cap) {
    (void)ctx; (void)dir;
    g_count = utrash_list(g_items, cap < PANE_FILES ? cap : PANE_FILES);
    for (int i = 0; i < g_count; i++) {
        memset(&out[i], 0, sizeof out[i]);
        strlcpy(out[i].name, g_items[i].name, sizeof out[i].name);
        out[i].size = g_items[i].size > 0xffffffffull ? 0xffffffffu : (uint32_t)g_items[i].size;
        out[i].is_dir = (uint32_t)g_items[i].is_dir;
        out[i].modified = g_items[i].deleted;
    }
    return g_count;
}

static int bin_path(void *ctx, int i, char *out, int cap) {
    (void)ctx;
    return i >= 0 && i < g_count && utrash_item_path(&g_items[i], out, cap);
}

static void bin_cell(void *ctx, int i, int col, char *out, int cap) {
    (void)ctx;
    out[0] = '\0';
    if (i < 0 || i >= g_count) return;
    const struct utrash_item *it = &g_items[i];
    if (col == 1 && !it->is_dir) human_size(out, (unsigned long)cap, it->size);
    else if (col == 2) udate_format(out, (unsigned long)cap, &it->deleted, UDATE_DATE | UDATE_TIME);
    else if (col == 3) k_path_dirname(it->orig, out, (size_t)cap);
}

// By the value, not the text: a date written the region's way does not
// sort as a string.
static int bin_compare(void *ctx, int a, int b, int col) {
    (void)ctx;
    const struct utrash_item *x = &g_items[a], *y = &g_items[b];
    if (col == 1) return x->size < y->size ? -1 : x->size > y->size;
    if (col == 2) {
        uint64_t tx = cal_rtc_to_epoch(&x->deleted), ty = cal_rtc_to_epoch(&y->deleted);
        return tx < ty ? -1 : tx > ty;
    }
    return strcmp(x->orig, y->orig);
}

static const struct uui_fileview_source g_bin_src = {
    .list = bin_list, .path = bin_path,
    .cols = bin_cols, .ncols = 4,
    .cell = bin_cell, .compare = bin_compare,
};

const struct uui_fileview_source *bin_source(void) { return &g_bin_src; }

int in_bin(const struct uui_fileview *fv) { return uui_fileview_source(fv) == &g_bin_src; }

int bin_count(void) { return g_count; }

// The menus' and the command bar's state while the active pane shows the
// bin: nothing is made, pasted, renamed or moved INTO a bin, and Up from
// it goes nowhere. 0 leaves `code` to files.c's own answer.
int bin_item_flags(int code, unsigned *out) {
    if (!in_bin(active())) return 0;
    switch (code) {
    case CMD_UP: case CMD_MENU_NEW: case CMD_MKDIR: case CMD_NEW_FILE:
    case CMD_CLIP_CUT: case CMD_CLIP_PASTE: case CMD_RENAME: case CMD_EDIT:
    case CMD_MOVE:
        *out = UUI_MI_DISABLED;
        return 1;
    case CMD_RESTORE: case CMD_DELETE_FOREVER: case CMD_DELETE:
        *out = operand_count() > 0 ? 0 : UUI_MI_DISABLED;
        return 1;
    case CMD_RESTORE_ALL: case CMD_EMPTY_BIN:
        *out = g_count > 0 ? 0 : UUI_MI_DISABLED;
        return 1;
    default:
        return 0;
    }
}

// What a pane showing `dir` watches for a change (files.c's on_tick):
// the system volume's bin's info folder, which every put, restore and
// empty there touches.
const char *fm_watch_path(const char *dir) {
    return strcmp(dir, FM_BIN) == 0 ? "/home/.Trash/info" : dir;
}

// The item a path inside a bin names; -1 for none.
static int item_of(const char *path) {
    char p[PATH_MAX_LEN];
    for (int i = 0; i < g_count; i++)
        if (utrash_item_path(&g_items[i], p, sizeof p) && !strcmp(p, path)) return i;
    return -1;
}

static int restore_one(int i, char *why, int cap) {
    int e = utrash_restore(&g_items[i]);
    if (!e) return 1;
    const char *name = k_path_basename(g_items[i].orig);
    if (e == -EEXIST) snprintf(why, (size_t)cap, "%s: something named that is in the way", name);
    else if (e == -ENOENT) snprintf(why, (size_t)cap, "%s: the folder it came from is gone", name);
    else snprintf(why, (size_t)cap, "%s: could not restore", name);
    ulogf("files: restore %s -> %s failed, errno %d\n", g_items[i].name, g_items[i].orig, -e);
    return 0;
}

// Restore the selection (or the marks), or, with `all`, everything.
// Renames, so it runs here rather than on the worker: it is instant.
void bin_restore(int all) {
    struct uui_fileview *fv = active();
    if (!in_bin(fv)) return;
    char why[96] = "";
    int done = 0, failed = 0;
    if (all) {
        for (int i = 0; i < g_count; i++)
            if (restore_one(i, why, sizeof why)) done++; else failed++;
    } else {
        char path[PATH_MAX_LEN];
        int marks = uui_fileview_mark_count(fv);
        for (int m = 0; m < (marks ? marks : 1); m++) {
            int ok = marks ? uui_fileview_marked_path(fv, m, path, sizeof path)
                           : uui_fileview_selected_path(fv, path, sizeof path);
            int i = ok ? item_of(path) : -1;
            if (i < 0) continue;
            if (restore_one(i, why, sizeof why)) done++; else failed++;
        }
    }
    if (!done && !failed) set_note("nothing selected");
    else if (failed) snprintf(g_stat_note, sizeof g_stat_note, "%s", why);
    else snprintf(g_stat_note, sizeof g_stat_note, "restored %d item%s", done, done == 1 ? "" : "s");
    ulogf("files: restored %d, %d refused\n", done, failed);
    reload_panes();
    refresh_status();
}

// Every item's path inside its bin, for Empty's job.
int bin_paths(char (*out)[PATH_MAX_LEN], int cap) {
    int n = 0;
    for (int i = 0; i < g_count && n < cap; i++)
        if (utrash_item_path(&g_items[i], out[n], PATH_MAX_LEN)) n++;
    return n;
}

unsigned long long bin_bytes(void) {
    unsigned long long b = 0;
    for (int i = 0; i < g_count; i++) b += g_items[i].size;
    return b;
}
