// The ring-3 toolkit's theme -- see ui/utheme.h. One live palette plus
// font-derived metrics, read by every widget through utheme_current()
// and the utheme_*() metric calls.
#include "ui/utheme.h"
#include "ui/ugfx.h"

static struct utheme g_theme;
static int g_inited;

// The default (light) palette. Values are apps/theme.h's, unchanged --
// filled at runtime because ugfx_rgb() is a function, not a constant.
void utheme_default(struct utheme *out) {
    out->window_bg   = ugfx_rgb(235, 235, 235);
    out->panel_bg    = ugfx_rgb(245, 245, 245);
    out->control_bg  = ugfx_rgb(225, 225, 230);
    out->field_bg    = ugfx_rgb(255, 255, 255);
    out->text        = ugfx_rgb(20, 20, 20);
    out->border      = ugfx_rgb(60, 60, 60);
    // The selection blue the desktop and widgets already draw with
    // (icon selection, table rows). A role now, so one place owns it.
    out->accent      = ugfx_rgb(70, 110, 160);
    out->accent_text = ugfx_rgb(255, 255, 255);
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
