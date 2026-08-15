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
    // hovered = -1: the applet interface (struct applet's draw/click)
    // doesn't carry the cursor position, so an applet cannot know what is
    // hovered. Passing -1 is honest rather than wrong -- the row hover
    // simply doesn't show here yet. Forwarding hover into applets is
    // recorded in docs/roadmap.md's known-issues list.
    ui_radio_list_draw(&g_tz_list, x, list_y, tz_current_index(), -1,
                        THEME_WINDOW_BG, THEME_TEXT, THEME_SELECTION_BG);

    // The current local time, so the effect of a change is visible
    // right here and not only in the taskbar clock.
    int list_h = 0;
    ui_radio_list_natural_size(&g_tz_list, 0, &list_h);
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

// The System Info page's budget, in characters and rows. Both feed
// control_panel_default_size() so the window opens big enough for this
// page as well as the timezone list -- keep them in step with what
// sysinfo_applet_draw() actually emits below. Every line there is
// written to fit SYSINFO_COLS; the widest is the "Ident:" row.
// 50 columns fits a real hardware brand string behind the 9-character
// "CPU:     " label -- "Intel(R) Core(TM) i7-9750H CPU @ 2.60GHz" is 40
// characters, and QEMU's own are shorter. The field can be up to 48, so
// the very longest possible brand still clips; that's deliberate rather
// than sizing every window for a worst case nothing real hits, and the
// window is resizable.
#define SYSINFO_COLS 50
#define SYSINFO_ROWS 11

// Filled once, on first draw. Nothing in it can change while the
// machine is running, and re-running ~10 CPUID instructions on every
// repaint of a window that repaints on every hover tick would be waste.
static struct cpu_info g_cpu;
static int g_cpu_loaded = 0;

// Total size of the caches at one level, summed across types -- "L1: 64
// KiB" reads better in a fixed-width panel than two lines for L1d/L1i.
// Returns 0 if the CPU reported nothing at that level.
static uint32_t cache_kb_at_level(int level) {
    uint32_t kb = 0;
    for (int i = 0; i < g_cpu.cache_count; i++) {
        if (g_cpu.cache[i].level == level) kb += g_cpu.cache[i].size_kb;
    }
    return kb;
}

