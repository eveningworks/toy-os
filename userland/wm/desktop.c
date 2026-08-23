// See desktop.h for the design writeup.
#include "desktop.h"
#include "wm_internal.h"
#include "context_menu.h"
#include "ui/uui.h"
#include "ui/utheme.h"
#include "rubberband.h"
#include "build_date.h" // GENERATED, and included ONLY here -- see gen_version.sh
#include "kapi.h"
#include "rt/sys.h"
#include "wm/wm_rawin.h"
#include "icon_grid.h"
#include <stdio.h>
#include "wm/wm_conf.h"
#include "wm/wm_log.h"
#include "lib/uimg.h"
#include "ui/uui_image.h"

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
// The plain background, shown when there is no wallpaper and behind a
// letterboxed one.
#define DESKTOP_BG ugfx_rgb(24, 60, 90)
// Where wallpapers live, and what a machine shows when the setting
// cannot be read at all (the registry's own default is the one that
// normally answers -- see wallpaper_reload()).
#define WALLPAPER_DIR     "/usr/share/wallpapers"
#define WALLPAPER_DEFAULT "aurora"

// Selection AND the in-progress band, both owned by the shared module
// (api/rubberband.h) rather than by this file. That is what lets a
// future ring-3 file manager get identical behaviour from the identical
// source rather than a second implementation -- and it is why the rules
// (a shrinking band deselects, Ctrl adds, a plain click on empty space
// clears) are KTESTed with no desktop involved at all.
static struct rubberband sel;

// The band's rect as it was last DRAWN, so a motion can damage the union
// of where it was and where it now is. Same bookkeeping-at-the-point-of-
// drawing rule as wm_render.c's prev_cursor_* -- a band that damages
// only its new rect leaves its old outline on screen.
static int band_drawn = 0;
static int band_x, band_y, band_w, band_h;
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

// GROUP DRAG: when the grabbed icon is part of a multi-selection, every
// selected icon moves together by the same delta and commits as a group
// -- as on Windows and GNOME, where dragging any one of a marquee'd set
// drags the whole set. `drag.index` is still the grabbed "primary"; the
// rest follow it.
//
// drag_start_col/row remember each icon's cell at grab time, so the live
// preview and the drop both offset from where the group STARTED rather
// than from wherever a member has been shoved to mid-drag. drag_origin_*
// is the primary's pixel position at grab, used to tell a real drag from
// a plain click (which collapses the selection to just the primary, the
// only way to single out one icon from a group by clicking it).
static int group_drag = 0;
static int drag_moved = 0;
static int drag_origin_px, drag_origin_py;
static int drag_start_col[DESKTOP_MAX_ICONS];
static int drag_start_row[DESKTOP_MAX_ICONS];

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
    int w = DESKTOP_ICON_LABEL_CHARS * ugfx_char_w() + 8;
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
    wm_conf_set(DESKTOP_CONF_PATH, gui_app_registry[i].name, value);
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
        if (!wm_conf_get(DESKTOP_CONF_PATH, gui_app_registry[i].name, value, sizeof(value))) continue;

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

// --- the wallpaper ----------------------------------------------------
//
// A background image, read from DESKTOP_CONF_PATH's `Wallpaper` key and
// placed according to `WallpaperMode` (fill / fit). Image Viewer writes
// both; nothing else has to know about either.
//
// THE DESKTOP DECODES IT, IN RING 3, and that is the whole layering
// point: KWin and Mutter do not parse image formats -- their shell
// decodes a wallpaper through a toolkit and hands the compositor
// pixels. Here the desktop IS a ring-3 process, so it can simply call
// uimg_load(); the kernel gained nothing to parse and nothing to be
// exploited through (lib/uimg.h has the argument in full).
//
// It is drawn through uui_image, the same widget Image Viewer uses.
// That is worth more than the few lines it saves: the fit maths, the
// centring, the cropping and -- the part that matters at this size --
// the CACHE of the scaled copy are one implementation. Rescaling
// 1280x720 on every desktop repaint would be a stutter that looks like
// the compositor's fault.
static struct uimg wallpaper_src;        // the decoded file, full size
static struct uui_image wallpaper_view;  // placement, and the scaled cache
static char wallpaper_name[SETTING_ABI_VALUE_MAX];
static char wallpaper_mode[16];
static int wallpaper_loaded;

