// Task Manager's Processes page: the table, its command bar, and the
// details pane. See userland/gui/system/taskmgr.c for the shell.
//
// THE TABLE IS uui_table's GROUPS AND TREE, and this file only answers
// questions about rows: which group, which parent, is it folded, how
// hot is this cell. Three views over the same rows, KDE System
// Monitor's list/tree switch plus Windows' grouping:
//
//   Grouped -- Apps / Background processes / System, each a tree.
//   Tree    -- the whole parent tree from init.
//   List    -- flat, sorted.
//
// ROWS ARE g_proc INDEXES THROUGH A FILTER (g_rows), and the SELECTION
// IS A PID. A table row index moves whenever a process starts or exits;
// holding it across a refresh put the selection on whichever process
// slid into that slot, and a Force Quit armed on one process could have
// landed on its neighbour.
//
// TWO WAYS TO END A PROCESS, as Windows separates them: Close asks the
// window to close (the X button's handshake, which an app may decline);
// Force Quit is SIGKILL. Both ARM on the first click and commit on the
// second -- docs/gui-guidelines.md's press-then-commit rule standing in
// for a confirmation dialog. Stop and Continue are SIGSTOP/SIGCONT and
// need no confirmation: each undoes the other.
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "rt/sys.h"
#include "ui/ulog.h"
#include "ui/ugfx.h"
#include "ui/uapp.h"
#include "ui/utheme.h"
#include "ui/uui_widget.h"
#include "ui/uui_route.h"
#include "ui/uui_layout.h"
#include "ui/uui_table.h"
#include "ui/uui_button.h"
#include "ui/uui_textbox.h"
#include "ui/uui_segmented.h"
#include "ui/uui_label.h"
#include "ui/uui_chart.h"
#include "ui/uui_statusbar.h"
#include "ui/uui_menubar.h"
#include "ui/uui_focus.h"
#include "lib/human.h"
#include "lib/icon_cache.h"
#include "keyboard.h"
#include "taskmgr/tm_internal.h"

enum {
    ID_TABLE = TM_ID_PROCS, ID_SEARCH, ID_VIEW, ID_STOP, ID_CLOSE, ID_KILL,
};
// Context-menu codes. The toolbar's buttons carry the same verbs.
enum { CMD_CLOSE = 1, CMD_STOP, CMD_CONTINUE, CMD_KILL, CMD_LOCATE, CMD_SERVICE };

enum { VIEW_GROUPED, VIEW_TREE, VIEW_LIST };
static const char *const VIEW_NAMES[] = { "Grouped", "Tree", "List" };

// --- rows ----------------------------------------------------------------

static int g_rows[SYS_PROC_MAX];   // g_proc index per table row
static int g_nrows;
static int g_group_count[TM_GROUPS];
static int g_sel_pid;              // the selection, by pid; 0 = none
static int g_view = VIEW_GROUPED;

// Folded parents, by pid, for the same reason the selection is a pid.
static int g_folded[SYS_PROC_MAX];
static int g_nfolded;

static struct tm_proc *row_proc(int row) {
    return (row >= 0 && row < g_nrows) ? &g_proc[g_rows[row]] : 0;
}

static int row_of_pid(int pid) {
    for (int r = 0; r < g_nrows; r++)
        if (g_proc[g_rows[r]].pid == pid) return r;
    return -1;
}

static int is_folded(int pid) {
    for (int i = 0; i < g_nfolded; i++) if (g_folded[i] == pid) return 1;
    return 0;
}

static void toggle_fold(int pid) {
    for (int i = 0; i < g_nfolded; i++)
        if (g_folded[i] == pid) { g_folded[i] = g_folded[--g_nfolded]; return; }
    if (g_nfolded < SYS_PROC_MAX) g_folded[g_nfolded++] = pid;
}

// --- the table ----------------------------------------------------------

enum { COL_NAME = 0, COL_PID, COL_STATUS, COL_CPU, COL_MEM };
// The machine's totals are the status bar's, not the headers': a title
// that grows a digit when the load does is clipped by the sort arrow.
static const struct uui_table_column COLUMNS[] = {
    { "Name",   0, UUI_TALIGN_LEFT  },
    { "PID",    6, UUI_TALIGN_RIGHT },
    { "Status", 18, UUI_TALIGN_LEFT },
    { "CPU",    7, UUI_TALIGN_RIGHT },
    { "Memory", 10, UUI_TALIGN_RIGHT },
};
#define COL_COUNT ((int)(sizeof COLUMNS / sizeof COLUMNS[0]))

