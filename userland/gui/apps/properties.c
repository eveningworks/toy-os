// Properties -- what this file or folder actually is.
//
// A WINDOW OF ITS OWN, spawned with the path as its argument, which is
// how Explorer and Dolphin do it and not how a toolkit modal would. The
// three reasons, in the order they mattered: a folder's total size is a
// RECURSIVE WALK and no event loop should be doing that, a Properties
// window you can leave open beside the listing is more useful than one
// that blocks it, and anything that can name a path can open one --
// today the File Manager, tomorrow the desktop's own icons.
//
// **THE WALK RUNS A FEW DIRECTORIES PER TICK, NOT ALL AT ONCE.** The
// counts climb while you watch, exactly as Explorer's do, and a folder
// with thousands of entries never stops the window answering. The queue
// is a fixed ring and a full one is REPORTED rather than silently
// truncating the answer -- a total that is quietly a floor is worse than
// one that says it is.
//
// NOT SINGLE-INSTANCE, deliberately: two files have two sets of
// properties, and comparing them is the reason to open the second.
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "rt/sys.h"
#include "kpath.h"
#include "lib/human.h"
#include "lib/udate.h"
#include "lib/uopen.h"
#include "ui/ugfx.h"
#include "ui/uapp.h"
#include "ui/ulog.h"
#include "ui/utheme.h"
#include "lib/icon_cache.h"

#define PATH_MAX_LEN 64          // FS_PATH_MAX

static char g_path[PATH_MAX_LEN];
static struct sys_stat g_stat;
static int g_stat_ok;
static char g_title[PATH_MAX_LEN + 16];

// --- the recursive walk, a few directories per tick --------------------
//
// A QUEUE, not recursion: one static dirent buffer serves the whole walk
// (SYS_LISTDIR_MAX of them is 20 KB, far past ring 3's 2 KiB frame
// budget), so a recursive call would have nowhere to put its listing.
// /bin/ls -R is built the same way and says so.
#define WALK_QUEUE 64
#define WALK_PER_TICK 4

static char g_queue[WALK_QUEUE][PATH_MAX_LEN];
static int g_qhead, g_qtail, g_qcount;
static struct sys_dirent g_ents[SYS_LISTDIR_MAX];
static unsigned long long g_bytes;
static int g_files, g_dirs;
static int g_walking, g_overflow;

static int queue_push(const char *path) {
    if (g_qcount >= WALK_QUEUE) { g_overflow = 1; return 0; }
    strlcpy(g_queue[g_qtail], path, PATH_MAX_LEN);
    g_qtail = (g_qtail + 1) % WALK_QUEUE;
    g_qcount++;
    return 1;
}

// One directory: adds its files to the totals and its subdirectories to
// the queue. Returns 0 when the listing failed, which is left silent on
// purpose -- a directory that cannot be read mid-walk (removed under us)
// should not abandon a total that is otherwise correct.
static int walk_one(void) {
    if (g_qcount <= 0) return 0;
    char dir[PATH_MAX_LEN];
    strlcpy(dir, g_queue[g_qhead], PATH_MAX_LEN);
    g_qhead = (g_qhead + 1) % WALK_QUEUE;
    g_qcount--;

    int n = sys_listdir(dir, g_ents, SYS_LISTDIR_MAX);
    if (n < 0) return 1;
    for (int i = 0; i < n; i++) {
        char child[PATH_MAX_LEN];
        if (!k_path_join(dir, g_ents[i].name, child, sizeof child)) {
            // A path too long to name is a subtree that cannot be
            // counted, which makes the total a floor -- the same thing
            // a full queue means, and said the same way.
            g_overflow = 1;
            continue;
        }
        if (g_ents[i].is_dir) { g_dirs++; queue_push(child); }
        else { g_files++; g_bytes += g_ents[i].size; }
    }
    return 1;
}

// --- the facts, as rows ------------------------------------------------
//
// A row is a label and a value; a SECTION is a row with no label, whose
// value is the heading. The name is not a row: it is the header, drawn
// large beside the icon, as Explorer's General tab and Dolphin's
// Properties both do.

#define ROW_MAX 12
static char g_label[ROW_MAX][16];
static char g_value[ROW_MAX][PATH_MAX_LEN + 24];
static int g_rows;

static void row(const char *label, const char *fmt, ...) {
    if (g_rows >= ROW_MAX) return;
    strlcpy(g_label[g_rows], label, sizeof g_label[0]);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_value[g_rows], sizeof g_value[0], fmt, ap);
    va_end(ap);
    g_rows++;
}

static void section(const char *title) { row("", "%s", title); }
static int is_section(int i) { return g_label[i][0] == '\0'; }

