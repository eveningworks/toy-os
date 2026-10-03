// Disk Mark -- sequential and random throughput, CrystalDiskMark's four
// readings with GNOME Disks' graph of the run (mockup D2, 2026-10-03):
// a command bar (Run, Stop, the size, the volume), four meter cards, the
// throughput over the run with its four phases marked, and every run
// kept in a history compared with the one before.
//
// **IT DOES NO I/O ITSELF.** It spawns /bin/diskbench and polls the
// report that writes. Running the passes in on_tick, sliced across
// frames, was the first version and was wrong: the slice was bounded in
// time but one "transfer" was not, and the window read "(Not
// Responding)" for a pass. A child is also the shape CrystalDiskMark's
// worker and the File Manager's /bin/cp take.
//
// THE LABELS SAY WHAT THIS OS ACTUALLY DOES: plain `SEQ`, not CDM's
// `SEQ1M`, because the request size is whatever SYS_WRITE_MAX is, and
// the status bar names it.
#include <stdint.h>
#include "rt/sys.h"
#include "syscall_abi.h"   // SYS_WRITE_MAX -- named in the status bar
#include "kpath.h"
#include "ui/ulog.h"
#include "ui/ugfx.h"
#include "ui/uui.h"
#include "ui/uapp.h"
#include "ui/utheme.h"
#include "ui/uui_meter.h"
#include "ui/uui_chart.h"
#include "ui/uui_toolbar.h"
#include "ui/uui_segmented.h"
#include "ui/uui_table.h"
#include "lib/uclip.h"
#include "lib/udate.h"
#include <stdio.h>
#include "lib/unum.h"
#include <string.h>
#include <stdlib.h>
#include <locale.h>
#include <time.h>
#include <sys/stat.h>
#include "tmppath.h"
#include "keyboard.h"

// TMP_VOLATILE: a report the GUI polls and then deletes, wanted for
// seconds and never across a boot.
static const char *result_path(void) {
    static char p[64];
    if (!p[0]) tmppath(p, sizeof p, TMP_VOLATILE, "diskmark.out");
    return p;
}
#define RESULT_PATH result_path()
// TMP_PERSISTENT, never TMP_VOLATILE: a disk benchmark pointed at the
// RAM scratch directory measures memcpy and reports a number that is
// enormous and meaningless, with nothing about it looking wrong.
static const char *root_work_path(void) {
    static char p[64];
    if (!p[0]) tmppath(p, sizeof p, TMP_PERSISTENT, "diskmark.tmp");
    return p;
}
#define BENCH_PATH   "/bin/diskbench"
#define HISTORY_DIR  "/var/lib/diskmark"
#define HISTORY_PATH "/var/lib/diskmark/history"

// Display order is CrystalDiskMark's read-then-write; diskbench runs
// them write-first and names each result, so the orders never agree.
#define P_SEQ_READ  0
#define P_SEQ_WRITE 1
#define P_RND_READ  2
#define P_RND_WRITE 3
#define PROFILES    4

// What the log and tools/diskmark_test.py call them.
static const char *PROFILE_NAME[PROFILES] = {
    "SEQ Q1T1 READ", "SEQ Q1T1 WRITE", "RND4K Q1T1 READ", "RND4K Q1T1 WRITE",
};
// What the cards say: Q1T1 is the same for all four, so it is in the
// status bar once rather than on every card.
static const char *CAPTION[PROFILES] = { "SEQ READ", "SEQ WRITE", "RND4K READ", "RND4K WRITE" };
static const char *WIRE_NAME[PROFILES] = { "SEQ-read", "SEQ-write", "RND4K-read", "RND4K-write" };
static const char *PHASE_LABEL[PROFILES] = { "SEQ read", "SEQ write", "RND4K read", "RND4K write" };
static int is_write(int p) { return p == P_SEQ_WRITE || p == P_RND_WRITE; }

static const char *const SIZE_ITEMS[] = { "16 MiB", "64 MiB", "256 MiB" };
static const int SIZE_MIB[] = { 16, 64, 256 };
#define SIZE_COUNT 3

enum { ST_IDLE, ST_RUNNING, ST_STOPPING, ST_DONE, ST_FAILED };

static int g_state = ST_IDLE;
static int g_pid = -1;
static int g_running_p = -1;          // the profile being measured now
static uint64_t g_result[PROFILES];   // milli-MB/s
static uint64_t g_iops[PROFILES], g_lat_us[PROFILES];
static int g_run_mib;                 // what the CURRENT readings were taken at
static char g_run_vol[64];
static time_t g_run_when;

static char g_value[PROFILES][24];
static char g_detail[PROFILES][48];
static char g_status[96];
static char g_size_pane[16], g_call_pane[24], g_when_pane[24];

// The child's output, re-read whole each tick. Static: the ring-3 frame
// budget is 2 KiB.
static char g_out[4096];

// THE BIG NUMBER'S FONT, loaded once; a failed load falls back to the
// session bold.
static struct ugfx_font g_big;
static int g_big_ok;

