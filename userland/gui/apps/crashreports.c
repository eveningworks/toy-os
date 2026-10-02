// Crash Reports -- the ring-3 crash reports the kernel writes to
// /var/crash (kernel/include/kernel/crash_report.h), newest first, with
// one report's details beside the list.
//
// TWO FACES, ONE PROGRAM. With no argument it is the list -- Windows'
// Reliability Monitor, GNOME's ABRT window. With `--report <path>` it is
// the dialog the desktop's crash notice opens for one crash ("Notepad
// quit unexpectedly"), macOS's and KDE DrKonqi's shape; "All reports"
// there opens the list.
//
// **IT READS THE REPORT'S TEXT HEADER, NOT THE KERNEL.** Everything
// shown -- program, fault, where -- is in the file, so a report from a
// previous boot reads the same as this boot's. Where the crash was is
// worked out from the header's memory map: a file mapping names the
// library and the offset into it ("libuapp.so +0x80370"), which is what
// tools/panic_resolve.py --crash wants on the host.
#include "ui/uapp.h"
#include "ui/uui.h"
#include "ui/uui_table.h"
#include "ui/uui_toolbar.h"
#include "ui/uui_button.h"
#include "ui/uui_label.h"
#include "ui/uui_focus.h"
#include "ui/utheme.h"
#include "ui/ugfx.h"
#include "lib/ufile.h"
#include "lib/uclip.h"
#include "rt/sys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#define CRASH_DIR   "/var/crash"
#define ENTRY_DIR   "/usr/wm/applications"
#define MAX_REPORTS 64
#define HEAD_CAP    8192   // the kernel's own HEADER_CAP: the whole text half
#define MAX_MAPS    48

struct report {
    char file[48];        // its name in CRASH_DIR
    char path[96];
    char program[64];     // the executable's path
    char name[40];        // what a person calls it: the desktop entry's Name=
    char fault[40];
    char where[72];
    char build[96];
    int pid;
    unsigned long long rip, cr2;
    unsigned err;
    time_t when;
    unsigned size;
};

static struct report g_rep[MAX_REPORTS];
static int g_n;
static struct report g_one;      // --report's
static int g_dialog;             // 1 = the one-report face

// --- what the desktop entries call a program -------------------------

static void friendly_name(const char *exec, char *out, int cap) {
    const char *base = strrchr(exec, '/');
    strlcpy(out, base ? base + 1 : exec, (size_t)cap);
    DIR *d = opendir(ENTRY_DIR);
    if (!d) return;
    struct dirent *e;
    char path[160], buf[1024];
    while ((e = readdir(d))) {
        if (!strstr(e->d_name, ".desktop")) continue;
        snprintf(path, sizeof path, ENTRY_DIR "/%s", e->d_name);
        size_t n = ufile_read_head(path, (uint8_t *)buf, sizeof buf - 1);
        buf[n] = '\0';
        char name[40] = "", ex[96] = "";
        for (char *l = buf; l && *l; ) {
            char *nl = strchr(l, '\n');
            if (nl) *nl = '\0';
            if (!strncmp(l, "Name=", 5)) strlcpy(name, l + 5, sizeof name);
            if (!strncmp(l, "Exec=", 5)) strlcpy(ex, l + 5, sizeof ex);
            l = nl ? nl + 1 : 0;
        }
        if (name[0] && !strcmp(ex, exec)) { strlcpy(out, name, (size_t)cap); break; }
    }
    closedir(d);
}

// --- reading one report ----------------------------------------------

struct map { unsigned long long a, b; char kind[8]; char path[64]; };

