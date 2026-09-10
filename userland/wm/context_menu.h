#ifndef CONTEXT_MENU_H
#define CONTEXT_MENU_H

// A reusable right-click popup menu -- same peer-file pattern as
// start_menu.h/.c (factored state + drawing + hit-testing, sharing the
// window manager's state through wm_internal.h rather than being an
// independently-reasoned-about module -- see wm.c's top comment for why
// that's the right call here). Unlike start_menu.c, which always shows
// the same fixed app list at a fixed taskbar-anchored position, this is
// genuinely generic: ANY caller (desktop background, a taskbar app
// button, a window's title bar/content area, a Start menu row) can open
// one with its own short item list, anchored at the cursor position
// where the right-click landed. First real callers: wm_input.c's
// right-click dispatch (see its own comment for exactly which surfaces
// wire this in this round) -- see docs/roadmap.md for what's
// deliberately NOT wired yet (desktop icons' own per-icon menu, once
// desktop.c grows real icon identity beyond "click launches app X").
//
// Deliberately minimal, same philosophy as every widget in this
// codebase: a list of label+callback rows, click-to-select,
// click-elsewhere-to-dismiss, plus the three things a desktop menu
// could not do without (2026-09-10): one level of submenu, a
// separator, and a tick. No icons, no keyboard navigation (arrow
// keys/Enter) -- add those only once a real need shows up.

struct context_menu_item {
    const char *label;
    void (*on_select)(void *ctx); // ctx is this same item's `ctx` field below, passed back untouched
    void *ctx; // caller-defined -- e.g. a gui_app pointer (desktop quick-launch) or a window
               // index packed into the pointer (taskbar "Close window") -- NULL if unused

    // --- optional, zero for a plain row (every existing caller) -------
    //
    // ONE LEVEL OF SUBMENU: a row with `sub` opens `sub_count` rows to
    // its right on hover or click, and its own on_select is never run.
    // The desktop's "Open >" and "Icon size >" are the callers; Windows'
    // and KDE's desktop menus are shaped this way. Owned by the caller
    // like `items` itself.
    const struct context_menu_item *sub;
    int sub_count;
    // Drawn with a tick in the gutter -- the current icon size.
    int checked;
    // A separator: a rule, half a row tall, not hoverable, never selected.
    int separator;
};

// Whether a context menu is currently open -- read by wm_render.c (draw
// or not) and wm_input.c (route a left-click here first while open,
// and to know a right-click should close-then-reopen rather than
// stacking menus).
extern int context_menu_open;

// Opens a popup listing `items` (an array of `count` items, OWNED BY
// THE CALLER -- must stay valid for as long as the menu might be open,
// which in practice means callers pass a small `static const` array,
// never a stack local) anchored so its top-left is at (x, y), clamped
// so the whole menu stays on screen (a right-click near the taskbar or
// the right/bottom edge would otherwise draw partly off it).
void context_menu_open_at(int x, int y, const struct context_menu_item *items, int count);

// Closes the menu with no action -- used when a left-click lands
// outside it, or when a fresh right-click elsewhere should replace it.
void context_menu_close(void);

// Draws the popup at its open position -- a no-op if context_menu_open
// is 0 (same "caller still checks, this just draws" contract as
// start_menu_draw()).
// (mx, my) is the live cursor, for the hover highlight -- derived fresh
// every frame rather than stored, the same way start_menu_draw() takes
// it. A no-op if the menu isn't open.
void context_menu_draw(int mx, int my);

// Handles a left-click at (mx, my) while the menu is open: runs the
// clicked row's on_select() and closes, or just closes if the click
// landed outside the menu. Returns 1 if the menu was open (whether or
// not the click hit a row) so wm_input.c knows to stop right there;
// returns 0 if it wasn't open at all.
// The overlay registry's hover and damage ops -- see wm_overlay.h.
// Before these existed the hovered row was derived inside the draw and
// no move repainted it.
int context_menu_hover_at(int mx, int my);
void context_menu_damage(void);

int context_menu_handle_click(int mx, int my);

// The open menu's geometry, for the debug console (`gui ctxmenu`) and
// therefore for tests. Returns the row count, or 0 when nothing is
// open; the out-params are the menu's rect and its row height, all
// taken from the SAME statics drawing and hit-testing use, so a test
// cannot be told a different position from the one a click would land
// on. `context_menu_row_label(i)` names row i.
//
// Exists because right-click > Close had no way to be driven from a
// test, which is how it went on tearing ring-3 windows down without
// their handshake while the X button beside it did the right thing.
int context_menu_geometry(int *x, int *y, int *w, int *item_h);
const char *context_menu_row_label(int index);
// The OPEN SUBMENU's geometry and labels, same contract; 0 when none
// is open. `gui ctxmenu` reports it beside the main menu.
int context_menu_sub_geometry(int *x, int *y, int *w, int *item_h);
const char *context_menu_sub_row_label(int index);
// The y a row's TOP sits at within its menu -- separators are half a
// row, so rows are no longer at index * item_h.
int context_menu_row_top(int index);
int context_menu_sub_row_top(int index);

#endif
