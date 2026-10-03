// See uui_fileinfo.h.
#include "ui/uui_fileinfo.h"
#include <stdio.h>
#include <string.h>
#include "ui/utheme.h"
#include "ui/uui_widget.h"
#include "ui/uui_primitives.h"
#include "lib/udate.h"
#include "lib/human.h"
#include "lib/icon_cache.h"

static int pad(void)  { return utheme_pad(); }
static int lh(void)   { return ugfx_char_h() + utheme_gap(); }
static int hdr_h(void) { return lh() + pad(); }
static int key_w(int compact) { int p = ugfx_char_advance('n'); return (p > 0 ? p : 8) * (compact ? 8 : 11); }
static int icon_px(const struct uui_fileinfo *w) { return w->compact ? 48 : 64; }

// --- building the sections from the facts ------------------------------------

static struct uui_fi_section *find(struct uui_fileinfo *w, const char *title) {
    for (int i = 0; i < w->nsec; i++)
        if (!strcmp(w->sec[i].title, title)) return &w->sec[i];
    return 0;
}

static struct uui_fi_section *add(struct uui_fileinfo *w, const char *title, int role, int open,
                                  const struct uui_fi_section *old, int nold) {
    if (w->nsec >= UUI_FI_SECTIONS) return 0;
    struct uui_fi_section *s = &w->sec[w->nsec++];
    memset(s, 0, sizeof *s);
    strlcpy(s->title, title, sizeof s->title);
    s->role = role;
    s->open = open;
    for (int i = 0; i < nold; i++)
        if (!strcmp(old[i].title, title)) {
            s->open = old[i].open;
            s->slot_id = old[i].slot_id;
            s->slot_h = old[i].slot_h;
        }
    return s;
}

static void row(struct uui_fi_section *s, const char *k, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#include <stdarg.h>
static void row(struct uui_fi_section *s, const char *k, const char *fmt, ...) {
    if (!s || s->nrows >= UUI_FI_ROWS) return;
    strlcpy(s->key[s->nrows], k, sizeof s->key[0]);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->val[s->nrows], UUI_FI_VALUE, fmt, ap);
    va_end(ap);
    s->nrows++;
}

static void when(char *out, int cap, const struct rtc_time *t) {
    udate_format(out, cap, t, UDATE_DATE | UDATE_TIME | UDATE_SECONDS);
}