static struct uui_table g_table;

static void cell(void *ctx, int row, int col, char *out, int cap) {
    (void)ctx;
    const struct tm_proc *p = row_proc(row);
    out[0] = '\0';
    if (!p) return;
    switch (col) {
    case COL_NAME:   strlcpy(out, p->title[0] ? p->title : "(unnamed)", (size_t)cap); break;
    case COL_PID:    snprintf(out, (size_t)cap, "%d", p->pid); break;
    case COL_STATUS: strlcpy(out, tm_status_text(p), (size_t)cap); break;
    case COL_CPU:
        // Tenths below ten percent, Windows' precision: a busy idle
        // machine is all "0%" otherwise.
        if (p->cpu_pm < 100) snprintf(out, (size_t)cap, "%u.%u%%", p->cpu_pm / 10, p->cpu_pm % 10);
        else snprintf(out, (size_t)cap, "%u%%", p->cpu_pm / 10);
        unum_localize(out, (unsigned long)cap, 0);
        break;
    case COL_MEM:    human_size_iec(out, cap, p->mem_bytes); break;
    default: break;
    }
}

// On the REAL values, never the formatted cells (uui_table.h).
static int compare_rows(void *ctx, int a, int b, int col) {
    (void)ctx;
    const struct tm_proc *x = row_proc(a), *y = row_proc(b);
    switch (col) {
    case COL_NAME:   return strcasecmp(x->title, y->title);
    case COL_PID:    return x->pid - y->pid;
    case COL_STATUS: return strcmp(tm_status_text(x), tm_status_text(y));
    case COL_CPU:    return (int)x->cpu_pm - (int)y->cpu_pm;
    case COL_MEM:
        // NOT a subtraction: a difference that does not fit an int makes
        // the order depend on how far apart two values happen to be.
        if (x->mem_bytes < y->mem_bytes) return -1;
        return x->mem_bytes > y->mem_bytes ? 1 : 0;
    default:         return 0;
    }
}

static int group_of(void *ctx, int row) { (void)ctx; return row_proc(row)->group; }

static void group_title(void *ctx, int g, char *out, int cap) {
    (void)ctx;
    static const char *const NAMES[TM_GROUPS] = { "Apps", "Background processes", "System" };
    if (g < 0 || g >= TM_GROUPS) { out[0] = '\0'; return; }
    snprintf(out, (size_t)cap, "%s (%d)", NAMES[g], g_group_count[g]);
}

static int parent_of(void *ctx, int row) {
    (void)ctx;
    return row_of_pid(row_proc(row)->ppid);   // -1 when filtered out or gone
}

static int folded_of(void *ctx, int row) { (void)ctx; return is_folded(row_proc(row)->pid); }

// Windows' heat map, in the theme's accent: a busy CPU cell and a large
// Memory cell stand out without reading a single number. Bands rather
// than a continuous ramp, so a process flickering between 0.2% and 0.4%
// does not shimmer.
static int heat_of(void *ctx, int row, int col) {
    (void)ctx;
    const struct tm_proc *p = row_proc(row);
    if (col == COL_CPU) {
        unsigned pm = p->cpu_pm;
        return pm < 5 ? 0 : pm < 50 ? 60 : pm < 150 ? 120 : pm < 400 ? 180 : 255;
    }
    if (col == COL_MEM && g_mem_total) {
        unsigned long long pm = p->mem_bytes * 1000ULL / g_mem_total;
        return pm < 2 ? 0 : pm < 10 ? 60 : pm < 30 ? 120 : pm < 100 ? 180 : 255;
    }
    return 0;
}

static const struct uimg *icon_of(void *ctx, int row, int px) {
    (void)ctx;
    const struct tm_proc *p = row_proc(row);
    return icon_get(p->app >= 0 && g_apps[p->app].icon[0] ? g_apps[p->app].icon : "file-app", px);
}

static void apply_view(void) {
    uui_table_set_groups(&g_table, g_view == VIEW_GROUPED ? group_of : 0, group_title);
    uui_table_set_tree(&g_table, g_view == VIEW_LIST ? 0 : parent_of, folded_of);
}

// --- the command bar -----------------------------------------------------

static struct uui_textbox g_search;
static struct uui_segmented g_viewsel;
static struct uui_button g_btn_stop, g_btn_close, g_btn_kill;
static struct uui_label g_spacer;
static int g_armed;   // ID_CLOSE or ID_KILL while waiting for the second click