static void locate(struct report *r, const struct map *m, int n) {
    for (int i = 0; i < n; i++) {
        if (r->rip < m[i].a || r->rip >= m[i].b) continue;
        if (!strcmp(m[i].kind, "file") && m[i].path[0]) {
            // The library's BASE is its lowest mapping, not the segment
            // the address fell in -- offsets are into the file.
            unsigned long long base = m[i].a;
            for (int j = 0; j < n; j++)
                if (!strcmp(m[j].path, m[i].path) && m[j].a < base) base = m[j].a;
            const char *b = strrchr(m[i].path, '/');
            snprintf(r->where, sizeof r->where, "%s +0x%llx", b ? b + 1 : m[i].path, r->rip - base);
        } else if (!strcmp(m[i].kind, "image")) {
            const char *b = strrchr(r->program, '/');
            snprintf(r->where, sizeof r->where, "%s +0x%llx", b ? b + 1 : r->program, r->rip - m[i].a);
        } else {
            snprintf(r->where, sizeof r->where, "%s memory, 0x%llx", m[i].kind, r->rip);
        }
        return;
    }
    snprintf(r->where, sizeof r->where, "0x%llx, outside every mapping", r->rip);
}

static int parse_report(const char *path, struct report *r) {
    static char buf[HEAD_CAP + 1];
    static struct map maps[MAX_MAPS];
    size_t len = ufile_read_head(path, (uint8_t *)buf, HEAD_CAP);
    if (!len) return 0;
    buf[len] = '\0';
    char *stack = strstr(buf, "\n---- stack ----");
    if (stack) *stack = '\0';
    memset(r, 0, sizeof *r);
    strlcpy(r->path, path, sizeof r->path);
    const char *b = strrchr(path, '/');
    strlcpy(r->file, b ? b + 1 : path, sizeof r->file);
    int nmaps = 0;
    for (char *l = buf; l && *l; ) {
        char *nl = strchr(l, '\n');
        if (nl) *nl = '\0';
        if (!strncmp(l, "program: ", 9)) strlcpy(r->program, l + 9, sizeof r->program);
        else if (!strncmp(l, "pid: ", 5)) r->pid = atoi(l + 5);
        else if (!strncmp(l, "fault: ", 7)) strlcpy(r->fault, l + 7, sizeof r->fault);
        else if (!strncmp(l, "kernel: ", 8)) strlcpy(r->build, l + 8, sizeof r->build);
        else if (!strncmp(l, "vector: ", 8)) {
            char *e = strstr(l, "error: ");
            if (e) r->err = (unsigned)strtoul(e + 7, 0, 16);
        } else if (!strncmp(l, "rip: ", 5)) r->rip = strtoull(l + 5, 0, 16);
        else if (!strncmp(l, "rsp: ", 5)) {
            char *c = strstr(l, "cr2: ");
            if (c) r->cr2 = strtoull(c + 5, 0, 16);
        } else if (!strncmp(l, "map: ", 5) && nmaps < MAX_MAPS) {
            // "map: <kind> 0xA-0xB [prot N] [path]"
            struct map *m = &maps[nmaps];
            memset(m, 0, sizeof *m);
            char *p = l + 5, *sp = strchr(p, ' ');
            if (sp) {
                *sp = '\0';
                strlcpy(m->kind, p, sizeof m->kind);
                char *dash;
                m->a = strtoull(sp + 1, &dash, 16);
                if (*dash == '-') m->b = strtoull(dash + 1, &p, 16);
                char *pr = strstr(p, "prot ");
                if (pr) { pr += 5; while (*pr && *pr != ' ') pr++; p = pr; }
                while (*p == ' ') p++;
                strlcpy(m->path, p, sizeof m->path);
                nmaps++;
            }
        }
        l = nl ? nl + 1 : 0;
    }
    if (!r->program[0]) return 0;
    locate(r, maps, nmaps);
    friendly_name(r->program, r->name, sizeof r->name);
    struct stat st;
    if (stat(path, &st) == 0) { r->when = st.st_mtime; r->size = (unsigned)st.st_size; }
    return 1;
}

static int newest_first(const void *a, const void *b) {
    const struct report *x = a, *y = b;
    return x->when < y->when ? 1 : x->when > y->when ? -1 : 0;
}