void uui_fileinfo_set(struct uui_fileinfo *w, const struct ufileinfo *fi) {
    static struct uui_fi_section old[UUI_FI_SECTIONS];
    int nold = w->nsec;
    memcpy(old, w->sec, sizeof old);
    w->fi = fi;
    w->nsec = 0;
    char size[48], t[48], vol[64] = "";
    ufileinfo_size_text(fi, size, sizeof size);
    if (fi->vol_point[0]) {
        char fr[16], tot[16];
        human_size(fr, sizeof fr, fi->vol_total - fi->vol_used);
        human_size(tot, sizeof tot, fi->vol_total);
        snprintf(vol, sizeof vol, "%s (%s): %s free of %s", fi->vol_point, fi->vol_fs, fr, tot);
    }

    // The summary under the name: what it is, how big, and its one
    // telling measure.
    char h[16];
    human_size(h, sizeof h, fi->st.is_dir ? fi->bytes : fi->st.size);
    if (!fi->ok) snprintf(w->subtitle, sizeof w->subtitle, "%s -- cannot be read", fi->type);
    else if (fi->st.is_dir && w->compact)
        snprintf(w->subtitle, sizeof w->subtitle, fi->files ? "Folder, %d items" : "Folder", fi->files);
    else if (fi->st.is_dir)
        snprintf(w->subtitle, sizeof w->subtitle, "Folder, %d file%s, %d folder%s%s", fi->files,
                 fi->files == 1 ? "" : "s", fi->dirs, fi->dirs == 1 ? "" : "s", fi->walking ? ", counting..." : "");
    else if (fi->img_w)
        snprintf(w->subtitle, sizeof w->subtitle, "%s, %s, %d x %d", fi->type, h, fi->img_w, fi->img_h);
    else if (fi->has_tags && fi->artist[0])
        snprintf(w->subtitle, sizeof w->subtitle, "%s, %s, %s", fi->type, h, fi->artist);
    else
        snprintf(w->subtitle, sizeof w->subtitle, "%s, %s", fi->type, h);

    struct uui_fi_section *g = add(w, "General", UTHEME_ACT_NAV, 1, old, nold);
    if (!fi->ok) { row(g, "Where", "%s", fi->dir); return; }
    if (w->compact) {
        // The compact form walks nothing: a folder's count is what its
        // caller listed (the File Manager's own items), size only for a file.
        if (!fi->st.is_dir) row(g, "Size", "%s", size);
        when(t, sizeof t, &fi->st.modified);
        row(g, "Modified", "%s", t);
        row(g, "Where", "%s", fi->dir);
        return;
    }
    row(g, "Where", "%s", fi->dir[0] ? fi->dir : "/");
    row(g, "Size", "%s", size);
    if (fi->st.is_dir)
        row(g, "Contains", "%d file%s, %d folder%s%s", fi->files, fi->files == 1 ? "" : "s",
            fi->dirs, fi->dirs == 1 ? "" : "s", fi->walking ? " (counting...)" : "");
    when(t, sizeof t, &fi->st.created);
    row(g, "Created", "%s", t);
    when(t, sizeof t, &fi->st.modified);
    row(g, "Modified", "%s", t);
    if (vol[0] && !fi->st.is_dir) row(g, "Volume", "%s", vol);

    if (fi->img_w) {
        struct uui_fi_section *s = add(w, "Image", UTHEME_ACT_VIEW, 1, old, nold);
        row(s, "Dimensions", "%d x %d pixels", fi->img_w, fi->img_h);
        row(s, "Format", "%s%s%s", fi->img_format, fi->img_detail[0] ? ", " : "", fi->img_detail);
    } else if (fi->has_tags) {
        struct uui_fi_section *s = add(w, "Audio", UTHEME_ACT_MEDIA, 1, old, nold);
        if (fi->title[0]) row(s, "Title", "%s", fi->title);
        if (fi->artist[0]) row(s, "Artist", "%s", fi->artist);
        if (fi->album[0]) row(s, "Album", "%s", fi->album);
        if (fi->length_ms) row(s, "Length", "%u:%02u", fi->length_ms / 60000, (fi->length_ms / 1000) % 60);
    }
    // Slot-only sections an app added before stay where they were...
    for (int i = 0; i < nold; i++)
        if (old[i].slot_id && !find(w, old[i].title))
            add(w, old[i].title, old[i].role, old[i].open, old, nold);
    // ...and Details, the advanced facts, comes last.
    struct uui_fi_section *d = add(w, "Details", UTHEME_ACT_NONE, 0, old, nold);
    row(d, "Inode", "%llu%s", (unsigned long long)fi->st.ino, (fi->st.flags & SYS_STAT_INODES) ? "" : " (synthetic)");
    row(d, "Links", "%u", fi->st.nlink);
}

int uui_fileinfo_slot(struct uui_fileinfo *w, const char *title, int role, int id, int h, int open) {
    struct uui_fi_section *s = find(w, title);
    if (!s) s = add(w, title, role, open, 0, 0);
    if (!s) return -1;
    s->slot_id = id;
    s->slot_h = h;
    return 0;
}

int uui_fileinfo_open(struct uui_fileinfo *w, const char *title, int open) {
    struct uui_fi_section *s = find(w, title);
    if (!s) return -1;
    s->open = open;
    return 0;
}

int uui_fileinfo_take_toggle(struct uui_fileinfo *w) {
    int t = w->toggled;
    w->toggled = 0;
    return t;
}

// --- geometry ----------------------------------------------------------------

static int stage_pic_h(const struct uui_fileinfo *w, int width);

