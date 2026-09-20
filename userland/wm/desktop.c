// See desktop.h for the design writeup.
#include "desktop.h"
#include "lib/usetting.h" // the MERGED registry: these settings are declared, not registered
#include "start_store.h"
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
#include "lib/icon_cache.h"
#include "wm/wm_log.h"
#include "lib/uimg.h"
#include "ui/uui_image.h"
#include "ui/uui_label.h"  // uui_label_wrap_next() -- the caption's wrap
#include "lib/uclip.h"      // the desktop's Copy/Cut/Paste ride the system clipboard
#include "confirm_dialog.h" // Delete asks first
#include "wm_dnd.h"         // a file icon dragged onto a window
#include "kpath.h"
#include <stdint.h>

// THE ICON SIZE IS A SETTING (`desktop.icon_size`, small|medium|large ->
// 32/48/64 px), read on the same generation poll as the wallpaper; the
// column pitch stays font-derived. Labels get TWO lines under the icon
// (DESKTOP_LABEL_LINES), word-wrapped and centred, the second cut with
// ".." when there is more -- KDE's and Windows' default.
#define DESKTOP_ICON_X 16
#define DESKTOP_ICON_START_Y 16
#define DESKTOP_LABEL_LINES 2
static int g_icon_px = 48;
static char g_icon_size[16];   // the setting's word, to notice a change
static int icon_px(void) { return g_icon_px; }
static int label_line_h(void) { return ugfx_char_h() + 1; }
static int label_h(void) { return DESKTOP_LABEL_LINES * label_line_h(); }
static int row_h(void) { return icon_px() + 4 + label_h() + 10; } // icon + gap + label + gap to the next row
// Column pitch, in CHARACTERS of the active font -- see icon_col_w().
// 13 is "Control Panel"/"Task Manager", the longest labels that should
// never be truncated; anything longer (the "(ring 3)" launchers) is cut
// with a ".." marker rather than widening every column to fit it.
#define DESKTOP_ICON_LABEL_CHARS 13
#define DESKTOP_DOUBLE_CLICK_TICKS 30 // ~300ms at the PIT's 100Hz -- same order of magnitude as start_menu.c's flash
#define DESKTOP_MAX_ICONS 64
#define DESKTOP_CONF_PATH "/etc/desktop.conf"
// THE DESKTOP FOLDER IS THE DESKTOP. Every icon is an entry of this
// directory, in name order with directories first -- KDE's Folder View
// and the Windows desktop, a folder with the shortcuts in it. A
// `.desktop` file here is a LAUNCHER: drawn with its Name= and Icon=,
// opened as the app it names (g_launch[i], see desktop_files_parse()).
// The application database (/usr/wm/applications) feeds the Start menu
// and the Open > submenu, never an icon. Read on the same generation
// poll as the wallpaper; a change re-derives the grid.
#define DESKTOP_DIR "/home/desktop"
#define DESKTOP_FILES_MAX DESKTOP_MAX_ICONS
static struct sys_dirent g_files[DESKTOP_FILES_MAX];
static struct gui_app_entry g_launch[DESKTOP_FILES_MAX];   // name[0] == 0: a plain file
static int g_file_count;
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
#define PATH_BUF 80
static void item_activate(int i);
static void reflow_overflow(void);
static int selected_paths(char paths[][PATH_BUF], int cap);

// The band's rect as it was last DRAWN, so a motion can damage the union
// of where it was and where it now is. Same bookkeeping-at-the-point-of-
// drawing rule as wm_render.c's prev_cursor_* -- a band that damages
// only its new rect leaves its old outline on screen.
static int band_drawn = 0;
static int band_x, band_y, band_w, band_h;
static int last_click_index = -1;
static uint64_t last_click_tick = 0;

// Per-icon grid position, indexed the same as g_files[] --
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
// **RESERVED IN A REPRESENTATIVE GLYPH, NOT THE WIDEST ONE.**
// `ugfx_char_w()` is the widest advance in the face: the same number as
// every other advance on a monospace face, and nearly twice the average
// on a proportional one. Thirteen of it took the column from 112px to
// 190px the day the interface face became Liberation Sans, and the
// selection highlight -- which is a column wide -- went with it.
//
// `advance('n')` keeps the column at the 112px it has always been,
// which is what the captions were sized against: "System Settings"
// measures ~105px and fits, a longer name still wraps to the second
// line. A column is a PITCH for text nobody has seen yet, so it wants
// the width of ordinary text rather than of the one widest glyph.
static int icon_col_w(void) {
    int per = ugfx_char_advance('n');
    if (per <= 0) per = ugfx_char_w();
    int w = DESKTOP_ICON_LABEL_CHARS * per + 8;
    return w < icon_px() + 8 ? icon_px() + 8 : w;
}

// The icon is CENTRED in its column (every desktop's arrangement); the
// cell's own x stays the column's left edge, which is what the grid,
// the drag and the saved positions speak.
static int icon_dx(void) { return (icon_col_w() - icon_px()) / 2; }

// Rows that fit above the taskbar at the current size -- what the
// default layout wraps at, and what a reflow after a size change keeps
// every icon within.
static int rows_that_fit(void) {
    int usable_h = (screen_h - taskbar_h) - DESKTOP_ICON_START_Y;
    int per_col = usable_h / row_h();
    return per_col < 1 ? 1 : per_col;
}