static void scan(void) {
    g_n = 0;
    DIR *d = opendir(CRASH_DIR);
    if (!d) return;
    struct dirent *e;
    char path[96];
    while ((e = readdir(d)) && g_n < MAX_REPORTS) {
        if (!strstr(e->d_name, ".crash")) continue;
        snprintf(path, sizeof path, CRASH_DIR "/%s", e->d_name);
        if (parse_report(path, &g_rep[g_n])) g_n++;
    }
    closedir(d);
    qsort(g_rep, (size_t)g_n, sizeof g_rep[0], newest_first);
}

// "Today 12:24", "2 Oct 12:24".
static void when_text(time_t t, char *out, int cap) {
    if (!t) { strlcpy(out, "?", (size_t)cap); return; }
    struct tm then = *localtime(&t);
    time_t now = time(0);
    struct tm today = *localtime(&now);
    if (then.tm_year == today.tm_year && then.tm_yday == today.tm_yday)
        strftime(out, (size_t)cap, "Today %H:%M", &then);
    else
        strftime(out, (size_t)cap, "%e %b %H:%M", &then);
}

// One paragraph a person can paste into a bug report.
static void summary(const struct report *r, char *out, int cap) {
    char when[32];
    when_text(r->when, when, sizeof when);
    snprintf(out, (size_t)cap,
             "%s (%s, pid %d) crashed: %s\nwhere: %s\nrip 0x%llx  error 0x%x  cr2 0x%llx\n"
             "build: %s\nwhen: %s\nreport: %s\n",
             r->name, r->program, r->pid, r->fault, r->where, r->rip, r->err, r->cr2,
             r->build, when, r->path);
}

// --- shared drawing --------------------------------------------------

static void key_value(struct ugfx_surface *s, int x, int *y, int kw, int w,
                      const char *k, const char *v, const struct ugfx_font *vfont) {
    uint32_t dim = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
    ugfx_draw_string(s, x, *y, k, dim, UGFX_TRANSPARENT);
    const struct ugfx_font *was = ugfx_set_font(vfont);
    ugfx_draw_string_elided(s, x + kw, *y, w - kw, v, UTHEME_TEXT, UGFX_TRANSPARENT);
    ugfx_set_font(was);
    *y += ugfx_char_h() + ugfx_char_h() / 3;
}

static void caption(struct ugfx_surface *s, int x, int *y, const char *t, int role) {
    *y += ugfx_char_h() / 2;
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    ugfx_draw_string(s, x, *y, t, utheme_action(role), UGFX_TRANSPARENT);
    ugfx_set_font(was);
    *y += ugfx_char_h() + ugfx_char_h() / 4;
}

static void draw_details(struct ugfx_surface *s, const struct report *r, int x, int y, int w) {
    const struct ugfx_font *mono = ugfx_font_mono(UGFX_FONT_REGULAR);
    const struct ugfx_font *ui = ugfx_font_session(UGFX_FONT_REGULAR);
    int kw = ugfx_char_advance('n') * 9;
    char v[96], when[32];
    caption(s, x, &y, "WHAT HAPPENED", UTHEME_ACT_DANGER);
    if (r->cr2 || r->err) snprintf(v, sizeof v, "%s, error 0x%x", r->fault, r->err);
    else strlcpy(v, r->fault, sizeof v);
    key_value(s, x, &y, kw, w, "Fault", v, ui);
    if (r->cr2) { snprintf(v, sizeof v, "0x%llx", r->cr2); key_value(s, x, &y, kw, w, "Address", v, mono); }
    key_value(s, x, &y, kw, w, "Where", r->where, mono);
    when_text(r->when, when, sizeof when);
    key_value(s, x, &y, kw, w, "When", when, ui);
    caption(s, x, &y, "PROGRAM", UTHEME_ACT_NAV);
    key_value(s, x, &y, kw, w, "Path", r->program, mono);
    snprintf(v, sizeof v, "%d", r->pid);
    key_value(s, x, &y, kw, w, "Pid", v, ui);
    key_value(s, x, &y, kw, w, "Build", r->build, ui);
    caption(s, x, &y, "REPORT", UTHEME_ACT_VIEW);
    key_value(s, x, &y, kw, w, "File", r->path, mono);
    snprintf(v, sizeof v, "%u.%u KiB", r->size / 1024, (r->size % 1024) * 10 / 1024);
    key_value(s, x, &y, kw, w, "Size", v, ui);
}

