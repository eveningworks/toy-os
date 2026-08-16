// See start_menu.h.
#include "start_menu.h"
#include "wm_internal.h"
#include "confirm_dialog.h"
#include "ui/ui.h"
#include "theme.h"
#include "kapi.h"

int start_menu_open = 0;

// System actions shown at the bottom of the Start menu, below the app
// list -- these don't open a window like a real gui_app_registry entry
// does, they trigger a WM-level action directly. "Exit to shell"
// replaces the old hardcoded "Esc always exits the window manager"
// shortcut: it's discoverable now instead of a hidden key, and frees
// Esc up for a future modal-cancel use (a confirm dialog, say) instead
// of double-booking it as "exit everything, no matter what's open or
// focused".
//
// Now goes through confirm_dialog.h's reusable Yes/No popup instead of
// setting wm_exit_requested directly -- exiting drops every open
// window's unsaved state (no session restore exists), so a stray
// Start-menu click landing on this row shouldn't be irreversible. This
// was the dialog's first real caller; Shutdown (below) is the second,
// per docs/roadmap.md's Shutdown item and confirm_dialog.h's own top
// comment.
static void do_exit_to_shell(void) { wm_exit_requested = 1; }
static void action_exit_to_shell(void) {
    confirm_dialog_open_with("Exit to shell? Unsaved changes will be lost.", do_exit_to_shell, 0);
}

// Shutdown -- docs/roadmap.md's own design notes settled the shape
// ahead of time: a Yes/No confirm first (confirm_dialog.h, same as
// Exit to shell above), then a real poweroff. system_poweroff()
// (kernel/core/power.c) is the QEMU/Bochs ACPI I/O-port trick, not a
// real parsed-ACPI shutdown (see power.h) -- that's still
// docs/roadmap.md's ACPI table parsing item, not done. Unlike
// do_exit_to_shell(), system_poweroff() never returns (it halts either
// way, see its own comment), so there's no wm_exit_requested-style flag
// to set here -- the callback just calls it directly.
static void do_shutdown(void) { system_poweroff(); }
static void action_shutdown(void) {
    confirm_dialog_open_with("Shut down? Unsaved changes will be lost.", do_shutdown, 0);
}

const struct start_action wm_system_actions[] = {
    { "Exit to shell", action_exit_to_shell },
    { "Shutdown", action_shutdown },
};
const int wm_system_action_count = sizeof(wm_system_actions) / sizeof(wm_system_actions[0]);

// A click on a Start menu row used to close the menu in the same frame
// it ran the row's action -- no visible confirmation the click actually
// landed. `flash_index` (-1 when idle) is which row is showing a brief
// "you clicked this" flash before the menu actually closes;
// `flash_until` is the pit_ticks() deadline for that (see
// START_MENU_FLASH_TICKS and start_menu_update() below). The row's
// action still runs immediately on click -- only closing the menu is
// deferred. See docs/decisions.md for the full "why a deadline, not a
// blocking sleep" writeup.
static int flash_index = -1;
static uint64_t flash_until = 0;
#define START_MENU_FLASH_TICKS 10 // ~100ms at the PIT's 100Hz -- long enough to register as a deliberate flash

// Wide enough for the longest label actually in the menu -- app names
// AND wm_system_actions (e.g. "Exit to shell", 13 chars, longer than
// any app name today) both need to fit, so this scans both instead of
// assuming a fixed char count.
int start_menu_w(void) {
    int max_chars = 0;
    int app_rows = gui_app_visible_count(GUI_SHOW_STARTMENU);
    for (int i = 0; i < app_rows; i++) {
        int n = (int)k_strlen(gui_app_visible_at(GUI_SHOW_STARTMENU, i)->name);
        if (n > max_chars) max_chars = n;
    }
    for (int i = 0; i < wm_system_action_count; i++) {
        int n = (int)k_strlen(wm_system_actions[i].label);
        if (n > max_chars) max_chars = n;
    }
    return max_chars * gfx_char_w() + 20;
}

// Shared by start_menu_draw() and start_menu_handle_click() so the two
// can never drift out of agreement on where each row actually is --
// previously this formula was duplicated by hand between wm_render.c
// and wm_input.c; now that both live in the same file, there's no
// reason not to share it outright.
void start_menu_geometry(int *out_menu_x, int *out_menu_y, int *out_menu_w,
                      int *out_item_h, int *out_total_items) {
    *out_item_h = gfx_char_h() + 6;
    *out_menu_w = start_menu_w();
    *out_menu_x = 4;
    *out_total_items = gui_app_visible_count(GUI_SHOW_STARTMENU) + wm_system_action_count;
    *out_menu_y = (screen_h - taskbar_h) - (*out_item_h) * (*out_total_items);
}

void start_menu_open_now(void) {
    start_menu_open = 1;
    flash_index = -1;
}