static struct icon_grid current_grid(void) {
    struct icon_grid g;
    g.origin_x = DESKTOP_ICON_X;
    g.origin_y = DESKTOP_ICON_START_Y;
    g.cell_w = icon_col_w();
    g.cell_h = row_h();
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

// --- the item space: the folder's entries -------------------------------
static int item_count(void) { return g_file_count; }
static int item_is_launcher(int i) { return g_launch[i].name[0] != '\0'; }
static int item_is_dir(int i) { return g_files[i].is_dir; }
static const char *item_name(int i) {
    return item_is_launcher(i) ? g_launch[i].name : g_files[i].name;
}
static int item_visible(int i) { return i >= 0 && i < item_count(); }
// The saved-position key is the FILENAME, so a launcher keeps its cell
// whatever its Name= says (the "file:" prefix is what older configs
// keyed files by, kept so their cells survive).
static const char *item_key(int i, char *buf, int cap) {
    k_snprintf(buf, (size_t)cap, "file:%s", g_files[i].name);
    return buf;
}
static void item_path(int i, char *out, int cap) {
    k_snprintf(out, (size_t)cap, DESKTOP_DIR "/%s", g_files[i].name);
}
static int is_desktop_file(const char *name) {
    size_t n = k_strlen(name);
    return n > 8 && k_strcmp(name + n - 8, ".desktop") == 0;
}
// Re-reads every launcher file. Only when the set of names changed, or
// on Refresh: a same-name edit is not seen until then, the trade the
// application database's fingerprint makes too.
static void desktop_files_parse(void) {
    for (int i = 0; i < g_file_count; i++) {
        g_launch[i].name[0] = '\0';
        if (item_is_dir(i) || !is_desktop_file(g_files[i].name)) continue;
        char path[PATH_BUF];
        item_path(i, path, sizeof path);
        if (!gui_app_read_entry(path, &g_launch[i])) g_launch[i].name[0] = '\0';
    }
}

// Re-lists the folder. Returns 1 if the set of names changed, which
// is what a caller uses to re-derive positions and drop a selection
// that indexed the old list.
static int desktop_files_reload(void) {
    static struct sys_dirent fresh[DESKTOP_FILES_MAX];
    int n = sys_listdir(DESKTOP_DIR, fresh, DESKTOP_FILES_MAX);
    if (n < 0) n = 0;
    // Directories first, then by name -- the File Manager's order.
    for (int i = 1; i < n; i++) {
        struct sys_dirent t = fresh[i];
        int j = i - 1;
        while (j >= 0 && (( !fresh[j].is_dir && t.is_dir) ||
                          (fresh[j].is_dir == t.is_dir && k_strcmp(fresh[j].name, t.name) > 0))) {
            fresh[j + 1] = fresh[j];
            j--;
        }
        fresh[j + 1] = t;
    }
    int changed = (n != g_file_count);
    for (int i = 0; !changed && i < n; i++)
        if (k_strcmp(fresh[i].name, g_files[i].name) != 0 || fresh[i].is_dir != g_files[i].is_dir)
            changed = 1;
    if (!changed) return 0;
    for (int i = 0; i < n; i++) g_files[i] = fresh[i];
    g_file_count = n;
    desktop_files_parse();
    return 1;
}

static void save_position(int i) {
    char value[16], keybuf[80];
    format_pos(value, icon_col[i], icon_row[i]);
    wm_conf_set(DESKTOP_CONF_PATH, item_key(i, keybuf, sizeof keybuf), value);
}

// Loads every icon's position from DESKTOP_CONF_PATH, defaulting to
// the default layout (top-to-bottom, wrapping into a new column at the
// bottom edge -- see below) for any app with no saved entry yet --
// keyed by app name (not registry index), so a registry reorder
// doesn't scramble saved positions. Runs once per boot; positions don't change except via a
// drag, which updates icon_col/icon_row directly, so there's nothing
// to invalidate this cache.
static int cell_taken(int col, int row, int exclude);

static void desktop_load_positions(void) {
    if (positions_loaded) return;
    positions_loaded = 1;

    int n = item_count();

    // How many icons fit in one column before running off the bottom.
    // The default layout WRAPS into a second column rather than being a
    // single unbounded one: that used to be `icon_row[i] = i`, which was
    // fine while the registry held seven apps and silently walked icons
    // off the screen the moment it held eleven (the four ring-3
    // launchers). Icons past the edge are not just invisible -- they are
    // unclickable, so an app can be in the registry and unreachable from
    // the desktop with nothing to indicate why.
    int per_col = rows_that_fit();

    // TWO PASSES, AND THE ORDER IS THE POINT. This used to default every
    // icon to its index's cell and then overwrite that with a saved
    // position, which means a NEW icon's default cell could be one a
    // saved position already owns -- with nothing checking. A launcher
    // added from the Start menu landed ON TOP of another icon.
    //
    // Pass 1 places only what the user actually chose and parks the rest
    // OFF-GRID, so pass 2 can tell "not placed yet" from "placed at 0,0".
    for (int i = 0; i < n; i++) {
        icon_col[i] = icon_row[i] = -1; // off-grid: never drawn, never hit-tested
        if (!item_visible(i)) continue;

        char value[16], keybuf[80];
        if (!wm_conf_get(DESKTOP_CONF_PATH, item_key(i, keybuf, sizeof keybuf), value, sizeof(value))) continue;

        // "<col>,<row>" -- split on the comma, then let knum.h's bounded
        // parser handle each half. Stricter than the digit loops this
        // replaced: those accepted trailing junk ("3,4x" parsed as 3,4),
        // where this treats the whole field as malformed and leaves the
        // icon to pass 2, matching how every other config value behaves.
        const char *comma = k_strchr(value, ',');
        if (!comma) continue;

        uint64_t col = 0, row = 0;
        if (!k_parse_u64_n(value, (size_t)(comma - value), &col)) continue;
        if (!k_parse_u64(comma + 1, &row)) continue;

        icon_col[i] = (int)col;
        icon_row[i] = (int)row;
    }

    // Pass 2: everything with no saved cell takes the first FREE one in
    // the default column-major order -- Windows' rule for a new shortcut.
    // Bounded by the pigeonhole: at most DESKTOP_MAX_ICONS icons can
    // occupy DESKTOP_MAX_ICONS cells, so one more slot than that is
    // always free.
    for (int i = 0; i < n; i++) {
        if (!item_visible(i) || icon_col[i] >= 0) continue;
        for (int slot = 0; slot <= DESKTOP_MAX_ICONS; slot++) {
            int col = slot / per_col, row = slot % per_col;
            if (cell_taken(col, row, i)) continue;
            icon_col[i] = col;
            icon_row[i] = row;
            break;
        }
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
    // usetting_get(), NOT sys_setting(): these are DECLARED in
    // /etc/settings.d, and the syscall answers for the kernel's
    // settings alone -- it would read both as unset.
    char v[SETTING_ABI_VALUE_MAX];
    if (usetting_get("desktop.wallpaper", v, sizeof v) && v[0])
        k_strlcpy(name, v, sizeof name);
    if (usetting_get("desktop.wallpaper_mode", v, sizeof v) && v[0])
        k_strlcpy(mode, v, sizeof mode);

    int name_changed = k_strcmp(name, wallpaper_name) != 0;
    int mode_changed = k_strcmp(mode, wallpaper_mode) != 0;
    if (!name_changed && !mode_changed) return;

    k_strlcpy(wallpaper_name, name, sizeof wallpaper_name);
    k_strlcpy(wallpaper_mode, mode, sizeof wallpaper_mode);

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

    // A MODE CHANGE IS NOT A NEW PICTURE. uui_image_draw() derives the
    // drawn size from `fit` every frame and rescales its own cache, so
    // the same decoded source serves either mode -- decoding it again
    // would spend a whole JPEG to arrive at identical pixels, on the
    // thread that owes the next frame.
    if (!name_changed && wallpaper_loaded) return;

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

// `desktop.icon_size`, the same way: the registry knows the words, the
// desktop knows the pixels. A change repaints the whole desktop; saved
// cell positions are kept, since a cell is a (col, row) and not a pixel.
static void icon_size_reload(void) {
    char value[SETTING_ABI_VALUE_MAX];
    const char *word = "medium";
    if (usetting_get("desktop.icon_size", value, sizeof value) && value[0])
        word = value;
    if (k_strcmp(word, g_icon_size) == 0) return;
    k_strlcpy(g_icon_size, word, sizeof g_icon_size);
    g_icon_px = k_strcmp(word, "small") == 0 ? 32 :
                k_strcmp(word, "large") == 0 ? 64 : 48;
    reflow_overflow();
    redraw_pending = 1;
    wm_damage_rect(0, 0, screen_w, screen_h);
}

const char *desktop_icon_size_word(void) { return g_icon_size; }

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
        icon_size_reload();
        wallpaper_reload();      // the first call is the initial load
        if (desktop_files_reload()) positions_loaded = 0;
        return;
    }
    if (gen == seen_gen) return;
    seen_gen = gen;
    icon_size_reload();
    wallpaper_reload();
    // The folder, unless a band or a drag indexes the current list --
    // the same rule the .desktop-entry reload follows (wm.c).
    if (!desktop_drag_active() && desktop_files_reload()) desktop_entries_changed();
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

// The rect a press or a band tests against: the centred icon box plus
// its label lines. One function, so drawing, hit-testing and the debug
// console's `gui icons` cannot disagree about where an icon is.
static void icon_box(const struct icon_grid *g, int i, int *x, int *y, int *w, int *h) {
    int cx, cy;
    icon_grid_cell_rect(g, icon_col[i], icon_row[i], &cx, &cy);
    *x = cx + icon_dx();
    *y = cy;
    *w = icon_px();
    *h = icon_px() + 4 + label_h();
}

// **THE SELECTION HIGHLIGHT'S RECT, IN ONE PLACE.** The fill and the
// damage both come from here, and they used not to: the fill was a
// literal in the draw loop and the damage used icon_box(), which is the
// HIT rect and is strictly inside the fill on all four sides. So every
// deselection left the overhang unpainted -- a blue outline around
// where the highlight had been, one row of it under every icon clicked
// past. Pre-existing; it surfaced while the proportional face was
// being measured, not because of it.
//
// Deliberately NOT icon_box() itself: that one is what a CLICK is
// tested against, and widening it would quietly enlarge every icon's
// target. One rect for one job, two jobs.
// Taken from a CELL'S TOP-LEFT rather than from an index, so the draw
// can pass a dragged icon's offset position and the damage the grid's,
// with one derivation of the size either way.
static void icon_hl_rect(int cell_x, int cell_y, int *x, int *y, int *w, int *h) {
    *x = cell_x + 2;
    *y = cell_y - 4;
    *w = icon_col_w() - 4;
    *h = icon_px() + 4 + label_h() + 6;
}

// How many label lines `name` takes at the column's width, capped at
// DESKTOP_LABEL_LINES -- what `gui icons` reports, so a test can assert
// a long name WRAPPED rather than reading pixels.
static int label_lines(const char *name) {
    int max_w = icon_col_w() - 4;
    char line[64];
    const char *rest = name;
    int n = 0;
    while (*rest && n < DESKTOP_LABEL_LINES) {
        rest = uui_label_wrap_next(rest, max_w, line, sizeof line);
        n++;
    }
    return n;
}

// The caption: up to DESKTOP_LABEL_LINES word-wrapped lines centred
// under the icon, the last cut with ".." when the name runs on --
// KDE's and Windows' default. ".." rather than U+2026: the font is
// indexed from ASCII 32 (kernel/drivers/font_ttf.c), so an ellipsis
// glyph would draw as nothing. Clipped, always (docs/gui-guidelines.md).
static void draw_label(int cell_x, int label_y, const char *name, uint32_t fg) {
    int max_w = icon_col_w() - 4;
    int left = cell_x + 2;
    char line[64];
    const char *rest = name;
    for (int n = 0; n < DESKTOP_LABEL_LINES && *rest; n++) {
        rest = uui_label_wrap_next(rest, max_w, line, sizeof line);
        int y = label_y + n * label_line_h();
        int last = (n == DESKTOP_LABEL_LINES - 1) && *rest;
        if (!last) {
            int tw = ugfx_text_width(line);
            int lx = tw < max_w ? left + (max_w - tw) / 2 : left;
            ugfx_draw_string_clipped_shadowed(wm_surface(), lx, y, max_w, line, fg);
            continue;
        }
        // More than fits: this line is cut two characters short and
        // marked, so a truncated caption reads AS truncated rather than
        // as a differently-named app.
        int ell = ugfx_text_width("..");
        int cut_w = max_w - ell;
        if (cut_w < ell) cut_w = ell;
        int tw = ugfx_text_width(line);
        if (tw > cut_w) tw = cut_w;
        int lx = left + (max_w - (tw + ell)) / 2;
        if (lx < left) lx = left;
        ugfx_draw_string_clipped_shadowed(wm_surface(), lx, y, cut_w, line, fg);
        ugfx_draw_string_clipped_shadowed(wm_surface(), lx + tw, y, ell, "..", fg);
    }
}

int desktop_icon_geometry(int i, const char **name, int *x, int *y, int *w, int *h,
                          int *lines, const char **kind) {
    desktop_load_positions();
    if (!item_visible(i)) return 0;
    struct icon_grid g = current_grid();
    icon_box(&g, i, x, y, w, h);
    if (name) *name = item_name(i);
    if (lines) *lines = label_lines(item_name(i));
    if (kind) *kind = item_is_launcher(i) ? "app" : item_is_dir(i) ? "dir" : "file";
    return 1;
}

int desktop_icon_count(void) { return item_count(); }

int desktop_icon_px(void) { return icon_px(); }

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

    for (int i = 0; i < item_count(); i++) {
        if (!item_visible(i)) continue;
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

        // The cell's x is the column's left; the icon sits centred.
        int cell_x = x;
        int px = icon_px();
        x += icon_dx();

        if (rb_is_selected(&sel, i)) {
            // The whole caption block, icon and both label lines, as on
            // Windows and KDE -- a highlight the width of the column.
            // The rect is icon_hl_rect()'s, so that what is PAINTED and
            // what is DAMAGED cannot drift apart.
            int hx, hy, hw, hh;
            icon_hl_rect(cell_x, y, &hx, &hy, &hw, &hh);
            ugfx_fill_rect(wm_surface(), hx, hy, hw, hh, icon_selected_bg);
        }

        // A REAL ICON IF THERE IS ONE, the letter tile if there is not.
        // The picture is composited (it has an alpha channel and a
        // rounded outline), so it sits on the wallpaper rather than in a
        // rectangle of its own -- which is the entire reason icons
        // waited for a codec with alpha.
        // A launcher's own artwork; a file's is the File Manager's
        // "folder"/"file", so the two views of one folder agree.
        const struct uimg *ico = item_is_launcher(i)
            ? icon_get(g_launch[i].icon, px)
            : icon_get(item_is_dir(i) ? "folder" : "file", px);
        if (ico) {
            ugfx_blit_alpha(wm_surface(), x, y, ico->w, ico->h, ico->px, ico->w);
        } else {
            ugfx_fill_rect(wm_surface(), x, y, px, px, ugfx_rgb(60, 90, 130));
            ugfx_draw_rect(wm_surface(), x, y, px, px, icon_fg);

            // The entry's Icon= character, falling back to the name's
            // first letter -- which is what this drew before desktop
            // entries existed, so an entry with no Icon= looks exactly
            // as it did.
            char ic = item_is_launcher(i) ? g_launch[i].glyph : 0;
            char initial[2] = { ic ? ic : item_name(i)[0], '\0' };
            int gx = x + (px - ugfx_text_width(initial)) / 2;
            int gy = y + (px - ugfx_char_h()) / 2;
            ugfx_draw_string(wm_surface(), gx, gy, initial, icon_fg, ugfx_rgb(60, 90, 130));
        }

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
        // SHADOWED, for the same reason the watermark below is: an icon
        // label sits on a wallpaper, and the flat DESKTOP_BG it used to
        // blend its anti-aliasing against stopped being what is behind
        // it. Every desktop shadows or outlines these -- macOS, GNOME
        // and KDE shadow, Windows outlines -- because no single ink is
        // legible on every photograph a person might choose.
        draw_label(cell_x, y + px + 4, item_name(i), label_fg);
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
        // SHADOWED AND TRANSPARENT, because the backdrop is a
        // user-chosen photograph. This was a dim blue on a hardcoded
        // DESKTOP_BG, which was exactly right while the desktop was a
        // flat colour and wrong the day wallpapers arrived: over a
        // light wallpaper the ink had almost no contrast left, and
        // every anti-aliased edge carried a halo of a colour that was
        // no longer anywhere on screen. It read as a rendering fault
        // rather than as a deliberately quiet watermark.
        //
        // Still deliberately quiet -- a light grey rather than white,
        // and the shadow is what carries it over a light wallpaper. It
        // is for the moment you go looking for it; a bright string in
        // the corner of every screenshot would compete with the
        // content.
        uint32_t fg = ugfx_rgb(198, 210, 220);
        int line_h = ugfx_char_h() + 2;
        // Bottom line sits one line above the taskbar; the block grows
        // UPWARDS, so adding a third line later moves nothing.
        int base_y = (screen_h - taskbar_h) - line_h - 8;
        for (int i = 0; i < 2; i++) {
            int w = ugfx_text_width(lines[i]);
            // Each line right-aligned on its own width, so the two stay
            // flush to the same edge whatever the font or the strings.
            ugfx_draw_string_clipped_shadowed(wm_surface(), screen_w - w - 12,
                                              base_y - (1 - i) * line_h,
                                              w, lines[i], fg);
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
    start_store_load();     // an entry that just arrived has history to find
    icon_cache_invalidate(); // an entry's artwork can have arrived with it
    if (!desktop_files_reload()) desktop_files_parse();   // same names: re-read the launchers
    positions_loaded = 0;   // re-read from DESKTOP_CONF_PATH, keyed by name
    rb_clear(&sel);         // indices into a table that just changed
    group_drag = 0;         // its snapshot indexes the table that changed
    band_drawn = 0;
    last_click_index = -1;
    last_click_tick = 0;
}

int desktop_icon_selected(int i) { return rb_is_selected(&sel, i); }

// Which icon (if any) is under (mx, my) -- shared by click and
// right-click handling so they can't disagree about hitboxes.
static int icon_hit_test(int mx, int my) {
    desktop_load_positions();
    struct icon_grid g = current_grid();
    for (int i = 0; i < item_count(); i++) {
        if (!item_visible(i)) continue;
        int x, y, w, h;
        icon_box(&g, i, &x, &y, &w, &h);
        if (uui_hit(x, y, w, h, mx, my)) return i;
    }
    return -1;
}

// EVERY ICON WHOSE HIGHLIGHT CHANGED, damaged by rect. `redraw_pending`
// alone is not enough: it repaints the whole screen only in a QUIET
// frame, and the taskbar clock is damaging one most seconds, so a
// deselected icon kept its highlight painted while the selection said
// otherwise -- which reads as clicking icons ADDING to the selection.
static void damage_selection_change(const uint32_t *before) {
    struct icon_grid g = current_grid();
    for (int i = 0; i < item_count(); i++) {
        int was = (before[i / 32] >> (i % 32)) & 1u;
        if (was == !!rb_is_selected(&sel, i)) continue;
        if (!item_visible(i)) continue;
        int x, y, w, h;
        // THE HIGHLIGHT'S rect, not the hit rect: what changed on
        // screen is what was filled, and icon_box() is strictly inside
        // it -- damaging that left the overhang painted.
        int cx, cy;
        icon_grid_cell_rect(&g, icon_col[i], icon_row[i], &cx, &cy);
        icon_hl_rect(cx, cy, &x, &y, &w, &h);
        wm_damage_rect(x, y, w, h);
    }
}

static void snapshot_selection(uint32_t *out) {
    for (int i = 0; i < (DESKTOP_MAX_ICONS + 31) / 32; i++) out[i] = 0;
    for (int i = 0; i < item_count(); i++)
        if (rb_is_selected(&sel, i)) out[i / 32] |= 1u << (i % 32);
}

void desktop_handle_click(int mx, int my) {
    desktop_load_positions();
    int idx = icon_hit_test(mx, my);
    uint64_t now = sys_ticks();
    uint32_t sel_before[(DESKTOP_MAX_ICONS + 31) / 32];
    snapshot_selection(sel_before);

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
        item_activate(idx);
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
        for (int i = 0; i < item_count(); i++) {
            drag_start_col[i] = icon_col[i];
            drag_start_row[i] = icon_row[i];
        }
    }

    damage_selection_change(sel_before);
    redraw_pending = 1;
}

// Whether (col, row) already belongs to some OTHER icon (not `exclude`,
// the one currently being dropped) -- O(item_count()), fine
// at this scale (a handful of icons).
static int cell_taken(int col, int row, int exclude) {
    for (int i = 0; i < item_count(); i++) {
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
                if (c < 0 || c >= g->cols || r < 0 || r >= rows_that_fit()) continue;
                if (!cell_taken(c, r, exclude)) { *out_col = c; *out_row = r; return; }
            }
        }
    }
    *out_col = col; *out_row = row; // unreachable in practice -- see comment above
}

