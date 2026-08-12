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
#define DESKTOP_ICON_ROW_H (DESKTOP_ICON_SIZE + 28) // icon + label + gap to the next row
#define DESKTOP_COL_GAP 24 // gap between a label's right edge and the next column
#define DESKTOP_DOUBLE_CLICK_TICKS 30 // ~300ms at the PIT's 100Hz -- same order of magnitude as start_menu.c's flash
#define DESKTOP_MAX_ICONS 32 // sanity cap on gui_app_registry_count -- registry is currently 5 entries
#define DESKTOP_CONF_PATH "/etc/desktop.conf"

static int selected_index = -1; // -1 = nothing selected
static int last_click_index = -1;
static uint64_t last_click_tick = 0;

// Per-icon grid position, indexed the same as gui_app_registry[] --
// see desktop.h's top comment. Loaded from DESKTOP_CONF_PATH (or
// defaulted) on first use by desktop_load_positions().
static int icon_col[DESKTOP_MAX_ICONS];
static int icon_row[DESKTOP_MAX_ICONS];
static int positions_loaded = 0;

// The active icon drag, if any -- armed by desktop_handle_click() on
// mouse-down over an icon, driven by desktop_update_drag() each tick,
// ended on mouse-up. drag_px/drag_py are the dragged icon's live pixel
// position while drag.active (desktop_draw() reads them instead of the
// icon's cell position); see ui_icon_grid.h for the reusable geometry/
// session this is built on.
static struct icon_drag drag = { .active = 0, .index = -1 };
static int drag_px, drag_py;

// The grid geometry, recomputed live (cheap -- a handful of registry
// entries) rather than cached, same "derive fresh, don't persist a
// stale answer" idiom wm_input.c's title-bar hover uses. Column width
// is sized to the longest current label so labels never overlap their
// neighboring column, using the live font size the same way title-bar
// button sizing does (see wm_internal.h's START_LABEL comment).
static struct icon_grid current_grid(void) {
    int cell_w = DESKTOP_ICON_SIZE;
    for (int i = 0; i < gui_app_registry_count; i++) {
        int label_w = (int)k_strlen(gui_app_registry[i].name) * gfx_char_w();
        if (label_w > cell_w) cell_w = label_w;
    }
    cell_w += DESKTOP_COL_GAP;

    struct icon_grid g;
    g.origin_x = DESKTOP_ICON_X;
    g.origin_y = DESKTOP_ICON_START_Y;
    g.cell_w = cell_w;
    g.cell_h = DESKTOP_ICON_ROW_H;
    g.cols = screen_w / cell_w;
    if (g.cols < 1) g.cols = 1;
    return g;
}

// Formats "col,row" into `out` (at least 12 bytes) -- no snprintf in
// this freestanding build, so a small local decimal formatter, same
// "stays local, not shared kernel-wide surface for one caller"
// reasoning as tz.c's tz_format_int(). col/row are always >= 0 here
// (icon_grid_nearest_cell() clamps), so no sign handling needed.
static void format_pos(char *out, int col, int row) {
    int n = 0;
    int vals[2] = { col, row };
    for (int v_i = 0; v_i < 2; v_i++) {
        int v = vals[v_i];
        if (v == 0) {
            out[n++] = '0';
        } else {
            char digits[12];
            int dn = 0;
            while (v > 0) { digits[dn++] = (char)('0' + v % 10); v /= 10; }
            while (dn > 0) out[n++] = digits[--dn];
        }
        if (v_i == 0) out[n++] = ',';
    }
    out[n] = '\0';
}

static void save_position(int i) {
    char value[16];
    format_pos(value, icon_col[i], icon_row[i]);
    etc_config_set(DESKTOP_CONF_PATH, gui_app_registry[i].name, value);
}

// Loads every icon's position from DESKTOP_CONF_PATH, defaulting to
// the pre-dragging layout (a single left-edge column, row = registry
// index) for any app with no saved entry yet -- keyed by app name
// (not registry index) so a registry reorder doesn't scramble saved
// positions. Runs once per boot; positions don't change except via a
// drag, which updates icon_col/icon_row directly, so there's nothing
// to invalidate this cache.
static void desktop_load_positions(void) {
    if (positions_loaded) return;
    positions_loaded = 1;

    int n = gui_app_registry_count;
    if (n > DESKTOP_MAX_ICONS) n = DESKTOP_MAX_ICONS;
    for (int i = 0; i < n; i++) {
        icon_col[i] = 0;
        icon_row[i] = i;

        char value[16];
        if (!etc_config_get(DESKTOP_CONF_PATH, gui_app_registry[i].name, value, sizeof(value))) continue;

        const char *p = value;
        int col = 0;
        while (*p >= '0' && *p <= '9') { col = col * 10 + (*p - '0'); p++; }
        if (*p != ',') continue; // malformed -- keep the default set above
        p++;
        int row = 0;
        while (*p >= '0' && *p <= '9') { row = row * 10 + (*p - '0'); p++; }

        icon_col[i] = col;
        icon_row[i] = row;
    }
}

void desktop_draw(void) {
    desktop_load_positions();
    struct icon_grid g = current_grid();

    gfx_clear(gfx_rgb(24, 60, 90)); // the plain background color every prior build used --
                                     // real wallpaper images are blocked on the not-yet-built
                                     // image decoder, see docs/roadmap.md.

    uint32_t label_fg = THEME_WHITE;
    uint32_t icon_fg = gfx_rgb(230, 230, 235);
    uint32_t icon_selected_bg = gfx_rgb(70, 110, 160);

    for (int i = 0; i < gui_app_registry_count; i++) {
        int x, y;
        if (drag.active && drag.index == i) {
            x = drag_px;
            y = drag_py;
        } else {
            icon_grid_cell_rect(&g, icon_col[i], icon_row[i], &x, &y);
        }

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
    desktop_load_positions();
    struct icon_grid g = current_grid();
    for (int i = 0; i < gui_app_registry_count; i++) {
        int x, y;
        icon_grid_cell_rect(&g, icon_col[i], icon_row[i], &x, &y);
        if (widget_hit(x, y, DESKTOP_ICON_SIZE, DESKTOP_ICON_SIZE + 18, mx, my)) return i;
    }
    return -1;
}

void desktop_handle_click(int mx, int my) {
    desktop_load_positions();
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

    // Arm a potential drag too, regardless of which branch above ran --
    // a plain click (no movement before release) just re-commits the
    // icon to the cell it's already in via desktop_update_drag(), so
    // this doesn't change the select/double-click behavior above.
    struct icon_grid g = current_grid();
    int ix, iy;
    icon_grid_cell_rect(&g, icon_col[idx], icon_row[idx], &ix, &iy);
    icon_drag_start(&drag, idx, mx, my, ix, iy);
    drag_px = ix;
    drag_py = iy;

    redraw_pending = 1;
}

void desktop_update_drag(int mx, int my, uint8_t buttons) {
    if (!drag.active) return;

    if (buttons & 0x1) {
        drag_px = mx - drag.grab_off_x;
        drag_py = my - drag.grab_off_y;
        redraw_pending = 1;
        return;
    }

    struct icon_grid g = current_grid();
    int col, row;
    icon_drag_update(&drag, &g, mx, my, &col, &row);
    if (col != icon_col[drag.index] || row != icon_row[drag.index]) {
        icon_col[drag.index] = col;
        icon_row[drag.index] = row;
        save_position(drag.index);
    }
    icon_drag_end(&drag);
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
