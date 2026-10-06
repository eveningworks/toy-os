// The details pane: a preview of the selection and its facts, down the
// right of the window -- Explorer's (Alt+Shift+P) and Dolphin's
// Information panel. Nothing selected, it describes the folder itself;
// several marked, their count and total. One item is drawn by the same
// widget as Properties (ui/uui_fileinfo.h), so the two cannot disagree.
//
// One of the File Manager's units -- see fm_internal.h for what is
// where and why these share their state directly.
#include "fm_internal.h"
#include "ui/utheme.h"
#include "ui/uui_label.h"
#include "lib/human.h"
#include "lib/udate.h"
#include "lib/ufiletype.h"
#include "lib/icon_cache.h"
#include "lib/ufileinfo.h"
#include "ui/uui_fileinfo.h"
#include "kpath.h"
#include <string.h>
#include <stdio.h>

static int g_dx, g_dy, g_dw, g_dh;   // the pane's rect, from layout_all()

#define PAD 12

// One item -- the selection, or the folder itself -- is the shared facts
// widget in its compact form (ui/uui_fileinfo.h, Properties' hero and
// General rows). Several marked keep their count and total, which no
// single-file view can say.
static struct ufileinfo g_fi;
static struct uui_fileinfo g_info;
static char g_fi_key[UUI_FILEVIEW_PATH_MAX + 32];   // path + mtime it was read for

int details_width(int cw) {
    int w = ugfx_char_advance('n') * 28;
    return w > cw / 3 ? cw / 3 : w;
}

static int line_h(void)    { return ugfx_char_h() + 4; }

// The path the pane describes, or 0 with several marked.
static int described(char *path, int cap, const struct sys_dirent **ent) {
    struct uui_fileview *fv = active();
    const struct sys_dirent *e = uui_fileview_selected_entry(fv);
    const char *dir = uui_fileview_dir(fv);
    *ent = e;
    if (uui_fileview_mark_count(fv) > 1) return 0;
    // The selection's REAL path -- in the Recycle Bin, where it lies in
    // the bin, not "trash:/" joined with its name.
    if (e) return uui_fileview_selected_path(fv, path, cap);
    if (in_bin(fv) || in_search(fv)) return 0;   // a virtual folder is nothing to describe
    strlcpy(path, dir, (size_t)cap);
    return 1;
}

// Re-read the facts only when the item (or its mtime) changed: the
// picture's headers are a file read, and this runs on every paint.
static void refresh(void) {
    static int inited;
    if (!inited) { uui_fileinfo_init(&g_info); g_info.compact = 1; inited = 1; }
    char path[UUI_FILEVIEW_PATH_MAX];
    const struct sys_dirent *e;
    if (!described(path, sizeof path, &e)) { g_fi_key[0] = 0; return; }
    char key[sizeof g_fi_key];
    snprintf(key, sizeof key, "%s|%u", path, e ? (unsigned)e->modified.second + 60u * e->modified.minute : 0u);
    if (strcmp(key, g_fi_key)) {
        strlcpy(g_fi_key, key, sizeof g_fi_key);
        ufileinfo_load(&g_fi, path, UFI_HEADERS);
        if (!e) g_fi.files = uui_fileview_count(active());   // the folder itself: its items
        uui_fileinfo_set(&g_info, &g_fi);
    }
    g_info.preview = 0;
    char parent[UUI_FILEVIEW_PATH_MAX];
    if (e && !e->is_dir && g_opt.thumbs && k_path_dirname(path, parent, sizeof parent))
        g_info.preview = pane_thumb(0, parent, e, uui_fileinfo_preview_px(&g_info, g_dw));
}

// Where the buttons go: under whatever the pane drew.
static int buttons_y(void) {
    if (g_fi_key[0]) return g_dy + uui_fileinfo_height(&g_info, g_dw) + 4;
    return g_dy + PAD + (g_dw - 2 * PAD) * 3 / 5 + 10 + 3 * line_h() + 8 + 2 * line_h() + 10;
}

