// A Markdown document, drawn as a document -- see ui/uui_markdown.h for
// the split with lib/umd.h, which owns what Markdown MEANS.
#include "ui/uui_markdown.h"
#include "ui/uui_widget.h"
#include "ui/uui_primitives.h"
#include "ui/utheme.h"
#include "lib/umd.h"
#include <stdlib.h>

// The faces this loads for itself. The session fonts cover body text and
// bold; what they cannot give is a bigger heading or a real monospace.
enum { F_H1, F_H2, F_MONO };

#define HEAD_FONT "/usr/share/fonts/liberation-sans-bold.ttf"
#define MONO_FONT "/usr/share/fonts/dejavu-sans-mono.ttf"

// A word being built. Long enough for any word in prose and for a URL;
// an overlong token is flushed early rather than truncated, so it runs
// on instead of losing its tail -- lib/umd.c's e_ch makes the same call.
#define WORD_CAP 128

// One walk over the document does BOTH jobs: it always measures, and it
// draws only when it has a surface. Two walks would be two ideas of how
// tall a heading is, and the scrollbar would disagree with the page.
struct md_ctx {
    struct uui_markdown *m;
    struct ugfx_surface *s;   // NULL while measuring

    int left, right;          // the text column, in document coordinates
    int pen_x, pen_y;         // pen_y is DOCUMENT space
    int ox, oy;               // added at the moment of drawing, so one
                              // walk can measure (offset irrelevant) and
                              // draw (offset by the scroll)
    int hang;                 // continuation indent for the current block
    int open;                 // a line has been started and not yet ended

    char w[WORD_CAP];
    unsigned st[WORD_CAP];
    int wn;
    int w_px;                 // the pending word's width

    // The heading level being flowed, 0 for body text. IN THE CONTEXT
    // rather than a file static: two of these widgets in one app would
    // otherwise share it, and a static that is only safe because two
    // walks never interleave is a trap waiting for the day one does.
    int level;
};

// --- fonts ------------------------------------------------------------

static void load_face(struct uui_markdown_face *f, const char *path, int px, int bold) {
    unsigned long need = ugfx_font_arena_size(px);
    f->arena = malloc(need);
    if (!f->arena) return;
    f->ok = ugfx_font_load(path, px, bold, &f->font, f->arena, need);
    if (!f->ok) { free(f->arena); f->arena = 0; }
}

// ONCE, and lazily: the faces need ugfx_font_init() to have run, which
// uapp does at startup but a widget cannot assume at init() time.
static void ensure_faces(struct uui_markdown *m) {
    if (m->faces_tried) return;
    m->faces_tried = 1;
    int base = ugfx_char_h();
    if (base < 8) return;                  // no font yet; try again never
    load_face(&m->face[F_H1], HEAD_FONT, base * 9 / 5, 1);
    load_face(&m->face[F_H2], HEAD_FONT, base * 7 / 5, 1);
    load_face(&m->face[F_MONO], MONO_FONT, base, 0);
}

static const struct ugfx_font *face_or(struct uui_markdown *m, int i,
                                        const struct ugfx_font *fallback) {
    return m->face[i].ok ? &m->face[i].font : fallback;
}

// The face a style asks for. A failed load falls back to the session
// bold, so a machine with no fonts on disk still renders a document
// whose headings are at least distinguishable.
static const struct ugfx_font *style_font(struct uui_markdown *m, unsigned style, int level) {
    const struct ugfx_font *bold = ugfx_font_session(UGFX_FONT_BOLD);
    if (level == 1) return face_or(m, F_H1, bold);
    if (level == 2) return face_or(m, F_H2, bold);
    if (level >= 3) return bold;
    if (style & UMD_STYLE_CODE) return face_or(m, F_MONO, ugfx_font_session(UGFX_FONT_REGULAR));
    if (style & UMD_STYLE_BOLD) return bold;
    return ugfx_font_session(UGFX_FONT_REGULAR);
}

static int font_h(const struct ugfx_font *f) {
    const struct ugfx_font *was = ugfx_set_font(f);
    int h = ugfx_char_h();
    ugfx_set_font(was);
    return h;
}

