// System Settings -- a RING-3 process.
//
// Named for what it shows, and deliberately NOT "Control Panel": that is
// Windows' name, and this shows exactly the SETTINGS registry -- not
// facts, not tunables (see docs/settings-and-queries.md's "The
// vocabulary"). KDE and macOS both call this System Settings, and the
// qualifier earns its keep here because a per-application settings
// window is a thing toy-os may grow later.
//
// THE THING WORTH KNOWING ABOUT THIS FILE: it contains no list of
// settings, and now no list of CATEGORIES either. Both come from the
// registry (api/setting.h), so a setting registered anywhere in the
// kernel appears in the sidebar under a heading, with its legal values,
// with no edit to this file. The kernel-space version this replaced had
// one hand-written applet per setting -- a second source of truth that
// drifts. Exactly what dropping a `.desktop` file already does for the
// Start menu.
//
// THE SHAPE is KDE System Settings': a navigation tree on the left, one
// page on the right, a status line underneath. GNOME, Windows Settings
// and macOS Ventura all converged on the same sidebar-plus-pane; the
// TREE half is specifically KDE's, and it is what lets a category
// collapse when the registry grows past a screenful.
//
// Two consequences that look like bugs if you do not expect them: the
// sidebar is empty if nothing registered (a real answer, not a broken
// page), and a value can be REFUSED -- the registry validates, this does
// not, and the refusal is shown rather than swallowed.
#include <stdint.h>
#include "rt/sys.h"
#include "lib/string.h"
#include "lib/stdio.h"
#include "ui/uapp.h"
#include "ui/uui.h"
#include "ui/uui_layout.h"
#include "ui/uui_tree.h"
#include "ui/uui_radio_list.h"
#include "ui/uui_button_group.h"
#include "ui/uui_statusbar.h"
#include "ui/uui_scrollview.h"
#include "ui/utheme.h"
#include "setting_abi.h"
#include "cpuinfo.h"
#include "version.h"

// Room for more than the registry can hold, so a full registry still
// fits rather than silently showing part of itself.
#define MAX_SETTINGS  SETTING_ABI_MAX
#define MAX_CHOICES   48

enum { ID_TREE = 1, ID_BODY, ID_PAGE, ID_CHOICES, ID_STATUS };

// What a sidebar node selects, encoded in its `id`. Ranges rather than a
// parallel array: uui_tree hands back the app's own id, and decoding it
// here keeps the node table the single description of the sidebar.
#define NODE_SYSINFO      1
#define NODE_CATEGORY_BASE 1000 // + category index -- a heading row
#define NODE_SETTING_BASE  2000 // + setting index

// --- the settings, as read from the registry -------------------------

static char        g_label[MAX_SETTINGS][SETTING_ABI_LABEL_MAX];
static const char *g_label_ptr[MAX_SETTINGS];
// The QUALIFIED name ("system.font_size"), not the bare key: a setting's
// identity is (namespace, name), so the bare key is not necessarily
// usable on its own -- and this panel both addresses settings with it
// and prints it in commands for the user to type.
static char        g_name[MAX_SETTINGS][SETTING_ABI_QUALIFIED_MAX];
static char        g_ns[MAX_SETTINGS][SETTING_ABI_NS_MAX];
static char        g_file[MAX_SETTINGS][SETTING_ABI_FILE_MAX];
static char        g_cat_of[MAX_SETTINGS][SETTING_ABI_CATEGORY_MAX];
static char        g_value[MAX_SETTINGS][SETTING_ABI_VALUE_MAX];
static uint32_t    g_type[MAX_SETTINGS];
static int         g_setting_count;
static uint32_t    g_generation;

static char        g_choice[MAX_CHOICES][SETTING_ABI_VALUE_MAX];
static const char *g_choice_ptr[MAX_CHOICES];
static int         g_choice_count;

// What the last SET reported, shown under the choices. Held rather than
// printed once because a GUI has no scrollback: an app that flashes
// "not saved" for one frame has not told anyone anything.
static char g_status[128];

// Which setting the page is showing, or -1 for a non-setting page.
static int g_page_setting = -1;
static int g_show_sysinfo;
// Set once, in on_size -- see its comment for why not on_open.
static int g_loaded;

