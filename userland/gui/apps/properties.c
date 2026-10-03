// Properties -- what this file or folder is, and the few things about it
// worth changing here: its name, its permission bits, what opens its type.
//
// A WINDOW OF ITS OWN, spawned with the path as its argument, which is
// how Explorer and Dolphin do it and not how a toolkit modal would: a
// folder's total size is a RECURSIVE WALK no event loop should block on,
// a Properties window left open beside the listing is more useful than
// one that blocks it, and anything that can name a path can open one.
// NOT SINGLE-INSTANCE: comparing two files is why you open the second.
//
// THE LOOK IS macOS's Get Info in this desktop's design language: the
// picture as the hero on a stage tinted by it (ui/uui_fileinfo.h, shared
// with the File Manager's details pane), then sections that open and
// close -- and the WINDOW grows and shrinks with them rather than
// scrolling. Edits apply AT ONCE with a status line, as the File
// Manager's own rename does; there is no OK/Apply to forget.
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "rt/sys.h"
#include "kpath.h"
#include "ksha256.h"
#include "lib/ufileinfo.h"
#include "lib/uopen.h"
#include "lib/uthumb.h"
#include "lib/uclip.h"
#include "ui/ugfx.h"
#include "ui/uapp.h"
#include "ui/ulog.h"
#include "ui/utheme.h"
#include "ui/uui_fileinfo.h"
#include "ui/uui_textbox.h"
#include "ui/uui_dropdown.h"
#include "ui/uui_checkbox.h"
#include "ui/uui_button.h"
#include "ui/uui_focus.h"
#include "ui/uui_route.h"   // UUI_REASON_*
#include "ui/uui_widget.h"  // uui_custom
#include "query_abi.h"
#include "lib/udate.h"
#include <fcntl.h>
#include <unistd.h>

enum {
    ID_INFO = 1, ID_NAME, ID_OPENS, ID_SUM, ID_CMP, ID_OPEN, ID_SHOW, ID_COPY,
    ID_PERM = 20,             // + 0..8: owner, group, others x read, write, run
};
enum { SLOT_NAME = 1, SLOT_OPENS, SLOT_PERM, SLOT_SUM };
enum { POST_THUMB = 1 };

static struct ufileinfo g_fi;
static struct uui_fileinfo g_info;
static struct uui_textbox g_name, g_cmp;
static struct uui_dropdown g_opens;
static struct uui_checkbox g_perm[9];
static struct uui_button g_sum, g_open, g_show, g_copy;
static struct uapp *g_app;
static struct uui_custom g_labels;
static char g_title[UFI_PATH + 16];
static char g_status[120];

static struct uopen_app g_apps[8];
static const char *g_app_names[8];
static int g_napps, g_app_cur;

// The checksum, read a slice per tick so the window keeps answering.
static int g_sum_fd = -1;
static struct ksha256 g_sha;
static char g_sum_hex[2 * KSHA256_LEN + 1];
static unsigned long long g_sum_done;

static const struct uimg *g_preview;

static int is_file(void) { return g_fi.ok && !g_fi.st.is_dir; }

static void status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof g_status, fmt, ap);
    va_end(ap);
    ulogf("properties: status %s\n", g_status);
}

// --- the widgets, as routed items --------------------------------------------

