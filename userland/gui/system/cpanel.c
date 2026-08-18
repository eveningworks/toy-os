// Control Panel -- a RING-3 process, replacing the kernel-space
// apps/control_panel.c.
//
// This is the last `Exec=builtin:` app, so moving it empties ring 0 of
// applications entirely and closes Milestone 41 stage 4's remaining
// prerequisite (docs/wm-ring3-design.md). What it needed from the
// kernel was a way to read and change settings from ring 3, and a way
// to ask about the machine -- SYS_SETTING and SYS_SYSINFO.
//
// THE THING WORTH KNOWING ABOUT THIS FILE: it contains no list of
// settings. The kernel-space version had one applet per setting, hand
// written, so a subsystem gaining a setting meant editing Control
// Panel -- a second source of truth that drifts. Here the rows come
// from the settings registry (api/setting.h), so a setting registered
// anywhere in the kernel appears here with no edit to this file, and
// its legal values come from the registry too rather than being
// duplicated as a local array. Exactly what dropping a `.desktop` file
// already does for the Start menu.
//
// Two consequences that look like bugs if you do not expect them: the
// list is empty if nothing registered (which is a real answer, not a
// broken page), and a value can be REFUSED -- the registry validates,
// this does not, and the refusal is shown rather than swallowed.
#include <stdint.h>
#include "rt/sys.h"
#include "lib/string.h"
#include "lib/stdio.h"
#include "ui/uapp.h"
#include "ui/uui.h"
#include "ui/uui_layout.h"
#include "ui/uui_listbox.h"
#include "ui/uui_radio_list.h"
#include "ui/uui_button_group.h"
#include "ui/uui_statusbar.h"
#include "ui/utheme.h"
#include "setting_abi.h"
#include "cpuinfo.h"
#include "version.h"

// Room for more than the registry can hold, so a full registry still
// fits rather than silently showing part of itself.
#define MAX_SETTINGS  SETTING_ABI_MAX
#define MAX_CHOICES   48

enum { ID_TABS = 1, ID_LIST, ID_CHOICES, ID_STATUS };
// NON-ZERO on purpose: uui_button_group_take_activated() returns 0
// for "nothing committed", so a button whose code is 0 can never be
// told apart from a press that was dragged off.
enum { TAB_SETTINGS = 101, TAB_SYSINFO = 102 };

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

static int g_tab = TAB_SETTINGS;

static struct uui_button      g_tab_btn[2];
static struct uui_button_group g_tabs;
static struct uui_listbox     g_list;
static struct uui_radio_list  g_choices;
static struct uui_statusbar   g_status_bar;

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
        strlcpy(g_value[k], m.value, sizeof g_value[k]);
        g_type[k] = m.type;
        g_label_ptr[k] = g_label[k];
        g_setting_count++;
    }
    g_generation = m.generation;
    uui_listbox_set_items(&g_list, g_label_ptr, g_setting_count);
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

    snprintf(g_status, sizeof g_status, "Stored in %s", g_file[row]);
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
    logf_("cpanel: set %s %s result %u\n", g_name[row], g_choice[c], m.result);

    // Re-read: the value in effect is whatever the registry now says,
    // which is NOT necessarily what was just asked for. Believing the
    // request instead of the answer is how a UI shows a setting that
    // was refused.
    int keep = g_list.selected;
    reload_settings();
    g_list.selected = keep;
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

static struct uui_item ITEMS[4];
static struct uui_layout LAYOUT;

static void set_tab(int tab) {
    g_tab = tab;
    // `hidden` removes a widget from BOTH the picture and hit-testing,
    // which is what makes this a page switch rather than an overlay a
    // stray click could still reach.
    ITEMS[1].hidden = (tab != TAB_SETTINGS);
    ITEMS[2].hidden = (tab != TAB_SETTINGS);
    logf_("cpanel: tab %s\n", tab == TAB_SETTINGS ? "settings" : "sysinfo");
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
    //
    // The tabs are exempt: uui_button_group does its own arming and
    // hands over a code only once a press completed on the same button,
    // so it has already applied this rule internally.
    if (id != ID_TABS && reason != UUI_REASON_RELEASE) return;

    switch (id) {
    case ID_TABS: {
        // The router names the WIDGET, and the group is one widget
        // holding both tabs -- so which one committed is collected from
        // the group rather than inferred. 0 means a press that was
        // dragged off and must change nothing.
        int code = uui_button_group_take_activated(&g_tabs);
        if (code) set_tab(code);
        break;
    }
    case ID_LIST:
        load_choices(g_list.selected);
        logf_("cpanel: select %s\n",
              g_list.selected >= 0 && g_list.selected < g_setting_count
                  ? g_name[g_list.selected] : "(none)");
        break;
    case ID_CHOICES:
        apply_choice(g_list.selected, g_choices.selected);
        break;
    default:
        break;
    }
    uapp_redraw(a);
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    struct ugfx_surface *s = uapp_surface(d);
    int w = uapp_width(a), h = uapp_height(a);
    int top = g_tab_btn[0].y + g_tab_btn[0].h + ugfx_char_h();

    if (g_tab == TAB_SYSINFO) {
        draw_sysinfo(s, ugfx_char_w(), top, w - 2 * ugfx_char_w(), h - top);
        return;
    }

    // The status line is a real uui_statusbar in the layout now, so the
    // layout RESERVES its height and the choices above cannot overlap
    // it -- which they did when this drew the line by hand at the
    // window's bottom edge.
    (void)h;
    if (g_setting_count == 0) {
        ugfx_draw_string_clipped(s, ugfx_char_w(), top, w - 2 * ugfx_char_w(),
                                  "No settings are registered.", UTHEME_TEXT, UTHEME_PANEL_BG);
    }
}