static void sysinfo_applet_draw(int x, int y, int w, int h) {
    int line_h = gfx_char_h() + 6;
    char line[96];
    uint64_t used = 0, total = 0;
    int row = 0;

    if (!g_cpu_loaded) { cpu_info_get(&g_cpu); g_cpu_loaded = 1; }

    // Two axes, two separate budgets, and the second one is easy to
    // forget because the helper's name suggests it's covered.
    //
    // WIDTH: gfx_draw_string_clipped(), not gfx_draw_string() -- the CPU
    // brand string runs to 48 characters and would draw straight off the
    // page. See docs/gui-guidelines.md.
    //
    // HEIGHT: that helper bounds width ONLY -- it has no row budget at
    // all -- so a row past the bottom of the page is simply drawn
    // wherever it lands. Shrinking this window used to send the lower
    // half of this page marching down the desktop, fully legible,
    // outside any window. The WM clips app drawing to its content
    // rect now (apps/wm/wm_render.c) so that can't reach the screen
    // any more, but stopping here as well means the page ends on a
    // whole line rather than one sliced through the middle of its
    // glyphs.
    #define SYSINFO_LINE(fmt_done) \
        do { \
            int ly = y + row * line_h; \
            if (ly + gfx_char_h() > y + h) break; \
            gfx_draw_string_clipped(x, ly, w, (fmt_done), THEME_TEXT, THEME_WINDOW_BG); \
            row++; \
        } while (0)

    SYSINFO_LINE("toy-os v" TOYOS_VERSION);

    // The brand string is a fixed 48-byte field, space-padded on both
    // sides by most CPUs -- trim before showing it.
    const char *brand = g_cpu.brand;
    while (*brand == ' ') brand++;
    if (!*brand) brand = g_cpu.vendor; // no brand-string leaves on very old parts
    k_snprintf(line, sizeof line, "CPU:     %s", brand);
    SYSINFO_LINE(line);

    k_snprintf(line, sizeof line, "Vendor:  %s", g_cpu.vendor);
    SYSINFO_LINE(line);

    k_snprintf(line, sizeof line, "Ident:   family %u, model %u, stepping %u",
               (unsigned)g_cpu.family, (unsigned)g_cpu.model, (unsigned)g_cpu.stepping);
    SYSINFO_LINE(line);

    if (g_cpu.mhz) {
        k_snprintf(line, sizeof line, "Speed:   ~%u MHz (%s)", (unsigned)g_cpu.mhz,
                   g_cpu.mhz_source == CPU_MHZ_CPUID_16H ? "CPUID" : "measured");
    } else {
        k_snprintf(line, sizeof line, "Speed:   unknown");
    }
    SYSINFO_LINE(line);

    uint32_t l1 = cache_kb_at_level(1), l2 = cache_kb_at_level(2), l3 = cache_kb_at_level(3);
    if (l1 || l2 || l3) {
        // L3 is routinely tens of megabytes, and "16384 KB" both reads
        // badly and costs the columns this line doesn't have.
        char l3buf[16];
        if (l3 >= 1024) k_snprintf(l3buf, sizeof l3buf, "%u MB", (unsigned)(l3 / 1024));
        else            k_snprintf(l3buf, sizeof l3buf, "%u KB", (unsigned)l3);
        k_snprintf(line, sizeof line, "Cache:   L1 %u KB  L2 %u KB  L3 %s",
                   (unsigned)l1, (unsigned)l2, l3buf);
    } else {
        k_snprintf(line, sizeof line, "Cache:   not reported by this CPU");
    }
    SYSINFO_LINE(line);

    // The supported-vs-enabled distinction, compressed to one line. See
    // /bin/lscpu for the full picture -- this is the summary that fits.
    k_snprintf(line, sizeof line, "Enabled: SSE %s  NX %s  SMEP %s  SMAP %s",
               (g_cpu.enabled & CPU_EN_SSE) ? "on" : "off",
               (g_cpu.enabled & CPU_EN_NX) ? "on" : "off",
               (g_cpu.enabled & CPU_EN_SMEP) ? "on" : "off",
               (g_cpu.enabled & CPU_EN_SMAP) ? "on" : "off");
    SYSINFO_LINE(line);

    k_snprintf(line, sizeof line, "Memory:  %u KB free of %u KB",
               (unsigned)(pmm_free_frames() * 4), (unsigned)(pmm_total_frames() * 4));
    SYSINFO_LINE(line);

    if (fs_disk_usage(&used, &total)) {
        k_snprintf(line, sizeof line, "Disk:    %u MB used of %u MB",
                   (unsigned)(used / (1024 * 1024)), (unsigned)(total / (1024 * 1024)));
    } else {
        k_snprintf(line, sizeof line, "Disk:    unavailable");
    }
    SYSINFO_LINE(line);

    k_snprintf(line, sizeof line, "PCI:     %u device(s)", (unsigned)pci_device_count());
    SYSINFO_LINE(line);

    k_snprintf(line, sizeof line, "Uptime:  %u s", (unsigned)(pit_ticks() / 100));
    SYSINFO_LINE(line);

    #undef SYSINFO_LINE
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
    int open_applet;   // -1 = showing the chooser grid
    int hover_icon;    // applet index under the cursor, or -1
    int armed_icon;    // applet index pressed but not yet released, or -1
    int armed_active;  // 1 while the cursor is still over armed_icon
    int hover_back;    // 1 when the cursor is over the Back button
    int armed_back;    // 1 while Back is held
    int last_px, last_py; // last position on_press saw, in absolute coords --
                          // on_release gets no coordinates of its own
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

// The applet cell under a point, or -1. ONE hit-test shared by drawing,
// hover, press and click: the Control Panel's first version had the
// press path and the draw path computing cell geometry separately, and
// the click path testing it in the wrong coordinate space entirely,
// which is how it shipped a grid that rendered perfectly and opened
// nothing. See docs/gui-guidelines.md.
static int applet_at(struct window *win, int px, int py) {
    struct icon_grid grid = chooser_grid(window_content_x(win), window_content_y(win));
    for (int i = 0; i < APPLET_COUNT; i++) {
        int ix, iy;
        icon_grid_cell_rect(&grid, i % CP_GRID_COLS, i / CP_GRID_COLS, &ix, &iy);
        if (widget_hit(ix, iy, CP_CELL_W, CP_CELL_H, px, py)) return i;
    }
    return -1;
}

void control_panel_default_size(int *w, int *h) {
    // Wide enough for the applet grid, tall enough for the tallest
    // applet page -- the timezone list at two columns. Computed from
    // the same constants both layouts use rather than guessed, so a
    // larger font doesn't crop either one.
    int grid_w = 2 * CP_MARGIN + CP_GRID_COLS * CP_CELL_W;
    int tz_w   = 2 * CP_MARGIN + 2 * (14 * gfx_char_w() + 30);
    // System Info is a third layout with its own demands, and it has to
    // be counted here or its lines get clipped to nothing in the default
    // window -- which is exactly what happened when the CPU section was
    // added and only the two widths above were considered. The page
    // clips correctly (gfx_draw_string_clipped, per the guidelines), so
    // the failure was silent truncation rather than overdraw: "L3 16"
    // with the unit cut off.
    int info_w = 2 * CP_MARGIN + SYSINFO_COLS * gfx_char_w();

    *w = grid_w;
    if (tz_w > *w) *w = tz_w;
    if (info_w > *w) *w = info_w;

    int tz_h   = 8 * (gfx_char_h() + 10);
    int info_h = SYSINFO_ROWS * (gfx_char_h() + 6);
    *h = 2 * CP_MARGIN + gfx_char_h() + CP_HEADER_GAP + (tz_h > info_h ? tz_h : info_h);
}

void control_panel_open(struct window *win) {
    g_state.open_applet = -1; // always opens on the chooser
    g_state.hover_icon = g_state.armed_icon = -1;
    g_state.armed_active = g_state.hover_back = g_state.armed_back = 0;
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

            // Interaction state for this cell. Armed-but-dragged-off
            // falls back to REST, not hover -- see
            // docs/gui-guidelines.md; a control about to be cancelled
            // must not look like it is still being interacted with.
            enum ui_state st = UI_STATE_REST;
            if (g_state.armed_icon == i) {
                st = g_state.armed_active ? UI_STATE_PRESSED : UI_STATE_REST;
            } else if (g_state.armed_icon < 0 && g_state.hover_icon == i) {
                st = UI_STATE_HOVER;
            }

            // The WHOLE CELL carries the state wash (icon box + label),
            // which is what makes a grid read as a grid rather than as
            // loose boxes with text under them.
            if (st != UI_STATE_REST) {
                gfx_fill_rect(ix, iy, CP_CELL_W, CP_CELL_H,
                               ui_state_bg(THEME_WINDOW_BG, st));
            }

            // A plain framed box for the icon, centred in its cell.
            // There's no image decoder yet (Milestone 19), so every
            // "icon" in this OS is drawn geometry -- same as the
            // desktop's.
            int nudge = (st == UI_STATE_PRESSED) ? 1 : 0;
            int icon_x = ix + (CP_CELL_W - CP_ICON_SIZE) / 2 + nudge;
            int icon_y = iy + nudge;
            gfx_fill_rect(icon_x, icon_y, CP_ICON_SIZE, CP_ICON_SIZE, THEME_BUTTON_BG);
            gfx_draw_rect(icon_x, icon_y, CP_ICON_SIZE, CP_ICON_SIZE, THEME_BORDER);

            // Label centred under the cell and clipped to it. The first
            // version of this hand-rolled the budgeting and got it
            // wrong ("Date & TSystem Info"); gfx_draw_string_clipped()
            // is the helper that mistake produced.
            const char *label = g_applets[i].name;
            int label_w = gfx_text_width(label);
            if (label_w > CP_CELL_W) label_w = CP_CELL_W;
            int label_x = ix + (CP_CELL_W - label_w) / 2 + nudge;
            gfx_draw_string_clipped(label_x, iy + CP_ICON_SIZE + 6 + nudge, CP_CELL_W,
                                     label, THEME_TEXT, ui_state_bg(THEME_WINDOW_BG, st));
        }
        return;
    }

    int bx, by, bw, bh;
    back_button_rect(win, &bx, &by, &bw, &bh);
    enum ui_state back_st = g_state.armed_back
                           ? (g_state.hover_back ? UI_STATE_PRESSED : UI_STATE_REST)
                           : (g_state.hover_back ? UI_STATE_HOVER : UI_STATE_REST);
    widget_button_state(bx, by, bw, bh, "< Back", THEME_BUTTON_BG, THEME_TEXT, back_st);
    gfx_draw_string(bx + bw + 14, by + 4, g_applets[g_state.open_applet].name,
                     THEME_TEXT, THEME_WINDOW_BG);

    int page_y = by + bh + CP_HEADER_GAP;
    g_applets[g_state.open_applet].draw(cx + CP_MARGIN, page_y,
                                         cw - 2 * CP_MARGIN, ch - (page_y - cy) - CP_MARGIN);
}

