// See desktop.h for the design writeup.
#include "desktop.h"
#include "wm_internal.h"
#include "context_menu.h"
#include "ui/ui.h"
#include "theme.h"
#include "kapi.h"

#define DESKTOP_ICON_SIZE 48
#define DESKTOP_ICON_X 16
#define DESKTOP_ICON_START_Y 16
#define DESKTOP_ICON_ROW_H (DESKTOP_ICON_SIZE + 28) // icon + label + gap to the next icon
#define DESKTOP_DOUBLE_CLICK_TICKS 30 // ~300ms at the PIT's 100Hz -- same order of magnitude as start_menu.c's flash

static int selected_index = -1; // -1 = nothing selected
static int last_click_index = -1;
static uint64_t last_click_tick = 0;

static void icon_rect(int i, int *x, int *y) {
    *x = DESKTOP_ICON_X;
    *y = DESKTOP_ICON_START_Y + i * DESKTOP_ICON_ROW_H;
}

void desktop_draw(void) {
    gfx_clear(gfx_rgb(24, 60, 90)); // the plain background color every prior build used --
                                     // real wallpaper images are blocked on the not-yet-built
                                     // image decoder, see docs/roadmap.md.

    uint32_t label_fg = THEME_WHITE;
    uint32_t icon_fg = gfx_rgb(230, 230, 235);
    uint32_t icon_selected_bg = gfx_rgb(70, 110, 160);

    for (int i = 0; i < gui_app_registry_count; i++) {
        int x, y;
        icon_rect(i, &x, &y);

        if (i == selected_index) {
            gfx_fill_rect(x - 4, y - 4, DESKTOP_ICON_SIZE + 8,
                           DESKTOP_ICON_SIZE + 8 + 18, icon_selected_bg);
        }

        gfx_fill_rect(x, y, DESKTOP_ICON_SIZE, DESKTOP_ICON_SIZE, gfx_rgb(60, 90, 130));
        gfx_draw_rect(x, y, DESKTOP_ICON_SIZE, DESKTOP_ICON_SIZE, icon_fg);

        // Stand-in glyph: the app name's first letter, centered in the
        // square -- see this file's top comment on why (no image
        // decoder yet).
        char initial[2] = { gui_app_registry[i].name[0], '\0' };
        int gx = x + (DESKTOP_ICON_SIZE - gfx_char_w()) / 2;
        int gy = y + (DESKTOP_ICON_SIZE - gfx_char_h()) / 2;
        gfx_draw_string(gx, gy, initial, icon_fg, gfx_rgb(60, 90, 130));

        gfx_draw_string(x, y + DESKTOP_ICON_SIZE + 4, gui_app_registry[i].name, label_fg, gfx_rgb(24, 60, 90));
    }
}

// Which icon (if any) is under (mx, my) -- shared by click and
// right-click handling so they can't disagree about hitboxes.
static int icon_hit_test(int mx, int my) {
    for (int i = 0; i < gui_app_registry_count; i++) {
        int x, y;
        icon_rect(i, &x, &y);
        if (widget_hit(x, y, DESKTOP_ICON_SIZE, DESKTOP_ICON_SIZE + 18, mx, my)) return i;
    }
    return -1;
}

void desktop_handle_click(int mx, int my) {
    int idx = icon_hit_test(mx, my);
    uint64_t now = pit_ticks();

    if (idx < 0) {
        selected_index = -1;
        last_click_index = -1;
        redraw_pending = 1;
        return;
    }

    if (idx == last_click_index && now - last_click_tick <= DESKTOP_DOUBLE_CLICK_TICKS) {
        open_app(&gui_app_registry[idx]);
        last_click_index = -1; // avoid a third click within the window re-triggering as a double
    } else {
        selected_index = idx;
        last_click_index = idx;
        last_click_tick = now;
    }
    redraw_pending = 1;
}

// Trampoline for context_menu_item's on_select(ctx) -- ctx is the
// gui_app this row launches, cast back from the void* it was stored as.
static void launch_from_menu(void *ctx) {
    open_app((const struct gui_app *)ctx);
}

void desktop_handle_right_click(int mx, int my) {
    (void)icon_hit_test; // per-icon menu is a future refinement, see desktop.h's top comment

    // A static array sized for the current registry, filled fresh each
    // open -- context_menu_open_at() only borrows the pointer for as
    // long as the menu stays open, and nothing here runs again before
    // the menu closes (single-threaded event loop), so a static scratch
    // array is safe the same way g_walk_scratch-style buffers are
    // elsewhere in this codebase.
    static struct context_menu_item items[16];
    int n = gui_app_registry_count;
    if (n > 16) n = 16; // sanity cap -- registry is currently 5 entries
    for (int i = 0; i < n; i++) {
        items[i].label = gui_app_registry[i].name;
        items[i].on_select = launch_from_menu;
        items[i].ctx = (void *)&gui_app_registry[i];
    }
    context_menu_open_at(mx, my, items, n);
}
