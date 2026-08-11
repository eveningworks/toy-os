// See file_picker.h for the design writeup (scope, what's deliberately
// not built yet, and why).
#include "file_picker.h"
#include "wm_internal.h"
#include "ui/ui.h"
#include "theme.h"
#include "kapi.h"

int file_picker_open = 0;

struct fp_entry {
    char name[FS_PATH_MAX];
    int is_dir;
};

#define FP_MAX_ENTRIES FS_MAX_FILES // a directory can never hold more than this (fs.h)
#define FP_TITLE_MAX 24
#define FP_ROWS_VISIBLE 8
#define FP_PAD 12
#define FP_SCROLLBAR_W 14

static enum file_picker_mode g_mode;
static char g_title[FP_TITLE_MAX];
static void (*g_on_choose)(const char *path);
static void (*g_on_cancel)(void);

static char g_cwd[FS_PATH_MAX];
static struct fp_entry g_entries[FP_MAX_ENTRIES];
static int g_entry_count;
static int g_has_up; // 1 if a synthetic ".." row is shown (g_cwd isn't root)

// "Combined rows" = the ".." row (if g_has_up), then g_entries[] --
// what's actually drawn/hit-tested as the list, so both agree on row
// indices without a caller ever having to special-case ".." separately.
static int fp_row_count(void) { return g_entry_count + (g_has_up ? 1 : 0); }
// Returns 1 and fills *out_is_up/*out_entry for a valid combined row
// index, 0 if out of range.
static int fp_row_at(int i, int *out_is_up, const struct fp_entry **out_entry) {
    if (i < 0 || i >= fp_row_count()) return 0;
    if (g_has_up) {
        if (i == 0) { *out_is_up = 1; *out_entry = 0; return 1; }
        i--;
    }
    *out_is_up = 0;
    *out_entry = &g_entries[i];
    return 1;
}

static int g_scroll_offset; // MY convention: 0 = top row shown, increasing = scrolled down
                             // (opposite of ui_scrollbar.h's own "0 = bottom" text-scrollback
                             // convention -- converted at every widget_scrollbar_*() call
                             // below, see fp_widget_scroll_offset())
static int g_selected_row;  // combined-row index of the currently-selected FILE row, or -1
                             // (directories/"..'" are never "selected" -- clicking one just
                             // navigates, see fp_handle_row_click())
static int g_last_click_row; // combined-row index of the last single click, for double-click
                              // detection -- -2 = none yet this dialog
static uint64_t g_last_click_tick;
#define FP_DOUBLE_CLICK_TICKS 30 // same threshold desktop.c's icon double-click uses

static struct ui_textbox g_name_box;

// Geometry -- computed once at open time (screen size can't change
// while a modal dialog is open), same reasoning as confirm_dialog.c's
// own geometry statics.
static int g_x, g_y, g_w, g_h;
static int g_path_y;
static int g_list_x, g_list_y, g_list_w, g_list_h, g_row_h;
static int g_field_y, g_field_h;
static int g_btn_y, g_btn_h;
static int g_ok_x, g_ok_w, g_cancel_x, g_cancel_w;

// ---- path helpers ----

// Appends `name` (one path component, no '/') onto `dir` (an already-
// normalized absolute path -- "/" or "/a/b", never a trailing slash
// except root itself) into `out`, truncating to fit FS_PATH_MAX. Same
// join logic apps/shell.c's resolve_path() folds into its own bigger
// ".."-collapsing routine -- this is the simpler "append one known-good
// component" case, no ".." parsing needed since directory names never
// contain one.
static void fp_join(char *out, const char *dir, const char *name) {
    int dl = (int)k_strlen(dir);
    if (dl > FS_PATH_MAX - 2) dl = FS_PATH_MAX - 2;
    k_memcpy(out, dir, dl);
    out[dl] = '\0';
    if (dl == 0 || out[dl - 1] != '/') {
        out[dl] = '/';
        out[dl + 1] = '\0';
        dl++;
    }
    int remaining = (FS_PATH_MAX - 1) - dl;
    int nl = (int)k_strlen(name);
    if (nl > remaining) nl = remaining;
    if (nl > 0) k_memcpy(out + dl, name, nl);
    out[dl + nl] = '\0';
}

