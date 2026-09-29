// Task Manager, as a RING-3 PROCESS: a navigation rail of three pages --
// Processes, Performance, Services -- which is Windows 11's Task Manager
// shape (22H2 moved it from tabs to a rail) with KDE System Monitor's
// tree and details pane on the Processes page.
//
// THIS FILE IS THE SHELL: the rail, the window, and the PROCESS MODEL
// every page reads (tm_internal.h). The pages are userland/taskmgr/,
// one file each, and share nothing with each other but what is declared
// there -- the File Manager's and System Settings' arrangement.
//
// A process is listed ONCE, its threads folded in (tgid != pid), which
// is the unit both Windows and KDE list. It is grouped the way Windows
// groups it: an APP is a program with a desktop entry, or anything
// started from one (a Terminal's shell) or from the desktop; SYSTEM is
// init and what the kernel spawned (ppid 0); everything else is a
// BACKGROUND process. Windows decides "app" by a visible window, which
// no ABI here reports (QUERY_WINDOWS was retired); a desktop entry is
// what the Start menu and taskbar already use to name a program.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "rt/sys.h"
#include "ui/ulog.h"
#include "ui/ugfx.h"
#include "ui/uapp.h"
#include "ui/utheme.h"
#include "ui/uui_widget.h"
#include "ui/uui_layout.h"
#include "ui/uui_sidebar.h"
#include "ui/uui_menubar.h"
#include "ui/uui_focus.h"
#include "lib/uconf.h"
#include "lib/uopen.h"
#include "etc_config.h"
#include "kpath.h"
#include "query_abi.h"
#include "taskmgr/tm_internal.h"

// --- the process model -------------------------------------------------

struct tm_proc g_proc[SYS_PROC_MAX];
int g_nproc;
int g_desktop_pid;
unsigned g_cpu_pm;
unsigned long long g_mem_used, g_mem_total;
int g_threads;

// Last tick's CPU totals, BY PID: rows move as processes come and go, so
// indexing by row would charge one process's time to whichever landed
// in its slot -- nonsense exactly when the table is busiest.
static struct { int pid; unsigned long long cpu; } g_prev[SYS_PROC_MAX];
static int g_nprev;
static unsigned long long g_prev_ns;
static unsigned long long g_load_proc, g_load_kernel;

static struct proc_info g_info[SYS_PROC_MAX];   // static: 4.6 KiB off the stack

int tm_proc_row(int pid) {
    for (int i = 0; i < g_nproc; i++)
        if (g_proc[i].pid == pid) return i;
    return -1;
}

const char *tm_status_text(const struct tm_proc *p) {
    switch (p->state) {
    case PROC_STATE_RUNNING:
    case PROC_STATE_READY:   return "Running";
    case PROC_STATE_STOPPED: return "Stopped";
    case PROC_STATE_ZOMBIE:  return "Exited";
    case PROC_STATE_BLOCKED: break;
    default:                 return "-";
    }
    // Plain words for WHY it waits -- Linux's WCHAN, which Task Manager
    // on Windows cannot show at all.
    switch (p->wait) {
    case PROC_WAIT_EVENT:  return "Waiting for input";
    case PROC_WAIT_PIPE:   return "Reading a pipe";
    case PROC_WAIT_CHILD:  return "Waiting for a child";
    case PROC_WAIT_TIMER:  return "Sleeping";
    case PROC_WAIT_KEY:    return "Waiting for a key";
    case PROC_WAIT_TTY:    return "Writing a terminal";
    case PROC_WAIT_THREAD: return "Joining a thread";
    case PROC_WAIT_NET:    return "Waiting for network";
    case PROC_WAIT_FUTEX:  return "Waiting";
    case PROC_WAIT_SIGNAL: return "Waiting for a signal";
    case PROC_WAIT_LOCK:   return "Waiting for a lock";
    case PROC_WAIT_DISK:   return "Waiting for the disk";
    default:               return "Waiting";
    }
}

// --- desktop entries -----------------------------------------------------

