// See taskbar_menu.h.
#include "wm_internal.h"
#include "taskbar_menu.h"
#include "context_menu.h"
#include "gui_apps.h"
#include "leave_page.h"
#include "wm_taskbar.h"
#include "lib/usetting.h"
#include "wm/wm_log.h"
#include "ui/utheme.h"
#include "rt/sys.h"

// --- shared rows -------------------------------------------------------

// An app by its AppId (the Exec basename unless the entry names one),
// or NULL when this system does not have it -- a row for a missing app
// is left out rather than greyed, as the Start menu leaves it out.
static const struct gui_app *app_named(const char *id) {
    return gui_app_by_id(GUI_SHOW_ALL, id);
}

static void open_app_row(void *ctx) { open_app((const struct gui_app *)ctx); }

// System Settings, on the page that holds `setting` (settings.c takes a
// setting's name as its argument and opens its group).
static void open_settings_at(const char *setting) {
    int pid = sys_spawn("/bin/wm/system/settings", setting, -1);
    if (pid > 0) wm_track_launched(pid);
    else wm_logf("taskbar: could not start System Settings");
}

// --- the empty strip --------------------------------------------------

// A choice row writes one setting; the taskbar picks the value up on
// its next config poll, as it does a change made in System Settings.
struct choice { const char *setting, *value; };

static void set_choice(void *ctx) {
    const struct choice *c = (const struct choice *)ctx;
    if (usetting_set(c->setting, c->value) == SETTING_INVALID)
        wm_logf("taskbar: %s=%s refused", c->setting, c->value);
}

static const struct choice COMBINE[] = {
    { "desktop.taskbar_combine", "always" },
    { "desktop.taskbar_combine", "full" },
    { "desktop.taskbar_combine", "never" },
};
static const struct choice ALIGN[] = {
    { "desktop.taskbar_align", "left" },
    { "desktop.taskbar_align", "center" },
};
static const struct choice BUTTONS[] = {
    { "desktop.taskbar_buttons", "labelled" },
    { "desktop.taskbar_buttons", "icons" },
};
static const struct choice FLOAT_ON  = { "desktop.taskbar_float", "on" };
static const struct choice FLOAT_OFF = { "desktop.taskbar_float", "off" };

static void open_taskbar_settings(void *ctx) {
    (void)ctx;
    open_settings_at("desktop.taskbar_combine");
}

// Static: context_menu_open_at() keeps the caller's rows while it is up.
// Rebuilt on every open, so the ticks say what is set NOW.
static struct context_menu_item g_combine_rows[3];
static struct context_menu_item g_align_rows[2];
static struct context_menu_item g_buttons_rows[2];
static struct context_menu_item g_strip_rows[8];

