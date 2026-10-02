// Disk Mark -- sequential and random throughput, in the shape
// CrystalDiskMark made familiar.
//
// **IT DOES NO I/O ITSELF.** It spawns /bin/diskbench and polls what
// that prints. The first version ran the passes inside on_tick, sliced
// across frames, and it was wrong in a way worth recording: the slice
// was bounded in time but the UNIT was not, and one "1 MiB transfer" is
// 1024 syscalls (SYS_WRITE_MAX was 1 KiB then and is far larger now --
// abi/syscall_abi.h), which on a 256 MiB file runs
// for many seconds. The compositor pings every client on a cadence, so
// the window sat there reading "Disk Mark (Not Responding)" for the
// length of a pass. Bounding a loop is useless when one turn of it is
// unbounded.
//
// A child process is the fix and it is also the shape everything else
// uses: CrystalDiskMark runs a worker while its window stays live, and
// the File Manager here spawns /bin/cp rather than reimplementing
// copying. The GUI cannot stall now however slow a device is, and the
// measurement stops being distorted by the tick cadence.
//
// THE LABELS SAY WHAT THIS OS ACTUALLY DOES. Plain `SEQ`, not CDM's
// `SEQ1M`, because the request size is whatever SYS_WRITE_MAX is and a
// number baked into the heading would be exactly the decorative label
// the Q1T1 footer exists to avoid -- it was 1 KiB, it is 64 KiB now,
// and the heading did not have to change. See userland/bin/diskbench.c.
#include <stdint.h>
#include "rt/sys.h"
#include "syscall_abi.h"   // SYS_WRITE_MAX -- named in the footer
#include "ui/ulog.h"
#include "ui/ugfx.h"
#include "ui/uui.h"
#include "ui/uui_label.h"
#include "ui/uui_layout.h"
#include "ui/uapp.h"
#include "ui/utheme.h"
#include "ui/uui_meter.h"
#include "lib/human.h"
#include <stdio.h>
#include "lib/unum.h"
#include <string.h>
#include <stdlib.h>
#include "tmppath.h"

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
static const char *work_path(void) {
    static char p[64];
    if (!p[0]) tmppath(p, sizeof p, TMP_PERSISTENT, "diskmark.tmp");
    return p;
}
#define WORK_PATH work_path()
#define BENCH_PATH  "/bin/diskbench"

// Display order is CrystalDiskMark's read-then-write; diskbench runs
// them write-first for its own reasons and names each result, so the
// two orders never have to agree.
#define P_SEQ_READ  0
#define P_SEQ_WRITE 1
#define P_RND_READ  2
#define P_RND_WRITE 3
#define PROFILES    4

static const char *PROFILE_NAME[PROFILES] = {
    "SEQ Q1T1 READ", "SEQ Q1T1 WRITE",
    "RND4K Q1T1 READ", "RND4K Q1T1 WRITE",
};

// What diskbench calls them on the wire.
static const char *WIRE_NAME[PROFILES] = {
    "SEQ-read", "SEQ-write", "RND4K-read", "RND4K-write",
};

static const char *const SIZE_ITEMS[] = { "16 MiB", "64 MiB", "256 MiB" };
static const int SIZE_MIB[] = { 16, 64, 256 };
#define SIZE_COUNT ((int)(sizeof SIZE_MIB / sizeof SIZE_MIB[0]))

#define ST_IDLE    0
#define ST_RUNNING 1
#define ST_DONE    2
#define ST_FAILED  3

static int g_state = ST_IDLE;
static int g_pid = -1;
static uint64_t g_result[PROFILES];   // milli-MB/s, for the bar rescale

static char g_value[PROFILES][24];
static char g_unit[PROFILES][12];
static char g_detail[PROFILES][48];
static char g_status[96];
static char g_footer[128];

// The child's output, re-read whole each tick. Static because the
// ring-3 stack has a 2048-byte frame budget.
static char g_out[4096];

// THE BIG NUMBER'S FONT, loaded once. A reading shown at body size is a
// label; the size IS the design here. A failed load is not fatal --
// value_font stays NULL and the meter falls back to the session bold.
static struct ugfx_font g_big;
static int g_big_ok;

static struct uui_meter g_meter[PROFILES];
static struct uui_dropdown g_size;
static struct uui_button g_run;
static struct uui_label g_status_label, g_footer_label;

static struct uui_item g_grid_items[PROFILES];
static struct uui_layout g_grid;
static struct uui_item g_bar_items[2];
static struct uui_layout g_bar;
static struct uui_item g_root_items[4];
static struct uui_layout g_root;

#define ID_RUN 1

static int bytes_mib(void) {
    int idx = uui_dropdown_selected(&g_size);
    if (idx < 0 || idx >= SIZE_COUNT) idx = 0;
    return SIZE_MIB[idx];
}