static struct uui_item g_items[] = {
    { .ops = &uui_fileinfo_ops, .widget = &g_info, .id = ID_INFO, .name = "info" },
    // The slots' labels, drawn over the widget's ground (custom: no hit).
    { .ops = &uui_custom_ops,   .widget = &g_labels },
    { .ops = &uui_textbox_ops,  .widget = &g_name, .id = ID_NAME, .name = "name" },
    { .ops = &uui_checkbox_ops, .widget = &g_perm[0], .id = ID_PERM + 0, .name = "perm0" },
    { .ops = &uui_checkbox_ops, .widget = &g_perm[1], .id = ID_PERM + 1, .name = "perm1" },
    { .ops = &uui_checkbox_ops, .widget = &g_perm[2], .id = ID_PERM + 2, .name = "perm2" },
    { .ops = &uui_checkbox_ops, .widget = &g_perm[3], .id = ID_PERM + 3, .name = "perm3" },
    { .ops = &uui_checkbox_ops, .widget = &g_perm[4], .id = ID_PERM + 4, .name = "perm4" },
    { .ops = &uui_checkbox_ops, .widget = &g_perm[5], .id = ID_PERM + 5, .name = "perm5" },
    { .ops = &uui_checkbox_ops, .widget = &g_perm[6], .id = ID_PERM + 6, .name = "perm6" },
    { .ops = &uui_checkbox_ops, .widget = &g_perm[7], .id = ID_PERM + 7, .name = "perm7" },
    { .ops = &uui_checkbox_ops, .widget = &g_perm[8], .id = ID_PERM + 8, .name = "perm8" },
    { .ops = &uui_button_ops,   .widget = &g_sum,  .id = ID_SUM,  .name = "sum" },
    { .ops = &uui_textbox_ops,  .widget = &g_cmp,  .id = ID_CMP,  .name = "compare" },
    { .ops = &uui_button_ops,   .widget = &g_open, .id = ID_OPEN, .name = "open" },
    { .ops = &uui_button_ops,   .widget = &g_show, .id = ID_SHOW, .name = "show" },
    { .ops = &uui_button_ops,   .widget = &g_copy, .id = ID_COPY, .name = "copy" },
    // The dropdown LAST: its open list is drawn over what lies below it.
    { .ops = &uui_dropdown_ops, .widget = &g_opens, .id = ID_OPENS, .name = "opens" },
};
#define N_ITEMS ((int)(sizeof g_items / sizeof g_items[0]))

static struct uui_focusable g_ring[N_ITEMS];
static struct uui_focus g_focus;

static struct uui_item *item(int id) {
    for (int i = 0; i < N_ITEMS; i++) if (g_items[i].id == id) return &g_items[i];
    return 0;
}

// --- sizes -------------------------------------------------------------------

static int per(void)    { int p = ugfx_char_advance('n'); return p > 0 ? p : 8; }
static int win_w(void)  { return per() * 60; }
static int bar_h(void)  { return utheme_control_h() + 2 * utheme_pad(); }
static int key_w(void)  { return per() * 11; }   // uui_fileinfo's label column
static int ctl_h(void)  { return utheme_control_h(); }
static int line_h(void) { return ugfx_char_h() + utheme_gap(); }

// ONE height, computed or not: the button, the compare field (there from
// the start, so a sum can be typed or pasted first), the digest's two
// lines and the verdict. A slot that grew on Compute pushed its own
// contents past the window's foot.
static int sum_slot_h(void) {
    int p = utheme_pad();
    return ctl_h() + p + ctl_h() + p + 2 * line_h() + line_h();
}

static void slots(void) {
    int p = utheme_pad();
    uui_fileinfo_slot(&g_info, "General", UTHEME_ACT_NAV, SLOT_NAME, ctl_h() + p, 1);
    if (is_file() && g_napps > 0)
        uui_fileinfo_slot(&g_info, "Opens with", UTHEME_ACT_EDIT, SLOT_OPENS, ctl_h() + p, 1);
    if (g_fi.ok)
        uui_fileinfo_slot(&g_info, "Permissions", UTHEME_ACT_ARRANGE, SLOT_PERM, 3 * (ctl_h() + p / 2) + line_h(), 0);
    if (is_file())
        uui_fileinfo_slot(&g_info, "Checksum", UTHEME_ACT_NONE, SLOT_SUM, sum_slot_h(), 0);
}

static void rebuild(void) {
    uui_fileinfo_set(&g_info, &g_fi);
    slots();
    uui_fileinfo_set(&g_info, &g_fi);   // again, so Details follows the slot sections
}

// No taller than the screen leaves room for: its height less the taskbar,
// a title bar and the window's offset, about twelve lines. Past that the
// widget scrolls.
// THE WINDOW NEVER GROWS PAST THE HEIGHT IT OPENED AT: the WM placed it
// at that size, and nothing moves a window back on screen, so growing
// as a section opened ran it under the taskbar on a 1080p panel. It may
// shrink as sections close; past it, the widget scrolls.
static int g_open_h;

static int content_h(void) {
    int h = uui_fileinfo_height(&g_info, win_w()) + bar_h();
    struct query_display q;
    int cap = ugfx_char_h() * 46;
    if (sys_query_record(QUERY_DISPLAY, 0, &q, sizeof q) >= (int)sizeof q && q.height)
        cap = (int)q.height - 12 * ugfx_char_h();
    if (g_open_h && cap > g_open_h) cap = g_open_h;
    return h > cap ? cap : h;
}

// --- layout: the slots' controls go where the widget says ---------------------