// --- volumes ----------------------------------------------------------
//
// ANY MOUNTED, PERSISTENT, WRITABLE filesystem. A RAM-only one is
// refused (it would measure memcpy) and a read-only one -- /boot by
// policy (kernel/fs/mount.c) -- cannot hold the scratch file.
#define VOL_MAX 6
struct vol { char point[64]; char label[80]; char dev[16]; char fs[16]; int root; };
static struct vol g_vols[VOL_MAX];
static const char *g_vol_items[VOL_MAX];
static int g_nvols;

static void scan_volumes(void) {
    g_nvols = 0;
    struct query_fsinfo fs;
    for (unsigned i = 0; i < 8 && g_nvols < VOL_MAX; i++) {
        if (sys_query_record(QUERY_FSINFO, i, &fs, sizeof fs) < (int)sizeof fs) break;
        if (!(fs.flags & QUERY_FS_MOUNTED) || !(fs.flags & QUERY_FS_PERSISTENT) ||
            (fs.flags & QUERY_FS_RDONLY))
            continue;
        struct vol *v = &g_vols[g_nvols];
        strlcpy(v->point, fs.point, sizeof v->point);
        strlcpy(v->dev, fs.device[0] ? fs.device : "-", sizeof v->dev);
        strlcpy(v->fs, fs.name, sizeof v->fs);
        v->root = (fs.flags & QUERY_FS_ROOT) != 0;
        snprintf(v->label, sizeof v->label, "%s on %s", v->point, v->dev);
        g_vol_items[g_nvols] = v->label;
        g_nvols++;
    }
}

static struct uui_dropdown g_vol;

static const struct vol *chosen_vol(void) {
    int i = uui_dropdown_selected(&g_vol);
    return (i >= 0 && i < g_nvols) ? &g_vols[i] : NULL;
}

// The scratch file: the root keeps the persistent temp directory; any
// other volume gets one at its top.
static const char *work_path_for(const struct vol *v) {
    static char p[96];
    if (!v || v->root) return root_work_path();
    if (!k_path_join(v->point, "diskmark.tmp", p, sizeof p)) return root_work_path();
    return p;
}
static char g_work[96];   // the one the running child was given

// --- widgets ----------------------------------------------------------

#define ID_MENU    1
#define ID_TOOLBAR 2
#define ID_SIZE    3
#define ID_VOL     4
#define ID_HIST    5
#define ID_STATUS  6

#define CMD_RUN     1
#define CMD_STOP    2
#define CMD_COPY    3
#define CMD_HISTORY 4
#define CMD_EXIT    5
#define CMD_SIZE_0  10   // ..12

static struct uui_menubar g_menu;
static struct uui_toolbar g_tb;
static struct uui_segmented g_size;
static struct uui_meter g_meter[PROFILES];
static struct uui_chart g_chart;
static struct uui_table g_hist;
static struct uui_statusbar g_sb;
static int g_show_hist;

static const struct uui_menu_item file_items[] = {
    UUI_MENU("Copy results", CMD_COPY, "Ctrl-C"),
    UUI_MENU_SEP,
    UUI_MENU("Exit", CMD_EXIT, "Alt+F4"),
};
static const struct uui_menu_item test_items[] = {
    UUI_MENU("Run", CMD_RUN, "F5"),
    UUI_MENU("Stop", CMD_STOP, "Esc"),
    UUI_MENU_SEP,
    UUI_MENU("16 MiB",  CMD_SIZE_0,     0),
    UUI_MENU("64 MiB",  CMD_SIZE_0 + 1, 0),
    UUI_MENU("256 MiB", CMD_SIZE_0 + 2, 0),
};
static const struct uui_menu_item view_items[] = {
    UUI_MENU("History", CMD_HISTORY, "F9"),
};
static const struct uui_menu_item menu_bar[] = {
    UUI_SUBMENU("File", file_items),
    UUI_SUBMENU("Test", test_items),
    UUI_SUBMENU("View", view_items),
};

// Run and Stop at the left, labelled; the size and the volume sit
// between (placed by layout()); Copy and History at the right.
static const struct uui_toolbar_item tb_items[] = {
    { "tb-play",    "Run (F5)",          CMD_RUN,     "Run",  0,          "F5",     UTHEME_ACT_CREATE },
    { "tb-stop",    "Stop (Esc)",        CMD_STOP,    "Stop", 0,          "Esc",    UTHEME_ACT_DANGER },
    { "tb-copy",    "Copy the results",  CMD_COPY,    0,      UUI_TB_END, "Ctrl-C", UTHEME_ACT_EDIT },
    { "tb-history", "History (F9)",      CMD_HISTORY, 0,      0,          "F9",     UTHEME_ACT_VIEW },
};
#define TB_STOP 1
#define TB_COPY 2