static int char_px(const struct ugfx_font *f, char c) {
    const struct ugfx_font *was = ugfx_set_font(f);
    int a = ugfx_char_advance(c);
    ugfx_set_font(was);
    return a;
}

// --- drawing primitives ----------------------------------------------

// Code is drawn on a tinted ground, the way every Markdown renderer
// marks it -- derived from the theme's panel colour rather than picked,
// so it follows a theme change (docs/gui-guidelines.md).
static uint32_t code_bg(void) { return UTHEME_PANEL_BG; }

static void put_run(struct md_ctx *c, int x, int y, const char *txt,
                     const unsigned *st, int n, int level) {
    if (!c->s) return;
    x += c->ox;
    y += c->oy;
    for (int i = 0; i < n; i++) {
        const struct ugfx_font *f = style_font(c->m, st[i], level);
        int adv = char_px(f, txt[i]);
        int line_h = font_h(f);
        if (st[i] & UMD_STYLE_CODE)
            ugfx_fill_rect(c->s, x, y, adv, line_h, code_bg());
        const struct ugfx_font *was = ugfx_set_font(f);
        ugfx_draw_char(c->s, x, y, txt[i], UTHEME_TEXT,
                        (st[i] & UMD_STYLE_CODE) ? code_bg() : UTHEME_WHITE);
        ugfx_set_font(was);
        x += adv;
    }
}

// --- the paragraph flow ------------------------------------------------
//
// The pixel twin of lib/umd.c's e_word: a word is buffered, measured,
// and placed -- wrapping when it does not fit. Kept here rather than in
// umd because WHERE a line breaks is a property of the renderer's
// medium, and umd's is columns.

static int line_height(struct md_ctx *c) {
    return font_h(style_font(c->m, 0, c->level)) + (c->level ? 2 : 1);
}

static void flush_word(struct md_ctx *c, int space_first) {
    if (c->wn == 0) return;
    int space = 0;
    if (c->open && space_first)
        space = char_px(style_font(c->m, 0, c->level), ' ');
    if (c->open && c->pen_x + space + c->w_px > c->right) {
        c->pen_y += line_height(c);
        c->pen_x = c->hang;
        space = 0;
    } else if (!c->open) {
        c->open = 1;
        space = 0;
    }
    put_run(c, c->pen_x + space, c->pen_y, c->w, c->st, c->wn, c->level);
    c->pen_x += space + c->w_px;
    c->wn = 0;
    c->w_px = 0;
}

static void md_sink(void *ctx, char ch, unsigned style) {
    struct md_ctx *c = (struct md_ctx *)ctx;
    if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
        flush_word(c, 1);
        return;
    }
    // An overlong token is placed as a CONTINUATION rather than dropped.
    if (c->wn >= WORD_CAP - 1) flush_word(c, 1);
    c->w[c->wn] = ch;
    c->st[c->wn] = style;
    c->wn++;
    c->w_px += char_px(style_font(c->m, style, c->level), ch);
}

// Ends the current flowed block and leaves the pen on a fresh line.
static void end_block(struct md_ctx *c) {
    flush_word(c, 1);
    if (c->open) c->pen_y += line_height(c);
    c->open = 0;
    c->pen_x = c->left;
    c->hang = c->left;
}

static void feed(struct md_ctx *c, const char *s, int n) {
    umd_inline_walk(s, n, md_sink, c);
    flush_word(c, 1);   // the LINE BREAK IS WHITESPACE (lib/umd.c says why)
}

// --- the document walk -------------------------------------------------

static void hrule(struct md_ctx *c, int strong) {
    if (!c->s) return;
    ugfx_fill_rect(c->s, c->ox + c->left, c->oy + c->pen_y, c->right - c->left,
                    strong ? 2 : 1, UTHEME_BORDER);
}