// AFTER A SIZE CHANGE, the cells that no longer fit above the taskbar
// move to the nearest free cell that does; everything that still fits
// stays put. Taller rows at a bigger size are what pushed a full
// column's tail off the screen (found on the laptop, medium -> large
// with a full column). A moved icon's position is SAVED, so the layout
// is the same after a reboot as it looked when it settled.
static void reflow_overflow(void) {
    if (!positions_loaded) return;   // the default layout already fits
    struct icon_grid g = current_grid();
    int limit = rows_that_fit();
    for (int i = 0; i < item_count(); i++) {
        if (!item_visible(i) || icon_row[i] < limit) continue;
        int col = icon_col[i] >= g.cols ? g.cols - 1 : icon_col[i];
        int fc, fr;
        nearest_free_cell(&g, col, limit - 1, i, &fc, &fr);
        icon_col[i] = fc;
        icon_row[i] = fr;
        save_position(i);
    }
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

// The items the band tests against: every icon on the desktop, at the
// rect the draw and the hit test already agree on. Indices are g_files
// indices, so rb_is_selected(i) needs no second mapping.
static int band_count(void *ctx) {
    (void)ctx;
    return item_count();
}

static void band_item_rect(void *ctx, int i, int *x, int *y, int *w, int *h) {
    (void)ctx;
    struct icon_grid g = current_grid();
    icon_box(&g, i, x, y, w, h);   // icon plus its label, as hit-tested
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
            if (ddx * ddx + ddy * ddy > 9) {
                drag_moved = 1;
                // An icon leaving its cell is also a drag the
                // compositor can offer to a window: its files go in
                // the drag slot now (the selection, if it is in it).
                {
                    static struct uclip c;
                    static char paths[DESKTOP_FILES_MAX][PATH_BUF];
                    int n = 0;
                    if (rb_is_selected(&sel, drag.index)) n = selected_paths(paths, DESKTOP_FILES_MAX);
                    if (n == 0) { item_path(drag.index, paths[0], PATH_BUF); n = 1; }
                    uclip_drag_begin(&c);
                    for (int i = 0; i < n; i++) uclip_add(&c, paths[i]);
                    if (uclip_drag_commit(&c)) wm_dnd_start_desktop(n);
                }
            }
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

    // A WINDOW TOOK THE DROP (wm_dnd.c): the file moved or copied
    // there, and the icon goes back to its cell -- the drag was not a
    // rearrangement. The folder re-lists on the next generation poll.
    if (wm_dnd_took_drop()) {
        wm_damage_rect(0, 0, screen_w, screen_h - taskbar_h);
        icon_drag_end(&drag);
        group_drag = 0;
        redraw_pending = 1;
        return;
    }

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
            for (int i = 0; i < item_count(); i++) {
                if (rb_is_selected(&sel, i)) { icon_col[i] = -1; icon_row[i] = -1; }
            }
            for (int i = 0; i < item_count(); i++) {
                if (!rb_is_selected(&sel, i)) continue;
                if (!item_visible(i)) continue;
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

// --- the desktop's verbs: open, copy, cut, paste, delete, new folder ----
//
// THE DESKTOP DOES NO FILE WORK ITSELF. A copy or a move is `/bin/cp -r`
// or `/bin/mv` spawned with an argv, a delete is `/bin/rm -r`, an open
// is `/bin/open` -- the compositor must not block on a file operation,
// and those programs already exist (docs/conventions/gui.md, "long
// work belongs in a child process"). Only mkdir is a syscall, because
// it is one.

static void spawn_argv(char *const *argv) {
    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.argv = argv;
    o.env = environ;
    int pid = sys_spawn_opts(argv[0], &o);
    if (pid <= 0) wm_logf("desktop: could not run %s", argv[0]);
    else wm_track_launched(pid);
}

// A launcher opens through the registry when the application database
// has its AppId -- single-instance and geometry rules live there -- and
// runs its Exec= directly when it does not (a launcher can outlive its
// entry).
static void launch_entry(const struct gui_app_entry *e) {
    for (int k = 0; k < gui_app_registry_count; k++)
        if (k_strcmp(gui_app_registry[k].app_id, e->app_id) == 0) {
            open_app(&gui_app_registry[k]);
            return;
        }
    char *const argv[] = { (char *)e->exec, 0 };
    spawn_argv(argv);
}

// The Start menu's "Add to desktop": a launcher file in the folder, as
// KDE copies the .desktop into ~/Desktop. Written from the registry's
// fields rather than copied, since the registry keeps no source
// filename; the folder poll picks it up like any new file.
void desktop_add_launcher(const struct gui_app *a) {
    char path[PATH_BUF], text[256], glyph[2] = { a->icon, '\0' };
    k_snprintf(path, sizeof path, DESKTOP_DIR "/%s.desktop", a->app_id);
    int n = k_snprintf(text, sizeof text, "Name=%s\nExec=%s\nIcon=%s\nAppId=%s\n",
                       a->name, a->exec_path, a->icon_name[0] ? a->icon_name : glyph, a->app_id);
    if (n <= 0 || n >= (int)sizeof text) return;
    int fd = sys_open(path, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (fd < 0) { wm_logf("desktop: cannot write %s", path); return; }
    if (sys_write(fd, text, (size_t)n) != n) wm_logf("desktop: short write to %s", path);
    sys_close(fd);
}

static void item_activate(int i) {
    if (!item_visible(i)) return;
    if (item_is_launcher(i)) { launch_entry(&g_launch[i]); return; }
    static char path[PATH_BUF];
    item_path(i, path, sizeof path);
    if (item_is_dir(i)) {
        // A folder opens in the File Manager, both panes there.
        char *const argv[] = { "/bin/wm/apps/files", path, path, 0 };
        spawn_argv(argv);
    } else {
        // A file opens by its association (`/bin/open`, mimeapps.conf).
        char *const argv[] = { "/bin/open", path, 0 };
        spawn_argv(argv);
    }
}

// The selection's paths, for the clipboard and the verbs. A launcher is
// a file here like any other: cut, copied and deleted as one.
static int selected_paths(char paths[][PATH_BUF], int cap) {
    int n = 0;
    for (int i = 0; i < item_count() && n < cap; i++)
        if (rb_is_selected(&sel, i)) item_path(i, paths[n++], PATH_BUF);
    return n;
}

static void clip_put(int op) {
    static struct uclip c;   // 64 KiB: lib/uclip.h says why static
    static char paths[DESKTOP_FILES_MAX][PATH_BUF];
    int n = selected_paths(paths, DESKTOP_FILES_MAX);
    if (n == 0) return;
    uclip_begin(&c, op);
    for (int i = 0; i < n; i++) uclip_add(&c, paths[i]);
    if (!uclip_commit(&c)) wm_logf("desktop: clipboard refused %d item(s)", n);
}
static void menu_copy(void *ctx) { (void)ctx; clip_put(UCLIP_COPY); }
static void menu_cut(void *ctx)  { (void)ctx; clip_put(UCLIP_CUT); }

// Paste's engine, over any loaded payload: the clipboard's (Paste,
// Ctrl+V) or the drag slot's (a drop from another window).
static void paste_from(const struct uclip *c, int op);

static void menu_paste(void *ctx) {
    (void)ctx;
    static struct uclip c;
    uclip_load(&c);
    int op = uclip_op(&c);
    if (op == UCLIP_NONE || uclip_kind(&c) != UCLIP_KIND_FILES) return;
    paste_from(&c, op);
    // A CUT IS SPENT BY THE PASTE, the File Manager's rule (fm_jobs.c).
    if (op == UCLIP_CUT) (void)uclip_clear();
}

// A drop from ANOTHER window onto the desktop background: the files in
// the drag slot move here, or copy with Ctrl (the toolkit's rule).
int desktop_drop_here(int mx, int my) {
    (void)mx; (void)my;
    static struct uclip c;
    uclip_drag_load(&c);
    if (uclip_count(&c) <= 0 || uclip_kind(&c) != UCLIP_KIND_FILES) return 0;
    int copy = (wm_rawin_mods_now() & KEY_MOD_CTRL) != 0;
    paste_from(&c, copy ? UCLIP_COPY : UCLIP_CUT);
    return 1;
}

static void paste_from(const struct uclip *c, int op) {
    static char src[PATH_BUF];
    for (int i = 0; i < uclip_count(c); i++) {
        const char *p = uclip_path(c, i);
        if (!p) break;
        k_strlcpy(src, p, sizeof src);
        // Already on the desktop: nothing to do, and `cp` onto itself
        // would be an error said in a log nobody reads.
        char dir[PATH_BUF];
        k_path_dirname(src, dir, sizeof dir);
        if (k_strcmp(dir, DESKTOP_DIR) == 0) continue;
        if (op == UCLIP_CUT) {
            char *const argv[] = { "/bin/mv", src, DESKTOP_DIR, 0 };
            spawn_argv(argv);
        } else {
            char *const argv[] = { "/bin/cp", "-r", src, DESKTOP_DIR, 0 };
            spawn_argv(argv);
        }
    }
}

// Delete asks first, through the WM's own confirm dialog. The paths are
// snapshotted when the dialog opens: the selection may change under it.
static char g_del_paths[DESKTOP_FILES_MAX][PATH_BUF];
static int g_del_count;
static char g_del_msg[64];
static void delete_confirmed(void) {
    static char *argv[DESKTOP_FILES_MAX + 3];
    int k = 0;
    argv[k++] = "/bin/rm";
    argv[k++] = "-r";
    for (int i = 0; i < g_del_count; i++) argv[k++] = g_del_paths[i];
    argv[k] = 0;
    spawn_argv(argv);
    g_del_count = 0;
}
static void menu_delete(void *ctx) {
    (void)ctx;
    g_del_count = selected_paths(g_del_paths, DESKTOP_FILES_MAX);
    if (g_del_count == 0) return;
    if (g_del_count == 1)
        k_snprintf(g_del_msg, sizeof g_del_msg, "Delete %s?", k_path_basename(g_del_paths[0]));
    else
        k_snprintf(g_del_msg, sizeof g_del_msg, "Delete %d items from the desktop?", g_del_count);
    confirm_dialog_open_labelled(g_del_msg, "Delete", "Cancel", delete_confirmed, 0);
}

// "New folder", then "New folder 2", ... -- Explorer's and Dolphin's
// naming. A syscall rather than a child: mkdir is one operation.
static void menu_new_folder(void *ctx) {
    (void)ctx;
    char path[PATH_BUF];
    for (int n = 1; n < 100; n++) {
        if (n == 1) k_snprintf(path, sizeof path, DESKTOP_DIR "/New folder");
        else        k_snprintf(path, sizeof path, DESKTOP_DIR "/New folder %d", n);
        struct sys_stat st;
        if (sys_stat(path, &st) == 0) continue;
        if (sys_mkdir(path) < 0) wm_logf("desktop: mkdir %s failed", path);
        return;
    }
}

static void menu_open_item(void *ctx) { item_activate((int)(intptr_t)ctx); }

// The keyboard's half: Ctrl+C / Ctrl+X / Ctrl+V and Delete when no
// window has the focus. Returns 1 if the key was the desktop's.
int desktop_handle_key(int key, unsigned mods) {
    if (key == KEY_DELETE) { menu_delete(0); return 1; }
    if (!(mods & KEY_MOD_CTRL)) return 0;
    if (key == 'c' || key == 'C' || key == 0x03) { menu_copy(0);  return 1; }
    if (key == 'x' || key == 'X' || key == 0x18) { menu_cut(0);   return 1; }
    if (key == 'v' || key == 'V' || key == 0x16) { menu_paste(0); return 1; }
    return 0;
}

// --- the desktop's context menu ---------------------------------------
//
// Windows' and KDE's shape: Open > (the launchers), then the desktop's
// own verbs, then Icon size > with a tick on the current one, then the
// way to the settings page. Static rows because context_menu_open_at()
// only borrows the pointer for as long as the menu stays open.
//
// OVER AN ICON the menu is that icon's -- Open, Cut, Copy, Delete,
// a launcher being a file too -- and the click SELECTS it first (the rule every file
// manager has, docs/conventions/gui.md), so the verbs act on what was
// pointed at. That is the per-icon identity desktop.h's top comment
// once said was missing: a file has one, a launcher's is "open it".

static void launch_from_menu(void *ctx) {
    open_app((const struct gui_app *)ctx);
}

static void set_icon_size(void *ctx) {
    const char *word = (const char *)ctx;
    if (usetting_set("desktop.icon_size", word) == SETTING_INVALID) {
        wm_logf("desktop: icon size %s refused", word);
        return;
    }
    icon_size_reload();   // now, not on the next generation poll
}

// Refresh: re-read the entries and the folder, drop the icon cache,
// repaint. What F5 does on every desktop.
static void menu_refresh(void *ctx) {
    (void)ctx;
    desktop_entries_changed();
    wm_damage_rect(0, 0, screen_w, screen_h);
    redraw_pending = 1;
}

// Sort by name: the default layout, re-derived and SAVED, so a desktop
// rearranged by hand goes back to columns in name order.
static void menu_sort(void *ctx) {
    (void)ctx;
    char keybuf[80];
    for (int i = 0; i < item_count(); i++)
        wm_conf_set(DESKTOP_CONF_PATH, item_key(i, keybuf, sizeof keybuf), "");
    positions_loaded = 0;
    desktop_load_positions();
    for (int i = 0; i < item_count(); i++)
        if (icon_col[i] >= 0) save_position(i);
    wm_damage_rect(0, 0, screen_w, screen_h);
    redraw_pending = 1;
}

// Desktop settings: System Settings, found by its app id so a renamed
// entry still opens it. Both Windows ("Personalize") and KDE
// ("Configure Desktop and Wallpaper") end their menu this way.
static void menu_settings(void *ctx) {
    (void)ctx;
    for (int i = 0; i < gui_app_registry_count; i++) {
        const struct gui_app *app = &gui_app_registry[i];
        if (app->app_id && k_strcmp(app->app_id, "settings") == 0) { open_app(app); return; }
    }
    wm_logf("desktop: no System Settings entry to open");
}

void desktop_handle_right_click(int mx, int my) {
    static struct context_menu_item launchers[16];
    static struct context_menu_item sizes[3];
    static struct context_menu_item items[12];
    int k = 0;

    int idx = icon_hit_test(mx, my);
    if (idx >= 0) {
        // The click selects the icon first, unless it is already in the
        // set -- right-clicking one of five must offer to act on five.
        if (!rb_is_selected(&sel, idx)) { rb_clear(&sel); rb_select(&sel, idx, 1); }
        redraw_pending = 1;
        items[k++] = (struct context_menu_item){ .label = "Open", .on_select = menu_open_item,
                                                 .ctx = (void *)(intptr_t)idx };
        items[k++] = (struct context_menu_item){ .separator = 1 };
        items[k++] = (struct context_menu_item){ .label = "Cut", .on_select = menu_cut };
        items[k++] = (struct context_menu_item){ .label = "Copy", .on_select = menu_copy };
        items[k++] = (struct context_menu_item){ .separator = 1 };
        items[k++] = (struct context_menu_item){ .label = "Delete", .on_select = menu_delete };
        context_menu_open_at(mx, my, items, k);
        return;
    }

    // Open > -- the desktop's own entries, so an entry hidden from this
    // surface by ShowIn= is not launchable from here either.
    int n = gui_app_visible_count(GUI_SHOW_DESKTOP);
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) {
        struct gui_app *app = gui_app_visible_at(GUI_SHOW_DESKTOP, i);
        launchers[i] = (struct context_menu_item){ .label = app->name,
                                                   .on_select = launch_from_menu,
                                                   .ctx = (void *)app };
    }
    static const char *const words[3] = { "small", "medium", "large" };
    static const char *const labels[3] = { "Small", "Medium", "Large" };
    for (int i = 0; i < 3; i++) {
        sizes[i] = (struct context_menu_item){ .label = labels[i],
                                               .on_select = set_icon_size,
                                               .ctx = (void *)words[i],
                                               .checked = k_strcmp(g_icon_size, words[i]) == 0 };
    }

    items[k++] = (struct context_menu_item){ .label = "Open", .sub = launchers, .sub_count = n };
    items[k++] = (struct context_menu_item){ .separator = 1 };
    items[k++] = (struct context_menu_item){ .label = "New folder", .on_select = menu_new_folder };
    items[k++] = (struct context_menu_item){ .label = "Paste", .on_select = menu_paste };
    items[k++] = (struct context_menu_item){ .separator = 1 };
    items[k++] = (struct context_menu_item){ .label = "Refresh", .on_select = menu_refresh };
    items[k++] = (struct context_menu_item){ .label = "Sort by name", .on_select = menu_sort };
    items[k++] = (struct context_menu_item){ .label = "Icon size", .sub = sizes, .sub_count = 3 };
    items[k++] = (struct context_menu_item){ .separator = 1 };
    items[k++] = (struct context_menu_item){ .label = "Desktop settings", .on_select = menu_settings };
    context_menu_open_at(mx, my, items, k);
}