void details_layout(int x, int y, int w, int h) {
    g_dx = x; g_dy = y; g_dw = w; g_dh = h;
    refresh();
    uui_fileinfo_set_geometry(&g_info, x + 1, y, w - 1, uui_fileinfo_height(&g_info, w - 1));
    int bw, bh, bw2;
    uui_button_natural_size(&g_dp_open, &bw, &bh);
    uui_button_natural_size(&g_dp_props, &bw2, &bh);
    int by = buttons_y();
    uui_button_set_geometry(&g_dp_open, x + PAD, by, bw, bh);
    uui_button_set_geometry(&g_dp_props, x + PAD + bw + 6, by, bw2, bh);
    // Nothing to open when nothing is chosen.
    g_dp_open.disabled = uui_fileview_selected_name(active()) == 0;
    g_dp_props.disabled = g_dp_open.disabled;
}

// Several marked: their count, total size and folder.
static void draw_marked(struct ugfx_surface *s, uint32_t bg) {
    struct uui_fileview *fv = active();
    int marks = uui_fileview_mark_count(fv);
    int bx = g_dx + PAD, by = g_dy + PAD, bw = g_dw - 2 * PAD, bh = (g_dw - 2 * PAD) * 3 / 5;
    const struct uimg *ico = icon_get("file", bh * 2 / 3);
    if (ico) ugfx_blit_alpha(s, bx + (bw - ico->w) / 2, by + (bh - ico->h) / 2, ico->w, ico->h, ico->px, ico->w);
    char title[48], h[24], path[UUI_FILEVIEW_PATH_MAX];
    if (marks) snprintf(title, sizeof title, "%d items selected", marks);
    else snprintf(title, sizeof title, "Nothing selected");
    int y = by + bh + 10;
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    ugfx_draw_string_clipped(s, bx, y, bw, title, UTHEME_TEXT, bg);
    ugfx_set_font(was);
    y += 3 * line_h() + 8;
    unsigned long long bytes = 0;
    for (int i = 0; i < marks; i++) {
        struct sys_stat st;
        if (uui_fileview_marked_path(fv, i, path, sizeof path) &&
            !uui_fileview_marked_is_dir(fv, i) && sys_stat(path, &st) == 0)
            bytes += st.size;
    }
    human_size(h, sizeof h, bytes);
    uint32_t dim = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
    int kw = ugfx_char_advance('n') * 9;
    if (marks) {   // nothing selected has no size worth a "0B"
        ugfx_draw_string_clipped(s, bx, y, kw, "Size", dim, bg);
        ugfx_draw_string_clipped(s, bx + kw, y, bw - kw, h, UTHEME_TEXT, bg);
        y += line_h();
    }
    ugfx_draw_string_clipped(s, bx, y, kw, "Location", dim, bg);
    // A virtual folder by its name, not its "trash:/" spelling.
    const char *where = in_bin(fv) ? "Recycle Bin" : in_search(fv) ? "Search results"
                      : uui_fileview_dir(fv);
    ugfx_draw_string_elided(s, bx + kw, y, bw - kw, where, UTHEME_TEXT, bg);
}

void details_draw(struct ugfx_surface *s) {
    uint32_t bg = ugfx_blend(UTHEME_PANEL_BG, UTHEME_WHITE, 128);   // a side pane, a step lighter
    ugfx_fill_rect(s, g_dx, g_dy, g_dw, g_dh, bg);
    ugfx_fill_rect(s, g_dx, g_dy, 1, g_dh, UTHEME_BORDER);
    ugfx_set_clip_rect(s, g_dx, g_dy, g_dw, g_dh);
    if (g_fi_key[0]) uui_fileinfo_draw(s, &g_info);
    else draw_marked(s, bg);
    ugfx_clear_clip_rect(s);
}