static struct uui_item g_widgets[] = {
    { .ops = &uui_menubar_ops,   .widget = &g_menu,     .id = ID_MENU,    .name = "menu" },
    { .ops = &uui_toolbar_ops,   .widget = &g_tb,       .id = ID_TOOLBAR, .name = "toolbar" },
    { .ops = &uui_segmented_ops, .widget = &g_size,     .id = ID_SIZE,    .name = "size" },
    { .ops = &uui_table_ops,     .widget = &g_hist,     .id = ID_HIST,    .name = "history", .hidden = 1 },
    { .ops = &uui_statusbar_ops, .widget = &g_sb,       .id = ID_STATUS,  .name = "status" },
    // AFTER everything it can open over: its popup is drawn last.
    { .ops = &uui_dropdown_ops,  .widget = &g_vol,      .id = ID_VOL,     .name = "volume" },
};
#define W_HIST 3

static int size_mib(void) {
    int i = g_size.selected;
    return SIZE_MIB[(i >= 0 && i < SIZE_COUNT) ? i : 0];
}

// --- history ----------------------------------------------------------
//
// One line per finished run, newest LAST in the file and first on
// screen: `epoch MiB point device sr sw rr rw rri rwi rrl rwl` -- the
// four milli-MB/s, then the random IOPS and latencies.
#define HIST_MAX 50
struct run { long long when; int mib; char point[32]; char dev[16];
             unsigned long long mb[PROFILES]; unsigned long long rri, rwi, rrl, rwl; };
static struct run g_runs[HIST_MAX];
static int g_nruns;

static void history_load(void) {
    g_nruns = 0;
    FILE *f = fopen(HISTORY_PATH, "r");
    if (!f) { uui_table_set_rows(&g_hist, 0); return; }
    static struct run all[HIST_MAX];   // static: the ring-3 frame budget is 2 KiB
    int n = 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        struct run r;
        memset(&r, 0, sizeof r);
        if (sscanf(line, "%lld %d %31s %15s %llu %llu %llu %llu %llu %llu %llu %llu",
                   &r.when, &r.mib, r.point, r.dev, &r.mb[0], &r.mb[1], &r.mb[2], &r.mb[3],
                   &r.rri, &r.rwi, &r.rrl, &r.rwl) != 12)
            continue;
        if (n == HIST_MAX) { memmove(all, all + 1, sizeof all[0] * (HIST_MAX - 1)); n--; }
        all[n++] = r;
    }
    fclose(f);
    for (int i = 0; i < n; i++) g_runs[i] = all[n - 1 - i];   // newest first
    g_nruns = n;
    uui_table_set_rows(&g_hist, g_nruns);
}

static void history_append(void) {
    mkdir(HISTORY_DIR, 0755);   // first use; an existing one is fine
    FILE *f = fopen(HISTORY_PATH, "a");
    if (!f) return;
    fprintf(f, "%lld %d %s %s %llu %llu %llu %llu %llu %llu %llu %llu\n",
            (long long)g_run_when, g_run_mib, g_run_vol[0] ? g_run_vol : "/",
            chosen_vol() ? chosen_vol()->dev : "-",
            (unsigned long long)g_result[0], (unsigned long long)g_result[1],
            (unsigned long long)g_result[2], (unsigned long long)g_result[3],
            (unsigned long long)g_iops[P_RND_READ], (unsigned long long)g_iops[P_RND_WRITE],
            (unsigned long long)g_lat_us[P_RND_READ], (unsigned long long)g_lat_us[P_RND_WRITE]);
    fclose(f);
}

static const struct uui_table_column HIST_COLS[] = {
    { "When",     0,  UUI_TALIGN_LEFT },
    { "Volume",   11, UUI_TALIGN_LEFT },
    { "Size",     7,  UUI_TALIGN_RIGHT },
    { "SEQ R",    10, UUI_TALIGN_RIGHT },
    { "SEQ W",    10, UUI_TALIGN_RIGHT },
    { "RND4K R",  10, UUI_TALIGN_RIGHT },
    { "RND4K W",  10, UUI_TALIGN_RIGHT },
};

static void fmt_mb(char *out, int cap, unsigned long long milli) {
    snprintf(out, (unsigned)cap, "%llu.%llu", milli / 1000, (milli % 1000) / 100);
    unum_localize(out, (unsigned)cap, 0);
}

// A reading against the run BEFORE it on the same volume and size -- a
// comparison across sizes or disks says nothing.
static void hist_cell(void *ctx, int row, int col, char *out, int cap) {
    (void)ctx;
    const struct run *r = &g_runs[row];
    switch (col) {
    case 0: {
        time_t t = (time_t)r->when;
        struct tm tm;
        localtime_r(&t, &tm);
        udate_format_tm(out, (unsigned long)cap, &tm, UDATE_DATE | UDATE_TIME);
        return;
    }
    case 1: snprintf(out, (unsigned)cap, "%s  %s", r->point, r->dev); return;
    case 2: snprintf(out, (unsigned)cap, "%d MiB", r->mib); return;
    default: {
        int p = col == 3 ? P_SEQ_READ : col == 4 ? P_SEQ_WRITE : col == 5 ? P_RND_READ : P_RND_WRITE;
        char v[16];
        fmt_mb(v, sizeof v, r->mb[p]);
        const struct run *prev = NULL;
        for (int k = row + 1; k < g_nruns; k++)
            if (g_runs[k].mib == r->mib && !strcmp(g_runs[k].point, r->point)) { prev = &g_runs[k]; break; }
        if (prev && prev->mb[p]) {
            long long d = ((long long)r->mb[p] - (long long)prev->mb[p]) * 100 / (long long)prev->mb[p];
            snprintf(out, (unsigned)cap, "%s %+lld%%", v, d);
        } else {
            snprintf(out, (unsigned)cap, "%s", v);
        }
        return;
    }
    }
}