// Reads the two settings and reloads only when something actually
// changed -- so this is safe to call on a generation bump, which fires
// for any write anywhere on the filesystem.
//
// IT ASKS THE SETTINGS REGISTRY, not the file. `wallpaper` and
// `wallpaper_mode` are registered settings (kernel/lib/wallpaper_config.c),
// so the registry knows their defaults and their legal values; reading
// /etc/desktop.conf directly here would mean a second copy of the
// default in a second place, and the two would disagree the first time
// either moved.
static void wallpaper_reload(void) {
    char name[sizeof wallpaper_name];
    char mode[sizeof wallpaper_mode];

    k_strlcpy(name, WALLPAPER_DEFAULT, sizeof name);
    k_strlcpy(mode, "fill", sizeof mode);
    struct setting_msg msg;
    for (unsigned i = 0; i < sizeof msg; i++) ((uint8_t *)&msg)[i] = 0;
    msg.op = SETTING_OP_GET;
    k_strlcpy(msg.name, "desktop.wallpaper", sizeof msg.name);
    if (sys_setting(&msg) == 0 && msg.value[0]) k_strlcpy(name, msg.value, sizeof name);
    for (unsigned i = 0; i < sizeof msg; i++) ((uint8_t *)&msg)[i] = 0;
    msg.op = SETTING_OP_GET;
    k_strlcpy(msg.name, "desktop.wallpaper_mode", sizeof msg.name);
    if (sys_setting(&msg) == 0 && msg.value[0]) k_strlcpy(mode, msg.value, sizeof mode);

    if (k_strcmp(name, wallpaper_name) == 0 && k_strcmp(mode, wallpaper_mode) == 0)
        return;

    k_strlcpy(wallpaper_name, name, sizeof wallpaper_name);
    k_strlcpy(wallpaper_mode, mode, sizeof wallpaper_mode);

    // THE WHOLE DESKTOP IS NOW WRONG, so say so. Without this the new
    // background sits in memory until something else happens to damage
    // the desktop -- and because the taskbar clock, a mouse move and a
    // window opening all do, it LOOKED like it worked while depending on
    // whatever came next. A wallpaper set on an idle desktop appeared
    // seconds later or not at all.
    // THE WHOLE DESKTOP IS NOW WRONG, so declare it rather than waiting
    // to be noticed. Honest scope: this makes the change PROMPT, not
    // correct -- the desktop is repainted on its own cadence anyway, so
    // a wallpaper set without this still appears, about a second later.
    // Measured, by removing these two lines and watching
    // tools/imgview_test.py stay green: the tool cannot tell the two
    // apart, and its comment says so rather than claiming a bug this
    // fixed.
    redraw_pending = 1;
    wm_damage_rect(0, 0, screen_w, screen_h);

    // "fit" letterboxes the whole picture; "fill" covers the screen and
    // crops, which is what every desktop defaults to.
    uui_image_set_fit(&wallpaper_view,
                      k_strcmp(mode, "fit") == 0 ? UIMG_FIT_CONTAIN : UIMG_FIT_COVER);

    uui_image_set(&wallpaper_view, NULL);
    if (wallpaper_loaded) {
        uimg_free(&wallpaper_src);
        wallpaper_loaded = 0;
    }
    if (!name[0] || k_strcmp(name, "none") == 0) {
        wm_logf("desktop: no wallpaper");
        return;
    }

    // A NAME, not a path -- the same rule a font face and a cursor theme
    // follow (kernel/lib/wallpaper_config.c says why).
    char path[80];
    k_snprintf(path, sizeof path, "%s/%s.jpg", WALLPAPER_DIR, name);

    uint64_t t0 = sys_ticks();
    int rc = uimg_load(path, &wallpaper_src);
    if (rc < 0) {
        // NOT fatal and NOT silent: the desktop falls back to its plain
        // colour and says why, because a background that quietly does
        // not appear is indistinguishable from a compositor bug.
        wm_logf("desktop: wallpaper %s not shown -- %s", path, uimg_last_error());
        return;
    }
    wallpaper_loaded = 1;
    uui_image_set(&wallpaper_view, &wallpaper_src);
    wm_logf("desktop: wallpaper %s %dx%d mode %s in %u ticks", name,
            wallpaper_src.w, wallpaper_src.h, mode,
            (unsigned)(sys_ticks() - t0));
}