#define TM_APPS_MAX 48
#define APPS_DIR "/usr/wm/applications"   // uopen.c's DESKTOP_ENTRY_DIR
struct tm_app g_apps[TM_APPS_MAX];
int g_napps;

// Read ONCE at start: a program installed while Task Manager is open is
// shown under its process name until the next start, which is the
// cheaper wrong answer than a directory walk every tick.
static void load_apps(void) {
    static struct sys_dirent ents[SYS_LISTDIR_MAX];
    int n = sys_listdir(APPS_DIR, ents, SYS_LISTDIR_MAX);
    struct etc_config_buf *cfg = malloc(sizeof *cfg);   // too big for a frame
    if (!cfg) return;
    for (int i = 0; i < n && g_napps < TM_APPS_MAX; i++) {
        if (ents[i].is_dir) continue;
        char path[96];
        if (!k_path_join(APPS_DIR, ents[i].name, path, sizeof path)) continue;
        if (!uconf_load(path, cfg)) continue;
        struct tm_app *e = &g_apps[g_napps];
        if (!etc_config_buf_get_in_or_top(cfg, UOPEN_ENTRY_SECTION, "Exec",
                                          e->exec, sizeof e->exec)) continue;
        if (!etc_config_buf_get_in_or_top(cfg, UOPEN_ENTRY_SECTION, "Name",
                                          e->name, sizeof e->name))
            strlcpy(e->name, ents[i].name, sizeof e->name);
        if (!etc_config_buf_get_in_or_top(cfg, UOPEN_ENTRY_SECTION, "Icon",
                                          e->icon, sizeof e->icon))
            e->icon[0] = '\0';
        // `Exec=` may carry arguments; the path is its first word.
        char *sp = strchr(e->exec, ' ');
        if (sp) *sp = '\0';
        g_napps++;
    }
    free(cfg);
}

static int app_for_path(const char *path) {
    if (!path[0]) return -1;
    for (int i = 0; i < g_napps; i++)
        if (strcmp(g_apps[i].exec, path) == 0) return i;
    return -1;
}

static unsigned long long prev_cpu(int pid) {
    for (int i = 0; i < g_nprev; i++)
        if (g_prev[i].pid == pid) return g_prev[i].cpu;
    return 0;   // new since the last tick: all its time is this tick's
}

static void classify(void) {
    g_desktop_pid = 0;
    for (int i = 0; i < g_nproc; i++)
        if (strcmp(g_proc[i].name, "toywm") == 0) g_desktop_pid = g_proc[i].pid;
    for (int i = 0; i < g_nproc; i++) {
        struct tm_proc *p = &g_proc[i];
        p->app = app_for_path(p->path);
        strlcpy(p->title, p->app >= 0 ? g_apps[p->app].name : p->name, sizeof p->title);
    }
    for (int i = 0; i < g_nproc; i++) {
        struct tm_proc *p = &g_proc[i];
        if (p->pid == 1 || p->ppid == 0) { p->group = TM_GROUP_SYSTEM; continue; }
        p->group = TM_GROUP_BACKGROUND;
        if (p->app >= 0) { p->group = TM_GROUP_APPS; continue; }
        // Started by an app, or by the desktop: an app's too. Bounded,
        // since a parent chain is data from the kernel, not a promise.
        int x = p->ppid;
        for (int steps = 0; x > 1 && steps < 8; steps++) {
            if (x == g_desktop_pid) { p->group = TM_GROUP_APPS; break; }
            int r = tm_proc_row(x);
            if (r < 0) break;
            if (g_proc[r].app >= 0) { p->group = TM_GROUP_APPS; break; }
            x = g_proc[r].ppid;
        }
    }
}

