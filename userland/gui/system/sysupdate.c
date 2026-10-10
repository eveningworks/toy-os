// System Update -- pull a new build from an update server, with the
// progress on screen.
//
// A WINDOW over userland/update/upd.c, the engine /bin/update also
// runs; the design, and why a library update waits for a restart, is
// docs/update-design.md. The layout is Windows Update's single page: a
// status heading, the server, one overall bar, then What's new / Files /
// Log as tabs (the mockups chosen 2026-09-30 and 2026-10-02). What's new
// is the release notes the engine cut at this machine's build.
//
// THREADS: a check or an install runs on a worker, which owns the plan
// while it runs and talks to this thread ONLY through uapp_post() and
// the log queue below. Everything drawn is touched here.
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "keyboard.h"   // KEY_PAGE_*, KEY_MOD_CTRL
#include "rt/sys.h"
#include "lib/uinitctl.h"
#include "lib/human.h"
#include "ui/uapp.h"
#include "ui/uui.h"
#include "ui/uui_label.h"
#include "ui/uui_markdown.h"
#include "ui/uui_table.h"
#include "ui/uui_tabs.h"
#include "ui/ulog.h"
#include "ui/utheme.h"
#include "update/upd.h"

enum { EV_LOG = 1, EV_PROGRESS, EV_DONE };
enum { JOB_NONE, JOB_CHECK, JOB_APPLY };

enum {
    ID_CHANGE = 1, ID_SERVER_EDIT, ID_RECENT, ID_USE, ID_EDIT_CANCEL,
    ID_TABLE, ID_LOGVIEW, ID_TABS, ID_NOTES, ID_CHECK, ID_MAIN,
};
enum { PAGE_NOTES, PAGE_FILES, PAGE_LOG, PAGE_COUNT };

// What the window is showing, which decides the heading and the buttons.
enum view {
    V_CHECKING, V_AVAILABLE, V_UPTODATE, V_INSTALLING, V_INSTALLED, V_RESTART, V_BLOCKED,
    V_FAILED, V_CANCELLED,
};

// ---- state shared with the worker -------------------------------------------

static struct upd_plan g_plan;
static char g_server[UPD_URL_MAX];
static int g_job;                   // JOB_*, set here, cleared here on EV_DONE
static volatile int g_cancel;
static int g_job_rc;

// Log lines cross threads through this queue; EV_LOG says "drain it".
#define LOGQ 64
static char g_logq[LOGQ][200];
static int g_logq_n;
static pthread_mutex_t g_logq_lock = PTHREAD_MUTEX_INITIALIZER;

static struct uapp *g_app;
static unsigned long long g_last_post_ns;

// ---- widgets --------------------------------------------------------------

static enum view g_view = V_CHECKING;
static char g_title_text[64], g_detail_text[200], g_amount_text[64], g_rate_text[64];
static struct uui_label g_title, g_detail, g_server_key, g_server_val, g_amount, g_rate, g_spacer;
static struct uui_button g_change, g_use, g_edit_cancel, g_check, g_main;
static struct uui_textbox g_edit;
static struct uui_dropdown g_recent;
static char g_recent_urls[UPD_RECENT_MAX][UPD_URL_MAX];
static const char *g_recent_items[UPD_RECENT_MAX + 1];
static struct uui_progress g_bar;
static struct uui_table g_table;
static struct uui_textview g_log;
static char g_log_buf[32768];
static int g_rows[UUI_TABLE_MAX_ROWS];   // plan index per table row
static int g_row_count;
static struct uui_markdown g_notes;
static char g_files_label[24] = "Files";
static struct uui_tab g_tab_list[PAGE_COUNT] = {
    { "What's new", 0, 0 }, { g_files_label, 0, 0 }, { "Log", 0, 0 },
};
static struct uui_tabs g_tabs;

// Speed, from the bytes done since the install started.
static unsigned long long g_apply_start_ns;

static const struct uui_table_column COLS[] = {
    { "File", 0, UUI_TALIGN_LEFT },
    { "Size", 8, UUI_TALIGN_RIGHT },
    { "Status", 12, UUI_TALIGN_LEFT },
};