static void place(int slot, int on, void (*fn)(int x, int y, int w, int h)) {
    int x, y, w, h;
    int shown = on && uui_fileinfo_slot_rect(&g_info, slot, &x, &y, &w, &h);
    if (shown) fn(x, y, w, h);
    (void)shown;
}

static int g_name_on, g_opens_on, g_perm_on, g_sum_on;

static void lay_name(int x, int y, int w, int h) {
    (void)h;
    g_name_on = 1;
    uui_textbox_set_geometry(&g_name, x + key_w(), y, w - key_w(), ctl_h());
}
static void lay_opens(int x, int y, int w, int h) {
    (void)h;
    g_opens_on = 1;
    uui_dropdown_set_geometry(&g_opens, x + key_w(), y, w - key_w(), ctl_h());
}
static void lay_perm(int x, int y, int w, int h) {
    (void)w; (void)h;
    g_perm_on = 1;
    int col = per() * 9, p = utheme_pad();
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            uui_checkbox_set_geometry(&g_perm[r * 3 + c], x + key_w() + c * col,
                                      y + r * (ctl_h() + p / 2) + (ctl_h() - ugfx_char_h()) / 2);
}
static void lay_sum(int x, int y, int w, int h) {
    (void)h;
    g_sum_on = 1;
    int bw, bh;
    uui_button_natural_size(&g_sum, &bw, &bh);
    uui_button_set_geometry(&g_sum, x, y, bw, ctl_h());
    uui_textbox_set_geometry(&g_cmp, x, y + ctl_h() + utheme_pad(), w, ctl_h());
}

// A control is shown only when wholly inside the facts widget: a slot
// partly scrolled away keeps the part that is in view.
static int inside(int y, int h) { return y >= g_info.y && y + h <= g_info.y + g_info.h; }

static void layout(int cw, int ch) {
    g_name_on = g_opens_on = g_perm_on = g_sum_on = 0;
    uui_fileinfo_set_geometry(&g_info, 0, 0, cw, ch - bar_h());
    g_labels.x = 0; g_labels.y = 0; g_labels.w = cw; g_labels.h = ch - bar_h();
    place(SLOT_NAME, 1, lay_name);
    place(SLOT_OPENS, is_file(), lay_opens);
    place(SLOT_PERM, g_fi.ok, lay_perm);
    place(SLOT_SUM, is_file(), lay_sum);
    item(ID_NAME)->hidden = !g_name_on || !inside(g_name.y, g_name.h);
    item(ID_OPENS)->hidden = !g_opens_on || !inside(g_opens.y, g_opens.h);
    for (int i = 0; i < 9; i++) item(ID_PERM + i)->hidden = !g_perm_on || !inside(g_perm[i].y, g_perm[i].h);
    item(ID_SUM)->hidden = !g_sum_on || !inside(g_sum.y, g_sum.h);
    item(ID_CMP)->hidden = !g_sum_on || !inside(g_cmp.y, g_cmp.h);

    int p = utheme_pad(), by = ch - bar_h() + p, bw, bh, x = p;
    uui_button_natural_size(&g_open, &bw, &bh);
    uui_button_set_geometry(&g_open, x, by, bw, ctl_h());
    x += bw + p / 2;
    uui_button_natural_size(&g_show, &bw, &bh);
    uui_button_set_geometry(&g_show, x, by, bw, ctl_h());
    uui_button_natural_size(&g_copy, &bw, &bh);
    uui_button_set_geometry(&g_copy, cw - p - bw, by, bw, ctl_h());
    item(ID_OPEN)->hidden = !g_fi.ok;

    // The keys reach only what is shown.
    int n = 0;
    for (int i = 0; i < N_ITEMS; i++) {
        const struct uui_item *it = &g_items[i];
        if (it->hidden || it->id == ID_INFO || it->widget == &g_labels) continue;
        g_ring[n].widget = it->widget;
        g_ring[n].ops = it->ops == &uui_textbox_ops ? &uui_textbox_focus_ops : it->ops;
        n++;
    }
    g_focus.items = g_ring;
    g_focus.count = n;
    if (g_focus.current >= n) g_focus.current = n - 1;
}

// A section opened or closed, or a slot grew: the window follows.
static void resize(void) {
    if (!g_app) return;
    uapp_resize(g_app, win_w(), content_h());
}

// --- the facts shown and logged ----------------------------------------------