// "TXT file", Windows' own wording, or "Folder". Derived from the
// extension because that is what this system actually keys on -- there
// is no magic-number sniffing and no mime database, and inventing a
// prettier name per extension would be a table with one caller.
static void type_of(char *out, int cap) {
    if (g_stat.is_dir) { strlcpy(out, "Folder", cap); return; }
    const char *base = k_path_basename(g_path);
    const char *dot = 0;
    for (const char *p = base; *p; p++) if (*p == '.') dot = p;
    if (!dot || !dot[1]) { strlcpy(out, "File", cap); return; }

    char ext[16];
    int n = 0;
    for (const char *p = dot + 1; *p && n < (int)sizeof ext - 1; p++)
        ext[n++] = (char)((*p >= 'a' && *p <= 'z') ? *p - 32 : *p);
    ext[n] = '\0';
    snprintf(out, (unsigned long)cap, "%s file", ext);
}

static char g_type[24];

static void build_rows(void) {
    g_rows = 0;
    type_of(g_type, sizeof g_type);

    section("General");
    row("Type", "%s", g_type);
    char dir[PATH_MAX_LEN];
    k_path_dirname(g_path, dir, sizeof dir);
    row("Location", "%s", dir);

    if (!g_stat_ok) {
        row("Size", "unreadable");
        return;
    }

    if (g_stat.is_dir) {
        char h[24];
        human_size(h, sizeof h, g_bytes);
        // "at least", not a bare number, when the walk could not reach
        // everything: a floor presented as a total is a wrong answer
        // wearing a right answer's clothes.
        row("Size", "%s%s (%llu bytes)", g_overflow ? "at least " : "",
             h, (unsigned long long)g_bytes);
        row("Contains", "%d file%s, %d folder%s%s", g_files,
             g_files == 1 ? "" : "s", g_dirs, g_dirs == 1 ? "" : "s",
             g_walking ? " (counting...)" : "");
    } else {
        char h[24];
        human_size(h, sizeof h, g_stat.size);
        row("Size", "%s (%llu bytes)", h, (unsigned long long)g_stat.size);

        char exec[PATH_MAX_LEN];
        row("Opens with", "%s",
             uopen_resolve(g_path, exec, sizeof exec) ? k_path_basename(exec)
                                                       : "nothing");
    }

    section("Dates");
    char when[32];
    rtc_format_iso(when, sizeof when, &g_stat.created, 1);
    row("Created", "%s", when);
    rtc_format_iso(when, sizeof when, &g_stat.modified, 1);
    row("Modified", "%s", when);

    section("Details");
    // SAID PLAINLY WHEN IT IS SYNTHETIC. SYS_STAT_INODES is the
    // filesystem answering "this number is mine"; without it the value
    // is a table slot that is stable for this boot and means nothing
    // outside it, and a bare number would invite believing otherwise.
    row("Inode", "%llu%s", (unsigned long long)g_stat.ino,
         (g_stat.flags & SYS_STAT_INODES) ? "" : " (synthetic)");
}

// --- the window --------------------------------------------------------
//
// FIXED SIZE, sized to its own rows: nothing in it reflows, so a resize
// handle would only ever crop it or pad it. Every measure is
// font-derived (docs/gui-guidelines.md).

#define ICON_PX 48

static int margin(void)  { return utheme_pad() * 2; }
static int line_h(void)  { return ugfx_char_h() + utheme_gap(); }
static int label_w(void) { return ugfx_char_w() * 11; }
static int indent(void)  { return ugfx_char_w(); }   // rows sit in from their heading
static int header_h(void) {
    int two = 2 * line_h();
    return ICON_PX > two ? ICON_PX : two;
}
// Where the rows start: under the header and its rule.
static int rows_y(void) { return margin() + header_h() + utheme_pad() * 2 + 1; }

// A section heading is set off by half a row above it; the first has
// the rule above it already.
static int row_y(int i) {
    int y = rows_y();
    for (int k = 0; k < i; k++) {
        y += line_h();
        if (is_section(k + 1) && k + 1 < g_rows) y += line_h() / 2;
    }
    return y;
}

static void prop_size(int *w, int *h) {
    int widest = 0;
    for (int i = 0; i < g_rows; i++) {
        int tw = ugfx_text_width(g_value[i]);
        if (tw > widest) widest = tw;
    }
    // The header's name is measured BOLD, since that is how it draws.
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    int name_w = ugfx_text_width(k_path_basename(g_path)) + ICON_PX + utheme_pad() - label_w() - indent();
    ugfx_set_font(was);
    if (name_w > widest) widest = name_w;
    // A floor and a ceiling: a one-word value must not produce a window
    // too narrow to read its own labels, and a long path must not open a
    // window wider than most screens.
    if (widest < ugfx_char_w() * 24) widest = ugfx_char_w() * 24;
    if (widest > ugfx_char_w() * 60) widest = ugfx_char_w() * 60;
    *w = 2 * margin() + indent() + label_w() + widest;
    *h = row_y(g_rows - 1) + line_h() + margin();
}