int uui_fileinfo_preview_px(const struct uui_fileinfo *w, int width) {
    int aw = width - 2 * pad(), ah = stage_pic_h(w, width);
    const struct ufileinfo *fi = w->fi;
    if (!fi || !fi->img_w || !fi->img_h) return ah;
    // The longest edge that fits the stage at this picture's own shape.
    int long_edge = fi->img_w > fi->img_h ? fi->img_w : fi->img_h;
    int by_w = (int)((long long)aw * long_edge / fi->img_w);
    int by_h = (int)((long long)ah * long_edge / fi->img_h);
    return by_w < by_h ? by_w : by_h;
}

static int stage_pic_h(const struct uui_fileinfo *w, int width) {
    int h = (width - 2 * pad()) * (w->compact ? 3 : 9) / (w->compact ? 5 : 16);
    int cap = ugfx_char_h() * 11;
    return h > cap ? cap : h;
}

// A picture's stage is there before its thumbnail arrives, so the
// window's first size already holds it.
static int staged(const struct uui_fileinfo *w) { return w->preview || (w->fi && w->fi->img_w); }

static int hero_h(const struct uui_fileinfo *w, int width) {
    if (staged(w)) return pad() + stage_pic_h(w, width) + pad() + 2 * lh() + pad();
    int text = 2 * lh() + ((w->fi && w->fi->st.is_dir && w->fi->vol_point[0]) ? lh() + pad() : 0);
    int ic = icon_px(w);
    return 2 * pad() + (ic > text ? ic : text);
}

static int section_h(const struct uui_fileinfo *w, const struct uui_fi_section *s) {
    if (w->compact) return s->nrows * lh() + pad();
    if (!s->open) return hdr_h();
    return hdr_h() + s->slot_h + s->nrows * lh() + pad();
}

int uui_fileinfo_height(const struct uui_fileinfo *w, int width) {
    int h = hero_h(w, width);
    int n = w->compact ? (w->nsec > 0) : w->nsec;
    for (int i = 0; i < n; i++) h += section_h(w, &w->sec[i]);
    return h;
}

static int section_y(const struct uui_fileinfo *w, int i) {
    int y = w->y - w->scroll + hero_h(w, w->w);
    for (int k = 0; k < i; k++) y += section_h(w, &w->sec[k]);
    return y;
}

int uui_fileinfo_slot_rect(const struct uui_fileinfo *w, int id, int *x, int *y, int *ww, int *hh) {
    if (w->compact) return 0;
    for (int i = 0; i < w->nsec; i++) {
        const struct uui_fi_section *s = &w->sec[i];
        if (s->slot_id != id) continue;
        if (!s->open || !s->slot_h) return 0;
        int sy = section_y(w, i) + hdr_h();
        // Any part in view counts: the app hides each control that is not
        // (a whole-slot test hid a tall slot entirely at the window's foot).
        if (sy >= w->y + w->h || sy + s->slot_h <= w->y) return 0;
        *x = w->x + 3 * pad();
        *y = sy;
        *ww = w->w - 4 * pad();
        *hh = s->slot_h;
        return 1;
    }
    return 0;
}

int uui_fileinfo_header_rect(const struct uui_fileinfo *w, const char *title, int *x, int *y, int *ww, int *hh) {
    if (w->compact) return 0;
    for (int i = 0; i < w->nsec; i++)
        if (!strcmp(w->sec[i].title, title)) {
            *x = w->x; *y = section_y(w, i); *ww = w->w; *hh = hdr_h();
            return *y >= w->y && *y + *hh <= w->y + w->h;
        }
    return 0;
}

void uui_fileinfo_natural_size(const struct uui_fileinfo *w, int *out_w, int *out_h) {
    int per = ugfx_char_advance('n');
    int width = (per > 0 ? per : 8) * 44;
    if (out_w) *out_w = width;
    if (out_h) *out_h = uui_fileinfo_height(w, width);
}

static int max_scroll(const struct uui_fileinfo *w) {
    int m = uui_fileinfo_height(w, w->w) - w->h;
    return m > 0 ? m : 0;
}

