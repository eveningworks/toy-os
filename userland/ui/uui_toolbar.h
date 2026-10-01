#ifndef UUI_TOOLBAR_H
#define UUI_TOOLBAR_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_menubar.h" // UUI_MI_* -- see item_flags below

// uui_toolbar -- a strip of icon buttons under the menu bar.
//
// THE ITEMS ARE THE MENU'S COMMANDS, PRESENTED AGAIN. Win32's toolbar,
// Qt's QToolBar and GTK's all bind the same actions the menus commit
// (Qt literally hosts QActions), and this widget copies that shape the
// cheap way: an item carries a `code`, and enabled/checked state is
// ASKED through the SAME `item_flags(int code)` callback the menu bar
// uses -- one state source, two presenters, so a toolbar latch cannot
// disagree with its menu tick. A checked item draws PRESSED-IN
// (UUI_STATE_PRESSED's wash), the latched look every desktop uses for
// a view toggle.
//
// AN ICON IS A NAME (`icon_get()`), drawn at a font-derived size; a
// missing file leaves a plain button with no glyph rather than an
// error, the icon cache's own rule.
//
// TOOLTIPS RIDE THE APP'S TICK. A tooltip appears a moment after the
// pointer STOPS, which no input event announces -- something has to
// re-evaluate on time passing, and a widget cannot repaint the window.
// So hover only records when it started, and the app calls
// uui_toolbar_tick() from its on_tick, repainting when it returns 1.
// An app that never ticks simply never shows tooltips; every button
// still works.
//
// THE TIP IS ITS OWN POPUP SURFACE when the compositor grants one
// (ui/uui_popup.h), so it can leave the window entirely -- and it is
// opened with NO GRAB, because a tooltip that took the pointer would
// swallow the click meant for the button beneath it. Without a surface
// it falls back to `draw_overlay`, drawn in-window and clamped to the
// window's edges, which is all it could ever do there.
//
// OVERFLOW, OPT-IN (`overflow = 1`): what does not fit collapses FROM
// THE END into a menu behind one more button at the right edge --
// QToolBar's extension button, the Windows 11 command bar's "See more",
// KDE's ActionToolBar. The menu is a uui_menubar opened as a context
// menu, its rows the hidden items' labels (or tips) and `accel`s, its
// ticks and greying from the SAME item_flags, so an item cannot be
// latched in the bar and unticked in the menu. A commit from the menu
// parks its code exactly as a button's does. A folded item reports a
// ZERO-WIDTH rect under the More button -- a UUI_TB_MENU item's popup,
// anchored at it, opens there -- and a layout must hand the strip its real
// width (UUI_FILL_W in a column) -- natural_size still asks for all of it.

struct uui_toolbar_item {
    const char *icon; // icon_get() name; NULL makes this a SEPARATOR
    const char *tip;  // tooltip text; NULL = no tooltip
    int code;         // reported on commit, and the item_flags key
    // A COMMAND BAR's button carries its word beside the icon -- Windows
    // 11's "New", "Sort", "View" -- and NULL keeps it icon-only. After
    // `code`, so a three-field initialiser still means what it did.
    const char *label;
    unsigned flags;   // UUI_TB_*
    // The key that does the same, shown beside the row when the item
    // has overflowed into the menu ("A", "Ctrl+R"). A label only, as
    // uui_menu_item's is: the app binds the key.
    const char *accel;
    // THE ICON'S COLOUR, drawn as a symbolic icon (ugfx_blit_tinted()):
    // 0 keeps the icon's own ink. A colour-coded command bar -- Image
    // Viewer's zoom in teal, rotate in violet -- sets one per item. A
    // latched accent button draws it in the accent's text colour instead.
    uint32_t tint;
};

// A chevron after the label: the button opens a menu. The toolbar only
// DRAWS it -- the app opens the popup under uui_toolbar_item_rect() when
// the code commits, as Explorer's "Sort" and "View" do.
#define UUI_TB_MENU 0x01
// This item and every one after it sit at the strip's RIGHT end.
#define UUI_TB_END  0x02
// A READOUT, not a button: `label` drawn as plain text, never hit, and
// never offered in the overflow menu. The label may point at a buffer
// the app rewrites ("speed 3"); the strip re-measures every draw.
#define UUI_TB_TEXT 0x04

