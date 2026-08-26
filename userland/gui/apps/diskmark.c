// Disk Mark -- sequential and random throughput, in the shape
// CrystalDiskMark made familiar.
//
// WHAT IT MEASURES, AND WHAT IT CANNOT
// ------------------------------------
// A ring-3 app has no raw block access -- there is no /dev here -- so
// every pass goes through the ordinary file syscalls. That is not a
// compromise: CrystalDiskMark measures a mounted volume too, and so
// does GNOME Disks' benchmark tab. What it does mean is that a WRITE
// number is the FILESYSTEM's, journal included, not the driver's.
//
// **EVERYTHING HERE IS Q1T1, and the label is not decoration.** CDM
// reports SEQ1M Q8T1 and RND4K Q32T1; both of those need several
// requests in flight, which needs an asynchronous block interface this
// OS does not have (docs/decisions/drivers.md, on why AHCI reports NCQ
// and does not use it). One request at a time is the honest ceiling, so
// the tiles say so rather than printing a queue depth that is a wish.
//
// WHY THE PASSES ARE CHUNKED ACROSS TICKS
// ---------------------------------------
// THE ONE THING TO KNOW BEFORE EDITING THIS. A 64 MiB pass takes tens
// of seconds, and the compositor PINGS every client on a cadence
// (WM_PING_INTERVAL_DEFAULT): a client that blocks through its whole
// benchmark stops answering, earns the busy pointer and then
// "(Not Responding)", and cannot draw progress either. So on_tick does
// one bounded SLICE of I/O and returns, and the pass is a state machine
// across frames. uapp_busy_begin() is the wrong tool here -- it is for
// work that is slow and SHORT.
//
// The timer is sys_monotonic_ns(), which advances in 10 ms steps under
// the PIT clocksource (finer only when the TSC one is picked). A single
// 4 KiB operation is far below that, so RND4K times a whole slice of
// thousands and divides -- timing one operation would read as either
// zero or infinity.
#include <stdint.h>
#include "rt/sys.h"
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
#include <string.h>
#include <stdlib.h>

#define TMP_PATH "/tmp/diskmark.tmp"

// The four profiles, in CDM's own order. SEQ moves a megabyte at a
// time; RND moves 4 KiB at a pseudo-random offset.
#define P_SEQ_READ  0
#define P_SEQ_WRITE 1
#define P_RND_READ  2
#define P_RND_WRITE 3
#define PROFILES    4

static const char *PROFILE_NAME[PROFILES] = {
    "SEQ1M Q1T1 READ", "SEQ1M Q1T1 WRITE",
    "RND4K Q1T1 READ", "RND4K Q1T1 WRITE",
};

// THE ORDER THEY RUN IN IS NOT THE ORDER THEY ARE SHOWN IN. The tiles
// keep CrystalDiskMark's read-then-write layout because that is what a
// reader expects; the passes run WRITE FIRST because the write is what
// lays the file down, and a separate "prepare" pass would move the same
// bytes again for no result -- a third of the run's I/O, and minutes of
// it on an emulated disk.
static const int RUN_ORDER[PROFILES] = { P_SEQ_WRITE, P_SEQ_READ, P_RND_WRITE, P_RND_READ };

// The SEQ transfer unit. A megabyte is the "1M" in SEQ1M and it is what
// is measured; the slice budget above governs how many of them go in one
// tick, not how big one is.
#define SEQ_BLOCK   (1024 * 1024)
#define RND_BLOCK   4096

// ONE TICK'S WORK IS BOUNDED IN TIME, NOT IN BYTES, and that is the
// difference between a working app and one the compositor declares
// dead. The WM pings every client every WM_PING_INTERVAL_DEFAULT ms and
// gives it WM_PING_TIMEOUT_DEFAULT to answer (200 and 300 today,
// userland/wm/wm_internal.h); a slice fixed at 4 MiB takes over a
// second on an emulated disk, so the title bar said
// "(Not Responding)" through every pass -- found by a test whose
// CONTROL was that the title bar must not change.
//
// A time budget also self-tunes: a fast device does more per tick and a
// slow one less, with no constant to revisit per backend. 120 ms leaves
// well over half the ping window, and is far enough above the PIT
// clocksource's 10 ms step to be measurable.
#define SLICE_BUDGET_NS 120000000ull

