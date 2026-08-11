#ifndef UI_TEXTBOX_H
#define UI_TEXTBOX_H
#include <stdint.h>
#include "widgets.h"

// A retained textbox object -- owns its own geometry plus the
// widgets.h struct text_field it wraps, the same "object owns its
// state, positioned separately from its identity" shape ui_button.h
// already established (see that file's top comment for the full
// design writeup -- not repeated here). widgets.h's widget_textfield_*
// functions are still what actually does the drawing/editing; this is
// just the same kind of thin retained-object shell around them that
// ui_button.h is around widget_button().
//
// First real caller: apps/notepad.c's filename field, migrated off
// raw struct text_field + widget_textfield_*() calls -- see
// docs/decisions.md.
struct ui_textbox {
    // Content-relative geometry, same convention as ui_button.
    int x, y, w, h;
    struct text_field field; // the actual text state -- buf/len/cursor/active (widgets.h)
    uint32_t bg, fg, border;
};

// Sets geometry, colors, and initial text; resets field state (cursor
// at end, inactive). Call once when the textbox is created (e.g.
// notepad_open()), not every frame -- same "don't wipe live state every
// redraw" reasoning as ui_button_init(). `initial` may be NULL for an
// empty field.
void ui_textbox_init(struct ui_textbox *tbx, int x, int y, int w, int h,
                      const char *initial, uint32_t bg, uint32_t fg, uint32_t border);

// Updates only x/y/w/h -- leaves buf/cursor/active/colors untouched.
// What a per-frame relayout (font size changed, window resized) should
// call instead of ui_textbox_init(), same contract as
// ui_button_set_geometry().
void ui_textbox_set_geometry(struct ui_textbox *tbx, int x, int y, int w, int h);

void ui_textbox_draw(const struct ui_textbox *tbx, int origin_x, int origin_y);
int ui_textbox_hit(const struct ui_textbox *tbx, int cx, int cy);

// Thin wrappers over widgets.h's widget_textfield_set_active()/_key() --
// exist so a caller touches ui_textbox consistently through its own
// API rather than reaching into tbx->field directly (mirrors how
// ui_button never exposes widget_button() itself). Same
// activate-on-click / deactivate-on-Enter-or-click-elsewhere contract
// as widget_textfield_* -- the caller still decides when to call these,
// this doesn't change that.
void ui_textbox_set_active(struct ui_textbox *tbx, int active);
int ui_textbox_key(struct ui_textbox *tbx, int key);

#endif
