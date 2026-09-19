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
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>

// Bounded, and the newest win. 400 was sized for the RINGS -- one that
// has wrapped has already lost its oldest, so holding more buys nothing.
// A stored boot is a different shape: a quiet boot on the test laptop is
// ~270 lines, but one with a driver retrying is thousands, and truncating
// to the tail hides the START of the boot, which is where an enumeration
// fault appears. 2000 covers a ~120 KiB boot whole; beyond that the most
// recent 2000 are kept. The table reads rows through a callback, so this
// bounds MEMORY (~204 bytes a line) and nothing else.
#define MAX_LINES 2000
#define TEXT_MAX  152
#define SRC_MAX   16

// **OLDER BOOTS COME FROM FILES, NOT FROM THE RINGS.** The rings hold
// this boot and nothing else -- that is what they are -- so everything
// before it is read from what logd persisted, one file per boot
// (docs/commands/logd.md). Same parse either way: logd writes the
// kernel's bytes VERBATIM after a tag, so a stored line differs from a
// live one only by that prefix.
#define BOOT_DIR   "/var/log/boot"
#define MAX_BOOTS  50            // storage.log_keep's own ceiling
#define BOOT_LABEL 16

// **THE SUBSYSTEM IS THE PREFIX BEFORE THE FIRST COLON**, which is this
// kernel's own convention -- `usb: port 2: connected`, `dhcp: lease
// 86400 seconds`, `wm: 19 desktop entries`. It is a SECOND level below
// the Source column: every one of those is source `kernel`, and "which
// part of the kernel" is the question a reader actually has.
//
// Derived rather than declared, because nothing in the ABI carries it --
// klog stores the bytes a subsystem wrote and the prefix is a habit the
// code keeps, not a field. So the list is whatever the loaded lines
// actually contain, and a line with no prefix is never filtered out, for
// the reason the Level filter already gives.
// 24, against a longest REAL prefix of 13 (`intel-display`, `syscall_stall`
// -- measured across the tree, not guessed). It was 14, which gave the scan
// 13 usable bytes and so stopped one character short of those two's colon:
// they recorded NO subsystem, and a line with no prefix is deliberately
// never filtered out, so `intel-display:` showed under every choice.
#define SUBSYS_MAX   24
#define MAX_SUBSYS   48

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
    char     subsys[SUBSYS_MAX];  // "usb", "dhcp", ... or "" for none
    char     text[TEXT_MAX];
};

static struct line g_lines[MAX_LINES];
static int         g_count;
static int         g_view[MAX_LINES];   // indices of g_lines that pass the filter
static int         g_view_count;

enum { ID_TABLE = 1, ID_LEVEL, ID_SEARCH, ID_BOOT, ID_SUBSYS };
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
static struct uui_dropdown g_level, g_boot, g_subsys;
static struct uui_textbox  g_search;
static struct uui_label    g_level_label, g_search_label, g_boot_label, g_subsys_label;

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
static void add_parsed_line(const char *src, const char *s, int len) {
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
    snprintf(l->source, sizeof l->source, "%s", src);
    int n = len - i;
    if (n > (int)sizeof l->text - 1) n = (int)sizeof l->text - 1;
    memcpy(l->text, s + i, (unsigned)n);
    l->text[n] = 0;

    // A PREFIX, NOT A WORD WITH A COLON IN IT: letters, digits, `_` and
    // `-` only, ending at ": ". That rejects `syscall: kill(pgid 2,
    // SIGHUP) by pid 14`'s later colons and, more importantly, a
    // message whose first colon is inside prose.
    l->subsys[0] = 0;
    for (int k = 0; k < (int)sizeof l->subsys - 1 && l->text[k]; k++) {
        char c = l->text[k];
        if (c == ':' && l->text[k + 1] == ' ' && k > 0) {
            memcpy(l->subsys, l->text, (unsigned)k);
            l->subsys[k] = 0;
            break;
        }
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) break;
    }
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
                add_parsed_line("kernel", acc, acc_len);
                acc_len = 0;
            } else if (acc_len < (int)sizeof acc) {
                acc[acc_len++] = c;
            }
        }
    }
    if (acc_len) add_parsed_line("kernel", acc, acc_len);
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