static void clamp(struct uui_fileinfo *w) {
    if (w->scroll > max_scroll(w)) w->scroll = max_scroll(w);
    if (w->scroll < 0) w->scroll = 0;
}

void uui_fileinfo_set_geometry(struct uui_fileinfo *w, int x, int y, int width, int height) {
    w->x = x; w->y = y; w->w = width; w->h = height;
    clamp(w);
}

// --- drawing -----------------------------------------------------------------

static void chevron(struct ugfx_surface *s, int x, int y, int open, uint32_t c) {
    int r = ugfx_char_h() / 4 + 1;
    if (open) {
        ugfx_draw_line(s, x, y - r / 2, x + r, y + r / 2, c, GEOM_AA);
        ugfx_draw_line(s, x + r, y + r / 2, x + 2 * r, y - r / 2, c, GEOM_AA);
    } else {
        ugfx_draw_line(s, x + r / 2, y - r, x + r + r / 2, y, c, GEOM_AA);
        ugfx_draw_line(s, x + r + r / 2, y, x + r / 2, y + r, c, GEOM_AA);
    }
}

// A value too long keeps its END for a path ("...share/wallpapers": the
// end says where), its start otherwise.
static void value(struct ugfx_surface *s, int x, int y, int vw, const char *k, const char *v,
                  uint32_t fg, uint32_t bg) {
    char buf[UUI_FI_VALUE + 4];
    if (!strcmp(k, "Where") && ugfx_text_width(v) > vw) {
        const char *t = v;
        while (*t && ugfx_text_width(t) + ugfx_text_width("...") > vw) t++;
        snprintf(buf, sizeof buf, "...%s", t);
        v = buf;
    }
    ugfx_draw_string_elided(s, x, y, vw, v, fg, bg);
}

static void draw_hero(struct ugfx_surface *s, const struct uui_fileinfo *w) {
    int Y = w->y - w->scroll;   // the hero scrolls with everything else
    const struct ufileinfo *fi = w->fi;
    const struct utheme *t = utheme_current();
    int p = pad(), hh = hero_h(w, w->w);
    const char *name = fi ? fi->name : "";
    if (staged(w)) {
        struct uui_fileinfo *mw = (struct uui_fileinfo *)w;   // the cached tint only
        if (mw->amb_for != w->preview) { uambient_from(&mw->amb, w->preview); mw->amb_for = w->preview; }
        uambient_paint(w->stage, &w->amb, s, w->x, Y, w->w, hh);
        int ah = stage_pic_h(w, w->w), aw = w->w - 2 * p;
        const struct uimg *im = w->preview;
        if (im) {
            int iw = im->w < aw ? im->w : aw, ih = im->h < ah ? im->h : ah;
            int ix = w->x + (w->w - iw) / 2, iy = Y + p + (ah - ih) / 2;
            if (im->has_alpha) ugfx_blit_alpha(s, ix, iy, iw, ih, im->px, im->w);
            else ugfx_blit(s, ix, iy, iw, ih, im->px, im->w);
        }
        int ty = Y + p + ah + p;
        const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
        int nw = ugfx_text_width(name);
        if (nw > aw) nw = aw;
        ugfx_draw_string_elided(s, w->x + (w->w - nw) / 2, ty, aw, name, 0xFFFFFFu, w->amb.centre);
        ugfx_set_font(was);
        int sw = ugfx_text_width(w->subtitle);
        if (sw > aw) sw = aw;
        ugfx_draw_string_elided(s, w->x + (w->w - sw) / 2, ty + lh(), aw, w->subtitle,
                                ugfx_blend(w->amb.centre, 0xFFFFFFu, 200), w->amb.centre);
        return;
    }
    uint32_t bg = ugfx_blend(t->panel_bg, t->field_bg, 140);
    ugfx_fill_rect(s, w->x, Y, w->w, hh, bg);
    ugfx_fill_rect(s, w->x, Y + hh - 1, w->w, 1, t->separator);
    int ic = icon_px(w);
    const struct uimg *icon = icon_get(fi && fi->icon ? fi->icon : "file", ic);
    if (icon) ugfx_blit_alpha(s, w->x + p * 2, Y + (hh - icon->h) / 2, icon->w, icon->h, icon->px, icon->w);
    int tx = w->x + 3 * p + ic, tw = w->w - (tx - w->x) - 2 * p;
    int blk = 2 * lh() + ((fi && fi->st.is_dir && fi->vol_point[0]) ? lh() + p : 0);
    int ty = Y + (hh - blk) / 2;
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    ugfx_draw_string_elided(s, tx, ty, tw, name, t->text, bg);
    ugfx_set_font(was);
    ugfx_draw_string_elided(s, tx, ty + lh(), tw, w->subtitle, uui_state_bg(t->text, UUI_STATE_DISABLED), bg);
    if (fi && fi->st.is_dir && fi->vol_point[0] && fi->vol_total) {
        // The volume as a bar coloured by state -- danger near full.
        int by = ty + 2 * lh() + p / 2, bh = p / 2 + 2;
        int used = (int)((fi->vol_used * (uint64_t)tw) / fi->vol_total);
        if (used < 1 && fi->vol_used) used = 1;
        uint32_t c = fi->vol_used * 10 > fi->vol_total * 9 ? utheme_action(UTHEME_ACT_DANGER) : t->accent;
        uui_fill_round_rect(s, tx, by, tw, bh, bh / 2, t->separator);
        if (used) uui_fill_round_rect(s, tx, by, used, bh, bh / 2, c);
        char fr[16], tot[16], line[80];
        human_size(fr, sizeof fr, fi->vol_total - fi->vol_used);
        human_size(tot, sizeof tot, fi->vol_total);
        snprintf(line, sizeof line, "%s (%s): %s free of %s", fi->vol_point, fi->vol_fs, fr, tot);
        ugfx_draw_string_elided(s, tx, by + bh + 2, tw, line, uui_state_bg(t->text, UUI_STATE_DISABLED), bg);
    }
}