static void sync_controls(void) {
    uui_textbox_set_text(&g_name, g_fi.name);
    unsigned m = g_fi.st.mode;
    static const unsigned bit[9] = { 0400, 0200, 0100, 040, 020, 010, 04, 02, 01 };
    for (int i = 0; i < 9; i++) g_perm[i].checked = (m & bit[i]) != 0;
    g_napps = is_file() ? uopen_apps_for(g_fi.path, g_apps, 8, &g_app_cur) : 0;
    for (int i = 0; i < g_napps; i++) g_app_names[i] = g_apps[i].name;
    uui_dropdown_set_items(&g_opens, g_app_names, g_napps);
    if (g_app_cur >= 0) uui_dropdown_set_selected(&g_opens, g_app_cur);
}

// What the window says, for the tools that read it (the name, type,
// location, size, contents, both times and the inode).
static void log_rows(void) {
    char size[48], t[48];
    ufileinfo_size_text(&g_fi, size, sizeof size);
    ulogf("properties: row Name=%s\n", g_fi.name);
    ulogf("properties: row Type=%s\n", g_fi.type);
    ulogf("properties: row Location=%s\n", g_fi.dir);
    if (!g_fi.ok) return;
    ulogf("properties: row Size=%s\n", size);
    if (g_fi.st.is_dir)
        ulogf("properties: row Contains=%d file%s, %d folder%s%s\n", g_fi.files, g_fi.files == 1 ? "" : "s",
              g_fi.dirs, g_fi.dirs == 1 ? "" : "s", g_fi.walking ? " (counting...)" : "");
    else if (g_fi.opens[0])
        ulogf("properties: row Opens with=%s\n", g_fi.opens);
    udate_format(t, sizeof t, &g_fi.st.created, UDATE_DATE | UDATE_TIME | UDATE_SECONDS);
    ulogf("properties: row Created=%s\n", t);
    udate_format(t, sizeof t, &g_fi.st.modified, UDATE_DATE | UDATE_TIME | UDATE_SECONDS);
    ulogf("properties: row Modified=%s\n", t);
    ulogf("properties: row Mode=%04o\n", g_fi.st.mode & 07777);
    ulogf("properties: row Inode=%llu\n", (unsigned long long)g_fi.st.ino);
}

static void set_title(void) {
    snprintf(g_title, sizeof g_title, "%s Properties", g_fi.name);
    if (g_app) uapp_set_title(g_app, g_title);
}

// --- the edits -----------------------------------------------------------------

static void commit_name(void) {
    const char *want = uui_textbox_text(&g_name), *why = 0;
    if (!strcmp(want, g_fi.name)) return;
    char was[UFI_PATH];
    strlcpy(was, g_fi.name, sizeof was);
    if (ufileinfo_rename(&g_fi, want, &why) < 0) {
        status("Not renamed: %s", why);
        uui_textbox_set_text(&g_name, g_fi.name);
        return;
    }
    status("Renamed %s to %s", was, g_fi.name);
    set_title();
    rebuild();
    log_rows();
}

static void commit_perm(void) {
    static const unsigned bit[9] = { 0400, 0200, 0100, 040, 020, 010, 04, 02, 01 };
    unsigned m = g_fi.st.mode & ~0777u;
    for (int i = 0; i < 9; i++) if (g_perm[i].checked) m |= bit[i];
    const char *why = 0;
    if (ufileinfo_chmod(&g_fi, m, &why) < 0) { status("Permissions not changed: %s", why); sync_controls(); return; }
    status("Permissions set to %04o", g_fi.st.mode & 07777);
    ulogf("properties: row Mode=%04o\n", g_fi.st.mode & 07777);
}

static void commit_opens(void) {
    int k = uui_dropdown_selected(&g_opens);
    if (k < 0 || k >= g_napps || k == g_app_cur) return;
    const char *dot = strrchr(g_fi.name, '.');
    if (uopen_set_default(g_fi.path, g_apps[k].entry) < 0) {
        status("Could not save the choice");
        uui_dropdown_set_selected(&g_opens, g_app_cur);
        return;
    }
    g_app_cur = k;
    strlcpy(g_fi.opens, g_apps[k].name, sizeof g_fi.opens);
    status("%s files open with %s now", dot ? dot : "These", g_apps[k].name);
    ulogf("properties: row Opens with=%s\n", g_fi.opens);
}

static void log_verdict(void);

