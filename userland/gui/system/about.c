// About: what this machine is running, and what it is running on.
//
// SHAPED LIKE EVERY OTHER ABOUT BOX, deliberately. Windows' `winver` is
// version and build; macOS's "About This Mac" is identity plus
// hardware; KDE's and GNOME's are a logo, then a Software section of
// component versions and a Hardware section of what they run on. The
// common core is IDENTITY, VERSIONS, AND THE MACHINE -- and none of
// them carry help text, which is why the "drag the title bar" lines
// that used to be here are gone. That was documentation in a window
// named after something else.
//
// EVERY FACT IS ASKED FOR, not compiled in. The kernel's version is the
// KERNEL's to report (QUERY_VERSION) -- a window built from string
// literals shows the version of the WINDOW, which is right only while
// kernel and userland ship as one image and wrong on any machine
// updated a piece at a time. /bin/about had exactly that bug and it
// misread a laptop by three commits, which is what the mismatch row
// below exists to catch.
//
// TWO COLUMNS, KDE's "About This System": the mark, name and version on
// the left; Software and Hardware on the right; Copy to clipboard (the
// whole report as text, for a bug report) and Close in a footer.
#include <stdint.h>
#include "rt/sys.h"
#include "ui/ugfx.h"
#include "ui/uapp.h"
#include "lib/human.h"
#include "ui/utheme.h"
#include "lib/icon_cache.h" // icon_get() -- an icon is a NAME, not a path
#include "version.h"        // TOYOS_VERSION*, generated -- tools/gen_version.sh
#include "build_date.h"     // TOYOS_BUILD_DATE -- the DAY this program was built
#include "cpuinfo.h"        // struct cpu_info -- SYS_CPU_INFO, the brand and topology
#include "ui/uui_button.h"
#include "ui/uui_focus.h"
#include "ui/uui_route.h"   // UUI_REASON_*
#include "ui/ulog.h"
#include "lib/uclip.h"
#include <stdio.h>
#include <string.h>

// Font-derived, so the window reflows with the session font.
static int pad(void)       { return utheme_pad() * 2; }
static int line_h(void)    { return ugfx_char_h() + ugfx_char_h() / 3; }
static int logo_px(void)   { return ugfx_char_h() * 5; }
static int label_gap(void) { return ugfx_char_w() * 3 / 2; }
static int footer_h(void)  { return utheme_control_h() + 2 * utheme_pad(); }

// A row is a label and a value, or a HEADING (value empty, label set,
// `heading` true), or a full-width NOTE (label empty). One table so the
// size callback and the draw agree by construction -- the arrangement
// that lets those two disagree is a ROWS macro beside the draw calls.
struct row {
    const char *label;
    char value[72];
    int heading;
    int warn;
};

#define MAX_ROWS 16
static struct row g_rows[MAX_ROWS];
static int g_nrows;
static char g_title[48];

static struct row *add(const char *label, int heading, int warn) {
    if (g_nrows >= MAX_ROWS) return &g_rows[MAX_ROWS - 1];
    struct row *r = &g_rows[g_nrows++];
    r->label = label;
    r->heading = heading;
    r->warn = warn;
    r->value[0] = 0;
    return r;
}