// The page's ground: the panel, or -- compact, as a side pane -- a step
// lighter, the design language's side pane (docs/gui-guidelines.md).
static uint32_t ground(const struct uui_fileinfo *w) {
    const struct utheme *t = utheme_current();
    return w->compact ? ugfx_blend(t->panel_bg, t->field_bg, 128) : t->panel_bg;
}

void uui_fileinfo_draw(struct ugfx_surface *s, const struct uui_fileinfo *w) {
    const struct utheme *t = utheme_current();
    uint32_t bg = ground(w), dim = uui_state_bg(t->text, UUI_STATE_DISABLED);
    ugfx_set_clip_rect(s, w->x, w->y, w->w, w->h);
    ugfx_fill_rect(s, w->x, w->y, w->w, w->h, bg);
    draw_hero(s, w);
    int p = pad(), n = w->compact ? (w->nsec > 0) : w->nsec;
    for (int i = 0; i < n; i++) {
        const struct uui_fi_section *sec = &w->sec[i];
        int y = section_y(w, i);
        if (!w->compact) {
            if (i > 0 || !w->preview) ugfx_fill_rect(s, w->x, y, w->w, 1, t->separator);
            if (i == w->hot) uui_fill_round_rect(s, w->x + p / 2, y + 2, w->w - p, hdr_h() - 4, p / 2,
                                                 uui_state_bg(bg, UUI_STATE_HOVER));
            uint32_t hb = i == w->hot ? uui_state_bg(bg, UUI_STATE_HOVER) : bg;
            int cy = y + hdr_h() / 2;
            chevron(s, w->x + p + 2, cy, sec->open, dim);
            const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
            uint32_t c = sec->role ? utheme_action(sec->role) : dim;
            ugfx_draw_string_clipped(s, w->x + 3 * p, cy - ugfx_char_h() / 2, w->w - 4 * p, sec->title, c, hb);
            ugfx_set_font(was);
            if (!sec->open) continue;
            y += hdr_h() + sec->slot_h;
        } else {
            y += p / 2;
        }
        int lx = w->x + (w->compact ? 2 * p : 3 * p), vx = lx + key_w(w->compact), vw = w->x + w->w - p - vx;
        for (int r = 0; r < sec->nrows; r++, y += lh()) {
            ugfx_draw_string_clipped(s, lx, y, key_w(w->compact) - p, sec->key[r], dim, bg);
            value(s, vx, y, vw, sec->key[r], sec->val[r], t->text, bg);
        }
    }
    // Taller than the window: a slim thumb at the right edge says so.
    int total = uui_fileinfo_height(w, w->w);
    if (total > w->h && w->h > 0) {
        int th = w->h * w->h / total, ty = w->y + (int)((long long)w->scroll * (w->h - th) / (total - w->h));
        uui_fill_round_rect(s, w->x + w->w - 5, ty, 3, th, 1, uui_state_bg(t->text, UUI_STATE_DISABLED));
    }
    ugfx_clear_clip_rect(s);
}

