#ifndef UUI_MENUBAR_H
#define UUI_MENUBAR_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"
#include "ui/uui_widget.h"

// uui_menubar -- a menu bar with nested pull-down menus, the control
// Windows and KDE both put across the top of an application window.
//
// THE MENU IS A STATIC TREE, DECLARED, NOT BUILT
// ----------------------------------------------
// Toykit has no allocator, so there is no uui_menu_add(). A menu is
// const arrays that point at each other, which nests to any depth and
// reads like the menu looks:
//
//     static const struct uui_menu_item recent[] = {
//         UUI_MENU("notes.txt", CMD_R0, 0),
//         UUI_MENU("todo.txt",  CMD_R1, 0),
//     };
//     static const struct uui_menu_item file[] = {
//         UUI_MENU("New",       CMD_NEW,  "Ctrl-N"),
//         UUI_MENU("Open...",   CMD_OPEN, "Ctrl-O"),
//         UUI_SUBMENU_CODE("Recent", recent, CMD_RECENT),
//         UUI_MENU_SEP,
//         UUI_MENU("Exit",      CMD_EXIT, "Esc"),
//     };
//     static const struct uui_menu_item bar[] = {
//         UUI_SUBMENU("File", file),
//         UUI_SUBMENU("Edit", edit),
//     };
//     uui_menubar_init(&g_menu, bar, 2);
//
// A commit arrives as the item's `code`, from uui_menubar_release() or
// uui_menubar_key() -- the same shape uui_button_group_release() already
// has, so an app routes both through one act_on(code).
//
// STATE IS ASKED FOR, NOT STORED IN THE TREE
// ------------------------------------------
// Whether "Save" is greyed out or "Word wrap" is ticked changes as the
// document does, and the tree is const. So there is no flags field to
// keep in sync: set `item_flags` and the widget ASKS, per item, every
// time it draws or hit-tests. That is GTK's action-state model rather
// than Win32's EnableMenuItem(), and it means an app can never forget to
// update the menu -- there is nothing to update.
//
// WHERE THE POPUP GOES, AND THE ONE DIVERGENCE FROM A REAL DESKTOP
// ----------------------------------------------------------------
// On Windows a popped-up menu is a real HWND of the built-in #32768
// class, positioned in SCREEN coordinates and constrained against the
// monitor work area; on KDE it is a Qt::Popup toplevel, which under
// Wayland is literally an xdg_popup with a positioner (anchor rect,
// gravity, and flip/slide/resize constraint adjustments) that the
// compositor resolves. In both, the menu is its own surface and may
// extend far outside the window that owns it.
//
// A TWP client cannot do that: it draws into its own window buffer and
// nothing else, which is enforced (docs/gui-guidelines.md). So the
// placement here implements the same vocabulary -- flip when it will not
// fit on the preferred side, slide when it will not fit along the other
// axis, clamp as the last resort -- against a BOUNDS RECTANGLE the
// caller supplies (uui_menubar_set_bounds), which today is the window's
// own content area.
//
// That rectangle is the whole of the difference. When TWP gains a popup
// surface (docs/roadmap.md, M41), this widget is handed the screen rect
// instead and the placement maths is already the right maths -- the
// change is one rect, not a rewrite. Sizing a menu against the window is
// a real divergence in the meantime, and it is visible only on a window
// small enough that a menu would have overflowed it.

// --- an item -----------------------------------------------------------

struct uui_menu_item {
    // NULL makes this a SEPARATOR. Nothing else about the item is read.
    const char *label;

    // Right-aligned reminder text ("Ctrl-O"). Purely a label: the widget
    // does not bind it, because the app already handles those keys and a
    // second binding here would be a second source of truth.
    const char *accel;

    // Delivered to the app on commit. A submenu may carry one too, so
    // `item_flags` can grey out a whole branch (an empty Recent list) --
    // it is never committed, only queried.
    int code;

