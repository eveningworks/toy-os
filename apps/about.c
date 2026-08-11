// A static info window -- no state, no input handling. It exists mainly
// so there are two apps to open at once: try opening Notepad and About
// together, dragging them so they overlap, and clicking between them to
// check focus/z-order switching (the title bar of whichever's on top
// should be the highlighted one).
#include "about.h"
#include "wm/wm.h"
#include "theme.h"
#include "kapi.h"

#define ABOUT_MARGIN 8
#define ABOUT_LINE_GAP 6
#define ABOUT_ROWS 6 // number of text lines drawn below
// Widest line is the version line ("toy-os v" TOYOS_VERSION " --
// graphics mode"); TOYOS_VERSION is a semver-ish string (see VERSION,
// repo root, and tools/gen_version.sh), e.g. "0.1.0-dev" while in
// development or "0.1.0" once released -- not a plain incrementing
// number anymore, so this leaves extra headroom past today's length
// rather than tracking it exactly.
#define ABOUT_COLS 44

// Content-area size for the current font -- see gui_apps.h's default_size.
// Kept in sync with about_draw()'s layout below (margins, line count,
// per-line height) since both are just macros over the same constants.
void about_default_size(int *w, int *h) {
    *w = 2 * ABOUT_MARGIN + ABOUT_COLS * gfx_char_w();
    *h = 2 * ABOUT_MARGIN + ABOUT_ROWS * gfx_char_h() + (ABOUT_ROWS - 1) * ABOUT_LINE_GAP;
}

void about_open(struct window *win) {
    (void)win; // no state needed
}

void about_draw(struct window *win) {
    int cx = window_content_x(win);
    int cy = window_content_y(win);
    int cw = window_content_w(win);
    int ch = window_content_h(win);

    uint32_t bg = THEME_WINDOW_BG;
    uint32_t fg = THEME_TEXT;
    gfx_fill_rect(cx, cy, cw, ch, bg);

    int line_h = gfx_char_h() + ABOUT_LINE_GAP;
    gfx_draw_string(cx + ABOUT_MARGIN, cy + ABOUT_MARGIN + 0 * line_h, "toy-os v" TOYOS_VERSION " -- graphics mode", fg, bg);
    gfx_draw_string(cx + ABOUT_MARGIN, cy + ABOUT_MARGIN + 1 * line_h, "Windows: drag the title", fg, bg);
    gfx_draw_string(cx + ABOUT_MARGIN, cy + ABOUT_MARGIN + 2 * line_h, "bar to move, use the", fg, bg);
    gfx_draw_string(cx + ABOUT_MARGIN, cy + ABOUT_MARGIN + 3 * line_h, "_ / o / x buttons.", fg, bg);
    gfx_draw_string(cx + ABOUT_MARGIN, cy + ABOUT_MARGIN + 4 * line_h, "Esc: back to shell.", fg, bg);
    gfx_draw_string(cx + ABOUT_MARGIN, cy + ABOUT_MARGIN + 5 * line_h,
                     fs_is_persistent() ? "Storage: disk (persistent)" : "Storage: RAM (not persistent)",
                     fg, bg);
    (void)ch;
}