// --- older boots, out of /var/log/boot ---------------------------------

// Newest first after "This boot", because the boot somebody wants is
// almost always a recent one and a dropdown is read from the top.
static unsigned long long g_boots[MAX_BOOTS];
static int                g_boot_count;
static char               g_boot_labels[MAX_BOOTS + 1][BOOT_LABEL];
static const char        *g_boot_items[MAX_BOOTS + 1];
static int                g_boot_item_count;

static int cmp_desc(const void *a, const void *b) {
    unsigned long long x = *(const unsigned long long *)a;
    unsigned long long y = *(const unsigned long long *)b;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void scan_boots(void) {
    DIR *d = opendir(BOOT_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && g_boot_count < MAX_BOOTS) {
            if (e->d_type == DT_DIR) continue;
            unsigned long long n = strtoull(e->d_name, 0, 10);
            if (n) g_boots[g_boot_count++] = n;
        }
        closedir(d);
    }
    qsort(g_boots, (unsigned)g_boot_count, sizeof g_boots[0], cmp_desc);

    snprintf(g_boot_labels[0], BOOT_LABEL, "This boot");
    g_boot_items[0] = g_boot_labels[0];
    for (int i = 0; i < g_boot_count; i++) {
        snprintf(g_boot_labels[i + 1], BOOT_LABEL, "Boot %llu", g_boots[i]);
        g_boot_items[i + 1] = g_boot_labels[i + 1];
    }
    g_boot_item_count = g_boot_count + 1;
}

// A STORED LINE IS "[tag   ] <whatever the source wrote>". The tag is
// padded to a fixed width so the file greps at a fixed offset, and
// everything after it is byte-for-byte what the live ring holds -- which
// is why this hands the remainder to the same parser the rings use.
static void add_file_line(char *s, int len) {
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) len--;
    if (len <= 0) return;
    char src[SRC_MAX] = "";
    int i = 0;
    if (s[0] == '[') {
        int j = 1;
        while (j < len && s[j] != ']') j++;
        if (j < len) {
            int n = j - 1;
            while (n > 0 && s[n] == ' ') n--;     // the padding
            if (n > (int)sizeof src - 1) n = (int)sizeof src - 1;
            memcpy(src, s + 1, (unsigned)n);
            src[n] = 0;
            i = j + 1;
            if (i < len && s[i] == ' ') i++;
        }
    }
    add_parsed_line(src[0] ? src : "?", s + i, len - i);
}

// TWO PASSES, AND THE FIRST ONE ONLY COUNTS. next_line() keeps the
// NEWEST lines by shifting the whole array down once it is full, which
// costs a MAX_LINES-entry memmove per line -- fine for a ring that hands
// over a few hundred, quadratic for a file. A 183 KiB boot (~3000 lines)
// spent about 200 MB of copying to display, and it showed: selecting it
// visibly hung the window. Counting first and skipping to the last
// MAX_LINES means the shift never runs at all.
static int count_lines(int fd) {
    static char buf[2048];
    int lines = 0, got, partial = 0;
    while ((got = (int)read(fd, buf, sizeof buf)) > 0) {
        for (int i = 0; i < got; i++) {
            if (buf[i] == '\n') { lines++; partial = 0; }
            else partial = 1;
        }
    }
    return lines + partial;
}

static void read_boot_file(unsigned long long n) {
    char path[64];
    snprintf(path, sizeof path, BOOT_DIR "/%04llu.log", n);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    int total = count_lines(fd);
    int skip = total > MAX_LINES ? total - MAX_LINES : 0;
    if (lseek(fd, 0, SEEK_SET) < 0) { close(fd); return; }

    static char buf[2048];
    static char line[512];
    int line_len = 0, index = 0, got;
    while ((got = (int)read(fd, buf, sizeof buf)) > 0) {
        for (int i = 0; i < got; i++) {
            if (buf[i] == '\n') {
                if (index++ >= skip) add_file_line(line, line_len);
                line_len = 0;
            } else if (line_len < (int)sizeof line - 1) {
                line[line_len++] = buf[i];
            }
        }
    }
    if (line_len && index >= skip) add_file_line(line, line_len);
    close(fd);
}

