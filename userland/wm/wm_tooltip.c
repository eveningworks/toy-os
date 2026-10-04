// See wm_tooltip.h.
#include "wm_tooltip.h"
#include "wm_overlay.h"
#include "wm_internal.h"
#include "ui/uui.h"
#include "ui/uui_label.h"
#include "ui/uui_toolbar.h"   // UUI_TOOLTIP_DELAY_TICKS -- one delay for the desktop
#include "ui/utheme.h"
#include "kapi.h"
#include "rt/sys.h"

int wm_tooltip_open = 0;

// What is being tracked. The TEXT IS COPIED, not pointed at: a caller
// hands over a `.desktop` entry's comment, and the registry that owns
// that string is rewritten in place on every reload -- a pointer would
// be reading a different app's words a moment later, or freed storage.
#define TIP_TEXT_MAX 160
static char tip_text[TIP_TEXT_MAX];
static int tip_x, tip_y, tip_w, tip_h;      // the rect being hovered
static uint64_t hot_since;                  // when the pointer arrived, in ticks

// Where it is DRAWN, kept from the last draw so damage() can declare it
// after it has been hidden -- the rows it just vacated, exactly as the
// Start menu does.
static int box_x, box_y, box_w, box_h;

// Long enough to read, short enough that a wordy entry cannot make a
// panel-wide banner. Wrapped to this and then capped at two lines,
// which is what every desktop's tooltip does with a sentence.
#define TIP_MAX_LINES 2
static int tip_max_w(void) { return screen_w / 3; }

void wm_tooltip_cancel(void) {
    if (wm_tooltip_open) {
        wm_tooltip_damage();
        redraw_pending = 1;
    }
    wm_tooltip_open = 0;
    tip_text[0] = '\0';
    hot_since = 0;
}

void wm_tooltip_track(const char *text, int x, int y, int w, int h) {
    // Nothing under a modal overlay says what it is.
    if (wm_overlay_modal_open()) { wm_tooltip_cancel(); return; }
    if (!text || !text[0]) { wm_tooltip_cancel(); return; }

    // THE SAME THING IS NOT A NEW THING. Re-arming on every frame would
    // mean the delay never elapsed and the tooltip never appeared,
    // which is how this fails if the comparison is forgotten.
    if (k_strcmp(tip_text, text) == 0 && x == tip_x && y == tip_y) return;

    if (wm_tooltip_open) {
        // Moving to another row takes the old one down FIRST -- a
        // tooltip whose text changed under it reads as a glitch, and
        // the rect it vacates has to be declared either way.
        wm_tooltip_damage();
        redraw_pending = 1;
    }
    k_strlcpy(tip_text, text, sizeof tip_text);
    tip_x = x; tip_y = y; tip_w = w; tip_h = h;
    hot_since = sys_ticks();
    wm_tooltip_open = 0;
}

// The box the text needs: up to TIP_MAX_LINES wrapped lines, as wide as
// the widest of them. Measured with the same wrapper the labels use, so
// a tooltip breaks a sentence where a caption would.
static void measure(int *out_w, int *out_h, int *out_lines) {
    int lines = 0, widest = 0;
    const char *rest = tip_text;
    char line[TIP_TEXT_MAX];
    while (*rest && lines < TIP_MAX_LINES) {
        rest = uui_label_wrap_next(rest, tip_max_w(), line, sizeof line);
        int n = ugfx_text_width(line);
        if (n > widest) widest = n;
        lines++;
    }
    if (!lines) lines = 1;
    *out_lines = lines;
    *out_w = widest + 12;
    *out_h = lines * ugfx_char_h() + 8;
}

void wm_tooltip_update(void) {
    if (!tip_text[0] || wm_tooltip_open) return;
    if (sys_ticks() - hot_since < UUI_TOOLTIP_DELAY_TICKS) return;

    int w, h, lines;
    measure(&w, &h, &lines);
    // BELOW-RIGHT OF THE ROW, not of the pointer: the pointer is inside
    // the row and a box that follows it exactly would sit under the
    // hand. Placed through the same clamp every panel popup uses
    // (wm_overlay.h), so it cannot leave the work area or cover the
    // taskbar.
    wm_popup_place(tip_x + tip_w / 2, tip_y + tip_h + 4, w, h, &box_x, &box_y);
    box_w = w;
    box_h = h;
    wm_tooltip_open = 1;
    wm_tooltip_damage();
    redraw_pending = 1;
}

int wm_tooltip_rect(int *x, int *y, int *w, int *h) {
    if (box_w <= 0 || box_h <= 0) return 0;   // nothing shown
    *x = box_x; *y = box_y; *w = box_w; *h = box_h;
    return 1;
}

void wm_tooltip_damage(void) { wm_overlay_damage("tooltip"); }

void wm_tooltip_draw(int mx, int my) {
    if (!wm_tooltip_open) return;
    (void)mx; (void)my;   // it is placed from the ROW, not from the pointer

    // A FIELD-COLOURED BOX WITH AN OUTLINE, which is what a tooltip is
    // everywhere: it must not look like a menu (those take clicks) or
    // like a window (those have chrome).
    ugfx_fill_rect(wm_surface(), box_x, box_y, box_w, box_h, UTHEME_WHITE);
    ugfx_draw_rect(wm_surface(), box_x, box_y, box_w, box_h, UTHEME_OUTLINE);

    const char *rest = tip_text;
    char line[TIP_TEXT_MAX];
    for (int n = 0; n < TIP_MAX_LINES && *rest; n++) {
        rest = uui_label_wrap_next(rest, tip_max_w(), line, sizeof line);
        // The LAST line of a text that still has more is elided, so a
        // tooltip cannot lie about being complete either.
        int y = box_y + 4 + n * ugfx_char_h();
        if (n == TIP_MAX_LINES - 1 && *rest)
            ugfx_draw_string_elided(wm_surface(), box_x + 6, y, box_w - 12,
                                    line, UTHEME_TEXT, UTHEME_WHITE);
        else
            ugfx_draw_string_clipped(wm_surface(), box_x + 6, y, box_w - 12,
                                     line, UTHEME_TEXT, UTHEME_WHITE);
    }
}

int wm_tooltip_state(const char **text, int *x, int *y, int *w, int *h) {
    if (!wm_tooltip_open) return 0;
    if (text) *text = tip_text;
    if (x) *x = box_x;
    if (y) *y = box_y;
    if (w) *w = box_w;
    if (h) *h = box_h;
    return 1;
}
