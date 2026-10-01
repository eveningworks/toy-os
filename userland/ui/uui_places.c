// places -- the Places and Devices list. See ui/uui_places.h.
#include "ui/uui_places.h"
#include "ui/uui_widget.h"
#include "ui/uui_primitives.h"
#include "ui/uui_describe.h"
#include "ui/uui_scrollbar.h"
#include "ui/utheme.h"
#include "lib/icon_cache.h"
#include "lib/human.h"
#include "rt/sys.h"
#include "query_abi.h"
#include "keyboard.h"
#include <string.h>
#include <stdio.h>

// --- geometry: rows of two heights, a rule and a caption between groups --

static int place_h(void)  { return ugfx_char_h() + 10; }
static int device_h(void) { return 2 * ugfx_char_h() + 18; }
static int gap_h(void)    { return 9 + ugfx_char_h() + 6; }   // the rule, then "This computer"

static int row_h(const struct uui_places *p, int i) {
    return p->row[i].device ? device_h() : place_h();
}

static int content_h(const struct uui_places *p);

static int row_y(const struct uui_places *p, int i) {
    int y = p->y + 4 - p->scroll;
    for (int j = 0; j < i; j++) y += row_h(p, j);
    // The same condition content_h() and the draw use: no places, no
    // rule and caption above the devices.
    if (i >= p->places && p->count > p->places && p->places) y += gap_h();
    return y;
}

// Every row, the rule and the caption: what scrolls when a short window
// cannot show it all.
static int content_h(const struct uui_places *p) {
    int h = 8;
    for (int i = 0; i < p->count; i++) h += row_h(p, i);
    if (p->count > p->places && p->places) h += gap_h();
    return h;
}

static int max_scroll(const struct uui_places *p) {
    int m = content_h(p) - p->h;
    return m > 0 ? m : 0;
}

static int bar_w(const struct uui_places *p) {
    if (!max_scroll(p)) return 0;
    int w;
    uui_scrollbar_natural_size(&w, 0);
    return w;
}

int uui_places_row_rect(const struct uui_places *p, int i, int *x, int *y, int *w, int *h) {
    if (i < 0 || i >= p->count) return 0;
    if (x) *x = p->x + 4;
    if (y) *y = row_y(p, i);
    if (w) *w = p->w - 8 - bar_w(p);
    if (h) *h = row_h(p, i);
    return 1;
}

static int row_at(const struct uui_places *p, int cx, int cy) {
    for (int i = 0; i < p->count; i++) {
        int x, y, w, h;
        uui_places_row_rect(p, i, &x, &y, &w, &h);
        if (uui_hit(x, y, w, h, cx, cy)) return i;
    }
    return -1;
}

// --- the API ------------------------------------------------------------

void uui_places_init(struct uui_places *p) {
    memset(p, 0, sizeof *p);
    p->selected = p->hot = p->armed = -1;
}

void uui_places_add(struct uui_places *p, const char *label, const char *icon,
                    const char *path) {
    if (p->count >= UUI_PLACES_MAX || p->count != p->places) return;
    struct uui_place *r = &p->row[p->count++];
    memset(r, 0, sizeof *r);
    strlcpy(r->label, label, sizeof r->label);
    r->icon = icon;
    strlcpy(r->path, path, sizeof r->path);
    p->places = p->count;
}

static void device_name(const char *point, char *out, int cap) {
    if (!strcmp(point, "/"))     { strlcpy(out, "System", (size_t)cap); return; }
    if (!strcmp(point, "/boot")) { strlcpy(out, "Boot", (size_t)cap); return; }
    if (!strcmp(point, "/tmp"))  { strlcpy(out, "Temporary", (size_t)cap); return; }
    const char *base = point;
    for (const char *s = point; *s; s++) if (*s == '/' && s[1]) base = s + 1;
    strlcpy(out, base, (size_t)cap);
}

void uui_places_refresh(struct uui_places *p) {
    char keep[UUI_PLACES_PATH_MAX] = "";
    if (p->selected >= 0) strlcpy(keep, p->row[p->selected].path, sizeof keep);
    p->count = p->places;
    struct query_fsinfo fs;
    for (int i = 0; p->count < UUI_PLACES_MAX; i++) {
        int n = sys_query_record(QUERY_FSINFO, i, &fs, sizeof fs);
        if (n <= 0) break;
        if ((unsigned)n < sizeof fs || !(fs.flags & QUERY_FS_MOUNTED)) continue;
        struct uui_place *r = &p->row[p->count++];
        memset(r, 0, sizeof *r);
        device_name(fs.point, r->label, sizeof r->label);
        strlcpy(r->path, fs.point, sizeof r->path);
        r->device = 1;
        r->persistent = (fs.flags & QUERY_FS_PERSISTENT) != 0;
        r->readonly = (fs.flags & QUERY_FS_RDONLY) != 0;
        r->icon = r->persistent ? "drive" : "drive-ram";
        r->total = fs.total_bytes;
        r->used = fs.used_bytes;
    }
    if (p->hot >= p->count) p->hot = -1;
    uui_places_select_path(p, keep[0] ? keep : "");
}

