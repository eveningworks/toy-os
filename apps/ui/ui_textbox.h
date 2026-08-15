#ifndef UI_TEXTBOX_H
#define UI_TEXTBOX_H
#include <stdint.h>

struct ui_focus_ops;

// A retained textbox object -- owns its own geometry plus a text_field
// (buf/len/cursor/active), the same "object owns its state, positioned
// separately from its identity" shape ui_button.h already established
// (see that file's top comment for the full design writeup -- not
// repeated here).
//
// The underlying single-line text-editing implementation
// (struct text_field, widget_textfield_*()) used to live in
// apps/widgets.c/.h as a standalone layer ui_textbox merely wrapped;
// now that ui_textbox is (and always was) its only real caller, the
// implementation moved in here directly rather than staying split
// across two files for no one -- see docs/decisions.md. The functions
// keep their old widget_textfield_* names on purpose (no benefit to
// renaming call sites that don't exist anywhere else).
//
// First real caller: apps/notepad.c's filename field, migrated off a
// raw struct text_field + widget_textfield_*() calls -- see
// docs/decisions.md.

#define TEXTFIELD_MAX 48

struct text_field {
    char buf[TEXTFIELD_MAX]; // NUL-terminated, always <= TEXTFIELD_MAX - 1 chars
    int len;    // k_strlen(buf), kept in sync so callers don't have to recompute it
    int cursor; // [0, len] -- caret position; insert/backspace/delete operate here
    int active; // 1 = has focus (draws a caret, accepts keys); 0 = inert, plain text
};

// Copies `initial` into buf (truncated to fit if longer than
// TEXTFIELD_MAX - 1), cursor at the end, active = 0. `initial` may be
// NULL for an empty field.
void widget_textfield_init(struct text_field *tf, const char *initial);

// Sets `active` directly -- the owning app decides when to
// activate/deactivate (typically: a click on the field's rect activates
// it via ui_textbox_hit(); Enter, or a click elsewhere, deactivates it).
void widget_textfield_set_active(struct text_field *tf, int active);

// Handles one key while `tf->active`: printable ASCII (32-126) inserts
// at the cursor, backspace/delete edit around it, left/right/home/end
// move it. Returns 1 if the key was handled (caller should redraw), 0
// if it wasn't recognized -- notably, '\r'/'\n' falls through
// unhandled, since committing/deactivating on Enter is the caller's
// decision, not this widget's. Always returns 0 when `tf->active` is 0.
int widget_textfield_key(struct text_field *tf, int key);

// Fills (x, y, w, h) with `bg`, draws a 1px border in `border`, and
// draws whichever slice of `tf->buf` fits inside the padded interior in
// `fg`, plus a caret at `tf->cursor` if `tf->active`. Text longer than
// fits `w` doesn't overflow past the border -- this function clips
// itself, sliding the visible window just far enough to keep the
// cursor in view while active. See docs/decisions.md.
void widget_textfield_draw(int x, int y, int w, int h, const struct text_field *tf,
                            uint32_t bg, uint32_t fg, uint32_t border);

// ---- the retained object itself ----

struct ui_textbox {
    // Content-relative geometry, same convention as ui_button.
    int x, y, w, h;
    struct text_field field; // the actual text state -- buf/len/cursor/active
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

// Height only: one row of text plus its insets. **Width is 0 -- no
// preference** (see ui_primitives.h). A field's width is whatever the
// form gives it; twenty columns would be an invented number that
// nothing honours once a layout stretches it.
void ui_textbox_natural_size(const struct ui_textbox *tbx, int *out_w, int *out_h);

void ui_textbox_draw(const struct ui_textbox *tbx, int origin_x, int origin_y);
int ui_textbox_hit(const struct ui_textbox *tbx, int cx, int cy);

// Thin wrappers over widget_textfield_set_active()/_key() -- exist so a
// caller touches ui_textbox consistently through its own API rather
// than reaching into tbx->field directly (mirrors how ui_button never
// exposes widget_button() itself).
void ui_textbox_set_active(struct ui_textbox *tbx, int active);
int ui_textbox_key(struct ui_textbox *tbx, int key);

// Joins a ui_focus ring (see ui_focus.h). Focus drives `active` here, so
// a focused textbox shows its caret and takes keys without the app
// having to keep the two in step by hand.
extern const struct ui_focus_ops ui_textbox_focus_ops;

#endif