    // Non-NULL makes this a SUBMENU. `sub_count` is its length.
    const struct uui_menu_item *sub;
    int sub_count;
};

// What `item_flags` may return. Absent = an ordinary enabled item.
#define UUI_MI_CHECKED  0x01 // draws a tick in the left gutter
#define UUI_MI_DISABLED 0x02 // greyed, unhittable, skipped by the arrows

#define UUI_MENU(label_, code_, accel_) { (label_), (accel_), (code_), 0, 0 }
#define UUI_MENU_SEP                    { 0, 0, 0, 0, 0 }
#define UUI_SUBMENU(label_, arr_) \
    { (label_), 0, 0, (arr_), (int)(sizeof(arr_) / sizeof((arr_)[0])) }
#define UUI_SUBMENU_CODE(label_, arr_, code_) \
    { (label_), 0, (code_), (arr_), (int)(sizeof(arr_) / sizeof((arr_)[0])) }

// How many popups may be open at once -- the bar's own row is not one of
// them, so this is the nesting depth BELOW a title. 5 is past anything a
// sane menu does and costs one small array.
#define UUI_MENU_MAX_DEPTH 5

struct uui_menu_level {
    const struct uui_menu_item *items;
    int count;
    int x, y, w, h;
    int hot;      // highlighted row, or -1
    int parent;   // index in the level above that opened this one
};

struct uui_menubar {
    // The bar strip itself.
    int x, y, w, h;

    const struct uui_menu_item *items;
    int count;

    // -1 when closed, else the index of the open title.
    int open_root;
    int hot_root;  // hovered title, or -1

    struct uui_menu_level level[UUI_MENU_MAX_DEPTH];
    int depth;     // open popups; 0 when closed

    // Where popups are allowed to be. See the header comment -- this is
    // the line that changes when TWP grows a popup surface.
    int bx, by, bw, bh;

    // Asked per item; NULL means every item is enabled and unticked.
    unsigned (*item_flags)(int code);

    // The code a commit produced, for the ROUTED path only -- see
    // uui_menubar_ops below. -1 when there is nothing waiting.
    int committed;

    uint32_t bar_bg, fg, popup_bg, hot_bg, border, accel_fg, disabled_fg;
};

void uui_menubar_init(struct uui_menubar *m, const struct uui_menu_item *items,
                       int count);

// The strip's rect. Popup bounds default to it if never set, which is
// almost never what you want -- call set_bounds too.
void uui_menubar_set_geometry(struct uui_menubar *m, int x, int y, int w, int h);
void uui_menubar_set_bounds(struct uui_menubar *m, int x, int y, int w, int h);

// Preferred minimum for the strip: every title side by side. Width 0
// would be wrong here -- a bar narrower than its titles clips them.
void uui_menubar_natural_size(const struct uui_menubar *m, int *out_w, int *out_h);

// The strip. Draw it with the rest of the window's chrome.
void uui_menubar_draw(struct ugfx_surface *s, const struct uui_menubar *m);

// **Call this AFTER every other widget has drawn.** Drawing is
// immediate-mode, so z-order is call order -- the same rule
// uui_dropdown_draw_popup() states, and the same bug if ignored: the
// menu is painted over by whatever draws next.
void uui_menubar_draw_popup(struct ugfx_surface *s, const struct uui_menubar *m);

// --- input -------------------------------------------------------------
//
// A menu bar OPENS ON PRESS. That is the documented exception to
// docs/gui-guidelines.md's press-then-commit-on-release rule, and it is
// how Windows, KDE, macOS and GTK all behave: pressing a title shows its
// menu immediately, and you may then either release and click an item,
// or keep the button held and release over the item you want. The COMMIT
// still happens on release, which is the half of the rule that protects
// the user -- pressing "Exit" and dragging off it does nothing.

int uui_menubar_is_open(const struct uui_menubar *m);