static void start_sum(void) {
    g_sum_fd = open(g_fi.path, O_RDONLY);
    if (g_sum_fd < 0) { status("Cannot read the file to sum it"); return; }
    ksha256_init(&g_sha);
    g_sum_done = 0;
    g_sum.disabled = 1;
    status("Computing SHA-256...");
}

static int sum_step(void) {
    static uint8_t buf[64 * 1024];
    if (g_sum_fd < 0) return 0;
    for (int i = 0; i < 4; i++) {
        long n = read(g_sum_fd, buf, sizeof buf);
        if (n > 0) { ksha256_update(&g_sha, buf, (size_t)n); g_sum_done += (unsigned long long)n; continue; }
        close(g_sum_fd);
        g_sum_fd = -1;
        if (n < 0) { status("Reading the file failed"); g_sum.disabled = 0; return 1; }
        unsigned char d[KSHA256_LEN];
        ksha256_final(&g_sha, d);
        for (int k = 0; k < KSHA256_LEN; k++) snprintf(g_sum_hex + 2 * k, 3, "%02x", d[k]);
        status("SHA-256 of %llu bytes", g_sum_done);
        ulogf("properties: row SHA-256=%s\n", g_sum_hex);
        log_verdict();
        return 1;
    }
    status("Computing SHA-256... %llu KB", g_sum_done / 1024);
    return 1;
}

// The pasted sum against ours: case and spaces ignored, as sha256sum -c
// lines carry the name after two spaces.
static int sum_verdict(void) {
    const char *s = uui_textbox_text(&g_cmp);
    char a[2 * KSHA256_LEN + 1];
    int n = 0;
    for (; *s && n < 2 * KSHA256_LEN; s++) {
        char c = *s;
        if (c == ' ' && n == 0) continue;
        if (c == ' ') break;
        a[n++] = (char)(c >= 'A' && c <= 'F' ? c + 32 : c);
    }
    a[n] = 0;
    if (!n) return 0;
    return !strcmp(a, g_sum_hex) ? 1 : -1;
}

// --- drawing -----------------------------------------------------------------

static void draw_slots(struct ugfx_surface *s) {
    const struct utheme *t = utheme_current();
    uint32_t dim = uui_state_bg(t->text, UUI_STATE_DISABLED), bg = t->panel_bg;
    int x, y, w, h, p = utheme_pad();
    if (uui_fileinfo_slot_rect(&g_info, SLOT_NAME, &x, &y, &w, &h))
        ugfx_draw_string_clipped(s, x, y + (ctl_h() - ugfx_char_h()) / 2, key_w() - p, "Name", dim, bg);
    if (is_file() && uui_fileinfo_slot_rect(&g_info, SLOT_OPENS, &x, &y, &w, &h))
        ugfx_draw_string_clipped(s, x, y + (ctl_h() - ugfx_char_h()) / 2, key_w() - p, "App", dim, bg);
    if (uui_fileinfo_slot_rect(&g_info, SLOT_PERM, &x, &y, &w, &h)) {
        static const char *const who[3] = { "Owner", "Group", "Others" };
        for (int r = 0; r < 3; r++)
            ugfx_draw_string_clipped(s, x, y + r * (ctl_h() + p / 2) + (ctl_h() - ugfx_char_h()) / 2,
                                     key_w() - p, who[r], dim, bg);
        char m[48];
        snprintf(m, sizeof m, "Mode %04o", g_fi.st.mode & 07777);
        const struct ugfx_font *was = ugfx_set_font(ugfx_font_mono(UGFX_FONT_REGULAR));
        ugfx_draw_string_clipped(s, x + key_w(), y + 3 * (ctl_h() + p / 2), w - key_w(), m, dim, bg);
        ugfx_set_font(was);
    }
    if (is_file() && uui_fileinfo_slot_rect(&g_info, SLOT_SUM, &x, &y, &w, &h)) {
        int dy = y + 2 * (ctl_h() + p);   // under the button and the compare field
        if (g_sum_hex[0]) {
            const struct ugfx_font *was = ugfx_set_font(ugfx_font_mono(UGFX_FONT_REGULAR));
            char half[KSHA256_LEN + 1];
            memcpy(half, g_sum_hex, KSHA256_LEN);
            half[KSHA256_LEN] = 0;
            ugfx_draw_string_clipped(s, x, dy, w, half, t->text, bg);
            ugfx_draw_string_clipped(s, x, dy + line_h(), w, g_sum_hex + KSHA256_LEN, t->text, bg);
            ugfx_set_font(was);
        } else {
            ugfx_draw_string_clipped(s, x, dy, w, g_sum_fd >= 0 ? "Computing..." : "Not computed yet",
                                     dim, bg);
        }
        int v = g_sum_hex[0] ? sum_verdict() : 0;
        int typed = uui_textbox_text(&g_cmp)[0] != 0;
        const char *msg = v > 0 ? "Matches the sum given" : v < 0 ? "Does NOT match the sum given"
                        : typed ? "Compute the sum to compare it" : "Type or paste a sum above to compare";
        uint32_t c = v > 0 ? utheme_action(UTHEME_ACT_CREATE) : v < 0 ? utheme_action(UTHEME_ACT_DANGER) : dim;
        ugfx_draw_string_clipped(s, x, dy + 2 * line_h(), w, msg, c, bg);
    }
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    struct ugfx_surface *s = d->surface;
    const struct utheme *t = utheme_current();
    layout(s->w, s->h);
    int by = s->h - bar_h();
    ugfx_fill_rect(s, 0, by, s->w, bar_h(), t->chrome);
    ugfx_fill_rect(s, 0, by, s->w, 1, t->separator);
    // The status between the bar's buttons, elided.
    int x0 = g_show.x + g_show.w + utheme_pad(), x1 = g_copy.x - utheme_pad();
    if (x1 > x0)
        ugfx_draw_string_elided(s, x0, by + (bar_h() - ugfx_char_h()) / 2, x1 - x0, g_status,
                                uui_state_bg(t->text, UUI_STATE_DISABLED), t->chrome);
    uapp_log_layout(a, "properties");
    for (int i = 0; i < g_info.nsec; i++) {
        int x, y, w, h;
        if (uui_fileinfo_header_rect(&g_info, g_info.sec[i].title, &x, &y, &w, &h))
            uapp_logf_layout("properties: section %s %d %d %d %d %d\n", g_info.sec[i].title,
                             x, y, w, h, g_info.sec[i].open);
    }
}