// --- the list ----------------------------------------------------------

enum { ID_TABLE = 1, ID_TB, ID_PANEL };
// ONE NUMBER SPACE with the widget ids: a dialog button's id IS its
// command, so the two must not overlap (the table's id was CMD_OPEN, and
// every click on a row opened Notepad).
enum { CMD_OPEN = 100, CMD_COPY, CMD_DELETE, CMD_REOPEN, CMD_ALL, CMD_CLOSE };
enum { COL_PROGRAM, COL_WHEN, COL_WHAT, COL_COUNT };

static const struct uui_table_column COLUMNS[COL_COUNT] = {
    { "Program", 16, UUI_TALIGN_LEFT },
    { "When", 14, UUI_TALIGN_LEFT },
    { "What happened", 0, UUI_TALIGN_LEFT },
};
static const struct uui_toolbar_item TB[] = {
    { "tb-open",   "Open the report in Notepad", CMD_OPEN,   "Open report", 0, 0, UTHEME_ACT_NAV },
    { "tb-copy",   "Copy a summary",             CMD_COPY,   "Copy",        0, 0, UTHEME_ACT_EDIT },
    UUI_TOOLBAR_SEP,
    { "tb-delete", "Delete this report",         CMD_DELETE, "Delete",      0, 0, UTHEME_ACT_DANGER },
};

static struct uui_table g_table;
static struct uui_toolbar g_tb;
static struct uui_label g_panel;     // the details' room; drawn in on_draw_over
static struct uui_item g_left_items[2], g_items[2];
static struct uui_layout g_left, g_root;

static const struct report *selected(void) {
    if (g_dialog) return &g_one;
    int v = g_table.selected;
    if (v < 0) return 0;
    int row = uui_table_source_row(&g_table, v);
    return row >= 0 && row < g_n ? &g_rep[row] : 0;
}

static void cell(void *ctx, int row, int col, char *out, int cap) {
    (void)ctx;
    const struct report *r = &g_rep[row];
    if (col == COL_PROGRAM) strlcpy(out, r->name, (size_t)cap);
    else if (col == COL_WHEN) when_text(r->when, out, cap);
    else snprintf(out, (size_t)cap, "%s  in %s", r->fault, r->where);
}

// --- the dialog face -----------------------------------------------------

static struct uui_label g_body;      // the dialog's text room
static struct uui_button g_b_open, g_b_copy, g_b_all, g_b_close, g_b_reopen;
static struct uui_label g_gap;
static struct uui_item g_btn_items[6], g_d_items[2];
static struct uui_layout g_btns, g_droot;

static void command(struct uapp *a, int code) {
    const struct report *r = selected();
    char text[768];
    switch (code) {
    case CMD_OPEN:   if (r) uapp_spawn(a, "/bin/wm/apps/notepad", r->path); break;
    case CMD_COPY:   if (r) { summary(r, text, sizeof text); uclip_set_text(text, (int)strlen(text)); } break;
    case CMD_REOPEN: if (r) uapp_spawn(a, r->program, 0); uapp_quit(a, 0); return;
    case CMD_ALL:    uapp_spawn(a, "/bin/wm/apps/crashreports", 0); uapp_quit(a, 0); return;
    case CMD_CLOSE:  uapp_quit(a, 0); return;
    case CMD_DELETE:
        if (r && !g_dialog) {
            unlink(r->path);
            scan();
            uui_table_set_rows(&g_table, g_n);
        }
        break;
    }
    uapp_redraw(a);
}