// The sidebar, built from the registry: one heading per distinct
// category, its settings beneath it, and System Information last.
#define MAX_CATEGORIES 12
static char        g_cat[MAX_CATEGORIES][SETTING_ABI_CATEGORY_MAX];
static int         g_cat_count;
// Node labels are POINTED AT by the tree, not copied, so they must
// outlive it -- file scope, per uui_tree.h.
static struct uui_tree_node g_nodes[MAX_CATEGORIES + MAX_SETTINGS + 1];
static int g_node_count;

static struct uui_tree       g_tree;
static struct uui_radio_list g_choices;
static struct uui_statusbar  g_status_bar;

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...) {
    // stderr, not stdout: a windowed client has no terminal, and the
    // kernel routes stderr to the log where a test tool can read it.
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    sys_eprint(buf);
}

// --- talking to the registry -----------------------------------------

// Builds the tree from whatever the registry just reported: one heading
// per distinct category IN FIRST-SEEN ORDER, its settings under it, and
// System Information as a leaf at the end.
//
// First-seen rather than sorted, deliberately. Registration order is the
// kernel's boot order, which groups related settings already, and a
// sort would put "Appearance" above "Startup" on one boot and leave a
// ring-3 program's category wherever the alphabet says on the next --
// a sidebar that reorders itself is one nobody builds muscle memory for.
static void rebuild_sidebar(void) {
    g_cat_count = 0;
    g_node_count = 0;

    for (int i = 0; i < g_setting_count; i++) {
        int c = -1;
        for (int j = 0; j < g_cat_count; j++)
            if (strcmp(g_cat[j], g_cat_of[i]) == 0) { c = j; break; }
        if (c < 0) {
            if (g_cat_count >= MAX_CATEGORIES) continue; // silently NOT dropped: see below
            c = g_cat_count++;
            strlcpy(g_cat[c], g_cat_of[i], sizeof g_cat[c]);
        }
        (void)c;
    }

    // Two passes, so every setting lands under its heading without the
    // nodes needing a sort: emit a category, then walk the settings
    // again for the ones that belong to it.
    for (int c = 0; c < g_cat_count; c++) {
        if (g_node_count >= (int)(sizeof g_nodes / sizeof g_nodes[0])) break;
        g_nodes[g_node_count++] = (struct uui_tree_node){
            .label = g_cat[c], .depth = 0, .id = NODE_CATEGORY_BASE + c
        };
        for (int i = 0; i < g_setting_count; i++) {
            if (strcmp(g_cat_of[i], g_cat[c]) != 0) continue;
            if (g_node_count >= (int)(sizeof g_nodes / sizeof g_nodes[0])) break;
            g_nodes[g_node_count++] = (struct uui_tree_node){
                .label = g_label[i], .depth = 1, .id = NODE_SETTING_BASE + i
            };
        }
    }

    // A top-level leaf, not a category: it has no settings under it, and
    // a heading you cannot expand is a heading that looks broken.
    if (g_node_count < (int)(sizeof g_nodes / sizeof g_nodes[0]))
        g_nodes[g_node_count++] = (struct uui_tree_node){
            .label = "System Information", .depth = 0, .id = NODE_SYSINFO
        };

    uui_tree_set_nodes(&g_tree, g_nodes, g_node_count);
}