void taskbar_menu_open_strip(int mx, int my) {
    enum taskbar_combine cb = taskbar_combine();
    static const char *const combine_label[3] = { "Always", "When full", "Never" };
    int combine_on[3] = { cb == TASKBAR_COMBINE_ALWAYS, cb == TASKBAR_COMBINE_FULL,
                          cb == TASKBAR_COMBINE_NEVER };
    for (int i = 0; i < 3; i++)
        g_combine_rows[i] = (struct context_menu_item){ .label = combine_label[i],
            .on_select = set_choice, .ctx = (void *)&COMBINE[i], .checked = combine_on[i] };

    int centred = taskbar_align_centered();
    g_align_rows[0] = (struct context_menu_item){ .label = "Left", .on_select = set_choice,
                                                  .ctx = (void *)&ALIGN[0], .checked = !centred };
    g_align_rows[1] = (struct context_menu_item){ .label = "Centre", .on_select = set_choice,
                                                  .ctx = (void *)&ALIGN[1], .checked = centred };

    int icons = taskbar_buttons() == TASKBAR_BUTTONS_ICONS;
    g_buttons_rows[0] = (struct context_menu_item){ .label = "Labelled", .on_select = set_choice,
                                                    .ctx = (void *)&BUTTONS[0], .checked = !icons };
    g_buttons_rows[1] = (struct context_menu_item){ .label = "Icons only", .on_select = set_choice,
                                                    .ctx = (void *)&BUTTONS[1], .checked = icons };

    struct context_menu_item *it = g_strip_rows;
    int n = 0;
    const struct gui_app *tm = app_named("taskmgr");
    if (tm) {
        it[n++] = (struct context_menu_item){ .label = "Task Manager", .on_select = open_app_row,
                                              .ctx = (void *)tm, .icon = tm->icon_name[0] ? tm->icon_name : 0 };
        it[n++] = (struct context_menu_item){ .separator = 1 };
    }
    it[n++] = (struct context_menu_item){ .label = "Combine buttons", .sub = g_combine_rows,
                                          .sub_count = 3, .icon = "tb-panes", .tint = UTHEME_ACT_ARRANGE };
    it[n++] = (struct context_menu_item){ .label = "Button position", .sub = g_align_rows,
                                          .sub_count = 2, .icon = "tb-move", .tint = UTHEME_ACT_ARRANGE };
    it[n++] = (struct context_menu_item){ .label = "Buttons", .sub = g_buttons_rows,
                                          .sub_count = 2, .icon = "tb-icons", .tint = UTHEME_ACT_VIEW };
    // A TOGGLE, ticked when on -- one row, not a submenu of two.
    int fl = taskbar_float_on();
    it[n++] = (struct context_menu_item){ .label = "Floating panel", .on_select = set_choice,
                                          .ctx = (void *)(fl ? &FLOAT_OFF : &FLOAT_ON), .checked = fl };
    it[n++] = (struct context_menu_item){ .separator = 1 };
    it[n++] = (struct context_menu_item){ .label = "Taskbar settings", .on_select = open_taskbar_settings,
                                          .icon = "tb-gear" };
    context_menu_open_at(mx, my, it, n);
}

// --- the Start button -------------------------------------------------

static void leave_restart(void *ctx)  { (void)ctx; leave_page_show(LEAVE_RESTART); }
static void leave_shutdown(void *ctx) { (void)ctx; leave_page_show(LEAVE_SHUTDOWN); }
static void leave_exit(void *ctx)     { (void)ctx; leave_page_show(LEAVE_EXIT); }

// The Leave rows go through the Leave PAGE, as the Start menu's footer
// does: it asks every app to close before acting and offers the other two.
static const struct context_menu_item LEAVE_ROWS[] = {
    { .label = "Restart",       .on_select = leave_restart,  .icon = "tb-refresh", .tint = UTHEME_ACT_VIEW },
    { .label = "Shut down",     .on_select = leave_shutdown, .icon = "tb-power",   .tint = UTHEME_ACT_DANGER },
    { .label = "Exit to shell", .on_select = leave_exit,     .icon = "tb-back",    .tint = UTHEME_ACT_NAV },
};

// The tools, in groups; an empty id is a separator.
static const char *const START_TOOLS[] = {
    "terminal", "files", "",
    "taskmgr", "devmgr", "logview", "crashreports", "bootmgr", "sysupdate", "",
    "settings",
};
#define START_TOOL_COUNT ((int)(sizeof START_TOOLS / sizeof START_TOOLS[0]))

static struct context_menu_item g_start_rows[START_TOOL_COUNT + 2];

void taskbar_menu_open_start(int mx, int my) {
    struct context_menu_item *it = g_start_rows;
    int n = 0;
    for (int i = 0; i < START_TOOL_COUNT; i++) {
        if (!START_TOOLS[i][0]) {
            // No rule at the top, and never two in a row.
            if (n && !it[n - 1].separator) it[n++] = (struct context_menu_item){ .separator = 1 };
            continue;
        }
        const struct gui_app *app = app_named(START_TOOLS[i]);
        if (!app) continue;
        it[n++] = (struct context_menu_item){ .label = app->name, .on_select = open_app_row,
                                              .ctx = (void *)app,
                                              .icon = app->icon_name[0] ? app->icon_name : 0 };
    }
    if (n && !it[n - 1].separator) it[n++] = (struct context_menu_item){ .separator = 1 };
    it[n++] = (struct context_menu_item){ .label = "Leave", .sub = LEAVE_ROWS,
                                          .sub_count = (int)(sizeof LEAVE_ROWS / sizeof LEAVE_ROWS[0]),
                                          .icon = "tb-power", .tint = UTHEME_ACT_DANGER };
    context_menu_open_at(mx, my, it, n);
}
