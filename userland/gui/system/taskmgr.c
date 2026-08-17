// Task Manager, as a RING-3 PROCESS.
//
// Replaces the kernel-space apps/taskmgr.c, which could only list
// WINDOWS -- it read the WM's own window table because that was the only
// per-something data a kernel-space app had access to. A real task
// manager lists PROCESSES, which needed three pieces of kernel
// bookkeeping that did not exist (a name, CPU time and memory per
// process; see abi/proc_info.h) and two syscalls to reach them.
//
// WHAT THIS DEMONSTRATES BEYOND ITSELF
// ------------------------------------
// It is the first app built on `uui_table`, and the reason that widget
// pulls its rows through a callback rather than storing them: the
// process table is re-read on every refresh, and there is no allocator
// in Toykit to hold a copy in. Nothing is cached, so nothing goes stale.
//
// TWO WAYS TO END A PROCESS, deliberately, exactly as Windows separates
// "Close" from "Force Quit" -- renamed from Windows' "End Task"/"End
// Process", which needed the four lines below to tell apart and which a
// user reasonably reads as two words for one thing. The names now say
// what the buttons do, and "Force Quit" is the term this desktop
// already uses in its own force-quit dialog, so one action does not
// have two names depending on where it was triggered:
//
//   End Task    -- asks the window to close (the same handshake the X
//                  button uses), so an app may save or decline.
//   End Process -- SYS_KILL, immediate and unconditional.
//
// Both ARM on the first click and commit on the second, which is this
// project's standing press-then-commit rule (docs/gui-guidelines.md)
// doing duty as the confirmation step -- rather than a modal dialog,
// which Toykit does not have and which this did not justify inventing.
//
// Killing anything is allowed, including the window manager once it is
// a process: see abi/syscall_abi.h's SYS_KILL on why there is no
// permission check and why the WM is not special-cased.
#include <stdint.h>
#include <stdarg.h>
#include "rt/sys.h"
#include "lib/string.h"
#include "lib/stdio.h"
#include "ui/ugfx.h"
#include "ui/uapp.h"
#include "ui/utheme.h"
#include "ui/uui_table.h"
#include "ui/uui_button.h"
#include "ui/uui_button_group.h"
#include "ui/uui_layout.h"
#include "ui/uui_widget.h"

