// A Control Panel with pluggable applets, Windows-style: a grid of
// applet icons, and clicking one drills into that applet's page with a
// Back button to return.
//
// **The applet registry mirrors gui_apps.h's app registry deliberately**
// -- a static table of `struct applet`, each with a name and a few
// function pointers, and the same "add a row, nothing else changes"
// property. That's the shape this codebase already uses for the thing
// this is (a small fixed set of plug-ins known at build time), so a
// second mechanism with different conventions would be a new thing to
// learn for no gain. An applet is NOT a gui_app: it has no window of
// its own, draws into a caller-provided rectangle, and can't be opened
// from the Start menu independently.
//
// The chooser reuses apps/ui/ui_icon_grid.c, whose header was written
// anticipating exactly this -- "built as a standalone widget so a
// future file manager's icon view can reuse the same cell math". This
// is the second caller it was waiting for, minus the drag session
// (applet icons don't move, so only the geometry half is used).
#include "control_panel.h"
#include "ui/ui.h"
#include "theme.h"
#include "kapi.h"

#define CP_MARGIN      12
#define CP_ICON_SIZE   40
#define CP_CELL_H      (CP_ICON_SIZE + 30)
#define CP_GRID_COLS   3
#define CP_HEADER_GAP  10

// Cell width is derived from the font, not a constant. The first
// version hardcoded 104px, which was 6px narrower than "Date & Time"
// renders at the default font -- and since gfx_draw_string() does no
// clipping at all (see docs/decisions.md), the label simply drew
// straight over its neighbour: "Date & TSystem Info". Sizing the cell
// to the longest label, and clipping anything longer than that, is the
// fix that entry says every fixed-box text caller has to make for
// itself.
#define CP_LABEL_MAX_CHARS 12
#define CP_CELL_W (CP_LABEL_MAX_CHARS * gfx_char_w() + 8)

// ---------------------------------------------------------------------
// The applet interface
// ---------------------------------------------------------------------

struct applet {
    const char *name;

    // Draws the applet's page into the given content rectangle. The
    // Back button and the title above it are the Control Panel's own
    // chrome, already drawn -- an applet only owns the area below.
    void (*draw)(int x, int y, int w, int h);

    // A click inside that same rectangle, in absolute screen
    // coordinates (matching what the WM hands on_click). Returns 1 if
    // the applet changed something and the window needs repainting.
    int (*click)(int x, int y, int w, int h, int px, int py);
};

// ---------------------------------------------------------------------
// Applet: Date & Time
// ---------------------------------------------------------------------
//
// A timezone picker over whatever /etc/timezones holds (tz.c), which is
// a database file rather than a hardcoded list -- so this applet shows
// however many cities that file has, and needs no change when it grows.
// See docs/decisions.md on why the city list and the *selection* are
// deliberately two different files.
//
// The selected index isn't cached here: it's read from
// tz_current_index() at draw time and written with tz_set_index(),
// which persists to /etc/toyos.conf itself. A local copy would be a
// second source of truth to keep in sync, and the `timezone` shell
// command can change the same setting behind this window's back.

#define TZ_MAX_SHOWN 12 // a screenful; the file has 7 today

static const char *g_tz_names[TZ_MAX_SHOWN];
static struct ui_radio_list g_tz_list;

static void tz_applet_build_list(void) {
    int n = tz_city_count();
    if (n > TZ_MAX_SHOWN) n = TZ_MAX_SHOWN;
    for (int i = 0; i < n; i++) g_tz_names[i] = tz_city_name(i);

    g_tz_list.options = g_tz_names;
    g_tz_list.count = n;
    g_tz_list.cols = 2;
    g_tz_list.row_h = gfx_char_h() + 10;
    g_tz_list.col_w = 14 * gfx_char_w() + 30; // longest name is "losangeles" (10) + marker + gap
    g_tz_list.marker_size = gfx_char_h() - 2;
}

static void tz_applet_draw(int x, int y, int w, int h) {
    (void)w; (void)h;
    tz_applet_build_list();

    gfx_draw_string(x, y, "Timezone:", THEME_TEXT, THEME_WINDOW_BG);
    int list_y = y + gfx_char_h() + 8;
    ui_radio_list_draw(&g_tz_list, x, list_y, tz_current_index(),
                        THEME_WINDOW_BG, THEME_TEXT, THEME_SELECTION_BG);

    // The current local time, so the effect of a change is visible
    // right here and not only in the taskbar clock.
    int list_h = 0;
    ui_radio_list_size(&g_tz_list, 0, &list_h);
    struct rtc_time t;
    rtc_read_local(&t);
    char line[48];
    k_snprintf(line, sizeof line, "Local time now: %02u:%02u:%02u",
               (unsigned)t.hour, (unsigned)t.minute, (unsigned)t.second);
    gfx_draw_string(x, list_y + list_h + 10, line, THEME_TEXT, THEME_WINDOW_BG);
}

