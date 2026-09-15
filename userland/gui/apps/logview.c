// Log Viewer -- the kernel ring and the application ring, in one table.
//
// **THE TWO RINGS ARE SEPARATE ON PURPOSE AND READ TOGETHER HERE.**
// klog stores BYTES (a kernel line is already prefixed with its
// subsystem) and applog stores RECORDS carrying a tag (application
// output's useful fact is WHO said it); they are separate so a chatty
// program cannot flush kernel evidence, which this project has paid
// for. api/applog.h stamps both with the same `cs` from `pit_ticks()`
// "so a merged log of both sources can be read in order" -- this is the
// reader that does.
//
// **THE MERGE IS THE TABLE'S SORT.** Both rings go into one array and
// the Time column sorts it; there is no merge loop here, and none of
// the "which ring is further ahead" bookkeeping a hand-written one
// needs. That is also why clicking another column just works.
//
// A LEVEL IS TEXT, in the stamp the kernel wrote (`[7.03] <3> `), which
// is what lets this filter and tint by severity without a new ABI. An
// application line has no level: a program declares none, so it is
// never filtered out by a level choice -- the same rule /bin/log keeps.
#include "ui/uapp.h"
#include "ui/uui.h"
#include "ui/uui_table.h"
#include "ui/uui_textbox.h"
#include "ui/uui_dropdown.h"
#include "ui/uui_label.h"
#include "ui/uui_focus.h"
#include "rt/sys.h"
#include "query_abi.h"
#include "applog.h"
#include <stdio.h>
#include <string.h>

// Bounded, and the newest win: a ring that has wrapped has already lost
// its oldest, so holding more than it retains buys nothing.
#define MAX_LINES 400
#define TEXT_MAX  152
#define SRC_MAX   16

struct line {
    unsigned cs;               // hundredths of a second since boot
    // **DID THIS LINE CARRY A STAMP?** The ring WRAPS, so its oldest
    // retained bytes begin mid-line and the first thing read is a
    // headless fragment. Giving that cs 0 and printing "0.00" says it
    // happened at boot, which is a lie a Time column tells confidently.
    // It sorts first either way -- it IS the oldest thing retained --
    // but the cell is left empty, the same honesty the Level column
    // already has for a line that declares none.
    int      stamped;
    int      level;            // 0..7, or -1 for a line that declares none
    char     source[SRC_MAX];  // "kernel", or the writing program's tag
    char     text[TEXT_MAX];
};

static struct line g_lines[MAX_LINES];
static int         g_count;
static int         g_view[MAX_LINES];   // indices of g_lines that pass the filter
static int         g_view_count;

enum { ID_TABLE = 1, ID_LEVEL, ID_SEARCH };
enum { COL_TIME = 0, COL_SOURCE, COL_LEVEL, COL_TEXT };

static const struct uui_table_column COLUMNS[] = {
    { "Time",   10, UUI_TALIGN_RIGHT },
    { "Source",  9, UUI_TALIGN_LEFT  },
    { "Level",   7, UUI_TALIGN_LEFT  },
    { "Message", 0, UUI_TALIGN_LEFT  },   // stretches with the window
};
#define COL_COUNT ((int)(sizeof COLUMNS / sizeof COLUMNS[0]))

// Ordered by severity, so the choice means "this and worse" exactly as
// `dmesg -l` does. The numbers are Linux's, via api/klog.h.
static const char *const LEVEL_NAMES[] = { "All", "Debug", "Info", "Warnings", "Errors" };
static const int LEVEL_MAX[] = { 7, 7, 6, 4, 3 };
#define LEVEL_COUNT ((int)(sizeof LEVEL_NAMES / sizeof LEVEL_NAMES[0]))

static struct uui_table    g_table;
static struct uui_dropdown g_level;
static struct uui_textbox  g_search;
static struct uui_label    g_level_label, g_search_label;

static const char *level_name(int level) {
    switch (level) {
    case 2: return "crit";
    case 3: return "err";
    case 4: return "warn";
    case 6: return "info";
    case 7: return "debug";
    default: return "";
    }
}

// --- reading the rings ------------------------------------------------

static struct line *next_line(void) {
    if (g_count < MAX_LINES) return &g_lines[g_count++];
    // Full: drop the oldest and keep the newest, which is what a person
    // looking at a log wants when it no longer fits.
    memmove(&g_lines[0], &g_lines[1], sizeof g_lines[0] * (MAX_LINES - 1));
    return &g_lines[MAX_LINES - 1];
}