static void labels_draw(struct ugfx_surface *s, const struct uui_custom *c) {
    (void)c;
    // Clipped to the facts widget, or a slot partly scrolled away would
    // draw its labels over the button bar.
    ugfx_set_clip_rect(s, g_info.x, g_info.y, g_info.w, g_info.h);
    draw_slots(s);
    ugfx_clear_clip_rect(s);
}

// --- input -------------------------------------------------------------------

static void on_widget(struct uapp *a, int id, int reason) {
    (void)reason;
    if (id == ID_INFO && uui_fileinfo_take_toggle(&g_info)) {
        resize();
        uapp_redraw(a);
        return;
    }
    if (id >= ID_PERM && id < ID_PERM + 9) commit_perm();
    else if (id == ID_OPENS) commit_opens();
    uapp_redraw(a);
}

static void on_action(struct uapp *a, int code) {
    switch (code) {
    case ID_SUM: start_sum(); break;
    case ID_OPEN:
        if (uopen_spawn(g_fi.path) < 0) status("Nothing opens %s", g_fi.name);
        break;
    case ID_SHOW: {
        char *const argv[] = { "files", g_fi.dir[0] ? g_fi.dir : "/", 0 };
        if (sys_spawn_argv("/bin/wm/apps/files", argv) < 0) status("Could not start Files");
        break;
    }
    case ID_COPY:
        status(uclip_set_text(g_fi.path, (int)strlen(g_fi.path)) ? "Copied the path" : "The clipboard refused it");
        break;
    }
    uapp_redraw(a);
}

// The verdict, logged when it changes -- what a test reads.
static void log_verdict(void) {
    static int was = -9;
    int v = g_sum_hex[0] ? sum_verdict() : 0;
    if (v == was) return;
    was = v;
    ulogf("properties: row Compare=%s\n", v > 0 ? "match" : v < 0 ? "mismatch" : "none");
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    log_verdict();
    if ((key == '\n' || key == '\r') && g_focus.current >= 0 && g_ring[g_focus.current].widget == &g_name)
        commit_name();
    uapp_redraw(a);
}

// Focus leaving the name field commits it, as a rename in place does.
static void on_press(struct uapp *a, int x, int y, unsigned buttons) {
    (void)buttons;
    if (g_name_on && !uui_textbox_hit(&g_name, x, y) && strcmp(uui_textbox_text(&g_name), g_fi.name))
        commit_name();
    uapp_redraw(a);
}

