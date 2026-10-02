// Task Manager's Services page: what init supervises, and Start, Stop
// and Restart -- Windows Task Manager's Services tab, over the same two
// halves `/bin/service` uses (docs/commands/service.md):
//
//   READING is /run/init.status, a file. init writes it only once a
//   reader has rung (the doorbell, SIGHUP) and keeps it current after,
//   so this rings ONCE and then only reads.
//   ACTING is the `initctl` channel (lib/uinitctl.h): one round trip
//   with a result. OK means init ACCEPTED it, not that the service is
//   already down -- the table catches up on the next read.
//
// RESTART IS STOP, THEN START ONCE THE STATE HAS LEFT `running` -- there
// is no restart verb, and sending the two back to back would start a
// service that had not finished stopping. The wait is a pending name
// checked on each tick, never a sleep on the paint path.
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include "rt/sys.h"
#include "ui/ulog.h"
#include "ui/ugfx.h"
#include "ui/uapp.h"
#include "ui/utheme.h"
#include "ui/uui_widget.h"
#include "ui/uui_layout.h"
#include "ui/uui_table.h"
#include "ui/uui_button.h"
#include "ui/uui_label.h"
#include "ui/uui_statusbar.h"
#include "ui/uui_focus.h"
#include "ui/uui_route.h"   // UUI_REASON_*
#include "lib/uchan.h"
#include "lib/uinitctl.h"
#include "lib/uconf.h"
#include "tmppath.h"
#include "taskmgr/tm_internal.h"

#define STATUS_PATH   TMP_RUNDIR "/init.status"
#define SERVICES_DIR  "/etc/services.d"
// A bound on the one round trip. init answers from its main loop, which
// the channel's kick wakes; this is the case where it is wedged.
#define CHAN_REPLY_MS 2000

enum { ID_TABLE = TM_ID_SVC, ID_START, ID_STOP, ID_RESTART };

struct tm_service g_svc[TM_SERVICES_MAX];
int g_nsvc;
static int g_rang;              // the doorbell, once
static char g_pending[24];      // a Restart waiting for its stop to land

int tm_service_of_pid(int pid) {
    if (pid <= 0) return -1;
    for (int i = 0; i < g_nsvc; i++) if (g_svc[i].pid == pid) return i;
    return -1;
}

static int init_pid(void) {
    // By being the kernel's `init`, not by number (service.c's rule).
    for (int i = 0; i < g_nproc; i++)
        if (g_proc[i].ppid == 0 && strcmp(g_proc[i].name, "init") == 0) return g_proc[i].pid;
    return 0;
}

// Field `want` of a whitespace-padded line: fields, not columns, so the
// padding init writes for a human is not a second thing to keep true.
static int field(const char *line, int want, char *out, int cap) {
    const char *p = line;
    out[0] = '\0';
    for (int idx = 0; ; idx++) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n') return 0;
        const char *s = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        if (idx == want) {
            int n = (int)(p - s);
            if (n >= cap) n = cap - 1;
            memcpy(out, s, (size_t)n);
            out[n] = '\0';
            return 1;
        }
    }
}

// Descriptions are the descriptors', read once per name: they change
// only when somebody edits /etc/services.d, and a file per service per
// tick is a directory walk nobody needs.
static void describe(struct tm_service *s, const struct tm_service *old, int nold) {
    for (int i = 0; i < nold; i++)
        if (strcmp(old[i].name, s->name) == 0) { strlcpy(s->desc, old[i].desc, sizeof s->desc); return; }
    char path[96];
    snprintf(path, sizeof path, SERVICES_DIR "/%s", s->name);
    if (!uconf_get(path, "Description", s->desc, sizeof s->desc)) s->desc[0] = '\0';
}

