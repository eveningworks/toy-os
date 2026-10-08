#ifndef UUI_PROPS_H
#define UUI_PROPS_H

// uui_props -- a page of PROPERTIES: titled sections of key/value rows,
// one scrolling column. What Device Manager's pane is; the shape
// KDE Info Center and Windows' Properties pages share. Generic by
// nature, so it lives here from its first caller (CLAUDE.md).
//
// A VALUE THAT DOES NOT FIT WRAPS, at spaces, up to four lines -- a
// reason sentence elided at the pane's edge says nothing.
//
// A section may carry an ACTION, a link at its title's right ("Copy
// details"); a click on it is read with uui_props_take_action(). A
// NOTICE section is a tinted box holding its rows' text, no title row --
// a problem the page leads with.
//
// SLOTS are how an app puts its own controls in: a section reserves a
// height (under its title, above its rows) and uui_props_slot_rect()
// says where -- 1 only while the WHOLE slot is in view, so the app hides
// a control the scroll has half covered rather than drawing it over the
// page's edge.
//
// TALLER THAN ITS RECT, IT SCROLLS (the wheel), with a slim thumb at the
// right edge. uui_props_begin() keeps the scroll when asked, so a page
// rebuilt for the same subject does not jump back to the top.

#include <stdint.h>
#include "ui/ugfx.h"

#define UUI_PROPS_SECTIONS 12
#define UUI_PROPS_ROWS     72
#define UUI_PROPS_KEY      20
#define UUI_PROPS_VALUE    120

enum { UUI_PROPS_PLAIN = 0, UUI_PROPS_NOTICE = 1 };

struct uui_props_row {
    char key[UUI_PROPS_KEY];      // "" for a sentence that spans the row
    char val[UUI_PROPS_VALUE];
};

struct uui_props_section {
    char title[24];
    int kind;                     // UUI_PROPS_PLAIN or UUI_PROPS_NOTICE
    char action[28];              // the link at the title's right, or ""
    int first, nrows;             // rows[first .. first + nrows)
    int slot_id, slot_h;          // the app's controls; 0 for none
};

struct uui_props {
    int x, y, w, h;
    uint32_t bg;                  // the page's ground; UTHEME_PANEL_BG by default
    struct uui_props_section sec[UUI_PROPS_SECTIONS];
    int nsec;
    struct uui_props_row row[UUI_PROPS_ROWS];
    int nrow;
    int scroll;                   // px scrolled; OWNED
    int hot, armed;               // the action link under the pointer / pressed; -1 none. OWNED
    int action;                   // the section whose link was clicked, -1; take_action(). OWNED
    // The wrapped layout, measured once per width -- every row's line
    // count and every section's height. -1 width: measure again. OWNED
    int laid_w;
    unsigned char lines[UUI_PROPS_ROWS];
    int sec_h[UUI_PROPS_SECTIONS];
    int key_w;
};

void uui_props_init(struct uui_props *w);
// Empty it, ready to be filled; `keep_scroll` 0 goes back to the top.
void uui_props_begin(struct uui_props *w, int keep_scroll);
// A new section, and the rows added after it are its. Returns its index,
// or -1 when there is no room.
int  uui_props_section(struct uui_props *w, const char *title, int kind);
void uui_props_set_action(struct uui_props *w, int section, const char *link);
void uui_props_row(struct uui_props *w, const char *key, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
// A slot of height `h` in `section`, for controls with app id `id`.
void uui_props_slot(struct uui_props *w, int section, int id, int h);
// Where slot `id` is: 1 with its rect when wholly in view, 0 otherwise.
int  uui_props_slot_rect(const struct uui_props *w, int id, int *x, int *y, int *ww, int *hh);
// Where slot `id` is wherever the scroll has put it, in view or not: 1
// with its rect, 0 when there is no such slot. For controls a uui_clip
// cuts to the pane (uui_clip.h), rather than hiding them until all of
// the slot shows.
int  uui_props_slot_place(const struct uui_props *w, int id, int *x, int *y, int *ww, int *hh);
// The section whose action link was clicked since the last call, or -1.
int  uui_props_take_action(struct uui_props *w);
// A section's title rect and its action link's, for a test to click; 0
// when it is not in view.
int  uui_props_action_rect(const struct uui_props *w, const char *title,
                           int *x, int *y, int *ww, int *hh);

// The page as text: each section's title, then "  key: value" -- what a
// Copy button puts on the clipboard. Returns the length, or -1 when it
// does not fit (and writes nothing).
int  uui_props_text(const struct uui_props *w, char *out, int cap);

// The height everything needs at `width`.
int  uui_props_height(const struct uui_props *w, int width);

void uui_props_natural_size(const struct uui_props *w, int *out_w, int *out_h);
void uui_props_set_geometry(struct uui_props *w, int x, int y, int width, int height);
void uui_props_draw(struct ugfx_surface *s, const struct uui_props *w);

struct uui_widget_ops;
extern const struct uui_widget_ops uui_props_ops;

#endif