static void set_labels(void) {
    const struct tm_proc *p = row_proc(row_of_pid(g_sel_pid));
    int stopped = p && p->state == PROC_STATE_STOPPED;
    g_btn_stop.label  = stopped ? "Continue" : "Stop";
    g_btn_close.label = g_armed == ID_CLOSE ? "Confirm?" : "Close";
    g_btn_kill.label  = g_armed == ID_KILL  ? "Confirm?" : "Force Quit";
    // Stopping the desktop, init or this window would leave nothing on
    // screen that can continue it.
    int self = p && (p->pid == (int)sys_getpid() || p->pid == g_desktop_pid || p->pid == 1);
    g_btn_stop.disabled  = !p || self;
    g_btn_close.disabled = !p;
    g_btn_kill.disabled  = !p;
}

// --- the details pane ------------------------------------------------------

#define DETAIL_ROWS 8
static struct uui_label g_d_title, g_d_sub;
static struct uui_label g_d_key[DETAIL_ROWS], g_d_val[DETAIL_ROWS];
static char g_d_title_text[TM_NAME_MAX], g_d_sub_text[48];
static char g_d_val_text[DETAIL_ROWS][TM_PATH_MAX];
static const char *const DETAIL_KEYS[DETAIL_ROWS] = {
    "Path", "Parent", "Group", "Threads", "Status", "CPU time", "Memory", "Service",
};
static struct uui_chart g_pcpu, g_pmem;
static int g_tracked;   // the pid the two charts are for
// Each chart's reading, top-right: now, and the peak its trace is
// scaled to -- the top of an autoscaled chart is otherwise unlabelled.
static char g_pcpu_val[40], g_pmem_val[48];

static void track(int pid) {
    g_tracked = pid;
    uui_chart_init(&g_pcpu, "CPU");
    uui_chart_init(&g_pmem, "Memory");
    uui_chart_set_interval(&g_pcpu, TM_REFRESH_MS);
    uui_chart_set_interval(&g_pmem, TM_REFRESH_MS);
    // Both AUTOSCALED to the process's own peak: against a 100% scale a
    // process at 1% is a flat line on the floor.
    uui_chart_set_scale(&g_pcpu, 0);
    uui_chart_set_scale(&g_pmem, 0);
    g_pcpu_val[0] = g_pmem_val[0] = '\0';
    uui_chart_set_value(&g_pcpu, g_pcpu_val);
    uui_chart_set_value(&g_pmem, g_pmem_val);
}

static uint32_t peak_of(const struct uui_chart *c) {
    uint32_t m = 0;
    for (int i = 0; i < c->count; i++) if (uui_chart_recent(c, i) > m) m = uui_chart_recent(c, i);
    return m;
}

void tm_track_pid(int pid) {
    if (pid != g_tracked) track(pid);
}

static void fill_details(void) {
    const struct tm_proc *p = row_proc(row_of_pid(g_sel_pid));
    for (int i = 0; i < DETAIL_ROWS; i++) g_d_val_text[i][0] = '\0';
    if (!p) {
        strlcpy(g_d_title_text, "No process selected", sizeof g_d_title_text);
        g_d_sub_text[0] = '\0';
        return;
    }
    strlcpy(g_d_title_text, p->title, sizeof g_d_title_text);
    snprintf(g_d_sub_text, sizeof g_d_sub_text, "%s, pid %d", p->name, p->pid);
    strlcpy(g_d_val_text[0], p->path[0] ? p->path : "-", TM_PATH_MAX);
    int pr = tm_proc_row(p->ppid);
    if (pr >= 0) snprintf(g_d_val_text[1], TM_PATH_MAX, "%s (%d)", g_proc[pr].name, p->ppid);
    else snprintf(g_d_val_text[1], TM_PATH_MAX, "%d", p->ppid);
    snprintf(g_d_val_text[2], TM_PATH_MAX, "%d", p->pgid);
    snprintf(g_d_val_text[3], TM_PATH_MAX, "%d", p->threads);
    strlcpy(g_d_val_text[4], tm_status_text(p), TM_PATH_MAX);
    unsigned long long ms = p->cpu_ns / 1000000ULL;
    snprintf(g_d_val_text[5], TM_PATH_MAX, "%llu.%02llu s", ms / 1000, (ms % 1000) / 10);
    unum_localize(g_d_val_text[5], TM_PATH_MAX, 0);
    human_size_iec(g_d_val_text[6], TM_PATH_MAX, p->mem_bytes);
    int s = tm_service_of_pid(p->pid);
    strlcpy(g_d_val_text[7], s >= 0 ? g_svc[s].name : "-", TM_PATH_MAX);
}