// --- the subsystem list ------------------------------------------------

static char        g_subsys_names[MAX_SUBSYS + 1][SUBSYS_MAX];
static const char *g_subsys_items[MAX_SUBSYS + 1];
static int         g_subsys_item_count;

static int cmp_name(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

// Rebuilt from whatever is loaded, and the SELECTION IS RESTORED BY
// NAME. The live view reloads every second and a boot that has just
// started logging `ahci:` would otherwise renumber the list under a
// reader who had picked `usb:` -- silently showing them a different
// subsystem than the one they chose.
static void rebuild_subsys(void) {
    // NOT WHILE THE POPUP IS OPEN: the list the reader is looking at
    // must not renumber under them mid-choice.
    if (g_subsys.open) return;
    char keep[SUBSYS_MAX] = "";
    int sel = uui_dropdown_selected(&g_subsys);
    if (sel > 0 && sel < g_subsys_item_count)
        snprintf(keep, sizeof keep, "%s", g_subsys_items[sel]);

    int n = 0;
    for (int i = 0; i < g_count && n < MAX_SUBSYS; i++) {
        if (!g_lines[i].subsys[0]) continue;
        int seen = 0;
        for (int k = 0; k < n; k++)
            if (strcmp(g_subsys_names[k + 1], g_lines[i].subsys) == 0) { seen = 1; break; }
        if (!seen) snprintf(g_subsys_names[++n], SUBSYS_MAX, "%s", g_lines[i].subsys);
    }
    qsort(g_subsys_names[1], (unsigned)n, SUBSYS_MAX, cmp_name);

    snprintf(g_subsys_names[0], SUBSYS_MAX, "All");
    for (int k = 0; k <= n; k++) g_subsys_items[k] = g_subsys_names[k];
    g_subsys_item_count = n + 1;

    uui_dropdown_set_items(&g_subsys, g_subsys_items, g_subsys_item_count);
    int restored = 0;
    for (int k = 1; k < g_subsys_item_count; k++)
        if (keep[0] && strcmp(g_subsys_items[k], keep) == 0) { restored = k; break; }
    uui_dropdown_set_selected(&g_subsys, restored);
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
        // A LINE WITH NO PREFIX IS NEVER FILTERED OUT by a subsystem
        // choice -- the same rule the Level column keeps, and for the
        // same reason: the prefix is a habit, not a field, so its
        // absence must not hide a line behind a choice it never made.
        int sub = uui_dropdown_selected(&g_subsys);
        if (sub > 0 && sub < g_subsys_item_count && g_lines[i].subsys[0] &&
            strcmp(g_lines[i].subsys, g_subsys_items[sub]) != 0) continue;
        if (needle && needle[0] && !strstr(g_lines[i].text, needle) &&
            !strstr(g_lines[i].source, needle)) continue;
        g_view[g_view_count++] = i;
    }
    uui_table_set_rows(&g_table, g_view_count);
}

// Index 0 is the live rings; anything else is a file, which does not
// grow -- so on_tick() leaves a stored boot alone rather than re-reading
// it every second and throwing away the reader's scroll position.
static int viewing_live(void) {
    return uui_dropdown_selected(&g_boot) == 0;
}