// Walks every block, measuring always and drawing when `s` is set.
// Returns the document's height in pixels.
static int walk(struct uui_markdown *m, struct ugfx_surface *s,
                 int text_w, int origin_x, int origin_y)
{
    struct md_ctx c;
    for (unsigned i = 0; i < sizeof c; i++) ((char *)&c)[i] = 0;
    c.m = m;
    c.s = s;
    c.left = 0;
    c.right = text_w;
    c.pen_x = 0;
    c.hang = 0;
    c.ox = origin_x;
    c.oy = origin_y;

    int body_h = font_h(ugfx_font_session(UGFX_FONT_REGULAR));
    int gap = body_h / 2;
    int fence = 0;
    int table_row = 0;

    int i = 0;
    while (i < m->len) {
        const char *ln;
        int n = umd_next_line(m->src, m->len, &i, &ln);

        int arg = 0;
        const char *txt = ln;
        int tn = n;
        enum umd_block kind = umd_classify(ln, n, &arg, &txt, &tn);

        if (kind == UMD_FENCE) { end_block(&c); fence = !fence; continue; }

        if (fence || kind == UMD_PRE) {
            // VERBATIM: no wrapping and no markup, on a tinted ground
            // that runs the full width so a block reads as one slab
            // rather than as ragged lines.
            end_block(&c);
            const struct ugfx_font *f = face_or(m, F_MONO,
                                                 ugfx_font_session(UGFX_FONT_REGULAR));
            int lh = font_h(f);
            if (s) {
                ugfx_fill_rect(s, c.ox + c.left, c.oy + c.pen_y,
                                c.right - c.left, lh, code_bg());
                const struct ugfx_font *was = ugfx_set_font(f);
                int x = c.ox + c.left + body_h / 2;
                const char *v = fence ? ln : txt;
                int vn = fence ? n : tn;
                for (int k = 0; k < vn; k++) {
                    ugfx_draw_char(s, x, c.oy + c.pen_y, v[k], UTHEME_TEXT, code_bg());
                    x += ugfx_char_advance(v[k]);
                }
                ugfx_set_font(was);
            }
            c.pen_y += lh;
            continue;
        }

        switch (kind) {
        case UMD_BLANK:
            end_block(&c);
            table_row = 0;
            c.pen_y += gap;
            break;

        case UMD_HEADING: {
            end_block(&c);
            c.pen_y += gap;
            c.level = arg;
            feed(&c, txt, tn);
            end_block(&c);
            c.level = 0;
            // A RULE UNDER THE TOP TWO LEVELS, which is what makes a
            // long page scannable -- GitHub, and every Markdown style
            // sheet since, draws one.
            if (arg <= 2) {
                c.pen_y += 2;
                hrule(&c, arg == 1);
                c.pen_y += gap;
            }
            break;
        }

        case UMD_RULE:
            end_block(&c);
            c.pen_y += gap;
            hrule(&c, 0);
            c.pen_y += gap;
            break;

        case UMD_BULLET:
        case UMD_NUMBERED: {
            end_block(&c);
            // A HANGING INDENT: the marker sits at the margin and the
            // text wraps under the text, not under the bullet. That one
            // detail is most of what makes a list look like a list.
            int marker_px = char_px(ugfx_font_session(UGFX_FONT_REGULAR), 'M') * 2;
            if (s) {
                const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
                int x = c.ox + c.left;
                if (kind == UMD_BULLET) {
                    ugfx_draw_char(s, x, c.oy + c.pen_y, '-', UTHEME_ACCENT, UTHEME_WHITE);
                } else {
                    for (int k = 0; k < arg && ln[k] != ' '; k++) {
                        ugfx_draw_char(s, x, c.oy + c.pen_y, ln[k], UTHEME_ACCENT, UTHEME_WHITE);
                        x += ugfx_char_advance(ln[k]);
                    }
                }
                ugfx_set_font(was);
            }
            c.pen_x = c.left + marker_px;
            c.hang = c.left + marker_px;
            c.open = 1;
            feed(&c, txt, tn);
            break;
        }

        case UMD_TABLE: {
            end_block(&c);
            if (arg) break;   // the ---|--- rule row draws nothing
            // Even columns: the widths a terminal derives from the
            // header do not survive a proportional font, and an even
            // split reads correctly for the tables these pages carry.
            int cols = 0;
            for (int k = 0; k < n; k++) if (ln[k] == '|') cols++;
            if (cols < 2) break;
            cols -= 1;
            int cw = (c.right - c.left) / (cols > 0 ? cols : 1);
            int col = 0, k = 1;
            c.level = table_row == 0 ? 3 : 0;   // the header row is bold
            while (k <= n && col < cols) {
                int start = k;
                while (k < n && ln[k] != '|') k++;
                c.pen_x = c.left + col * cw;
                c.hang = c.pen_x;
                c.right = c.left + (col + 1) * cw - 4;
                c.open = 1;
                feed(&c, ln + start, k - start);
                c.right = text_w;
                col++;
                k++;
            }
            c.level = 0;
            c.open = 1;
            end_block(&c);
            table_row++;
            break;
        }

        default:
            // A paragraph, fed one source line at a time -- the wrap is
            // this renderer's, not the author's.
            if (!c.open) { c.pen_x = c.left; c.hang = c.left; }
            feed(&c, txt, tn);
            break;
        }
    }
    end_block(&c);
    return c.pen_y;
}