// --- the status bar --------------------------------------------------------

static struct uui_statusbar g_status;
static char g_st_count[48], g_st_cpu[24], g_st_mem[40], g_st_up[24];

static void fill_status(void) {
    snprintf(g_st_count, sizeof g_st_count, "%d processes, %d threads", g_nproc, g_threads);
    snprintf(g_st_cpu, sizeof g_st_cpu, "CPU %u%%", g_cpu_pm / 10);
    char u[16], t[16];
    human_size_iec(u, sizeof u, g_mem_used);
    human_size_iec(t, sizeof t, g_mem_total);
    snprintf(g_st_mem, sizeof g_st_mem, "Memory %s of %s", u, t);
    unsigned long long s = sys_monotonic_ns() / 1000000000ULL;
    snprintf(g_st_up, sizeof g_st_up, "Up %llu:%02llu:%02llu", s / 3600, (s / 60) % 60, s % 60);
}

// --- layout ----------------------------------------------------------------

static struct uui_item BAR_ITEMS[6];
static struct uui_layout BAR;
static struct uui_item DETAIL_GRID_ITEMS[DETAIL_ROWS * 2];
static struct uui_layout DETAIL_GRID;
static struct uui_item DETAIL_ITEMS[5];
static struct uui_layout DETAIL;
static struct uui_item BODY_ITEMS[2];
static struct uui_layout BODY;
static struct uui_item PAGE_ITEMS[3];
static struct uui_layout PAGE;
static struct uui_item g_root = { .ops = &uui_layout_ops, .widget = &PAGE, .name = "procs" };

// --- refreshing ---------------------------------------------------------------

// Case-blind substring: tolibc has strcasecmp but no strcasestr.
static int contains(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (strncasecmp(hay, needle, n) == 0) return 1;
    return 0;
}

static int matches(const struct tm_proc *p, const char *needle) {
    if (!needle[0]) return 1;
    char pid[12];
    snprintf(pid, sizeof pid, "%d", p->pid);
    return contains(p->title, needle) || contains(p->name, needle) ||
           strcmp(pid, needle) == 0;
}

// The order on screen, for a test to find a pid without pixels: app rows
// through the table's own view map, captions skipped.
static void report_order(int force) {
    char order[200];
    int n = 0;
    order[0] = '\0';
    for (int v = 0; v < uui_table_view_count(&g_table) && n < (int)sizeof order - 8; v++) {
        int r = uui_table_source_row(&g_table, v);
        if (r < 0) continue;
        n += snprintf(order + n, sizeof order - (size_t)n, "%s%d", n ? " " : "", row_proc(r)->pid);
    }
    static int last_col = -99, last_dir, last_view = -1;
    static char last[200];
    if (!force && g_table.sort_col == last_col && g_table.sort_dir == last_dir &&
        g_view == last_view && strcmp(order, last) == 0)
        return;
    last_col = g_table.sort_col; last_dir = g_table.sort_dir; last_view = g_view;
    strlcpy(last, order, sizeof last);
    ulogf("taskmgr: sort col %d dir %d view %s\n", g_table.sort_col, g_table.sort_dir,
          VIEW_NAMES[g_view]);
    ulogf("taskmgr: order %s\n", order);
}

// The hovered process, recorded when the table reports a hover CHANGE.
// Not re-derived from the row at a rebuild: by then refresh_model() has
// rewritten g_proc under the old g_rows, and the row names a neighbour.
static int g_hover_pid;

static void rebuild_rows(void) {
    const char *needle = uui_textbox_text(&g_search);
    g_nrows = 0;
    for (int g = 0; g < TM_GROUPS; g++) g_group_count[g] = 0;
    for (int i = 0; i < g_nproc; i++) {
        if (!matches(&g_proc[i], needle)) continue;
        g_rows[g_nrows++] = i;
        g_group_count[g_proc[i].group]++;
    }
    // The selection FOLLOWS ITS PID; a process that exited leaves none.
    int r = row_of_pid(g_sel_pid);
    if (r < 0) { g_sel_pid = 0; g_armed = 0; }
    g_table.selected = r;
    // The HOVER follows its pid too. Clearing it here blinked the
    // highlight off on every tick and every pointer move.
    g_table.hovered = row_of_pid(g_hover_pid);
    uui_table_set_rows(&g_table, g_nrows);
    // The table may have moved the selection -- onto a folded row's
    // visible ancestor. Believe it.
    // A move is a NEW selection: an arm aimed at the folded child must
    // not commit against its parent, and the traces follow the new pid.
    const struct tm_proc *sp = row_proc(g_table.selected);
    int now = sp ? sp->pid : 0;
    if (now != g_sel_pid) {
        if (g_armed) ulogf("taskmgr: disarmed, selection moved to pid %d\n", now);
        g_armed = 0;
        g_sel_pid = now;
        tm_track_pid(now);
    }

    set_labels();
    fill_details();
    fill_status();
    report_order(0);
}

