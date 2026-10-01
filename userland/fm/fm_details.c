// The details pane: a preview of the selection and its facts, down the
// right of the window -- Explorer's (Alt+Shift+P) and Dolphin's
// Information panel. Nothing selected, it describes the folder itself;
// several marked, their count and total.
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
#include "kpath.h"
#include <string.h>
#include <stdio.h>

static int g_dx, g_dy, g_dw, g_dh;   // the pane's rect, from layout_all()

#define PAD 12

int details_width(int cw) {
    int w = ugfx_char_advance('n') * 28;
    return w > cw / 3 ? cw / 3 : w;
}

static int preview_h(void) { return (g_dw - 2 * PAD) * 3 / 5; }
static int line_h(void)    { return ugfx_char_h() + 4; }

// How many fact rows details_draw() writes for what is selected now --
// its branches, counted, so the buttons sit right under the last one.
static int fact_rows(void) {
    struct uui_fileview *fv = active();
    const struct sys_dirent *e = uui_fileview_selected_entry(fv);
    const char *dir = uui_fileview_dir(fv);
    if (uui_fileview_mark_count(fv) > 1) return 2;          // Size, Location
    if (e) return e->is_dir ? 2 : 3;                        // (Size,) Modified, Location
    return (dir[0] == '/' && !dir[1]) ? 1 : 2;              // Items (, Location)
}

// Where the buttons go: under the preview, two name lines, the type and
// the facts.
static int buttons_y(void) {
    return g_dy + PAD + preview_h() + 10 + 2 * line_h() + line_h() + 8 +
           fact_rows() * line_h() + 10;
}

void details_layout(int x, int y, int w, int h) {
    g_dx = x; g_dy = y; g_dw = w; g_dh = h;
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

// A label and its value, one row. A value too long keeps its END --
// "...share/wallpapers" -- because the end of a path is the part that
// says where.
static int fact(struct ugfx_surface *s, int y, const char *k, const char *v, uint32_t bg) {
    int kw = ugfx_char_advance('n') * 9, vw = g_dw - 2 * PAD - kw;
    uint32_t dim = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
    ugfx_draw_string_clipped(s, g_dx + PAD, y, kw, k, dim, bg);
    char buf[80];
    if (ugfx_text_width(v) > vw) {
        const char *t = v;
        while (*t && ugfx_text_width(t) + ugfx_text_width("...") > vw) t++;
        snprintf(buf, sizeof buf, "...%s", t);
        v = buf;
    }
    ugfx_draw_string_clipped(s, g_dx + PAD + kw, y, vw, v, UTHEME_TEXT, bg);
    return y + line_h();
}

void details_draw(struct ugfx_surface *s) {
    uint32_t bg = ugfx_blend(UTHEME_PANEL_BG, UTHEME_WHITE, 128);
    ugfx_fill_rect(s, g_dx, g_dy, g_dw, g_dh, bg);
    ugfx_fill_rect(s, g_dx, g_dy, 1, g_dh, UTHEME_BORDER);
    ugfx_set_clip_rect(s, g_dx, g_dy, g_dw, g_dh);

    struct uui_fileview *fv = active();
    const struct sys_dirent *e = uui_fileview_selected_entry(fv);
    int marks = uui_fileview_mark_count(fv);
    const char *dir = uui_fileview_dir(fv);

    // --- the preview: the picture itself, else the type's icon ---------
    int bx = g_dx + PAD, by = g_dy + PAD, bw = g_dw - 2 * PAD, bh = preview_h();
    const struct uimg *pic = 0;
    if (e && !e->is_dir && marks <= 1 && g_opt.thumbs) pic = pane_thumb(0, dir, e, bh);
    if (pic) {
        int x = bx + (bw - pic->w) / 2, y = by + (bh - pic->h) / 2;
        if (pic->has_alpha) ugfx_blit_alpha(s, x, y, pic->w, pic->h, pic->px, pic->w);
        else                ugfx_blit(s, x, y, pic->w, pic->h, pic->px, pic->w);
        ugfx_draw_rect(s, x - 1, y - 1, pic->w + 2, pic->h + 2, UTHEME_BORDER);
    } else {
        int ipx = bh * 2 / 3;
        const char *name = e ? ufiletype_icon(e->name, e->is_dir) : "folder";
        const struct uimg *ico = icon_get(marks > 1 ? "file" : name, ipx);
        if (ico) ugfx_blit_alpha(s, bx + (bw - ico->w) / 2, by + (bh - ico->h) / 2,
                                 ico->w, ico->h, ico->px, ico->w);
    }

    // --- the name, bold, over two lines at most ------------------------
    char title[UUI_FILEVIEW_PATH_MAX + 24], type[40];
    if (marks > 1) {
        snprintf(title, sizeof title, "%d items selected", marks);
        strlcpy(type, "", sizeof type);
    } else if (e) {
        strlcpy(title, e->name, sizeof title);
        strlcpy(type, ufiletype_name(e->name, e->is_dir), sizeof type);
    } else {
        strlcpy(title, (dir[0] == '/' && !dir[1]) ? "System" : k_path_basename(dir), sizeof title);
        strlcpy(type, "Folder", sizeof type);
    }
    int y = by + bh + 10;
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    const char *rest = title;
    char line[UUI_FILEVIEW_PATH_MAX + 24];
    for (int n = 0; n < 2 && *rest; n++) {
        rest = uui_label_wrap_next(rest, bw, line, sizeof line);
        ugfx_draw_string_clipped(s, bx, y + n * line_h(), bw, line, UTHEME_TEXT, bg);
    }
    ugfx_set_font(was);
    y += 2 * line_h();
    ugfx_draw_string_clipped(s, bx, y, bw, type, uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED), bg);
    y += line_h() + 8;

    // --- the facts ------------------------------------------------------
    char v[64], h[24];
    if (marks > 1) {
        unsigned long long bytes = 0;
        char path[UUI_FILEVIEW_PATH_MAX];
        for (int i = 0; i < marks; i++) {
            struct sys_stat st;
            if (uui_fileview_marked_path(fv, i, path, sizeof path) &&
                !uui_fileview_marked_is_dir(fv, i) && sys_stat(path, &st) == 0)
                bytes += st.size;
        }
        human_size(h, sizeof h, bytes);
        y = fact(s, y, "Size", h, bg);
        y = fact(s, y, "Location", dir, bg);
    } else if (e) {
        if (!e->is_dir) {
            human_size(h, sizeof h, e->size);
            y = fact(s, y, "Size", h, bg);
        }
        udate_format(v, sizeof v, &e->modified, UDATE_DATE | UDATE_TIME);
        y = fact(s, y, "Modified", v, bg);
        y = fact(s, y, "Location", dir, bg);
    } else {
        snprintf(v, sizeof v, "%d", uui_fileview_count(fv));
        y = fact(s, y, "Items", v, bg);
        k_path_dirname(dir, v, sizeof v);
        if (!(dir[0] == '/' && !dir[1])) y = fact(s, y, "Location", v, bg);
    }
    ugfx_clear_clip_rect(s);
}
