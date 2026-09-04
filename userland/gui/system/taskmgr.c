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
#include "ui/ulog.h"
#include <stdarg.h>
#include "rt/sys.h"
#include <string.h>
#include <stdio.h>
#include "ui/ugfx.h"
#include "ui/uapp.h"
#include "lib/human.h"
#include "ui/utheme.h"
#include "ui/uui_table.h"
#include "ui/uui_button.h"
#include "ui/uui_button_group.h"
#include "ui/uui_layout.h"
#include "ui/uui_widget.h"
#include "ui/uui_route.h"   // UUI_REASON_KEY
#include "ui/uui_tabs.h"
#include "ui/uui_meter.h"
#include "ui/uui_scrollview.h"
#include "query_abi.h"


#define ID_TABLE   1
#define ID_END     2
#define ID_KILL    3
#define ID_BUTTONS 4
#define ID_TABS    5

// --- the Overview page -----------------------------------------------
//
// KDE's System Monitor overview, with the readings this machine can
// actually back. Its screenshot also carries GPU and Swap rings; toy-os
// has neither -- there is no GPU utilisation fact anywhere, and the
// whole swap track is unbuilt (docs/roadmap.md) -- and a gauge that is
// permanently empty teaches a reader to distrust the ones beside it.
//
// CPU and Memory are RINGS because they are a fraction of a fixed
// whole. Disks are BARS because there may be several and they stack; a
// row of rings for four mounts is a lot of ink for four percentages.
// Same widget both times, which is why uui_meter grew a style rather
// than a sibling.
#define OV_DISKS_MAX 4
#define OV_METERS    (2 + OV_DISKS_MAX)

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
        if (sys_proc_info(i, &info) != 0) continue;
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
        case 4: human_size(out, cap, r->mem_bytes); break;
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

static struct uui_tab TABS[2] = { { "Overview", 0 }, { "Processes", 0 } };
static struct uui_tabs g_tabs;

static void select_page(struct uapp *a, int index);

