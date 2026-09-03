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
// WHAT IS NOT HERE, AND WHY. The processor MODEL: `kernel/arch/x86_64/
// cpuid.c` reads the 48-byte brand string, but QUERY_CPUS carries only
// acpi/apic ids and flags, so ring 3 cannot ask for it. Reporting the
// count is the honest answer until that record grows a field. Adding
// one is an ABI change, not a side effect of this window.
#include <stdint.h>
#include "rt/sys.h"
#include "ui/ugfx.h"
#include "ui/uapp.h"
#include "ui/utheme.h"
#include "lib/icon_cache.h" // icon_get() -- an icon is a NAME, not a path
#include "version.h"        // TOYOS_VERSION*, generated -- tools/gen_version.sh
#include "build_date.h"     // TOYOS_BUILD_DATE -- the DAY this program was built
#include "cpuinfo.h"        // struct cpu_info -- SYS_CPU_INFO, the brand and topology
#include <stdio.h>
#include <string.h>

#define MARGIN    14
#define LINE_GAP  6
#define LOGO      48
#define LOGO_GAP  12
#define LABEL_GAP 10
#define SECTION_GAP 10

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

// MiB rather than bytes: this is a window somebody reads, not a script.
// GiB to one decimal, in integer tenths: there is no floating point in
// this userland either. 3499 MiB reads as "3.4 GiB", which is the
// figure a person compares against the sticker on the machine.
static void gib(char *out, int cap, uint64_t bytes) {
    unsigned long long tenths = (bytes * 10) / (1024ull * 1024 * 1024);
    snprintf(out, cap, "%llu.%llu GiB", tenths / 10, tenths % 10);
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

    add("Software", 1, 0);

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

    add("Hardware", 1, 0);

    // A LIST class: count the records rather than asking for a count
    // nobody publishes. See the note at the top on the missing model.
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
        gib(inst, sizeof inst, mem.phys_usable_bytes);
        gib(tot, sizeof tot, mem.frame_total * mem.frame_bytes);
        gib(freeb, sizeof freeb, mem.frame_free * mem.frame_bytes);
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

static int line_h(void) { return ugfx_char_h() + LINE_GAP; }

static int label_col(void) {
    int w = 0;
    for (int i = 0; i < g_nrows; i++) {
        if (g_rows[i].heading || !g_rows[i].label[0]) continue;
        int tw = ugfx_text_width(g_rows[i].label);
        if (tw > w) w = tw;
    }
    return w;
}

// A heading gets the gap ABOVE it, except the first.
static int row_y(int i) {
    int y = 0;
    for (int k = 0; k < i; k++)
        y += line_h() + (g_rows[k + 1].heading && k ? SECTION_GAP : 0);
    return y;
}

static void about_size(int *w, int *h) {
    fill_rows();
    int lab = label_col();
    // MEASURE IN THE WEIGHT THAT DRAWS: the title and the headings are
    // bold, and bold is wider.
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    int widest = ugfx_text_width(g_title);
    for (int i = 0; i < g_nrows; i++)
        if (g_rows[i].heading && ugfx_text_width(g_rows[i].label) > widest)
            widest = ugfx_text_width(g_rows[i].label);
    ugfx_set_font(was);
    for (int i = 0; i < g_nrows; i++) {
        if (g_rows[i].heading) continue;
        int tw = !g_rows[i].label[0]
                 ? ugfx_text_width(g_rows[i].value)
                 : lab + LABEL_GAP + ugfx_text_width(g_rows[i].value);
        if (tw > widest) widest = tw;
    }
    int head = LOGO > 2 * ugfx_char_h() ? LOGO : 2 * ugfx_char_h();
    *w = 2 * MARGIN + (LOGO + LOGO_GAP + widest);
    *h = 2 * MARGIN + head + SECTION_GAP + row_y(g_nrows);
}

static void about_draw(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    fill_rows();
    ugfx_fill(d->surface, UTHEME_PANEL_BG);

    // The logo, then the name beside it -- the arrangement every About
    // box uses, and the reason the mark got a plate (see gen_icons.py's
    // icon_toyos: the Start button paints its own background, nothing
    // sits behind this one).
    const struct uimg *logo = icon_get("toyos", LOGO);
    if (logo)
        ugfx_blit_alpha(d->surface, MARGIN, MARGIN, logo->w, logo->h,
                        logo->px, logo->w);
    int text_x = MARGIN + LOGO + LOGO_GAP;
    int right = d->surface->w - MARGIN;
    // The title and the section headings are BOLD -- the desktop font's
    // bold weight, which falls back to regular where the face has none.
    const struct ugfx_font *bold = ugfx_font_session(UGFX_FONT_BOLD);
    const struct ugfx_font *was = ugfx_set_font(bold);
    ugfx_draw_string_clipped(d->surface, text_x, MARGIN + 6, right - text_x,
                             g_title, UTHEME_TEXT, UTHEME_PANEL_BG);
    ugfx_set_font(was);
    ugfx_draw_string_clipped(d->surface, text_x, MARGIN + 6 + line_h(),
                             right - text_x, "a small x86-64 operating system",
                             UTHEME_BORDER, UTHEME_PANEL_BG);

    int head = LOGO > 2 * ugfx_char_h() ? LOGO : 2 * ugfx_char_h();
    int top = MARGIN + head + SECTION_GAP;
    int lab = label_col();
    for (int i = 0; i < g_nrows; i++) {
        struct row *r = &g_rows[i];
        int y = top + row_y(i);
        // Clipped, like anything in a fixed box (docs/gui-guidelines.md):
        // the window is resizable, so a narrowed one truncates rather
        // than painting past its own edge.
        if (r->heading) {
            ugfx_set_font(bold);
            ugfx_draw_string_clipped(d->surface, text_x, y, right - text_x,
                                     r->label, UTHEME_ACCENT, UTHEME_PANEL_BG);
            ugfx_set_font(was);
            continue;
        }
        if (r->label[0])
            ugfx_draw_string_clipped(d->surface,
                                     text_x + (lab - ugfx_text_width(r->label)),
                                     y, lab, r->label,
                                     UTHEME_BORDER, UTHEME_PANEL_BG);
        int vx = r->label[0] ? text_x + lab + LABEL_GAP : text_x;
        ugfx_draw_string_clipped(d->surface, vx, y, right - vx, r->value,
                                 r->warn ? UTHEME_ACCENT : UTHEME_TEXT,
                                 UTHEME_PANEL_BG);
    }
}

int main(void) {
    struct uapp_desc desc = {
        .title   = "About",
        // A second identical, static About box is never what the user
        // meant by clicking About twice.
        .app_id  = "about",
        .on_size = about_size,
        .on_draw = about_draw,
        // FIXED SIZE: the window is sized from its rows, and nothing in
        // it reflows -- every About box is (macOS, GNOME, KDE Info Center).
        .flags   = UAPP_SINGLE_INSTANCE,
    };
    return uapp_run(&desc);
}
