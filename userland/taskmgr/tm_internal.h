#ifndef TM_INTERNAL_H
#define TM_INTERNAL_H

// Task Manager's parts, shared between its files. The shell -- the
// navigation rail, the process model every page reads, the window --
// is userland/gui/system/taskmgr.c; each page is a file here, the
// File Manager's and System Settings' arrangement (userland/fm/,
// userland/settings/).

#include <stdint.h>
#include "rt/sys.h"

struct uapp;
struct uui_item;
struct uui_focusable;
struct uui_menubar;

// How often everything is re-read, in milliseconds. Long enough that a
// CPU percentage is measured over a real interval rather than a couple
// of ticks, short enough to feel live.
#define TM_REFRESH_MS 500

// --- the process model (taskmgr.c) -------------------------------------
//
// One row per PROCESS: a thread (tgid != pid) is folded into its owner,
// its CPU time added and counted in `threads`, which is Windows' and
// KDE's unit. Re-read every tick whatever page is showing, because the
// Performance page's CPU figures and the Services page's pids are read
// from it too.

enum { TM_GROUP_APPS, TM_GROUP_BACKGROUND, TM_GROUP_SYSTEM, TM_GROUPS };

#define TM_PATH_MAX 96
#define TM_NAME_MAX 32

struct tm_proc {
    int pid, ppid, pgid;
    unsigned state, wait;     // PROC_STATE_*, PROC_WAIT_*
    int threads;              // 1 + the threads folded into it
    unsigned long long cpu_ns;
    unsigned long long mem_bytes;
    unsigned cpu_pm;          // per mille of the CPU over the last tick
    int group;                // TM_GROUP_*
    int app;                  // index into the desktop entries, or -1
    char name[PROC_NAME_MAX]; // the process's own
    char title[TM_NAME_MAX];  // what the table shows: the entry's Name, else `name`
    char path[TM_PATH_MAX];   // "" when the kernel does not know
};

extern struct tm_proc g_proc[SYS_PROC_MAX];
extern int g_nproc;
extern int g_desktop_pid;          // the compositor's pid, or 0
extern unsigned g_cpu_pm;          // the whole machine, per mille
extern unsigned long long g_mem_used, g_mem_total;
extern int g_threads;              // every thread, the processes' included

int tm_proc_row(int pid);          // g_proc index of `pid`, or -1
const char *tm_status_text(const struct tm_proc *p);

// A desktop entry: what an Exec= path is called and drawn as.
struct tm_app { char exec[TM_PATH_MAX]; char name[TM_NAME_MAX]; char icon[24]; };
extern struct tm_app g_apps[];
extern int g_napps;

// The same history for the pid selected on the Processes page. Reset
// when the selection moves, so a chart never splices two processes.
void tm_track_pid(int pid);

// --- the pages --------------------------------------------------------
//
// Each page is ONE item the shell swaps into its right-hand slot, and a
// handful of hooks. A hook returns 1 when it consumed the event.

struct tm_page {
    const char *label;
    const char *icon;
    struct uui_item *root;               // what the shell lays out
    void (*open)(struct uapp *a);        // the page became visible
    void (*tick)(struct uapp *a, int shown);
    int  (*widget)(struct uapp *a, int id, int reason);
    int  (*action)(struct uapp *a, int code);   // a button's click; 1 = repaint
    int  (*key)(struct uapp *a, int key, unsigned mods);
    void (*press)(struct uapp *a, int x, int y, unsigned buttons);
    void (*release)(struct uapp *a, int x, int y, unsigned buttons);
    // The page's stops in the focus ring, after the rail's: written to
    // `out`, at most `cap`, and counted. The first one is where focus
    // lands when the page opens.
    int (*focusables)(struct uui_focusable *out, int cap);
};

// Widget ids, one range per page, so a shared on_widget can dispatch by
// range without a table of who owns what.
#define TM_ID_RAIL     1
#define TM_ID_PROCS    100
#define TM_ID_PERF     200
#define TM_ID_SVC      300
#define TM_ID_CTX      400   // the one context menu, shared

// The shell's, for a page: show another page, move the keyboard focus.
void tm_show_page(struct uapp *a, int index);
void tm_focus(void *widget);
void tm_relayout(void);            // lay the window out again, now
int  tm_focus_park(void);          // no focus stop while a menu is open; returns the old one
void tm_focus_restore(int index);
extern struct uui_menubar g_ctx;   // the one context menu, the shell's

void tm_procs_init(struct tm_page *p);
void tm_procs_size(void);   // pins the details pane; from the shell's size hook
int  tm_procs_fit(void);    // hides/shows the pane by width; 1 = lay out again
int  tm_procs_min_width(void);   // the page with its whole toolbar showing
void tm_perf_init(struct tm_page *p);
void tm_services_init(struct tm_page *p);

// Every page's per-tick sampling, including the ones not showing --
// history is collected whether or not anybody is looking at it.
void tm_perf_sample(void);

// --- services (tm_services.c) ------------------------------------------
struct tm_service {
    char name[24];
    char state[12];   // init's word: running, stopped, exited, done, ...
    int  pid;
    int  fails;
    char ready[8];
    char exec[TM_PATH_MAX];
    char desc[96];
};
#define TM_SERVICES_MAX 64   // init.c's SVC_MAX
extern struct tm_service g_svc[TM_SERVICES_MAX];
extern int g_nsvc;
void tm_services_read(void);       // /run/init.status, cheap: a file read
int  tm_service_of_pid(int pid);   // index into g_svc, or -1
void tm_services_select(const char *name);   // "Go to service" lands on it

#endif