#define UUI_TOOLBAR_SEP { 0, 0, 0, 0, 0, 0, 0 }

// Rows the overflow menu can hold. Past it, the last items are unreachable.
#define UUI_TOOLBAR_MENU_MAX 24

// ~500ms at the PIT's 100 Hz -- every desktop delays about this long.
#define UUI_TOOLTIP_DELAY_TICKS 50

#define UUI_TIP_PAD 4 // inset around the tip's text
#define UUI_TIP_GAP 3 // between the button and the tip, in-window only

struct uui_toolbar {
    int x, y, w, h;

    const struct uui_toolbar_item *items; // caller-owned
    int count;

    // Asked per item, every draw and hit test; NULL means every item
    // is enabled and unlatched. Point it at the SAME function the menu
    // bar's item_flags uses.
    unsigned (*item_flags)(int code);

    int hot;      // hovered item, or -1; OWNED
    int armed;    // pressed item awaiting its release, or -1; OWNED

    // The commit, PARKED for uui_toolbar_take_code() -- the ops
    // release slot can only say "changed", the menu bar's arrangement.
    int committed;

    // Tooltip state -- see the header comment.
    unsigned long hot_since; // sys_ticks() when `hot` last changed
    int tip_shown;           // OWNED; flipped by uui_toolbar_tick()

    // The tooltip's own popup surface (ui/uui_popup.h), or 0 when it is
    // drawn in-window instead. Opened WITHOUT a grab: a tooltip that
    // took the pointer would eat the click meant for the button it is
    // describing. Opened from tick()/motion(), never from the draw op,
    // which is const.
    int tip_popup;

    uint32_t bg, fg, border, tip_bg, tip_fg;
    // Icons at the text's height and no strip border -- for a toolbar
    // living inside a status bar. Set after init.
    int compact;
    // Latched items FILLED in the theme's accent with its text colour,
    // rather than washed pressed-in -- for a strip whose latches are a
    // selection to read at a glance (Shapes' scene and view toggles),
    // where the grey wash barely differs from the bar. Set after init.
    int accent_latch;

    // --- overflow (see the header comment); set after init ---------------
    int overflow;
    // The button the hidden items collapse behind: "tb-more" with the
    // tip "See more" by default. Hover and press report it as index
    // `count`. An app may relabel it ("View").
    struct uui_toolbar_item more;
    // OWNED: the menu, and the rows it was opened with.
    struct uui_menubar menu;
    struct uui_menu_item menu_items[UUI_TOOLBAR_MENU_MAX];
    int menu_press;   // the grab's press went to the open menu
};

void uui_toolbar_init(struct uui_toolbar *t,
                       const struct uui_toolbar_item *items, int count);

// The strip wants one row of buttons; width is every item side by side.
void uui_toolbar_natural_size(const struct uui_toolbar *t, int *out_w, int *out_h);

// Its height alone -- what an app laying out by hand asks, and ten
// of them wrapped natural_size() to answer it.
static inline int uui_toolbar_height(const struct uui_toolbar *t) {
    int h = 0;
    uui_toolbar_natural_size(t, 0, &h);
    return h;
}

// Item `i`'s rect, for tests and layout logs. Returns 0 past the end;
// `i == count` is the overflow button, and 0 while it is not shown.
int  uui_toolbar_item_rect(const struct uui_toolbar *t, int i,
                            int *x, int *y, int *w, int *h);

// How many leading items are in the strip; the rest are in the menu.
int  uui_toolbar_shown(const struct uui_toolbar *t);

// Where an IN-WINDOW overflow menu may go -- the window's content rect;
// uui_menubar_set_bounds()'s meaning. A menu granted its own popup
// surface ignores it.
void uui_toolbar_set_bounds(struct uui_toolbar *t, int x, int y, int w, int h);

// Item index at (cx, cy), or -1. Separators and disabled items are -1.
int  uui_toolbar_hit_item(const struct uui_toolbar *t, int cx, int cy);

// 1 when tooltip visibility changed and the app should repaint. Call
// from on_tick.
int  uui_toolbar_tick(struct uui_toolbar *t);

// The parked commit's code, or -1. Taken once and cleared, so a
// redraw cannot replay a command.
int  uui_toolbar_take_code(struct uui_toolbar *t);

struct uui_widget_ops;
extern const struct uui_widget_ops uui_toolbar_ops;

#endif