static void select_pid(int pid) {
    if (pid != g_sel_pid) g_armed = 0;
    g_sel_pid = pid;
    tm_track_pid(pid);
    g_table.selected = row_of_pid(pid);
    set_labels();
    fill_details();
    ulogf("taskmgr: selected pid %d\n", pid);
}

// --- the verbs -----------------------------------------------------------------

static void act(struct uapp *a, int cmd) {
    const struct tm_proc *p = row_proc(row_of_pid(g_sel_pid));
    if (!p) { ulogf("taskmgr: no row selected\n"); return; }
    int pid = p->pid;
    switch (cmd) {
    case CMD_STOP:
    case CMD_CONTINUE:
        if (sys_kill(pid, cmd == CMD_STOP ? SIGSTOP : SIGCONT) == 0)
            ulogf("taskmgr: %s pid %d\n", cmd == CMD_STOP ? "stopped" : "continued", pid);
        break;
    case CMD_CLOSE:
        uapp_request_close_pid(a, pid);
        ulogf("taskmgr: asked pid %d to close\n", pid);
        break;
    case CMD_KILL:
        sys_kill(pid, SIGKILL);
        ulogf("taskmgr: killed pid %d\n", pid);
        break;
    case CMD_LOCATE:
        // The File Manager selects a file it is handed (docs/conventions).
        if (p->path[0]) uapp_spawn(a, "/bin/wm/apps/files", p->path);
        break;
    case CMD_SERVICE: {
        int si = tm_service_of_pid(pid);
        if (si >= 0) tm_services_select(g_svc[si].name);
        tm_show_page(a, 2);
        break;
    }
    default: break;
    }
    g_armed = 0;
}

// Press-then-commit for the two that end something.
static void arm_or_commit(struct uapp *a, int id) {
    if (!g_sel_pid) return;
    if (g_armed != id) {
        g_armed = id;
        ulogf("taskmgr: armed %s pid %d\n", id == ID_KILL ? "kill" : "end", g_sel_pid);
        set_labels();
        return;
    }
    act(a, id == ID_KILL ? CMD_KILL : CMD_CLOSE);
    set_labels();
}

// --- the context menu ------------------------------------------------------------

static const struct uui_menu_item CTX_ITEMS[] = {
    UUI_MENU("Close", CMD_CLOSE, 0),
    UUI_MENU("Stop", CMD_STOP, 0),
    UUI_MENU("Continue", CMD_CONTINUE, 0),
    UUI_MENU("Force Quit", CMD_KILL, 0),
    UUI_MENU_SEP,
    UUI_MENU("Open file location", CMD_LOCATE, 0),
    UUI_MENU("Go to service", CMD_SERVICE, 0),
};

static unsigned ctx_flags(int code) {
    const struct tm_proc *p = row_proc(row_of_pid(g_sel_pid));
    if (!p) return UUI_MI_DISABLED;
    int self = p->pid == (int)sys_getpid() || p->pid == g_desktop_pid || p->pid == 1;
    switch (code) {
    case CMD_STOP:     return (self || p->state == PROC_STATE_STOPPED) ? UUI_MI_DISABLED : 0;
    case CMD_CONTINUE: return p->state == PROC_STATE_STOPPED ? 0 : UUI_MI_DISABLED;
    case CMD_LOCATE:   return p->path[0] ? 0 : UUI_MI_DISABLED;
    case CMD_SERVICE:  return tm_service_of_pid(p->pid) >= 0 ? 0 : UUI_MI_DISABLED;
    default:           return 0;
    }
}

static int g_ctx_armed, g_ctx_x, g_ctx_y;
static int g_parked = -2;   // the focus stop parked while the menu is open; -2 = none

// The menu closed (a choice, Esc, a click outside): give the focus back.
static void menu_closed_check(void) {
    if (g_parked != -2 && !uui_menubar_is_open(&g_ctx)) {
        tm_focus_restore(g_parked);
        g_parked = -2;
    }
}