static void refresh_model(void) {
    unsigned long long now = sys_monotonic_ns();
    unsigned long long elapsed = now - g_prev_ns;

    int ninfo = 0;
    for (int i = 0; i < SYS_PROC_MAX; i++) {
        if (sys_proc_info(i, &g_info[ninfo]) != 0) continue;
        if (g_info[ninfo].pid == 0) continue;   // an empty slot: skip, do not stop
        ninfo++;
    }
    g_threads = ninfo;

    int n = 0;
    for (int i = 0; i < ninfo; i++) {
        const struct proc_info *in = &g_info[i];
        if (in->tgid && in->tgid != in->pid) continue;   // a thread: below
        struct tm_proc *p = &g_proc[n++];
        memset(p, 0, sizeof *p);
        p->pid = in->pid;
        p->ppid = in->ppid;
        p->pgid = in->pgid;
        p->state = in->state;
        p->wait = in->wait_reason;
        p->threads = 1;
        p->cpu_ns = in->cpu_ns;
        p->mem_bytes = in->mem_bytes;
        strlcpy(p->name, in->name, sizeof p->name);
    }
    g_nproc = n;
    for (int i = 0; i < ninfo; i++) {
        const struct proc_info *in = &g_info[i];
        if (!in->tgid || in->tgid == in->pid) continue;
        int r = tm_proc_row(in->tgid);
        if (r < 0) continue;
        g_proc[r].cpu_ns += in->cpu_ns;
        g_proc[r].threads++;
        // A process is running while ANY of its threads is.
        if (in->state == PROC_STATE_RUNNING || in->state == PROC_STATE_READY)
            g_proc[r].state = in->state;
    }

    for (int i = 0; i < n; i++) {
        struct tm_proc *p = &g_proc[i];
        unsigned long long was = prev_cpu(p->pid);
        unsigned long long used = p->cpu_ns > was ? p->cpu_ns - was : 0;
        p->cpu_pm = elapsed ? (unsigned)(used * 1000ULL / elapsed) : 0;
        if (p->cpu_pm > 1000) p->cpu_pm = 1000;   // a slot reused mid-interval
    }
    g_nprev = n;
    for (int i = 0; i < n; i++) { g_prev[i].pid = g_proc[i].pid; g_prev[i].cpu = g_proc[i].cpu_ns; }
    g_prev_ns = now;

    struct query_procpath pp;
    QUERY_FOREACH(QUERY_PROCPATH, pp, qi) {
        int r = tm_proc_row(pp.pid);
        if (r >= 0) strlcpy(g_proc[r].path, pp.path, sizeof g_proc[r].path);
    }
    classify();

    // The machine: the CPU against the kernel's own idle, as the Overview
    // rings measured it; memory in frames.
    struct query_cpuload cl;
    if (sys_query_record(QUERY_CPULOAD, 0, &cl, sizeof cl) >= (int)sizeof cl) {
        unsigned long long dp = cl.proc_ns - g_load_proc, dk = cl.kernel_ns - g_load_kernel;
        if (g_load_proc && dp + dk) g_cpu_pm = (unsigned)(dp * 1000ULL / (dp + dk));
        if (g_cpu_pm > 1000) g_cpu_pm = 1000;
        g_load_proc = cl.proc_ns;
        g_load_kernel = cl.kernel_ns;
    }
    struct query_meminfo mi;
    if (sys_query_record(QUERY_MEMINFO, 0, &mi, sizeof mi) >= (int)sizeof mi) {
        g_mem_total = mi.frame_total * mi.frame_bytes;
        unsigned long long freeb = mi.frame_free * mi.frame_bytes;
        g_mem_used = g_mem_total > freeb ? g_mem_total - freeb : 0;
    }
}

// --- the shell ----------------------------------------------------------

enum { PAGE_PROCS, PAGE_PERF, PAGE_SERVICES, PAGE_COUNT };
static struct tm_page g_pages[PAGE_COUNT];
static int g_page;

static struct uui_sidebar_row g_rail_rows[PAGE_COUNT];
static struct uui_sidebar g_rail;

// The one context menu, a second uui_menubar with no bar of its own
// (docs/conventions/gui.md). The Processes and Services pages fill it.
struct uui_menubar g_ctx;

// Laid out: the rail and the page. Routed: those and the context menu,
// LAST so it is hit-tested first -- its popup covers whatever is under it.
static struct uui_item ITEMS[3];
static struct uui_layout LAYOUT;

