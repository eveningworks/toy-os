// Recent as the File Manager shows it: the "recent:/" virtual folder --
// a uui_fileview source over lib/urecent -- banded by day, newest first,
// with the app each file was opened in. Explorer's Recent and KDE's
// recentlyused:/. Its verbs are Open folder, Remove from list and Clear
// list: the rows are the files themselves, but nothing here deletes or
// renames one -- Delete takes a row off the list, as Explorer's does.
#include "fm_internal.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <caltime.h>
#include "kpath.h"
#include "lib/urecent.h"
#include "lib/human.h"
#include "lib/udate.h"
#include "ui/uui_table.h"
#include "ui/ulog.h"

// ONE LISTING FOR BOTH PANES, as the bin's (fm_trash.c).
static struct urecent_item g_items[URECENT_MAX];
static int g_count;

enum { G_TODAY, G_YESTERDAY, G_WEEK, G_EARLIER };

// Name / Opened in / Time / Folder, the mockup's order; Time sits on the
// Modified column's index, so a sort by date stays one in both.
static const struct uui_table_column recent_cols[] = {
    { "Name",      0,  UUI_TALIGN_LEFT },
    { "Opened in", 14, UUI_TALIGN_LEFT },
    { "Time",      16, UUI_TALIGN_LEFT },
    { "Folder",    22, UUI_TALIGN_LEFT },
};

// Local days since the epoch, for "today" and "yesterday".
static long local_day(long long when) {
    time_t t = (time_t)when;
    struct tm tm;
    localtime_r(&t, &tm);
    return (long)tm.tm_year * 366 + tm.tm_yday;   // ordered, and equal on the same day
}

static int day_group(long long when) {
    struct tm a, b;
    time_t now = time(0), t = (time_t)when;
    localtime_r(&now, &a);
    localtime_r(&t, &b);
    if (local_day(when) == local_day((long long)now)) return G_TODAY;
    if (local_day(when) == local_day((long long)now - 86400)) return G_YESTERDAY;
    if ((long long)now - when < 7LL * 86400) return G_WEEK;
    return G_EARLIER;
}

static int recent_list(void *ctx, const char *dir, struct sys_dirent *out, int cap) {
    (void)ctx; (void)dir;
    g_count = urecent_list(g_items, cap < URECENT_MAX ? cap : URECENT_MAX);
    for (int i = 0; i < g_count; i++) {
        memset(&out[i], 0, sizeof out[i]);
        strlcpy(out[i].name, k_path_basename(g_items[i].path), sizeof out[i].name);
        struct sys_stat st;
        if (sys_stat(g_items[i].path, &st) == 0) {
            out[i].size = st.size;
            out[i].is_dir = st.is_dir;
        }
        cal_epoch_to_rtc((uint64_t)g_items[i].when, &out[i].modified);
    }
    return g_count;
}

static int recent_path(void *ctx, int i, char *out, int cap) {
    (void)ctx;
    return i >= 0 && i < g_count && strlcpy(out, g_items[i].path, (size_t)cap) < (size_t)cap;
}

static void recent_cell(void *ctx, int i, int col, char *out, int cap) {
    (void)ctx;
    out[0] = '\0';
    if (i < 0 || i >= g_count) return;
    const struct urecent_item *it = &g_items[i];
    if (col == 1) strlcpy(out, it->app, (size_t)cap);
    else if (col == 2) {
        struct rtc_time t;
        cal_epoch_to_rtc((uint64_t)it->when, &t);
        int g = day_group(it->when);   // the band already says which day
        udate_format(out, (unsigned long)cap, &t, g <= G_YESTERDAY ? UDATE_TIME : UDATE_DATE | UDATE_TIME);
    } else if (col == 3) k_path_dirname(it->path, out, (size_t)cap);
}

