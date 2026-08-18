// See file_picker.h for the design writeup (scope, what's deliberately
// not built yet, and why).
#include "file_picker.h"
#include "wm_internal.h"
#include "ui/uui.h"
#include "ui/utheme.h"
#include "kapi.h"
#include "rt/sys.h"
#include "wm/wm_fs.h"

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

static struct uui_textbox g_name_box;

// Geometry -- computed once at open time (screen size can't change
// while a modal dialog is open), same reasoning as confirm_dialog.c's
// own geometry statics.
static int g_x, g_y, g_w, g_h;
static int g_path_y;
static int g_list_x, g_list_y, g_list_w, g_list_h, g_row_h;
static int g_field_y, g_field_h;
static int g_btn_y, g_btn_h;
// OK/Cancel as a real ui_button_group -- hover, pressed and
// commit-on-release come from it. They were hand-drawn rectangles acting
// on button-DOWN until an audit against docs/gui-guidelines.md; see
// confirm_dialog.c, which had the identical three problems and the
// identical fix.
#define FP_BTN_OK     0
#define FP_BTN_CANCEL 1
static struct uui_button g_btns[2];
static struct uui_button_group g_group;

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
    const char *typed = g_name_box.buf;
    if (typed[0] == '/') k_strcpy(out, typed);
    else fp_join(out, g_cwd, typed);
}

// ---- listing ----

// Directories first, then alphabetical within each group -- a directory
// listing arrives in table order (insertion order), not sorted. FP_MAX_ENTRIES is small (<=32) so a plain insertion sort costs
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
    // A LOOP, not a callback. The kernel's fs_list() walked a directory
    // through a callback with no context pointer, so this file had to
    // collect into a global and parse afterwards; sys_listdir() fills an
    // array, so the collector is gone entirely.
    g_entry_count = 0;
    // STATIC, not on the stack. A ring-3 stack is 16 KiB with ONE 4 KiB
    // guard page below it, and FP_MAX_ENTRIES entries of `struct dirent` is
    // 20,608 bytes -- a frame that large does not merely overflow, it
    // steps clean OVER the guard into unmapped space, which is the
    // Stack Clash shape. Found by -Wframe-larger-than the day it was
    // added to USERLAND_CFLAGS. Safe here: the WM is one event loop and
    // this does not recurse.
    static struct dirent ents[FP_MAX_ENTRIES];
    int n = wm_fs_list(g_cwd, ents, FP_MAX_ENTRIES);
    for (int i = 0; i < n && g_entry_count < FP_MAX_ENTRIES; i++) {
        k_strcpy(g_entries[g_entry_count].name, ents[i].name);
        g_entries[g_entry_count].is_dir = (int)ents[i].is_dir;
        g_entry_count++;
    }
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
    if (dir && (k_strcmp(dir, "/") == 0 || wm_fs_is_dir(dir))) k_strcpy(g_cwd, dir);
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
    int cw = ugfx_char_w(), ch = ugfx_char_h();

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
    // gfx_text_width(), not k_strlen() * cw -- see api/gfx.h on why that
    // identity is only true for a fixed-cell font.
    const char *ok_label = (g_mode == FILE_PICKER_SAVE) ? "Save" : "Open";
    int ok_w = ugfx_text_width(ok_label) + 24;
    int cancel_w = ugfx_text_width("Cancel") + 24;
    int ok_x = g_x + g_w - FP_PAD - cancel_w - 8 - ok_w;
    int cancel_x = g_x + g_w - FP_PAD - cancel_w;
    uui_button_init(&g_btns[FP_BTN_OK], ok_x, g_btn_y, ok_w, g_btn_h, ok_label,
                    UTHEME_BUTTON_BG, UTHEME_TEXT, FP_BTN_OK);
    uui_button_init(&g_btns[FP_BTN_CANCEL], cancel_x, g_btn_y, cancel_w, g_btn_h,
                    "Cancel", UTHEME_BUTTON_BG, UTHEME_TEXT, FP_BTN_CANCEL);
    uui_button_group_init(&g_group, g_btns, 2);
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

    uui_textbox_init(&g_name_box, initial_name);
    uui_textbox_set_geometry(&g_name_box, g_list_x, g_field_y, g_list_w, g_field_h);

    file_picker_open = 1;
    redraw_pending = 1;
}

static void fp_close(void) {
    file_picker_open = 0;
    redraw_pending = 1;
}

// ---- committing a choice ----

