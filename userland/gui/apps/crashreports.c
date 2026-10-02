// Crash Reports -- the ring-3 crash reports the kernel writes to
// /var/crash (kernel/include/kernel/crash_report.h), newest first, with
// one report's details beside the list.
//
// THREE FACES, ONE PROGRAM. With no argument it is the list -- Windows'
// Reliability Monitor, GNOME's ABRT window. With `--report <path>` it is
// the dialog the desktop's crash notice opens for one crash ("Notepad
// quit unexpectedly"), macOS's and KDE DrKonqi's shape; "All reports"
// there opens the list. With a report's PATH it is the VIEWER: the whole
// report as one document -- what happened, the backtrace, registers,
// memory map and the kernel log -- macOS's crash report layout; "Open
// report" in the other two faces opens it, and so does a .crash file.
//
// **IT READS THE REPORT, NOT THE KERNEL** (lib/ucrash.h), so a report
// from a previous boot reads the same as this boot's. The backtrace is
// recovered from the saved stack and named from the binaries on disk.
#include "ui/uapp.h"
#include "ui/uui.h"
#include "ui/uui_table.h"
#include "ui/uui_toolbar.h"
#include "ui/uui_button.h"
#include "ui/uui_label.h"
#include "ui/uui_focus.h"
#include "ui/uui_markdown.h"
#include "ui/ulog.h"
#include "ui/utheme.h"
#include "ui/ugfx.h"
#include "lib/ufile.h"
#include "lib/uappentry.h"
#include "lib/uclip.h"
#include "lib/ucrash.h"
#include "rt/sys.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#define CRASH_DIR   "/var/crash"
#define MAX_REPORTS 64

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
    int can_reopen;       // a desktop app; a daemon is init's to restart
};

static struct report g_rep[MAX_REPORTS];
static int g_n;
static struct report g_one;      // --report's
static int g_dialog;             // 1 = the one-report face

// --- what the desktop entries call a program -------------------------

// The entry's Name= for the program, else its file name. 1 when it has
// an entry: only then is it a program a person starts, and so one
// Reopen may start. toywm, fontd and telnetd are init's services -- a
// second toywm evicts the live desktop, and init restarts it anyway
// (the desktop's crash notice draws the same line, crash_notice.c).
static int friendly_name(const char *exec, char *out, int cap) {
    struct uappentry e;
    if (uappentry_find_exec(exec, &e)) { strlcpy(out, e.name, (size_t)cap); return 1; }
    const char *base = strrchr(exec, '/');
    strlcpy(out, base ? base + 1 : exec, (size_t)cap);
    return 0;
}

// --- reading one report ----------------------------------------------

