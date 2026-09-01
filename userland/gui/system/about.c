// About, as a RING-3 PROCESS.
//
// The same static info window as apps/about.c, moved out of the kernel
// in Milestone 41's stage 0 (docs/wm-ring3-design.md): still no state
// and no input handling, but drawn by a ring-3 client into its own
// buffer instead of by a gui_apps.h callback into the framebuffer.
//
// WHAT CHANGED IN THE PORT, AND WHY
// ---------------------------------
// Coordinates are content-relative with no window_content_x/y() to add
// (a client owns its buffer's origin), gfx_* became ugfx_* and THEME_*
// became UTHEME_*, and the size callback derives from ugfx_char_w/h()
// the way the kernel version derived from gfx_char_w/h().
//
// ONE LINE IS STILL MISSING, BUT NOT FOR THE REASON IT USED TO BE.
// The kernel version printed the filesystem backend and whether it
// persists, and this comment used to say ring 3 could not ask --
// `fs_backend_name()`/`fs_is_persistent()` had no syscall behind them.
// That is no longer true: QUERY_FSINFO reports both (2026-08-20, added
// so `df` could stop being a builtin), and `sys_query_record(
// QUERY_FSINFO, 0, ...)` is all this would need.
//
// It is simply not done yet -- adding it means widening this window and
// re-checking the size callback that derives from the widest line, which
// is a change to make deliberately rather than as a side effect of
// somebody else`s work. See docs/roadmap.md. `df` reports both facts
// in the meantime.
#include <stdint.h>
#include "rt/sys.h"
#include "ui/ugfx.h"
#include "ui/uapp.h"
#include "ui/utheme.h"
#include "version.h"     // TOYOS_VERSION*, generated -- see tools/gen_version.sh
#include "build_date.h"  // TOYOS_BUILD_DATE -- the DAY this program was built
#include <stdio.h>       // snprintf
#include <string.h>      // strcmp

#define MARGIN   8
#define LINE_GAP 6

// TOYOS_VERSION_FULL rather than TOYOS_VERSION: on a dev build that
// carries the commit id (and "-dirty" when the tree did not match it),
// which is the only way to tell which build an ISO actually is. A
// release shows the bare number -- its tag pins it. The window sizes
// itself from the widest line, so a longer version string widens the
// window instead of being clipped.
//
// Kept as one table so the size callback and the draw agree by
// construction -- the kernel version had a ROWS macro beside the draw
// calls, which is the arrangement that lets the two disagree.
// THE FIRST THREE ROWS ARE FILLED AT RUNTIME, because the kernel's
// version is the kernel's to report -- QUERY_VERSION. A window built
// from string literals shows the version of the WINDOW, which is right
// only while kernel and userland ship as one image and wrong on any
// machine updated a piece at a time. /bin/about had exactly this bug
// and it misread a laptop by three commits.
static char g_kernel_line[80];
static char g_userland_line[80];
static char g_mismatch_line[64];

static const char *LINES[] = {
    g_kernel_line,
    g_userland_line,
    g_mismatch_line,     // empty unless they differ -- an empty row is skipped
    "",
    "Windows: drag the title bar to move,",
    "use the _ / o / x buttons.",
    "Alt+F4 closes the focused window.",
    "",
    "This window is a ring-3 process.",
};
#define ROWS ((int)(sizeof LINES / sizeof LINES[0]))

// Idempotent, and called from BOTH the size callback and the draw:
// about_size() runs first and measures these strings, so they have to
// exist by then. A row filled only in draw() would size the window from
// empty text and then paint past it.
static void fill_lines(void) {
    if (g_kernel_line[0]) return;

    struct query_version kv;
    int have = sys_query_record(QUERY_VERSION, 0, &kv, sizeof kv) >= (int)sizeof kv;
    if (have)
        snprintf(g_kernel_line, sizeof g_kernel_line, "toy-os v%s (%s), built %s",
                 kv.version, kv.build_id, kv.stamp);
    else
        snprintf(g_kernel_line, sizeof g_kernel_line,
                 "toy-os v%s (kernel does not report its version)", TOYOS_VERSION);

    snprintf(g_userland_line, sizeof g_userland_line, "desktop v%s, built %s",
             TOYOS_VERSION_FULL, TOYOS_BUILD_DATE);

    if (have && strcmp(kv.build_id, TOYOS_BUILD_ID) != 0)
        snprintf(g_mismatch_line, sizeof g_mismatch_line,
                 "** kernel and desktop are from different builds **");
}

static int line_h(void) { return ugfx_char_h() + LINE_GAP; }

static void about_size(int *w, int *h) {
    fill_lines();
    int widest = 0;
    for (int i = 0; i < ROWS; i++) {
        int tw = ugfx_text_width(LINES[i]);
        if (tw > widest) widest = tw;
    }
    *w = 2 * MARGIN + widest;
    *h = 2 * MARGIN + ROWS * ugfx_char_h() + (ROWS - 1) * LINE_GAP;
}

static void about_draw(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    fill_lines();
    ugfx_fill(d->surface, UTHEME_PANEL_BG);
    for (int i = 0; i < ROWS; i++) {
        if (!LINES[i][0]) continue;
        // Clipped, like anything in a fixed box (docs/gui-guidelines.md):
        // the window is resizable, so a narrowed window must truncate
        // rather than paint past its own edge.
        ugfx_draw_string_clipped(d->surface, MARGIN, MARGIN + i * line_h(),
                                 d->surface->w - 2 * MARGIN, LINES[i],
                                 UTHEME_TEXT, UTHEME_PANEL_BG);
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
        .flags   = UAPP_RESIZABLE | UAPP_SINGLE_INSTANCE,
    };
    return uapp_run(&desc);
}
