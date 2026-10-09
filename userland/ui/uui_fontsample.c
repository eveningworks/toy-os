// A font face, loaded and shown -- see ui/uui_fontsample.h.
#include "ui/uui_fontsample.h"
#include "ui/utheme.h"
#include "ui/uui_primitives.h"
#include <stdio.h>
#include <string.h>

int uui_fontface_family_file(const char *name, char *stem, int cap) {
    size_t len = strlen(name);
    if (len < 5 || strcmp(name + len - 4, ".ttf")) return 0;
    if (len > 9 && !strcmp(name + len - 9, "-bold.ttf")) return 0;   // a weight, not a family
    snprintf(stem, (size_t)cap, "%.*s", (int)(len - 4), name);
    return 1;
}

int uui_fontface_open(struct uui_fontface *f, const char *dir, const char *stem) {
    memset(f, 0, sizeof *f);
    snprintf(f->stem, sizeof f->stem, "%s", stem);
    char path[128];
    snprintf(path, sizeof path, "%s/%s.ttf", dir, stem);
    if (!uglyph_open(&f->reg, path)) return 0;
    snprintf(path, sizeof path, "%s/%s-bold.ttf", dir, stem);
    f->has_bold = uglyph_open(&f->bold, path);
    if (!uglyph_family(&f->reg, f->family, sizeof f->family))
        snprintf(f->family, sizeof f->family, "%s", stem);
    f->mono = uglyph_text_width(&f->reg, "i", 20) == uglyph_text_width(&f->reg, "W", 20);
    return 1;
}

void uui_fontface_close(struct uui_fontface *f) {
    if (f->reg.ok) { uglyph_close(&f->reg); uglyph_forget(&f->reg); }
    if (f->has_bold) { uglyph_close(&f->bold); uglyph_forget(&f->bold); }
    memset(f, 0, sizeof *f);
}

// One line, in the face or (f == NULL) the baked face, clipped to the box.
static void line(struct ugfx_surface *s, struct uui_fontface *f, const char *text, int px,
                 int x, int baseline, int w, uint32_t fg, uint32_t bg) {
    if (f) {
        // uglyph draws no clip of its own: stop at the last character
        // that ends inside the box.
        char fit[96];
        size_t n = strlen(text);
        if (n >= sizeof fit) n = sizeof fit - 1;
        memcpy(fit, text, n);
        fit[n] = 0;
        while (n && uglyph_text_width(&f->reg, fit, px) > w) fit[--n] = 0;
        uglyph_text(s, &f->reg, fit, px, x, baseline, fg);
        return;
    }
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_baked());
    ugfx_draw_string_clipped(s, x, baseline - ugfx_char_h() * 3 / 4, w, text, fg, bg);
    ugfx_set_font(was);
}

void uui_fontsample_draw(struct ugfx_surface *s, struct uui_fontface *f, int style,
                         int x, int y, int w, int h, uint32_t bg, int dim) {
    int ch = ugfx_char_h(), pad = ch / 2;
    const char *name = f ? f->family : "Built-in";
    if (style == UUI_FONTSAMPLE_TERMINAL) {
        uint32_t term = ugfx_rgb(31, 37, 46), ink = ugfx_rgb(214, 220, 228);
        if (dim) ink = ugfx_blend(ink, term, 120);
        uui_fill_round_rect(s, x, y, w, h, ch / 3, term);
        static const char *const LINES[] = { "$ ls -l /etc", "-rw   1200  motd", "drw      -  wm/" };
        // Three lines in whatever height the box has: the size follows
        // the pitch, never the other way round, so a shorter box (a card
        // with a note under it) gets smaller text rather than overlapping.
        int pitch = (h - pad) / 3, px = pitch * 4 / 5;
        if (px > ch) px = ch;
        // The baked face has one size: the session's, line for line.
        if (!f) pitch = ch;
        for (int i = 0; i < 3; i++)
            line(s, f, LINES[i], px, x + pad, y + pad / 2 + pitch * i + px, w - 2 * pad, ink, term);
        return;
    }
    // A ROW, not a stack: a gallery's tiles are wide and short. The name
    // is the card's own label underneath, so it is not repeated here.
    (void)name;
    uint32_t fg = dim ? ugfx_blend(UTHEME_TEXT, bg, 110) : UTHEME_TEXT;
    uint32_t soft = ugfx_blend(UTHEME_TEXT, bg, 150);
    static const char SAMPLE[] = "The quick brown fox jumps 0123";
    if (f) {
        int big = h * 3 / 5, aw = uglyph_text_width(&f->reg, "Aa", big);
        int base = y + (h + big * 7 / 10) / 2;
        line(s, f, "Aa", big, x + pad, base, w - 2 * pad, fg, bg);
        int sx = x + pad + aw + pad;
        if (sx < x + w - pad) line(s, f, SAMPLE, ch, sx, base, x + w - pad - sx, soft, bg);
        return;
    }
    // The baked face has one size, so it is shown at that size.
    int base = y + (h + ch) / 2;
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_baked());
    int aw = ugfx_text_width("Aa");
    ugfx_set_font(was);
    line(s, 0, "Aa", ch, x + pad, base, w - 2 * pad, fg, bg);
    line(s, 0, SAMPLE, ch, x + pad + aw + pad, base, w - 3 * pad - aw, soft, bg);
}