// "[7.03] <3> usb: ..." -> cs, level, and the text after both.
static void add_kernel_line(const char *s, int len) {
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) len--;
    if (len <= 0) return;

    unsigned secs = 0, hund = 0;
    int i = 0, level = -1, stamped = 0;
    if (s[0] == '[') {
        i = 1;
        while (i < len && s[i] >= '0' && s[i] <= '9') secs = secs * 10 + (unsigned)(s[i++] - '0');
        if (i + 4 <= len && s[i] == '.' && s[i + 3] == ']' && s[i + 4] == ' ') {
            hund = (unsigned)(s[i + 1] - '0') * 10 + (unsigned)(s[i + 2] - '0');
            i += 5;
            stamped = 1;
        } else {
            i = 0;   // not a stamp after all: the whole thing is the message
        }
    }
    if (i + 3 < len && s[i] == '<' && s[i + 2] == '>' && s[i + 3] == ' ' &&
        s[i + 1] >= '0' && s[i + 1] <= '7') {
        level = s[i + 1] - '0';
        i += 4;
    }

    struct line *l = next_line();
    l->cs = secs * 100 + hund;
    l->stamped = stamped;
    l->level = level;
    snprintf(l->source, sizeof l->source, "kernel");
    int n = len - i;
    if (n > (int)sizeof l->text - 1) n = (int)sizeof l->text - 1;
    memcpy(l->text, s + i, (unsigned)n);
    l->text[n] = 0;
}

static void read_klog(void) {
    struct query_klog r;
    static char acc[512];          // a line spans slices; this reassembles
    int acc_len = 0;
    for (int i = 0; ; i++) {
        if (sys_query_record(QUERY_KLOG, (unsigned)i, &r, sizeof r) <= 0) break;
        for (unsigned j = 0; j < r.len; j++) {
            char c = (char)r.data[j];
            if (c == '\n') {
                add_kernel_line(acc, acc_len);
                acc_len = 0;
            } else if (acc_len < (int)sizeof acc) {
                acc[acc_len++] = c;
            }
        }
    }
    if (acc_len) add_kernel_line(acc, acc_len);
}

static void read_applog(void) {
    struct query_applog a;
    if (sys_query_record(QUERY_APPLOG, 0, &a, sizeof a) <= 0) return;
    unsigned long long total = a.total, oldest = a.oldest;
    if (total < oldest) return;

    // ONE WRITE IS ONE RECORD and a line is often several (api/applog.h),
    // so fragments are joined until one says it ended a line. Joined by
    // TAG rather than in arrival order: two programs writing at once
    // interleave, and concatenating that blindly would splice one
    // program's half-line onto another's.
    struct { char tag[SRC_MAX]; char text[TEXT_MAX]; int len; unsigned cs; } frag[6];
    memset(frag, 0, sizeof frag);

    for (unsigned long long seq = oldest; seq <= total; seq++) {
        unsigned idx = (unsigned)(seq - oldest);
        if (sys_query_record(QUERY_APPLOG, idx, &a, sizeof a) <= 0) break;

        int f = -1, free_slot = -1;
        for (int k = 0; k < (int)(sizeof frag / sizeof frag[0]); k++) {
            if (frag[k].len && strcmp(frag[k].tag, a.tag) == 0) { f = k; break; }
            if (!frag[k].len && free_slot < 0) free_slot = k;
        }
        if (f < 0) f = free_slot >= 0 ? free_slot : 0;
        if (!frag[f].len) {
            snprintf(frag[f].tag, sizeof frag[f].tag, "%s", a.tag);
            frag[f].cs = (unsigned)a.cs;
        }
        int room = (int)sizeof frag[f].text - 1 - frag[f].len;
        int take = (int)a.len < room ? (int)a.len : room;
        if (take > 0) {
            memcpy(frag[f].text + frag[f].len, a.text, (unsigned)take);
            frag[f].len += take;
        }
        if (a.eol || take < (int)a.len) {
            frag[f].text[frag[f].len] = 0;
            struct line *l = next_line();
            l->cs = frag[f].cs;
            l->stamped = 1;           // a record carries its own cs
            l->level = -1;            // a program declares none
            snprintf(l->source, sizeof l->source, "%s", frag[f].tag);
            snprintf(l->text, sizeof l->text, "%s", frag[f].text);
            frag[f].len = 0;
        }
    }
}

// --- the filter -------------------------------------------------------

static void rebuild_view(void) {
    int want = LEVEL_MAX[uui_dropdown_selected(&g_level)];
    const char *needle = uui_textbox_text(&g_search);
    g_view_count = 0;
    for (int i = 0; i < g_count; i++) {
        // A LINE WITH NO LEVEL IS NEVER FILTERED OUT by a level choice:
        // an application line declares none, and hiding every service's
        // output behind "Errors" would be worse than showing too much.
        if (g_lines[i].level >= 0 && g_lines[i].level > want) continue;
        if (needle && needle[0] && !strstr(g_lines[i].text, needle) &&
            !strstr(g_lines[i].source, needle)) continue;
        g_view[g_view_count++] = i;
    }
    uui_table_set_rows(&g_table, g_view_count);
}

static void reload(void) {
    g_count = 0;
    read_klog();
    read_applog();
    rebuild_view();
}

// --- the table's callbacks -------------------------------------------