// Idempotent, and called from BOTH the size callback and the draw:
// about_size() runs first and measures these strings, so they have to
// exist by then. A row filled only in draw() would size the window from
// empty text and then paint past it.
static void fill_rows(void) {
    if (g_nrows) return;

    struct query_version kv;
    int have_kv = sys_query_record(QUERY_VERSION, 0, &kv, sizeof kv) >= (int)sizeof kv;
    snprintf(g_title, sizeof g_title, "toy-os %s",
             have_kv ? kv.version : TOYOS_VERSION);

    add("SOFTWARE", 1, 0);

    struct row *r = add("Kernel", 0, 0);
    if (have_kv) snprintf(r->value, sizeof r->value, "%s (%s)", kv.version, kv.build_id);
    else         snprintf(r->value, sizeof r->value, "does not report its version");

    if (have_kv) {
        r = add("Built", 0, 0);
        snprintf(r->value, sizeof r->value, "%s", kv.stamp);
    }

    // The same two rows as the kernel's, so the two components read as
    // a pair and a mismatch is visible as one.
    r = add("Desktop", 0, 0);
    snprintf(r->value, sizeof r->value, "%s", TOYOS_VERSION_FULL);
    r = add("Built", 0, 0);
    snprintf(r->value, sizeof r->value, "%s", TOYOS_BUILD_DATE);

    // The row this window exists to make impossible to miss.
    if (have_kv && strcmp(kv.build_id, TOYOS_BUILD_ID) != 0) {
        r = add("", 0, 1);
        snprintf(r->value, sizeof r->value,
                 "** kernel and desktop are from different builds **");
    }

    struct query_fsinfo fs;
    if (sys_query_record(QUERY_FSINFO, 0, &fs, sizeof fs) >= (int)sizeof fs) {
        r = add("Filesystem", 0, 0);
        if (!(fs.flags & QUERY_FS_MOUNTED))
            snprintf(r->value, sizeof r->value, "none mounted");
        else
            snprintf(r->value, sizeof r->value, "%s%s%s, %s%s", fs.name,
                     fs.device[0] ? " on " : "", fs.device,
                     (fs.flags & QUERY_FS_PERSISTENT) ? "persistent" : "RAM-only",
                     (fs.flags & QUERY_FS_RDONLY) ? ", read-only" : "");
    }

    add("HARDWARE", 1, 0);

    // THE MACHINE BY NAME, when the firmware gave one (SMBIOS type 1);
    // a nameless one is simply not mentioned, as /bin/about does.
    struct query_smbios sm;
    if (sys_query_record(QUERY_SMBIOS, 0, &sm, sizeof sm) >= (int)sizeof sm &&
        sm.found && (sm.sys_vendor[0] || sm.product[0])) {
        r = add("Device", 0, 0);
        snprintf(r->value, sizeof r->value, "%s%s%s", sm.sys_vendor,
                 sm.sys_vendor[0] && sm.product[0] ? " " : "", sm.product);
    }

    // A LIST class: count the records rather than asking for a count
    // nobody publishes.
    int cpus = 0;
    struct query_cpu c;
    while (cpus < 256 &&
           sys_query_record(QUERY_CPUS, cpus, &c, sizeof c) >= (int)sizeof c)
        cpus++;
    // BRAND, THEN TOPOLOGY -- Windows' About shows the brand and its
    // Task Manager the cores/threads split. CPUID knows one package;
    // the number of packages is the MADT's logical count divided by
    // what one package holds.
    static struct cpu_info ci;   // 300+ bytes: not a stack local
    int have_ci = sys_call(SYS_CPU_INFO, (uint64_t)(uintptr_t)&ci, 0, 0) == 0;
    if (have_ci && ci.brand[0]) {
        r = add("Processor", 0, 0);
        snprintf(r->value, sizeof r->value, "%s", ci.brand);
    }
    if (cpus) {
        r = add("Cores", 0, 0);
        int per_pkg = have_ci ? ci.cores * ci.threads_per_core : 0;
        if (per_pkg > 0) {
            int pkgs = cpus / per_pkg > 0 ? cpus / per_pkg : 1;
            snprintf(r->value, sizeof r->value, "%d x %d core%s, %d thread%s",
                     pkgs, ci.cores, ci.cores == 1 ? "" : "s",
                     cpus, cpus == 1 ? "" : "s");
        } else {
            snprintf(r->value, sizeof r->value, "%d logical", cpus);
        }
    }

    struct query_meminfo mem;
    if (sys_query_record(QUERY_MEMINFO, 0, &mem, sizeof mem) >= (int)sizeof mem) {
        // INSTALLED, USABLE, FREE -- Windows' "8.00 GB (7.87 GB usable)"
        // shape. Installed is the firmware map's usable total; usable is
        // every frame the allocator manages, high zone included, which
        // a process can now be handed. The gap between the two is what
        // the firmware kept and what a partial 2 MiB granule cost.
        char inst[24], tot[24], freeb[24];
        human_size_iec(inst, sizeof inst, mem.phys_usable_bytes);
        human_size_iec(tot, sizeof tot, mem.frame_total * mem.frame_bytes);
        human_size_iec(freeb, sizeof freeb, mem.frame_free * mem.frame_bytes);
        r = add("Memory", 0, 0);
        snprintf(r->value, sizeof r->value, "%s installed, %s usable, %s free",
                 inst, tot, freeb);
    }

    // The monitor by its own name when it gave one, else the driver.
    struct query_display disp;
    if (sys_query_record(QUERY_DISPLAY, 0, &disp, sizeof disp) >= (int)sizeof disp &&
        disp.driver[0]) {
        r = add("Display", 0, 0);
        if ((disp.flags & QUERY_DISPLAY_F_EDID) && disp.panel[0])
            snprintf(r->value, sizeof r->value, "%s %s, %llux%llu",
                     disp.vendor, disp.panel,
                     (unsigned long long)disp.width, (unsigned long long)disp.height);
        else
            snprintf(r->value, sizeof r->value, "%llux%llu (%s)",
                     (unsigned long long)disp.width, (unsigned long long)disp.height,
                     disp.driver);
    }

    struct query_ahci ah;
    if (sys_query_record(QUERY_AHCI, 0, &ah, sizeof ah) >= (int)sizeof ah &&
        (ah.flags & QUERY_AHCI_DRIVE) && ah.model[0]) {
        r = add("Disk", 0, 0);
        snprintf(r->value, sizeof r->value, "%s", ah.model);
    }
}