#define FOCUS_MAX 12
static struct uui_focusable g_focusables[FOCUS_MAX];
static struct uui_focus g_focus;

static struct uapp *g_app;

// Moves keyboard focus to `widget` if it is a stop on the ring.
void tm_focus(void *widget) {
    for (int i = 0; i < g_focus.count; i++)
        if (g_focus.items[i].widget == widget) { uui_focus_set(&g_focus, i); return; }
}

// A context menu has no key slot of its own (uui_menubar_ops), and the
// focus ring is offered every key before on_key -- so while a page's menu
// is open the ring is PARKED, and the arrows and Enter reach the menu
// through on_key instead of moving the table underneath it.
int tm_focus_park(void) {
    int was = g_focus.current;
    uui_focus_set(&g_focus, -1);
    return was;
}

void tm_focus_restore(int index) { uui_focus_set(&g_focus, index); }

void tm_relayout(void) {
    if (g_app) uui_layout_run(&LAYOUT, 0, 0, uapp_width(g_app), uapp_height(g_app));
}

static void select_page(struct uapp *a, int index) {
    if (index < 0 || index >= PAGE_COUNT) return;
    g_page = index;
    const struct tm_page *p = &g_pages[index];
    ITEMS[1].ops = p->root->ops;
    ITEMS[1].widget = p->root->widget;
    ITEMS[1].name = p->root->name;

    // The ring: the rail, then the page's own stops.
    g_focusables[0] = (struct uui_focusable){ &g_rail, &uui_sidebar_ops };
    int n = 1 + (p->focusables ? p->focusables(g_focusables + 1, FOCUS_MAX - 1) : 0);
    uui_focus_init(&g_focus, g_focusables, n);
    uui_focus_set(&g_focus, n > 1 ? 1 : 0);

    if (p->open) p->open(a);
    if (a) uui_layout_run(&LAYOUT, 0, 0, uapp_width(a), uapp_height(a));
    ulogf("taskmgr: page %d\n", index);
}

void tm_show_page(struct uapp *a, int index) {
    uui_sidebar_select_id(&g_rail, index);
    select_page(a, index);
}

static void on_widget(struct uapp *a, int id, int reason) {
    if (id == TM_ID_RAIL) {
        int want = uui_sidebar_selected_id(&g_rail);
        if (want != g_page) select_page(a, want);
        uapp_redraw(a);
        return;
    }
    const struct tm_page *p = &g_pages[g_page];
    if (p->widget && p->widget(a, id, reason)) uapp_redraw(a);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    const struct tm_page *p = &g_pages[g_page];
    if (p->key && p->key(a, key, mods)) uapp_redraw(a);
}

static void on_press(struct uapp *a, int x, int y, unsigned buttons) {
    const struct tm_page *p = &g_pages[g_page];
    if (p->press) p->press(a, x, y, buttons);
}

static void on_release(struct uapp *a, int x, int y, unsigned buttons) {
    const struct tm_page *p = &g_pages[g_page];
    if (p->release) p->release(a, x, y, buttons);
}

// Every page samples every tick, showing or not: history is collected
// whether or not anybody is looking at it, so a page opened after a
// spike still shows the spike.
static int on_tick(struct uapp *a) {
    static int ticks;
    refresh_model();
    if (++ticks % 4 == 0) tm_services_read();   // two seconds; it is a file
    tm_perf_sample();
    for (int i = 0; i < PAGE_COUNT; i++)
        if (g_pages[i].tick) g_pages[i].tick(a, i == g_page);
    uui_layout_run(&LAYOUT, 0, 0, uapp_width(a), uapp_height(a));
    return 1;
}

// Before the widgets draw and after every layout, so a resize that
// leaves the table too narrow drops the details pane in the same frame.
static void on_draw(struct uapp *a, struct uapp_draw *d) {
    (void)d;
    if (g_page == PAGE_PROCS && tm_procs_fit())
        uui_layout_run(&LAYOUT, 0, 0, uapp_width(a), uapp_height(a));
    uapp_log_layout(a, "taskmgr");
}