static struct uui_item g_server_items[] = {
    { .ops = &uui_label_ops,  .widget = &g_server_key, .name = "serverkey" },
    { .ops = &uui_label_ops,  .widget = &g_server_val, .name = "server", .flags = UUI_FILL_W },
    { .ops = &uui_button_ops, .widget = &g_change, .id = ID_CHANGE, .name = "change" },
};
static struct uui_layout g_server_row = {
    .dir = UUI_ROW, .items = g_server_items,
    .count = sizeof g_server_items / sizeof g_server_items[0],
};
static struct uui_item g_edit_items[] = {
    { .ops = &uui_textbox_ops,  .widget = &g_edit, .id = ID_SERVER_EDIT, .name = "serveredit",
      .flags = UUI_FILL_W },
    { .ops = &uui_dropdown_ops, .widget = &g_recent, .id = ID_RECENT, .name = "recent" },
    { .ops = &uui_button_ops,   .widget = &g_use, .id = ID_USE, .name = "use" },
    { .ops = &uui_button_ops,   .widget = &g_edit_cancel, .id = ID_EDIT_CANCEL, .name = "editcancel" },
};
static struct uui_layout g_edit_row = {
    .dir = UUI_ROW, .items = g_edit_items, .count = sizeof g_edit_items / sizeof g_edit_items[0],
};
static struct uui_item g_amount_items[] = {
    { .ops = &uui_label_ops, .widget = &g_amount, .name = "amount", .flags = UUI_FILL_W },
    { .ops = &uui_label_ops, .widget = &g_rate, .name = "rate" },
};
static struct uui_layout g_amount_row = {
    .dir = UUI_ROW, .items = g_amount_items,
    .count = sizeof g_amount_items / sizeof g_amount_items[0],
};
static struct uui_item g_button_items[] = {
    { .ops = &uui_label_ops,  .widget = &g_spacer, .flags = UUI_FILL_W },
    { .ops = &uui_button_ops, .widget = &g_check, .id = ID_CHECK, .name = "check" },
    { .ops = &uui_button_ops, .widget = &g_main, .id = ID_MAIN, .name = "main" },
};
static struct uui_layout g_button_row = {
    .dir = UUI_ROW, .items = g_button_items,
    .count = sizeof g_button_items / sizeof g_button_items[0],
};

// The tab strip and the one page under it, tight against it.
static struct uui_item g_page_items[] = {
    { .ops = &uui_tabs_ops,     .widget = &g_tabs, .id = ID_TABS, .name = "tabs",
      .flags = UUI_FILL_W },
    { .ops = &uui_markdown_ops, .widget = &g_notes, .id = ID_NOTES, .name = "notes",
      .flags = UUI_FILL_W | UUI_FILL_H },
    { .ops = &uui_table_ops,    .widget = &g_table, .id = ID_TABLE, .name = "files",
      .flags = UUI_FILL_W | UUI_FILL_H, .hidden = 1 },
    { .ops = &uui_textview_ops, .widget = &g_log, .id = ID_LOGVIEW, .name = "log",
      .flags = UUI_FILL_W | UUI_FILL_H, .hidden = 1 },
};
static struct uui_layout g_page = {
    .dir = UUI_COLUMN, .gap = 1, .items = g_page_items,
    .count = sizeof g_page_items / sizeof g_page_items[0],
};

// Indices into g_items for the row that comes and goes.
enum { ITEM_EDIT = 3 };
static struct uui_item g_items[] = {
    { .ops = &uui_label_ops,    .widget = &g_title, .name = "title" },
    { .ops = &uui_label_ops,    .widget = &g_detail, .name = "detail", .flags = UUI_FILL_W },
    { .ops = &uui_layout_ops,   .widget = &g_server_row, .flags = UUI_FILL_W },
    { .ops = &uui_layout_ops,   .widget = &g_edit_row, .flags = UUI_FILL_W, .hidden = 1 },
    { .ops = &uui_progress_ops, .widget = &g_bar, .name = "bar", .flags = UUI_FILL_W },
    { .ops = &uui_layout_ops,   .widget = &g_amount_row, .flags = UUI_FILL_W },
    { .ops = &uui_layout_ops,   .widget = &g_page, .flags = UUI_FILL_W | UUI_FILL_H },
    { .ops = &uui_layout_ops,   .widget = &g_button_row, .flags = UUI_FILL_W },
};
static struct uui_layout g_root = {
    .dir = UUI_COLUMN, .items = g_items, .count = sizeof g_items / sizeof g_items[0],
};