static int reload_settings(void) {
    struct setting_msg m;
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_COUNT;
    if (sys_setting(&m) != 0) { g_setting_count = 0; return 0; }

    int n = m.count;
    if (n > MAX_SETTINGS) n = MAX_SETTINGS;
    g_setting_count = 0;

    for (int i = 0; i < n; i++) {
        memset(&m, 0, sizeof m);
        m.op = SETTING_OP_INFO;
        m.index = i;
        if (sys_setting(&m) != 0) continue;

        int k = g_setting_count;
        (void)k;
        // The namespace rides in the visible label when there is one:
        // two programs may own settings with the same name and the same
        // human label, and a list that showed only the label would offer
        // two identical rows.
        if (m.ns[0]) snprintf(g_label[k], sizeof g_label[k], "%s  (%s)", m.label, m.ns);
        else         strlcpy(g_label[k], m.label, sizeof g_label[k]);
        strlcpy(g_ns[k],    m.ns,    sizeof g_ns[k]);
        if (m.ns[0]) snprintf(g_name[k], sizeof g_name[k], "%s.%s", m.ns, m.name);
        else         strlcpy(g_name[k], m.name, sizeof g_name[k]);
        strlcpy(g_file[k],  m.file,  sizeof g_file[k]);
        // The kernel substitutes SETTING_CATEGORY_DEFAULT for a setting
        // that declared none, so this is never empty and the app needs
        // no fallback of its own -- one place the default is written.
        strlcpy(g_cat_of[k], m.category, sizeof g_cat_of[k]);
        strlcpy(g_value[k], m.value, sizeof g_value[k]);
        g_type[k] = m.type;
        g_label_ptr[k] = g_label[k];
        g_setting_count++;
    }
    g_generation = m.generation;
    rebuild_sidebar();
    return g_setting_count;
}

// Fills the choice list for setting `row`, and points the radio list's
// selection at whatever is currently in effect.
static void load_choices(int row) {
    g_choice_count = 0;
    g_choices.selected = -1;

    if (row < 0 || row >= g_setting_count) {
        g_choices.options = 0;
        g_choices.count = 0;
        return;
    }

    if (g_type[row] != SETTING_ABI_TYPE_ENUM) {
        // A free-text setting has no choices to offer. Shown as an
        // empty picker with an explanation rather than omitted, so the
        // row does not look broken -- editing one needs a text field,
        // which is why `config set` exists for these today.
        g_choices.options = 0;
        g_choices.count = 0;
        snprintf(g_status, sizeof g_status,
                 "%s is free text -- change it with `config set %s <value>`",
                 g_label[row], g_name[row]);
        return;
    }

    for (int c = 0; c < MAX_CHOICES; c++) {
        struct setting_msg m;
        memset(&m, 0, sizeof m);
        m.op = SETTING_OP_CHOICE;
        m.index = row;
        m.choice = c;
        if (sys_setting(&m) != 0) break; // past the last one
        strlcpy(g_choice[c], m.value, sizeof g_choice[c]);
        g_choice_ptr[c] = g_choice[c];
        g_choice_count = c + 1;
        if (strcmp(g_choice[c], g_value[row]) == 0) g_choices.selected = c;
    }

    g_choices.options = g_choice_ptr;
    g_choices.count = g_choice_count;
    g_choices.cols = 1;

    // The page's title lives HERE rather than being drawn in the page --
    // see on_draw() for why. Name first, because that is what a reader
    // is looking for; the file is the supporting detail.
    snprintf(g_status, sizeof g_status, "%s  --  stored in %s",
             g_label[row], g_file[row]);
}

// Applies choice `c` of the selected setting, and reports the OUTCOME
// rather than assuming it worked -- SETTING_UNSAVED means the change is
// live but will not survive a reboot, which is the one thing a settings
// UI must never report as plain success.
static void apply_choice(int row, int c) {
    if (row < 0 || row >= g_setting_count) return;
    if (c < 0 || c >= g_choice_count) return;

    struct setting_msg m;
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_SET;
    strlcpy(m.name, g_name[row], sizeof m.name);
    strlcpy(m.value, g_choice[c], sizeof m.value);
    if (sys_setting(&m) != 0) {
        strlcpy(g_status, "The settings registry is unavailable.", sizeof g_status);
        return;
    }

    switch (m.result) {
    case SETTING_SAVED:
        snprintf(g_status, sizeof g_status, "%s = %s  (saved to %s)",
                 g_label[row], g_choice[c], g_file[row]);
        break;
    case SETTING_UNSAVED:
        snprintf(g_status, sizeof g_status,
                 "%s = %s  -- APPLIED BUT NOT SAVED; it reverts at the next boot",
                 g_label[row], g_choice[c]);
        break;
    default:
        snprintf(g_status, sizeof g_status, "%s does not accept '%s'",
                 g_label[row], g_choice[c]);
        break;
    }
    logf_("settings: set %s %s result %u\n", g_name[row], g_choice[c], m.result);

    // Re-read: the value in effect is whatever the registry now says,
    // which is NOT necessarily what was just asked for. Believing the
    // request instead of the answer is how a UI shows a setting that
    // was refused.
    // The SELECTED SETTING is kept across the reload, and by id rather
    // than by row: rebuilding the sidebar can move rows (a new category
    // shifts everything below it) while an id names the same setting.
    int keep = g_page_setting;
    int keep_node = uui_tree_selected_id(&g_tree);
    reload_settings();
    uui_tree_select_id(&g_tree, keep_node);
    g_page_setting = keep;
    load_choices(keep);
}

