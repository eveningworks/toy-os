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
// ONE LINE IS MISSING ON PURPOSE. The kernel version printed the
// filesystem backend and whether it persists (`fs_backend_name()` /
// `fs_is_persistent()`), which are kernel calls with no syscall behind
// them -- ring 3 cannot ask. Stage 0 is deliberately the stage that
// needs NO new kernel capability, and stage 4's list already carries
// the settings/process syscalls this would join, so the line waits for
// that rather than growing the ABI here. `df` and `fsck` report the
// same two facts in the meantime.
#include <stdint.h>
#include "rt/sys.h"
#include "ui/ugfx.h"
#include "ui/uapp.h"
#include "ui/utheme.h"
#include "version.h"   // TOYOS_VERSION, generated -- see tools/gen_version.sh

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
static const char *const LINES[] = {
    "toy-os v" TOYOS_VERSION_FULL,
    "",
    "Windows: drag the title bar to move,",
    "use the _ / o / x buttons.",
    "Alt+F4 closes the focused window.",
    "",
    "This window is a ring-3 process.",
};
#define ROWS ((int)(sizeof LINES / sizeof LINES[0]))

static int line_h(void) { return ugfx_char_h() + LINE_GAP; }

static void about_size(int *w, int *h) {
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
        .on_size = about_size,
        .on_draw = about_draw,
        .flags   = UAPP_RESIZABLE,
    };
    return uapp_run(&desc);
}