// Resolves the filename field's current text against g_cwd: absolute
// (starts with '/') is used as-is, anything else is joined onto g_cwd
// -- same "absolute or cwd-relative" rule fs.h's own top comment
// documents for every path it takes, minus the ".."/"." collapsing a
// typed filename has no legitimate reason to contain here.
static void fp_resolve_typed(char *out) {
    const char *typed = g_name_box.field.buf;
    if (typed[0] == '/') k_strcpy(out, typed);
    else fp_join(out, g_cwd, typed);
}

// ---- listing ----

static void fp_collect_cb(const char *name, uint32_t size, int is_dir) {
    (void)size;
    if (g_entry_count >= FP_MAX_ENTRIES) return; // can't happen -- fs.h caps a dir at this many entries -- defensive only
    k_strcpy(g_entries[g_entry_count].name, name);
    g_entries[g_entry_count].is_dir = is_dir;
    g_entry_count++;
}

// Directories first, then alphabetical within each group -- fs_list()
// itself hands back table order (insertion order), not sorted (see
// fs.h). FP_MAX_ENTRIES is small (<=32) so a plain insertion sort costs
// nothing worth optimizing.
static void fp_sort_entries(void) {
    for (int i = 1; i < g_entry_count; i++) {
        struct fp_entry key = g_entries[i];
        int j = i - 1;
        while (j >= 0) {
            int key_first = key.is_dir && !g_entries[j].is_dir;
            int same_kind = key.is_dir == g_entries[j].is_dir;
            int later_alpha = same_kind && k_strcmp(key.name, g_entries[j].name) < 0;
            if (!key_first && !later_alpha) break;
            g_entries[j + 1] = g_entries[j];
            j--;
        }
        g_entries[j + 1] = key;
    }
}

static void fp_refresh_listing(void) {
    g_entry_count = 0;
    fs_list(g_cwd, fp_collect_cb);
    fp_sort_entries();
    g_has_up = (k_strcmp(g_cwd, "/") != 0);
    g_selected_row = -1;
    g_scroll_offset = 0;
    g_last_click_row = -2;
    // Every caller changes what's on screen (a new directory's
    // contents) -- set this here, once, rather than relying on each
    // caller (fp_go_up(), a directory double-click, file_picker_open_with())
    // to remember it individually. A real bug this way round caught by
    // QMP testing: fp_go_up()'s caller forgot this, so navigating "up"
    // silently updated g_cwd/g_entries[] with nothing on screen
    // reflecting it until some unrelated later click forced a repaint.
    redraw_pending = 1;
}

// Sets g_cwd to `dir` if it actually resolves to a real directory (or
// root), falling back to "/" otherwise -- the one place a caller-
// supplied `start_dir` (file_picker_open_with()) or a typed path that
// turned out to be a directory (fp_confirm()) gets validated before
// becoming the active listing.
static void fp_set_dir(const char *dir) {
    if (dir && (k_strcmp(dir, "/") == 0 || fs_is_dir(dir))) k_strcpy(g_cwd, dir);
    else k_strcpy(g_cwd, "/");
    fp_refresh_listing();
}

static void fp_go_up(void) {
    if (k_strcmp(g_cwd, "/") == 0) return;
    int i = (int)k_strlen(g_cwd) - 1;
    while (i > 0 && g_cwd[i] != '/') i--;
    if (i == 0) g_cwd[1] = '\0'; // parent of a single top-level dir ("/docs") is root
    else g_cwd[i] = '\0';
    fp_set_dir(g_cwd);
}

// ---- open / geometry ----