// --- the System Info page --------------------------------------------

static struct cpu_info g_cpu;
static int g_cpu_loaded;

static void draw_sysinfo(struct ugfx_surface *s, int x, int y, int w, int h) {
    int line_h = ugfx_char_h() + 6;
    int row = 0;
    char line[128];

    if (!g_cpu_loaded) { sys_cpu_info(&g_cpu); g_cpu_loaded = 1; }

    struct sys_info si;
    memset(&si, 0, sizeof si);
    sys_sysinfo(&si);

    // Two budgets, and the second is the one that is easy to forget:
    // ugfx_draw_text_clipped bounds WIDTH only, so a row past the
    // bottom would simply be drawn wherever it landed. Stop on a whole
    // line rather than one sliced through its glyphs.
    #define INFO_LINE(str) do { \
        int ly = y + row * line_h; \
        if (ly + ugfx_char_h() > y + h) break; \
        ugfx_draw_string_clipped(s, x, ly, w, (str), UTHEME_TEXT, UTHEME_PANEL_BG); \
        row++; \
    } while (0)

    INFO_LINE("toy-os v" TOYOS_VERSION_FULL);

    const char *brand = g_cpu.brand;
    while (*brand == ' ') brand++;
    if (!*brand) brand = g_cpu.vendor;
    snprintf(line, sizeof line, "CPU:     %s", brand);
    INFO_LINE(line);

    snprintf(line, sizeof line, "Vendor:  %s", g_cpu.vendor);
    INFO_LINE(line);

    snprintf(line, sizeof line, "Ident:   family %u, model %u, stepping %u",
             (unsigned)g_cpu.family, (unsigned)g_cpu.model, (unsigned)g_cpu.stepping);
    INFO_LINE(line);

    if (g_cpu.mhz) {
        snprintf(line, sizeof line, "Speed:   ~%u MHz (%s)", (unsigned)g_cpu.mhz,
                 g_cpu.mhz_source == CPU_MHZ_CPUID_16H ? "CPUID" : "measured");
    } else {
        snprintf(line, sizeof line, "Speed:   unknown");
    }
    INFO_LINE(line);

    snprintf(line, sizeof line, "Enabled: SSE %s  NX %s  SMEP %s  SMAP %s",
             (g_cpu.enabled & CPU_EN_SSE) ? "on" : "off",
             (g_cpu.enabled & CPU_EN_NX) ? "on" : "off",
             (g_cpu.enabled & CPU_EN_SMEP) ? "on" : "off",
             (g_cpu.enabled & CPU_EN_SMAP) ? "on" : "off");
    INFO_LINE(line);

    snprintf(line, sizeof line, "Memory:  %u KB free of %u KB",
             (unsigned)si.mem_free_kb, (unsigned)si.mem_total_kb);
    INFO_LINE(line);

    if (si.flags & SYS_INFO_DISK_VALID) {
        snprintf(line, sizeof line, "Disk:    %u MB used of %u MB",
                 (unsigned)(si.disk_used_bytes / (1024 * 1024)),
                 (unsigned)(si.disk_total_bytes / (1024 * 1024)));
    } else {
        snprintf(line, sizeof line, "Disk:    unavailable");
    }
    INFO_LINE(line);

    snprintf(line, sizeof line, "PCI:     %u device(s)", (unsigned)sys_pci_count());
    INFO_LINE(line);

    snprintf(line, sizeof line, "Uptime:  %u s",
             (unsigned)(sys_monotonic_ns() / 1000000000ull));
    INFO_LINE(line);

    #undef INFO_LINE
}