// The floor: at least one operation per tick, however slow the device.
// Without it a device slower than the budget would make no progress at
// all and the pass would never end.
#define SLICE_MIN_OPS 1

// The sizes offered. 64 MiB by default: large enough to swamp any
// caching, small enough that a full run is not a coffee break.
static const char *const SIZE_ITEMS[] = { "16 MiB", "64 MiB", "256 MiB" };
static const int SIZE_MIB[] = { 16, 64, 256 };
#define SIZE_COUNT ((int)(sizeof SIZE_MIB / sizeof SIZE_MIB[0]))

// One megabyte of it, reused. Static because the ring-3 stack has a
// 2048-byte frame budget (USERLAND_CFLAGS' -Wframe-larger-than) and a
// megabyte on it would step clean over the guard page.
static uint8_t g_buf[SEQ_BLOCK];

// --- state -----------------------------------------------------------

#define ST_IDLE    0
#define ST_RUNNING 2
#define ST_DONE    3
#define ST_FAILED  4

static int g_state = ST_IDLE;
static int g_profile;              // which pass is running (a P_* index)
static int g_step;                 // where in RUN_ORDER that is
static int g_fd = -1;
static uint64_t g_total;           // bytes this pass must move
static uint64_t g_moved;           // bytes moved so far
static uint64_t g_ops;             // operations completed (random passes)
static uint64_t g_elapsed_ns;      // summed across slices, so redraw time is excluded
static uint64_t g_result[PROFILES];// milli-MB/s, kept for the bar rescale below
static uint32_t g_rand = 0x9E3779B9u;

static char g_value[PROFILES][24];
static char g_unit[PROFILES][12];
static char g_detail[PROFILES][48];
static char g_status[96];
static char g_footer[96];

// THE BIG NUMBER'S FONT, loaded once. A reading shown at body size is
// a label; the size IS the design here, the same way it is in
// CrystalDiskMark and Blackmagic's disk test. Loaded at roughly twice
// the session pitch so it reflows with `fontsize` rather than pinning a
// pixel height (docs/gui-guidelines.md: layout is font-derived).
//
// A FAILED LOAD IS NOT FATAL: value_font stays NULL and every meter
// falls back to the session bold weight, which is what uui_meter
// documents. An app that refused to start over a decorative font would
// be the wrong trade.
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

// THE ROUTED PATH REPORTS THE ITEM'S id THROUGH on_widget, and
// on_action belongs to uui_button_group -- uapp.c fires it only from
// `desc.buttons`. A lone button in a `widgets` array therefore needs an
// id here and nothing in the button's own `code`, which is the opposite
// of what Calculator (a group) does and cost a build to find out.
#define ID_RUN 1

// A cheap xorshift. NOT krandom: the point is a repeatable spread of
// offsets, not entropy, and a benchmark whose access pattern changed
// between runs would not be comparable with itself.
static uint32_t next_rand(void) {
    g_rand ^= g_rand << 13;
    g_rand ^= g_rand >> 17;
    g_rand ^= g_rand << 5;
    return g_rand;
}

static uint64_t bytes_for_run(void) {
    int idx = uui_dropdown_selected(&g_size);
    if (idx < 0 || idx >= SIZE_COUNT) idx = 1;
    return (uint64_t)SIZE_MIB[idx] * 1024u * 1024u;
}

// --- the root filesystem, as a fact ----------------------------------

// Refuses a RAM-only root, the same way `stress` does: benchmarking a
// filesystem in the kernel heap measures memcpy, and reporting that as
// disk throughput is the most misleading thing this app could do.
static int root_ok(char *why, int cap) {
    struct query_fsinfo fs;
    for (int i = 0; i < 8; i++) {
        if (sys_query_record(QUERY_FSINFO, (unsigned)i, &fs, sizeof fs) < (int)sizeof fs) break;
        if (!(fs.flags & QUERY_FS_ROOT)) continue;
        if (!(fs.flags & QUERY_FS_MOUNTED)) {
            snprintf(why, cap, "no filesystem is mounted");
            return 0;
        }
        if (!(fs.flags & QUERY_FS_PERSISTENT)) {
            snprintf(why, cap, "the root is RAM-only -- this would measure memcpy");
            return 0;
        }
        if (fs.flags & QUERY_FS_RDONLY) {
            snprintf(why, cap, "the root is mounted read-only");
            return 0;
        }
        snprintf(g_footer, sizeof g_footer,
                 "%s on %s  -  Q1T1: this OS has no asynchronous block I/O",
                 fs.name, fs.device[0] ? fs.device : "(no volume)");
        return 1;
    }
    snprintf(why, cap, "could not read the root mount");
    return 0;
}