// Cursor moved over the window with no button held. Returns 1 only when
// the hovered item CHANGED -- gui_apps.h's on_hover contract, and what
// keeps this from repainting the window every single tick.
int control_panel_hover(struct window *win, int rel_x, int rel_y) {
    int cx = window_content_x(win);
    int cy = window_content_y(win);
    int left = (rel_x < 0 || rel_y < 0); // the cursor left this window
    int px = cx + rel_x, py = cy + rel_y;

    if (g_state.open_applet < 0) {
        int now = left ? -1 : applet_at(win, px, py);
        if (now == g_state.hover_icon) return 0;
        g_state.hover_icon = now;
        return 1;
    }

    int bx, by, bw, bh;
    back_button_rect(win, &bx, &by, &bw, &bh);
    int now = left ? 0 : widget_hit(bx, by, bw, bh, px, py);
    if (now == g_state.hover_back) return 0;
    g_state.hover_back = now;
    return 1;
}

// Button held. Called every tick with live coordinates (see gui_apps.h),
// which is what lets "still over the armed control?" track the cursor
// rather than being decided once at press time.
int control_panel_press(struct window *win, int rel_x, int rel_y) {
    int px = window_content_x(win) + rel_x;
    int py = window_content_y(win) + rel_y;
    g_state.last_px = px;
    g_state.last_py = py;

    if (g_state.open_applet < 0) {
        int hit = applet_at(win, px, py);
        if (g_state.armed_icon < 0) {           // first tick of this press
            if (hit < 0) return 0;
            g_state.armed_icon = hit;
            g_state.armed_active = 1;
            g_state.hover_icon = -1;            // press visual owns it now
            return 1;
        }
        int active = (hit == g_state.armed_icon);
        if (active == g_state.armed_active) return 0;
        g_state.armed_active = active;
        return 1;
    }

    int bx, by, bw, bh;
    back_button_rect(win, &bx, &by, &bw, &bh);
    int over = widget_hit(bx, by, bw, bh, px, py);
    if (!g_state.armed_back) {
        if (!over) return 0;
        g_state.armed_back = 1;
        return 1;
    }
    if (over == g_state.hover_back) return 0;
    g_state.hover_back = over;
    return 1;
}