static int label_col(void) {
    int w = 0;
    for (int i = 0; i < g_nrows; i++) {
        if (g_rows[i].heading || !g_rows[i].label[0]) continue;
        int tw = ugfx_text_width(g_rows[i].label);
        if (tw > w) w = tw;
    }
    return w;
}

// A heading after the first gets a blank half-line above it.
static int row_y(int i) {
    int y = 0;
    for (int k = 0; k < i; k++)
        y += line_h() + (g_rows[k + 1].heading ? line_h() / 2 : 0);
    return y;
}

// --- the left column: the mark, the name, the version -----------------

static const char *const TAGLINE[] = { "x86-64 hobby", "operating system" };

// g_title is "toy-os <version>"; the column shows the two on two lines.
static const char *version_text(void) { return g_title + sizeof "toy-os"; }

static int left_w(void) {
    int w = logo_px();
    if (ugfx_char_w() * 16 > w) w = ugfx_char_w() * 16;
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    if (ugfx_text_width("toy-os") > w) w = ugfx_text_width("toy-os");
    ugfx_set_font(was);
    if (ugfx_text_width(version_text()) > w) w = ugfx_text_width(version_text());
    for (unsigned i = 0; i < sizeof TAGLINE / sizeof TAGLINE[0]; i++)
        if (ugfx_text_width(TAGLINE[i]) > w) w = ugfx_text_width(TAGLINE[i]);
    return w;
}

static int left_h(void) { return logo_px() + line_h() / 2 + 4 * line_h(); }

// The right column starts here, past the divider.
static int right_x(void) { return pad() + left_w() + pad() + 1 + pad(); }

// --- the footer: Copy to clipboard, Close ------------------------------

enum { ID_COPY = 1, ID_CLOSE };
static char g_copy_label[24] = "Copy to clipboard";
static struct uui_button g_copy, g_close;
static struct uui_item g_widgets[] = {
    { .ops = &uui_button_ops, .widget = &g_copy,  .id = ID_COPY,  .name = "copy" },
    { .ops = &uui_button_ops, .widget = &g_close, .id = ID_CLOSE, .name = "close" },
};
static struct uui_focusable g_focusables[] = {
    { &g_copy,  &uui_button_ops },
    { &g_close, &uui_button_ops },
};
static struct uui_focus g_focus;

// Sized for the WIDEST label the Copy button can show, so "Copied" does
// not shrink it under the pointer.
static int button_w(const struct uui_button *b) {
    int w, h;
    uui_button_natural_size(b, &w, &h);
    int min = ugfx_text_width("Copy to clipboard") + 2 * utheme_pad();
    return w > min ? w : min;
}

// The whole report as text -- what a bug report wants pasted into it.
static void copy_report(void) {
    static char buf[MAX_ROWS * 96 + 64];
    int n = snprintf(buf, sizeof buf, "%s\n", g_title);
    for (int i = 0; i < g_nrows && n < (int)sizeof buf; i++) {
        const struct row *r = &g_rows[i];
        if (r->heading)
            n += snprintf(buf + n, sizeof buf - n, "\n%s\n", r->label);
        else if (r->label[0])
            n += snprintf(buf + n, sizeof buf - n, "%s: %s\n", r->label, r->value);
        else
            n += snprintf(buf + n, sizeof buf - n, "%s\n", r->value);
    }
    if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
    // SAID EITHER WAY: a Copy that did nothing visible reads as broken.
    strlcpy(g_copy_label, uclip_set_text(buf, n) ? "Copied" : "Copy failed",
            sizeof g_copy_label);
    ulogf("about: copy %d %s\n", n, g_copy_label);
}

static void about_size(int *w, int *h) {
    fill_rows();
    int lab = label_col();
    // MEASURE IN THE WEIGHT THAT DRAWS: the headings are bold.
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    int widest = 0;
    for (int i = 0; i < g_nrows; i++)
        if (g_rows[i].heading && ugfx_text_width(g_rows[i].label) > widest)
            widest = ugfx_text_width(g_rows[i].label);
    ugfx_set_font(was);
    for (int i = 0; i < g_nrows; i++) {
        if (g_rows[i].heading) continue;
        int tw = !g_rows[i].label[0]
                 ? ugfx_text_width(g_rows[i].value)
                 : lab + label_gap() + ugfx_text_width(g_rows[i].value);
        if (tw > widest) widest = tw;
    }
    int body = row_y(g_nrows);
    if (left_h() > body) body = left_h();
    *w = right_x() + widest + pad();
    int buttons = pad() + button_w(&g_copy) + utheme_gap() + button_w(&g_close) + pad();
    if (buttons > *w) *w = buttons;
    *h = pad() + body + pad() + footer_h();
}