// --- the widget --------------------------------------------------------

static int bar_width(struct uui_markdown *m) {
    if (m->bar_w > 0) return m->bar_w;
    int w = 0;
    uui_scrollbar_natural_size(&w, 0);
    return w;
}

void uui_markdown_init(struct uui_markdown *m) {
    for (unsigned i = 0; i < sizeof *m; i++) ((char *)m)[i] = 0;
    m->thumb_grab = -1;
}

void uui_markdown_set_text(struct uui_markdown *m, const char *src, int len) {
    if (m->src == src && m->len == len) return;   // a redraw must not jump to the top
    m->src = src;
    m->len = len;
    m->scroll = 0;
}

void uui_markdown_set_geometry(struct uui_markdown *m, int x, int y, int w, int h) {
    m->x = x; m->y = y; m->w = w; m->h = h;
}

void uui_markdown_natural_size(const struct uui_markdown *m, int *out_w, int *out_h) {
    (void)m;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
}

static int text_width(struct uui_markdown *m) {
    int pad = ugfx_char_w();
    int w = m->w - bar_width(m) - 2 * pad;
    return w > 16 ? w : 16;
}

static void clamp_scroll(struct uui_markdown *m) {
    int max = m->doc_h - m->h;
    if (max < 0) max = 0;
    if (m->scroll > max) m->scroll = max;
    if (m->scroll < 0) m->scroll = 0;
}

void uui_markdown_draw(struct ugfx_surface *s, struct uui_markdown *m) {
    ensure_faces(m);
    ugfx_fill_rect(s, m->x, m->y, m->w, m->h, UTHEME_WHITE);
    if (!m->src || m->len <= 0) return;

    int pad = ugfx_char_w();
    int tw = text_width(m);

    // MEASURED FIRST, with no surface: the scrollbar and the clamp both
    // need the document's height before a single glyph is placed.
    m->doc_h = walk(m, 0, tw, 0, 0);
    clamp_scroll(m);

    // The document is drawn into a CLIPPED band, so a block that
    // straddles the edge is cut rather than spilling over the chrome --
    // the widget draws whole blocks and lets the clip do the rest.
    ugfx_set_clip_rect(s, m->x, m->y, m->w - bar_width(m), m->h);
    walk(m, s, tw, m->x + pad, m->y - m->scroll);
    ugfx_clear_clip_rect(s);

    int bw = bar_width(m);
    uui_scrollbar_draw(s, m->x + m->w - bw, m->y, bw, m->h,
                        m->doc_h, m->h, m->doc_h - m->h - m->scroll,
                        UTHEME_PANEL_BG, UTHEME_BORDER, 0);
}

int uui_markdown_wheel(struct uui_markdown *m, int notches) {
    m->scroll -= notches * ugfx_char_h() * 3;
    clamp_scroll(m);
    return 1;
}