// The list's row: lib/ucrash's reading, reduced to what a row and the
// details panel show. No backtrace here -- that reads the binaries, and
// the list holds sixty-four reports.
static int parse_report(const char *path, struct report *r) {
    static struct ucrash c;
    if (ucrash_load(&c, path) != 0) return 0;
    memset(r, 0, sizeof *r);
    strlcpy(r->path, c.path, sizeof r->path);
    strlcpy(r->file, c.file, sizeof r->file);
    strlcpy(r->program, c.program, sizeof r->program);
    strlcpy(r->fault, c.fault, sizeof r->fault);
    strlcpy(r->build, c.kernel, sizeof r->build);
    r->pid = c.pid;
    r->rip = c.rip;
    r->cr2 = c.vector == 14 ? c.cr2 : 0;
    r->err = (unsigned)c.err;
    r->when = c.when;
    r->size = c.size;
    ucrash_where(&c, r->where, sizeof r->where);
    r->can_reopen = friendly_name(r->program, r->name, sizeof r->name);
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
enum { CMD_OPEN = 100, CMD_COPY, CMD_DELETE, CMD_REOPEN, CMD_ALL, CMD_CLOSE, CMD_NOTEPAD, CMD_SHOW };
enum { COL_PROGRAM, COL_WHEN, COL_WHAT, COL_COUNT };

static const struct uui_table_column COLUMNS[COL_COUNT] = {
    { "Program", 16, UUI_TALIGN_LEFT },
    { "When", 14, UUI_TALIGN_LEFT },
    { "What happened", 0, UUI_TALIGN_LEFT },
};
static const struct uui_toolbar_item TB[] = {
    { "tb-open",   "Read the report",            CMD_OPEN,   "Open report", 0, 0, UTHEME_ACT_NAV },
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
    case CMD_OPEN:   if (r) uapp_spawn(a, "/bin/wm/apps/crashreports", r->path); break;
    case CMD_NOTEPAD: if (r) uapp_spawn(a, "/bin/wm/apps/notepad", r->path); break;
    case CMD_SHOW:   if (r) uapp_spawn(a, "/bin/wm/apps/files", r->path); break;
    case CMD_COPY:   if (r) { summary(r, text, sizeof text); uclip_set_text(text, (int)strlen(text)); } break;
    case CMD_REOPEN:
        if (!r || !r->can_reopen) break;
        uapp_spawn(a, r->program, 0);
        uapp_quit(a, 0);
        return;
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

// The dialog's buttons; a button's code is its command.
static void on_action(struct uapp *a, int code) { command(a, code); }

static void on_widget(struct uapp *a, int id, int reason) {
    if (id == ID_TB) { int c = uui_toolbar_take_code(&g_tb); if (c > 0) command(a, c); return; }
    // A double-click on a row opens it, as in Reliability Monitor: two
    // releases on the same row within Windows' 400 ms.
    if (id == ID_TABLE && reason == UUI_REASON_RELEASE) {
        static unsigned long long last;
        static int last_row = -1;
        unsigned long long now = sys_monotonic_ns();
        if (g_table.selected >= 0 && g_table.selected == last_row && now - last < 400000000ULL) {
            last_row = -1;
            command(a, CMD_OPEN);
            return;
        }
        last = now;
        last_row = g_table.selected;
    }
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

// The keyboard: the table in the list; the buttons in the dialog, Reopen (if any)
// first. Esc closes the dialog, as any dialog's does.
static struct uui_focusable g_list_focus[1];
static struct uui_focusable g_dlg_focus[5];
static struct uui_focus g_focus;

static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    if (g_dialog && key == 0x1B) uapp_quit(a, 0);
    else if (!g_dialog && (key == '\n' || key == '\r')) command(a, CMD_OPEN);
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

// --- the viewer face ---------------------------------------------------
//
// THE WHOLE REPORT AS ONE MARKDOWN DOCUMENT, drawn by uui_markdown: the
// sections are headings and the tables are tables, so the page is one
// scroll and the layout is the widget's. Rebuilt only when "Show all"
// toggles the kernel log.

enum { ID_DOC = 3, ID_VTB };
static struct ucrash g_cr;
static struct uui_markdown g_doc;
static struct uui_toolbar g_vtb;
static struct uui_item g_v_items[2];
static struct uui_layout g_vroot;
static char g_md[40000];
static int g_md_len, g_full_log;

static const struct uui_toolbar_item VTB[] = {
    { "tb-open",    "Open the file as text",    CMD_NOTEPAD, "Open in Notepad", 0, 0, UTHEME_ACT_NAV },
    { "tb-copy",    "Copy a summary",           CMD_COPY,    "Copy summary",    0, 0, UTHEME_ACT_EDIT },
    UUI_TOOLBAR_SEP,
    { "tb-refresh", "Start the program again",  CMD_REOPEN,  "Reopen",          0, 0, UTHEME_ACT_CREATE },
    { "tb-details", "Show the report in Files", CMD_SHOW,    "Show in Files",   0, 0, UTHEME_ACT_VIEW },
};
#define LOG_ALL  "show-all-lines"   // ONE word: a link is an inline-code word (uui_markdown.h)
#define LOG_FEW  "show-its-lines"

static unsigned view_flags(int code) {
    return code == CMD_REOPEN && !g_one.can_reopen ? UUI_MI_DISABLED : 0;
}

static void md(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void md(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int room = (int)sizeof g_md - g_md_len;
    int n = room > 0 ? vsnprintf(g_md + g_md_len, (size_t)room, fmt, ap) : 0;
    va_end(ap);
    // A line that does not fit is left out whole, never cut mid-table.
    if (n > 0 && n < room) g_md_len += n;
    else if (room > 0) g_md[g_md_len] = '\0';
}

static const char *prot_word(unsigned p) {
    static const char *const w[8] = { "---", "r--", "-w-", "rw-", "--x", "r-x", "-wx", "rwx" };
    return w[p & 7];
}

// Which frame, if any, lies in a mapping: the map is annotated with it.
static int frame_in(const struct ucrash_map *m) {
    for (int i = 0; i < g_cr.nframe; i++)
        if (g_cr.frame[i].addr >= m->a && g_cr.frame[i].addr < m->b) return i;
    return -1;
}

static void build_doc(void) {
    const struct ucrash *r = &g_cr;
    char t[192], when[32];
    g_md_len = 0;
    g_md[0] = '\0';
    md("# %s quit unexpectedly\n\n", g_one.name);
    ucrash_explain(r, t, sizeof t);
    md("%s Other windows are not affected.\n\n", t);
    ucrash_where(r, t, sizeof t);
    md("- **Where:** `%s`\n", t);
    if (r->nframe > 1) {
        md("- **Called from:**");
        for (int i = 1; i < r->nframe && i < 4; i++) {
            const struct ucrash_frame *f = &r->frame[i];
            if (f->func[0]) md("%s `%s`", i > 1 ? " <-" : "", f->func);
            else md("%s `%s +0x%llx`", i > 1 ? " <-" : "", f->module, (unsigned long long)f->off);
        }
        md("\n");
    }
    when_text(r->when, when, sizeof when);
    md("- **When:** %s\n", when);
    md("- **Program:** `%s`, pid %d\n", r->program, r->pid);
    md("- **Build:** %s\n\n", r->kernel);

    md("## Backtrace\n\n");
    if (!r->nframe) {
        md("No code address was found on the saved stack.\n\n");
    } else {
        md("| # | Function | Module | Address |\n|---|---|---|---|\n");
        for (int i = 0; i < r->nframe; i++) {
            const struct ucrash_frame *f = &r->frame[i];
            if (f->func[0])
                md("| %d | %s`%s +0x%llx`%s | %s | `0x%llx` |\n", i, i ? "" : "**", f->func,
                   (unsigned long long)f->off, i ? "" : "**", f->module, (unsigned long long)f->addr);
            else
                md("| %d | (no symbol) `+0x%llx` | %s | `0x%llx` |\n", i, (unsigned long long)f->off,
                   f->module, (unsigned long long)f->addr);
        }
        md("\nFound by scanning the stack: each address follows a call instruction, "
           "and is named from that file's symbol table.");
        if (r->stale) md(" **A file was replaced after this crash, so some names may be wrong.**");
        md("\n\n");
    }

    md("## Registers\n\n```\n");
    md("   rip %-18llx rsp %-18llx rflags %llx\n", (unsigned long long)r->rip,
       (unsigned long long)r->rsp, (unsigned long long)r->rflags);
    // rax first, as a person reads them; the report's order is r15..rax.
    for (int i = 14, k = 0; i >= 0; i--, k++)
        md("%6s %-18llx%s", ucrash_reg_names[i], (unsigned long long)r->reg[i], k % 3 == 2 ? "\n" : "");
    md("\n    cs %-18llx  ss %-18llx  cr2 %llx\n```\n\n", (unsigned long long)r->cs,
       (unsigned long long)r->ss, (unsigned long long)r->cr2);

    md("## Memory map\n\n| Start | End | Kind | Prot | What |\n|---|---|---|---|---|\n");
    for (int i = 0; i < r->nmap; i++) {
        const struct ucrash_map *m = &r->map[i];
        int f = frame_in(m);
        const char *b = strrchr(m->path, '/');
        char what[96];
        if (f >= 0) snprintf(what, sizeof what, "**%s** -- frame %d", m->path[0] ? (b ? b + 1 : m->path) : m->kind, f);
        else snprintf(what, sizeof what, "%s", m->path[0] ? (b ? b + 1 : m->path) : "");
        md("| `%llx` | `%llx` | %s | `%s` | %s |\n", (unsigned long long)m->a,
           (unsigned long long)m->b, m->kind, prot_word(m->prot), what);
    }
    md("\n## Kernel log\n\n");
    static char mine[UCRASH_LOG];
    int n = ucrash_log_lines(r, mine, sizeof mine);
    const char *b = strrchr(r->program, '/');
    if (g_full_log) {
        // A fence closes only on a line of its own: the log's tail need
        // not end in a newline.
        size_t ll = strlen(r->log);
        md("The last lines the kernel logged before the crash:\n\n```\n%s%s```\n\n`%s`\n", r->log,
           ll && r->log[ll - 1] != '\n' ? "\n" : "", LOG_FEW);
    } else if (n) {
        md("The lines about %s (pid %d):\n\n```\n%s```\n\n`%s`\n", b ? b + 1 : r->program, r->pid, mine, LOG_ALL);
    } else {
        md("No line names %s or pid %d.\n\n`%s`\n", b ? b + 1 : r->program, r->pid, LOG_ALL);
    }
    uui_markdown_set_text(&g_doc, g_md, g_md_len);
}

static int log_link(void *ctx, const char *word) {
    (void)ctx;
    return !strcmp(word, LOG_ALL) || !strcmp(word, LOG_FEW);
}

static void view_widget(struct uapp *a, int id, int reason) {
    (void)reason;
    char link[48];
    if (id == ID_VTB) { int c = uui_toolbar_take_code(&g_vtb); if (c > 0) command(a, c); return; }
    if (id == ID_DOC && uui_markdown_take_link(&g_doc, link, sizeof link)) {
        g_full_log = !strcmp(link, LOG_ALL);
        int keep = g_doc.scroll;
        build_doc();
        g_doc.scroll = keep;   // the text changed, so set_text went to the top
    }
    uapp_redraw(a);
}

static void view_size(int *w, int *h) { *w = ugfx_char_advance('n') * 100; *h = ugfx_char_h() * 42; }

static int view_main(const char *path) {
    if (!parse_report(path, &g_one) || ucrash_load(&g_cr, path) != 0) {
        fprintf(stderr, "crashreports: %s is not a crash report\n", path);
        return 1;
    }
    g_dialog = 2;
    ucrash_backtrace(&g_cr);
    // What a test reads: the frames as this program named them.
    ulogf("crashreports: view %s frames %d stale %d reopen %d\n", g_cr.file, g_cr.nframe, g_cr.stale,
          g_one.can_reopen);
    for (int i = 0; i < g_cr.nframe; i++)
        ulogf("crashreports: frame %d 0x%llx %s %s +0x%llx\n", i, (unsigned long long)g_cr.frame[i].addr,
              g_cr.frame[i].module, g_cr.frame[i].func[0] ? g_cr.frame[i].func : "-",
              (unsigned long long)g_cr.frame[i].off);
    uui_markdown_init(&g_doc);
    uui_markdown_set_links(&g_doc, log_link, 0);
    build_doc();
    uui_toolbar_init(&g_vtb, VTB, (int)(sizeof VTB / sizeof VTB[0]));
    g_vtb.item_flags = view_flags;
    g_v_items[0] = (struct uui_item){ .ops = &uui_toolbar_ops, .widget = &g_vtb, .id = ID_VTB,
                                      .flags = UUI_FILL_W, .name = "tb" };
    g_v_items[1] = (struct uui_item){ .ops = &uui_markdown_ops, .widget = &g_doc, .id = ID_DOC,
                                      .flags = UUI_FILL_W | UUI_FILL_H, .name = "report" };
    g_vroot = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_v_items, .count = 2, .margin = 1, .gap = 1 };
    static char title[96];
    snprintf(title, sizeof title, "%s - Crash Report", g_cr.file);
    struct uapp_desc desc = {
        .title = title, .app_id = "crashview", .on_size = view_size,
        .min_w = 480, .min_h = 320,
        .layout = &g_vroot, .widgets = g_v_items, .widget_count = 2,
        .on_widget = view_widget, .flags = UAPP_RESIZABLE,
    };
    return uapp_run(&desc);
}

int main(int argc, char **argv) {
    // A report's path alone is the viewer -- what `open` and the File
    // Manager pass for a .crash file (Handles=, uopen.h).
    if (argc == 2 && argv[1][0] == '/') return view_main(argv[1]);
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
        // No Reopen for a service: Close is then the default button.
        int reopen = g_one.can_reopen;
        button(&g_b_close, "Close", CMD_CLOSE, !reopen, 4);
        button(&g_b_reopen, "Reopen", CMD_REOPEN, 1, 5);
        g_btns = (struct uui_layout){ .dir = UUI_ROW, .items = g_btn_items, .count = reopen ? 6 : 5,
                                      .margin = 10 };
        g_d_items[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_body,
                                          .flags = UUI_FILL_W | UUI_FILL_H, .name = "body" };
        g_d_items[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_btns, .flags = UUI_FILL_W };
        g_droot = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_d_items, .count = 2 };
        char title[64];
        snprintf(title, sizeof title, "%s crashed", g_one.name);
        struct uui_button *order[5] = { &g_b_reopen, &g_b_close, &g_b_all, &g_b_copy, &g_b_open };
        int first = reopen ? 0 : 1;
        for (int i = first; i < 5; i++)
            g_dlg_focus[i - first] = (struct uui_focusable){ order[i], &uui_button_ops };
        uui_focus_init(&g_focus, g_dlg_focus, 5 - first);
        uui_focus_set(&g_focus, 0);
        struct uapp_desc desc = {
            // ITS OWN app id: the WM remembers a window's size per id, and
            // sharing the list's made the list open dialog-sized.
            .title = title, .app_id = "crashreport", .on_size = dialog_size,
            .layout = &g_droot, .widgets = g_d_items, .widget_count = 2,
            .on_widget = on_widget, .on_action = on_action, .on_draw_over = on_draw_over,
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
