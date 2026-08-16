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
// Column pitch, in CHARACTERS of the active font -- see icon_col_w().
// 13 is "Control Panel"/"Task Manager", the longest labels that should
// never be truncated; anything longer (the "(ring 3)" launchers) is cut
// with a ".." marker rather than widening every column to fit it.
#define DESKTOP_ICON_LABEL_CHARS 13
#define DESKTOP_DOUBLE_CLICK_TICKS 30 // ~300ms at the PIT's 100Hz -- same order of magnitude as start_menu.c's flash
#define DESKTOP_MAX_ICONS 32 // sanity cap on gui_app_registry_count -- registry currently holds 11 entries
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
// stale answer" idiom wm_input.c's title-bar hover uses.
//
// Column width: wide enough for a DESKTOP_ICON_LABEL_CHARS label at the
// active font, never narrower than the icon box itself.
//
// It is a FIXED pitch, NOT sized to the longest label actually present.
// An earlier version did that (label_w-driven cell_w) and it meant a
// single long name anywhere in the registry widened EVERY column, even
// ones nowhere near it -- measured against a real 2-column layout, a
// ~144px pitch driven solely by "Task Manager" in column 0 row 2, while
// the row-0 icons that were actually adjacent needed ~94px. A fixed
// pitch is the standard real-desktop tradeoff (Windows/GNOME both do
// it), and it is what desktop_draw() clips labels against.
//
// The value used to be DESKTOP_ICON_ROW_H -- square 76px cells. That
// was survivable only while the desktop was one column deep: a long
// label ran off to the right over empty background and stayed readable,
// which desktop_draw()'s comment recorded as an accepted quirk. Adding
// a second column ended that -- the overflow landed on the neighbouring
// column's labels and both became unreadable. Clipping alone was not
// the fix either: at 76px only ~7 characters fit, turning "Control
// Panel" and "Calculator" into "Contr.." and "Calcu..". So the pitch
// widened to fit a real label, and clipping handles what still doesn't.
static int icon_col_w(void) {
    int w = DESKTOP_ICON_LABEL_CHARS * gfx_char_w() + 8;
    return w < DESKTOP_ICON_SIZE + 8 ? DESKTOP_ICON_SIZE + 8 : w;
}