// --- readings ---------------------------------------------------------

static void set_status(const char *s) { strlcpy(g_status, s, sizeof g_status); }

// Each bar against the FASTEST of this run: a ceiling picked for an SSD
// leaves every bar here empty, and one picked for an emulator lies.
static void rescale_bars(void) {
    uint64_t best = 0;
    for (int i = 0; i < PROFILES; i++) if (g_result[i] > best) best = g_result[i];
    for (int i = 0; i < PROFILES; i++)
        uui_meter_set_fill(&g_meter[i], best ? (int)((g_result[i] * 1000ull) / best) : 0);
}

static int wire_index(const char *name, int len) {
    for (int i = 0; i < PROFILES; i++) {
        int n = (int)strlen(WIRE_NAME[i]);
        if (n == len && !strncmp(name, WIRE_NAME[i], (unsigned)n)) return i;
    }
    return -1;
}

// THE CHART: one series for reads and one for writes, a mark where each
// phase begins. A phase too short to be sampled still gets its mark and
// its result as one point.
static int g_phase_seen[PROFILES];
static int g_phase_samples[PROFILES];
static int g_last_p = -1;
static unsigned long long g_last_moved, g_last_us;
static char g_chart_value[40];

static void phase_begin(int p) {
    if (g_phase_seen[p]) return;
    g_phase_seen[p] = 1;
    uui_chart_set_series(&g_chart, is_write(p) ? 1 : 0);
    uui_chart_add_mark(&g_chart, PHASE_LABEL[p]);
    g_last_p = p;
    g_last_moved = g_last_us = 0;
}

static void sample(int p, unsigned long long moved, unsigned long long us) {
    phase_begin(p);
    if (p != g_last_p) { g_last_p = p; g_last_moved = g_last_us = 0; }
    if (us <= g_last_us || moved <= g_last_moved) return;
    // Bytes per microsecond is MB/s; in milli-MiB/s, the results' unit.
    unsigned long long dm = moved - g_last_moved, du = us - g_last_us;
    unsigned long long milli = (dm * 1000ull / 1048576ull) * 1000000ull / du;
    uui_chart_push(&g_chart, (uint32_t)milli);
    g_phase_samples[p]++;
    g_last_moved = moved;
    g_last_us = us;
    char v[16];
    fmt_mb(v, sizeof v, milli);
    snprintf(g_chart_value, sizeof g_chart_value, "%s  %s MB/s", PHASE_LABEL[p], v);
}

static void show_result(int p, uint64_t milli, uint64_t iops, uint64_t us) {
    // THE DRAIN IS IDEMPOTENT, SO THIS MUST BE: re-LOGGING a seen line
    // every tick floods dmesg.
    if (g_result[p] == milli) return;
    // A PHASE TOO SHORT TO SAMPLE is still drawn: its result as a flat
    // run of two points rather than a dot nobody sees.
    phase_begin(p);
    if (p == g_last_p)
        while (g_phase_samples[p] < 2) { uui_chart_push(&g_chart, (uint32_t)milli); g_phase_samples[p]++; }
    g_result[p] = milli;
    g_iops[p] = iops;
    g_lat_us[p] = us;
    fmt_mb(g_value[p], sizeof g_value[p], milli);
    // IOPS and latency on the random profiles only: at a 256 KiB request
    // the sequential op count is the byte count again.
    if (p == P_RND_READ || p == P_RND_WRITE) {
        char ms[16];
        snprintf(ms, sizeof ms, "%llu.%llu", (unsigned long long)(us / 1000),
                 (unsigned long long)((us % 1000) / 100));
        unum_localize(ms, sizeof ms, 0);
        snprintf(g_detail[p], sizeof g_detail[p], "%llu IOPS, %s ms avg", (unsigned long long)iops, ms);
        unum_localize(g_detail[p], sizeof g_detail[p], 0);
    } else {
        g_detail[p][0] = 0;
    }
    uui_meter_set(&g_meter[p], CAPTION[p], g_value[p], "MB/s", g_detail[p][0] ? g_detail[p] : NULL);
    ulogf("diskmark: result %s = %s MB/s %s\n", PROFILE_NAME[p], g_value[p],
          g_detail[p][0] ? g_detail[p] : "-");
}