void uui_places_select_path(struct uui_places *p, const char *dir) {
    p->selected = -1;
    for (int i = 0; i < p->count; i++)
        if (!strcmp(p->row[i].path, dir)) { p->selected = i; return; }
}

int uui_places_take(struct uui_places *p, char *out, int cap) {
    if (!p->has_taken) return 0;
    strlcpy(out, p->taken, (size_t)cap);
    p->has_taken = 0;
    return 1;
}

// --- drawing ------------------------------------------------------------

static uint32_t bg_of(const struct uui_places *p) {
    return p->bg ? p->bg : ugfx_blend(UTHEME_PANEL_BG, UTHEME_WHITE, 128);
}

// A DRIVE'S BAR IS COLOURED BY WHAT IT IS, so the three read apart at a
// glance -- and turns red when nearly full, Explorer's warning.
static uint32_t bar_colour(const struct uui_place *r) {
    if (r->total && r->used * 10 >= r->total * 9) return utheme_action(UTHEME_ACT_DANGER);
    if (!r->persistent) return utheme_action(UTHEME_ACT_EDIT);      // RAM: violet
    if (r->readonly)    return utheme_action(UTHEME_ACT_ARRANGE);   // read-only: orange
    return utheme_action(UTHEME_ACT_CREATE);                        // a disk: green
}

static void op_draw(struct ugfx_surface *s, const void *w) {
    const struct uui_places *p = w;
    uint32_t bg = bg_of(p), fg = UTHEME_TEXT;
    uint32_t dim = uui_state_bg(fg, UUI_STATE_DISABLED);
    ugfx_fill_rect(s, p->x, p->y, p->w, p->h, bg);
    ugfx_set_clip_rect(s, p->x, p->y, p->w, p->h);
    int px = ugfx_char_h();

    for (int i = 0; i < p->count; i++) {
        const struct uui_place *r = &p->row[i];
        int x, y, rw, rh;
        uui_places_row_rect(p, i, &x, &y, &rw, &rh);
        if (i == p->places && i > 0) {
            // The rule and the caption over the devices.
            int ry = y - gap_h();
            ugfx_fill_rect(s, x + 6, ry + 4, rw - 12, 1, UTHEME_SEPARATOR);
            const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
            ugfx_draw_string_clipped(s, x + 6, ry + 9, rw - 12, "This computer", dim, bg);
            ugfx_set_font(was);
        }
        uint32_t rbg = bg, rfg = fg;
        if (i == p->selected) rbg = UTHEME_SELECTION;
        else if (i == p->armed) rbg = uui_state_bg(bg, UUI_STATE_PRESSED);
        else if (i == p->hot) rbg = uui_state_bg(bg, UUI_STATE_HOVER);
        if (rbg != bg) ugfx_fill_rect(s, x, y, rw, rh, rbg);

        int line_y = y + (place_h() - px) / 2;
        const struct uimg *ico = r->icon ? icon_get(r->icon, px) : 0;
        if (ico) ugfx_blit_alpha(s, x + 8, line_y, ico->w, ico->h, ico->px, ico->w);
        int tx = x + 8 + px + 8;
        ugfx_draw_string_clipped(s, tx, line_y, x + rw - tx - 4, r->label, rfg, rbg);
        if (!r->device) continue;
        if (r->readonly) {
            // Beside the name, where it cannot be cut off with the sizes.
            int lw = ugfx_text_width(r->label), rw2 = ugfx_text_width("read-only");
            if (tx + lw + 8 + rw2 < x + rw - 4)
                ugfx_draw_string(s, x + rw - 4 - rw2, line_y, "read-only",
                                 i == p->selected ? rfg : dim, rbg);
        }

        // How full, as a bar and in words -- Explorer's This PC and
        // Dolphin's device rows both show both.
        int bar_y = line_y + px + 4, bar_w = x + rw - tx - 10;
        if (bar_w > 4 && r->total) {
            ugfx_fill_rect(s, tx, bar_y, bar_w, 4, ugfx_blend(rbg, UTHEME_BORDER, 90));
            uint64_t used = r->used > r->total ? r->total : r->used;
            int fill = (int)((uint64_t)bar_w * used / r->total);
            if (fill < 1 && used) fill = 1;
            ugfx_fill_rect(s, tx, bar_y, fill, 4, bar_colour(r));
        }
        char free_s[16], total_s[16], line[64];
        human_size(free_s, sizeof free_s, r->total > r->used ? r->total - r->used : 0);
        human_size(total_s, sizeof total_s, r->total);
        if (!r->persistent)
            snprintf(line, sizeof line, "%s free, not kept", free_s);
        else
            snprintf(line, sizeof line, "%s free of %s", free_s, total_s);
        ugfx_draw_string_clipped(s, tx, bar_y + 8, x + rw - tx - 4, line,
                                  i == p->selected ? rfg : dim, rbg);
    }
    ugfx_clear_clip_rect(s);
    int bw = bar_w(p);
    if (bw) uui_scrollbar_draw(s, p->x + p->w - bw, p->y, bw, p->h, content_h(p), p->h,
                               max_scroll(p) - p->scroll, bg,
                               uui_state_bg(bg, UUI_STATE_PRESSED), 0);
    if (p->focused && p->selected >= 0) {
        int x, y, rw, rh;
        uui_places_row_rect(p, p->selected, &x, &y, &rw, &rh);
        uui_focus_ring(s, x, y, rw, rh);
    }
}