static struct icon_grid current_grid(void) {
    struct icon_grid g;
    g.origin_x = DESKTOP_ICON_X;
    g.origin_y = DESKTOP_ICON_START_Y;
    g.cell_w = icon_col_w();
    g.cell_h = DESKTOP_ICON_ROW_H;
    g.cols = screen_w / icon_col_w();
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
// the default layout (top-to-bottom, wrapping into a new column at the
// bottom edge -- see below) for any app with no saved entry yet --
// keyed by app name (not registry index), so a registry reorder
// doesn't scramble saved positions. Runs once per boot; positions don't change except via a
// drag, which updates icon_col/icon_row directly, so there's nothing
// to invalidate this cache.
static void desktop_load_positions(void) {
    if (positions_loaded) return;
    positions_loaded = 1;

    int n = gui_app_registry_count;
    if (n > DESKTOP_MAX_ICONS) n = DESKTOP_MAX_ICONS;

    // How many icons fit in one column before running off the bottom.
    // The default layout WRAPS into a second column rather than being a
    // single unbounded one: that used to be `icon_row[i] = i`, which was
    // fine while the registry held seven apps and silently walked icons
    // off the screen the moment it held eleven (the four ring-3
    // launchers). Icons past the edge are not just invisible -- they are
    // unclickable, so an app can be in the registry and unreachable from
    // the desktop with nothing to indicate why.
    int usable_h = (screen_h - taskbar_h) - DESKTOP_ICON_START_Y;
    int per_col = usable_h / DESKTOP_ICON_ROW_H;
    if (per_col < 1) per_col = 1;   // a tiny screen still gets one per column

    // `slot` counts icons actually PLACED, not registry entries, so an
    // entry hidden by ShowIn= leaves no gap in the default grid.
    int slot = 0;
    for (int i = 0; i < n; i++) {
        if (!gui_app_shows_in(&gui_app_registry[i], GUI_SHOW_DESKTOP)) {
            icon_col[i] = icon_row[i] = -1; // never drawn, never hit-tested
            continue;
        }
        icon_col[i] = slot / per_col;
        icon_row[i] = slot % per_col;
        slot++;

        char value[16];
        if (!etc_config_get(DESKTOP_CONF_PATH, gui_app_registry[i].name, value, sizeof(value))) continue;

        // "<col>,<row>" -- split on the comma, then let knum.h's bounded
        // parser handle each half. Stricter than the digit loops this
        // replaced: those accepted trailing junk ("3,4x" parsed as 3,4),
        // where this treats the whole field as malformed and keeps the
        // default, matching how every other config value here behaves.
        const char *comma = k_strchr(value, ',');
        if (!comma) continue; // malformed -- keep the default set above

        uint64_t col = 0, row = 0;
        if (!k_parse_u64_n(value, (size_t)(comma - value), &col)) continue;
        if (!k_parse_u64(comma + 1, &row)) continue;

        icon_col[i] = (int)col;
        icon_row[i] = (int)row;
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
        if (!gui_app_shows_in(&gui_app_registry[i], GUI_SHOW_DESKTOP)) continue;
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
        // The entry's Icon= character, falling back to the name's first
        // letter -- which is what this drew before desktop entries
        // existed, so an entry with no Icon= looks exactly as it did.
        char ic = gui_app_registry[i].icon;
        char initial[2] = { ic ? ic : gui_app_registry[i].name[0], '\0' };
        int gx = x + (DESKTOP_ICON_SIZE - gfx_char_w()) / 2;
        int gy = y + (DESKTOP_ICON_SIZE - gfx_char_h()) / 2;
        gfx_draw_string(gx, gy, initial, icon_fg, gfx_rgb(60, 90, 130));

        // CLIPPED to the cell pitch, not drawn free-hand. current_grid()
        // deliberately uses a fixed column width rather than sizing to
        // the longest label (see its comment), and the consequence used
        // to be accepted as a cosmetic quirk because the desktop was one
        // column deep -- there was nothing to the right to run into.
        // With a second column it stops being cosmetic: "Calculator
        // (ring 3)" printed straight over its neighbour's label and both
        // became unreadable. This is the bug class
        // docs/gui-guidelines.md names first -- gfx_draw_string() does
        // not clip, so anything in a fixed box needs the _clipped()
        // form.
        int label_y = y + DESKTOP_ICON_SIZE + 4;
        int label_max_w = icon_col_w() - 4; // -4: a gap, so adjacent labels never touch
        uint32_t label_bg = gfx_rgb(24, 60, 90);
        const char *label = gui_app_registry[i].name;

        if (gfx_text_width(label) <= label_max_w) {
            gfx_draw_string_clipped(x, label_y, label_max_w, label, label_fg, label_bg);
        } else {
            // Too long: cut it two characters short and mark the cut, so
            // a truncated label reads AS truncated rather than as a
            // differently-named app -- "Calculator" and "Calculator
            // (ring 3)" both cut to "Calculat" otherwise, which is worse
            // than useless on a desktop that now shows both.
            //
            // ".." rather than a single ellipsis character: the font is
            // indexed from ASCII 32 (kernel/drivers/font_ttf.c), so
            // U+2026 -- and Latin-1 0x85 -- have no glyph and would draw
            // as nothing at all.
            int cut_w = label_max_w - 2 * gfx_char_w();
            if (cut_w < gfx_char_w()) cut_w = gfx_char_w(); // always show at least one char
            gfx_draw_string_clipped(x, label_y, cut_w, label, label_fg, label_bg);
            gfx_draw_string_clipped(x + cut_w, label_y, 2 * gfx_char_w(), "..",
                                     label_fg, label_bg);
        }
    }
}

int desktop_drag_active(void) { return drag.active; }

void desktop_entries_changed(void) {
    positions_loaded = 0;   // re-read from DESKTOP_CONF_PATH, keyed by name
    selected_index = -1;
    last_click_index = -1;
    last_click_tick = 0;
}

// Which icon (if any) is under (mx, my) -- shared by click and
// right-click handling so they can't disagree about hitboxes.
static int icon_hit_test(int mx, int my) {
    desktop_load_positions();
    struct icon_grid g = current_grid();
    for (int i = 0; i < gui_app_registry_count; i++) {
        if (!gui_app_shows_in(&gui_app_registry[i], GUI_SHOW_DESKTOP)) continue;
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

// Whether (col, row) already belongs to some OTHER icon (not `exclude`,
// the one currently being dropped) -- O(gui_app_registry_count), fine
// at this scale (a handful of icons).
static int cell_taken(int col, int row, int exclude) {
    for (int i = 0; i < gui_app_registry_count; i++) {
        if (i == exclude) continue;
        if (icon_col[i] == col && icon_row[i] == row) return 1;
    }
    return 0;
}

// Nearest cell to (col, row) not already occupied by another icon --
// searches outward ring by ring (Chebyshev distance) so dropping an
// icon onto an already-occupied cell settles it into an adjacent free
// one instead of the two silently overlapping. Ties within a ring
// resolve to whichever cell the scan order (top row of the ring,
// left to right) hits first -- not a strict Euclidean-nearest
// tiebreak, but close enough at icon-grid scale. Always terminates:
// DESKTOP_MAX_ICONS icons can occupy at most DESKTOP_MAX_ICONS cells,
// so a search out to that many rings is guaranteed to find a free one.
static void nearest_free_cell(const struct icon_grid *g, int col, int row,
                               int exclude, int *out_col, int *out_row) {
    if (!cell_taken(col, row, exclude)) { *out_col = col; *out_row = row; return; }

    for (int radius = 1; radius <= DESKTOP_MAX_ICONS; radius++) {
        for (int dr = -radius; dr <= radius; dr++) {
            for (int dc = -radius; dc <= radius; dc++) {
                // Skip the interior -- already checked at a smaller radius.
                if (dc > -radius && dc < radius && dr > -radius && dr < radius) continue;
                int c = col + dc, r = row + dr;
                if (c < 0 || c >= g->cols || r < 0) continue;
                if (!cell_taken(c, r, exclude)) { *out_col = c; *out_row = r; return; }
            }
        }
    }
    *out_col = col; *out_row = row; // unreachable in practice -- see comment above
}

// Extra top margin damage_icon_row()/desktop_update_drag() pad their
// strip by -- desktop_draw()'s selection-highlight rect
// (icon_selected_bg) draws 4px ABOVE an icon's own y (`y - 4`, see
// desktop_draw()), which a strip anchored exactly at y wouldn't
// otherwise cover, leaving a thin stale sliver of highlight behind
// mid-drag (found by actually dragging an icon and looking, not by
// inspection -- see docs/decisions.md).
#define DESKTOP_DRAG_DAMAGE_MARGIN 4

// Reports a dragged icon's footprint as scene damage -- a full-width
// horizontal strip covering [top, top+cell_h) rather than a tight box
// around the icon square itself, deliberately: a label can visually
// run past its own cell into a neighboring column (desktop.h's own
// top comment covers why that's an accepted quirk, not a bug), and a
// tight box would leave stale label pixels behind as it drags through
// that overflow. Cheap enough at this scale (a handful of icon rows,
// not the whole screen) not to matter.
static void damage_icon_row(const struct icon_grid *g, int y) {
    wm_damage_rect(0, y - DESKTOP_DRAG_DAMAGE_MARGIN, screen_w,
                   g->cell_h + DESKTOP_DRAG_DAMAGE_MARGIN);
}

void desktop_update_drag(int mx, int my, uint8_t buttons) {
    if (!drag.active) return;

    if (buttons & 0x1) {
        int old_py = drag_py;
        drag_px = mx - drag.grab_off_x;
        drag_py = my - drag.grab_off_y;
        struct icon_grid g = current_grid();
        int top = (old_py < drag_py ? old_py : drag_py) - DESKTOP_DRAG_DAMAGE_MARGIN;
        int bottom = (old_py > drag_py ? old_py : drag_py) + g.cell_h;
        wm_damage_rect(0, top, screen_w, bottom - top);
        redraw_pending = 1;
        return;
    }

    struct icon_grid g = current_grid();
    int col, row;
    icon_drag_update(&drag, &g, mx, my, &col, &row);
    nearest_free_cell(&g, col, row, drag.index, &col, &row);
    damage_icon_row(&g, drag_py); // wherever it was last dragged to...
    if (col != icon_col[drag.index] || row != icon_row[drag.index]) {
        icon_col[drag.index] = col;
        icon_row[drag.index] = row;
        save_position(drag.index);
    }
    int fx, fy;
    icon_grid_cell_rect(&g, icon_col[drag.index], icon_row[drag.index], &fx, &fy);
    damage_icon_row(&g, fy); // ...and wherever it actually settled (the snap itself)
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
    // The desktop's own menu, so it shows what the desktop shows -- an
    // entry hidden from this surface by ShowIn= must not be launchable
    // from here either, or "hidden" would mean "hidden unless you
    // right-click".
    int n = gui_app_visible_count(GUI_SHOW_DESKTOP);
    if (n > 16) n = 16; // sanity cap; the registry holds ~8 entries today
    for (int i = 0; i < n; i++) {
        struct gui_app *app = gui_app_visible_at(GUI_SHOW_DESKTOP, i);
        items[i].label = app->name;
        items[i].on_select = launch_from_menu;
        items[i].ctx = (void *)app;
    }
    context_menu_open_at(mx, my, items, n);
}