static void cell(void *ctx, int row, int col, char *out, int cap) {
    (void)ctx;
    if (row < 0 || row >= g_view_count) { out[0] = 0; return; }
    const struct line *l = &g_lines[g_view[row]];
    switch (col) {
    case COL_TIME:
        if (l->stamped) snprintf(out, (unsigned)cap, "%u.%02u", l->cs / 100, l->cs % 100);
        else            out[0] = 0;   // a fragment the ring wrapped into
        break;
    case COL_SOURCE: snprintf(out, (unsigned)cap, "%s", l->source); break;
    case COL_LEVEL:  snprintf(out, (unsigned)cap, "%s", level_name(l->level)); break;
    default:         snprintf(out, (unsigned)cap, "%s", l->text); break;
    }
}

// Sorted on the REAL values, not the formatted cells -- "10" sorts
// before "9" as text, and the Time column is the one that matters here
// because sorting it IS the merge of the two rings.
static int compare_rows(void *ctx, int a, int b, int col) {
    (void)ctx;
    const struct line *x = &g_lines[g_view[a]], *y = &g_lines[g_view[b]];
    switch (col) {
    case COL_TIME:   return x->cs < y->cs ? -1 : x->cs > y->cs ? 1 : 0;
    case COL_SOURCE: return strcmp(x->source, y->source);
    case COL_LEVEL:  return x->level - y->level;
    default:         return strcmp(x->text, y->text);
    }
}

// **NO ROW TINT, AND THAT IS A DECISION.** A severity wants to be
// visible at a glance, but the theme has no error or warning role and
// docs/gui-guidelines.md is explicit that colours come from the theme
// and are never hand-picked -- a red this file chose would be wrong in
// the next palette and invisible in a dark one. The Level column says
// the same thing in words, which is the guidelines' own preference for
// shape over colour. Revisit if the theme ever grows the roles.

// --- the app ----------------------------------------------------------

static struct uui_item   g_form_items[4];
static struct uui_layout g_form;
static struct uui_item   g_items[2];
static struct uui_layout g_root;

static struct uui_focusable g_focusables[] = {
    { &g_level,  &uui_dropdown_ops },
    { &g_search, &uui_textbox_focus_ops },
    { &g_table,  &uui_table_ops },
};
static struct uui_focus g_focus;

static void on_widget(struct uapp *a, int id, int reason) {
    (void)reason;
    if (id == ID_LEVEL || id == ID_SEARCH) {
        rebuild_view();
        uapp_redraw(a);
    }
}

static int on_tick(struct uapp *a) {
    reload();
    uapp_redraw(a);
    return 1;
}

static void on_size(int *w, int *h) {
    *w = ugfx_char_advance('n') * 100;
    *h = ugfx_char_h() * 32;
}

int main(void) {
    uui_table_init(&g_table, 0, 0, 100, 100, COLUMNS, COL_COUNT, cell, 0);
    uui_table_set_compare(&g_table, compare_rows);
    // NEWEST LAST, which is what a log reads like and what the merge
    // wants: the Time column ascending interleaves the two rings.
    uui_table_set_sort(&g_table, COL_TIME, 1);
    uui_table_set_seek_col(&g_table, COL_TEXT);

    uui_dropdown_init(&g_level, 0, 0, 0, 0, LEVEL_NAMES, LEVEL_COUNT);
    uui_textbox_init(&g_search, "");
    uui_label_init(&g_level_label, "Level:");
    uui_label_init(&g_search_label, "Search:");

    g_form_items[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_level_label,
                                         .name = "levellabel" };
    g_form_items[1] = (struct uui_item){ .ops = &uui_dropdown_ops, .widget = &g_level,
                                         .id = ID_LEVEL, .name = "level" };
    g_form_items[2] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_search_label,
                                         .name = "searchlabel" };
    g_form_items[3] = (struct uui_item){ .ops = &uui_textbox_ops, .widget = &g_search,
                                         .id = ID_SEARCH, .name = "search",
                                         .flags = UUI_FILL_W };
    g_form = (struct uui_layout){ .dir = UUI_ROW, .items = g_form_items, .count = 4 };

    g_items[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_form,
                                    .name = "form", .flags = UUI_FILL_W };
    g_items[1] = (struct uui_item){ .ops = &uui_table_ops, .widget = &g_table,
                                    .id = ID_TABLE, .name = "log",
                                    .flags = UUI_FILL_W | UUI_FILL_H };
    g_root = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_items, .count = 2 };

    uui_focus_init(&g_focus, g_focusables,
                   (int)(sizeof g_focusables / sizeof g_focusables[0]));
    uui_focus_set(&g_focus, 2);   // the log itself has the keyboard on open

    reload();

    struct uapp_desc desc = {
        .title        = "Log Viewer",
        .app_id       = "logview",
        .on_size      = on_size,
        .layout       = &g_root,
        .widgets      = g_items,
        .widget_count = 2,
        .on_widget    = on_widget,
        .on_tick      = on_tick,
        .tick_ms      = 1000,   // the log grows; a second is under notice
        .focus        = &g_focus,
        .flags        = UAPP_RESIZABLE,
    };
    return uapp_run(&desc);
}