// Re-reads the child's report whole and applies every line. Idempotent:
// re-applying what was seen costs a few compares and no read offset.
static void drain_output(void) {
    int fd = sys_open(RESULT_PATH, 0);
    if (fd < 0) return;
    int64_t n = sys_read(fd, g_out, sizeof g_out - 1);
    sys_close(fd);
    if (n <= 0) return;
    g_out[n] = 0;

    for (char *line = g_out; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        const char *p = strstr(line, "diskbench: ");
        if (p) {
            p += 11;
            if (!strncmp(p, "result ", 7)) {
                p += 7;
                const char *sp = strchr(p, ' ');
                unsigned long long milli = 0, iops = 0, us = 0;
                int idx = sp ? wire_index(p, (int)(sp - p)) : -1;
                if (idx >= 0 && sscanf(sp + 1, "%llu %llu %llu", &milli, &iops, &us) == 3)
                    show_result(idx, milli, iops, us);
            } else if (!strncmp(p, "progress ", 9)) {
                p += 9;
                const char *sp = strchr(p, ' ');
                int idx = sp ? wire_index(p, (int)(sp - p)) : -1;
                int pct = 0;
                unsigned long long moved = 0, us = 0;
                int got = idx >= 0 ? sscanf(sp + 1, "%d %llu %llu", &pct, &moved, &us) : 0;
                if (got >= 1 && !g_result[idx]) {
                    g_running_p = idx;
                    snprintf(g_status, sizeof g_status, "%s: %d%%", PHASE_LABEL[idx], pct);
                    snprintf(g_detail[idx], sizeof g_detail[idx], "running, %d%%", pct);
                    uui_meter_set(&g_meter[idx], CAPTION[idx], "--", NULL, g_detail[idx]);
                    uui_meter_set_fill(&g_meter[idx], pct * 10);
                }
                if (got == 3) sample(idx, moved, us);
            } else if (!strncmp(p, "error ", 6)) {
                snprintf(g_status, sizeof g_status, "diskbench failed: %s", p + 6);
                g_state = ST_FAILED;
            }
        }
        line = nl ? nl + 1 : 0;
    }
}

static void cleanup(void) {
    sys_unlink(RESULT_PATH);
    if (g_work[0]) sys_unlink(g_work);
}

static void reset_readings(void) {
    for (int i = 0; i < PROFILES; i++) {
        g_value[i][0] = g_detail[i][0] = 0;
        g_result[i] = g_iops[i] = g_lat_us[i] = 0;
        g_phase_seen[i] = 0;
        g_phase_samples[i] = 0;
        uui_meter_set(&g_meter[i], CAPTION[i], "--", NULL, "waiting");
        uui_meter_set_fill(&g_meter[i], 0);
        g_meter[i].active = 0;
    }
    g_running_p = -1;
    g_last_p = -1;
    g_chart_value[0] = 0;
    uui_chart_set_fit(&g_chart, 1);
}

static void begin_run(struct uapp *a) {
    if (g_pid > 0) return;   // running, or a stopped child not yet reaped
    const struct vol *v = chosen_vol();
    if (!v) {
        set_status("no writable disk volume is mounted -- RAM would measure memcpy");
        g_state = ST_FAILED;
        uapp_redraw(a);
        return;
    }
    reset_readings();
    strlcpy(g_work, work_path_for(v), sizeof g_work);
    cleanup();

    // THE CHILD WRITES THE REPORT ITSELF (`--out`) with no stdout: a pipe
    // would couple the two (PIPE_MAX is 8 KiB kernel-wide), so a window
    // slow to drain would block the benchmark it is timing.
    static char args[200];
    snprintf(args, sizeof args, "--size %d --path %s --out %s", size_mib(), g_work, RESULT_PATH);
    g_pid = sys_spawn(BENCH_PATH, args, -1);
    if (g_pid < 0) {
        set_status("could not start " BENCH_PATH);
        g_state = ST_FAILED;
        cleanup();
    } else {
        g_state = ST_RUNNING;
        g_run_mib = size_mib();
        strlcpy(g_run_vol, v->point, sizeof g_run_vol);
        g_run_when = time(NULL);
        snprintf(g_status, sizeof g_status, "Running %d MiB on %s...", size_mib(), v->point);
        ulogf("diskmark: spawned %s as pid %d\n", BENCH_PATH, g_pid);
    }
    uapp_redraw(a);
}

static void stop_run(struct uapp *a) {
    if (g_pid <= 0 || g_state != ST_RUNNING) return;
    // KILLED, and reaped on the next ticks; diskbench has no handler to
    // remove its scratch file, so cleanup() does once it is gone.
    sys_kill(g_pid, 9);
    g_state = ST_STOPPING;
    set_status("Stopping...");
    uapp_redraw(a);
}

