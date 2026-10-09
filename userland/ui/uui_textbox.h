#ifndef UUI_TEXTBOX_H
#define UUI_TEXTBOX_H

// Renamed from `uui_field` so the two sides of one widget share a name
// as well as a path: the kernel's version is apps/ui/ui_textbox.h.
// Matching paths were the point of splitting the toolkit one file per
// widget, and mismatched names undercut that -- porting between the two
// should be a file-to-file comparison, not a translation.

#include <stdint.h>
#include "ui/uui_edit.h"
#include "ui/uui_undo.h"
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"


// Split out of the single uwidgets.c/.h this used to be, one file per
// widget -- the same shape as apps/ui/, so a widget's kernel-side and
// ring-3 versions live at matching paths. See ui/uui.h.

// --- single-line text field -------------------------------------------

// Room for a whole setting value (SETTING_ABI_VALUE_MAX, asserted in
// System Settings) and for a pasted `sha256sum` line -- 64 hex digits,
// two spaces and a name -- which Properties compares. A shorter field
// truncates what it is handed: a silent wrong answer, not a full field.
#define UUI_TEXTBOX_MAX 128

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

    // Every field has an edit history (Ctrl+Z / Ctrl+Y), in storage of
    // its own: a field's text is small, so a few hundred bytes hold many
    // steps. Re-pointed at each key, so a struct copied by value still
    // records into its own copy.
    struct uui_undo undo;
    unsigned char undo_mem[512];

    int active; // 1 = focused: draws a caret and accepts keys

    // 1 = greyed and inert, as on every other control here. A field with
    // no such flag was the odd one out, and System Settings needs it:
    // a setting the registry has made unavailable must READ as
    // unavailable, and a live-looking field that silently refuses what
    // is typed into it is the failure the `unavailable` sentence exists
    // to prevent.
    int disabled;

    // The widget's OWN colours, defaulted at init from the theme. They
    // used to be arguments to draw(), which meant the toolkit could not
    // draw a field on an app's behalf -- the generic draw slot has
    // nowhere to carry three colours. An app that wants different ones
    // assigns them after init.
    uint32_t bg, fg, border, sel_bg;

    // Shown dimmed while the field is EMPTY ("Find a setting"); NULL for
    // none. A hint, never a value: it is not in `buf`.
    const char *placeholder;

    // 1 = drawn INSIDE a container's own frame (the Start menu's search
    // field), which shows that the field is listening: no focus ring
    // here, or the frame reads as doubled. Zero, as init leaves it, is
    // every ordinary field.
    int bare;

    // 1 = a password: every character is drawn as '*' (the font is
    // Latin-1, which has no bullet), and clicks place the caret by those.
    // The text itself is unchanged in `buf`.
    int masked;
};

void uui_textbox_init(struct uui_textbox *f, const char *initial);
void uui_textbox_set_active(struct uui_textbox *f, int active);
// Replace what the field says and leave everything else -- crucially
// `active`, which init() clears. Re-initing a field the focus ring
// points at leaves the two disagreeing and the field unclickable.
void uui_textbox_set_text(struct uui_textbox *f, const char *text);
// Selects [start, end) with the caret at `end` -- a rename selecting the
// name and leaving the extension, which typing then replaces.
void uui_textbox_select(struct uui_textbox *f, int start, int end);

// Returns 1 if the key was consumed. Deliberately does NOT consume
// Enter: "commit this field" is the caller's decision, not the widget's.
//
// `mods` is passed through to the edit core; Shift arrives as its own
// key code rather than a bit (api/keyboard.h), so most callers can pass
// 0 and lose nothing.
int uui_textbox_key(struct uui_textbox *f, int key);
int uui_textbox_key_mods(struct uui_textbox *f, int key, unsigned mods);

// Types `s` at the caret, over any selection, as one undoable edit --
// what a button that inserts a token does. All of it or nothing: 0 when
// it would not fit, or the field is disabled. Works whether or not the
// field has the focus.
int uui_textbox_insert(struct uui_textbox *f, const char *s);

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
// A field's colours, RESOLVED -- read these, never the struct fields,
// which hold UUI_COLOR_UNSET until they are drawn (utheme.h).
uint32_t uui_textbox_c_bg(const struct uui_textbox *f);
uint32_t uui_textbox_c_fg(const struct uui_textbox *f);
uint32_t uui_textbox_c_border(const struct uui_textbox *f);

int uui_textbox_hit(const struct uui_textbox *f, int cx, int cy);

// The character index a click at `cx` lands on -- the inverse of the
// placement draw() uses, including its horizontal scrolling, so the
// caret goes where the glyph is rather than near it. Rounds to the
// nearest gap, so clicking a glyph's right half lands after it.
int uui_textbox_index_at_x(const struct uui_textbox *f, int cx);

// The edit_target slot (ui/uui_widget.h), for a composite whose inner
// field it is: the field at (cx, cy), or with UUI_NOWHERE the field if
// it is active. 0 when disabled or not there.
int uui_textbox_edit_target(struct uui_textbox *f, int cx, int cy,
                            struct uui_edit_target *out);

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