static void fp_compute_geometry(void) {
    int cw = gfx_char_w(), ch = gfx_char_h();

    g_w = 44 * cw + 2 * FP_PAD;
    if (g_w > screen_w - 20) g_w = screen_w - 20;

    g_row_h = ch + 6;
    g_field_h = ch + 10;
    g_btn_h = ch + 10;
    int list_h = FP_ROWS_VISIBLE * g_row_h;

    g_h = FP_PAD + ch          // title row
        + 6 + ch               // path row
        + 8 + list_h           // list
        + 8 + g_field_h        // filename field
        + 8 + g_btn_h           // Open/Save + Cancel row
        + FP_PAD;
    if (g_h > screen_h - 20) g_h = screen_h - 20;

    g_x = (screen_w - g_w) / 2;
    g_y = (screen_h - g_h) / 2;

    int y = g_y + FP_PAD + ch + 6;
    g_path_y = y;
    y += ch + 8;

    g_list_x = g_x + FP_PAD;
    g_list_y = y;
    g_list_w = g_w - 2 * FP_PAD;
    g_list_h = list_h;
    y += list_h + 8;

    g_field_y = y;
    y += g_field_h + 8;

    g_btn_y = y;
    const char *ok_label = (g_mode == FILE_PICKER_SAVE) ? "Save" : "Open";
    g_ok_w = (int)k_strlen(ok_label) * cw + 24;
    g_cancel_w = 6 * cw + 24; // "Cancel"
    g_ok_x = g_x + g_w - FP_PAD - g_cancel_w - 8 - g_ok_w;
    g_cancel_x = g_x + g_w - FP_PAD - g_cancel_w;
}

void file_picker_open_with(enum file_picker_mode mode, const char *title,
                            const char *start_dir, const char *initial_name,
                            void (*on_choose)(const char *path),
                            void (*on_cancel)(void)) {
    g_mode = mode;
    k_strcpy(g_title, title);
    g_on_choose = on_choose;
    g_on_cancel = on_cancel;

    fp_compute_geometry();
    fp_set_dir(start_dir);

    ui_textbox_init(&g_name_box, g_list_x, g_field_y, g_list_w, g_field_h,
                     initial_name, THEME_WHITE, THEME_TEXT, THEME_BORDER);

    file_picker_open = 1;
    redraw_pending = 1;
}

static void fp_close(void) {
    file_picker_open = 0;
    redraw_pending = 1;
}

// ---- committing a choice ----

static void fp_confirm(void) {
    if (g_name_box.field.len == 0) return; // nothing typed/selected -- no-op, same as Notepad's old "Bad filename." guard

    char path[FS_PATH_MAX];
    fp_resolve_typed(path);

    // Typed/selected a directory -- real dialogs treat this as
    // "navigate into it", not an error and not a valid choice (you
    // can't Open/Save "a directory" through this simple picker).
    if (fs_is_dir(path)) {
        fp_set_dir(path);
        ui_textbox_init(&g_name_box, g_list_x, g_field_y, g_list_w, g_field_h,
                         "", THEME_WHITE, THEME_TEXT, THEME_BORDER);
        redraw_pending = 1;
        return;
    }

    if (g_mode == FILE_PICKER_OPEN && !fs_exists(path)) return; // silently ignore -- no error UI yet, see file_picker.h

    fp_close();
    if (g_on_choose) g_on_choose(path);
}

static void fp_cancel(void) {
    fp_close();
    if (g_on_cancel) g_on_cancel();
}

// ---- scrollbar offset convention conversion (see g_scroll_offset's comment) ----

static int fp_max_scroll(void) {
    int rows = fp_row_count();
    return rows > FP_ROWS_VISIBLE ? rows - FP_ROWS_VISIBLE : 0;
}

static int fp_widget_scroll_offset(void) {
    return fp_max_scroll() - g_scroll_offset;
}

// ---- drawing ----