static void copy_results(void) {
    char text[512];
    int n = snprintf(text, sizeof text, "Disk Mark -- %s, %d MiB, Q1T1, %u KiB per call\n",
                     g_run_vol[0] ? g_run_vol : "/", g_run_mib ? g_run_mib : size_mib(),
                     (unsigned)(SYS_WRITE_MAX / 1024));
    for (int p = 0; p < PROFILES && n < (int)sizeof text; p++) {
        char v[16];
        fmt_mb(v, sizeof v, g_result[p]);
        n += snprintf(text + n, sizeof text - (unsigned)n, "%-18s %8s MB/s%s%s\n", PROFILE_NAME[p],
                      g_result[p] ? v : "--", g_detail[p][0] && g_result[p] ? "  " : "",
                      g_result[p] ? g_detail[p] : "");
    }
    if (n > (int)sizeof text - 1) n = (int)sizeof text - 1;
    set_status(uclip_set_text(text, n) ? "Copied the results" : "The clipboard refused it");
}

static unsigned item_flags(int code) {
    int busy = g_pid > 0;
    switch (code) {
    case CMD_RUN:     return busy ? UUI_MI_DISABLED : 0;
    case CMD_STOP:    return g_state == ST_RUNNING ? 0 : UUI_MI_DISABLED;
    case CMD_COPY:    return (g_result[0] || g_result[3]) ? 0 : UUI_MI_DISABLED;
    case CMD_HISTORY: return g_show_hist ? UUI_MI_CHECKED : 0;
    default:
        if (code >= CMD_SIZE_0 && code < CMD_SIZE_0 + SIZE_COUNT)
            return (g_size.selected == code - CMD_SIZE_0 ? UUI_MI_CHECKED : 0) |
                   (busy ? UUI_MI_DISABLED : 0);
        return 0;
    }
}

static int on_close(struct uapp *a);

static void do_command(struct uapp *a, int code) {
    switch (code) {
    case CMD_RUN:     begin_run(a); break;
    case CMD_STOP:    stop_run(a); break;
    case CMD_COPY:    copy_results(); break;
    case CMD_HISTORY: g_show_hist = !g_show_hist; if (g_show_hist) history_load(); break;
    case CMD_EXIT:    on_close(a); uapp_quit(a, 0); break;
    default:
        if (code >= CMD_SIZE_0 && code < CMD_SIZE_0 + SIZE_COUNT && g_pid <= 0)
            g_size.selected = code - CMD_SIZE_0;
        break;
    }
    uapp_redraw(a);
}

// --- layout -----------------------------------------------------------

static int pad(void) { return ugfx_char_h() * 3 / 4; }
static int card_min_w(void) { return 17 * ugfx_char_advance('n'); }

static void layout(int cw, int ch) {
    int y = 0;
    int mh = uui_menubar_height(&g_menu);
    uui_menubar_set_geometry(&g_menu, 0, 0, cw, mh);
    uui_menubar_set_bounds(&g_menu, 0, 0, cw, ch);
    y += mh;
    int th = uui_toolbar_height(&g_tb);
    uui_toolbar_ops.set_geometry(&g_tb, 0, y, cw, th);

    // The size and the volume BETWEEN Stop and the right-hand group.
    int sx, sy, sw, sh, rx, ry, rw, rh;
    uui_toolbar_item_rect(&g_tb, TB_STOP, &sx, &sy, &sw, &sh);
    if (!uui_toolbar_item_rect(&g_tb, TB_COPY, &rx, &ry, &rw, &rh) || rw == 0) rx = cw;
    int gap = ugfx_char_h() / 2;
    int segw, segh;
    uui_segmented_natural_size(&g_size, &segw, &segh);
    int x = sx + sw + gap * 2;
    uui_segmented_set_geometry(&g_size, x, y + (th - segh) / 2);
    x += segw + gap * 2;
    int dw, dh;
    uui_dropdown_natural_size(&g_vol, &dw, &dh);
    if (dw > rx - gap * 2 - x) dw = rx - gap * 2 - x;
    if (dw < ugfx_char_h() * 3) dw = ugfx_char_h() * 3;
    uui_dropdown_set_geometry(&g_vol, x, y + (th - dh) / 2, dw, dh);
    y += th + 1;

    int sbh = uui_statusbar_height(&g_sb);
    uui_statusbar_set_geometry(&g_sb, 0, ch - sbh, cw, sbh);

    // Four cards in a row, or two by two when the window is narrow.
    int p = pad();
    int mw, mhh;
    uui_meter_natural_size(&g_meter[0], &mw, &mhh);
    // A FIXED floor, not the natural width -- that moves with the number
    // on the card, and the grid would reflow as a run filled it in.
    mw = card_min_w();
    int cols = (cw - 5 * p) / 4 >= mw ? 4 : 2;
    int rows = PROFILES / cols;
    int cardw = (cw - (cols + 1) * p) / cols;
    y += p;
    for (int i = 0; i < PROFILES; i++) {
        int c = i % cols, r = i / cols;
        uui_meter_ops.set_geometry(&g_meter[i], p + c * (cardw + p), y + r * (mhh + p), cardw, mhh);
    }
    y += rows * (mhh + p);

    int bottom_h = ch - sbh - p - y;
    if (bottom_h < 1) bottom_h = 1;
    uui_chart_set_geometry(&g_chart, p, y, cw - 2 * p, bottom_h);
    uui_table_ops.set_geometry(&g_hist, p, y, cw - 2 * p, bottom_h);
    g_widgets[W_HIST].hidden = !g_show_hist;
}