// --- input: a section header opens and closes (on release) -------------------

static int header_at(const struct uui_fileinfo *w, int cx, int cy) {
    if (w->compact || cx < w->x || cx >= w->x + w->w) return -1;
    for (int i = 0; i < w->nsec; i++) {
        int y = section_y(w, i);
        if (cy >= y && cy < y + hdr_h()) return i;
    }
    return -1;
}

static void fi_natural(const void *w, int *ow, int *oh) { uui_fileinfo_natural_size(w, ow, oh); }
static void fi_geom(void *w, int x, int y, int ww, int hh) { uui_fileinfo_set_geometry(w, x, y, ww, hh); }
static void fi_bounds(const void *v, int *x, int *y, int *ww, int *hh) {
    const struct uui_fileinfo *w = v;
    *x = w->x; *y = w->y; *ww = w->w; *hh = w->h;
}
static void fi_draw(struct ugfx_surface *s, const void *w) { uui_fileinfo_draw(s, w); }
static int fi_hit(const void *v, int cx, int cy) {
    const struct uui_fileinfo *w = v;
    return cx >= w->x && cx < w->x + w->w && cy >= w->y && cy < w->y + w->h;
}
static int fi_press(void *v, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_fileinfo *w = v;
    w->armed = header_at(w, cx, cy);
    return w->armed >= 0;
}
static int fi_motion(void *v, int cx, int cy, unsigned buttons) {
    (void)buttons;
    struct uui_fileinfo *w = v;
    int h = header_at(w, cx, cy);
    if (h == w->hot) return 0;
    w->hot = h;
    return 1;
}
static int fi_release(void *v, int cx, int cy) {
    struct uui_fileinfo *w = v;
    int h = header_at(w, cx, cy), was = w->armed;
    w->armed = -1;
    if (h < 0 || h != was) return was >= 0;
    w->sec[h].open = !w->sec[h].open;
    w->toggled = 1;
    clamp(w);
    // Opened near the bottom: scroll it into view, header first.
    if (w->sec[h].open) {
        int bottom = section_y(w, h) + section_h(w, &w->sec[h]);
        if (bottom > w->y + w->h) w->scroll += bottom - (w->y + w->h);
        int top = section_y(w, h);
        if (top < w->y) w->scroll -= w->y - top;
        clamp(w);
    }
    return 1;
}

static int fi_wheel(void *v, int notches) {
    struct uui_fileinfo *w = v;
    int was = w->scroll;
    w->scroll -= notches * 3 * lh();
    clamp(w);
    return w->scroll != was;
}

const struct uui_widget_ops uui_fileinfo_ops = {
    .natural_size = fi_natural,
    .set_geometry = fi_geom,
    .bounds       = fi_bounds,
    .draw         = fi_draw,
    .hit          = fi_hit,
    .press        = fi_press,
    .motion       = fi_motion,
    .release      = fi_release,
    .wheel        = fi_wheel,
};

void uui_fileinfo_init(struct uui_fileinfo *w) {
    memset(w, 0, sizeof *w);
    w->hot = w->armed = -1;
    w->stage = &w->stage_store;
    uambient_default(&w->amb);
}