static void on_widget(struct uapp *a, int id, int reason) {
    (void)reason;
    if (id == ID_TB) { int c = uui_toolbar_take_code(&g_tb); if (c > 0) command(a, c); return; }
    if (id >= CMD_OPEN && id <= CMD_CLOSE) { command(a, id); return; }
    uapp_redraw(a);
}

static void on_draw_over(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    struct ugfx_surface *s = uapp_surface(d);
    int pad = ugfx_char_w();
    if (g_dialog) {
        const struct report *r = &g_one;
        int x = g_body.x + 2 * pad, y = g_body.y + pad, w = g_body.w - 4 * pad;
        char t[96];
        snprintf(t, sizeof t, "%s quit unexpectedly", r->name);
        const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
        ugfx_draw_string_elided(s, x, y, w, t, UTHEME_TEXT, UGFX_TRANSPARENT);
        ugfx_set_font(was);
        y += ugfx_char_h() + ugfx_char_h() / 2;
        if (!strncmp(r->fault, "Killed by ", 10))
            snprintf(t, sizeof t, "It was killed by %s. Other windows are not affected.", r->fault + 10);
        else {
            snprintf(t, sizeof t, "A %s stopped it. Other windows are not affected.", r->fault);
            if (t[2] >= 'A' && t[2] <= 'Z') t[2] = (char)(t[2] + 32);
        }
        ugfx_draw_string_elided(s, x, y, w, t, UTHEME_TEXT, UGFX_TRANSPARENT);
        y += ugfx_char_h();
        draw_details(s, r, x, y, w);
        return;
    }
    // The details panel: a step lighter than the window, a hairline left.
    ugfx_fill_rect(s, g_panel.x, g_panel.y, g_panel.w, g_panel.h, UTHEME_PANEL_BG);
    ugfx_fill_rect(s, g_panel.x, g_panel.y, 1, g_panel.h, UTHEME_SEPARATOR);
    const struct report *r = selected();
    int x = g_panel.x + 2 * pad, y = g_panel.y + pad, w = g_panel.w - 3 * pad;
    if (!r) {
        ugfx_draw_string(s, x, y, g_n ? "Pick a report." : "Nothing has crashed.",
                         uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED), UGFX_TRANSPARENT);
        return;
    }
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    ugfx_draw_string_elided(s, x, y, w, r->name, UTHEME_TEXT, UGFX_TRANSPARENT);
    ugfx_set_font(was);
    draw_details(s, r, x, y + ugfx_char_h(), w);
}

// The keyboard: the table in the list; the buttons in the dialog, Reopen
// first. Esc closes the dialog, as any dialog's does.
static struct uui_focusable g_list_focus[1];
static struct uui_focusable g_dlg_focus[5];
static struct uui_focus g_focus;

static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    if (g_dialog && key == 0x1B) uapp_quit(a, 0);
}

// The details panel's width is set here, not in main(): before uapp_run()
// loads the font, ugfx_char_advance() is 0 and the panel lays out 0 wide.
static void list_size(int *w, int *h) {
    g_items[1].main_size = ugfx_char_advance('n') * 40;
    *w = ugfx_char_advance('n') * 120; *h = ugfx_char_h() * 30;
}
static void dialog_size(int *w, int *h) { *w = ugfx_char_advance('n') * 74; *h = ugfx_char_h() * 25; }

static void button(struct uui_button *b, const char *label, int code, int primary, int i) {
    uui_button_init(b, 0, 0, 0, 0, label, primary ? UTHEME_ACCENT : UTHEME_BUTTON_BG,
                    primary ? UTHEME_ACCENT_TEXT : UTHEME_TEXT, code);
    g_btn_items[i] = (struct uui_item){ .ops = &uui_button_ops, .widget = b, .id = code,
                                        .name = label };
}