static void log_layout(struct uapp *a) {
    int x, y, w, h;
    // What tools/diskmark_test.py clicks and compares.
    if (uui_toolbar_item_rect(&g_tb, 0, &x, &y, &w, &h))
        uapp_logf_layout("diskmark: layout run %d %d %d %d\n", x, y, w, h);
    for (int i = 0; i < PROFILES; i++)
        uapp_logf_layout("diskmark: layout tile %d %d %d %d %d\n", i,
                         g_meter[i].x, g_meter[i].y, g_meter[i].w, g_meter[i].h);
    if (!g_show_hist)
        uapp_logf_layout("diskmark: layout chart %d %d %d %d samples %d marks %d\n",
                         g_chart.x, g_chart.y, g_chart.w, g_chart.h, g_chart.count,
                         g_chart.mark_n);
    uapp_log_layout(a, "diskmark");
}

static void update_panes(void) {
    snprintf(g_size_pane, sizeof g_size_pane, "%d MiB", g_run_mib ? g_run_mib : size_mib());
    snprintf(g_call_pane, sizeof g_call_pane, "%u KiB per call", (unsigned)(SYS_WRITE_MAX / 1024));
    if (g_run_when) {
        struct tm tm;
        time_t t = g_run_when;
        localtime_r(&t, &tm);
        udate_format_tm(g_when_pane, sizeof g_when_pane, &tm, UDATE_TIME);
    } else {
        strlcpy(g_when_pane, "-", sizeof g_when_pane);
    }
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    struct ugfx_surface *s = uapp_surface(d);
    layout(s->w, s->h);
    update_panes();
    for (int i = 0; i < PROFILES; i++) {
        g_meter[i].active = (g_state == ST_RUNNING && i == g_running_p);
        uui_meter_draw(s, &g_meter[i]);
    }
    if (!g_show_hist) {
        uui_chart_set_value(&g_chart, g_chart_value[0] ? g_chart_value : NULL);
        uui_chart_draw(s, &g_chart);
    }
    log_layout(a);
}

static int on_tick(struct uapp *a) {
    int redraw = uui_toolbar_tick(&g_tb);
    if (g_pid <= 0) return redraw;
    if (g_state == ST_RUNNING) drain_output();

    // POLLED WHATEVER THE STATE: a failed or stopped child still has to
    // be reaped, or it stays a zombie and its files stay behind.
    int code = 0;
    if (sys_waitpid_nohang(g_pid, &code) == g_pid) {
        g_pid = -1;
        if (g_state == ST_RUNNING) {
            drain_output();   // its last lines can land between the read and its exit
            rescale_bars();
            g_state = ST_DONE;
            if (code) snprintf(g_status, sizeof g_status, "diskbench exited %d", code);
            else {
                set_status("Done.");
                g_chart_value[0] = 0;
                ulog("diskmark: all four passes complete\n");
                history_append();
                if (g_show_hist) history_load();
            }
        } else if (g_state == ST_STOPPING) {
            g_state = ST_IDLE;
            set_status("Stopped.");
            ulog("diskmark: stopped\n");
        }
        g_running_p = -1;
        cleanup();
    }
    uapp_redraw(a);
    return 1;
}

static void on_widget(struct uapp *a, int id, int reason) {
    (void)reason;
    if (id == ID_MENU) {
        int code = uui_menubar_take_code(&g_menu);
        if (code > 0) do_command(a, code);
    } else if (id == ID_TOOLBAR) {
        int code = uui_toolbar_take_code(&g_tb);
        if (code > 0) do_command(a, code);
    }
    uapp_redraw(a);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    int code;
    if (uui_menubar_key(&g_menu, key, &code)) {
        if (code >= 0) do_command(a, code);
        uapp_redraw(a);
        return;
    }
    switch (key) {
    case KEY_F5: do_command(a, CMD_RUN); break;
    case KEY_F9: do_command(a, CMD_HISTORY); break;
    case 0x1B:   do_command(a, CMD_STOP); break;   // Esc stops a run; it closes nothing
    case 0x03:   do_command(a, CMD_COPY); break;   // Ctrl-C
    default: break;
    }
}

static int on_close(struct uapp *a) {
    (void)a;
    // A run cut off by closing the window would otherwise leave its
    // scratch file AND an orphan still writing to the disk.
    if (g_pid > 0) sys_kill(g_pid, 9);
    cleanup();
    return 1;
}

static void default_size(int *w, int *h);