void file_picker_draw(void) {
    if (!file_picker_open) return;

    uint32_t bg = THEME_PANEL_BG, border = THEME_BORDER, fg = THEME_TEXT;
    gfx_fill_rect(g_x, g_y, g_w, g_h, bg);

    gfx_draw_string(g_x + FP_PAD, g_y + FP_PAD, g_title, fg, bg);

    // Current path, truncated to fit if it's longer than the dialog is
    // wide -- FS_PATH_MAX (64) can exceed what 44 columns comfortably
    // shows at a large font size.
    int max_path_chars = g_list_w / gfx_char_w();
    char path_buf[FS_PATH_MAX];
    int pl = (int)k_strlen(g_cwd);
    if (pl > max_path_chars) pl = max_path_chars;
    k_memcpy(path_buf, g_cwd, pl);
    path_buf[pl] = '\0';
    gfx_draw_string(g_list_x, g_path_y, path_buf, gfx_rgb(90, 90, 90), bg);

    // The list box itself.
    gfx_fill_rect(g_list_x, g_list_y, g_list_w, g_list_h, THEME_WHITE);
    gfx_draw_rect(g_list_x, g_list_y, g_list_w, g_list_h, border);

    int show_scrollbar = fp_row_count() > FP_ROWS_VISIBLE;
    int text_w = g_list_w - (show_scrollbar ? FP_SCROLLBAR_W : 0);
    int max_chars = (text_w - 8) / gfx_char_w();
    if (max_chars < 1) max_chars = 1;

    for (int row = 0; row < FP_ROWS_VISIBLE; row++) {
        int idx = g_scroll_offset + row;
        int is_up;
        const struct fp_entry *e;
        if (!fp_row_at(idx, &is_up, &e)) break;

        int ry = g_list_y + row * g_row_h;
        int selected = (!is_up && idx == g_selected_row);
        uint32_t row_bg = selected ? gfx_rgb(51, 144, 255) : THEME_WHITE;
        uint32_t row_fg = selected ? THEME_WHITE : fg;
        if (selected) gfx_fill_rect(g_list_x, ry, text_w, g_row_h, row_bg);

        char label[FS_PATH_MAX + 2];
        if (is_up) {
            k_strcpy(label, "../");
        } else if (e->is_dir) {
            k_strcpy(label, e->name);
            int n = (int)k_strlen(label);
            if (n < (int)sizeof(label) - 1) { label[n] = '/'; label[n + 1] = '\0'; }
        } else {
            k_strcpy(label, e->name);
        }
        int ln = (int)k_strlen(label);
        if (ln > max_chars) { label[max_chars] = '\0'; }

        gfx_draw_string(g_list_x + 4, ry + 3, label, row_fg, row_bg);
    }

    if (show_scrollbar) {
        widget_scrollbar_draw(g_list_x + text_w, g_list_y, FP_SCROLLBAR_W, g_list_h,
                               fp_row_count(), FP_ROWS_VISIBLE, fp_widget_scroll_offset(),
                               gfx_rgb(225, 225, 230), gfx_rgb(150, 150, 160));
    }

    ui_textbox_draw(&g_name_box, 0, 0); // screen-absolute geometry (like the rest of this dialog), so origin is (0,0)

    const char *ok_label = (g_mode == FILE_PICKER_SAVE) ? "Save" : "Open";
    gfx_fill_rect(g_ok_x, g_btn_y, g_ok_w, g_btn_h, THEME_BUTTON_BG);
    gfx_draw_rect(g_ok_x, g_btn_y, g_ok_w, g_btn_h, border);
    gfx_draw_string(g_ok_x + (g_ok_w - (int)k_strlen(ok_label) * gfx_char_w()) / 2,
                     g_btn_y + (g_btn_h - gfx_char_h()) / 2, ok_label, fg, THEME_BUTTON_BG);

    gfx_fill_rect(g_cancel_x, g_btn_y, g_cancel_w, g_btn_h, THEME_BUTTON_BG);
    gfx_draw_rect(g_cancel_x, g_btn_y, g_cancel_w, g_btn_h, border);
    gfx_draw_string(g_cancel_x + (g_cancel_w - 6 * gfx_char_w()) / 2,
                     g_btn_y + (g_btn_h - gfx_char_h()) / 2, "Cancel", fg, THEME_BUTTON_BG);

    gfx_draw_rect(g_x, g_y, g_w, g_h, border); // last, so no row/field fill overpaints it -- same ordering lesson start_menu.c's own comment documents
}

// ---- clicks ----