// App items (gui_app_registry) first, then wm_system_actions ("Exit to
// shell") below them -- same item_h, same click math as
// start_menu_handle_click() (geometry() above). The only structural
// visual difference is a 1px divider rule drawn at the boundary between
// the two groups; it doesn't consume a row of its own.
//
// Two distinct row highlights, drawn as a filled background band behind
// the label:
//   - hover: whichever row (mx, my) is currently over, recomputed fresh
//     every call -- not persisted anywhere, purely a function of the
//     current mouse position (wm.c's main loop forces a full redraw on
//     every mouse move while the menu's open specifically so this stays
//     live).
//   - flash: the row that was just clicked, shown in a distinct color
//     for a few ticks before the menu closes (flash_index). Takes
//     priority over hover -- once a row's been clicked, the flash is
//     what's showing regardless of where the mouse drifts to next.
void start_menu_draw(int mx, int my) {
    int menu_x, menu_y, menu_w, item_h, total_items;
    start_menu_geometry(&menu_x, &menu_y, &menu_w, &item_h, &total_items);
    int menu_h = item_h * total_items;

    uint32_t bg = THEME_PANEL_BG, border = THEME_BORDER, fg = THEME_TEXT;
    // Hover comes from ui_state_bg() now, derived from the row's OWN
    // colour, rather than the hand-picked gfx_rgb(90, 110, 150) this used
    // to carry. Two reasons: docs/gui-guidelines.md says not to hand-pick
    // tints (a fixed "lighter" wash is what made hover invisible on this
    // near-white theme once), and a fixed dark blue also forced the label
    // to white, so the menu had a second text colour nothing else used.
    // Derived, it darkens a light row and the ordinary text colour still
    // reads against it.
    uint32_t hover_bg = ui_state_bg(bg, UI_STATE_HOVER);
    // The click flash stays a deliberately DISTINCT warm colour, not a
    // ui_state derivation: it is not an interaction state, it is a
    // momentary confirmation that a row was chosen (see the flash_index
    // comment above), and it has to be obviously different from hover
    // rather than one step further along the same axis.
    uint32_t flash_bg = gfx_rgb(230, 190, 90);
    uint32_t flash_fg = THEME_WHITE;
    gfx_fill_rect(menu_x, menu_y, menu_w, menu_h, bg);

    int hot = -1;
    if (flash_index >= 0) {
        hot = flash_index;
    } else if (widget_hit(menu_x, menu_y, menu_w, menu_h, mx, my)) {
        hot = (my - menu_y) / item_h;
    }

    int app_rows = gui_app_visible_count(GUI_SHOW_STARTMENU);
    for (int i = 0; i < app_rows; i++) {
        int y = menu_y + i * item_h;
        uint32_t row_bg = bg, row_fg = fg;
        if (i == hot) {
            row_bg = (i == flash_index) ? flash_bg : hover_bg;
            row_fg = (i == flash_index) ? flash_fg : fg;
            gfx_fill_rect(menu_x, y, menu_w, item_h, row_bg);
        }
        // Clipped: an app name longer than the menu is wide would
        // otherwise be drawn through the border (docs/gui-guidelines.md).
        gfx_draw_string_clipped(menu_x + 8, y + 3, menu_w - 16,
                                 gui_app_visible_at(GUI_SHOW_STARTMENU, i)->name,
                                 row_fg, row_bg);
    }
    if (wm_system_action_count > 0) {
        int divider_y = menu_y + app_rows * item_h;
        gfx_fill_rect(menu_x, divider_y, menu_w, 1, border);
        for (int i = 0; i < wm_system_action_count; i++) {
            int idx = app_rows + i;
            int y = menu_y + idx * item_h;
            uint32_t row_bg = bg, row_fg = fg;
            if (idx == hot) {
                row_bg = (idx == flash_index) ? flash_bg : hover_bg;
                row_fg = (idx == flash_index) ? flash_fg : fg;
                gfx_fill_rect(menu_x, y, menu_w, item_h, row_bg);
            }
            gfx_draw_string_clipped(menu_x + 8, y + 3, menu_w - 16,
                                     wm_system_actions[i].label, row_fg, row_bg);
        }
    }
    // Border last, after every row fill -- a hover/flash band spans the
    // full menu_w, the same columns the border's left/right edges sit
    // on, so drawing the border first would get overpainted wherever a
    // highlighted row touches it (same lesson as Notepad's field border
    // fix, see docs/decisions.md).
    gfx_draw_rect(menu_x, menu_y, menu_w, menu_h, border);
}

int start_menu_handle_click(int mx, int my) {
    if (!start_menu_open) return 0;

    int menu_x, menu_y, menu_w, item_h, total_items;
    start_menu_geometry(&menu_x, &menu_y, &menu_w, &item_h, &total_items);

    if (widget_hit(menu_x, menu_y, menu_w, item_h * total_items, mx, my)) {
        int idx = (my - menu_y) / item_h;
        int app_rows = gui_app_visible_count(GUI_SHOW_STARTMENU);
        struct gui_app *app = gui_app_visible_at(GUI_SHOW_STARTMENU, idx);
        if (app) {
            open_app(app);
        } else if (idx >= app_rows && idx < total_items) {
            wm_system_actions[idx - app_rows].on_select();
        }
        // The row's action already ran above -- only closing the menu
        // is deferred, so the click gets a brief visible flash instead
        // of vanishing in the same frame it landed. start_menu_update()
        // closes the menu once the deadline passes.
        flash_index = idx;
        flash_until = pit_ticks() + START_MENU_FLASH_TICKS;
    } else {
        // Clicked elsewhere while the menu was open (the desktop, a
        // window) -- no row was selected, so there's nothing to flash;
        // close immediately.
        start_menu_open = 0;
    }
    redraw_pending = 1;
    return 1;
}

void start_menu_update(void) {
    if (flash_index < 0) return;
    if (pit_ticks() >= flash_until) {
        flash_index = -1;
        start_menu_open = 0;
        redraw_pending = 1;
    }
}