int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "--report")) {
        if (!parse_report(argv[2], &g_one)) {
            fprintf(stderr, "crashreports: cannot read %s\n", argv[2]);
            return 1;
        }
        g_dialog = 1;
        uui_label_init(&g_body, "");
        uui_label_init(&g_gap, "");
        button(&g_b_open, "Open report", CMD_OPEN, 0, 0);
        button(&g_b_copy, "Copy", CMD_COPY, 0, 1);
        button(&g_b_all, "All reports", CMD_ALL, 0, 2);
        g_btn_items[3] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_gap, .flags = UUI_FILL_W };
        button(&g_b_close, "Close", CMD_CLOSE, 0, 4);
        button(&g_b_reopen, "Reopen", CMD_REOPEN, 1, 5);
        g_btns = (struct uui_layout){ .dir = UUI_ROW, .items = g_btn_items, .count = 6, .margin = 10 };
        g_d_items[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_body,
                                          .flags = UUI_FILL_W | UUI_FILL_H, .name = "body" };
        g_d_items[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_btns, .flags = UUI_FILL_W };
        g_droot = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_d_items, .count = 2 };
        char title[64];
        snprintf(title, sizeof title, "%s crashed", g_one.name);
        struct uui_button *order[5] = { &g_b_reopen, &g_b_close, &g_b_all, &g_b_copy, &g_b_open };
        for (int i = 0; i < 5; i++) g_dlg_focus[i] = (struct uui_focusable){ order[i], &uui_button_ops };
        uui_focus_init(&g_focus, g_dlg_focus, 5);
        uui_focus_set(&g_focus, 0);
        struct uapp_desc desc = {
            // ITS OWN app id: the WM remembers a window's size per id, and
            // sharing the list's made the list open dialog-sized.
            .title = title, .app_id = "crashreport", .on_size = dialog_size,
            .layout = &g_droot, .widgets = g_d_items, .widget_count = 2,
            .on_widget = on_widget, .on_draw_over = on_draw_over,
            .focus = &g_focus, .on_key = on_key,
        };
        return uapp_run(&desc);
    }

    scan();
    uui_table_init(&g_table, 0, 0, 100, 100, COLUMNS, COL_COUNT, cell, 0);
    uui_table_set_rows(&g_table, g_n);
    if (g_n) g_table.selected = 0;
    uui_toolbar_init(&g_tb, TB, (int)(sizeof TB / sizeof TB[0]));
    uui_label_init(&g_panel, "");
    g_left_items[0] = (struct uui_item){ .ops = &uui_toolbar_ops, .widget = &g_tb, .id = ID_TB,
                                         .flags = UUI_FILL_W, .name = "tb" };
    g_left_items[1] = (struct uui_item){ .ops = &uui_table_ops, .widget = &g_table, .id = ID_TABLE,
                                         .flags = UUI_FILL_W | UUI_FILL_H, .name = "reports" };
    // EDGE TO EDGE: the command bar, the table and the details panel meet
    // the window and each other, as the app design language's bars do
    // (docs/gui-guidelines.md). 1, because 0 means "the default".
    g_left = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_left_items, .count = 2, .gap = 1 };
    g_items[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_left,
                                    .flags = UUI_FILL_W | UUI_FILL_H };
    g_items[1] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_panel, .id = ID_PANEL,
                                    .flags = UUI_FILL_H, .name = "details" };
    g_root = (struct uui_layout){ .dir = UUI_ROW, .items = g_items, .count = 2, .margin = 1, .gap = 1 };
    g_list_focus[0] = (struct uui_focusable){ &g_table, &uui_table_ops };
    uui_focus_init(&g_focus, g_list_focus, 1);
    uui_focus_set(&g_focus, 0);
    struct uapp_desc desc = {
        .title = "Crash Reports", .app_id = "crashreports", .on_size = list_size,
        .min_w = 640, .min_h = 320,
        .focus = &g_focus, .on_key = on_key,
        .layout = &g_root, .widgets = g_items, .widget_count = 2,
        .on_widget = on_widget, .on_draw_over = on_draw_over, .flags = UAPP_RESIZABLE,
    };
    return uapp_run(&desc);
}