// 1 if the point is on the bar or on any open popup. An app uses this to
// keep a click that dismisses a menu from also landing on the text
// underneath.
int uui_menubar_hit(const struct uui_menubar *m, int cx, int cy);

// Opens, switches or dismisses. Returns 1 if it consumed the press --
// including the click-outside that dismisses an open menu, which the
// menu owns rather than letting fall through (same rule as the
// dropdown's).
int uui_menubar_press(struct uui_menubar *m, int cx, int cy);

// Tracks the highlight and auto-opens submenus on hover, as every real
// menu does. Returns 1 if anything changed and the app should repaint.
// Pass it motion with or without a button held: a menu bar behaves the
// same either way once it is open, which is what makes press-drag-release
// work.
int uui_menubar_motion(struct uui_menubar *m, int cx, int cy);

// The committed item's code, or -1 for "nothing committed" -- released
// over a separator, a disabled item, a submenu parent, a title, or
// outside. Committing closes the whole chain.
int uui_menubar_release(struct uui_menubar *m, int cx, int cy);

// Keyboard navigation: F10 opens the bar, arrows walk it (Left/Right
// across titles and out of/into submenus, Up/Down over items, skipping
// separators and disabled rows), Enter or Space commits, Esc closes one
// level, and a letter jumps to the next item starting with it --
// activating immediately if it is the only match, which is what Windows
// does.
//
// Returns 1 if the key was consumed; a committed code (if any) is
// written to *out_code, which is left alone otherwise.
//
// There are deliberately NO Alt+letter mnemonics. Alt does not reach an
// app as a modifier in this OS -- it arrives terminal-style as an ESC
// prefix (api/keyboard.h), so Alt-F is ESC then 'f', which is
// indistinguishable from the Esc that has to close the menu. F10 is
// Windows' own second binding for the same job and has no such ambiguity.
int uui_menubar_key(struct uui_menubar *m, int key, int *out_code);

// Closes everything. For an app that opens a dialog and wants the menu
// out of the way.
void uui_menubar_close(struct uui_menubar *m);

// --- geometry, for tests and for an app that reports its layout --------
//
// docs/gui-guidelines.md: a GUI test asks the app where things are.
// These are what makes that possible for a control whose rectangles are
// computed at open time and depend on what is nested where.
// Each returns 1 and fills the rect, or 0 if that thing is not present.

// --- as a routed widget ------------------------------------------------
//
// Every call above is the HAND-ROUTED interface, which Notepad uses: the
// app owns its own on_press/on_motion/on_release and calls into the menu
// from them. That works for an app whose other controls are hand-drawn.
//
// **AN APP WITH ROUTED WIDGETS NEEDS THIS TABLE INSTEAD**, and not for
// tidiness: a popup drops down OVER whatever is below the bar, and the
// router runs before an app's own on_press, so a click on the File
// menu's first item would ALSO land on the widget underneath it. That is
// the problem `uui_widget_ops.overlay_active` exists to solve -- a
// widget claiming an overlay is offered every press first, with no hit
// test -- and this table declares it. The popup is drawn from
// `draw_overlay`, which uui_router_draw() runs after every widget's
// `draw`, so the z-order comes out right with nothing for the app to
// order by hand.
//
// A commit still has to reach the app. The ops `release` slot returns
// only "did anything change", so the code is parked and the app takes it
// when the router names this widget through uapp_desc.on_widget.
// TAKEN ONCE and cleared: a redraw must not replay a command.
int uui_menubar_take_code(struct uui_menubar *m);

extern const struct uui_widget_ops uui_menubar_ops;

int uui_menubar_title_rect(const struct uui_menubar *m, int index,
                            int *x, int *y, int *w, int *h);
int uui_menubar_popup_rect(const struct uui_menubar *m, int level,
                            int *x, int *y, int *w, int *h);
int uui_menubar_item_rect(const struct uui_menubar *m, int level, int index,
                           int *x, int *y, int *w, int *h);
int uui_menubar_depth(const struct uui_menubar *m);

#endif