static struct uui_focusable g_focusables[] = {
    { &g_change, &uui_button_ops },
    { &g_edit, &uui_textbox_ops },
    { &g_recent, &uui_dropdown_ops },
    { &g_use, &uui_button_ops },
    { &g_edit_cancel, &uui_button_ops },
    { &g_table, &uui_table_ops },
    { &g_check, &uui_button_ops },
    { &g_main, &uui_button_ops },
};
static struct uui_focus g_focus;

// ---- the worker ------------------------------------------------------------

static void w_log(void *ctx, const char *line) {
    (void)ctx;
    pthread_mutex_lock(&g_logq_lock);
    if (g_logq_n < LOGQ) snprintf(g_logq[g_logq_n++], sizeof g_logq[0], "%s", line);
    pthread_mutex_unlock(&g_logq_lock);
    uapp_post(g_app, EV_LOG, 0);
}

// Throttled: a download calls this every 32 KiB, and ten repaints a
// second is all a person can see.
static void w_progress(void *ctx, const struct upd_plan *p, int idx) {
    (void)ctx;
    unsigned long long now = sys_monotonic_ns();
    int milestone = idx >= 0 && p->files[idx].status != UPD_FETCHING;
    if (!milestone && now - g_last_post_ns < 100000000ULL) return;
    g_last_post_ns = now;
    uapp_post(g_app, EV_PROGRESS, idx);
}

static int w_cancelled(void *ctx) { (void)ctx; return g_cancel; }

static const struct upd_hooks HOOKS = {
    .log = w_log, .progress = w_progress, .cancelled = w_cancelled,
};

static void *worker(void *arg) {
    int job = (int)(long)arg;
    if (job == JOB_CHECK) {
        upd_plan_free(&g_plan);
        g_job_rc = upd_check(g_server, &g_plan, &HOOKS);
    } else {
        g_job_rc = upd_apply(&g_plan, &HOOKS);
    }
    uapp_post(g_app, EV_DONE, job);
    return 0;
}

static void refresh(struct uapp *a);
static void show_notes(void);

static void start_job(struct uapp *a, int job) {
    if (g_job != JOB_NONE) return;
    g_job = job;
    g_cancel = 0;
    g_view = job == JOB_CHECK ? V_CHECKING : V_INSTALLING;
    if (job == JOB_APPLY) g_apply_start_ns = sys_monotonic_ns();
    if (job == JOB_CHECK) {
        g_row_count = 0;
        show_notes();   // the worker frees the plan the page points into
    }
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    if (pthread_create(&th, &at, worker, (void *)(long)job) != 0) {
        worker((void *)(long)job);   // no thread: the window freezes for it, but it still works
    }
    ulogf("sysupdate: %s started\n", job == JOB_CHECK ? "check" : "install");
    refresh(a);
}

// ---- presenting --------------------------------------------------------------

static const char *status_word(const struct upd_file *f) {
    static char pct[16];
    switch (f->status) {
    case UPD_FETCHING:
        snprintf(pct, sizeof pct, "%d%%", f->size ? (int)(f->got * 100 / f->size) : 100);
        return pct;
    case UPD_STAGED:    return "Downloaded";
    case UPD_INSTALLED: return "Installed";
    case UPD_AT_BOOT:   return "At restart";
    case UPD_REMOVED:   return "Removed";
    case UPD_FAILED:    return g_view == V_CANCELLED ? "Stopped" : "Failed";
    default:            return f->change == UPD_NEW ? "New"
                             : f->change == UPD_REMOVE ? "To remove" : "Queued";
    }
}

static void cell(void *ctx, int row, int col, char *out, int cap) {
    (void)ctx;
    out[0] = '\0';
    if (row < 0 || row >= g_row_count) return;
    const struct upd_file *f = &g_plan.files[g_rows[row]];
    if (col == 0) snprintf(out, (size_t)cap, "%s", f->path);
    else if (col == 1 && f->change != UPD_REMOVE) human_size(out, (unsigned long)cap, f->size);
    else snprintf(out, (size_t)cap, "%s", status_word(f));
}