void tm_services_read(void) {
    static char buf[8192];   // init.c's SVC_STATUS_MAX
    static struct tm_service old[TM_SERVICES_MAX];
    int fd = open(STATUS_PATH, O_RDONLY);
    if (fd < 0) {
        if (!g_rang) {
            int pid = init_pid();
            if (pid > 0) sys_kill(pid, SIGHUP);   // "somebody is reading"
            g_rang = 1;
        }
        return;   // the next read finds it
    }
    int64_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    int nold = g_nsvc;
    memcpy(old, g_svc, sizeof old);
    g_nsvc = 0;
    for (const char *line = buf; *line && g_nsvc < TM_SERVICES_MAX; ) {
        const char *next = strchr(line, '\n');
        if (*line != '#') {
            struct tm_service *s = &g_svc[g_nsvc];
            char num[12];
            memset(s, 0, sizeof *s);
            if (field(line, 0, s->name, sizeof s->name) &&
                field(line, 1, s->state, sizeof s->state)) {
                if (field(line, 2, num, sizeof num)) s->pid = atoi(num);
                if (field(line, 3, num, sizeof num)) s->fails = atoi(num);
                field(line, 4, s->ready, sizeof s->ready);
                field(line, 5, s->exec, sizeof s->exec);
                describe(s, old, nold);
                g_nsvc++;
            }
        }
        if (!next) break;
        line = next + 1;
    }
}

// One verb over the channel. Returns INITCTL_* or -1 when init did not
// answer; the file-and-doorbell fallback `/bin/service` keeps is not
// repeated here -- that path is for an init too old to have a channel.
// ONE CLIENT FOR THE LIFE OF THE APP, never one per request: the server
// knows a client by PID and keeps the ring it first mapped, so a client
// that closed and reopened -- a new ring under the same name -- is not
// heard until the server happens to notice the old one gone
// (docs/bugs.md). A failed call drops the client, and the next verb
// opens a fresh one.
static struct uchan_client g_chan;
static int g_chan_open;

static int send_verb(uint32_t verb, const char *name) {
    if (!g_chan_open) {
        if (uchan_client_open(&g_chan, INITCTL_SERVICE) < 0) return -1;
        g_chan_open = 1;
    }
    struct initctl_msg m, reply;
    memset(&m, 0, sizeof m);
    m.verb = verb;
    snprintf(m.name, sizeof m.name, "%s", name);
    unsigned long long t0 = sys_monotonic_ns();
    int ok = uchan_call(&g_chan, &m, sizeof m, &reply, sizeof reply, CHAN_REPLY_MS) == 0;
    if (!ok) { uchan_client_close(&g_chan); g_chan_open = 0; }
    ulogf("taskmgr: service %s %s -> %d in %llu ms\n", verb == INITCTL_START ? "start" : "stop",
          name, ok ? (int)reply.result : -1, (sys_monotonic_ns() - t0) / 1000000ULL);
    return ok ? (int)reply.result : -1;
}

// --- the page ----------------------------------------------------------------

enum { COL_NAME, COL_STATE, COL_PID, COL_FAILS, COL_DESC };
static const struct uui_table_column COLUMNS[] = {
    { "Name",        12, UUI_TALIGN_LEFT  },
    { "State",       10, UUI_TALIGN_LEFT  },
    { "PID",         5,  UUI_TALIGN_RIGHT },
    { "Fails",       5,  UUI_TALIGN_RIGHT },
    { "Description", 0,  UUI_TALIGN_LEFT  },
};
static struct uui_table g_table;
static struct uui_button g_btn_start, g_btn_stop, g_btn_restart;
static struct uui_label g_title, g_spacer, g_detail;
static char g_detail_text[160];
static struct uui_statusbar g_status;
static char g_st_count[64], g_st_msg[64];
static char g_sel_name[24];   // the selection, by NAME -- rows are re-read

static void cell(void *ctx, int row, int col, char *out, int cap) {
    (void)ctx;
    out[0] = '\0';
    if (row < 0 || row >= g_nsvc) return;
    const struct tm_service *s = &g_svc[row];
    switch (col) {
    case COL_NAME:  strlcpy(out, s->name, (size_t)cap); break;
    case COL_STATE: strlcpy(out, s->state, (size_t)cap); break;
    case COL_PID:   if (s->pid > 0) snprintf(out, (size_t)cap, "%d", s->pid); break;
    case COL_FAILS: snprintf(out, (size_t)cap, "%d", s->fails); break;
    case COL_DESC:  strlcpy(out, s->desc, (size_t)cap); break;
    default: break;
    }
}

static int compare(void *ctx, int a, int b, int col) {
    (void)ctx;
    const struct tm_service *x = &g_svc[a], *y = &g_svc[b];
    switch (col) {
    case COL_PID:   return x->pid - y->pid;
    case COL_FAILS: return x->fails - y->fails;
    case COL_STATE: return strcmp(x->state, y->state);
    case COL_DESC:  return strcmp(x->desc, y->desc);
    default:        return strcmp(x->name, y->name);
    }
}