static int recent_compare(void *ctx, int a, int b, int col) {
    (void)ctx;
    const struct urecent_item *x = &g_items[a], *y = &g_items[b];
    if (col == 1) return strcmp(x->app, y->app);
    if (col == 2) return x->when < y->when ? -1 : x->when > y->when;
    return strcmp(x->path, y->path);
}

static int recent_group(void *ctx, int i) {
    (void)ctx;
    return i >= 0 && i < g_count ? day_group(g_items[i].when) : G_EARLIER;
}

static void recent_group_title(void *ctx, int g, char *out, int cap) {
    (void)ctx;
    static const char *const names[] = { "Today", "Yesterday", "Earlier this week", "Earlier" };
    strlcpy(out, g >= 0 && g <= G_EARLIER ? names[g] : "", (size_t)cap);
}

static const struct uui_fileview_source g_recent_src = {
    .list = recent_list, .path = recent_path,
    .cols = recent_cols, .ncols = 4,
    .cell = recent_cell, .compare = recent_compare,
    .group = recent_group, .group_title = recent_group_title,
    .sort_col = 2, .sort_dir = -1,   // newest first
};

const struct uui_fileview_source *recent_source(void) { return &g_recent_src; }

int in_recent(const struct uui_fileview *fv) { return uui_fileview_source(fv) == &g_recent_src; }

// The menus' and the command bar's state while the active pane shows
// Recent: nothing is made, pasted, renamed or moved here, and Delete is
// Remove from list. 0 leaves `code` to files.c's own answer.
int recent_item_flags(int code, unsigned *out) {
    if (!in_recent(active())) return 0;
    switch (code) {
    case CMD_UP: case CMD_MENU_NEW: case CMD_MKDIR: case CMD_NEW_FILE:
    case CMD_CLIP_CUT: case CMD_CLIP_PASTE: case CMD_RENAME: case CMD_MOVE:
    case CMD_DELETE_FOREVER:
        *out = UUI_MI_DISABLED;
        return 1;
    case CMD_RECENT_FOLDER:
        *out = uui_fileview_selected_name(active()) ? 0 : UUI_MI_DISABLED;
        return 1;
    case CMD_RECENT_FORGET: case CMD_DELETE:
        *out = operand_count() > 0 ? 0 : UUI_MI_DISABLED;
        return 1;
    case CMD_RECENT_CLEAR:
        *out = g_count > 0 ? 0 : UUI_MI_DISABLED;
        return 1;
    default:
        return 0;
    }
}

// Remove from list: every marked row, or the one selected.
void recent_forget(void) {
    static char paths[PANE_FILES][PATH_MAX_LEN];
    struct uui_fileview *fv = active();
    int marks = uui_fileview_mark_count(fv), n = 0, done = 0;
    for (int i = 0; i < marks && n < PANE_FILES; i++)
        if (uui_fileview_marked_path(fv, i, paths[n], PATH_MAX_LEN)) n++;
    if (!marks && uui_fileview_selected_path(fv, paths[0], PATH_MAX_LEN)) n = 1;
    for (int i = 0; i < n; i++)
        if (urecent_forget(paths[i]) == 0) done++;
    snprintf(g_stat_note, sizeof g_stat_note, "removed %d from Recent", done);
    ulogf("files: recent forget %d of %d\n", done, n);
    reload_panes();
}

void recent_clear(void) {
    if (urecent_clear() != 0) { set_note("could not clear Recent"); return; }
    set_note("Recent cleared");
    ulogf("files: recent cleared\n");
    reload_panes();
}

// Open folder: the folder the selected file is in, with it selected.
void recent_open_folder(void) {
    char path[PATH_MAX_LEN], dir[PATH_MAX_LEN];
    if (!uui_fileview_selected_path(active(), path, sizeof path) ||
        !k_path_dirname(path, dir, sizeof dir)) { set_note("select a file"); return; }
    if (fm_goto(g_active, dir)) uui_fileview_select_name(active(), k_path_basename(path));
}