static void on_open(struct uapp *a) {
    g_app = a;
    g_prev_ns = sys_monotonic_ns();
    refresh_model();
    tm_services_read();
    tm_perf_sample();
    for (int i = 0; i < PAGE_COUNT; i++)
        if (g_pages[i].tick) g_pages[i].tick(a, i == g_page);
    select_page(a, g_page);
    ulogf("taskmgr: rows %d\n", g_nproc);
}

// Static, not main()'s local: on_size() sets the minimum from the font,
// which is known only once uapp_run() has started.
static struct uapp_desc g_desc;

static void on_size(int *w, int *h) {
    // FROM WHAT THE WIDGETS ASK FOR, plus room for names: the table's Name
    // column stretches, so its natural width is only its title, and a
    // width fixed in widest-glyph units starved it to nothing under the
    // laptop's larger face while fitting in QEMU.
    tm_procs_size();
    int nw = 0, nh = 0;
    uui_layout_natural_size(&LAYOUT, &nw, &nh);
    // Names, and the filter box, which asks for no width of its own.
    *w = nw + ugfx_char_advance('0') * (18 + 16);
    *h = ugfx_char_h() * 34;
    if (*h < nh) *h = nh;

    // A MINIMUM, so a resize -- or a remembered size -- cannot squeeze the
    // toolbar off the window: the rail, the page's toolbar, the margins.
    int rw = 0, rh = 0;
    uui_sidebar_ops.natural_size(&g_rail, &rw, &rh);
    g_desc.min_w = rw + tm_procs_min_width() + 3 * uui_layout_margin(&LAYOUT)
                   + uui_layout_gap(&LAYOUT);
    g_desc.min_h = ugfx_char_h() * 16;
}

int main(void) {
    load_apps();
    tm_procs_init(&g_pages[PAGE_PROCS]);
    tm_perf_init(&g_pages[PAGE_PERF]);
    tm_services_init(&g_pages[PAGE_SERVICES]);

    for (int i = 0; i < PAGE_COUNT; i++)
        g_rail_rows[i] = (struct uui_sidebar_row){ .label = g_pages[i].label,
                                                   .kind = UUI_SIDEBAR_TOP,
                                                   .icon = g_pages[i].icon, .id = i };
    uui_sidebar_init(&g_rail, 0, 0, 0, 0, g_rail_rows, PAGE_COUNT);
    uui_sidebar_select_id(&g_rail, PAGE_PROCS);
    uui_menubar_init(&g_ctx, 0, 0);

    ITEMS[0] = (struct uui_item){ .ops = &uui_sidebar_ops, .widget = &g_rail,
                                   .id = TM_ID_RAIL, .name = "rail", .flags = UUI_FILL_H };
    ITEMS[1] = (struct uui_item){ .ops = g_pages[0].root->ops, .widget = g_pages[0].root->widget,
                                   .name = g_pages[0].root->name,
                                   .flags = UUI_FILL_W | UUI_FILL_H };
    ITEMS[2] = (struct uui_item){ .ops = &uui_menubar_ops, .widget = &g_ctx,
                                   .id = TM_ID_CTX, .name = "ctxmenu" };
    LAYOUT = (struct uui_layout){ .dir = UUI_ROW, .items = ITEMS, .count = 2 };

    g_desc = (struct uapp_desc){
        .title = "Task Manager",
        .app_id = "taskmgr",
        .layout = &LAYOUT,
        .tick_ms = TM_REFRESH_MS,
        .on_size = on_size,
        .flags = UAPP_RESIZABLE | UAPP_SINGLE_INSTANCE,
        .widgets = ITEMS,
        .widget_count = 3,
        .focus = &g_focus,
        .on_draw = on_draw,
        .on_key = on_key,
        .on_press = on_press,
        .on_release = on_release,
        .on_widget = on_widget,
        .on_open = on_open,
        .on_tick = on_tick,
    };
    return uapp_run(&g_desc);
}
