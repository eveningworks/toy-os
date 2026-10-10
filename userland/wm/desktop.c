// See desktop.h for the design writeup.
#include "wm/wm_watch.h" // desktop_poll_config()'s change counter
#include "desktop.h"
#include "lib/usetting.h" // the MERGED registry: these settings are declared, not registered
#include "start_store.h"
#include "wm_internal.h"
#include "context_menu.h"
#include "ui/uui.h"
#include "ui/utheme.h"
#include "lib/rubberband.h"
#include "build_date.h" // GENERATED, and included ONLY here -- see gen_version.sh
#include "kapi.h"
#include "rt/sys.h"
#include "wm/wm_rawin.h"
#include "lib/icon_grid.h"
#include <stdio.h>
#include "wm/wm_conf.h"
#include "lib/icon_cache.h"
#include "lib/ufiletype.h"  // a file's icon, by its type
#include "lib/uopen.h"      // what a launcher opens, for a file dropped on it
#include "lib/uthumb.h"     // thumbnails: made by /bin/thumb, only READ here
#include "caltime.h"         // cal_rtc_to_epoch(), for a file's age
#include "wm/wm_log.h"
#include "lib/uimg.h"
#include "lib/ulivewall.h" // ulivewall_colour() -- the plain colours
#include "wm_background.h"   // a live frame, drawn where the picture would be
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
// The icon under the pointer, or -1 -- a glass wash fainter than the
// selection's (Windows 11's and Plasma's desktop hover). Reset on reload:
// it is a registry index, like the selection.
static int g_hover = -1;
static int g_rename = -1;   // the icon whose caption is being edited, or -1 (rename in place, below)
static void draw_rename(void);
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
// (lib/rubberband.h) rather than by this file. That is what lets a
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

// A drag resting ON an icon (desktop_drop_hover()): which one, what a
// release there does, and what the drag's label says meanwhile.
enum { DROP_NONE, DROP_MOVE, DROP_COPY, DROP_OPEN, DROP_CANT, DROP_BIN };
static int g_drop_on = -1;
static int g_drop_verb = DROP_NONE;
static char g_drop_label[96];
static int item_is_bin(int i);
static int drop_desktop_drag(void);
static int g_bin_full;          // the Recycle Bin's picture: anything in it
static void damage_icon_hl(int i);

// THUMBNAILS, by item: a picture's or a video's own, and up to two of
// the pictures inside a folder. Loaded on the poll, never while drawing,
// and ONLY from the cache: /bin/thumb makes them (lib/uthumb.h says why
// the compositor never decodes someone else's file).
#define DESKTOP_PEEKS 2
static struct uimg g_thumb[DESKTOP_FILES_MAX];
static struct uimg g_peek[DESKTOP_FILES_MAX][DESKTOP_PEEKS];
static uint8_t g_thumb_asked[DESKTOP_FILES_MAX];   // /bin/thumb already asked about it
// When each item last changed: a file still being copied grows a block at
// a time, and asking at every step ran /bin/thumb once per block. An item
// is asked about only once it has kept still for THUMB_SETTLE_TICKS.
static uint64_t g_thumb_changed[DESKTOP_FILES_MAX];
static uint64_t g_thumb_due;     // a refresh owed then, for what was too fresh; 0 none
#define THUMB_SETTLE_TICKS 100   // a second
static uint32_t g_files_epoch;   // moves when the list or the icon size does
static void thumbs_reset(void);
static void thumbs_refresh(void);
static void thumb_forget(int i);
static void spawn_argv(char *const *argv);

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
// Re-reads every launcher file: when the set of names changed, when a
// launcher's size or mtime did (desktop_files_reload), and on Refresh.
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
    if (!changed) {
        // SAME NAMES, EDITED LAUNCHER: re-parse, but keep the positions.
        // A launcher written in pieces (`write`, then `append`) is listed
        // after its first line, with no Name= yet, and only this notices
        // the rest arrive -- KDE's desktop re-reads a modified file too.
        int edited = 0;
        for (int i = 0; i < n; i++)
            if (is_desktop_file(fresh[i].name) &&
                (fresh[i].size != g_files[i].size ||
                 k_memcmp(&fresh[i].modified, &g_files[i].modified, sizeof fresh[i].modified) != 0)) {
                g_files[i] = fresh[i];
                edited = 1;
            }
        if (edited) {
            desktop_files_parse();
            redraw_pending = 1;
            wm_damage_rect(0, 0, screen_w, screen_h);
        }
        // A picture that finished arriving, or a folder that gained one,
        // is asked about again: the first ask may have met a half-written
        // file, and a folder's contents have no watch of their own.
        for (int i = 0; i < n; i++)
            if (!is_desktop_file(fresh[i].name) &&
                (fresh[i].size != g_files[i].size ||
                 k_memcmp(&fresh[i].modified, &g_files[i].modified, sizeof fresh[i].modified) != 0)) {
                g_files[i] = fresh[i];
                thumb_forget(i);
            }
        return 0;
    }
    thumbs_reset();   // indexed by the list that is about to change
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
static uint32_t g_background_gen;   // desktop_background_gen()
// `desktop.wallpaper_type` is colour: the picture stays loaded (a switch
// back costs no decode) but is not drawn.
static int g_plain_only;
static uint32_t g_plain_rgb = 0x183c5a;   // `desktop.background_colour`

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

    // THE TYPE AND THE PLAIN COLOUR. "none" is what a plain background
    // was before the type existed, and still reads as one.
    char type[16] = "picture";
    if (usetting_get("desktop.wallpaper_type", v, sizeof v) && v[0])
        k_strlcpy(type, v, sizeof type);
    uint32_t plain = ulivewall_colour(
        usetting_get("desktop.background_colour", v, sizeof v) ? v : "blue");
    int plain_only = k_strcmp(type, "colour") == 0 || k_strcmp(name, "none") == 0;
    if (plain != g_plain_rgb || plain_only != g_plain_only) {
        g_plain_rgb = plain;
        g_plain_only = plain_only;
        desktop_background_changed();
    }

    int name_changed = k_strcmp(name, wallpaper_name) != 0;
    int mode_changed = k_strcmp(mode, wallpaper_mode) != 0;
    if (!name_changed && !mode_changed) return;

    k_strlcpy(wallpaper_name, name, sizeof wallpaper_name);
    k_strlcpy(wallpaper_mode, mode, sizeof wallpaper_mode);
    g_background_gen++;

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
    // THE FIRST FORMAT THAT LOADS: the value is the stem, as a ChoiceDir
    // in stem mode lists it, so the file may be any of these.
    static const char *const EXT[] = { "jpg", "gif", "png", "qoi" };
    char path[80];
    uint64_t t0 = sys_ticks();
    int rc = -1;
    for (unsigned e = 0; e < sizeof EXT / sizeof EXT[0] && rc < 0; e++) {
        k_snprintf(path, sizeof path, "%s/%s.%s", WALLPAPER_DIR, name, EXT[e]);
        rc = uimg_load(path, &wallpaper_src);   // a GIF's first frame: a picture is a still
    }
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
    thumbs_reset();   // made at the old size
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
    uint64_t gen = wm_watch_config_gen() + wm_watch_gen(WM_TOPIC_DESKTOP);
    if (!primed) {
        primed = 1;
        seen_gen = gen;
        icon_size_reload();
        wallpaper_reload();      // the first call is the initial load
        if (desktop_files_reload()) positions_loaded = 0;
        return;
    }
    // Thumbnails: when the list, the icon size or the cache changes.
    static uint64_t thumbs_gen;
    if (g_thumb_due && sys_ticks() >= g_thumb_due) { g_thumb_due = 0; g_files_epoch++; }
    uint64_t tg = wm_watch_gen(WM_TOPIC_THUMBS) + g_files_epoch;
    if (tg != thumbs_gen && !desktop_drag_active()) {
        thumbs_gen = tg;
        thumbs_refresh();
    }
    // The Recycle Bin's picture: is anything in it? One entry answers.
    static uint64_t bin_gen;
    uint64_t bg = wm_watch_gen(WM_TOPIC_TRASH);
    if (bg != bin_gen) {
        static struct sys_dirent one[1];
        bin_gen = bg;
        int full = sys_listdir("/home/.Trash/files", one, 1) > 0;
        if (full != g_bin_full) {
            g_bin_full = full;
            for (int i = 0; i < item_count(); i++) if (item_is_bin(i)) damage_icon_hl(i);
            redraw_pending = 1;
        }
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
    const uint32_t *live = wm_bg_frame();
    if (live) {
        ugfx_blit(wm_surface(), 0, 0, screen_w, screen_h, live, screen_w);
        return;
    }
    if (wallpaper_loaded && !g_plain_only) {
        wallpaper_view.x = 0;
        wallpaper_view.y = 0;
        wallpaper_view.w = screen_w;
        wallpaper_view.h = screen_h;
        uui_image_draw(wm_surface(), &wallpaper_view);
        return;
    }
    ugfx_fill(wm_surface(), g_plain_rgb);
}