// --- uapp plumbing ----------------------------------------------------

// The settings page SCROLLS. The tabs and the status bar do not: they
// sit outside the scroll view, so shrinking the window moves the page
// under them rather than pushing them off the bottom, which is what it
// used to do -- a short window lost the status bar entirely and left
// most of a setting's choices unreachable.
//
// The list and the choices are inside ONE scroll region rather than
// each scrolling itself. Two nested scrollbars would be the obvious
// alternative and the worse one: the list is laid out at its natural
// height in here, so every row is present and there is nothing for it
// to scroll -- one page, one scrollbar, one thing the wheel does.
// THE SHAPE: a row (sidebar | page) above a status bar.
//
//   +----------------+---------------------------+
//   | Appearance     |  Font size                |
//   |   Font size    |   ( ) 12                  |
//   |   Cursor       |   (o) 14                  |
//   | Input          |   ( ) 16                  |
//   | System Info    |                           |
//   +----------------+---------------------------+
//   | Stored in /etc/toyos.conf                  |
//   +--------------------------------------------+
//
// THE PAGE SCROLLS; THE SIDEBAR AND THE STATUS BAR DO NOT. uui_layout
// does not shrink children below their natural size -- it OVERFLOWS --
// so a page with more choices than fit would push the status bar off
// the bottom, which is exactly what this app did before it had a scroll
// view. The chrome stays outside it, per CLAUDE.md.
//
// The sidebar is NOT in the scroll view either: it scrolls itself, and
// one scroll region per page is the rule. A tree inside a scroll view
// would be laid out at full height with nothing left to scroll.
static struct uui_item PAGE[1];
static struct uui_layout PAGE_LAYOUT;
static struct uui_scrollview PAGE_SCROLL;

static struct uui_item ITEMS_BODY[2];
static struct uui_layout BODY_LAYOUT;

static struct uui_item ITEMS[2];
static struct uui_layout LAYOUT;

static void navigate(int node_id) {
    if (node_id == NODE_SYSINFO) {
        g_show_sysinfo = 1;
        g_page_setting = -1;
        // `hidden` removes a widget from BOTH the picture and
        // hit-testing, which is what makes this a page switch rather
        // than an overlay a stray click could still reach.
        ITEMS_BODY[1].hidden = 1;
        strlcpy(g_status, "About this machine", sizeof g_status);
        return;
    }
    g_show_sysinfo = 0;
    ITEMS_BODY[1].hidden = 0;

    int idx = -1;
    if (node_id >= NODE_SETTING_BASE) {
        idx = node_id - NODE_SETTING_BASE;
    } else if (node_id >= NODE_CATEGORY_BASE) {
        int c = node_id - NODE_CATEGORY_BASE;
        for (int i = 0; i < g_setting_count; i++)
            if (strcmp(g_cat_of[i], g_cat[c]) == 0) { idx = i; break; }
    }
    if (idx < 0 || idx >= g_setting_count) {
        g_page_setting = -1;
        return;
    }
    g_page_setting = idx;
    load_choices(idx);
    logf_("settings: page %s\n", g_name[idx]);
}

