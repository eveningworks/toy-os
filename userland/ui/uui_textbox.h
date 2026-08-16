#ifndef UUI_TEXTBOX_H
#define UUI_TEXTBOX_H

// Renamed from `uui_field` so the two sides of one widget share a name
// as well as a path: the kernel's version is apps/ui/ui_textbox.h.
// Matching paths were the point of splitting the toolkit one file per
// widget, and mismatched names undercut that -- porting between the two
// should be a file-to-file comparison, not a translation.

#include <stdint.h>
#include "ui/uui_edit.h"
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"


// Split out of the single uwidgets.c/.h this used to be, one file per
// widget -- the same shape as apps/ui/, so a widget's kernel-side and
// ring-3 versions live at matching paths. See ui/uui.h.

// --- single-line text field -------------------------------------------

#define UUI_TEXTBOX_MAX 48

struct uui_textbox {
    // Content-relative geometry -- see apps/ui/ui_radio_list.h's note.
    // Unlike the radio list, a field CAN be stretched, so w/h are
    // whatever set_geometry is told (its natural width is 0, meaning
    // "no preference" -- see uui_primitives.h).
    int x, y, w, h;

    char buf[UUI_TEXTBOX_MAX]; // NUL-terminated
    int len;

    // Caret and selection, and the standard keymap that goes with them
    // (Ctrl+A, Shift+arrows, typing replaces the selection...) -- see
    // ui/uui_edit.h. Shared with the multi-line editor so a field and a
    // document cannot behave differently.
    //
    // `ed.cursor` is what `cursor` used to be; reach through it rather
    // than keeping a second copy.
    struct uui_edit ed;

    int active; // 1 = focused: draws a caret and accepts keys

    // The widget's OWN colours, defaulted at init from the theme. They
    // used to be arguments to draw(), which meant the toolkit could not
    // draw a field on an app's behalf -- the generic draw slot has
    // nowhere to carry three colours. An app that wants different ones
    // assigns them after init.
    uint32_t bg, fg, border, sel_bg;
};

void uui_textbox_init(struct uui_textbox *f, const char *initial);
void uui_textbox_set_active(struct uui_textbox *f, int active);

// Returns 1 if the key was consumed. Deliberately does NOT consume
// Enter: "commit this field" is the caller's decision, not the widget's.
//
// `mods` is passed through to the edit core; Shift arrives as its own
// key code rather than a bit (api/keyboard.h), so most callers can pass
// 0 and lose nothing.
int uui_textbox_key(struct uui_textbox *f, int key);
int uui_textbox_key_mods(struct uui_textbox *f, int key, unsigned mods);

// The text, and how much of it is selected. For an app that wants to
// read a field without knowing about the edit core.
const char *uui_textbox_text(const struct uui_textbox *f);
int uui_textbox_has_selection(const struct uui_textbox *f);

// Height only: one row plus insets. **Width is 0 -- no preference**
// (see uui_primitives.h): a field's width is whatever the form gives it.
void uui_textbox_natural_size(const struct uui_textbox *f, int *out_w, int *out_h);

// Draws the field, scrolling its content horizontally so the caret
// stays visible -- typing past the right edge behaves like a real text
// input rather than drawing through the border.
void uui_textbox_set_geometry(struct uui_textbox *f, int x, int y, int w, int h);

// Point (cx, cy) inside the field, content-relative.
int uui_textbox_hit(const struct uui_textbox *f, int cx, int cy);

// The character index a click at `cx` lands on -- the inverse of the
// placement draw() uses, including its horizontal scrolling, so the
// caret goes where the glyph is rather than near it. Rounds to the
// nearest gap, so clicking a glyph's right half lands after it.
int uui_textbox_index_at_x(const struct uui_textbox *f, int cx);

void uui_textbox_draw(struct ugfx_surface *s, const struct uui_textbox *f);

// The focus ring's ops table for a field: hit/key/set_focused only, the
// drawing and layout slots left NULL (every slot is optional -- see
// uui_widget.h). A field's draw takes three colours the generic
// signature cannot carry, so a full table here would be half-honest.
struct uui_widget_ops;
extern const struct uui_widget_ops uui_textbox_focus_ops;

// Full table with ROUTED POINTER INPUT (ui/uui_route.h): a press places
// the caret. Everything a text field does with the KEYBOARD is still
// uui_textbox_key()'s.
extern const struct uui_widget_ops uui_textbox_ops;

#endif