// A stopped or crashed service is marked, the way `service list` would
// make you read for it -- the table's row tint, the widget's own hook.
static uint32_t tint(void *ctx, int row) {
    (void)ctx;
    const char *st = g_svc[row].state;
    if (!strcmp(st, "crash-loop") || !strcmp(st, "failed") || !strcmp(st, "no-exec"))
        return ugfx_blend(UTHEME_WHITE, ugfx_rgb(200, 90, 40), 40);
    return 0;
}

void tm_services_select(const char *name) {
    strlcpy(g_sel_name, name, sizeof g_sel_name);
    ulogf("taskmgr: service selected %s\n", g_sel_name);
}

static const struct tm_service *selected(void) {
    for (int i = 0; i < g_nsvc; i++)
        if (strcmp(g_svc[i].name, g_sel_name) == 0) return &g_svc[i];
    return 0;
}

static void refresh_page(void) {
    int sel = -1;
    for (int i = 0; i < g_nsvc; i++) if (strcmp(g_svc[i].name, g_sel_name) == 0) sel = i;
    g_table.selected = sel;
    uui_table_set_rows(&g_table, g_nsvc);

    const struct tm_service *s = selected();
    int running = s && !strcmp(s->state, "running");
    // The selected service's state, logged on a CHANGE -- what a test
    // waits for before the next click, instead of guessing at timing.
    static char last[40];
    char now[40];
    snprintf(now, sizeof now, "%s %s", s ? s->name : "-", s ? s->state : "-");
    if (s && strcmp(now, last) != 0) {
        ulogf("taskmgr: service %s state %s\n", s->name, s->state);
        strlcpy(last, now, sizeof last);
    }
    g_btn_start.disabled = !s || running;
    g_btn_stop.disabled = !s || !running;
    g_btn_restart.disabled = !s || !running;
    if (!s)
        strlcpy(g_detail_text, "Select a service to start or stop it.", sizeof g_detail_text);
    else if (s->pid > 0)
        snprintf(g_detail_text, sizeof g_detail_text, "%s -- runs %s as pid %d, from %s/%s",
                 s->name, s->exec, s->pid, SERVICES_DIR, s->name);
    else
        snprintf(g_detail_text, sizeof g_detail_text, "%s -- %s, from %s/%s",
                 s->name, s->exec, SERVICES_DIR, s->name);

    int run = 0, stop = 0, other = 0;
    for (int i = 0; i < g_nsvc; i++) {
        if (!strcmp(g_svc[i].state, "running")) run++;
        else if (!strcmp(g_svc[i].state, "stopped")) stop++;
        else other++;
    }
    snprintf(g_st_count, sizeof g_st_count, "%d services: %d running, %d stopped, %d other",
             g_nsvc, run, stop, other);
}

static void verb(int id) {
    const struct tm_service *s = selected();
    if (!s) return;
    int r;
    if (id == ID_START) r = send_verb(INITCTL_START, s->name);
    else r = send_verb(INITCTL_STOP, s->name);
    if (id == ID_RESTART && r == INITCTL_OK) strlcpy(g_pending, s->name, sizeof g_pending);
    const char *what = id == ID_START ? "Start" : id == ID_STOP ? "Stop" : "Restart";
    if (r == INITCTL_OK) snprintf(g_st_msg, sizeof g_st_msg, "%s %s: asked", what, s->name);
    else if (r < 0) snprintf(g_st_msg, sizeof g_st_msg, "%s %s: init did not answer", what, s->name);
    else snprintf(g_st_msg, sizeof g_st_msg, "%s %s: refused", what, s->name);
}

static int on_widget(struct uapp *a, int id, int reason) {
    (void)reason;
    (void)a;
    if (id == ID_TABLE) {
        if (g_table.selected >= 0 && g_table.selected < g_nsvc &&
            strcmp(g_sel_name, g_svc[g_table.selected].name) != 0) {
            strlcpy(g_sel_name, g_svc[g_table.selected].name, sizeof g_sel_name);
            ulogf("taskmgr: service selected %s\n", g_sel_name);
        }
        refresh_page();
        return 1;
    }
    return 0;
}