static void on_open(struct uapp *a) {
    (void)a;
    logf_("cpanel: settings %d\n", g_setting_count);
    logf_("cpanel: layout list %d %d %d %d\n", g_list.x, g_list.y, g_list.w, g_list.h);
    logf_("cpanel: layout choices %d %d %d %d\n",
          g_choices.x, g_choices.y, g_choices.w, g_choices.h);
    logf_("cpanel: layout tab0 %d %d %d %d\n",
          g_tab_btn[0].x, g_tab_btn[0].y, g_tab_btn[0].w, g_tab_btn[0].h);
    logf_("cpanel: layout tab1 %d %d %d %d\n",
          g_tab_btn[1].x, g_tab_btn[1].y, g_tab_btn[1].w, g_tab_btn[1].h);
}

static int g_loaded;

static void on_size(int *w, int *h) {
    // THE REGISTRY IS READ HERE, not in on_open. on_size is the first
    // hook with a font, and -- the part that matters -- it runs BEFORE
    // the layout, which sizes each widget from its natural_size. A
    // listbox with no items and a radio list with no options both
    // report a natural size of nothing, so loading them in on_open
    // (which runs after) laid the page out around two empty widgets and
    // then filled them, leaving both at whatever the layout had already
    // decided.
    //
    // Guarded, because on_size runs again on every resize and a reload
    // there would throw away the user's current selection mid-drag.
    if (!g_loaded) {
        g_loaded = 1;
        reload_settings();
        if (g_setting_count > 0) {
            g_list.selected = 0;
            load_choices(0);
        }
    }

    // Sized HERE and not in main(): ugfx_char_w() returns 0 until
    // uapp_run() has fetched the font, so buttons sized in main() come
    // out zero-wide -- drawing as nothing and hit-testing as nothing,
    // which looks exactly like a broken click handler.
    int bw = ugfx_char_w() * 14, bh = ugfx_char_h() + 12;
    g_tab_btn[0].x = 0;      g_tab_btn[0].y = 0;
    g_tab_btn[0].w = bw;     g_tab_btn[0].h = bh;
    g_tab_btn[1].x = bw + 8; g_tab_btn[1].y = 0;
    g_tab_btn[1].w = bw;     g_tab_btn[1].h = bh;

    *w = ugfx_char_w() * 58;
    *h = ugfx_char_h() * 22;
}

int main(void) {
    uui_button_init(&g_tab_btn[0], 0, 0, 0, 0, "Settings",
                     UTHEME_BUTTON_BG, UTHEME_TEXT, TAB_SETTINGS);
    uui_button_init(&g_tab_btn[1], 0, 0, 0, 0, "System Info",
                     UTHEME_BUTTON_BG, UTHEME_TEXT, TAB_SYSINFO);
    uui_button_group_init(&g_tabs, g_tab_btn, 2);

    uui_listbox_init(&g_list, 0, 0, 100, 100, 0, 0);

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

    ITEMS[0] = (struct uui_item){ .ops = &uui_button_group_ops, .widget = &g_tabs,
                                   .id = ID_TABS };
    ITEMS[1] = (struct uui_item){ .ops = &uui_listbox_ops, .widget = &g_list,
                                   .id = ID_LIST, .flags = UUI_FILL_W | UUI_FILL_H };
    ITEMS[2] = (struct uui_item){ .ops = &uui_radio_list_ops, .widget = &g_choices,
                                   .id = ID_CHOICES, .flags = UUI_FILL_W };
    ITEMS[3] = (struct uui_item){ .ops = &uui_statusbar_ops, .widget = &g_status_bar,
                                   .id = ID_STATUS, .flags = UUI_FILL_W };

    LAYOUT = (struct uui_layout){ .dir = UUI_COLUMN, .items = ITEMS, .count = 4 };

    struct uapp_desc desc = {
        .title = "Control Panel",
        // One is enough, and two would show the same registry while
        // each believing its own cached copy -- a second window is the
        // fastest way to see a stale value.
        .app_id = "cpanel",
        .layout = &LAYOUT,
        .flags = UAPP_RESIZABLE | UAPP_SINGLE_INSTANCE,
        .widgets = ITEMS,
        .widget_count = 4,
        .on_widget = on_widget,
        .on_draw = on_draw,
        .on_open = on_open,
        .on_size = on_size,
    };
    return uapp_run(&desc);
}