static void on_press(struct uapp *a, int x, int y, unsigned buttons) {
    if (!(buttons & 0x2)) return;
    if (uui_menubar_is_open(&g_ctx)) { uui_menubar_close(&g_ctx); uapp_redraw(a); return; }
    int r = uui_table_hit(&g_table, x, y);
    if (r < 0) return;   // not over a row: no menu, as on the chrome
    // THE CLICK SELECTS FIRST, Explorer's and Dolphin's rule: a menu
    // acting on a row other than the one pointed at ends the wrong one.
    select_pid(row_proc(r)->pid);
    g_ctx_armed = 1;
    g_ctx_x = x; g_ctx_y = y;
    uapp_redraw(a);
}

static void on_release(struct uapp *a, int x, int y, unsigned buttons) {
    (void)x; (void)y; (void)buttons;
    if (!g_ctx_armed) return;
    g_ctx_armed = 0;
    g_ctx.item_flags = ctx_flags;
    uui_menubar_open_at(&g_ctx, CTX_ITEMS, (int)(sizeof CTX_ITEMS / sizeof CTX_ITEMS[0]),
                        g_ctx_x, g_ctx_y);
    if (g_parked == -2) g_parked = tm_focus_park();
    uapp_redraw(a);
}

// --- hooks -------------------------------------------------------------------------

// A button names itself on press, motion AND release; it has committed
// only on the release (or a key). Acting on every report sent each
// signal three or four times.
static int on_widget(struct uapp *a, int id, int reason) {
    (void)reason;
    menu_closed_check();
    switch (id) {
    case TM_ID_CTX: {
        int code = uui_menubar_take_code(&g_ctx);
        if (code > 0) { act(a, code); rebuild_rows(); }
        return 1;
    }
    case ID_TABLE: {
        // The table reports hover motion too; only a FOLD changes the
        // rows, so only a fold rebuilds them (a rebuild per pointer move
        // was the flicker).
        int t = uui_table_take_toggled(&g_table);
        const struct tm_proc *hp = row_proc(g_table.hovered);
        g_hover_pid = hp ? hp->pid : 0;
        const struct tm_proc *p = row_proc(g_table.selected);
        if (p && p->pid != g_sel_pid) select_pid(p->pid);
        if (t >= 0) {
            toggle_fold(row_proc(t)->pid);
            ulogf("taskmgr: toggled pid %d\n", row_proc(t)->pid);
            rebuild_rows();
        }
        return 1;
    }
    case ID_SEARCH:
        rebuild_rows();
        return 1;
    case ID_VIEW:
        if (g_viewsel.selected >= 0 && g_viewsel.selected != g_view) {
            g_view = g_viewsel.selected;
            apply_view();
            rebuild_rows();
        }
        return 1;
    default:
        return 0;
    }
}

// Stop / Continue, Close and Force Quit.
static int on_action(struct uapp *a, int code) {
    switch (code) {
    case ID_STOP: {
        const struct tm_proc *p = row_proc(row_of_pid(g_sel_pid));
        if (p) act(a, p->state == PROC_STATE_STOPPED ? CMD_CONTINUE : CMD_STOP);
        return 1;
    }
    case ID_CLOSE:
    case ID_KILL:
        arm_or_commit(a, code);
        return 1;
    default:
        return 0;
    }
}

static int on_key(struct uapp *a, int key, unsigned mods) {
    (void)a;
    if (uui_menubar_is_open(&g_ctx)) {
        int code = -1;
        if (uui_menubar_key(&g_ctx, key, &code)) {
            if (code > 0) { act(a, code); rebuild_rows(); }
            menu_closed_check();
            return 1;
        }
    }
    menu_closed_check();
    // Ctrl+F: the filter, as in every list that has one.
    if ((mods & KEY_MOD_CTRL) && (key == 'f' || key == 'F')) { tm_focus(&g_search); return 1; }
    return 0;
}

