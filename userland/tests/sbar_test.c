// uui_sbar -- the scrollbar state every scrolling widget embeds -- held
// to docs/gui-guidelines.md's "Scrollbars": the thumb does not jump when
// grabbed, a drag is absolute and clamps, the trough pages, the thumb's
// end is the right end, and hover is the strip's own. A ring-3 test
// because the toolkit is ring 3's (see typeahead_test.c).
//
// Every position is read back through the PAINTER's own geometry
// (uui_scrollbar_thumb_rect()), not this struct's arithmetic, so a
// conversion the two disagree on fails here rather than on screen.
//
// Prints one line per check and exits with the number of failures.
#include <stdio.h>
#include "rt/sys.h"
#include "ui/ugfx.h"
#include "ui/uui_sbar.h"
#include "ui/uui_scrollbar.h"
#include "lib/utest.h"

#define X 200
#define Y 40
#define H 300
#define TOTAL 100
#define VISIBLE 10

static void oki(const char *name, int got, int want) {
    char d[64];
    snprintf(d, sizeof d, "got %d, want %d", got, want);
    utest_check_detail(got == want, name, d);
}

// The thumb's top as the PAINTER would draw it now.
static int thumb_y(const struct uui_sbar *b, int *th) {
    int ty, h;
    uui_scrollbar_thumb_rect(b->y, b->h, b->total, b->visible,
                             (b->total - b->visible) - b->top, &ty, &h, b->w, 0);
    if (th) *th = h;
    return ty;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    ugfx_font_init();
    utest_begin("sbar_test", "the shared scrollbar state", UTEST_VERDICT_FILE);
    utest_check_detail(ugfx_char_h() > 0, "the font loaded, so the width is real", "char_h is 0");

    struct uui_sbar b;
    uui_sbar_init(&b);
    uui_sbar_place(&b, X, Y, H);
    uui_sbar_set(&b, TOTAL, VISIBLE, 0);
    int bx = X + b.w / 2;

    // --- the direction trap: from the START here, from the bottom below --
    int th;
    oki("at the start the thumb is at the TOP of the strip", thumb_y(&b, &th) <= Y + 2, 1);
    uui_sbar_set(&b, TOTAL, VISIBLE, TOTAL);
    oki("a top past the end clamps to the last page", b.top, TOTAL - VISIBLE);
    oki("...and the thumb is at the bottom", thumb_y(&b, &th) + th >= Y + H - 2, 1);
    uui_sbar_set(&b, TOTAL, VISIBLE, 0);

    // --- grab: the thumb does not jump to the cursor -------------------
    int ty = thumb_y(&b, &th);
    int grab_y = ty + th / 2;
    int r = uui_sbar_press(&b, bx, grab_y);
    oki("a press on the thumb is the bar's", (r & UUI_SBAR_TOOK) != 0, 1);
    oki("...and grabs it where it landed", b.grab, grab_y - ty);
    r = uui_sbar_motion(&b, bx, grab_y, 1);
    oki("a motion that has not moved scrolls nothing", b.top, 0);
    oki("...and says so", (r & UUI_SBAR_MOVED) != 0, 0);

    // --- the drag is absolute, clamps, and comes back without drift ----
    uui_sbar_motion(&b, bx, grab_y + H, 1);
    oki("dragged past the end: the last page", b.top, TOTAL - VISIBLE);
    uui_sbar_motion(&b, bx + 500, grab_y + 60, 1);
    int mid = b.top;
    utest_check_detail(mid > 0 && mid < TOTAL - VISIBLE, "a drag off the strip still tracks y",
                       "top did not land between the ends");
    uui_sbar_motion(&b, bx, grab_y, 1);
    oki("dragged back to where it began: the start again", b.top, 0);
    r = uui_sbar_release(&b);
    oki("a release lets go", b.grab, -1);
    oki("...and is the bar's", (r & UUI_SBAR_TOOK) != 0, 1);
    oki("a second release is nobody's", uui_sbar_release(&b), 0);

    // --- the trough pages, keeping a line ------------------------------
    ty = thumb_y(&b, &th);
    r = uui_sbar_press(&b, bx, ty + th + 10);
    oki("a press below the thumb pages down by a page less a line", b.top, VISIBLE - 1);
    oki("...and moved the view", (r & UUI_SBAR_MOVED) != 0, 1);
    oki("...and grabbed nothing", b.grab, -1);
    ty = thumb_y(&b, &th);
    uui_sbar_press(&b, bx, ty - 3);
    oki("a press above it pages back", b.top, 0);
    b.step = 4;
    ty = thumb_y(&b, &th);
    uui_sbar_press(&b, bx, ty + th + 10);
    oki("a widget's own line size sets the overlap", b.top, VISIBLE - 4);
    b.step = 1;
    uui_sbar_set(&b, TOTAL, VISIBLE, 0);

    // --- hover is the strip's, and only the strip's --------------------
    r = uui_sbar_motion(&b, X - 5, Y + 10, 0);
    oki("a motion beside the strip is not the bar's", r, 0);
    r = uui_sbar_motion(&b, bx, Y + 10, 0);
    oki("onto the strip: hovered", b.hover, 1);
    oki("...the bar's, and it redraws", r, UUI_SBAR_TOOK | UUI_SBAR_REDRAW);
    r = uui_sbar_motion(&b, X - 5, Y + 10, 0);
    oki("off again: not hovered, and it redraws", b.hover * 10 + r, UUI_SBAR_REDRAW);

    // --- nothing to scroll: nothing there -------------------------------
    uui_sbar_set(&b, VISIBLE, VISIBLE, 0);
    oki("content that fits shows no bar", uui_sbar_shown(&b), 0);
    oki("...and a press there is not the bar's", uui_sbar_press(&b, bx, Y + 10), 0);

    return utest_end();
}