// Refuses a RAM-only root the way `stress` does: benchmarking a
// filesystem in the kernel heap measures memcpy, and reporting that as
// disk throughput is the most misleading thing this app could do.
static int root_ok(char *why, int cap) {
    struct query_fsinfo fs;
    for (int i = 0; i < 8; i++) {
        if (sys_query_record(QUERY_FSINFO, (unsigned)i, &fs, sizeof fs) < (int)sizeof fs) break;
        if (!(fs.flags & QUERY_FS_ROOT)) continue;
        if (!(fs.flags & QUERY_FS_MOUNTED)) { snprintf(why, cap, "no filesystem is mounted"); return 0; }
        if (!(fs.flags & QUERY_FS_PERSISTENT)) {
            snprintf(why, cap, "the root is RAM-only -- this would measure memcpy");
            return 0;
        }
        if (fs.flags & QUERY_FS_RDONLY) { snprintf(why, cap, "the root is mounted read-only"); return 0; }
        // BOTH LIMITS NAMED. Q1T1 and the 1 KiB syscall cap are the two
        // things CrystalDiskMark reports that this OS cannot deliver,
        // and stating them beats letting a heading imply otherwise.
        snprintf(g_footer, sizeof g_footer,
                 "%s on %s  -  %u KiB per syscall (SYS_WRITE_MAX), Q1T1: no async block I/O",
                 fs.name, fs.device[0] ? fs.device : "(no volume)",
                 (unsigned)(SYS_WRITE_MAX / 1024));
        return 1;
    }
    snprintf(why, cap, "could not read the root mount");
    return 0;
}

// Fills each bar relative to the FASTEST of this run's own results. A
// fixed ceiling picked for a real SSD leaves every bar here empty, and
// one picked for an emulator is a lie on hardware.
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

static void show_result(int p, uint64_t milli, uint64_t iops, uint64_t us) {
    // THE DRAIN IS IDEMPOTENT, SO THIS MUST BE. Re-applying a line
    // already seen costs a few compares; re-LOGGING it floods dmesg with
    // the same result every tick, which is what `desktop.layout_log`
    // was gated for.
    if (g_result[p] == milli) return;
    g_result[p] = milli;
    snprintf(g_value[p], sizeof g_value[p], "%llu.%llu",
             (unsigned long long)(milli / 1000), (unsigned long long)((milli % 1000) / 100));
    unum_localize(g_value[p], sizeof g_value[p], 0);
    snprintf(g_unit[p], sizeof g_unit[p], "MB/s");
    // IOPS and latency on the random profiles only: MB/s at 4 KiB is a
    // hard figure to feel, and at 1 KiB sequential the op count is just
    // the byte count again.
    if (p == P_RND_READ || p == P_RND_WRITE) {
        snprintf(g_detail[p], sizeof g_detail[p], "%llu IOPS, %llu.%llu ms avg",
                 (unsigned long long)iops,
                 (unsigned long long)(us / 1000), (unsigned long long)((us % 1000) / 100));
        // The IOPS count is the first number, so the latency is
        // localised on its own: unum_localize() takes the first only.
        char *ms = strchr(g_detail[p], ',');
        if (ms) unum_localize(ms + 1, sizeof g_detail[p] - (unsigned long)(ms + 1 - g_detail[p]), 0);
    } else {
        g_detail[p][0] = 0;
    }
    uui_meter_set(&g_meter[p], PROFILE_NAME[p], g_value[p], g_unit[p],
                  g_detail[p][0] ? g_detail[p] : NULL);
    ulogf("diskmark: result %s = %s %s %s\n", PROFILE_NAME[p], g_value[p], g_unit[p],
          g_detail[p][0] ? g_detail[p] : "-");
}