// Called once per frame from wm.c, beside the .desktop-entry poll and
// for the same reason: there is no inotify here, so a global generation
// counter is what says "something on disk changed, ask again". The idle
// cost is one compare.
void desktop_poll_config(void) {
    static uint64_t seen_gen;
    static int primed;
    uint64_t gen = sys_fs_generation();
    if (!primed) {
        primed = 1;
        seen_gen = gen;
        wallpaper_reload();      // the first call is the initial load
        return;
    }
    if (gen == seen_gen) return;
    seen_gen = gen;
    wallpaper_reload();
}

// Paints the background: the wallpaper if there is one, the plain colour
// otherwise. The plain colour is also what shows THROUGH a letterboxed
// ("fit") wallpaper, which is why it is the widget's own background
// rather than a separate fill.
static void draw_background(void) {
    if (wallpaper_loaded) {
        wallpaper_view.x = 0;
        wallpaper_view.y = 0;
        wallpaper_view.w = screen_w;
        wallpaper_view.h = screen_h;
        uui_image_draw(wm_surface(), &wallpaper_view);
        return;
    }
    ugfx_fill(wm_surface(), DESKTOP_BG);
}

void desktop_draw(void) {
    desktop_load_positions();
    struct icon_grid g = current_grid();

    draw_background();

    uint32_t label_fg = UTHEME_WHITE;
    uint32_t icon_fg = ugfx_rgb(230, 230, 235);
    uint32_t icon_selected_bg = ugfx_rgb(70, 110, 160);

    // The primary follows the cursor; during a GROUP drag the other
    // selected icons follow it by the same pixel delta, drawn from where
    // the group started (their cells don't move until the drop commits).
    int gdx = 0, gdy = 0;
    if (group_drag && drag.active) {
        gdx = drag_px - drag_origin_px;
        gdy = drag_py - drag_origin_py;
    }

    for (int i = 0; i < gui_app_registry_count; i++) {
        if (!gui_app_shows_in(&gui_app_registry[i], GUI_SHOW_DESKTOP)) continue;
        int x, y;
        if (drag.active && drag.index == i) {
            x = drag_px;
            y = drag_py;
        } else if (group_drag && drag.active && rb_is_selected(&sel, i)) {
            icon_grid_cell_rect(&g, drag_start_col[i], drag_start_row[i], &x, &y);
            x += gdx;
            y += gdy;
        } else {
            icon_grid_cell_rect(&g, icon_col[i], icon_row[i], &x, &y);
        }

        if (rb_is_selected(&sel, i)) {
            ugfx_fill_rect(wm_surface(), x - 4, y - 4, DESKTOP_ICON_SIZE + 8,
                           DESKTOP_ICON_SIZE + 8 + 18, icon_selected_bg);
        }

        ugfx_fill_rect(wm_surface(), x, y, DESKTOP_ICON_SIZE, DESKTOP_ICON_SIZE, ugfx_rgb(60, 90, 130));
        ugfx_draw_rect(wm_surface(), x, y, DESKTOP_ICON_SIZE, DESKTOP_ICON_SIZE, icon_fg);

        // Stand-in glyph: the app name's first letter, centered in the
        // square -- see this file's top comment on why (no image
        // decoder yet).
        // The entry's Icon= character, falling back to the name's first
        // letter -- which is what this drew before desktop entries
        // existed, so an entry with no Icon= looks exactly as it did.
        char ic = gui_app_registry[i].icon;
        char initial[2] = { ic ? ic : gui_app_registry[i].name[0], '\0' };
        int gx = x + (DESKTOP_ICON_SIZE - ugfx_char_w()) / 2;
        int gy = y + (DESKTOP_ICON_SIZE - ugfx_char_h()) / 2;
        ugfx_draw_string(wm_surface(), gx, gy, initial, icon_fg, ugfx_rgb(60, 90, 130));

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
        uint32_t label_bg = ugfx_rgb(24, 60, 90);
        const char *label = gui_app_registry[i].name;

        if (ugfx_text_width(label) <= label_max_w) {
            ugfx_draw_string_clipped(wm_surface(), x, label_y, label_max_w, label, label_fg, label_bg);
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
            int cut_w = label_max_w - 2 * ugfx_char_w();
            if (cut_w < ugfx_char_w()) cut_w = ugfx_char_w(); // always show at least one char
            ugfx_draw_string_clipped(wm_surface(), x, label_y, cut_w, label, label_fg, label_bg);
            ugfx_draw_string_clipped(wm_surface(), x + cut_w, label_y, 2 * ugfx_char_w(), "..",
                                     label_fg, label_bg);
        }
    }

    // Version watermark, bottom right -- what build am I looking at, at
    // a glance, the way Windows marks a preview build. Deliberately dim
    // (a few steps off the background rather than UTHEME_WHITE): it is
    // for the moment you go looking for it, and a bright string in the
    // corner of every screenshot would compete with the actual content.
    //
    // TOYOS_VERSION_FULL, not TOYOS_VERSION -- it carries the commit and
    // the -dirty marker, which is the whole reason to want it on screen
    // (api/version.h). Right-aligned from its own measured width, so it
    // stays anchored when the version string or the font size changes.
    //
    // The build DATE goes underneath, because the version string cannot
    // answer "am I still running yesterday's build?" -- a whole day of
    // dev builds share one commit-and-dirty marker. It comes from its
    // own generated header (build_date.h) rather than version.h, at day
    // granularity, so that it rebuilds THIS file and not the tree; see
    // tools/gen_version.sh.
    {
        const char *lines[2] = {
            "toy-os " TOYOS_VERSION_FULL,
            "built " TOYOS_BUILD_DATE,
        };
        uint32_t fg = ugfx_rgb(90, 125, 155);
        uint32_t bg = ugfx_rgb(24, 60, 90);
        int line_h = ugfx_char_h() + 2;
        // Bottom line sits one line above the taskbar; the block grows
        // UPWARDS, so adding a third line later moves nothing.
        int base_y = (screen_h - taskbar_h) - line_h - 8;
        for (int i = 0; i < 2; i++) {
            int w = ugfx_text_width(lines[i]);
            // Each line right-aligned on its own width, so the two stay
            // flush to the same edge whatever the font or the strings.
            ugfx_draw_string_clipped(wm_surface(), screen_w - w - 12,
                                     base_y - (1 - i) * line_h,
                                     w, lines[i], fg, bg);
        }
    }

    // The band LAST, so it sits above every icon it crosses. Drawing is
    // immediate-mode here, so z-order is call order -- the same rule
    // apps/ui's popups follow.
    //
    // An outline rather than the translucent fill Windows and KDE use:
    // gfx.c has no alpha blend, and a solid fill would hide the very
    // icons whose highlight the user is watching appear.
    int bx, by, bw, bh;
    if (rb_rect(&sel, &bx, &by, &bw, &bh)) {
        ugfx_draw_rect(wm_surface(), bx, by, bw, bh, UTHEME_WHITE);
    }
}