static void on_open(struct uapp *a) {
    // A REMEMBERED SIZE FROM THE OLD LAYOUT is smaller than this one
    // needs; grow to fit, never shrink what the person chose.
    int dw, dh;
    default_size(&dw, &dh);
    if (uapp_width(a) < dw || uapp_height(a) < dh)
        uapp_resize(a, uapp_width(a) > dw ? uapp_width(a) : dw,
                    uapp_height(a) > dh ? uapp_height(a) : dh);
    if (!g_nvols) set_status("no writable disk volume is mounted -- RAM would measure memcpy");
    else set_status("Ready.");
    layout(uapp_width(a), uapp_height(a));
    log_layout(a);
    ulog("diskmark: ready\n");
}

static void load_big_font(void) {
    int px = ugfx_char_h() * 2;
    if (px < 16) px = 16;
    unsigned long need = ugfx_font_arena_size(px);
    void *arena = malloc(need);
    if (!arena) return;
    g_big_ok = ugfx_font_load("/usr/share/fonts/dejavu-sans-mono-bold.ttf",
                              px, 1, &g_big, arena, need);
    if (!g_big_ok) free(arena);
}

// From the font: four cards across and a chart under them.
static void default_size(int *w, int *h) {
    int mw, mh;
    uui_meter_natural_size(&g_meter[0], &mw, &mh);
    mw = card_min_w();
    int p = pad();
    int want = 4 * mw + 5 * p;
    int floor = 76 * ugfx_char_advance('n');
    *w = want > floor ? want : floor;
    *h = uui_menubar_height(&g_menu) + uui_toolbar_height(&g_tb) + 1 + p + mh + p +
         16 * ugfx_char_h() + p + uui_statusbar_height(&g_sb);
}

int main(void) {
    if (!ugfx_font_init()) return 2;   // every metric below comes from it
    setlocale(LC_ALL, "");             // the history's dates and the numbers
    load_big_font();
    scan_volumes();

    uui_menubar_init(&g_menu, menu_bar, (int)(sizeof menu_bar / sizeof menu_bar[0]));
    g_menu.item_flags = item_flags;
    uui_toolbar_init(&g_tb, tb_items, (int)(sizeof tb_items / sizeof tb_items[0]));
    g_tb.item_flags = item_flags;
    g_tb.accent_latch = 1;
    // 16 MiB by default: an emulated disk makes 64 MiB a multi-minute run.
    uui_segmented_init(&g_size, SIZE_ITEMS, SIZE_COUNT, 0);
    uui_dropdown_init(&g_vol, 0, 0, 0, 0, g_vol_items, g_nvols);
    for (int i = 0; i < g_nvols; i++) if (g_vols[i].root) uui_dropdown_set_selected(&g_vol, i);

    for (int i = 0; i < PROFILES; i++) {
        uui_meter_init(&g_meter[i]);
        if (g_big_ok) g_meter[i].value_font = &g_big;
        uint32_t c = utheme_action(is_write(i) ? UTHEME_ACT_ARRANGE : UTHEME_ACT_NAV);
        g_meter[i].value_fg = c;
        g_meter[i].accent = c;
        uui_meter_set(&g_meter[i], CAPTION[i], "--", NULL, NULL);
    }
    uui_chart_init(&g_chart, "THROUGHPUT DURING THE RUN");
    uui_chart_set_scale(&g_chart, 0);
    uui_chart_set_fit(&g_chart, 1);
    g_chart.series_col[0] = utheme_action(UTHEME_ACT_NAV);
    g_chart.series_col[1] = utheme_action(UTHEME_ACT_ARRANGE);

    uui_table_init(&g_hist, 0, 0, 0, 0, HIST_COLS,
                   (int)(sizeof HIST_COLS / sizeof HIST_COLS[0]), hist_cell, NULL);
    history_load();
    uui_table_set_rows(&g_hist, g_nruns);

    uui_statusbar_init(&g_sb);
    g_sb.count = 5;
    g_sb.panes[0].text = g_status;    g_sb.panes[0].chars = 0;
    g_sb.panes[1].text = g_size_pane; g_sb.panes[1].chars = 8;
    g_sb.panes[2].text = g_call_pane; g_sb.panes[2].chars = 14;
    g_sb.panes[3].text = "Q1T1";      g_sb.panes[3].chars = 5;
    g_sb.panes[4].text = g_when_pane; g_sb.panes[4].chars = 6;

    struct uapp_desc desc = {
        .title        = "Disk Mark",
        .app_id       = "diskmark",
        .x            = 200,
        .y            = 90,
        .on_size      = default_size,
        .flags        = UAPP_SINGLE_INSTANCE | UAPP_RESIZABLE,
        .min_w        = 360,
        .min_h        = 300,
        .widgets      = g_widgets,
        .widget_count = (int)(sizeof g_widgets / sizeof g_widgets[0]),
        // POLLING A CHILD, NOT ANIMATING: 500 ms is the rate ui/uapp.h
        // names for a process list, and diskbench reports every quarter
        // second, so each tick has a fresh sample for the chart.
        .tick_ms      = 500,
        .on_tick      = on_tick,
        .on_open      = on_open,
        .on_draw      = on_draw,
        .on_widget    = on_widget,
        .on_key       = on_key,
        .on_close     = on_close,
    };
    return uapp_run(&desc);
}