// Re-reads the child's output whole and applies every line. Idempotent
// on purpose: the file only grows, so re-applying what was already seen
// costs a few string compares and removes any need to track a read
// offset across ticks.
static void drain_output(void) {
    int fd = sys_open(RESULT_PATH, 0);
    if (fd < 0) return;
    int64_t n = sys_read(fd, g_out, sizeof g_out - 1);
    sys_close(fd);
    if (n <= 0) return;
    g_out[n] = 0;

    char *line = g_out;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;

        const char *p = strstr(line, "diskbench: ");
        if (p) {
            p += 11;
            if (!strncmp(p, "result ", 7)) {
                p += 7;
                const char *sp = strchr(p, ' ');
                if (sp) {
                    int idx = wire_index(p, (int)(sp - p));
                    unsigned long long milli = 0, iops = 0, us = 0;
                    if (idx >= 0 && sscanf(sp + 1, "%llu %llu %llu", &milli, &iops, &us) == 3)
                        show_result(idx, milli, iops, us);
                }
            } else if (!strncmp(p, "progress ", 9)) {
                p += 9;
                const char *sp = strchr(p, ' ');
                if (sp) {
                    int idx = wire_index(p, (int)(sp - p));
                    int pct = (int)strtol(sp + 1, NULL, 10);
                    if (idx >= 0) {
                        snprintf(g_status, sizeof g_status, "%s: %d%%", PROFILE_NAME[idx], pct);
                        if (!g_result[idx]) uui_meter_set_fill(&g_meter[idx], pct * 10);
                    }
                }
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
    sys_unlink(WORK_PATH);
}

static int on_tick(struct uapp *a) {
    if (g_state != ST_RUNNING) return 1;

    drain_output();

    int code = 0;
    int r = sys_waitpid_nohang(g_pid, &code);
    if (r == g_pid) {
        // Reaped. Drain ONCE MORE: the child's last lines can land
        // between the read above and its exit, and a result dropped
        // that way is a tile that stays blank for no visible reason.
        drain_output();
        g_pid = -1;
        if (g_state != ST_FAILED) {
            rescale_bars();
            g_state = ST_DONE;
            if (code) snprintf(g_status, sizeof g_status, "diskbench exited %d", code);
            else      snprintf(g_status, sizeof g_status, "Done.");
            ulog("diskmark: all four passes complete\n");
        }
        cleanup();
    }
    uapp_redraw(a);
    return 1;
}

static void begin_run(struct uapp *a) {
    if (g_state == ST_RUNNING) return;

    char why[80];
    if (!root_ok(why, sizeof why)) {
        snprintf(g_status, sizeof g_status, "%s", why);
        g_state = ST_FAILED;
        uapp_redraw(a);
        return;
    }

    for (int i = 0; i < PROFILES; i++) {
        g_value[i][0] = g_unit[i][0] = g_detail[i][0] = 0;
        g_result[i] = 0;
        uui_meter_set(&g_meter[i], PROFILE_NAME[i], "--", NULL, NULL);
        uui_meter_set_fill(&g_meter[i], 0);
    }
    cleanup();

    // THE CHILD WRITES THE REPORT ITSELF (`--out`), and spawns with no
    // stdout at all. SYS_SPAWN's stdout_fd must be a PIPE write end, and
    // a pipe would re-couple the two: PIPE_MAX is 8 KiB kernel wide, so
    // a window that was slow to drain would block the benchmark it is
    // timing. With a file neither side waits for the other, and this can
    // re-read it whenever it likes.
    static char args[128];
    snprintf(args, sizeof args, "--size %d --path %s --out %s",
             bytes_mib(), WORK_PATH, RESULT_PATH);
    g_pid = sys_spawn(BENCH_PATH, args, -1);

    if (g_pid < 0) {
        snprintf(g_status, sizeof g_status, "could not start " BENCH_PATH);
        g_state = ST_FAILED;
        cleanup();
    } else {
        g_state = ST_RUNNING;
        snprintf(g_status, sizeof g_status, "Running (%d MiB)...", bytes_mib());
        ulogf("diskmark: spawned %s as pid %d\n", BENCH_PATH, g_pid);
    }
    uapp_redraw(a);
}

// A completed click only (docs/gui-guidelines.md); the library enforces it.
static void on_action(struct uapp *a, int code) {
    if (code == ID_RUN) begin_run(a);
}

static int on_close(struct uapp *a) {
    (void)a;
    // A run interrupted by closing the window would otherwise leave its
    // temp files behind AND an orphan child still writing to the disk.
    if (g_pid > 0) sys_kill(g_pid, 9);
    cleanup();
    return 1;
}

static void on_open(struct uapp *a) {
    char why[80];
    if (!root_ok(why, sizeof why)) snprintf(g_status, sizeof g_status, "%s", why);
    else                           snprintf(g_status, sizeof g_status, "Ready.");

    // GEOMETRY AS THE CLIENT SEES IT, so a test clicks what the layout
    // produced rather than re-deriving it in Python.
    // The named widgets report themselves (`run`, `size`); the tiles are
    // indexed by profile, which a bounds line has no slot for, so they
    // stay the app's -- through the gate, never ulogf().
    uapp_log_layout(a, "diskmark");
    for (int i = 0; i < PROFILES; i++)
        uapp_logf_layout("diskmark: layout tile %d %d %d %d %d\n", i,
                         g_meter[i].x, g_meter[i].y, g_meter[i].w, g_meter[i].h);
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

int main(void) {
    if (!ugfx_font_init()) return 2;   // every metric below comes from it

    load_big_font();

    // The FOOTER's real text before the layout measures anything: a
    // window sized against a placeholder is a window sized wrong, and
    // the placeholder is what shipped the first time -- the footer came
    // out clipped because natural_size saw one space.
    char why[80];
    if (!root_ok(why, sizeof why)) snprintf(g_footer, sizeof g_footer, "%s", why);

    for (int i = 0; i < PROFILES; i++) {
        uui_meter_init(&g_meter[i]);
        if (g_big_ok) g_meter[i].value_font = &g_big;
        uui_meter_set(&g_meter[i], PROFILE_NAME[i], "--", NULL, NULL);
        g_grid_items[i].ops = &uui_meter_ops;
        g_grid_items[i].widget = &g_meter[i];
        g_grid_items[i].flags = UUI_FILL_W;
    }

    uui_dropdown_init(&g_size, 0, 0, 0, 0, SIZE_ITEMS, SIZE_COUNT);
    g_size.list.selected = 0;   // 16 MiB -- the popup IS the listbox.
                                // Small by default because an emulated disk
                                // makes 64 MiB a multi-minute run.
    uui_button_init(&g_run, 0, 0, 0, 0, "Run", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_RUN);

    // key-routing-ok: the popup is what takes keys here, and an OPEN
    // popup gets them from uui_router_overlay_key() before either door
    // in uapp.c -- so the dropdown is fully usable by keyboard once it
    // is open. What a closed one loses is the letter seek; the arrows
    // are deliberately inert there anyway (ui/uui_dropdown.h). Listed
    // under the roadmap's papercuts.
    g_bar_items[0].ops = &uui_dropdown_ops;
    g_bar_items[0].widget = &g_size;
    g_bar_items[0].name = "size";
    g_bar_items[1].ops = &uui_button_ops;
    g_bar_items[1].widget = &g_run;
    g_bar_items[1].id = ID_RUN;
    g_bar_items[1].name = "run";
    g_bar.dir = UUI_ROW;
    g_bar.margin = 0;
    g_bar.items = g_bar_items;
    g_bar.count = 2;

    g_grid.dir = UUI_GRID;
    g_grid.cols = 2;
    g_grid.margin = 0;
    g_grid.items = g_grid_items;
    g_grid.count = PROFILES;

    uui_label_init(&g_status_label, g_status);
    uui_label_init(&g_footer_label, g_footer);
    // WRAPPED, and two rows reserved: the footer is a sentence, and a
    // sentence's width has no business deciding how wide this window
    // is. A wrapping label asks for almost no width and takes what the
    // column gives it (ui/uui_label.h).
    uui_label_set_wrap(&g_footer_label, 2);
    snprintf(g_status, sizeof g_status, "Ready.");

    g_root_items[0].ops = &uui_layout_ops;
    g_root_items[0].widget = &g_bar;
    g_root_items[1].ops = &uui_layout_ops;
    g_root_items[1].widget = &g_grid;
    g_root_items[1].flags = UUI_FILL_W | UUI_FILL_H;
    g_root_items[2].ops = &uui_label_ops;
    g_root_items[2].widget = &g_status_label;
    g_root_items[2].flags = UUI_FILL_W;
    g_root_items[3].ops = &uui_label_ops;
    g_root_items[3].widget = &g_footer_label;
    g_root_items[3].flags = UUI_FILL_W;
    g_root.dir = UUI_COLUMN;
    g_root.items = g_root_items;
    g_root.count = 4;

    struct uapp_desc desc = {
        .title     = "Disk Mark",
        .app_id    = "diskmark",
        .x         = 260,
        .y         = 110,
        .layout    = &g_root,
        // THE SAME ARRAY, TWICE, AND BOTH ARE LOAD-BEARING: `layout`
        // sizes and draws, `widgets` is what gets mouse input. Declaring
        // only the first is a window that renders perfectly and cannot
        // be clicked, with nothing to say so (ui/uapp.h).
        .widgets   = g_root_items,
        .widget_count = 4,
        // A cadence, not a spin: on_tick does ONE slice and returns, so
        // the client keeps answering the compositor's pings through a
        // pass that takes tens of seconds. See this file's header.
        // POLLING A CHILD, NOT ANIMATING. Every tick re-reads the report
        // file, which is real disk I/O -- at 30 ms that was competing
        // with the benchmark it is watching and roughly halving the
        // numbers. 500 ms is the rate ui/uapp.h names for a process
        // list, and progress moves in 5% steps anyway.
        .tick_ms   = 500,
        .on_tick   = on_tick,
        .on_open   = on_open,
        .on_action = on_action,
        .on_close  = on_close,
        // Two copies would fight over one temp file and produce two
        // sets of numbers from one disk.
        .flags     = UAPP_SINGLE_INSTANCE,
    };
    return uapp_run(&desc);
}