// --- input ----------------------------------------------------------------

static void take_row(struct uui_places *p, int i) {
    strlcpy(p->taken, p->row[i].path, sizeof p->taken);
    p->has_taken = 1;
}

static int op_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_places *p = w;
    p->armed = row_at(p, cx, cy);
    return p->armed >= 0;
}

static int op_release(void *w, int cx, int cy) {
    struct uui_places *p = w;
    int i = p->armed;
    p->armed = -1;
    if (i >= 0 && row_at(p, cx, cy) == i) take_row(p, i);
    return 1;
}

static int op_motion(void *w, int cx, int cy, unsigned buttons) {
    (void)buttons;
    struct uui_places *p = w;
    int i = row_at(p, cx, cy);
    if (i == p->hot) return 0;
    p->hot = i;
    return 1;
}

// Up and Down go to the next place, as arrowing through any sidebar does.
static int op_key(void *w, int key, unsigned mods) {
    (void)mods;
    struct uui_places *p = w;
    if (!p->count) return 0;
    int i = p->selected;
    if (key == KEY_ARROW_DOWN) i = i < 0 ? 0 : (i + 1 < p->count ? i + 1 : i);
    else if (key == KEY_ARROW_UP) i = i <= 0 ? 0 : i - 1;
    else if ((key == '\n' || key == '\r') && i >= 0) { take_row(p, i); return 1; }
    else return 0;
    if (i == p->selected) return 1;
    p->selected = i;
    take_row(p, i);
    return 1;
}

static int op_wheel(void *w, int notches) {
    struct uui_places *p = w;
    int was = p->scroll;
    p->scroll -= notches * place_h() * 2;
    if (p->scroll > max_scroll(p)) p->scroll = max_scroll(p);
    if (p->scroll < 0) p->scroll = 0;
    return p->scroll != was;
}

static int op_accepts_focus(const void *w) { return ((const struct uui_places *)w)->count > 0; }
static void op_set_focused(void *w, int on) { ((struct uui_places *)w)->focused = on; }

static void op_natural(const void *w, int *out_w, int *out_h) {
    const struct uui_places *p = w;
    if (out_w) *out_w = ugfx_char_advance('n') * 20;
    if (out_h) *out_h = content_h(p);
}

static void op_geometry(void *w, int x, int y, int width, int height) {
    struct uui_places *p = w;
    p->x = x; p->y = y; p->w = width; p->h = height;
    if (p->scroll > max_scroll(p)) p->scroll = max_scroll(p);
}

static void op_bounds(const void *w, int *x, int *y, int *bw, int *bh) {
    const struct uui_places *p = w;
    if (x) *x = p->x;
    if (y) *y = p->y;
    if (bw) *bw = p->w;
    if (bh) *bh = p->h;
}

static int op_hit(const void *w, int cx, int cy) {
    const struct uui_places *p = w;
    return uui_hit(p->x, p->y, p->w, p->h, cx, cy);
}

static void op_describe(const void *w, const struct uui_describe *d) {
    const struct uui_places *p = w;
    for (int i = 0; i < p->count; i++) {
        int x, y, rw, rh;
        uui_places_row_rect(p, i, &x, &y, &rw, &rh);
        uui_describe_rect_i(d, "row", i, x, y, rw, rh);
    }
    uui_describe_int(d, "selected", p->selected);
}

const struct uui_widget_ops uui_places_ops = {
    .natural_size  = op_natural,
    .set_geometry  = op_geometry,
    .bounds        = op_bounds,
    .draw          = op_draw,
    .hit           = op_hit,
    .press         = op_press,
    .motion        = op_motion,
    .release       = op_release,
    .wheel         = op_wheel,
    .key           = op_key,
    .accepts_focus = op_accepts_focus,
    .set_focused   = op_set_focused,
    .describe      = op_describe,
};