static int tz_applet_click(int x, int y, int w, int h, int px, int py) {
    (void)w; (void)h;
    tz_applet_build_list();
    int list_y = y + gfx_char_h() + 8;
    int hit = ui_radio_list_hit(&g_tz_list, x, list_y, px, py);
    if (hit < 0) return 0;
    return tz_set_index(hit); // persists to /etc/toyos.conf itself
}

// ---------------------------------------------------------------------
// Applet: System Info (read-only)
// ---------------------------------------------------------------------
//
// Deliberately included as a SECOND applet from the start, even though
// it changes nothing: a plug-in mechanism with exactly one plug-in
// proves nothing about being pluggable. It's the cheapest possible
// second entry -- no state, no input -- and it's what makes the icon
// grid, the drill-in and the Back button meaningful rather than an
// elaborate way to show one page.

static void sysinfo_applet_draw(int x, int y, int w, int h) {
    (void)w; (void)h;
    int line_h = gfx_char_h() + 6;
    char line[64];
    uint64_t used = 0, total = 0;

    gfx_draw_string(x, y, "toy-os v" TOYOS_VERSION, THEME_TEXT, THEME_WINDOW_BG);

    k_snprintf(line, sizeof line, "Memory:  %u KB free of %u KB",
               (unsigned)(pmm_free_frames() * 4), (unsigned)(pmm_total_frames() * 4));
    gfx_draw_string(x, y + 1 * line_h, line, THEME_TEXT, THEME_WINDOW_BG);

    if (fs_disk_usage(&used, &total)) {
        k_snprintf(line, sizeof line, "Disk:    %u MB used of %u MB",
                   (unsigned)(used / (1024 * 1024)), (unsigned)(total / (1024 * 1024)));
    } else {
        k_snprintf(line, sizeof line, "Disk:    unavailable");
    }
    gfx_draw_string(x, y + 2 * line_h, line, THEME_TEXT, THEME_WINDOW_BG);

    k_snprintf(line, sizeof line, "PCI:     %u device(s)", (unsigned)pci_device_count());
    gfx_draw_string(x, y + 3 * line_h, line, THEME_TEXT, THEME_WINDOW_BG);

    k_snprintf(line, sizeof line, "Uptime:  %u s", (unsigned)(pit_ticks() / 100));
    gfx_draw_string(x, y + 4 * line_h, line, THEME_TEXT, THEME_WINDOW_BG);
}

static int sysinfo_applet_click(int x, int y, int w, int h, int px, int py) {
    (void)x; (void)y; (void)w; (void)h; (void)px; (void)py;
    return 0; // read-only
}

// ---------------------------------------------------------------------
// The registry, and the window
// ---------------------------------------------------------------------

static const struct applet g_applets[] = {
    { .name = "Date & Time", .draw = tz_applet_draw,      .click = tz_applet_click },
    { .name = "System Info", .draw = sysinfo_applet_draw, .click = sysinfo_applet_click },
};
#define APPLET_COUNT ((int)(sizeof(g_applets) / sizeof(g_applets[0])))

struct control_panel_state {
    int open_applet; // -1 = showing the chooser grid
};

static struct control_panel_state g_state;

static struct icon_grid chooser_grid(int cx, int cy) {
    struct icon_grid g;
    g.origin_x = cx + CP_MARGIN;
    g.origin_y = cy + CP_MARGIN;
    g.cell_w = CP_CELL_W;
    g.cell_h = CP_CELL_H;
    g.cols = CP_GRID_COLS;
    return g;
}

void control_panel_default_size(int *w, int *h) {
    // Wide enough for the applet grid, tall enough for the tallest
    // applet page -- the timezone list at two columns. Computed from
    // the same constants both layouts use rather than guessed, so a
    // larger font doesn't crop either one.
    int grid_w = 2 * CP_MARGIN + CP_GRID_COLS * CP_CELL_W;
    int page_w = 2 * CP_MARGIN + 2 * (14 * gfx_char_w() + 30);
    *w = grid_w > page_w ? grid_w : page_w;
    *h = 2 * CP_MARGIN + gfx_char_h() + CP_HEADER_GAP + 8 * (gfx_char_h() + 10);
}

void control_panel_open(struct window *win) {
    g_state.open_applet = -1; // always opens on the chooser
    window_set_state(win, &g_state);
}

// The Back button's rectangle, in absolute coordinates. Shared by
// draw() and click() so they can't disagree -- the same reason
// ui_radio_list keeps its geometry in one place.
static void back_button_rect(struct window *win, int *x, int *y, int *w, int *h) {
    *x = window_content_x(win) + CP_MARGIN;
    *y = window_content_y(win) + CP_MARGIN;
    *w = 7 * gfx_char_w() + 12;
    *h = gfx_char_h() + 8;
}