static void on_widget(struct uapp *a, int id, int reason) {
    // COMMIT ON RELEASE, which is docs/gui-guidelines.md's rule for
    // every control here -- and this file used to discard `reason`
    // entirely. The router delivers press, MOTION, release and wheel,
    // so acting on all of them meant merely moving the pointer across
    // the choice list applied a setting: each motion event wrote
    // /etc/toyos.conf, which bumped fs_generation(), which made the
    // desktop re-read and re-parse every .desktop file. Hovering froze
    // the machine for seconds, and the resulting filesystem churn is
    // what exposed a re-entrancy bug in fs_read() (see vfs.c).
    if (reason != UUI_REASON_RELEASE) return;

    switch (id) {
    case ID_TREE:
        // The tree hands back the APP's id, not a row -- rows move as
        // categories collapse, ids do not.
        navigate(uui_tree_selected_id(&g_tree));
        break;
    case ID_CHOICES:
        apply_choice(g_page_setting, g_choices.selected);
        break;
    default:
        break;
    }
    uapp_redraw(a);
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    struct ugfx_surface *s = uapp_surface(d);
    (void)a;

    // THE PAGE AREA IS THE SCROLL VIEW'S RECT, asked of the widget
    // rather than recomputed from the window size. The layout owns
    // where things ended up; a second calculation here would be a
    // second answer, and it would be wrong the first time the sidebar's
    // natural width changed.
    int px = PAGE_SCROLL.x, py = PAGE_SCROLL.y;
    int pw = PAGE_SCROLL.w, ph = PAGE_SCROLL.h;
    int pad = ugfx_char_w();

    if (g_show_sysinfo) {
        draw_sysinfo(s, px + pad, py + pad, pw - 2 * pad, ph - pad);
        return;
    }

    if (g_setting_count == 0) {
        ugfx_draw_string_clipped(s, px + pad, py + pad, pw - 2 * pad,
                                  "No settings are registered.",
                                  UTHEME_TEXT, UTHEME_PANEL_BG);
        return;
    }

    // NO HEADING IS DRAWN IN THE PAGE, and that is deliberate.
    //
    // The first version painted the setting's name at the top of the
    // page rect, in on_draw -- which runs AFTER the toolkit paints the
    // widgets, so it landed on top of the first choice. Reserving space
    // for it would only move the problem: the page SCROLLS, so content
    // would then slide underneath a heading that stays put.
    //
    // A fixed heading needs to be chrome OUTSIDE the scroll view, which
    // means a label widget this toolkit does not have. The information
    // -- which setting this is, and which file it persists to -- is in
    // the status bar instead, which is already chrome and already
    // outside. One less widget, and nothing overlaps at any scroll
    // offset.
    (void)pad;
}

static void on_open(struct uapp *a) {
    (void)a;
    // The lines tools/ asserts on. Kept in the app rather than derived
    // from a screenshot because a layout is a fact, and a number a test
    // can read beats a picture it has to interpret.
    logf_("settings: settings %d\n", g_setting_count);
    logf_("settings: categories %d\n", g_cat_count);
    logf_("settings: nodes %d\n", g_node_count);
    logf_("settings: layout tree %d %d %d %d\n",
          g_tree.x, g_tree.y, g_tree.w, g_tree.h);
    logf_("settings: layout page %d %d %d %d\n",
          PAGE_SCROLL.x, PAGE_SCROLL.y, PAGE_SCROLL.w, PAGE_SCROLL.h);
    logf_("settings: layout choices %d %d %d %d\n",
          g_choices.x, g_choices.y, g_choices.w, g_choices.h);
    // EVERY VISIBLE ROW, with the y a click should land on. Reported by
    // the app rather than re-derived in Python, for the reason
    // DebugConsole.menu_row() exists: a tool that computes row offsets
    // itself needs re-measuring every time a font or an inset changes,
    // and this repo has re-measured gui_flow.py's constants three times.
    // The y is the row's CENTRE, so a rounding difference cannot land
    // the click on the neighbour.
    int rh = uui_tree_row_h(&g_tree);
    for (int r = 0; r < uui_tree_visible_count(&g_tree); r++) {
        int node = uui_tree_node_at_row(&g_tree, r);
        if (node < 0) break;
        logf_("settings: row %d id %d y %d depth %d %s\n", r, g_nodes[node].id,
              g_tree.y + r * rh + rh / 2, g_nodes[node].depth, g_nodes[node].label);
    }
}

static void on_size(int *w, int *h) {
    // THE REGISTRY IS READ HERE, not in on_open. on_size is the first
    // hook with a font, and -- the part that matters -- it runs BEFORE
    // the layout, which sizes each widget from its natural_size. A tree
    // with no nodes and a radio list with no options both report a
    // natural size of nothing, so loading them in on_open (which runs
    // after) would lay the page out around two empty widgets and then
    // fill them, leaving both at whatever the layout had already
    // decided.
    //
    // Guarded, because on_size runs again on every resize and a reload
    // there would throw away the user's current selection mid-drag.
    if (!g_loaded) {
        g_loaded = 1;
        reload_settings();
        if (g_node_count > 0) {
            g_tree.selected = 0;
            navigate(g_nodes[0].id);
        }
    }

    // Wide enough for a sidebar and a page side by side. Sized HERE and
    // not in main(): ugfx_char_w() returns 0 until uapp_run() has
    // fetched the font, so anything sized in main() comes out zero-wide
    // -- drawing as nothing and hit-testing as nothing, which looks
    // exactly like a broken click handler.
    *w = ugfx_char_w() * 66;
    *h = ugfx_char_h() * 22;
}