static void tick(struct uapp *a, int shown) {
    (void)a;
    menu_closed_check();
    // The tracked process's history runs whether the page shows or not.
    // Through g_proc, NOT g_rows: the rows are rebuilt only while the
    // page shows and exclude what the filter hides.
    int ti = g_tracked ? tm_proc_row(g_tracked) : -1;
    const struct tm_proc *p = ti >= 0 ? &g_proc[ti] : 0;
    if (p) {
        // Per mille, so a process under 1% still draws a trace.
        uui_chart_push(&g_pcpu, p->cpu_pm);
        uui_chart_push(&g_pmem, (uint32_t)(p->mem_bytes >> 10));
        uint32_t pk = peak_of(&g_pcpu);
        char cur[16], top_pct[16];
        snprintf(cur, sizeof cur, "%u.%u%%", p->cpu_pm / 10, p->cpu_pm % 10);
        snprintf(top_pct, sizeof top_pct, "%u.%u%%", pk / 10, pk % 10);
        unum_localize(cur, sizeof cur, 0);
        unum_localize(top_pct, sizeof top_pct, 0);
        snprintf(g_pcpu_val, sizeof g_pcpu_val, "%s, peak %s", cur, top_pct);
        char now[16], top[16];
        human_size_iec(now, sizeof now, p->mem_bytes);
        human_size_iec(top, sizeof top, (unsigned long long)peak_of(&g_pmem) << 10);
        snprintf(g_pmem_val, sizeof g_pmem_val, "%s, peak %s", now, top);
    }
    if (shown) rebuild_rows();
}

static void page_open(struct uapp *a) {
    (void)a;
    rebuild_rows();
    report_order(1);
}

static int focusables(struct uui_focusable *out, int cap) {
    int n = 0;
    if (n < cap) out[n++] = (struct uui_focusable){ &g_table, &uui_table_ops };
    if (n < cap) out[n++] = (struct uui_focusable){ &g_search, &uui_textbox_focus_ops };
    if (n < cap) out[n++] = (struct uui_focusable){ &g_viewsel, &uui_segmented_ops };
    return n;
}

void tm_procs_init(struct tm_page *page) {
    uui_table_init(&g_table, 0, 0, 100, 100, COLUMNS, COL_COUNT, cell, 0);
    uui_table_set_seek_col(&g_table, COL_NAME);
    uui_table_set_compare(&g_table, compare_rows);
    uui_table_set_sort(&g_table, COL_NAME, 1);
    uui_table_set_heat(&g_table, heat_of);
    uui_table_set_icon(&g_table, icon_of);
    apply_view();

    uui_textbox_init(&g_search, "");
    g_search.placeholder = "Filter by name or PID";
    uui_segmented_init(&g_viewsel, VIEW_NAMES, 3, VIEW_GROUPED);
    uui_label_init(&g_spacer, "");
    uui_button_init(&g_btn_stop, 0, 0, 0, 0, "Stop", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_STOP);
    uui_button_init(&g_btn_close, 0, 0, 0, 0, "Close", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_CLOSE);
    uui_button_init(&g_btn_kill, 0, 0, 0, 0, "Force Quit", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_KILL);
    g_btn_stop.outlined = g_btn_close.outlined = g_btn_kill.outlined = 1;

    // The filter takes the bar's spare width, as Windows' search box
    // does: a fixed box clipped its own hint on a larger face.
    BAR_ITEMS[0] = (struct uui_item){ .ops = &uui_textbox_ops, .widget = &g_search,
                                       .id = ID_SEARCH, .name = "search", .flags = UUI_FILL_W };
    BAR_ITEMS[1] = (struct uui_item){ .ops = &uui_segmented_ops, .widget = &g_viewsel,
                                       .id = ID_VIEW, .name = "view" };
    BAR_ITEMS[2] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_spacer };
    BAR_ITEMS[3] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_btn_stop,
                                       .id = ID_STOP, .name = "btn_stop" };
    BAR_ITEMS[4] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_btn_close,
                                       .id = ID_CLOSE, .name = "btn_end" };
    BAR_ITEMS[5] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_btn_kill,
                                       .id = ID_KILL, .name = "btn_kill" };
    BAR = (struct uui_layout){ .dir = UUI_ROW, .items = BAR_ITEMS, .count = 6 };

    uui_label_init(&g_d_title, g_d_title_text);
    g_d_title.font = ugfx_font_session(UGFX_FONT_BOLD);
    uui_label_init(&g_d_sub, g_d_sub_text);
    for (int i = 0; i < DETAIL_ROWS; i++) {
        uui_label_init(&g_d_key[i], DETAIL_KEYS[i]);
        g_d_key[i].fg = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
        uui_label_init(&g_d_val[i], g_d_val_text[i]);
        DETAIL_GRID_ITEMS[2 * i] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_d_key[i] };
        DETAIL_GRID_ITEMS[2 * i + 1] = (struct uui_item){ .ops = &uui_label_ops,
                                                          .widget = &g_d_val[i],
                                                          .flags = UUI_FILL_W };
    }
    DETAIL_GRID = (struct uui_layout){ .dir = UUI_ROW, .items = DETAIL_GRID_ITEMS, .count = 0 };
    // A two-column grid of key and value, left-aligned: a ROW per pair
    // would size each key to its own text and the values would not line
    // up. UUI_GRID's cells are uniform, which is what lines them up.
    DETAIL_GRID.dir = UUI_GRID;
    DETAIL_GRID.cols = 2;
    DETAIL_GRID.count = DETAIL_ROWS * 2;
    track(0);
    DETAIL_ITEMS[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_d_title, .flags = UUI_FILL_W };
    DETAIL_ITEMS[1] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_d_sub, .flags = UUI_FILL_W };
    DETAIL_ITEMS[2] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &DETAIL_GRID, .flags = UUI_FILL_W };
    DETAIL_ITEMS[3] = (struct uui_item){ .ops = &uui_chart_ops, .widget = &g_pcpu,
                                          .flags = UUI_FILL_W, .name = "pcpu" };
    DETAIL_ITEMS[4] = (struct uui_item){ .ops = &uui_chart_ops, .widget = &g_pmem,
                                          .flags = UUI_FILL_W, .name = "pmem" };
    DETAIL = (struct uui_layout){ .dir = UUI_COLUMN, .items = DETAIL_ITEMS, .count = 5 };

    BODY_ITEMS[0] = (struct uui_item){ .ops = &uui_table_ops, .widget = &g_table,
                                        .id = ID_TABLE, .name = "table",
                                        // The hover follows its pid across a rebuild.
                                        .flags = UUI_FILL_W | UUI_FILL_H | UUI_TRACK_HOVER };
    BODY_ITEMS[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &DETAIL,
                                        .name = "details", .flags = UUI_FILL_H };
    BODY = (struct uui_layout){ .dir = UUI_ROW, .items = BODY_ITEMS, .count = 2 };

    uui_statusbar_init(&g_status);
    g_status.count = 4;
    g_status.panes[0] = (struct uui_status_pane){ g_st_count, 0 };
    g_status.panes[1] = (struct uui_status_pane){ g_st_cpu, 9 };
    g_status.panes[2] = (struct uui_status_pane){ g_st_mem, 24 };
    g_status.panes[3] = (struct uui_status_pane){ g_st_up, 12 };

    PAGE_ITEMS[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &BAR, .flags = UUI_FILL_W };
    PAGE_ITEMS[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &BODY,
                                        .flags = UUI_FILL_W | UUI_FILL_H };
    PAGE_ITEMS[2] = (struct uui_item){ .ops = &uui_statusbar_ops, .widget = &g_status,
                                        .flags = UUI_FILL_W, .name = "status" };
    PAGE = (struct uui_layout){ .dir = UUI_COLUMN, .items = PAGE_ITEMS, .count = 3 };

    *page = (struct tm_page){
        .label = "Processes", .icon = "tb-details", .root = &g_root,
        .open = page_open, .tick = tick, .widget = on_widget, .action = on_action, .key = on_key,
        .press = on_press, .release = on_release, .focusables = focusables,
    };
}