// The row being fetched is tinted, so the eye finds it in a long list.
static uint32_t tint(void *ctx, int row) {
    (void)ctx;
    if (row < 0 || row >= g_row_count) return 0;
    return g_plan.files[g_rows[row]].status == UPD_FETCHING ? UTHEME_SELECTION : 0;
}

static void collect_rows(void) {
    g_row_count = 0;
    for (int i = 0; i < g_plan.count && g_row_count < UUI_TABLE_MAX_ROWS; i++) {
        int c = g_plan.files[i].change;
        if (c == UPD_CHANGED || c == UPD_NEW || c == UPD_REMOVE) g_rows[g_row_count++] = i;
    }
    uui_table_set_rows(&g_table, g_row_count);
}

// What the What's new page says when the engine has no notes to give.
static void show_notes(void) {
    const char *t = g_plan.notes;
    if (g_job == JOB_CHECK) t = "Checking for release notes...";
    else if (!t && g_plan.notes_since) t = "Nothing new since this machine's build.";
    else if (!t) t = "This server publishes no release notes.";
    uui_markdown_set_text(&g_notes, t, (int)strlen(t));
}

static void show_page(struct uapp *a, int page) {
    g_page_items[1].hidden = page != PAGE_NOTES;
    g_page_items[2].hidden = page != PAGE_FILES;
    g_page_items[3].hidden = page != PAGE_LOG;
    uui_tabs_select(&g_tabs, page);
    if (a) {
        uui_layout_run(&g_root, 0, 0, uapp_width(a), uapp_height(a));
        uapp_redraw(a);
    }
}

static void tab_selected(void *ctx, int index) { show_page(ctx, index); }

static void set_button(struct uui_button *b, const char *label, int enabled) {
    b->label = label;
    b->disabled = !enabled;
}

static void relayout(struct uapp *a) {
    uui_layout_run(&g_root, 0, 0, uapp_width(a), uapp_height(a));
}