void desktop_draw_background_into(struct ugfx_surface *dst) {
    g_wm_surface_override = dst;
    ugfx_clear_clip_rect(dst);
    draw_background();
    g_wm_surface_override = 0;
}

uint32_t desktop_background_gen(void) { return g_background_gen; }

void desktop_background_changed(void) {
    g_background_gen++;
    redraw_pending = 1;
    wm_damage_rect(0, 0, screen_w, screen_h);
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

// The artwork an icon is drawn with -- one answer for the draw loop and
// for `gui icons`, so a test reads what is painted.
const char *desktop_icon_art(int i) {
    if (!item_visible(i)) return "";
    if (item_is_bin(i)) return g_bin_full ? "trash-full" : "trash-empty";
    if (item_is_launcher(i)) return g_launch[i].icon;
    return ufiletype_icon(item_name(i), item_is_dir(i));
}

int desktop_drop_target(void) { return g_drop_on; }

int desktop_icon_preview(int i, int *peeks) {
    int n = 0;
    for (int k = 0; item_visible(i) && k < DESKTOP_PEEKS; k++) n += g_peek[i][k].px != 0;
    if (peeks) *peeks = n;
    return item_visible(i) && g_thumb[i].px != 0;
}

// --- thumbnails ------------------------------------------------------------

static int has_thumb_type(const char *name) {
    const char *t = ufiletype_icon(name, 0);
    return k_strcmp(t, "file-image") == 0 || k_strcmp(t, "file-video") == 0;
}

static void thumb_forget(int i) {
    uimg_free(&g_thumb[i]);
    for (int k = 0; k < DESKTOP_PEEKS; k++) uimg_free(&g_peek[i][k]);
    g_thumb_asked[i] = 0;
    g_thumb_changed[i] = sys_ticks();
    g_files_epoch++;
}

// Written in the last two seconds? Then a thumbnail made now could carry
// the SAME second as its source, and the cache's strictly-newer rule
// (lib/uthumb.c) would refuse it for good -- so it waits.
static int too_fresh(const struct rtc_time *m) {
    struct rtc_time now;
    if (sys_gettime(&now) != 0) return 0;
    return cal_rtc_to_epoch(&now) < cal_rtc_to_epoch(m) + 2;
}

static void thumbs_reset(void) {
    for (int i = 0; i < DESKTOP_FILES_MAX; i++) {
        uimg_free(&g_thumb[i]);
        for (int k = 0; k < DESKTOP_PEEKS; k++) uimg_free(&g_peek[i][k]);
        g_thumb_asked[i] = 0;
        g_thumb_changed[i] = sys_ticks();   // a new list may hold a file still arriving
    }
    g_files_epoch++;
}

// Every missing thumbnail the cache has now is loaded; the rest are asked
// of /bin/thumb ONCE per item and list, in one run -- its writes move the
// cache's watch, which brings this round again to load them.
static void thumbs_refresh(void) {
    static char *argv[DESKTOP_FILES_MAX * DESKTOP_PEEKS + 6];
    static char paths[2][DESKTOP_FILES_MAX * DESKTOP_PEEKS][PATH_BUF];   // per size
    static char size[2][8];
    int px = icon_px(), peek_px = icon_px() * 9 / 20;
    k_snprintf(size[0], sizeof size[0], "%d", px);
    k_snprintf(size[1], sizeof size[1], "%d", peek_px);
    int want[2] = { 0, 0 };   // paths waiting, at each size
    const int cap = DESKTOP_FILES_MAX * DESKTOP_PEEKS;
    uint64_t now = sys_ticks();
    g_thumb_due = 0;
    for (int i = 0; i < g_file_count && i < DESKTOP_FILES_MAX; i++) {
        if (item_is_launcher(i)) continue;
        // Still changing: loaded if the cache has it, asked about later.
        int settled = now - g_thumb_changed[i] >= THUMB_SETTLE_TICKS &&
                      (item_is_dir(i) || !too_fresh(&g_files[i].modified));
        if (!settled && !g_thumb_asked[i]) g_thumb_due = now + THUMB_SETTLE_TICKS;
        int ask = settled && !g_thumb_asked[i];
        char path[PATH_BUF];
        item_path(i, path, sizeof path);
        if (!item_is_dir(i)) {
            if (g_thumb[i].px || !has_thumb_type(g_files[i].name)) continue;
            if (uthumb_load_cached(path, &g_files[i].modified, px, &g_thumb[i]) == 0) {
                damage_icon_hl(i);
                redraw_pending = 1;
            } else if (ask && want[0] < cap) {
                k_strlcpy(paths[0][want[0]++], path, PATH_BUF);
            }
            if (ask) g_thumb_asked[i] = 1;
            continue;
        }
        // A folder: its first pictures, by name, from one short listing.
        if (g_peek[i][DESKTOP_PEEKS - 1].px) continue;
        static struct sys_dirent kids[32];
        int m = sys_listdir(path, kids, 32), got = 0, waiting = 0;
        for (int k = 0; k < m && got < DESKTOP_PEEKS; k++) {
            if (kids[k].is_dir || !has_thumb_type(kids[k].name)) continue;
            char kid[PATH_BUF];
            if (!k_path_join(path, kids[k].name, kid, sizeof kid)) continue;
            if (g_peek[i][got].px || uthumb_load_cached(kid, &kids[k].modified, peek_px, &g_peek[i][got]) == 0) {
                damage_icon_hl(i);
                redraw_pending = 1;
            } else if (too_fresh(&kids[k].modified)) {
                waiting = 1;
            } else if (ask && want[1] < cap) {
                k_strlcpy(paths[1][want[1]++], kid, PATH_BUF);
            }
            got++;
        }
        // A folder's contents have no watch here: a picture still too new
        // to ask about is looked at again shortly.
        if (waiting) g_thumb_due = now + THUMB_SETTLE_TICKS;
        else if (ask) g_thumb_asked[i] = 1;
    }
    // One run per size: the item thumbnails, then the peeks.
    // At most THUMB_RUN files a run, so a full desktop stays far inside
    // what a spawn's argument block holds.
    enum { THUMB_RUN = 32 };
    for (int s = 0; s < 2; s++) {
        for (int at = 0; at < want[s]; at += THUMB_RUN) {
            int k = 0;
            argv[k++] = "/bin/thumb";
            argv[k++] = "-q";
            argv[k++] = "-s";
            argv[k++] = size[s];
            for (int j = at; j < want[s] && j < at + THUMB_RUN; j++) argv[k++] = paths[s][j];
            argv[k] = 0;
            spawn_argv(argv);
        }
        if (want[s]) wm_logf("desktop: asked /bin/thumb for %d thumbnail(s) at %s px", want[s], size[s]);
    }
}

int desktop_icon_px(void) { return icon_px(); }

// A thumbnail in a thin white frame, centred in the icon's square -- a
// photograph laid on the desktop, Windows' and GNOME's look. A video
// carries a play mark.
static void draw_thumb(const struct uimg *t, int x, int y, int px, int video) {
    int tx = x + (px - t->w) / 2, ty = y + (px - t->h) / 2;
    ugfx_fill_rect(wm_surface(), tx - 2, ty - 1, t->w + 4, t->h + 4, ugfx_rgb(0, 0, 0));
    ugfx_fill_rect(wm_surface(), tx - 2, ty - 2, t->w + 4, t->h + 4, UTHEME_WHITE);
    ugfx_blit_alpha(wm_surface(), tx, ty, t->w, t->h, t->px, t->w);
    if (video) {
        int r = px / 7, cx = tx + t->w / 2, cy = ty + t->h / 2;
        ugfx_fill_circle(wm_surface(), cx, cy, r, ugfx_rgb(20, 22, 28));
        int pts_x[3] = { cx - r / 3, cx - r / 3, cx + r / 2 };
        int pts_y[3] = { cy - r / 2, cy + r / 2, cy };
        ugfx_fill_polygon(wm_surface(), pts_x, pts_y, 3, UTHEME_WHITE);
    }
}

// A folder's peek: its first pictures, small and framed, laid across its
// front -- the folder still reads as a folder, and says what it holds.
static void draw_peeks(int i, int x, int y, int px) {
    for (int k = 0; k < DESKTOP_PEEKS; k++) {
        const struct uimg *t = &g_peek[i][k];
        if (!t->px) continue;
        int tx = x + px / 2 - t->w / 2 + (k ? px / 6 : -px / 8);
        int ty = y + px / 2 - t->h / 3 + (k ? px / 12 : 0);
        ugfx_fill_rect(wm_surface(), tx - 1, ty - 1, t->w + 2, t->h + 2, UTHEME_WHITE);
        ugfx_blit_alpha(wm_surface(), tx, ty, t->w, t->h, t->px, t->w);
    }
}

void desktop_draw(void) {
    desktop_load_positions();
    struct icon_grid g = current_grid();

    draw_background();

    uint32_t label_fg = UTHEME_WHITE;
    uint32_t icon_fg = ugfx_rgb(230, 230, 235);

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

        int selected = rb_is_selected(&sel, i);
        int hovered = i == g_hover && !drag.active;
        int target = i == g_drop_on;   // a drag would land on it
        if (selected || hovered || target) {
            // The whole caption block, icon and both label lines, as on
            // Windows and KDE -- a highlight the width of the column.
            // The rect is icon_hl_rect()'s, so that what is PAINTED and
            // what is DAMAGED cannot drift apart. GLASS, Windows 11's:
            // white laid over the wallpaper, a rounded edge when
            // selected, so the picture still shows through.
            int hx, hy, hw, hh;
            icon_hl_rect(cell_x, y, &hx, &hy, &hw, &hh);
            uint8_t fill = target ? 80 : selected ? (hovered ? 72 : 56) : 28;
            uint8_t edge = target ? 200 : selected ? (hovered ? 160 : 140) : 28;
            uui_glass_round_rect(wm_surface(), hx, hy, hw, hh, ugfx_char_h() / 2,
                                 UTHEME_WHITE, fill, edge);
        }

        // A REAL ICON IF THERE IS ONE, the letter tile if there is not.
        // The picture is composited (it has an alpha channel and a
        // rounded outline), so it sits on the wallpaper rather than in a
        // rectangle of its own -- which is the entire reason icons
        // waited for a codec with alpha.
        // A launcher's own artwork; a file's is the File Manager's for
        // its type (ufiletype.h), so the two views of one folder agree.
        const struct uimg *ico = icon_get(desktop_icon_art(i), px);
        if (g_thumb[i].px) {
            draw_thumb(&g_thumb[i], x, y, px, k_strcmp(desktop_icon_art(i), "file-video") == 0);
        } else if (ico) {
            ugfx_blit_alpha(wm_surface(), x, y, ico->w, ico->h, ico->px, ico->w);
            draw_peeks(i, x, y, px);
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
        if (i != g_rename) draw_label(cell_x, y + px + 4, item_name(i), label_fg);
    }
    draw_rename();   // over the icons, under the band

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
    // Glass, as the selection is: a faint fill the icons show through
    // and a firmer edge -- Windows' and KDE's band.
    int bx, by, bw, bh;
    if (rb_rect(&sel, &bx, &by, &bw, &bh)) {
        uui_glass_round_rect(wm_surface(), bx, by, bw, bh, 0, UTHEME_WHITE, 40, 200);
    }
}

// A band counts as a drag for the reload guard's purposes: its selection
// is a set of REGISTRY indices, so a reload underneath one would leave
// the user having selected different icons than the ones they swept.
int desktop_drag_active(void) { return drag.active || sel.armed; }

void desktop_entries_changed(void) {
    g_hover = -1;
    start_store_load();     // an entry that just arrived has history to find
    icon_cache_invalidate(); // an entry's artwork can have arrived with it
    if (!desktop_files_reload()) desktop_files_parse();   // same names: re-read the launchers
    positions_loaded = 0;   // re-read from DESKTOP_CONF_PATH, keyed by name
    rb_clear(&sel);         // indices into a table that just changed
    group_drag = 0;         // its snapshot indexes the table that changed
    band_drawn = 0;
    last_click_index = -1;
    last_click_tick = 0;
    // EVERY CELL MAY HAVE CHANGED, so all of them are damage. Leaving it
    // to "a frame with no damage is a full repaint" lost whenever the
    // clock damaged its strip in the same frame: new icons went undrawn
    // and old ones stayed where they had been.
    wm_damage_rect(0, 0, screen_w, screen_h - taskbar_h);
    redraw_pending = 1;
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

static void damage_icon_hl(int i) {
    if (i < 0 || i >= item_count() || !item_visible(i)) return;
    struct icon_grid g = current_grid();
    int cx, cy, x, y, w, h;
    icon_grid_cell_rect(&g, icon_col[i], icon_row[i], &cx, &cy);
    icon_hl_rect(cx, cy, &x, &y, &w, &h);
    wm_damage_rect(x, y, w, h);
}

void desktop_update_hover(int mx, int my, int on_desktop) {
    int now = (on_desktop && !drag.active && !sel.armed) ? icon_hit_test(mx, my) : -1;
    if (now == g_hover) return;
    damage_icon_hl(g_hover);
    damage_icon_hl(now);
    g_hover = now;
    redraw_pending = 1;
}

int desktop_hovered_icon(void) { return g_hover; }

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
    uint8_t mods = wm_rawin_pointer_mods();
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

    // A WINDOW TOOK THE DROP (wm_dnd.c), or an ICON did (a folder, an
    // app, the Recycle Bin): the icon goes back to its cell -- the drag
    // was not a rearrangement. The folder re-lists on its next poll.
    if (wm_dnd_took_drop() || (drag_moved && drop_desktop_drag())) {
        wm_damage_rect(0, 0, screen_w, screen_h - taskbar_h);
        icon_drag_end(&drag);
        group_drag = 0;
        redraw_pending = 1;
        return;
    }
    desktop_drop_hover(-1, -1);   // a click, or a drop that was nobody's

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
// or `/bin/mv` spawned with an argv, a delete is `/bin/trash put` (to the
// Recycle Bin; Shift+Delete is `/bin/rm -r`, for good), an open
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
    if (item_is_bin(i)) {
        // The bin is a place, not a program: the File Manager, open on it.
        char *const argv[] = { "/bin/wm/apps/files", "trash:/", "trash:/", 0 };
        spawn_argv(argv);
        return;
    }
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
static int drop_onto_target(char paths[][PATH_BUF], int n);

int desktop_drop_here(int mx, int my) {
    (void)mx; (void)my;
    static struct uclip c;
    uclip_drag_load(&c);
    if (uclip_count(&c) <= 0 || uclip_kind(&c) != UCLIP_KIND_FILES) return 0;
    if (g_drop_on >= 0) {
        static char paths[DESKTOP_FILES_MAX][PATH_BUF];
        int n = 0;
        for (int i = 0; i < uclip_count(&c) && n < DESKTOP_FILES_MAX; i++)
            if (uclip_path(&c, i)) k_strlcpy(paths[n++], uclip_path(&c, i), PATH_BUF);
        return drop_onto_target(paths, n);
    }
    int copy = (wm_rawin_pointer_mods() & KEY_MOD_CTRL) != 0;
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

// --- dropping ONTO an icon ------------------------------------------------
//
// A folder takes the files (Ctrl copies), an app opens them, the Recycle
// Bin deletes them -- Explorer's targets and GNOME's (chosen 2026-10-10).
// Every drag that crosses the desktop is asked here, its own icons' and a
// window's files alike, so the two cannot answer differently. NOTHING
// HERE READS THE DISK: an app's Handles= is the registry's, read with
// the entries, and the drag's first path is what decides what it opens.

static int item_is_bin(int i) {
    return item_is_launcher(i) && k_strcmp(g_launch[i].app_id, "trash") == 0;
}

static const struct gui_app *launcher_app(int i) {
    for (int k = 0; k < gui_app_registry_count; k++)
        if (k_strcmp(gui_app_registry[k].app_id, g_launch[i].app_id) == 0) return &gui_app_registry[k];
    return 0;
}

// Is `i` being dragged itself? It is never its own target.
static int being_dragged(int i) {
    if (!drag.active) return 0;
    return i == drag.index || (rb_is_selected(&sel, drag.index) && rb_is_selected(&sel, i));
}

// The paths a drag carries: the desktop's own (the selection, or the one
// icon), else the drag slot a window filled.
static int drag_paths(char paths[][PATH_BUF], int cap) {
    if (drag.active) {
        int n = rb_is_selected(&sel, drag.index) ? selected_paths(paths, cap) : 0;
        if (n == 0 && cap > 0) { item_path(drag.index, paths[0], PATH_BUF); n = 1; }
        return n;
    }
    static struct uclip c;
    uclip_drag_load(&c);
    int n = 0;
    for (int i = 0; i < uclip_count(&c) && n < cap; i++)
        if (uclip_path(&c, i)) k_strlcpy(paths[n++], uclip_path(&c, i), PATH_BUF);
    return n;
}

static const char *ext_of(const char *path) {
    const char *base = k_path_basename(path), *dot = 0;
    for (const char *p = base; *p; p++) if (*p == '.') dot = p;
    return dot && dot != base ? dot : 0;
}

static int verb_for(int t, const char *first) {
    if (item_is_bin(t)) return DROP_BIN;
    if (item_is_launcher(t)) {
        const struct gui_app *a = launcher_app(t);
        const char *ext = ext_of(first);
        return a && a->handles && ext && uopen_ext_matches(a->handles, ext) ? DROP_OPEN : DROP_CANT;
    }
    if (item_is_dir(t)) return (wm_rawin_pointer_mods() & KEY_MOD_CTRL) ? DROP_COPY : DROP_MOVE;
    return DROP_NONE;
}

void desktop_drop_hover(int mx, int my) {
    static char first[PATH_BUF];
    int t = mx < 0 ? -1 : icon_hit_test(mx, my);
    if (t >= 0 && being_dragged(t)) t = -1;
    // The payload's first path is read once per icon entered, not per move.
    if (t >= 0 && t != g_drop_on) {
        static char one[1][PATH_BUF];
        if (drag_paths(one, 1) == 1) k_strlcpy(first, one[0], sizeof first);
        else first[0] = '\0';
    }
    int verb = t >= 0 ? verb_for(t, first) : DROP_NONE;
    if (verb == DROP_NONE) t = -1;
    if (t == g_drop_on && verb == g_drop_verb) return;
    damage_icon_hl(g_drop_on);
    damage_icon_hl(t);
    g_drop_on = t;
    g_drop_verb = verb;
    const char *name = t >= 0 ? item_name(t) : "";
    const char *ext = ext_of(first);
    switch (verb) {
    case DROP_MOVE: k_snprintf(g_drop_label, sizeof g_drop_label, "Move to %s", name); break;
    case DROP_COPY: k_snprintf(g_drop_label, sizeof g_drop_label, "Copy to %s", name); break;
    case DROP_OPEN: k_snprintf(g_drop_label, sizeof g_drop_label, "Open with %s", name); break;
    case DROP_BIN:  k_snprintf(g_drop_label, sizeof g_drop_label, "Move to %s", name); break;
    case DROP_CANT:
        if (ext) k_snprintf(g_drop_label, sizeof g_drop_label, "%s can't open %s files", name, ext);
        else k_snprintf(g_drop_label, sizeof g_drop_label, "%s can't open this", name);
        break;
    default: g_drop_label[0] = '\0'; break;
    }
    redraw_pending = 1;
}

const char *desktop_drop_label(void) { return g_drop_on >= 0 ? g_drop_label : 0; }

// A release over the target: do what its label said, then forget it.
// Returns 1 when the drop was the target's -- a refusal (CANT) included,
// so the icons go back to their cells rather than being rearranged.
static int drop_onto_target(char paths[][PATH_BUF], int n) {
    int t = g_drop_on, verb = g_drop_verb;
    if (t < 0 || verb == DROP_NONE) return 0;
    damage_icon_hl(t);
    g_drop_on = -1;
    g_drop_verb = DROP_NONE;
    redraw_pending = 1;
    static char dest[PATH_BUF];
    if (verb == DROP_MOVE || verb == DROP_COPY) {
        item_path(t, dest, sizeof dest);
        size_t dl = k_strlen(dest);
        for (int i = 0; i < n; i++) {
            // Not into itself, nor into a folder inside itself.
            size_t pl = k_strlen(paths[i]);
            if (k_strcmp(paths[i], dest) == 0) continue;
            if (dl > pl && k_strncmp(dest, paths[i], pl) == 0 && dest[pl] == '/') continue;
            char *const mv[] = { "/bin/mv", paths[i], dest, 0 };
            char *const cp[] = { "/bin/cp", "-r", paths[i], dest, 0 };
            spawn_argv(verb == DROP_MOVE ? mv : cp);
        }
        wm_logf("desktop: %s %d item(s) to %s", verb == DROP_MOVE ? "moved" : "copied", n, dest);
    } else if (verb == DROP_OPEN) {
        const struct gui_app *a = launcher_app(t);
        int opened = 0;
        for (int i = 0; a && i < n && opened < 8; i++) {
            const char *ext = ext_of(paths[i]);
            if (!ext || !uopen_ext_matches(a->handles, ext)) continue;
            char *const argv[] = { (char *)a->exec_path, paths[i], 0 };
            spawn_argv(argv);
            opened++;
        }
        wm_logf("desktop: opened %d item(s) with %s", opened, item_name(t));
    } else if (verb == DROP_BIN) {
        static char *argv[DESKTOP_FILES_MAX + 3];
        int k = 0;
        argv[k++] = "/bin/trash";
        argv[k++] = "put";
        for (int i = 0; i < n && k < DESKTOP_FILES_MAX + 2; i++) argv[k++] = paths[i];
        argv[k] = 0;
        spawn_argv(argv);
        wm_logf("desktop: %d item(s) to the Recycle Bin", n);
    } else {
        wm_logf("desktop: %s refused the drop", item_name(t));
    }
    return 1;
}

// The release of the desktop's OWN icon drag, over an icon.
static int drop_desktop_drag(void) {
    if (g_drop_on < 0) return 0;
    static char paths[DESKTOP_FILES_MAX][PATH_BUF];
    int n = drag_paths(paths, DESKTOP_FILES_MAX);
    return drop_onto_target(paths, n);
}

// Delete asks first, through the WM's own confirm dialog, and moves the
// items to the Recycle Bin; Shift+Delete deletes them for good. The paths
// are snapshotted when the dialog opens: the selection may change under it.
static char g_del_paths[DESKTOP_FILES_MAX][PATH_BUF];
static int g_del_count;
static int g_del_forever;
static char g_del_msg[PATH_BUF + 40];
static void delete_confirmed(void) {
    static char *argv[DESKTOP_FILES_MAX + 3];
    int k = 0;
    argv[k++] = g_del_forever ? "/bin/rm" : "/bin/trash";
    argv[k++] = g_del_forever ? "-r" : "put";
    for (int i = 0; i < g_del_count; i++) argv[k++] = g_del_paths[i];
    argv[k] = 0;
    spawn_argv(argv);
    g_del_count = 0;
}
static void delete_selected(int forever) {
    g_del_forever = forever;
    g_del_count = selected_paths(g_del_paths, DESKTOP_FILES_MAX);
    if (g_del_count == 0) return;
    if (g_del_count == 1)
        k_snprintf(g_del_msg, sizeof g_del_msg, forever ? "Permanently delete %s?"
                                                         : "Move %s to the Recycle Bin?",
                   k_path_basename(g_del_paths[0]));
    else
        k_snprintf(g_del_msg, sizeof g_del_msg, forever ? "Permanently delete %d items?"
                                                         : "Move %d items to the Recycle Bin?",
                   g_del_count);
    confirm_dialog_open_labelled(g_del_msg, forever ? "Delete" : "Move", "Cancel",
                                 delete_confirmed, 0);
}
static void menu_delete(void *ctx) {
    (void)ctx;
    delete_selected(0);
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

static void bin_empty_confirmed(void) {
    char *const argv[] = { "/bin/trash", "empty", 0 };
    spawn_argv(argv);
    wm_logf("desktop: emptied the Recycle Bin");
}
static void menu_empty_bin(void *ctx) {
    (void)ctx;
    confirm_dialog_open_labelled("Permanently delete everything in the Recycle Bin?", "Empty", "Cancel",
                                 bin_empty_confirmed, 0);
}

// --- rename in place ---------------------------------------------------
//
// F2 or the icon menu's Rename: the caption becomes a field -- the
// toolkit's uui_textbox, as the File Manager's in-place rename is -- and
// Enter or a click anywhere else commits, Esc abandons. A FILE is renamed
// (sys_rename refuses an existing name rather than replacing it); a
// LAUNCHER's caption is its Name= key, so that is what changes and the
// file keeps its name, its saved position and its AppId -- KDE's rule
// for a .desktop file on the desktop.
static char g_rename_path[PATH_BUF];    // its path when the edit began
static struct uui_textbox g_rename_box;

static int rename_rect(int *x, int *y, int *w, int *h) {
    if (g_rename < 0 || !item_visible(g_rename)) return 0;
    struct icon_grid g = current_grid();
    int cx, cy;
    icon_grid_cell_rect(&g, icon_col[g_rename], icon_row[g_rename], &cx, &cy);
    *x = cx + 2;
    *y = cy + icon_px() + 2;
    *w = icon_col_w() - 4;
    *h = ugfx_char_h() + 6;
    return 1;
}

static void damage_rename(void) {
    if (g_rename >= 0) damage_icon_hl(g_rename);
    redraw_pending = 1;
}

static void begin_rename(int i) {
    if (i < 0 || !item_visible(i)) return;
    if (g_rename >= 0) damage_rename();
    g_rename = i;
    item_path(i, g_rename_path, sizeof g_rename_path);
    const char *text = item_is_launcher(i) ? g_launch[i].name : k_path_basename(g_rename_path);
    uui_textbox_init(&g_rename_box, text);
    uui_textbox_set_active(&g_rename_box, 1);
    g_rename_box.border = UTHEME_ACCENT;
    // The NAME selected, not the extension -- the File Manager's rule.
    int n = (int)k_strlen(text), stem = n;
    const char *dot = k_strrchr(text, '.');
    if (!item_is_launcher(i) && !item_is_dir(i) && dot && dot != text) stem = (int)(dot - text);
    uui_textbox_select(&g_rename_box, 0, stem);
    damage_rename();
}

// A launcher's new caption: its Name= line rewritten, every other line
// kept as it was. Small files only -- a .desktop entry is a few lines.
static void rename_launcher(const char *path, const char *name) {
    static char in[1024], out[1200];
    int fd = sys_open(path, 0);   // read-only
    if (fd < 0) { wm_logf("desktop: cannot read %s", path); return; }
    int n = (int)sys_read(fd, in, sizeof in - 1);
    sys_close(fd);
    if (n <= 0 || n >= (int)sizeof in - 1) { wm_logf("desktop: %s unreadable or too long", path); return; }
    in[n] = '\0';
    int o = 0, done = 0;
    for (const char *line = in; *line; ) {
        const char *nl = k_strchr(line, '\n');
        int len = nl ? (int)(nl - line) : (int)k_strlen(line);
        if (!done && len >= 5 && k_strncmp(line, "Name=", 5) == 0) {
            o += k_snprintf(out + o, sizeof out - (size_t)o, "Name=%s\n", name);
            done = 1;
        } else if (o + len + 1 < (int)sizeof out) {
            k_memcpy(out + o, line, (size_t)len);
            o += len;
            out[o++] = '\n';
        }
        line = nl ? nl + 1 : line + len;
        if (o >= (int)sizeof out - 1) break;
    }
    if (!done) o += k_snprintf(out + o, sizeof out - (size_t)o, "Name=%s\n", name);
    if (o <= 0 || o >= (int)sizeof out) return;
    fd = sys_open(path, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (fd < 0) { wm_logf("desktop: cannot write %s", path); return; }
    if (sys_write(fd, out, (size_t)o) != o) wm_logf("desktop: short write to %s", path);
    sys_close(fd);
}

static void end_rename(int commit) {
    if (g_rename < 0) return;
    int i = g_rename;
    damage_rename();
    g_rename = -1;
    uui_textbox_set_active(&g_rename_box, 0);
    const char *to = uui_textbox_text(&g_rename_box);
    if (!commit || !to[0] || k_strchr(to, '/')) return;
    if (item_is_launcher(i)) {
        if (k_strcmp(to, g_launch[i].name) == 0) return;
        rename_launcher(g_rename_path, to);
    } else {
        char dst[PATH_BUF];
        if (k_strcmp(to, k_path_basename(g_rename_path)) == 0) return;
        if (!k_path_join(DESKTOP_DIR, to, dst, sizeof dst)) return;
        if (sys_rename(g_rename_path, dst) < 0) wm_logf("desktop: rename to %s refused", to);
    }
    desktop_entries_changed();
    wm_damage_rect(0, 0, screen_w, screen_h - taskbar_h);
}

// A left click anywhere while a caption is being edited: on the field it
// places the caret (1, consumed); anywhere else it commits and the click
// goes on to do what it would have done (0) -- Explorer's behaviour.
int desktop_rename_click(int mx, int my) {
    if (g_rename < 0) return 0;
    int x, y, w, h;
    if (rename_rect(&x, &y, &w, &h) && uui_hit(x, y, w, h, mx, my)) {
        uui_textbox_set_geometry(&g_rename_box, x, y, w, h);
        uui_textbox_ops.press(&g_rename_box, mx, my, 0);
        damage_rename();
        return 1;
    }
    end_rename(1);
    return 0;
}

int desktop_renaming(void) { return g_rename; }

static void draw_rename(void) {
    int x, y, w, h;
    if (!rename_rect(&x, &y, &w, &h)) return;
    uui_textbox_set_geometry(&g_rename_box, x, y, w, h);
    uui_textbox_draw(wm_surface(), &g_rename_box);
}

static void menu_rename(void *ctx) {
    (void)ctx;
    for (int i = 0; i < item_count(); i++)
        if (rb_is_selected(&sel, i)) { begin_rename(i); return; }
}

// Properties: the Properties app with the path, one window per item --
// it is not single-instance, so two can be compared (properties.c).
// Capped, as Explorer is not: a band over forty icons should not open
// forty windows.
#define DESKTOP_PROPERTIES_MAX 4
static void menu_properties(void *ctx) {
    (void)ctx;
    static char paths[DESKTOP_PROPERTIES_MAX][PATH_BUF];
    int n = selected_paths(paths, DESKTOP_PROPERTIES_MAX);
    for (int i = 0; i < n; i++) {
        char *const argv[] = { "/bin/wm/apps/properties", paths[i], 0 };
        spawn_argv(argv);
    }
}

static void menu_refresh(void *ctx);

// The keyboard's half when no window has the focus: Ctrl+C / Ctrl+X /
// Ctrl+V, Delete, F2 (rename), F5 (refresh) and Alt+Enter (properties);
// and EVERY key while a caption is being edited. Returns 1 if the key
// was the desktop's.
int desktop_handle_key(int key, unsigned mods) {
    if (g_rename >= 0) {
        if (key == '\n' || key == '\r') end_rename(1);
        else if (key == 0x1B) end_rename(0);
        else uui_textbox_key_mods(&g_rename_box, key, mods);
        damage_rename();
        return 1;
    }
    if (key == KEY_DELETE) { delete_selected((mods & KEY_MOD_SHIFT) != 0); return 1; }
    if (key == KEY_F2) { menu_rename(0); return 1; }
    if (key == KEY_F5) { menu_refresh(0); return 1; }
    if ((key == '\n' || key == '\r') && (mods & KEY_MOD_ALT)) { menu_properties(0); return 1; }
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

// The last row opens System Settings, found by its app id so a renamed
// entry still opens it, and called by the app's own name -- it opens the
// whole app, not a desktop page. Windows ("Personalize") and KDE
// ("Configure Desktop and Wallpaper") end their menu the same way.
static void menu_settings(void *ctx) {
    (void)ctx;
    for (int i = 0; i < gui_app_registry_count; i++) {
        const struct gui_app *app = &gui_app_registry[i];
        if (app->app_id && k_strcmp(app->app_id, "settings") == 0) { open_app(app); return; }
    }
    wm_logf("desktop: no System Settings entry to open");
}

// Open > groups the desktop's apps by their Start-menu folder (Category=),
// so the list has no cap to fall off -- it used to stop at sixteen and
// lose whatever sorted last. KDE's and Openbox's root menus are this shape.
#define DESKTOP_OPEN_CATS 12
#define DESKTOP_OPEN_APPS 48

static int build_open_menu(struct context_menu_item *cats, struct context_menu_item *apps) {
    int napps = gui_app_visible_count(GUI_SHOW_DESKTOP);
    if (napps > DESKTOP_OPEN_APPS) napps = DESKTOP_OPEN_APPS;
    static char cat_icons[DESKTOP_OPEN_CATS][24];
    int ncat = 0, k = 0;
    // Visible apps come sorted by (category, name), so a category is a run.
    for (int i = 0; i < napps; i++) {
        struct gui_app *app = gui_app_visible_at(GUI_SHOW_DESKTOP, i);
        const char *key = app->category ? app->category : "";
        if (ncat == 0 || k_strcmp(key, (const char *)cats[ncat - 1].ctx) != 0) {
            if (ncat == DESKTOP_OPEN_CATS) break;
            k_snprintf(cat_icons[ncat], sizeof cat_icons[ncat], "cat-%s", key);
            cats[ncat] = (struct context_menu_item){ .label = gui_app_cat_label_for(key),
                                                     .ctx = (void *)key, .sub = &apps[k],
                                                     .icon = cat_icons[ncat] };
            ncat++;
        }
        apps[k++] = (struct context_menu_item){ .label = app->name, .on_select = launch_from_menu,
                                                .ctx = (void *)app,
                                                .icon = app->icon_name[0] ? app->icon_name : 0 };
        cats[ncat - 1].sub_count++;
    }
    return ncat;
}

// Is there anything on the clipboard the desktop can paste? Asked when
// the menu opens, so Paste greys out instead of doing nothing.
static int can_paste(void) {
    static struct uclip c;
    uclip_load(&c);
    return uclip_op(&c) != UCLIP_NONE && uclip_kind(&c) == UCLIP_KIND_FILES;
}

void desktop_handle_right_click(int mx, int my) {
    static struct context_menu_item cats[DESKTOP_OPEN_CATS];
    static struct context_menu_item apps[DESKTOP_OPEN_APPS];
    static struct context_menu_item sizes[3];
    static struct context_menu_item items[12];
    int k = 0;
    if (g_rename >= 0) end_rename(1);

    int idx = icon_hit_test(mx, my);
    if (idx >= 0) {
        // The click selects the icon first, unless it is already in the
        // set -- right-clicking one of five must offer to act on five.
        if (!rb_is_selected(&sel, idx)) { rb_clear(&sel); rb_select(&sel, idx, 1); }
        redraw_pending = 1;
        // The Recycle Bin alone is a place, not a file: Open and Empty.
        if (item_is_bin(idx) && rb_selected_count(&sel) == 1) {
            items[k++] = (struct context_menu_item){ .label = "Open", .on_select = menu_open_item,
                                                     .ctx = (void *)(intptr_t)idx,
                                                     .icon = "tb-open", .tint = UTHEME_ACT_NAV };
            items[k++] = (struct context_menu_item){ .label = "Empty Recycle Bin", .on_select = menu_empty_bin,
                                                     .icon = "tb-bin-empty", .tint = UTHEME_ACT_DANGER,
                                                     .disabled = !g_bin_full };
            context_menu_open_at(mx, my, items, k);
            return;
        }
        // Windows 11's shape: the file verbs as a strip of buttons, then
        // the rows. Rename names one thing, so it greys out for several.
        int one = rb_selected_count(&sel) == 1;
        items[k++] = (struct context_menu_item){ .label = "Cut", .on_select = menu_cut, .strip = 1,
                                                 .icon = "tb-cut", .tint = UTHEME_ACT_EDIT, .accel = "Ctrl+X" };
        items[k++] = (struct context_menu_item){ .label = "Copy", .on_select = menu_copy, .strip = 1,
                                                 .icon = "tb-copy", .tint = UTHEME_ACT_EDIT, .accel = "Ctrl+C" };
        items[k++] = (struct context_menu_item){ .label = "Rename", .on_select = menu_rename, .strip = 1,
                                                 .icon = "tb-rename", .tint = UTHEME_ACT_EDIT, .accel = "F2",
                                                 .disabled = !one };
        items[k++] = (struct context_menu_item){ .label = "Delete", .on_select = menu_delete, .strip = 1,
                                                 .icon = "tb-delete", .tint = UTHEME_ACT_DANGER, .accel = "Del" };
        items[k++] = (struct context_menu_item){ .separator = 1 };
        items[k++] = (struct context_menu_item){ .label = "Open", .on_select = menu_open_item,
                                                 .ctx = (void *)(intptr_t)idx,
                                                 .icon = "tb-open", .tint = UTHEME_ACT_NAV };
        items[k++] = (struct context_menu_item){ .label = "Properties", .on_select = menu_properties,
                                                 .icon = "tb-info", .accel = "Alt+Enter" };
        context_menu_open_at(mx, my, items, k);
        return;
    }

    int ncat = build_open_menu(cats, apps);
    static const char *const words[3] = { "small", "medium", "large" };
    static const char *const labels[3] = { "Small", "Medium", "Large" };
    for (int i = 0; i < 3; i++) {
        sizes[i] = (struct context_menu_item){ .label = labels[i],
                                               .on_select = set_icon_size,
                                               .ctx = (void *)words[i],
                                               .checked = k_strcmp(g_icon_size, words[i]) == 0 };
    }

    items[k++] = (struct context_menu_item){ .label = "Open", .sub = cats, .sub_count = ncat,
                                             .icon = "tb-open", .tint = UTHEME_ACT_NAV };
    items[k++] = (struct context_menu_item){ .separator = 1 };
    items[k++] = (struct context_menu_item){ .label = "New folder", .on_select = menu_new_folder,
                                             .icon = "tb-mkdir", .tint = UTHEME_ACT_CREATE };
    items[k++] = (struct context_menu_item){ .label = "Paste", .on_select = menu_paste,
                                             .icon = "tb-paste", .tint = UTHEME_ACT_EDIT,
                                             .accel = "Ctrl+V", .disabled = !can_paste() };
    items[k++] = (struct context_menu_item){ .separator = 1 };
    items[k++] = (struct context_menu_item){ .label = "Refresh", .on_select = menu_refresh,
                                             .icon = "tb-refresh", .tint = UTHEME_ACT_VIEW, .accel = "F5" };
    items[k++] = (struct context_menu_item){ .label = "Sort by name", .on_select = menu_sort,
                                             .icon = "tb-sort", .tint = UTHEME_ACT_ARRANGE };
    items[k++] = (struct context_menu_item){ .label = "Icon size", .sub = sizes, .sub_count = 3,
                                             .icon = "tb-icons", .tint = UTHEME_ACT_VIEW };
    items[k++] = (struct context_menu_item){ .separator = 1 };
    items[k++] = (struct context_menu_item){ .label = "System Settings", .on_select = menu_settings,
                                             .icon = "tb-gear" };
    context_menu_open_at(mx, my, items, k);
}