static int on_user(struct uapp *a, int a0, int a1) {
    (void)a; (void)a1;
    if (a0 != POST_THUMB) return 0;
    uthumb_posted();
    return 1;
}

static int wake(void *ctx) { (void)ctx; return uapp_post(g_app, POST_THUMB, 0); }

static int on_tick(struct uapp *a) {
    (void)a;
    int changed = 0;
    if (g_fi.walking) {
        ufileinfo_walk(&g_fi, 4);
        uui_fileinfo_set(&g_info, &g_fi);
        if (!g_fi.walking) {
            ulogf("properties: walk done %s %d files %d dirs %llu bytes%s\n", g_fi.path, g_fi.files,
                  g_fi.dirs, g_fi.bytes, g_fi.overflow ? " (INCOMPLETE)" : "");
            log_rows();
        }
        changed = 1;
    }
    if (sum_step()) changed = 1;
    if (uthumb_tick()) changed = 1;
    // The picture arrives from the thumbnail worker: ask until it does.
    if (is_file() && g_fi.img_w && !g_preview) {
        int px = uui_fileinfo_preview_px(&g_info, win_w());
        g_preview = uthumb_get(g_fi.path, &g_fi.st.modified, (uint32_t)g_fi.st.size, px);
        if (g_preview) {
            g_info.preview = g_preview;   // its stage was reserved: no resize
            changed = 1;
        }
    }
    return changed;
}

static void on_open(struct uapp *a) {
    g_app = a;
    set_title();
    static const struct uthumb_config cfg = { .wake = wake, .log_prefix = "properties" };
    if (is_file() && g_fi.img_w) uthumb_init(&cfg);
    resize();
}

static void on_size(int *w, int *h) {
    rebuild();
    *w = win_w();
    *h = content_h();
    if (!g_open_h) g_open_h = *h;
}

int main(int argc, char **argv) {
    if (argc < 2 || !argv[1][0]) {
        ulog("properties: no path given\n");
        return 1;
    }
    ufileinfo_load(&g_fi, argv[1], UFI_HEADERS | UFI_WALK | UFI_OPENS);
    snprintf(g_title, sizeof g_title, "%s Properties", g_fi.name);

    uui_fileinfo_init(&g_info);
    g_labels.draw = labels_draw;
    uui_textbox_init(&g_name, g_fi.name);
    uui_textbox_init(&g_cmp, "");
    g_cmp.placeholder = "Paste a SHA-256 to compare";
    uui_dropdown_init(&g_opens, 0, 0, 0, 0, g_app_names, 0);
    static const char *const labels[3] = { "Read", "Write", "Run" };
    for (int i = 0; i < 9; i++)
        uui_checkbox_init(&g_perm[i], 0, 0, 0, labels[i % 3], UUI_COLOR_UNSET, UUI_COLOR_UNSET);
    for (int i = 0; i < 9; i++) g_perm[i].accent = 1;
    uui_button_init(&g_sum, 0, 0, 0, 0, "Compute SHA-256", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_SUM);
    uui_button_init(&g_open, 0, 0, 0, 0, "Open", UTHEME_ACCENT, UTHEME_ACCENT_TEXT, ID_OPEN);
    uui_button_init(&g_show, 0, 0, 0, 0, "Show in Files", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_SHOW);
    uui_button_init(&g_copy, 0, 0, 0, 0, "Copy path", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_COPY);
    g_sum.outlined = g_show.outlined = g_copy.outlined = 1;
    sync_controls();
    rebuild();
    uui_focus_init(&g_focus, g_ring, 0);
    log_rows();

    struct uapp_desc desc = {
        .title        = g_title,
        // NOT single-instance: see the header. Named all the same, so the
        // taskbar groups them and the title bar finds the icon.
        .app_id       = "properties",
        .widgets      = g_items,
        .widget_count = N_ITEMS,
        .focus        = &g_focus,
        .tick_ms      = 100,
        .on_size      = on_size,
        .on_open      = on_open,
        .on_draw      = on_draw,
        .on_widget    = on_widget,
        .on_action    = on_action,
        .on_key       = on_key,
        .on_press     = on_press,
        .on_user      = on_user,
        .on_tick      = on_tick,
    };
    return uapp_run(&desc);
}