// Where the action actually happens.
//
// NOT in on_click: despite its name and its doc comment, the WM fires
// on_click on button-DOWN (wm_input.c), so nothing armed by on_press
// exists yet when it runs -- a control that committed there would fire
// on press and could never be cancelled by dragging away. on_release is
// the only callback that means "the user let go", which is what
// commit-on-target requires. Same structure as the title-bar buttons'
// wm_update_title_btn_press(). See docs/gui-guidelines.md.
void control_panel_release(struct window *win) {
    int armed_icon = g_state.armed_icon;
    int armed_active = g_state.armed_active;
    int armed_back = g_state.armed_back;
    g_state.armed_icon = -1;
    g_state.armed_active = 0;
    g_state.armed_back = 0;

    if (g_state.open_applet < 0) {
        // Commit only if the release landed on the same cell the press
        // armed. Press, drag away, release: nothing happens.
        if (armed_icon >= 0 && armed_active) {
            g_state.open_applet = armed_icon;
            g_state.hover_icon = -1;
            window_invalidate(win);
            return;
        }
        window_invalidate(win); // clear the pressed look
        return;
    }

    int bx, by, bw, bh;
    back_button_rect(win, &bx, &by, &bw, &bh);
    if (armed_back && widget_hit(bx, by, bw, bh, g_state.last_px, g_state.last_py)) {
        g_state.open_applet = -1;
        g_state.hover_back = 0;
        window_invalidate(win);
        return;
    }

    // Inside an applet page: hand the release to the applet, so its own
    // controls follow the same press-then-release-on-target rule.
    int cw = window_content_w(win), ch = window_content_h(win);
    int cy = window_content_y(win);
    int page_y = by + bh + CP_HEADER_GAP;
    if (g_applets[g_state.open_applet].click(window_content_x(win) + CP_MARGIN, page_y,
                                              cw - 2 * CP_MARGIN,
                                              ch - (page_y - cy) - CP_MARGIN,
                                              g_state.last_px, g_state.last_py)) {
        window_invalidate(win);
    }
}