// (mx, my) already known to be inside the list box (not the scrollbar
// strip) -- resolves which combined row it landed on and applies the
// single/double-click contract described in file_picker.h.
static void fp_handle_row_click(int mx, int my) {
    (void)mx;
    int row = (my - g_list_y) / g_row_h;
    int idx = g_scroll_offset + row;
    int is_up;
    const struct fp_entry *e;
    if (!fp_row_at(idx, &is_up, &e)) return;

    uint64_t now = pit_ticks();
    int is_double = (idx == g_last_click_row && now - g_last_click_tick <= FP_DOUBLE_CLICK_TICKS);

    if (is_up) {
        // fp_go_up() calls fp_set_dir()/fp_refresh_listing(), which
        // change g_cwd/g_entries[] but don't themselves touch
        // redraw_pending (fp_set_dir() is also called from
        // file_picker_open_with(), which sets it separately) -- has to
        // be set here, same as the directory-double-click branch below,
        // or the navigation happens but the screen keeps showing the
        // old listing until some unrelated later event forces a
        // repaint. Caught by QMP testing: a double-click on ".." looked
        // like a no-op from the screenshot even though the dialog's
        // internal state had actually moved up a directory.
        if (is_double) { fp_go_up(); redraw_pending = 1; return; }
        g_last_click_row = idx;
        g_last_click_tick = now;
        redraw_pending = 1;
        return;
    }

    if (e->is_dir) {
        if (is_double) {
            char path[FS_PATH_MAX];
            fp_join(path, g_cwd, e->name);
            fp_set_dir(path);
            g_last_click_row = -2;
            redraw_pending = 1;
            return;
        }
        g_last_click_row = idx;
        g_last_click_tick = now;
        redraw_pending = 1;
        return;
    }

    // A file row.
    g_selected_row = idx;
    ui_textbox_init(&g_name_box, g_list_x, g_field_y, g_list_w, g_field_h,
                     e->name, THEME_WHITE, THEME_TEXT, THEME_BORDER);
    if (is_double) {
        fp_confirm();
        g_last_click_row = -2;
        return;
    }
    g_last_click_row = idx;
    g_last_click_tick = now;
    redraw_pending = 1;
}

int file_picker_handle_click(int mx, int my) {
    if (!file_picker_open) return 0;

    if (widget_hit(g_ok_x, g_btn_y, g_ok_w, g_btn_h, mx, my)) { fp_confirm(); return 1; }
    if (widget_hit(g_cancel_x, g_btn_y, g_cancel_w, g_btn_h, mx, my)) { fp_cancel(); return 1; }

    if (ui_textbox_hit(&g_name_box, mx, my)) {
        ui_textbox_set_active(&g_name_box, 1);
        redraw_pending = 1;
        return 1;
    }
    if (g_name_box.field.active) {
        ui_textbox_set_active(&g_name_box, 0);
        redraw_pending = 1;
    }

    int show_scrollbar = fp_row_count() > FP_ROWS_VISIBLE;
    int text_w = g_list_w - (show_scrollbar ? FP_SCROLLBAR_W : 0);

    if (widget_hit(g_list_x, g_list_y, text_w, g_list_h, mx, my)) {
        fp_handle_row_click(mx, my);
        return 1;
    }

    if (show_scrollbar) {
        enum scrollbar_zone zone = widget_scrollbar_hit(g_list_x + text_w, g_list_y, FP_SCROLLBAR_W, g_list_h,
                                                          fp_row_count(), FP_ROWS_VISIBLE, fp_widget_scroll_offset(),
                                                          mx, my);
        int max_scroll = fp_max_scroll();
        int page = FP_ROWS_VISIBLE > 1 ? FP_ROWS_VISIBLE - 1 : 1;
        if (zone == SCROLLBAR_ZONE_ABOVE) {
            g_scroll_offset -= page;
            if (g_scroll_offset < 0) g_scroll_offset = 0;
            redraw_pending = 1;
        } else if (zone == SCROLLBAR_ZONE_BELOW) {
            g_scroll_offset += page;
            if (g_scroll_offset > max_scroll) g_scroll_offset = max_scroll;
            redraw_pending = 1;
        }
        // ZONE_THUMB: no drag-to-scroll yet, see file_picker.h -- a
        // click directly on the thumb just does nothing this round.
        return 1;
    }

    // Anywhere else inside (or outside) the dialog -- modal, swallowed,
    // same as confirm_dialog.c's own catch-all.
    return 1;
}

int file_picker_handle_key(int key) {
    if (!file_picker_open) return 0;

    if (g_name_box.field.active) {
        if (key == '\r' || key == '\n') {
            ui_textbox_set_active(&g_name_box, 0);
            fp_confirm();
            return 1;
        }
        if (ui_textbox_key(&g_name_box, key)) redraw_pending = 1;
    }
    return 1;
}