// --- reporting -------------------------------------------------------

// Bytes per second from a byte count and a nanosecond span, without
// overflowing and without floating point (there is none in this
// project's shared code). Returns thousandths of a MB/s, so the caller
// can print one decimal.
static uint64_t mbps_milli(uint64_t bytes, uint64_t ns) {
    if (!ns) return 0;
    // ORDER MATTERS TWICE HERE. Scale to milli-MiB FIRST, so the
    // multiply by a billion cannot overflow 64 bits at the largest size
    // offered; and divide by `ns` LAST, so a sub-second pass does not
    // truncate to zero on the way -- which is what the first version
    // did, printing 0.0 for every result while the IOPS beside it were
    // right.
    return (bytes * 1000ull / 1048576ull) * 1000000000ull / ns;
}

static void record(int profile, uint64_t bytes, uint64_t ns, uint64_t ops) {
    uint64_t milli = mbps_milli(bytes, ns);
    snprintf(g_value[profile], sizeof g_value[profile], "%llu.%llu",
             (unsigned long long)(milli / 1000), (unsigned long long)((milli % 1000) / 100));
    snprintf(g_unit[profile], sizeof g_unit[profile], "MB/s");

    if (ops) {
        // IOPS and the average latency, which is what a random number is
        // actually about -- MB/s at 4 KiB is a hard figure to feel.
        uint64_t iops = ns ? (ops * 1000000000ull) / ns : 0;
        uint64_t us = ops ? (ns / ops) / 1000ull : 0;
        snprintf(g_detail[profile], sizeof g_detail[profile],
                 "%llu IOPS, %llu.%llu ms avg",
                 (unsigned long long)iops,
                 (unsigned long long)(us / 1000), (unsigned long long)((us % 1000) / 100));
    } else {
        g_detail[profile][0] = 0;
    }

    uui_meter_set(&g_meter[profile], PROFILE_NAME[profile],
                  g_value[profile], g_unit[profile],
                  g_detail[profile][0] ? g_detail[profile] : NULL);

    // REPORTED, so a test can assert on the NUMBER rather than on the
    // run having finished. "All four passes complete" was true for a
    // build whose every result read 0.0 -- the throughput arithmetic
    // truncated to zero while the IOPS beside it were right.
    ulogf("diskmark: result %s = %s %s %s\n", PROFILE_NAME[profile],
          g_value[profile], g_unit[profile],
          g_detail[profile][0] ? g_detail[profile] : "-");
    // The bar is deliberately NOT drawn against an absolute scale. A
    // fixed ceiling picked for a real SSD leaves every bar here empty,
    // and one picked for an emulator is a lie on real hardware. So it
    // is filled in relative to the FASTEST of this run's own four
    // results, once they all exist -- see rescale_bars(). Until then it
    // shows progress.
    g_result[profile] = milli;
}

// Fills each bar relative to the FASTEST result of this run. The bar
// therefore answers "which of these four is this machine good at",
// which is a question the four numbers alone make you do arithmetic
// for, and it needs no absolute scale to be honest about.
static void rescale_bars(void) {
    uint64_t best = 0;
    for (int i = 0; i < PROFILES; i++) if (g_result[i] > best) best = g_result[i];
    for (int i = 0; i < PROFILES; i++) {
        uui_meter_set_fill(&g_meter[i], best ? (int)((g_result[i] * 1000ull) / best) : 0);
    }
}

// --- the passes ------------------------------------------------------

// A SHORT TRANSFER IS NOT AN ERROR, and treating one as an error is
// what made the first run die 70 s in. sys_write() completes a whole
// buffer (the kernel loops internally, see docs/conventions/build.md),
// but sys_read() may legitimately return less than asked for -- Unix's
// rule everywhere -- so a read has to loop until the request is
// satisfied or the file genuinely ends. Returns the bytes moved, or -1
// on a real error.
static int64_t io_full(int fd, void *buf, uint32_t len, int writing) {
    uint32_t done = 0;
    while (done < len) {
        int64_t n = writing ? sys_write(fd, (uint8_t *)buf + done, len - done)
                            : sys_read(fd, (uint8_t *)buf + done, len - done);
        if (n < 0) return -1;
        if (n == 0) break;        // end of file, which is not a failure
        done += (uint32_t)n;
    }
    return (int64_t)done;
}

