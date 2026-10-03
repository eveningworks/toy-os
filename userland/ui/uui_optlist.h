#ifndef UUI_OPTLIST_H
#define UUI_OPTLIST_H

// A scrolling list of OPTIONS, one per row: a checkbox, the option's
// name, an optional value field edited in place, and a one-line
// description. Windows' msconfig boot options have the shape (a box,
// some with a value: "Maximum memory"), as do Firefox's about:config rows.
//
// The caller owns the item array, as with uui_listbox and uui_tree; the
// widget writes `on`, `value` and `changed` into it. The value is edited
// through an embedded uui_textbox, so the toolkit keeps ONE line editor
// (ui/uui_edit.h).

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"
#include "ui/uui_scrollbar.h"
#include "ui/uui_scrollanim.h"
#include "ui/uui_textbox.h"

// The value lives in the embedded field, so the two share one bound.
#define UUI_OPTLIST_VALUE_MAX UUI_TEXTBOX_MAX

// What changed on a row since the app last asked, uui_optlist_take_change().
#define UUI_OPTLIST_CH_ON     0x1   // `on` was toggled
#define UUI_OPTLIST_CH_VALUE  0x2   // an edit committed a different value
#define UUI_OPTLIST_CH_SELECT 0x4   // the row became the selected one

struct uui_optlist_item {
    const char *name;
    const char *desc;     // one line of secondary text; NULL for none
    const char *hint;     // greyed in an EMPTY value field ("<W>x<H>"); NULL for none
    int has_value;        // 0 = a plain on/off option, no field drawn
    int on;
    char value[UUI_OPTLIST_VALUE_MAX];
    unsigned changed;     // OWNED: UUI_OPTLIST_CH_* bits not yet taken
};

struct uui_optlist {
    int x, y, w, h;
    struct uui_optlist_item *items;   // caller-owned
    int count;
    int selected;     // or -1
    int hovered;      // OWNED
    int focused;      // OWNED -- set_focused
    int top;          // first visible row; OWNED
    int row_h;        // 0 = derive from the font
    int value_chars;  // the value column's width in characters; 0 = 16
    int bar_w;
    int thumb_grab;   // OWNED: -1, or the grab offset within the thumb
    struct uui_scrollanim anim;       // OWNED

    // The inline edit: the row being edited, or -1. The field is drawn
    // and placed by the widget; `value` changes only on a commit.
    int editing;      // OWNED
    int edit_drag;    // OWNED: a press in the field is extending a selection
    struct uui_textbox edit;          // OWNED

    uint32_t bg, fg;  // UUI_COLOR_UNSET = the theme's (utheme.h)
};

void uui_optlist_init(struct uui_optlist *ol, struct uui_optlist_item *items, int count);
// The array was rebuilt or resized: clamps the selection and the scroll,
// and drops an edit in progress without committing it.
void uui_optlist_set_items(struct uui_optlist *ol, struct uui_optlist_item *items, int count);

int  uui_optlist_row_h(const struct uui_optlist *ol);
int  uui_optlist_visible_rows(const struct uui_optlist *ol);
int  uui_optlist_scrollbar_visible(const struct uui_optlist *ol);

// Preferred minimum: every column, the description capped, and every row.
// Given less height the list scrolls.
void uui_optlist_natural_size(const struct uui_optlist *ol, int *out_w, int *out_h);
void uui_optlist_draw(struct ugfx_surface *s, const struct uui_optlist *ol);

// Row index at (cx, cy), or -1 outside, below the last row or on the bar.
int  uui_optlist_hit(const struct uui_optlist *ol, int cx, int cy);
// A row's checkbox and value field, content-relative, where they are drawn
// now; 0 when the row is scrolled out of sight or (value) has none.
int  uui_optlist_check_rect(const struct uui_optlist *ol, int row,
                            int *x, int *y, int *w, int *h);
int  uui_optlist_value_rect(const struct uui_optlist *ol, int row,
                            int *x, int *y, int *w, int *h);

// Each returns 1 if anything changed. Changes are queued on the item.
int  uui_optlist_toggle(struct uui_optlist *ol, int row);
int  uui_optlist_select(struct uui_optlist *ol, int row);
// Starts editing `row`'s value with all of it selected; 0 if it has none.
int  uui_optlist_begin_edit(struct uui_optlist *ol, int row);
// Adopts the typed value. A non-empty value committed into an OFF row
// turns it on. A no-op when nothing is being edited.
void uui_optlist_commit_edit(struct uui_optlist *ol);
void uui_optlist_cancel_edit(struct uui_optlist *ol);

// The next row with a change the app has not taken, and what changed
// (UUI_OPTLIST_CH_*); 0 when there is none. Taking clears it. Call it in
// a loop from on_widget: one press can commit an edit on one row and
// toggle another. A commit by FOCUS LEAVING (Tab, a click on another
// focusable) is reported to no id, so drain in on_key/on_press too if
// that matters.
int  uui_optlist_take_change(struct uui_optlist *ol, int *out_row);

// Arrows, Home/End, PgUp/PgDn move; Space toggles; Enter edits a row
// with a value. While editing: Enter commits, Esc cancels, everything
// else goes to the field.
int  uui_optlist_key(struct uui_optlist *ol, int key, unsigned mods);
int  uui_optlist_wheel(struct uui_optlist *ol, int notches);

struct uui_widget_ops;
extern const struct uui_widget_ops uui_optlist_ops;

#endif