static void refresh(struct uapp *a) {
    char total[16], done[16];
    human_size(total, sizeof total, g_plan.bytes);
    human_size(done, sizeof done, g_plan.done_bytes);
    const char *title = "";
    g_detail_text[0] = g_amount_text[0] = g_rate_text[0] = '\0';
    char built[96] = "", removals[48] = "";
    if (g_plan.removals)
        snprintf(removals, sizeof removals, ", %d stale file%s %s", g_plan.removals,
                 g_plan.removals == 1 ? "" : "s", g_view == V_INSTALLED ? "removed" : "to remove");
    if (g_plan.built[0])
        snprintf(built, sizeof built, ", build of %s%s%s%s", g_plan.built,
                 g_plan.version[0] ? " (" : "", g_plan.version, g_plan.version[0] ? ")" : "");

    switch (g_view) {
    case V_CHECKING:
        title = "Checking for updates";
        snprintf(g_detail_text, sizeof g_detail_text, "Comparing this machine with the server");
        uui_progress_set_busy(&g_bar);
        break;
    case V_AVAILABLE:
        title = "Updates available";
        snprintf(g_detail_text, sizeof g_detail_text, "%d of %d files changed, %s%s%s",
                 g_plan.changed, g_plan.count - g_plan.removals, total, removals, built);
        uui_progress_set(&g_bar, 0);
        snprintf(g_amount_text, sizeof g_amount_text, "%s to download", total);
        break;
    case V_UPTODATE:
        title = "Up to date";
        snprintf(g_detail_text, sizeof g_detail_text, "All %d files match the server%s",
                 g_plan.count - g_plan.removals, built);
        uui_progress_set(&g_bar, 1000);
        break;
    case V_INSTALLING:
        title = "Installing updates";
        snprintf(g_detail_text, sizeof g_detail_text, "%d of %d files changed, %s%s",
                 g_plan.changed, g_plan.count, total, built);
        uui_progress_set(&g_bar, g_plan.bytes ? (int)(g_plan.done_bytes * 1000 / g_plan.bytes) : 0);
        snprintf(g_amount_text, sizeof g_amount_text, "%s of %s", done, total);
        {
            unsigned long long el = sys_monotonic_ns() - g_apply_start_ns;
            unsigned long long ms = el / 1000000ULL;
            if (ms > 500 && g_plan.done_bytes) {
                unsigned long long rate = g_plan.done_bytes * 1000ULL / ms;
                unsigned long long left = g_plan.bytes > g_plan.done_bytes
                                          ? (g_plan.bytes - g_plan.done_bytes) / (rate ? rate : 1) : 0;
                char r[16];
                human_size(r, sizeof r, rate);
                snprintf(g_rate_text, sizeof g_rate_text, "%s/s, about %llu s left", r,
                         left + 1);
            }
        }
        break;
    case V_INSTALLED:
        title = "Updates installed";
        snprintf(g_detail_text, sizeof g_detail_text, "%d file%s updated%s%s", g_plan.changed,
                 g_plan.changed == 1 ? "" : "s", removals, built);
        uui_progress_set(&g_bar, 1000);
        snprintf(g_amount_text, sizeof g_amount_text, "%s", total);
        break;
    case V_RESTART:
        title = "Restart to finish";
        if (g_plan.staged_for_boot && !g_plan.at_boot)
            snprintf(g_detail_text, sizeof g_detail_text,
                     "An update is waiting; it is applied while the machine starts");
        else
            snprintf(g_detail_text, sizeof g_detail_text,
                     "%d files downloaded and verified%s. They are put in place while "
                     "the machine restarts, because %s", g_plan.changed, built,
                     g_plan.kernel_installed ? "the kernel changed" : "a library changed");
        uui_progress_set(&g_bar, 1000);
        if (g_plan.at_boot || g_plan.kernel_installed)
            snprintf(g_amount_text, sizeof g_amount_text, "%s downloaded", total);
        break;
    case V_BLOCKED:
        title = "The kernel cannot be updated";
        snprintf(g_detail_text, sizeof g_detail_text,
                 "GRUB on this machine shows no menu, so the previous kernel could not be "
                 "chosen if a new one failed. Nothing will be installed.");
        uui_progress_set(&g_bar, 0);
        break;
    case V_CANCELLED:
        title = "Update cancelled";
        snprintf(g_detail_text, sizeof g_detail_text, "Nothing was changed");
        uui_progress_set(&g_bar, 0);
        break;
    case V_FAILED:
        title = "Update failed";
        snprintf(g_detail_text, sizeof g_detail_text, "%s", g_plan.error[0] ? g_plan.error : "see the log");
        uui_progress_set(&g_bar, 0);
        break;
    }
    snprintf(g_title_text, sizeof g_title_text, "%s", title);
    uui_label_set_text(&g_title, g_title_text);
    uui_label_set_text(&g_detail, g_detail_text);
    uui_label_set_text(&g_amount, g_amount_text);
    uui_label_set_text(&g_rate, g_rate_text);
    uui_label_set_text(&g_server_val, g_server);

    int busy = g_job != JOB_NONE;
    set_button(&g_check, "Check again", !busy);
    set_button(&g_change, "Change...", !busy);
    switch (g_view) {
    case V_CHECKING: case V_INSTALLING: set_button(&g_main, "Cancel", 1); break;
    case V_AVAILABLE:                   set_button(&g_main, "Install", 1); break;
    case V_RESTART:                     set_button(&g_main, "Restart now", 1); break;
    default:                            set_button(&g_main, "Install", 0); break;
    }
    uui_table_set_rows(&g_table, g_row_count);
    if (g_row_count) snprintf(g_files_label, sizeof g_files_label, "Files (%d)", g_row_count);
    else snprintf(g_files_label, sizeof g_files_label, "Files");
    show_notes();
    relayout(a);
    uapp_redraw(a);
}

static void drain_log(void) {
    pthread_mutex_lock(&g_logq_lock);
    for (int i = 0; i < g_logq_n; i++) {
        for (const char *c = g_logq[i]; *c; c++) utext_putc(&g_log.tb, *c);
        utext_putc(&g_log.tb, '\n');
    }
    g_logq_n = 0;
    pthread_mutex_unlock(&g_logq_lock);
}