static void reload(void) {
    g_count = 0;
    if (viewing_live()) {
        read_klog();
        read_applog();
    } else {
        int i = uui_dropdown_selected(&g_boot) - 1;
        if (i >= 0 && i < g_boot_count) read_boot_file(g_boots[i]);
    }
    rebuild_subsys();
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

static struct uui_item   g_form_items[8];
static struct uui_layout g_form;
static struct uui_item   g_items[2];
static struct uui_layout g_root;

static struct uui_focusable g_focusables[] = {
    { &g_boot,   &uui_dropdown_ops },
    { &g_level,  &uui_dropdown_ops },
    { &g_subsys, &uui_dropdown_ops },
    { &g_search, &uui_textbox_focus_ops },
    { &g_table,  &uui_table_ops },
};
static struct uui_focus g_focus;

static void on_widget(struct uapp *a, int id, int reason) {
    (void)reason;
    if (id == ID_BOOT) {
        // A DIFFERENT SOURCE, so the lines are re-read rather than
        // re-filtered -- and back to the top, since row 40 of one boot
        // means nothing in another.
        reload();
        uui_table_set_rows(&g_table, g_view_count);
        uapp_redraw(a);
    } else if (id == ID_LEVEL || id == ID_SEARCH || id == ID_SUBSYS) {
        rebuild_view();
        uapp_redraw(a);
    }
}

static int on_tick(struct uapp *a) {
    if (!viewing_live()) return 1;   // a stored boot is finished; leave it
    reload();
    uapp_redraw(a);
    return 1;
}

static void on_size(int *w, int *h) {
    // WIDE ENOUGH FOR THE FILTER ROW. Four controls and their labels sit
    // in one row, and uui_layout OVERFLOWS rather than shrinking below a
    // natural size -- so the default has to fit them or the search box
    // is pushed off the right edge with nothing to say so.
    *w = ugfx_char_advance('n') * 118;
    *h = ugfx_char_h() * 32;
}

int main(void) {
    uui_table_init(&g_table, 0, 0, 100, 100, COLUMNS, COL_COUNT, cell, 0);
    uui_table_set_compare(&g_table, compare_rows);
    // NEWEST LAST, which is what a log reads like and what the merge
    // wants: the Time column ascending interleaves the two rings.
    uui_table_set_sort(&g_table, COL_TIME, 1);
    uui_table_set_seek_col(&g_table, COL_TEXT);

    scan_boots();
    uui_dropdown_init(&g_boot, 0, 0, 0, 0, g_boot_items, g_boot_item_count);
    uui_label_init(&g_boot_label, "Boot:");
    snprintf(g_subsys_names[0], SUBSYS_MAX, "All");
    g_subsys_items[0] = g_subsys_names[0];
    g_subsys_item_count = 1;
    uui_dropdown_init(&g_subsys, 0, 0, 0, 0, g_subsys_items, g_subsys_item_count);
    uui_dropdown_init(&g_level, 0, 0, 0, 0, LEVEL_NAMES, LEVEL_COUNT);
    uui_textbox_init(&g_search, "");
    uui_label_init(&g_level_label, "Level:");
    uui_label_init(&g_search_label, "Search:");
    uui_label_init(&g_subsys_label, "Subsystem:");

    g_form_items[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_boot_label,
                                         .name = "bootlabel" };
    g_form_items[1] = (struct uui_item){ .ops = &uui_dropdown_ops, .widget = &g_boot,
                                         .id = ID_BOOT, .name = "boot" };
    g_form_items[2] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_level_label,
                                         .name = "levellabel" };
    g_form_items[3] = (struct uui_item){ .ops = &uui_dropdown_ops, .widget = &g_level,
                                         .id = ID_LEVEL, .name = "level" };
    g_form_items[4] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_subsys_label,
                                         .name = "subsyslabel" };
    g_form_items[5] = (struct uui_item){ .ops = &uui_dropdown_ops, .widget = &g_subsys,
                                         .id = ID_SUBSYS, .name = "subsys" };
    g_form_items[6] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_search_label,
                                         .name = "searchlabel" };
    g_form_items[7] = (struct uui_item){ .ops = &uui_textbox_ops, .widget = &g_search,
                                         .id = ID_SEARCH, .name = "search",
                                         .flags = UUI_FILL_W };
    g_form = (struct uui_layout){ .dir = UUI_ROW, .items = g_form_items, .count = 8 };

    g_items[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_form,
                                    .name = "form", .flags = UUI_FILL_W };
    g_items[1] = (struct uui_item){ .ops = &uui_table_ops, .widget = &g_table,
                                    .id = ID_TABLE, .name = "log",
                                    .flags = UUI_FILL_W | UUI_FILL_H };
    g_root = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_items, .count = 2 };

    uui_focus_init(&g_focus, g_focusables,
                   (int)(sizeof g_focusables / sizeof g_focusables[0]));
    uui_focus_set(&g_focus, 4);   // the log itself has the keyboard on open

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