static void about_draw(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    fill_rows();
    struct ugfx_surface *s = d->surface;
    int foot_y = s->h - footer_h();
    ugfx_fill(s, UTHEME_PANEL_BG);

    // --- left: centred in its column ---
    int lw = left_w(), lx = pad(), cx = lx + lw / 2, y = pad();
    const struct uimg *logo = icon_get("toyos", logo_px());
    if (logo)
        ugfx_blit_alpha(s, cx - logo->w / 2, y, logo->w, logo->h, logo->px, logo->w);
    y += logo_px() + line_h() / 2;
    const struct ugfx_font *bold = ugfx_font_session(UGFX_FONT_BOLD);
    const struct ugfx_font *was = ugfx_set_font(bold);
    ugfx_draw_string_clipped(s, cx - ugfx_text_width("toy-os") / 2, y, lw, "toy-os",
                             UTHEME_TEXT, UTHEME_PANEL_BG);
    ugfx_set_font(was);
    y += line_h();
    ugfx_draw_string_clipped(s, cx - ugfx_text_width(version_text()) / 2, y, lw,
                             version_text(), UTHEME_TEXT, UTHEME_PANEL_BG);
    y += line_h();
    for (unsigned i = 0; i < sizeof TAGLINE / sizeof TAGLINE[0]; i++, y += line_h())
        ugfx_draw_string_clipped(s, cx - ugfx_text_width(TAGLINE[i]) / 2, y, lw,
                                 TAGLINE[i], UTHEME_BORDER, UTHEME_PANEL_BG);

    // --- the divider, then the sections ---
    ugfx_fill_rect(s, lx + lw + pad(), pad(), 1, foot_y - 2 * pad(), UTHEME_SEPARATOR);
    int text_x = right_x(), right = s->w - pad(), lab = label_col();
    for (int i = 0; i < g_nrows; i++) {
        struct row *r = &g_rows[i];
        int ry = pad() + row_y(i);
        // Clipped, like anything in a fixed box (docs/gui-guidelines.md).
        if (r->heading) {
            ugfx_set_font(bold);
            ugfx_draw_string_clipped(s, text_x, ry, right - text_x, r->label,
                                     UTHEME_ACCENT, UTHEME_PANEL_BG);
            ugfx_set_font(was);
            continue;
        }
        if (r->label[0])
            ugfx_draw_string_clipped(s, text_x + (lab - ugfx_text_width(r->label)), ry,
                                     lab, r->label, UTHEME_BORDER, UTHEME_PANEL_BG);
        int vx = r->label[0] ? text_x + lab + label_gap() : text_x;
        ugfx_draw_string_clipped(s, vx, ry, right - vx, r->value,
                                 r->warn ? UTHEME_ACCENT : UTHEME_TEXT, UTHEME_PANEL_BG);
    }

    // --- the footer bar; the library draws the buttons on top ---
    ugfx_fill_rect(s, 0, foot_y, s->w, footer_h(), UTHEME_WINDOW_BG);
    ugfx_fill_rect(s, 0, foot_y, s->w, 1, UTHEME_SEPARATOR);
    int bh = utheme_control_h(), by = foot_y + (footer_h() - bh) / 2;
    uui_button_set_geometry(&g_copy, pad(), by, button_w(&g_copy), bh);
    int cw = button_w(&g_close);
    uui_button_set_geometry(&g_close, s->w - pad() - cw, by, cw, bh);
}

static void on_action(struct uapp *a, int code) {
    if (code == ID_COPY) { copy_report(); uapp_redraw(a); }
    else if (code == ID_CLOSE) uapp_quit(a, 0);
}

int main(void) {
    uui_button_init(&g_copy, 0, 0, 0, 0, g_copy_label, UTHEME_BUTTON_BG, UTHEME_TEXT, ID_COPY);
    uui_button_init(&g_close, 0, 0, 0, 0, "Close", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_CLOSE);
    g_copy.outlined = g_close.outlined = 1;
    uui_focus_init(&g_focus, g_focusables, 2);
    struct uapp_desc desc = {
        .title   = "About",
        // A second identical, static About box is never what the user
        // meant by clicking About twice.
        .app_id  = "about",
        .on_size = about_size,
        .on_draw = about_draw,
        .widgets = g_widgets,
        .widget_count = 2,
        .focus   = &g_focus,
        .on_action = on_action,
        // FIXED SIZE: the window is sized from its rows, and nothing in
        // it reflows -- every About box is (macOS, GNOME, KDE Info Center).
        .flags   = UAPP_SINGLE_INSTANCE,
    };
    return uapp_run(&desc);
}