int main(void) {
    // Nodes arrive in on_size(); a tree with none is a legitimate state
    // (nothing registered) and must not be a special case here.
    uui_tree_init(&g_tree, 0, 0, 0, 0, g_nodes, 0);
    g_tree.bg = UTHEME_PANEL_BG;
    g_tree.fg = UTHEME_TEXT;

    // One stretching pane, pointed at g_status -- the bar does not copy
    // the text, so the buffer has to outlive it, which a file-scope
    // array does.
    uui_statusbar_init(&g_status_bar);
    g_status_bar.panes[0].text = g_status;
    g_status_bar.panes[0].chars = 0;
    g_status_bar.count = 1;

    g_choices.cols = 1;
    g_choices.selected = -1;
    g_choices.hovered = -1;
    g_choices.bg = UTHEME_PANEL_BG;
    g_choices.fg = UTHEME_TEXT;

    PAGE[0] = (struct uui_item){ .ops = &uui_radio_list_ops, .widget = &g_choices,
                                  .id = ID_CHOICES, .flags = UUI_FILL_W };
    PAGE_LAYOUT = (struct uui_layout){ .dir = UUI_COLUMN, .items = PAGE,
                                        .count = (int)(sizeof PAGE / sizeof PAGE[0]),
                                        .margin = 0 };
    uui_scrollview_init(&PAGE_SCROLL, &PAGE_LAYOUT);
    uui_scrollview_set_preferred_rows(&PAGE_SCROLL, 12);

    // DESIGNATED INITIALISERS throughout: positional ones silently
    // re-bind when struct uui_item gains a field, and adding `hidden`
    // once put every widget's id into it.
    ITEMS_BODY[0] = (struct uui_item){ .ops = &uui_tree_ops, .widget = &g_tree,
                                        .id = ID_TREE, .flags = UUI_FILL_H };
    ITEMS_BODY[1] = (struct uui_item){ .ops = &uui_scrollview_ops, .widget = &PAGE_SCROLL,
                                        .id = ID_PAGE,
                                        .flags = UUI_FILL_W | UUI_FILL_H };
    BODY_LAYOUT = (struct uui_layout){ .dir = UUI_ROW, .items = ITEMS_BODY,
                                        .count = (int)(sizeof ITEMS_BODY / sizeof ITEMS_BODY[0]),
                                        .margin = 0 };

    ITEMS[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &BODY_LAYOUT,
                                   .id = ID_BODY, .flags = UUI_FILL_W | UUI_FILL_H };
    ITEMS[1] = (struct uui_item){ .ops = &uui_statusbar_ops, .widget = &g_status_bar,
                                   .id = ID_STATUS, .flags = UUI_FILL_W };
    LAYOUT = (struct uui_layout){ .dir = UUI_COLUMN, .items = ITEMS,
                                  .count = (int)(sizeof ITEMS / sizeof ITEMS[0]) };

    struct uapp_desc desc = {
        .title = "System Settings",
        // One is enough, and two would show the same registry while
        // each believing its own cached copy -- a second window is the
        // fastest way to see a stale value.
        .app_id = "settings",
        .layout = &LAYOUT,
        .flags = UAPP_RESIZABLE | UAPP_SINGLE_INSTANCE,
        .widgets = ITEMS,
        // DERIVED, not written out: this said 4 while the array was 3
        // for exactly one build, and the router walked one item past
        // the end into a garbage ops table.
        .widget_count = (int)(sizeof ITEMS / sizeof ITEMS[0]),
        .on_widget = on_widget,
        .on_draw = on_draw,
        .on_open = on_open,
        .on_size = on_size,
    };
    return uapp_run(&desc);
}