// Start, Stop, Restart.
static int on_action(struct uapp *a, int code) {
    (void)a;
    if (code != ID_START && code != ID_STOP && code != ID_RESTART) return 0;
    verb(code);
    tm_services_read();
    refresh_page();
    return 1;
}

static void tick(struct uapp *a, int shown) {
    (void)a;
    // Every tick while showing (the shell's own read is every fourth): a
    // Stop must enable Start within a tick, not two seconds later.
    if (shown) tm_services_read();
    if (g_pending[0]) {
        for (int i = 0; i < g_nsvc; i++) {
            if (strcmp(g_svc[i].name, g_pending)) continue;
            if (strcmp(g_svc[i].state, "running") && strcmp(g_svc[i].state, "starting")) {
                send_verb(INITCTL_START, g_pending);
                g_pending[0] = '\0';
            }
        }
    }
    if (shown) refresh_page();
}

static void page_open(struct uapp *a) {
    (void)a;
    tm_services_read();
    refresh_page();
    ulogf("taskmgr: services %d\n", g_nsvc);
}

static int focusables(struct uui_focusable *out, int cap) {
    if (cap < 1) return 0;
    out[0] = (struct uui_focusable){ &g_table, &uui_table_ops };
    return 1;
}

static struct uui_item BAR_ITEMS[5];
static struct uui_layout BAR;
static struct uui_item PAGE_ITEMS[4];
static struct uui_layout PAGE;
static struct uui_item g_root = { .ops = &uui_layout_ops, .widget = &PAGE, .name = "services" };

void tm_services_init(struct tm_page *page) {
    uui_table_init(&g_table, 0, 0, 100, 100, COLUMNS,
                   (int)(sizeof COLUMNS / sizeof COLUMNS[0]), cell, 0);
    uui_table_set_compare(&g_table, compare);
    uui_table_set_sort(&g_table, COL_NAME, 1);
    uui_table_set_tint(&g_table, tint);

    uui_label_init(&g_title, "Services supervised by init");
    g_title.font = ugfx_font_session(UGFX_FONT_BOLD);
    uui_label_init(&g_spacer, "");
    uui_label_init(&g_detail, g_detail_text);
    uui_button_init(&g_btn_start, 0, 0, 0, 0, "Start", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_START);
    uui_button_init(&g_btn_stop, 0, 0, 0, 0, "Stop", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_STOP);
    uui_button_init(&g_btn_restart, 0, 0, 0, 0, "Restart", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_RESTART);
    g_btn_start.outlined = g_btn_stop.outlined = g_btn_restart.outlined = 1;

    BAR_ITEMS[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_title };
    BAR_ITEMS[1] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_spacer, .flags = UUI_FILL_W };
    BAR_ITEMS[2] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_btn_start,
                                       .id = ID_START, .name = "svc_start" };
    BAR_ITEMS[3] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_btn_stop,
                                       .id = ID_STOP, .name = "svc_stop" };
    BAR_ITEMS[4] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_btn_restart,
                                       .id = ID_RESTART, .name = "svc_restart" };
    BAR = (struct uui_layout){ .dir = UUI_ROW, .items = BAR_ITEMS, .count = 5 };

    uui_statusbar_init(&g_status);
    g_status.count = 2;
    g_status.panes[0] = (struct uui_status_pane){ g_st_count, 0 };
    g_status.panes[1] = (struct uui_status_pane){ g_st_msg, 28 };

    PAGE_ITEMS[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &BAR, .flags = UUI_FILL_W };
    PAGE_ITEMS[1] = (struct uui_item){ .ops = &uui_table_ops, .widget = &g_table, .id = ID_TABLE,
                                        .name = "svctable", .flags = UUI_FILL_W | UUI_FILL_H };
    PAGE_ITEMS[2] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_detail,
                                        .flags = UUI_FILL_W, .name = "svcdetail" };
    PAGE_ITEMS[3] = (struct uui_item){ .ops = &uui_statusbar_ops, .widget = &g_status,
                                        .flags = UUI_FILL_W };
    PAGE = (struct uui_layout){ .dir = UUI_COLUMN, .items = PAGE_ITEMS, .count = 4 };

    *page = (struct tm_page){
        .label = "Services", .icon = "tb-gear", .root = &g_root,
        .open = page_open, .tick = tick, .widget = on_widget, .action = on_action, .focusables = focusables,
    };
}