static void close_fd(void) {
    if (g_fd >= 0) { sys_close(g_fd); g_fd = -1; }
}

static void fail(struct uapp *a, const char *why) {
    close_fd();
    sys_unlink(TMP_PATH);
    g_state = ST_FAILED;
    snprintf(g_status, sizeof g_status, "%s", why);
    ulogf("diskmark: %s\n", why);
    uapp_redraw(a);
}

static int open_pass(int profile) {
    close_fd();
    int writing = (profile == P_SEQ_WRITE || profile == P_RND_WRITE || profile < 0);
    int flags = writing ? (SYS_O_WRITE | SYS_O_CREAT) : 0;
    // Only the PREPARE pass truncates. A random-write pass truncating
    // would leave it writing into a hole rather than over real blocks,
    // which is a different measurement wearing the same label.
    if (profile < 0) flags |= SYS_O_TRUNC;
    g_fd = sys_open(TMP_PATH, flags);
    return g_fd >= 0;
}

static int seq_slice(int writing) {
    uint64_t start = sys_monotonic_ns();
    int did = 0;
    while (g_moved < g_total) {
        uint64_t left = g_total - g_moved;
        uint32_t chunk = left < SEQ_BLOCK ? (uint32_t)left : SEQ_BLOCK;
        if (io_full(g_fd, g_buf, chunk, writing) != (int64_t)chunk) return 0;
        g_moved += chunk;
        g_ops++;
        did++;
        if (did >= SLICE_MIN_OPS && sys_monotonic_ns() - start >= SLICE_BUDGET_NS) break;
    }
    g_elapsed_ns += sys_monotonic_ns() - start;
    return 1;
}

static int rnd_slice(int writing) {
    // Offsets are 4 KiB-aligned within the file, which is what "random
    // 4K" means everywhere else -- an unaligned one would measure
    // read-modify-write instead.
    uint64_t blocks = g_total / RND_BLOCK;
    if (!blocks) return 0;

    uint64_t start = sys_monotonic_ns();
    int did = 0;
    while (g_moved < g_total) {
        uint64_t off = (uint64_t)(next_rand() % (uint32_t)blocks) * RND_BLOCK;
        if (sys_lseek(g_fd, (long long)off, SYS_SEEK_SET) < 0) return 0;
        if (io_full(g_fd, g_buf, RND_BLOCK, writing) != RND_BLOCK) return 0;
        g_moved += RND_BLOCK;
        g_ops++;
        did++;
        if (did >= SLICE_MIN_OPS && sys_monotonic_ns() - start >= SLICE_BUDGET_NS) break;
    }
    g_elapsed_ns += sys_monotonic_ns() - start;
    return 1;
}

static int start_profile(int step) {
    int profile = RUN_ORDER[step];
    g_step = step;
    g_profile = profile;
    g_moved = 0;
    g_ops = 0;
    g_elapsed_ns = 0;
    g_rand = 0x9E3779B9u;   // same offsets every run, so runs compare
    // The FIRST pass creates and truncates; the rest reuse the file it
    // laid down. Truncating on a later write pass would leave it writing
    // into a hole rather than over real blocks -- a different
    // measurement wearing the same label.
    if (!open_pass(step == 0 ? -1 : profile)) return 0;
    // A random pass covers the file once in total, not once per block.
    if (profile == P_RND_READ || profile == P_RND_WRITE) g_total = bytes_for_run() / 8;
    else                                                 g_total = bytes_for_run();
    snprintf(g_status, sizeof g_status, "%s: 0%%  (%d of %d)",
             PROFILE_NAME[profile], step + 1, PROFILES);
    return 1;
}