// A band counts as a drag for the reload guard's purposes: its selection
// is a set of REGISTRY indices, so a reload underneath one would leave
// the user having selected different icons than the ones they swept.
int desktop_drag_active(void) { return drag.active || sel.armed; }

void desktop_entries_changed(void) {
    positions_loaded = 0;   // re-read from DESKTOP_CONF_PATH, keyed by name
    rb_clear(&sel);         // indices into a table that just changed
    group_drag = 0;         // its snapshot indexes the table that changed
    band_drawn = 0;
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
        if (uui_hit(x, y, DESKTOP_ICON_SIZE, DESKTOP_ICON_SIZE + 18, mx, my)) return i;
    }
    return -1;
}

void desktop_handle_click(int mx, int my) {
    desktop_load_positions();
    int idx = icon_hit_test(mx, my);
    uint64_t now = sys_ticks();

    // Modifiers come from the LIVE keyboard state -- a click carries
    // none of its own. Ctrl adds to the selection, Shift too (both are
    // "extend" on every desktop this imitates); plain replaces.
    uint8_t mods = wm_rawin_mods_now();
    enum rb_mode mode = (mods & (KEY_MOD_CTRL | KEY_MOD_SHIFT))
                        ? RB_ADD : RB_REPLACE;

    if (idx < 0) {
        // Empty space: start a band. The selection is NOT cleared here
        // -- rb_end() does it, and only if this turns out to be a click
        // rather than a drag. Clearing now would make the icons flicker
        // dark the instant a band starts, which is the opposite of what
        // the band is for.
        rb_begin(&sel, mx, my, mode);
        last_click_index = -1;
        redraw_pending = 1;
        return;
    }

    int launched = 0;
    if (idx == last_click_index && now - last_click_tick <= DESKTOP_DOUBLE_CLICK_TICKS) {
        open_app(&gui_app_registry[idx]);
        last_click_index = -1; // avoid a third click within the window re-triggering as a double
        launched = 1;
    } else {
        // Pressing an icon that is ALREADY part of the selection keeps
        // the whole selection, so a drag can move the group -- pressing
        // a SELECTED item must not collapse to it (that is what release
        // without a drag does, below). Pressing an UNSELECTED icon
        // replaces the selection with just it. A modifier always adds.
        if (mode == RB_REPLACE) {
            if (!rb_is_selected(&sel, idx)) { rb_clear(&sel); rb_select(&sel, idx, 1); }
        } else {
            rb_select(&sel, idx, 1);
        }
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

    // A group drag when the grabbed icon is one of several selected;
    // otherwise the ordinary single-icon path. Snapshot every icon's
    // cell so the group offsets from where it started (see the state
    // declarations). group_drag stays 0 after a double-click launch.
    drag_moved = 0;
    drag_origin_px = ix;
    drag_origin_py = iy;
    group_drag = (!launched && drag.active &&
                  rb_is_selected(&sel, idx) && rb_selected_count(&sel) > 1);
    if (group_drag) {
        for (int i = 0; i < gui_app_registry_count && i < DESKTOP_MAX_ICONS; i++) {
            drag_start_col[i] = icon_col[i];
            drag_start_row[i] = icon_row[i];
        }
    }

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

// The items the band tests against: every icon actually ON the desktop,
// at the rect the draw and the hit test already agree on. Indices are
// REGISTRY indices, so rb_is_selected(i) lines up with
// gui_app_registry[i] without a second mapping to keep in step.
static int band_count(void *ctx) {
    (void)ctx;
    return gui_app_registry_count;
}

static void band_item_rect(void *ctx, int i, int *x, int *y, int *w, int *h) {
    (void)ctx;
    struct icon_grid g = current_grid();
    if (!gui_app_shows_in(&gui_app_registry[i], GUI_SHOW_DESKTOP)) {
        // Hidden entries keep their index but occupy nothing, so the
        // band can never select something that is not on screen.
        *x = *y = 0;
        *w = *h = 0;
        return;
    }
    icon_grid_cell_rect(&g, icon_col[i], icon_row[i], x, y);
    *w = DESKTOP_ICON_SIZE;
    *h = DESKTOP_ICON_SIZE + 18; // icon box plus its label, as hit-tested
}

static const struct rb_ops BAND_OPS = { band_count, band_item_rect };

// Damage the union of where the band was drawn and where it is now.
// Damaging only the new rect leaves the old outline behind -- the WM
// repaints declared damage only (docs/gui-guidelines.md).
static void damage_band(void) {
    int nx, ny, nw, nh;
    int have_new = rb_rect(&sel, &nx, &ny, &nw, &nh);

    if (band_drawn) {
        wm_damage_rect(band_x - 1, band_y - 1, band_w + 3, band_h + 3);
    }
    if (have_new) {
        wm_damage_rect(nx - 1, ny - 1, nw + 3, nh + 3);
    }
    band_drawn = have_new;
    band_x = nx; band_y = ny; band_w = nw; band_h = nh;
}

void desktop_update_drag(int mx, int my, uint8_t buttons) {
    // A band in progress takes precedence: it is only ever armed when
    // the press missed every icon, so the two can never both be live.
    if (sel.armed) {
        if (buttons & 0x1) {
            int before = rb_selected_count(&sel);
            rb_motion(&sel, mx, my, &BAND_OPS, 0);
            damage_band();
            // Icons highlight and un-highlight AS the band sweeps, which
            // means their own rects need repainting whenever the set
            // changes -- the band's own rect does not cover them.
            if (rb_selected_count(&sel) != before) {
                wm_damage_rect(0, 0, screen_w, screen_h - taskbar_h);
            }
            redraw_pending = 1;
            return;
        }
        rb_end(&sel);
        damage_band();          // erase the outline
        wm_damage_rect(0, 0, screen_w, screen_h - taskbar_h);
        redraw_pending = 1;
        return;
    }

    if (!drag.active) return;

    if (buttons & 0x1) {
        int old_py = drag_py;
        drag_px = mx - drag.grab_off_x;
        drag_py = my - drag.grab_off_y;
        // A few pixels of slop before a press counts as a drag rather
        // than a click -- a click on a grouped icon singles it out (on
        // release, below), so a shaky hand must not silently drag.
        if (!drag_moved) {
            int ddx = drag_px - drag_origin_px, ddy = drag_py - drag_origin_py;
            if (ddx * ddx + ddy * ddy > 9) drag_moved = 1;
        }
        struct icon_grid g = current_grid();
        if (group_drag) {
            // The group is spread across the desktop, not one strip;
            // repaint the whole icon area. Cheap at this scale (a handful
            // of icons) and always correct.
            wm_damage_rect(0, 0, screen_w, screen_h - taskbar_h);
        } else {
            int top = (old_py < drag_py ? old_py : drag_py) - DESKTOP_DRAG_DAMAGE_MARGIN;
            int bottom = (old_py > drag_py ? old_py : drag_py) + g.cell_h;
            wm_damage_rect(0, top, screen_w, bottom - top);
        }
        redraw_pending = 1;
        return;
    }

    struct icon_grid g = current_grid();

    // Group drop: move every selected icon by the primary's cell delta,
    // then settle each into the nearest free cell so a group landing
    // partly off-grid or onto occupied cells fans out rather than
    // stacking. A release that never became a drag singles the primary
    // out of the group instead (the only way to click one icon out of a
    // marquee'd set).
    if (group_drag) {
        if (!drag_moved) {
            rb_clear(&sel);
            rb_select(&sel, drag.index, 1);
        } else {
            int pcol, prow;
            icon_drag_update(&drag, &g, mx, my, &pcol, &prow);
            int dcol = pcol - drag_start_col[drag.index];
            int drow = prow - drag_start_row[drag.index];
            // Clear the movers first so a member's OLD cell never blocks
            // another member's target during nearest_free_cell().
            for (int i = 0; i < gui_app_registry_count && i < DESKTOP_MAX_ICONS; i++) {
                if (rb_is_selected(&sel, i)) { icon_col[i] = -1; icon_row[i] = -1; }
            }
            for (int i = 0; i < gui_app_registry_count && i < DESKTOP_MAX_ICONS; i++) {
                if (!rb_is_selected(&sel, i)) continue;
                if (!gui_app_shows_in(&gui_app_registry[i], GUI_SHOW_DESKTOP)) continue;
                int wc = drag_start_col[i] + dcol;
                int wr = drag_start_row[i] + drow;
                if (wc < 0) wc = 0;
                if (wc >= g.cols) wc = g.cols - 1;
                if (wr < 0) wr = 0;
                int fc, fr;
                nearest_free_cell(&g, wc, wr, i, &fc, &fr);
                icon_col[i] = fc;
                icon_row[i] = fr;
                save_position(i);
            }
        }
        wm_damage_rect(0, 0, screen_w, screen_h - taskbar_h);
        icon_drag_end(&drag);
        group_drag = 0;
        redraw_pending = 1;
        return;
    }

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