// Diagnostics go to STDERR, which the kernel routes to its log and to
// dmesg. A windowed client's stdout goes nowhere useful -- finding that
// out cost a previous session half an hour of "the app is clearly
// running and the log is empty" (CLAUDE.md).
static void logf_(const char *fmt, ...) {
    char line[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    sys_eprint(line);
}

#define ID_TABLE   1
#define ID_END     2
#define ID_KILL    3
#define ID_BUTTONS 4

// How often to re-read the process table, in milliseconds. Paced by a
// TWS timer (uapp.h's tick_ms), so the app BLOCKS in between rather
// than waking on every loop pass to decide it has nothing to do -- it
// used to count 40 passes for the same effect, which still meant being
// scheduled a hundred times a second to refresh twice.
//
// Long enough that the CPU percentages are computed over a meaningful
// interval rather than over a couple of ticks, where rounding
// dominates; short enough to feel live.
#define REFRESH_MS 500

struct row {
    int pid;
    unsigned state;
    unsigned long long cpu_ns;
    unsigned long mem_bytes;
    unsigned cpu_pct;      // computed between refreshes
    char name[PROC_NAME_MAX];
};

static struct row g_rows[SYS_PROC_MAX];
static int g_row_count;

// Last sample, indexed BY PID rather than by row, because rows move as
// processes come and go -- indexing by row would attribute one
// process's previous CPU total to whichever process later landed in
// that row, and the percentage would be nonsense exactly when the table
// is busiest.
static unsigned long long g_prev_cpu[SYS_PROC_MAX + 1];
static unsigned long long g_prev_ns;

static struct uui_table g_table;

// A GROUP, not two standalone buttons. `uui_button_ops` is draw and
// hit-testing only -- a lone button routes no input at all, so clicking
// one does nothing and looks exactly like a broken handler. The group is
// what arms on press, commits on release, and declines to commit a press
// dragged off its button (docs/gui-guidelines.md).
static struct uui_button g_buttons[2];
static struct uui_button_group g_group;

#define BTN_END  0
#define BTN_KILL 1

// Which button is armed, or 0. Cleared whenever the selection changes,
// so an arm can never carry over onto a different process than the one
// it was aimed at.
static int g_armed;

// Named, so compare_rows() below and this array cannot drift: a column
// inserted in the middle otherwise silently re-points every case in the
// comparator at its neighbour.
enum { COL_PID = 0, COL_NAME, COL_STATE, COL_CPU, COL_MEM };

static const struct uui_table_column COLUMNS[] = {
    { "PID",    6, UUI_TALIGN_RIGHT },
    { "Name",   0, UUI_TALIGN_LEFT  }, // stretches with the window
    { "State",  9, UUI_TALIGN_LEFT  },
    { "CPU",    6, UUI_TALIGN_RIGHT },
    { "Memory", 10, UUI_TALIGN_RIGHT },
};
#define COL_COUNT ((int)(sizeof COLUMNS / sizeof COLUMNS[0]))

static const char *state_name(unsigned s);

// The whole app-side cost of sortable columns: answer "does row a come
// before row b in this column?" on the REAL values. The table owns
// everything else -- the clickable header, the arrow,
// click-again-to-reverse, keyboard motion in the sorted order -- so
// nothing else in this file mentions sorting.
//
// Comparing the real fields rather than the formatted cells is the
// point (see uui_table.h): "10" sorts before "9" as text, and 4 KB
// against 1 MB is meaningless, so three of these five columns would be
// wrong if the widget sorted what it draws.
static int compare_rows(void *ctx, int a, int b, int col) {
    (void)ctx;
    const struct row *x = &g_rows[a], *y = &g_rows[b];
    switch (col) {
    case COL_PID:   return x->pid - y->pid;
    case COL_NAME:  return strcmp(x->name, y->name);
    case COL_STATE: return strcmp(state_name(x->state), state_name(y->state));
    case COL_CPU:   return (int)x->cpu_pct - (int)y->cpu_pct;
    case COL_MEM:
        // NOT a subtraction: these are unsigned longs, and a difference
        // that does not fit an int is the classic comparator bug --
        // it makes the sort order depend on how far apart two values
        // happen to be.
        if (x->mem_bytes < y->mem_bytes) return -1;
        return x->mem_bytes > y->mem_bytes ? 1 : 0;
    default: return 0;
    }
}

static const char *state_name(unsigned s) {
    switch (s) {
        case PROC_STATE_RUNNING: return "running";
        case PROC_STATE_READY:   return "ready";
        case PROC_STATE_BLOCKED: return "blocked";
        case PROC_STATE_ZOMBIE:  return "exited";
        default:                  return "-";
    }
}

// Bytes as a human-readable size. A task manager column is a few
// characters wide, so "1.2M" beats "1258291" -- and the integer maths
// keeps one decimal without any floating point (this is a freestanding
// binary; the kernel has no FP at all and userland has no printf %f).
static void format_bytes(char *out, int cap, unsigned long b) {
    if (b < 1024UL) { snprintf(out, (size_t)cap, "%lu B", b); return; }
    if (b < 1024UL * 1024UL) {
        unsigned long k10 = (b * 10UL) / 1024UL;
        snprintf(out, (size_t)cap, "%lu.%lu K", k10 / 10, k10 % 10);
        return;
    }
    unsigned long m10 = (b * 10UL) / (1024UL * 1024UL);
    snprintf(out, (size_t)cap, "%lu.%lu M", m10 / 10, m10 % 10);
}

// Re-reads the process table and recomputes the CPU percentages.
static void refresh(void) {
    // NANOSECONDS on both sides of the ratio: the numerator is a delta
    // of cpu_ns and this is a delta of the same clock (SYS_MONOTONIC_NS).
    // It used to be ticks, which could not express anything a process
    // did inside a 10ms slice -- an animating client legitimately read
    // 0% while drawing every frame.
    unsigned long long now = sys_monotonic_ns();
    unsigned long long elapsed = now - g_prev_ns;

    int n = 0;
    for (int i = 0; i < SYS_PROC_MAX && n < SYS_PROC_MAX; i++) {
        struct proc_info info;
        if (!sys_proc_info(i, &info)) continue;
        if (info.pid == 0) continue; // empty slot -- skip, do not stop

        struct row *r = &g_rows[n++];
        r->pid = info.pid;
        r->state = info.state;
        r->cpu_ns = (unsigned long long)info.cpu_ns;
        r->mem_bytes = (unsigned long)info.mem_bytes;
        strlcpy(r->name, info.name, sizeof r->name);

        // This process's share of the interval. See the note above on
        // why both sides are deltas of the same clock.
        unsigned long long prev = (info.pid <= SYS_PROC_MAX) ? g_prev_cpu[info.pid] : 0;
        unsigned long long used = r->cpu_ns > prev ? r->cpu_ns - prev : 0;
        r->cpu_pct = elapsed > 0 ? (unsigned)((used * 100ULL) / elapsed) : 0;
        if (r->cpu_pct > 100) r->cpu_pct = 100; // a slot reused mid-interval
        if (info.pid <= SYS_PROC_MAX) g_prev_cpu[info.pid] = r->cpu_ns;
    }

    g_prev_ns = now;
    g_row_count = n;
    uui_table_set_rows(&g_table, n);
}

// The table pulls one cell at a time -- see ui/uui_table.h.
static void cell(void *ctx, int row, int col, char *out, int cap) {
    (void)ctx;
    if (row < 0 || row >= g_row_count) { out[0] = '\0'; return; }
    const struct row *r = &g_rows[row];

    switch (col) {
        case 0: snprintf(out, (size_t)cap, "%d", r->pid); break;
        case 1: strlcpy(out, r->name[0] ? r->name : "(unnamed)", (size_t)cap); break;
        case 2: strlcpy(out, state_name(r->state), (size_t)cap); break;
        case 3: snprintf(out, (size_t)cap, "%u%%", r->cpu_pct); break;
        case 4: format_bytes(out, cap, r->mem_bytes); break;
        default: out[0] = '\0'; break;
    }
}

static void set_labels(void) {
    // The armed button says what the NEXT click does. An armed action
    // that looked identical to an unarmed one would make the
    // confirmation invisible, which is the same failure as no
    // confirmation at all.
    // `label` is a plain, unowned field (ui/uui_button.h) and every
    // string here is a literal, so it outlives the button.
    g_buttons[BTN_END].label  = (g_armed == ID_END)  ? "Confirm?" : "Close";
    g_buttons[BTN_KILL].label = (g_armed == ID_KILL) ? "Confirm?" : "Force Quit";
}

static int selected_pid(void) {
    int sel = g_table.selected;
    if (sel < 0 || sel >= g_row_count) return 0;
    return g_rows[sel].pid;
}

static void on_widget(struct uapp *a, int id, int reason) {
    (void)reason;

    if (id == ID_TABLE) {
        // The selection moved: any arm was aimed at the previous row and
        // must not survive onto this one.
        if (g_armed) { g_armed = 0; set_labels(); }
        logf_("taskmgr: selected pid %d\n", selected_pid());
        uapp_redraw(a);
        return;
    }

    if (id != ID_BUTTONS) return;

    // The router reports which WIDGET changed, and the group is one
    // widget holding two buttons -- so which button committed is
    // collected from the group rather than inferred from the id.
    int code = uui_button_group_take_activated(&g_group);
    if (!code) return;   // a press that did not commit (dragged off)
    id = code;

    int pid = selected_pid();
    if (!pid) {
        // Nothing selected: say so. A silent return here is
        // indistinguishable from a click that missed the button
        // entirely, and a test cannot tell those apart from outside.
        logf_("taskmgr: no row selected\n");
        return;
    }

    if (g_armed != id) {          // first click: arm
        g_armed = id;
        set_labels();
        logf_("taskmgr: armed %s pid %d\n",
                id == ID_KILL ? "kill" : "end", pid);
        uapp_redraw(a);
        return;
    }

    g_armed = 0;                   // second click: commit
    if (id == ID_KILL) {
        sys_kill(pid, 137); // 128 + SIGKILL's 9, the shell convention
        logf_("taskmgr: killed pid %d\n", pid);
    } else {
        // Polite: ask the window to close. A process with no window
        // simply has nothing to ask, which is reported rather than
        // silently doing nothing.
        if (!uapp_request_close_pid(a, pid)) {
            logf_("taskmgr: pid %d has no window to close\n", pid);
        } else {
            logf_("taskmgr: asked pid %d to close\n", pid);
        }
    }
    set_labels();
    refresh();
    uapp_redraw(a);
}

// The sort state, the column rects and the pids IN SCREEN ORDER.
//
// Reported when they CHANGE, not once at startup: a sort state logged
// only at open cannot show whether a header click did anything, which
// is the whole question a sorting test asks. Same reasoning as the
// table's own rect being re-reported on resize, one bug later.
static void report_sort(int force) {
    char order[160];
    int n = 0;
    order[0] = '\0';
    for (int i = 0; i < g_row_count && n < (int)sizeof order - 8; i++) {
        int src = uui_table_source_row(&g_table, i);
        if (src < 0) break;
        n += snprintf(order + n, sizeof order - n, "%s%d",
                      i ? " " : "", g_rows[src].pid);
    }

    static int last_col = -99, last_dir = 0;
    static char last_order[160];
    if (!force && g_table.sort_col == last_col && g_table.sort_dir == last_dir
        && strcmp(order, last_order) == 0) {
        return;
    }
    last_col = g_table.sort_col;
    last_dir = g_table.sort_dir;
    strlcpy(last_order, order, sizeof last_order);

    logf_("taskmgr: sort col %d dir %d\n", g_table.sort_col, g_table.sort_dir);
    // Each column's own rect, so a test can click a HEADER without
    // re-deriving column widths from the character counts in COLUMNS[]
    // -- the re-derivation that has drifted in four tools here already.
    for (int c = 0; c < COL_COUNT; c++) {
        int colx, colw;
        uui_table_column_rect(&g_table, c, &colx, &colw);
        logf_("taskmgr: layout col%d %d %d\n", c, colx, colw);
    }
    logf_("taskmgr: order %s\n", order);
}

static int on_tick(struct uapp *a) {
    // Report the table's rect whenever it CHANGES, not just at open.
    // A geometry logged once at startup cannot show whether a resize
    // reflowed, which is exactly the question a resize bug raises.
    static int last_w, last_h;
    if (g_table.w != last_w || g_table.h != last_h) {
        last_w = g_table.w;
        last_h = g_table.h;
        logf_("taskmgr: layout table %d %d %d %d\n",
                g_table.x, g_table.y, g_table.w, g_table.h);
        // The BUTTONS move with it -- they sit below the table, so a
        // resize relocates them. Reporting only the table left a tool
        // clicking the buttons' pre-resize coordinates, which misses
        // them entirely and reads as "the button does nothing".
        logf_("taskmgr: layout btn_end %d %d %d %d\n",
                g_buttons[BTN_END].x, g_buttons[BTN_END].y,
                g_buttons[BTN_END].w, g_buttons[BTN_END].h);
        logf_("taskmgr: layout btn_kill %d %d %d %d\n",
                g_buttons[BTN_KILL].x, g_buttons[BTN_KILL].y,
                g_buttons[BTN_KILL].w, g_buttons[BTN_KILL].h);
    }

    refresh();
    report_sort(0);
    (void)a;
    return 1; // repaint
}

static void on_open(struct uapp *a) {
    (void)a;
    g_prev_ns = sys_monotonic_ns();
    refresh();
    set_labels();
    // Report the layout for the test tool, per this repo's rule that a
    // geometry a tool would otherwise re-derive belongs in the app's own
    // log (four tools have been bitten by re-deriving one).
    logf_("taskmgr: layout table %d %d %d %d\n",
            g_table.x, g_table.y, g_table.w, g_table.h);
    logf_("taskmgr: layout row_h %d header_h %d\n",
            uui_table_row_h(&g_table), uui_table_header_h(&g_table));
    // The BUTTONS too. A geometry an app does not report is one a test
    // re-derives in Python and gets wrong -- which happened here on the
    // first attempt at driving this window, and is the same mistake four
    // other tools in this repo have already paid for.
    logf_("taskmgr: layout btn_end %d %d %d %d\n",
            g_buttons[BTN_END].x, g_buttons[BTN_END].y,
            g_buttons[BTN_END].w, g_buttons[BTN_END].h);
    logf_("taskmgr: layout btn_kill %d %d %d %d\n",
            g_buttons[BTN_KILL].x, g_buttons[BTN_KILL].y,
            g_buttons[BTN_KILL].w, g_buttons[BTN_KILL].h);
    logf_("taskmgr: rows %d\n", g_row_count);

    // The sort state, and the pids IN SCREEN ORDER. The order line is
    // what makes a sorting test assertable at all: "the table changed"
    // is satisfied by a repaint, and reading the order out of pixels
    // would mean OCR. This is the same rule as every other layout line
    // here -- ask the app, do not re-derive.
    report_sort(1);
}

static void on_size(int *w, int *h) {
    // THE BUTTONS ARE SIZED HERE, not in main(). ugfx_char_w() returns 0
    // until uapp_run() has fetched the font, so sizing them in main()
    // produced zero-width buttons -- which drew as nothing, hit-tested
    // as nothing, and looked exactly like a broken click handler. This
    // callback is the first hook that runs with the font available, and
    // it runs before the layout, which is what the group's natural size
    // is read from.
    int bw = ugfx_char_w() * 13, bh = ugfx_char_h() + 12;
    g_buttons[BTN_END].x = 0;
    g_buttons[BTN_END].y = 0;
    g_buttons[BTN_END].w = bw;
    g_buttons[BTN_END].h = bh;
    g_buttons[BTN_KILL].x = bw + 8;
    g_buttons[BTN_KILL].y = 0;
    g_buttons[BTN_KILL].w = bw;
    g_buttons[BTN_KILL].h = bh;

    // Font-derived, per docs/gui-guidelines.md -- wide enough for every
    // column, tall enough for a useful number of rows. The window is
    // resizable and the table reflows, so this is a starting point
    // rather than a constraint.
    *w = ugfx_char_w() * 56;
    *h = ugfx_char_h() * 20;
}

static struct uui_item ITEMS[2];
static struct uui_layout LAYOUT;

int main(void) {
    uui_table_init(&g_table, 0, 0, 100, 100, COLUMNS, COL_COUNT, cell, 0);
    // Opting in is one call. Sorted by PID ascending to start with,
    // which is the order the process table is already in -- so the
    // opening view is unchanged and the first header click is the first
    // thing that visibly reorders anything.
    uui_table_set_compare(&g_table, compare_rows);
    uui_table_set_sort(&g_table, COL_PID, 1);

    // Labels and codes only -- the RECTS are set in on_size(), because
    // the font does not exist yet at this point and every font-derived
    // size here would be 0.
    uui_button_init(&g_buttons[BTN_END], 0, 0, 0, 0, "Close",
                     UTHEME_BUTTON_BG, UTHEME_TEXT, ID_END);
    uui_button_init(&g_buttons[BTN_KILL], 0, 0, 0, 0, "Force Quit",
                     UTHEME_BUTTON_BG, UTHEME_TEXT, ID_KILL);
    uui_button_group_init(&g_group, g_buttons, 2);

    ITEMS[0] = (struct uui_item){ .ops = &uui_table_ops, .widget = &g_table,
                                   .id = ID_TABLE,
                                   .flags = UUI_FILL_W | UUI_FILL_H };
    ITEMS[1] = (struct uui_item){ .ops = &uui_button_group_ops,
                                   .widget = &g_group, .id = ID_BUTTONS };

    LAYOUT = (struct uui_layout){ .dir = UUI_COLUMN, .items = ITEMS, .count = 2 };

    struct uapp_desc desc = {
        .title = "Task Manager",
        // Exactly one of these is useful: a second copy shows the same
        // table, costs a process slot, and adds its own polling to the
        // CPU figures it is meant to be reporting. Opening it again
        // raises the one that exists.
        .app_id = "taskmgr",
        .layout = &LAYOUT,
        .tick_ms = REFRESH_MS,
        .on_size = on_size,
        // Resizable, and the table follows: uui_table's set_geometry
        // recomputes its visible rows and its stretch column from
        // whatever size the layout hands it, so more window means more
        // processes on screen with no arithmetic here.
        .flags = UAPP_RESIZABLE | UAPP_SINGLE_INSTANCE,
        .min_w = 0, .min_h = 0,
        .widgets = ITEMS,
        .widget_count = 2,
        .on_widget = on_widget,
        .on_open = on_open,
        .on_tick = on_tick,
    };
    return uapp_run(&desc);
}