static int on_tick(struct uapp *a) {
    if (g_state != ST_RUNNING) return 1;

    int ok;
    switch (g_profile) {
        case P_SEQ_READ:  ok = seq_slice(0); break;
        case P_SEQ_WRITE: ok = seq_slice(1); break;
        case P_RND_READ:  ok = rnd_slice(0); break;
        default:          ok = rnd_slice(1); break;
    }
    if (!ok) { fail(a, "a transfer failed part-way through"); return 1; }

    // Progress on the STATUS line and on the running tile's bar, so a
    // pass that takes minutes does not read as a hang.
    int pct = g_total ? (int)((g_moved * 100u) / g_total) : 0;
    snprintf(g_status, sizeof g_status, "%s: %d%%  (%d of %d)",
             PROFILE_NAME[g_profile], pct, g_step + 1, PROFILES);
    uui_meter_set_fill(&g_meter[g_profile], pct * 10);

    if (g_moved >= g_total) {
        record(g_profile, g_moved, g_elapsed_ns,
               (g_profile == P_RND_READ || g_profile == P_RND_WRITE) ? g_ops : 0);
        if (g_step + 1 < PROFILES) {
            if (!start_profile(g_step + 1)) { fail(a, "could not reopen the test file"); return 1; }
        } else {
            close_fd();
            sys_unlink(TMP_PATH);   // self-cleaning; see on_close for the other exit
            rescale_bars();
            g_state = ST_DONE;
            snprintf(g_status, sizeof g_status, "Done.");
            ulog("diskmark: all four passes complete\n");
        }
    }
    uapp_redraw(a);
    return 1;
}

// --- the UI ----------------------------------------------------------

static void begin_run(struct uapp *a) {
    if (g_state == ST_RUNNING) return;

    char why[80];
    if (!root_ok(why, sizeof why)) { fail(a, why); return; }

    for (int i = 0; i < PROFILES; i++) {
        g_value[i][0] = g_unit[i][0] = g_detail[i][0] = 0;
        g_result[i] = 0;
        uui_meter_set(&g_meter[i], PROFILE_NAME[i], "--", NULL, NULL);
        uui_meter_set_fill(&g_meter[i], 0);
    }

    g_state = ST_RUNNING;
    if (!start_profile(0)) { fail(a, "could not create " TMP_PATH); return; }
    uapp_redraw(a);
}

static void on_widget(struct uapp *a, int id, int reason) {
    // COMMIT ON RELEASE, not on press: a control that acts on button-down
    // can never be cancelled by dragging off it (docs/gui-guidelines.md).
    if (id == ID_RUN && reason == UUI_REASON_RELEASE) begin_run(a);
}

static int on_close(struct uapp *a) {
    (void)a;
    // The other way out. A run interrupted by closing the window would
    // otherwise leave its temp file behind, which is exactly the litter
    // the size picker exists to bound.
    close_fd();
    sys_unlink(TMP_PATH);
    return 1;
}

static void on_open(struct uapp *a) {
    char why[80];
    if (!root_ok(why, sizeof why)) snprintf(g_status, sizeof g_status, "%s", why);
    else                           snprintf(g_status, sizeof g_status, "Ready.");
    // GEOMETRY AS THE CLIENT SEES IT, so a test clicks what the layout
    // actually produced rather than re-deriving it in Python -- the trap
    // calculator_client_test.py documents at length. Not gated on
    // desktop.layout_log: this is two lines at startup, not a per-frame
    // block, and a tool that had to turn a setting on first would be
    // driving the thing it is trying to measure.
    ulogf("diskmark: layout run %d %d %d %d\n", g_run.x, g_run.y, g_run.w, g_run.h);
    for (int i = 0; i < PROFILES; i++) {
        ulogf("diskmark: layout tile %d %d %d %d %d\n", i,
              g_meter[i].x, g_meter[i].y, g_meter[i].w, g_meter[i].h);
    }
    uapp_log_layout(a, "diskmark");
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

    for (unsigned i = 0; i < sizeof g_buf; i++) g_buf[i] = (uint8_t)(i * 31u + 7u);

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
    uui_button_init(&g_run, 0, 0, 0, 0, "Run", UTHEME_BUTTON_BG, UTHEME_TEXT, 0);

    g_bar_items[0].ops = &uui_dropdown_ops;
    g_bar_items[0].widget = &g_size;
    g_bar_items[1].ops = &uui_button_ops;
    g_bar_items[1].widget = &g_run;
    g_bar_items[1].id = ID_RUN;
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
        .tick_ms   = 30,
        .on_tick   = on_tick,
        .on_open   = on_open,
        .on_widget = on_widget,
        .on_close  = on_close,
        // Two copies would fight over one temp file and produce two
        // sets of numbers from one disk.
        .flags     = UAPP_SINGLE_INSTANCE,
    };
    return uapp_run(&desc);
}
