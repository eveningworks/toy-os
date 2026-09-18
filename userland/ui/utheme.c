// The ring-3 toolkit's theme -- see ui/utheme.h. One live palette plus
// font-derived metrics, read by every widget through utheme_current()
// and the utheme_*() metric calls.
#include "ui/utheme.h"
#include "ui/ugfx.h"

static struct utheme g_theme;
static int g_inited;

// The default (light) palette, filled at runtime because ugfx_rgb() is
// a function, not a constant.
//
// The greys are ordered: field_bg is the brightest thing on a page and
// the page (panel_bg) sits far enough below it that a text box reads as
// a box without its outline -- macOS's and Windows 10's spacing, not
// Breeze's, whose buttons are lighter than the ground; here a face is a
// step DARKER than the page, and bars a step darker again. Hover and
// press derive from these by luminance (uui_state_bg), so nothing else
// moves when a rung does.
void utheme_default(struct utheme *out) {
    out->window_bg   = ugfx_rgb(228, 228, 228);
    out->panel_bg    = ugfx_rgb(236, 236, 236);
    out->control_bg  = ugfx_rgb(220, 220, 226);
    out->field_bg    = ugfx_rgb(255, 255, 255);
    out->bar_bg      = ugfx_rgb(226, 226, 229);
    out->text        = ugfx_rgb(20, 20, 20);
    out->border      = ugfx_rgb(60, 60, 60);
    // The selection blue the desktop and widgets already draw with
    // (icon selection, table rows). A role now, so one place owns it.
    out->accent      = ugfx_rgb(70, 110, 160);
    out->accent_text = ugfx_rgb(255, 255, 255);
    // Darker than the strip ground (window_bg) as well as the control
    // face, so a resting tab reads as recessed and the selected one --
    // field_bg -- as raised out of it.
    out->tab_rest    = ugfx_rgb(200, 200, 207);
    // The two the widgets had been spelling out by hand: this outline
    // appeared in eight of them and this selection tint in six.
    out->outline     = ugfx_rgb(150, 155, 165);
    out->selection_bg = ugfx_rgb(205, 220, 240);
    out->separator   = ugfx_rgb(205, 205, 210);
}

void utheme_init(void) {
    utheme_default(&g_theme);
    g_inited = 1;
}

const struct utheme *utheme_current(void) {
    // Lazy, so a UTHEME_* colour used before uapp_run() calls
    // utheme_init() still resolves to the default rather than to zero
    // (which is black -- a silent, ugly failure).
    if (!g_inited) utheme_init();
    return &g_theme;
}

void utheme_set(const struct utheme *t) {
    g_theme = *t;
    g_inited = 1;
}

// --- metrics: derived from the font, clamped for before-font-up -------

// The line height, or a sane floor when the font is not fetched yet.
static int ch(void) {
    int h = ugfx_char_h();
    return h > 0 ? h : 14;
}

int utheme_indicator(void) { return ch(); }          // box ~ one line tall
int utheme_pad(void)       { return ch() / 2 + 1; }  // ~8 at a 14px line
int utheme_gap(void)       { return utheme_pad(); }
int utheme_border_w(void)  { return 1; }
int utheme_control_h(void) { return ch() + utheme_pad(); }
int utheme_focus_w(void)   { return 2; }
