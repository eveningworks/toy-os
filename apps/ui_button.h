#ifndef UI_BUTTON_H
#define UI_BUTTON_H
#include <stdint.h>

// A single self-contained button object -- owns its own geometry, label,
// colors, and (new, as of this file) its own `pressed` state, instead of
// widget_button() (widgets.h) being just a stateless draw call that
// leaves every app to hand-roll its own "which button is pressed"
// tracking and hit-testing (calculator.c used to do exactly this with a
// bare `int g_pressed_index` and a private button_at() loop -- see
// CHANGELOG.md and docs/decisions.md for the full writeup of why this
// file exists).
//
// Modeled on Brutal OS's libs/brutal-ui/button.c/.h -- a widget as an
// object that owns its own state and reacts to events, not just a
// function you call every frame -- but deliberately sized down to what
// toy-os's GUI actually needs: no generic UiView base class, no
// view-tree/mounting, no layout DSL, no hover tracking (the WM doesn't
// dispatch mouse-enter/leave today, only press/click/release -- see
// gui_apps.h). A bare struct ui_button is rarely used alone in practice;
// see ui_button_group.h for the part that manages a whole row/grid of
// these (hit-testing, "which one is currently down").
struct ui_button {
    // Geometry is CONTENT-RELATIVE (same convention gui_apps.h's
    // on_click/on_press cx/cy already use), not screen-absolute --
    // ui_button_draw() takes the window's content origin as a separate
    // parameter so hit-testing (ui_button_hit(), used from event
    // handlers that only ever see content-relative coordinates) never
    // needs to know it.
    int x, y, w, h;
    const char *label; // not owned/copied -- caller's string must outlive the button, same requirement widget_button() already had
    uint32_t bg, fg;
    int code;    // app-defined id delivered back on a completed click -- calculator.c uses calc_input()'s char codes, cast to/from int
    int pressed; // OWNED state, driven by ui_button_group_press()/_release() -- treat as read-only from app code
};

// Sets every field including `pressed` (cleared to 0) -- call once when
// a button is created/reset (e.g. calculator_open()), not every frame:
// geometry alone often changes every draw (font size can change live,
// see gfx_set_font_size()) without the button's *identity* resetting,
// so re-running ui_button_init() on every frame would wipe out
// in-progress press state right before it could ever be drawn. Use
// ui_button_set_geometry() for the "just repositioning" case instead.
void ui_button_init(struct ui_button *b, int x, int y, int w, int h,
                     const char *label, uint32_t bg, uint32_t fg, int code);

// Updates only x/y/w/h -- leaves label/bg/fg/code/pressed untouched.
// This is what a per-frame relayout (font size changed, window resized)
// should call, not ui_button_init().
void ui_button_set_geometry(struct ui_button *b, int x, int y, int w, int h);

void ui_button_draw(const struct ui_button *b, int origin_x, int origin_y);
int ui_button_hit(const struct ui_button *b, int cx, int cy);

#endif