static void job_done(struct uapp *a, int job) {
    g_job = JOB_NONE;
    if (job == JOB_CHECK) {
        collect_rows();
        if (g_job_rc != 0) g_view = g_cancel ? V_CANCELLED : V_FAILED;
        else if (g_plan.staged_for_boot) g_view = V_RESTART;
        else if (g_plan.kernel_blocked) g_view = V_BLOCKED;
        else g_view = (g_plan.changed || g_plan.removals) ? V_AVAILABLE : V_UPTODATE;
    } else {
        if (g_job_rc != 0) g_view = g_cancel ? V_CANCELLED : V_FAILED;
        else if (g_plan.at_boot || g_plan.kernel_installed) g_view = V_RESTART;
        else g_view = V_INSTALLED;
    }
    // What a test reads: the view, and the counts it was drawn from.
    ulogf("sysupdate: view %d changed %d removals %d staged %d installed %s\n", (int)g_view,
          g_plan.changed, g_plan.removals, g_plan.at_boot, g_job_rc == 0 ? "ok" : "failed");
    ulogf("sysupdate: notes %d hood %d quiet %d since %d\n", g_plan.notes_count,
          g_plan.notes_hood, g_plan.notes_quiet, g_plan.notes_since);
    refresh(a);
    uapp_log_layout(a, "sysupdate");   // the tab slots, for a test to click (ui/uui_describe.h)
}

// ---- the server editor --------------------------------------------------------

static void show_editor(struct uapp *a, int on) {
    g_items[ITEM_EDIT].hidden = !on;
    if (on) {
        uui_textbox_set_text(&g_edit, g_server);
        int n = upd_recent(g_recent_urls, UPD_RECENT_MAX);
        g_recent_items[0] = "Recent";
        for (int i = 0; i < n; i++) g_recent_items[i + 1] = g_recent_urls[i];
        uui_dropdown_init(&g_recent, 0, 0, 0, 0, g_recent_items, n + 1);
        uui_focus_set(&g_focus, 1);
    }
    relayout(a);
    uapp_redraw(a);
}

static void use_server(struct uapp *a) {
    const char *url = uui_textbox_text(&g_edit);
    if (upd_server_set(url) != 0) {
        snprintf(g_detail_text, sizeof g_detail_text,
                 "Not saved: the address must start with http:// and fit in 63 characters");
        uui_label_set_text(&g_detail, g_detail_text);
        uapp_redraw(a);
        return;
    }
    snprintf(g_server, sizeof g_server, "%s", url);
    ulogf("sysupdate: server %s\n", g_server);
    show_editor(a, 0);
    start_job(a, JOB_CHECK);
}

// ---- callbacks ----------------------------------------------------------------

static void on_widget(struct uapp *a, int id, int reason) {
    // A fold opened or the notes scrolled: their rows moved, and a test
    // reads where from the layout log (ui/uui_describe.h).
    if (id == ID_NOTES && (reason == UUI_REASON_RELEASE || reason == UUI_REASON_WHEEL)) {
        uapp_log_layout(a, "sysupdate");
        return;
    }
    if (id == ID_RECENT && reason == UUI_REASON_RELEASE) {
        int s = uui_dropdown_selected(&g_recent);
        if (s > 0) uui_textbox_set_text(&g_edit, g_recent_items[s]);
        return;
    }
}

static void on_action(struct uapp *a, int code) {
    switch (code) {
    case ID_CHANGE:      if (g_job == JOB_NONE) show_editor(a, g_items[ITEM_EDIT].hidden); break;
    case ID_USE:         use_server(a); break;
    case ID_EDIT_CANCEL: show_editor(a, 0); break;
    case ID_CHECK:       start_job(a, JOB_CHECK); break;
    case ID_MAIN:
        if (g_view == V_CHECKING || g_view == V_INSTALLING) g_cancel = 1;
        else if (g_view == V_AVAILABLE) start_job(a, JOB_APPLY);
        else if (g_view == V_RESTART) { sys_sync(); uinitctl_shutdown(1); }
        break;
    }
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    // Ctrl+PgUp/PgDn steps the tabs, as in the Terminal.
    if ((mods & KEY_MOD_CTRL) && (key == KEY_PAGE_UP || key == KEY_PAGE_DOWN)) {
        int step = key == KEY_PAGE_DOWN ? 1 : PAGE_COUNT - 1;
        show_page(a, (g_tabs.selected + step) % PAGE_COUNT);
        return;
    }
    if ((key == '\n' || key == '\r') && !g_items[ITEM_EDIT].hidden) use_server(a);
}