static void on_widget(struct uapp *a, int id, int reason) {
    (void)reason;

    if (id == ID_TABS) {
        select_page(a, g_tabs.selected);
        ulogf("taskmgr: page %d\n", g_tabs.selected);
        uapp_redraw(a);
        return;
    }

    if (id == ID_TABLE) {
        // The selection moved: any arm was aimed at the previous row and
        // must not survive onto this one.
        if (g_armed) { g_armed = 0; set_labels(); }
        ulogf("taskmgr: selected pid %d\n", selected_pid());
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
        ulogf("taskmgr: no row selected\n");
        return;
    }

    if (g_armed != id) {          // first click: arm
        g_armed = id;
        set_labels();
        ulogf("taskmgr: armed %s pid %d\n",
                id == ID_KILL ? "kill" : "end", pid);
        uapp_redraw(a);
        return;
    }

    g_armed = 0;                   // second click: commit
    if (id == ID_KILL) {
        // SIGKILL, which is what Force Quit means: uncatchable, and
        // the one signal that works on a process wedged in its own
        // loop. The 137 this used to pass was the EXIT CODE the
        // kernel now derives from the signal itself.
        sys_kill(pid, SIGKILL);
        ulogf("taskmgr: killed pid %d\n", pid);
    } else {
        // Polite: ask the window to close. A process with no window
        // simply has nothing to ask, which is reported rather than
        // silently doing nothing.
        if (!uapp_request_close_pid(a, pid)) {
            ulogf("taskmgr: pid %d has no window to close\n", pid);
        } else {
            ulogf("taskmgr: asked pid %d to close\n", pid);
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

    ulogf("taskmgr: sort col %d dir %d\n", g_table.sort_col, g_table.sort_dir);
    // The columns' rects come from the table itself (`table.col i`).
    ulogf("taskmgr: order %s\n", order);
}

// THE BODY IS SWAPPED, NOT REBUILT. Both pages are layouts, so
// selecting a tab changes one pointer -- the pattern System Settings
// already uses for its pages. The router descends into whichever body
// is current through uui_layout_ops' `children`, so `widgets` stays the
// top-level pair and no input wiring changes with the tab.
static struct uui_item PROC_ITEMS[2];
static struct uui_layout PROC_LAYOUT;
static struct uui_item OV_GAUGES[2];   // CPU and Memory, side by side
static struct uui_layout OV_GAUGE_ROW;
static struct uui_item OV_ITEMS[1 + OV_DISKS_MAX];
static struct uui_layout OV_LAYOUT;
// THE OVERVIEW CAN OVERFLOW: two gauges plus one tile per mount is more
// than a small window holds, and uui_layout places the surplus PAST the
// bottom edge with nothing to say it is there. The tab strip stays
// OUTSIDE it, or it would scroll away with the page (CLAUDE.md).
static struct uui_scrollview OV_SCROLL;
static struct uui_item ITEMS[2];
static struct uui_layout LAYOUT;


static int g_ov_count;   // meters actually in use this refresh

// THE LAYOUT HAS TO BE RE-RUN, and that is the whole trick. uapp runs
// it on open, on resize and on a font change -- not when an app swaps
// what a container holds, because it cannot know that happened. Without
// this the meters keep the zero geometry they were born with and the
// page draws nothing at all, which is exactly how this shipped its
// first time. System Settings' apply_split() re-runs it for the same
// reason.
static void select_page(struct uapp *a, int index) {
    ITEMS[1].ops    = (index == 0) ? &uui_scrollview_ops : &uui_layout_ops;
    ITEMS[1].widget = (index == 0) ? (void *)&OV_SCROLL : (void *)&PROC_LAYOUT;
    // The Overview's tiles are only as many as the machine has; a stale
    // count would lay out a meter with no strings in it.
    // The gauge row plus one tile per disk found. g_ov_count counts
    // METERS, and the first two of those live inside the nested row.
    OV_LAYOUT.count = 1 + (g_ov_count > 2 ? g_ov_count - 2 : 0);
    uui_scrollview_content_changed(&OV_SCROLL);
    if (a) uui_layout_run(&LAYOUT, 0, 0, uapp_width(a), uapp_height(a));
}

// --- Overview state ---------------------------------------------------
//
// EVERY STRING IS A STATIC BUFFER, because uui_meter POINTS AT its
// strings rather than copying them (uui_meter.h) -- a formatted local
// would be a dangling pointer the moment refresh() returned, and it
// would draw perfectly well until the stack was reused.
static struct uui_meter g_ov[OV_METERS];
static char g_ov_cap[OV_METERS][24];
static char g_ov_val[OV_METERS][16];
static char g_ov_unit[OV_METERS][24];
static char g_ov_det[OV_METERS][28];

// The CPU split's previous sample. Cumulative counters, so the reading
// is a ratio of DELTAS -- see QUERY_CPULOAD.
static unsigned long long g_cpu_prev_proc, g_cpu_prev_kernel;
static int g_cpu_have_prev;


static void ov_set(int i, const char *caption, int per_mille,
                   const char *value, const char *unit, const char *detail,
                   enum uui_meter_style style) {
    if (i < 0 || i >= OV_METERS) return;
    snprintf(g_ov_cap[i],  sizeof g_ov_cap[i],  "%s", caption);
    snprintf(g_ov_val[i],  sizeof g_ov_val[i],  "%s", value ? value : "");
    snprintf(g_ov_unit[i], sizeof g_ov_unit[i], "%s", unit ? unit : "");
    snprintf(g_ov_det[i],  sizeof g_ov_det[i],  "%s", detail ? detail : "");
    uui_meter_set(&g_ov[i], g_ov_cap[i], g_ov_val[i],
                  g_ov_unit[i][0] ? g_ov_unit[i] : NULL,
                  g_ov_det[i][0] ? g_ov_det[i] : NULL);
    uui_meter_set_fill(&g_ov[i], per_mille);
    uui_meter_set_style(&g_ov[i], style);
}

// Re-reads the three system facts behind the Overview. Separate from
// refresh() because the process table is re-read far more often than
// this needs to be, and because a failed query here must leave the
// PROCESS list working -- an Overview that cannot read a disk is a
// missing tile, not a broken app.
static void refresh_overview(void) {
    char val[16], unit[24], det[28];
    int n = 0;

    // CPU: a ratio of deltas. The first sample has nothing to compare
    // against, so it reads 0 rather than a made-up number.
    struct query_cpuload cl;
    int pct = 0;
    if (sys_query_record(QUERY_CPULOAD, 0, &cl, sizeof cl) >= (int)sizeof cl) {
        if (g_cpu_have_prev) {
            unsigned long long dp = cl.proc_ns - g_cpu_prev_proc;
            unsigned long long dk = cl.kernel_ns - g_cpu_prev_kernel;
            if (dp + dk > 0) pct = (int)((dp * 100ULL) / (dp + dk));
            if (pct > 100) pct = 100;
        }
        g_cpu_prev_proc = cl.proc_ns;
        g_cpu_prev_kernel = cl.kernel_ns;
        g_cpu_have_prev = 1;
    }
    snprintf(val, sizeof val, "%d%%", pct);
    // No unit under the number: a percentage that moves says "busy" on
    // its own, and a static word inside the hole is one more thing to
    // clip. Asked for directly.
    ov_set(n++, "CPU", pct * 10, val, NULL, NULL, UUI_METER_RING);

    // Memory: used against total, which is every frame the allocator
    // manages -- the same number About calls "usable".
    struct query_meminfo mi;
    if (sys_query_record(QUERY_MEMINFO, 0, &mi, sizeof mi) >= (int)sizeof mi) {
        unsigned long long total = mi.frame_total * mi.frame_bytes;
        unsigned long long freeb = mi.frame_free * mi.frame_bytes;
        unsigned long long used  = total > freeb ? total - freeb : 0;
        int per = total ? (int)((used * 1000ULL) / total) : 0;
        snprintf(val, sizeof val, "%d%%", per / 10);
        human_size_iec(unit, sizeof unit, used);
        char t[16];
        human_size_iec(t, sizeof t, freeb);
        snprintf(det, sizeof det, "%s free", t);
        // "used" not "12.0 GiB used": the hole is only as wide as the
        // ring's inside, and a string longer than that is CLIPPED, which
        // reads as a rendering bug rather than as a long label.
        ov_set(n++, "Memory", per, val, unit, det, UUI_METER_RING);
    }

    // Disks: one bar per mounted filesystem, root first, capped. A
    // machine with more mounts than tiles shows the first few rather
    // than overflowing the page -- uui_layout does not shrink children.
    struct query_fsinfo fs;
    for (int i = 0; n < OV_METERS; i++) {
        int got = sys_query_record(QUERY_FSINFO, i, &fs, sizeof fs);
        if (got <= 0) break;
        if (got < (int)sizeof fs) break;
        if (!(fs.flags & QUERY_FS_MOUNTED) || !fs.total_bytes) continue;
        int per = (int)((fs.used_bytes * 1000ULL) / fs.total_bytes);
        snprintf(val, sizeof val, "%d%%", per / 10);
        human_size_iec(unit, sizeof unit, fs.used_bytes);
        char t[16];
        human_size_iec(t, sizeof t, fs.total_bytes);
        snprintf(det, sizeof det, "of %s", t);
        char cap[24];
        snprintf(cap, sizeof cap, "%s", fs.point[0] ? fs.point : fs.name);
        ov_set(n++, cap, per, val, unit, det, UUI_METER_BAR);
    }

    g_ov_count = n;
}

static int on_tick(struct uapp *a) {
    // Report the table's rect whenever it CHANGES, not just at open.
    // A geometry logged once at startup cannot show whether a resize
    // reflowed, which is exactly the question a resize bug raises.
    // The table and the tabs report themselves from on_draw (the walk,
    // deduped per frame); only the buttons, members of a group with no
    // rect of its own, are reported here.

    refresh();
    refresh_overview();
    select_page(a, g_tabs.selected);
    report_sort(0);
    (void)a;
    return 1; // repaint
}

// The widgets report themselves (ui/uui_describe.h); the two buttons
// live in a group with no rect of its own, so they are the app's.
static void on_draw(struct uapp *a, struct uapp_draw *d) {
    (void)d;
    uapp_log_layout(a, "taskmgr");
    uapp_logf_layout("taskmgr: layout btn_end %d %d %d %d\n",
            g_buttons[BTN_END].x, g_buttons[BTN_END].y,
            g_buttons[BTN_END].w, g_buttons[BTN_END].h);
    uapp_logf_layout("taskmgr: layout btn_kill %d %d %d %d\n",
            g_buttons[BTN_KILL].x, g_buttons[BTN_KILL].y,
            g_buttons[BTN_KILL].w, g_buttons[BTN_KILL].h);
}

static void on_open(struct uapp *a) {
    (void)a;
    g_prev_ns = sys_monotonic_ns();
    refresh();
    refresh_overview();
    select_page(a, g_tabs.selected);
    set_labels();
    // Report the layout for the test tool, per this repo's rule that a
    // geometry a tool would otherwise re-derive belongs in the app's own
    // log (four tools have been bitten by re-deriving one).
    // The BUTTONS too. A geometry an app does not report is one a test
    // re-derives in Python and gets wrong -- which happened here on the
    // first attempt at driving this window, and is the same mistake four
    // other tools in this repo have already paid for.
    uapp_logf_layout("taskmgr: layout btn_end %d %d %d %d\n",
            g_buttons[BTN_END].x, g_buttons[BTN_END].y,
            g_buttons[BTN_END].w, g_buttons[BTN_END].h);
    uapp_logf_layout("taskmgr: layout btn_kill %d %d %d %d\n",
            g_buttons[BTN_KILL].x, g_buttons[BTN_KILL].y,
            g_buttons[BTN_KILL].w, g_buttons[BTN_KILL].h);
    // THE TAB RECTS, because a tab is as wide as its label and a tool
    // that guessed would click the wrong one the day a label changes.
    // Same rule as every other layout line here: ask the app.
    ulogf("taskmgr: page %d\n", g_tabs.selected);
    ulogf("taskmgr: rows %d\n", g_row_count);

    // The sort state, and the pids IN SCREEN ORDER. The order line is
    // what makes a sorting test assertable at all: "the table changed"
    // is satisfied by a repaint, and reading the order out of pixels
    // would mean OCR. This is the same rule as every other layout line
    // here -- ask the app, do not re-derive.
    report_sort(1);
}

// The table is the only thing here that wants the keyboard, so keys go
// straight to it -- the File Manager's arrangement. A uapp_desc.focus
// ring would be the toolkit's own idiom, but it would put a Tab stop on
// the button group, and no widget here draws a focus indicator yet
// (docs/roadmap.md's item), so Tab would move a cursor nobody can see.
static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    if (!uui_table_key(&g_table, key)) return;
    // Report it the way a click on a row is reported, so the armed
    // action is cleared and the selection is logged from one place.
    on_widget(a, ID_TABLE, UUI_REASON_KEY);
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
    // Taller than the table alone needed: the Overview's two gauges are
    // nine text rows each and a disk tile follows them. It still
    // scrolls when it has to, so this is a comfortable start rather
    // than a requirement.
    *h = ugfx_char_h() * 26;
}


int main(void) {
    uui_table_init(&g_table, 0, 0, 100, 100, COLUMNS, COL_COUNT, cell, 0);
    // Typing a letter seeks by NAME, not by PID: column 0 here is a
    // number nobody knows by heart.
    uui_table_set_seek_col(&g_table, 1);
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

    PROC_ITEMS[0] = (struct uui_item){ .ops = &uui_table_ops, .widget = &g_table,
                                        .id = ID_TABLE, .name = "table",
                                        .flags = UUI_FILL_W | UUI_FILL_H };
    PROC_ITEMS[1] = (struct uui_item){ .ops = &uui_button_group_ops,
                                        .widget = &g_group, .id = ID_BUTTONS };
    PROC_LAYOUT = (struct uui_layout){ .dir = UUI_COLUMN, .items = PROC_ITEMS,
                                        .count = 2, .margin = 0 };

    // THE TWO RINGS SIDE BY SIDE, the disks stacked underneath: KDE's
    // arrangement, and the one that stops a full-width tile spending a
    // whole row on one circle. Nested layouts, which the router
    // descends through on its own.
    for (int i = 0; i < OV_METERS; i++) uui_meter_init(&g_ov[i]);

    OV_GAUGES[0] = (struct uui_item){ .ops = &uui_meter_ops, .widget = &g_ov[0],
                                       .flags = UUI_FILL_W | UUI_FILL_H };
    OV_GAUGES[1] = (struct uui_item){ .ops = &uui_meter_ops, .widget = &g_ov[1],
                                       .flags = UUI_FILL_W | UUI_FILL_H };
    OV_GAUGE_ROW = (struct uui_layout){ .dir = UUI_ROW, .items = OV_GAUGES,
                                         .count = 2, .margin = 0 };

    OV_ITEMS[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &OV_GAUGE_ROW,
                                      .flags = UUI_FILL_W };
    for (int i = 0; i < OV_DISKS_MAX; i++) {
        OV_ITEMS[1 + i] = (struct uui_item){ .ops = &uui_meter_ops,
                                              .widget = &g_ov[2 + i],
                                              .flags = UUI_FILL_W };
    }
    OV_LAYOUT = (struct uui_layout){ .dir = UUI_COLUMN, .items = OV_ITEMS,
                                      .count = 1, .margin = 0 };
    uui_scrollview_init(&OV_SCROLL, &OV_LAYOUT);

    // No on_select callback: the tab arrives through on_widget like
    // every other control, which is the one path that carries the
    // `struct uapp *` the re-layout needs.
    uui_tabs_init(&g_tabs, TABS, 2, NULL);

    ITEMS[0] = (struct uui_item){ .ops = &uui_tabs_ops, .widget = &g_tabs,
                                   .id = ID_TABS, .name = "tabs" };
    ITEMS[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &PROC_LAYOUT,
                                   .flags = UUI_FILL_W | UUI_FILL_H };

    LAYOUT = (struct uui_layout){ .dir = UUI_COLUMN, .items = ITEMS, .count = 2 };

    struct uapp_desc desc = {
        .title = "Task Manager",
        .on_draw = on_draw,
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
        .on_key = on_key,
        .on_widget = on_widget,
        .on_open = on_open,
        .on_tick = on_tick,
    };
    return uapp_run(&desc);
}