void control_panel_draw(struct window *win) {
    int cx = window_content_x(win);
    int cy = window_content_y(win);
    int cw = window_content_w(win);
    int ch = window_content_h(win);

    gfx_fill_rect(cx, cy, cw, ch, THEME_WINDOW_BG);

    if (g_state.open_applet < 0) {
        struct icon_grid grid = chooser_grid(cx, cy);
        for (int i = 0; i < APPLET_COUNT; i++) {
            int ix, iy;
            icon_grid_cell_rect(&grid, i % CP_GRID_COLS, i / CP_GRID_COLS, &ix, &iy);

            // A plain framed box for the icon, centred in its cell.
            // There's no image decoder yet (Milestone 19), so every
            // "icon" in this OS is drawn geometry -- same as the
            // desktop's.
            int icon_x = ix + (CP_CELL_W - CP_ICON_SIZE) / 2;
            gfx_fill_rect(icon_x, iy, CP_ICON_SIZE, CP_ICON_SIZE, THEME_BUTTON_BG);
            gfx_draw_rect(icon_x, iy, CP_ICON_SIZE, CP_ICON_SIZE, THEME_BORDER);

            // Label centred under the cell and clipped to it. The first
            // version of this hand-rolled the budgeting and got it
            // wrong ("Date & TSystem Info"); gfx_draw_string_clipped()
            // is the helper that mistake produced.
            const char *label = g_applets[i].name;
            int label_w = gfx_text_width(label);
            if (label_w > CP_CELL_W) label_w = CP_CELL_W;
            int label_x = ix + (CP_CELL_W - label_w) / 2;
            gfx_draw_string_clipped(label_x, iy + CP_ICON_SIZE + 6, CP_CELL_W,
                                     label, THEME_TEXT, THEME_WINDOW_BG);
        }
        return;
    }

    int bx, by, bw, bh;
    back_button_rect(win, &bx, &by, &bw, &bh);
    widget_button(bx, by, bw, bh, "< Back", THEME_BUTTON_BG, THEME_TEXT, 0 /* not pressed */);
    gfx_draw_string(bx + bw + 14, by + 4, g_applets[g_state.open_applet].name,
                     THEME_TEXT, THEME_WINDOW_BG);

    int page_y = by + bh + CP_HEADER_GAP;
    g_applets[g_state.open_applet].draw(cx + CP_MARGIN, page_y,
                                         cw - 2 * CP_MARGIN, ch - (page_y - cy) - CP_MARGIN);
}

void control_panel_click(struct window *win, int rel_x, int rel_y) {
    int cx = window_content_x(win);
    int cy = window_content_y(win);
    int cw = window_content_w(win);
    int ch = window_content_h(win);

    // on_click hands CONTENT-RELATIVE coordinates (gui_apps.h: "0,0 =
    // top-left of the content area"), while everything drawn above --
    // and therefore every rectangle hit-tested below -- is in absolute
    // screen coordinates. Converting once here keeps a single
    // coordinate space through the rest of this file. Getting this
    // wrong doesn't fail loudly: clicks just silently do nothing, which
    // is exactly how it presented the first time (the icon grid drew
    // perfectly and refused to open anything).
    int px = cx + rel_x;
    int py = cy + rel_y;

    if (g_state.open_applet < 0) {
        struct icon_grid grid = chooser_grid(cx, cy);
        for (int i = 0; i < APPLET_COUNT; i++) {
            int ix, iy;
            icon_grid_cell_rect(&grid, i % CP_GRID_COLS, i / CP_GRID_COLS, &ix, &iy);
            // The icon box plus its label row, so the text is clickable
            // too -- icon_grid_nearest_cell() deliberately isn't used
            // here: it always returns a cell, which would make a click
            // anywhere in the window open whatever applet was closest.
            if (widget_hit(ix, iy, CP_CELL_W, CP_CELL_H, px, py) && i < APPLET_COUNT) {
                g_state.open_applet = i;
                window_invalidate(win);
                return;
            }
        }
        return;
    }

    int bx, by, bw, bh;
    back_button_rect(win, &bx, &by, &bw, &bh);
    if (widget_hit(bx, by, bw, bh, px, py)) {
        g_state.open_applet = -1;
        window_invalidate(win);
        return;
    }

    int page_y = by + bh + CP_HEADER_GAP;
    if (g_applets[g_state.open_applet].click(cx + CP_MARGIN, page_y,
                                              cw - 2 * CP_MARGIN,
                                              ch - (page_y - cy) - CP_MARGIN, px, py)) {
        window_invalidate(win);
    }
}