static int on_user(struct uapp *a, int kind, int arg) {
    if (kind == EV_LOG) { drain_log(); return 1; }
    if (kind == EV_DONE) { drain_log(); job_done(a, arg); return 1; }
    if (kind == EV_PROGRESS) {
        if (g_job == JOB_CHECK && g_plan.count && g_row_count == 0) {
            snprintf(g_detail_text, sizeof g_detail_text, "Comparing %d files", g_plan.count);
            uui_label_set_text(&g_detail, g_detail_text);
        }
        if (g_job == JOB_APPLY) refresh(a);
        return 1;
    }
    return 0;
}

static int on_tick(struct uapp *a) {
    (void)a;
    return uui_progress_tick(&g_bar);
}

// Closing mid-install would leave the worker writing into a process
// that is going away; ask it to stop first, and let the next close go.
static int on_close(struct uapp *a) {
    (void)a;
    if (g_job == JOB_NONE) return 1;
    g_cancel = 1;
    return 0;
}

// Font-derived, so here rather than in main(): the session font is not
// mapped until uapp_run() fetches it.
static void on_size(int *w, int *h) {
    g_title.font = ugfx_font_session(UGFX_FONT_BOLD);
    *w = ugfx_char_w() * 76;
    *h = ugfx_char_h() * 28;
}

static void on_open(struct uapp *a) {
    g_app = a;
    g_tabs.ctx = a;
    start_job(a, JOB_CHECK);
}

int main(void) {
    if (!ugfx_font_init()) return 2;
    upd_server_get(g_server, sizeof g_server);

    uui_label_init(&g_title, g_title_text);
    uui_label_init(&g_detail, g_detail_text);
    uui_label_set_wrap(&g_detail, 2);
    uui_label_init(&g_server_key, "Server");
    uui_label_init(&g_server_val, g_server);
    uui_label_init(&g_amount, g_amount_text);
    uui_label_init(&g_rate, g_rate_text);
    uui_label_init(&g_spacer, "");
    uui_button_init(&g_change, 0, 0, 0, 0, "Change...", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_CHANGE);
    uui_button_init(&g_use, 0, 0, 0, 0, "Use", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_USE);
    uui_button_init(&g_edit_cancel, 0, 0, 0, 0, "Cancel", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_EDIT_CANCEL);
    uui_button_init(&g_check, 0, 0, 0, 0, "Check again", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_CHECK);
    uui_button_init(&g_main, 0, 0, 0, 0, "Restart now", UTHEME_ACCENT, UTHEME_ACCENT_TEXT, ID_MAIN);
    g_change.outlined = g_use.outlined = g_edit_cancel.outlined = 1;
    g_check.outlined = 1;
    uui_textbox_init(&g_edit, g_server);
    g_recent_items[0] = "Recent";
    uui_dropdown_init(&g_recent, 0, 0, 0, 0, g_recent_items, 1);
    uui_progress_init(&g_bar);
    uui_table_init(&g_table, 0, 0, 0, 0, COLS, (int)(sizeof COLS / sizeof COLS[0]), cell, 0);
    uui_table_set_tint(&g_table, tint);
    uui_textview_init(&g_log, 0, 0, 0, 0, UTHEME_TEXT, UTHEME_WHITE, UTHEME_SELECTION,
                      g_log_buf, (int)sizeof g_log_buf);
    uui_markdown_init(&g_notes);
    uui_tabs_init(&g_tabs, g_tab_list, PAGE_COUNT, 0);
    g_tabs.on_select = tab_selected;
    show_page(0, PAGE_NOTES);
    uui_focus_init(&g_focus, g_focusables, (int)(sizeof g_focusables / sizeof g_focusables[0]));

    struct uapp_desc desc = {
        .title = "System Update",
        .app_id = "sysupdate",
        .layout = &g_root,
        .widgets = g_items,
        .widget_count = sizeof g_items / sizeof g_items[0],
        .focus = &g_focus,
        .on_widget = on_widget,
        .on_action = on_action,
        .on_key = on_key,
        .on_open = on_open,
        .on_user = on_user,
        .on_tick = on_tick,
        .tick_ms = 50,
        .on_close = on_close,
        .flags = UAPP_RESIZABLE | UAPP_SINGLE_INSTANCE,
        .on_size = on_size,
    };
    return uapp_run(&desc);
}