int uui_markdown_hit(const struct uui_markdown *m, int cx, int cy) {
    // THE WHOLE RECT, because this widget draws a scrollbar: answering
    // with the text area alone refuses press and wheel on the bar
    // column, and the thumb cannot be dragged (CLAUDE.md).
    return uui_hit(m->x, m->y, m->w, m->h, cx, cy);
}

int uui_markdown_press(struct uui_markdown *m, int cx, int cy) {
    int bw = bar_width(m);
    int bx = m->x + m->w - bw;
    if (cx < bx) return 0;

    int off = m->doc_h - m->h - m->scroll;
    enum uui_scrollbar_zone z =
        uui_scrollbar_hit(bx, m->y, bw, m->h, m->doc_h, m->h, off, cx, cy, 0);
    switch (z) {
    case UUI_SB_NONE:  return 0;
    case UUI_SB_ABOVE: m->scroll -= m->h; break;
    case UUI_SB_BELOW: m->scroll += m->h; break;
    case UUI_SB_THUMB: {
        int ty, th;
        uui_scrollbar_thumb_rect(m->y, m->h, m->doc_h, m->h, off, &ty, &th, bw, 0);
        m->thumb_grab = cy - ty;
        return 1;
    }
    default: return 1;
    }
    clamp_scroll(m);
    return 1;
}

void uui_markdown_motion(struct uui_markdown *m, int cx, int cy) {
    (void)cx;
    if (m->thumb_grab < 0) return;
    int off = uui_scrollbar_offset_for_drag(m->y, m->h, m->doc_h, m->h,
                                             cy, m->thumb_grab, bar_width(m), 0);
    m->scroll = m->doc_h - m->h - off;
    clamp_scroll(m);
}

void uui_markdown_release(struct uui_markdown *m) { m->thumb_grab = -1; }

void uui_markdown_free(struct uui_markdown *m) {
    for (int i = 0; i < UUI_MD_FACES; i++) {
        free(m->face[i].arena);
        m->face[i].arena = 0;
        m->face[i].ok = 0;
    }
    m->faces_tried = 0;
}

// --- the ops table -----------------------------------------------------

static void op_draw(struct ugfx_surface *s, const void *w) {
    uui_markdown_draw(s, (struct uui_markdown *)(void *)(const void *)w);
}
static void op_natural(const void *w, int *ow, int *oh) {
    uui_markdown_natural_size((const struct uui_markdown *)w, ow, oh);
}
static void op_geom(void *w, int x, int y, int cw, int ch) {
    uui_markdown_set_geometry((struct uui_markdown *)w, x, y, cw, ch);
}
static void op_bounds(const void *w, int *x, int *y, int *cw, int *ch) {
    const struct uui_markdown *m = (const struct uui_markdown *)w;
    if (x) *x = m->x;
    if (y) *y = m->y;
    if (cw) *cw = m->w;
    if (ch) *ch = m->h;
}
static int op_hit(const void *w, int x, int y) {
    return uui_markdown_hit((const struct uui_markdown *)w, x, y);
}
static int op_press(void *w, int x, int y, unsigned mods) {
    (void)mods;
    return uui_markdown_press((struct uui_markdown *)w, x, y);
}
static int op_motion(void *w, int x, int y, unsigned buttons) {
    if (!buttons) return 0;
    uui_markdown_motion((struct uui_markdown *)w, x, y);
    return 1;
}
static int op_release(void *w, int x, int y) {
    (void)x; (void)y;
    uui_markdown_release((struct uui_markdown *)w);
    return 1;
}
static int op_wheel(void *w, int notches) {
    return uui_markdown_wheel((struct uui_markdown *)w, notches);
}

const struct uui_widget_ops uui_markdown_ops = {
    .draw = op_draw,
    .natural_size = op_natural,
    .set_geometry = op_geom,
    .bounds = op_bounds,
    .hit = op_hit,
    .press = op_press,
    .motion = op_motion,
    .release = op_release,
    .wheel = op_wheel,
};