static void prop_draw(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    struct ugfx_surface *s = d->surface;
    ugfx_fill(s, UTHEME_PANEL_BG);
    int m = margin(), lh = line_h();

    // The header: the icon, the name in bold beside it, the type under
    // the name in the muted colour.
    const struct uimg *icon = icon_get(g_stat_ok && g_stat.is_dir ? "folder" : "file", ICON_PX);
    int iy = m + (header_h() - ICON_PX) / 2;
    if (icon) ugfx_blit_alpha(s, m, iy, icon->w, icon->h, icon->px, icon->w);
    int tx = m + ICON_PX + utheme_pad();
    int ty = m + (header_h() - 2 * lh) / 2;
    int avail = s->w - tx - m;
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    ugfx_draw_string_clipped(s, tx, ty, avail, k_path_basename(g_path),
                              UTHEME_TEXT, UTHEME_PANEL_BG);
    ugfx_set_font(was);
    ugfx_draw_string_clipped(s, tx, ty + lh, avail, g_type, UTHEME_BORDER, UTHEME_PANEL_BG);

    // The rule between the header and the facts.
    int ry = m + header_h() + utheme_pad();
    ugfx_fill_rect(s, m, ry, s->w - 2 * m, 1, UTHEME_BORDER);

    int lx = m + indent();
    int vx = lx + label_w();
    int vw = s->w - vx - m;
    for (int i = 0; i < g_rows; i++) {
        int y = row_y(i);
        if (is_section(i)) {
            // Headings in bold and in the accent: the one colour this
            // theme reserves for "look here".
            was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
            ugfx_draw_string_clipped(s, m, y, s->w - 2 * m, g_value[i],
                                      UTHEME_ACCENT, UTHEME_PANEL_BG);
            ugfx_set_font(was);
            continue;
        }
        // Clipped, both halves: a path is routinely longer than any
        // width it is given (docs/gui-guidelines.md's oldest trap).
        ugfx_draw_string_clipped(s, lx, y, label_w(), g_label[i],
                                  UTHEME_BORDER, UTHEME_PANEL_BG);
        ugfx_draw_string_clipped(s, vx, y, vw, g_value[i], UTHEME_TEXT, UTHEME_PANEL_BG);
    }
}

// The counts climb while the walk runs, then it stops asking for ticks.
static void log_rows(void);

static int prop_tick(struct uapp *a) {
    (void)a;
    if (!g_walking) return 0;
    for (int i = 0; i < WALK_PER_TICK && g_qcount > 0; i++) walk_one();
    if (g_qcount <= 0) {
        g_walking = 0;
        ulogf("properties: walk done %s %d files %d dirs %llu bytes%s\n",
              g_path, g_files, g_dirs, (unsigned long long)g_bytes,
              g_overflow ? " (INCOMPLETE)" : "");
    }
    build_rows();
    if (!g_walking) log_rows();   // the final totals, for the test that waits on them
    return 1;
}

// The layout log a test asserts on -- a fact, not a screenshot to
// interpret (docs/gui-guidelines.md). The name is a row here even
// though it draws as the header, since "what does the window say" is
// the question and the header says it.
static void log_rows(void) {
    ulogf("properties: row Name=%s\n", k_path_basename(g_path));
    for (int i = 0; i < g_rows; i++)
        if (!is_section(i)) ulogf("properties: row %s=%s\n", g_label[i], g_value[i]);
}

int main(int argc, char **argv) {
    if (argc < 2 || !argv[1][0]) {
        ulog("properties: no path given\n");
        return 1;
    }
    strlcpy(g_path, argv[1], sizeof g_path);
    g_stat_ok = (sys_stat(g_path, &g_stat) == 0);

    if (g_stat_ok && g_stat.is_dir) {
        queue_push(g_path);
        g_walking = 1;
    }
    build_rows();

    snprintf(g_title, sizeof g_title, "%s Properties", k_path_basename(g_path));

    log_rows();

    struct uapp_desc desc = {
        .title   = g_title,
        // NOT single-instance: see the header. Named all the same, so
        // the taskbar groups them and the title bar finds the icon.
        .app_id  = "properties",
        .tick_ms = 200,
        .on_size = prop_size,
        .on_draw = prop_draw,
        .on_tick = prop_tick,
    };
    return uapp_run(&desc);
}