// RESPONSIVE, as Windows' details pane is: below the width where the
// Name column keeps ~12 digits the pane goes, and it comes back once
// there is room for both. A narrow window -- and a window reopens at the
// size it was left, which may predate this layout -- otherwise squeezed
// the Name column to nothing (seen on the laptop's larger face).
// Returns 1 when the caller must lay the page out again.
int tm_procs_fit(void) {
    int nx, nw;
    uui_table_column_rect(&g_table, COL_NAME, &nx, &nw);
    int want = ugfx_char_advance('0') * 12;
    struct uui_item *pane = &BODY_ITEMS[1];
    if (!pane->hidden && nw < want) { pane->hidden = 1; return 1; }
    if (pane->hidden && nw >= want + pane->main_size + ugfx_char_w()) { pane->hidden = 0; return 1; }
    return 0;
}

// The narrowest the page can be with its whole toolbar showing: the
// bar's natural width (the filter asks for none) plus a usable filter.
int tm_procs_min_width(void) {
    int w = 0, h = 0;
    uui_layout_natural_size(&BAR, &w, &h);
    return w + ugfx_char_advance('0') * 10;
}

// The details pane's width, pinned: a pane that sized itself to the
// selected process's path would jump on every click. Called by the shell
// from its size hook, since only then is the font known.
void tm_procs_size(void) {
    BODY_ITEMS[1].main_size = ugfx_char_w() * 20;
    // Four rows each: a trend, not a reading -- the numbers are above.
    DETAIL_ITEMS[3].main_size = ugfx_char_h() * 4;
    DETAIL_ITEMS[4].main_size = ugfx_char_h() * 4;
}