static void fp_confirm(void) {
    if (g_name_box.len == 0) return; // nothing typed/selected -- no-op, same as Notepad's old "Bad filename." guard

    char path[FS_PATH_MAX];
    fp_resolve_typed(path);

    // Typed/selected a directory -- real dialogs treat this as
    // "navigate into it", not an error and not a valid choice (you
    // can't Open/Save "a directory" through this simple picker).
    if (wm_fs_is_dir(path)) {
        fp_set_dir(path);
        uui_textbox_init(&g_name_box, "");
        uui_textbox_set_geometry(&g_name_box, g_list_x, g_field_y, g_list_w, g_field_h);
        redraw_pending = 1;
        return;
    }

    if (g_mode == FILE_PICKER_OPEN && !wm_fs_exists(path)) return; // silently ignore -- no error UI yet, see file_picker.h

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

    uint32_t bg = UTHEME_PANEL_BG, border = UTHEME_BORDER, fg = UTHEME_TEXT;
    ugfx_fill_rect(wm_surface(), g_x, g_y, g_w, g_h, bg);

    ugfx_draw_string(wm_surface(), g_x + FP_PAD, g_y + FP_PAD, g_title, fg, bg);

    // Current path, truncated to fit if it's longer than the dialog is
    // wide -- FS_PATH_MAX (64) can exceed what 44 columns comfortably
    // shows at a large font size.
    int max_path_chars = g_list_w / ugfx_char_w();
    char path_buf[FS_PATH_MAX];
    int pl = (int)k_strlen(g_cwd);
    if (pl > max_path_chars) pl = max_path_chars;
    k_memcpy(path_buf, g_cwd, pl);
    path_buf[pl] = '\0';
    ugfx_draw_string(wm_surface(), g_list_x, g_path_y, path_buf, ugfx_rgb(90, 90, 90), bg);

    // The list box itself.
    ugfx_fill_rect(wm_surface(), g_list_x, g_list_y, g_list_w, g_list_h, UTHEME_WHITE);
    ugfx_draw_rect(wm_surface(), g_list_x, g_list_y, g_list_w, g_list_h, border);

    int show_scrollbar = fp_row_count() > FP_ROWS_VISIBLE;
    int text_w = g_list_w - (show_scrollbar ? FP_SCROLLBAR_W : 0);
    int max_chars = (text_w - 8) / ugfx_char_w();
    if (max_chars < 1) max_chars = 1;

    for (int row = 0; row < FP_ROWS_VISIBLE; row++) {
        int idx = g_scroll_offset + row;
        int is_up;
        const struct fp_entry *e;
        if (!fp_row_at(idx, &is_up, &e)) break;

        int ry = g_list_y + row * g_row_h;
        int selected = (!is_up && idx == g_selected_row);
        uint32_t row_bg = selected ? ugfx_rgb(51, 144, 255) : UTHEME_WHITE;
        uint32_t row_fg = selected ? UTHEME_WHITE : fg;
        if (selected) ugfx_fill_rect(wm_surface(), g_list_x, ry, text_w, g_row_h, row_bg);

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

        ugfx_draw_string(wm_surface(), g_list_x + 4, ry + 3, label, row_fg, row_bg);
    }

    if (show_scrollbar) {
        uui_scrollbar_draw(wm_surface(), g_list_x + text_w, g_list_y,
                           FP_SCROLLBAR_W, g_list_h,
                           fp_row_count(), FP_ROWS_VISIBLE, fp_widget_scroll_offset(),
                           ugfx_rgb(225, 225, 230), ugfx_rgb(150, 150, 160), 0);
    }

    uui_textbox_draw(wm_surface(), &g_name_box); // screen-absolute geometry (like the rest of this dialog), so origin is (0,0)

    uui_button_group_draw(&g_group, wm_surface()); // screen-absolute, so no origin

    ugfx_draw_rect(wm_surface(), g_x, g_y, g_w, g_h, border); // last, so no row/field fill overpaints it -- same ordering lesson start_menu.c's own comment documents
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

    uint64_t now = sys_ticks();
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
    uui_textbox_init(&g_name_box, e->name);
    uui_textbox_set_geometry(&g_name_box, g_list_x, g_field_y, g_list_w, g_field_h);
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

    // The buttons deliberately do NOT act here: this runs on
    // button-DOWN, and a control that commits here can never be
    // cancelled. Arming and committing are in
    // file_picker_update_press() below. Still swallowed, since the
    // picker is modal.
    if (uui_button_group_press(&g_group, mx, my)) { redraw_pending = 1; return 1; }

    if (uui_textbox_hit(&g_name_box, mx, my)) {
        uui_textbox_set_active(&g_name_box, 1);
        redraw_pending = 1;
        return 1;
    }
    if (g_name_box.active) {
        uui_textbox_set_active(&g_name_box, 0);
        redraw_pending = 1;
    }

    int show_scrollbar = fp_row_count() > FP_ROWS_VISIBLE;
    int text_w = g_list_w - (show_scrollbar ? FP_SCROLLBAR_W : 0);

    if (uui_hit(g_list_x, g_list_y, text_w, g_list_h, mx, my)) {
        fp_handle_row_click(mx, my);
        return 1;
    }

    if (show_scrollbar) {
        enum uui_scrollbar_zone zone = uui_scrollbar_hit(g_list_x + text_w, g_list_y, FP_SCROLLBAR_W, g_list_h,
                                                          fp_row_count(), FP_ROWS_VISIBLE, fp_widget_scroll_offset(),
                                                          mx, my, 0);
        int max_scroll = fp_max_scroll();
        int page = FP_ROWS_VISIBLE > 1 ? FP_ROWS_VISIBLE - 1 : 1;
        if (zone == UUI_SB_ABOVE) {
            g_scroll_offset -= page;
            if (g_scroll_offset < 0) g_scroll_offset = 0;
            redraw_pending = 1;
        } else if (zone == UUI_SB_BELOW) {
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

    if (g_name_box.active) {
        if (key == '\r' || key == '\n') {
            uui_textbox_set_active(&g_name_box, 0);
            fp_confirm();
            return 1;
        }
        if (uui_textbox_key(&g_name_box, key)) redraw_pending = 1;
    }
    return 1;
}

void file_picker_update_press(int mx, int my, uint8_t buttons) {
    if (!file_picker_open) return;

    if (buttons & 0x1) {
        // Re-hit-tested every tick, so dragging off a button un-presses
        // it visibly and dragging back re-presses.
        if (uui_button_group_press(&g_group, mx, my)) redraw_pending = 1;
        return;
    }

    int code = uui_button_group_release(&g_group);
    if (code < 0) return; // nothing armed, or the press was dragged off
    if (code == FP_BTN_OK) fp_confirm();
    else fp_cancel();
}

int file_picker_update_hover(int mx, int my) {
    if (!file_picker_open) return 0;
    return uui_button_group_hover(&g_group, mx, my);
}
