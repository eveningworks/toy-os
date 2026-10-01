// The cursor shape format -- see lib/ucursor.h. Moved here from the
// compositor's cursor_theme.c so System Settings can draw a theme too.
#include "lib/ucursor.h"
#include "lib/uimg.h"
#include "lib/ufile.h"
#include "string.h"
#include "knum.h"
#include "kfmt.h"
#include <stddef.h>

// --- the parser ------------------------------------------------------

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c == '.') return 0;
    return -1;
}

// One coverage character -> 0..255. The file stores 16 levels (see
// tools/gen_cursors.py), so 'f' must land on exactly 255 rather than
// 240: a fully opaque pixel that comes out 94% opaque makes every
// cursor faintly translucent, which looks like a blending bug.
static int cov_to_alpha(char c, unsigned char *out) {
    int v = hex_val(c);
    if (v < 0) return 0;
    *out = (unsigned char)(v * 255 / 15);
    return 1;
}

// The longest line worth holding. Comment and header lines run to ~60
// characters; a grid row is at most CURSOR_SHAPE_MAX. Sized for the
// former, because the first version sized it for the latter and every
// file then failed on its own comment header -- with the built-in
// fallback quietly keeping the pointer working, so nothing looked
// broken.
#define CURSOR_LINE_MAX 128

// An image file's name: a bare filename in the theme's own directory,
// ending ".qoi" -- no '/' and no "..", so a descriptor cannot point the
// loader anywhere else on the disk.
static int image_name_ok(const char *v) {
    uint32_t n = (uint32_t)k_strlen(v);
    if (n < 5 || n >= CURSOR_IMAGE_NAME_MAX) return 0;
    if (k_strcmp(v + n - 4, ".qoi") != 0) return 0;
    for (uint32_t i = 0; i < n; i++) {
        char c = v[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '@' ||
                 (c == '.' && i > 0 && v[i - 1] != '.');
        if (!ok) return 0;
    }
    return 1;
}


// Copies one line out, returning its FULL length (which may exceed what
// fits) and advancing `*pos` past the newline. -1 at end of input.
//
// Returning the full length rather than the copied length is what lets
// the caller refuse a grid row that is too long instead of silently
// accepting its first 127 characters -- while an over-long COMMENT is
// harmlessly skipped. A parser here rejects rather than guesses, but
// only about the thing it is parsing.
static int next_line(const char *text, uint32_t len, uint32_t *pos,
                      char *out, uint32_t out_size) {
    if (*pos >= len) return -1;
    uint32_t n = 0, full = 0;
    while (*pos < len && text[*pos] != '\n') {
        if (n + 1 < out_size) out[n++] = text[*pos];
        full++;
        (*pos)++;
    }
    if (*pos < len) (*pos)++; // the newline
    out[n] = '\0';
    return (int)full;
}

static int parse_grid(const char *text, uint32_t len, uint32_t *pos,
                       int w, int h, unsigned char grid[][CURSOR_SHAPE_MAX]) {
    char line[CURSOR_LINE_MAX];
    for (int y = 0; y < h; y++) {
        int n = next_line(text, len, pos, line, sizeof line);
        if (n != w) return 0; // a short or long row is a malformed file
        for (int x = 0; x < w; x++)
            if (!cov_to_alpha(line[x], &grid[y][x])) return 0;
    }
    return 1;
}

// Parses straight INTO `out` rather than into a local. Two reasons, both
// practical: the struct is a couple of KB, so a local would put two of
// them on the kernel stack and make the copy out a `memcpy` call this
// kernel has no symbol for (it link-failed exactly that way once); and
// `loaded` is set only on the last line, so a failed parse leaves the
// destination unloaded, which is precisely the fall-back-to-built-in
// state the caller wants. The trap that follows: `out` is scribbled on
// during a parse that may still fail, so never hand this a shape that
// is currently in use.
int cursor_shape_parse(const char *text, uint32_t len, struct cursor_shape *out) {
    if (!text || !out) return 0;

    struct cursor_shape *sp = out;
    k_memset(sp, 0, sizeof *sp);
    int w = 0, h = 0, hx = 0, hy = 0;
    int have_outline = 0, have_fill = 0, have_image = 0;

    uint32_t pos = 0;
    char line[CURSOR_LINE_MAX];
    for (;;) {
        int n = next_line(text, len, &pos, line, sizeof line);
        if (n < 0) break;
        if (n == 0 || line[0] == '#') continue;

        if (k_strncmp(line, "width=", 6) == 0) {
            if (!k_parse_u32(line + 6, (uint32_t *)&w)) return 0;
        } else if (k_strncmp(line, "height=", 7) == 0) {
            if (!k_parse_u32(line + 7, (uint32_t *)&h)) return 0;
        } else if (k_strncmp(line, "hotspot=", 8) == 0) {
            const char *comma = k_strchr(line + 8, ',');
            if (!comma) return 0;
            char xs[8];
            uint32_t xn = (uint32_t)(comma - (line + 8));
            if (xn >= sizeof xs) return 0;
            k_memcpy(xs, line + 8, xn);
            xs[xn] = '\0';
            if (!k_parse_u32(xs, (uint32_t *)&hx)) return 0;
            if (!k_parse_u32(comma + 1, (uint32_t *)&hy)) return 0;
        } else if (k_strcmp(line, "outline=") == 0 || k_strcmp(line, "fill=") == 0) {
            // Both grids come AFTER the dimensions, always -- a grid
            // read before them has no idea how wide a row should be, so
            // the only safe answer is to refuse.
            if (w <= 0 || h <= 0 || w > CURSOR_SHAPE_MAX || h > CURSOR_SHAPE_MAX)
                return 0;
            int is_outline = (line[0] == 'o');
            if (!parse_grid(text, len, &pos, w, h,
                             is_outline ? sp->outline : sp->fill))
                return 0;
            if (is_outline) have_outline = 1; else have_fill = 1;
        } else if (k_strncmp(line, "image", 5) == 0) {
            // image= is the 1x rendering, image2=/image3= the larger
            // ones. A key naming a scale past the cap is refused, not
            // ignored: it is this format's key, written wrong.
            int k = 1;
            const char *v = line + 5;
            if (*v >= '2' && *v <= '9') k = *v++ - '0';
            if (*v != '=' || k > CURSOR_SCALE_MAX) return 0;
            if (!image_name_ok(v + 1)) return 0;
            k_strlcpy(sp->image[k - 1], v + 1, sizeof sp->image[k - 1]);
            if (k == 1) have_image = 1;
        }
        // Unknown keys (`shape=`, and anything a later version adds) are
        // ignored rather than refused, so an older WM can still read a
        // newer theme's files.
    }

    // ONE kind or the other: masks AND an image is a file that does not
    // know what it is, and an image with no 1x rendering has no floor
    // for the sizes it does not cover.
    if (have_image) {
        if (have_outline || have_fill) return 0;
        if (w <= 0 || h <= 0 || w > CURSOR_SHAPE_MAX || h > CURSOR_SHAPE_MAX) return 0;
        sp->is_image = 1;
    } else if (!have_outline || !have_fill) {
        return 0;
    }
    if (hx < 0 || hx >= w || hy < 0 || hy >= h) return 0; // hotspot off the shape

    sp->w = w; sp->h = h; sp->hot_x = hx; sp->hot_y = hy;
    sp->baked = 1;
    sp->loaded = 1; // last, so a failure above leaves it unloaded
    return 1;
}

// --- decoding and loading ----------------------------------------------

#define CURSOR_PATH_MAX 192

void ucursor_release(struct cursor_shape *s) {
    struct uimg im = { .px = s->px };
    uimg_free(&im);
    s->px = 0;
    s->loaded = 0;
}

int ucursor_decode(const char *theme, struct cursor_shape *s, int scale,
                   char *why, int why_len) {
    int k = scale;
    if (k < 1 || k > CURSOR_SCALE_MAX || !s->image[k - 1][0]) k = 1;

    char path[CURSOR_PATH_MAX];
    if (!k_snprintf(path, sizeof path, "%s/%s/%s", UCURSOR_DIR, theme, s->image[k - 1]))
        return 0;
    struct uimg im;
    if (uimg_load(path, &im) < 0) {
        if (why) k_snprintf(why, (size_t)why_len, "%s did not decode (%s)", path, uimg_last_error());
        return 0;
    }
    if (im.w != s->w * k || im.h != s->h * k) {
        if (why) k_snprintf(why, (size_t)why_len, "%s is %dx%d, not %dx%d -- refused",
                            path, im.w, im.h, s->w * k, s->h * k);
        uimg_free(&im);
        return 0;
    }
    s->px = im.px;   // ownership moves here; ucursor_release() frees it
    s->baked = k;
    return 1;
}

int ucursor_load(const char *theme, const char *name, int scale, struct cursor_shape *out) {
    char path[CURSOR_PATH_MAX];
    if (!k_snprintf(path, sizeof path, "%s/%s/%s", UCURSOR_DIR, theme, name)) return 0;
    // static: a 4 KiB local is a quarter of a ring-3 stack, past its guard.
    static char body[CURSOR_FILE_MAX];
    size_t n = 0;
    if (ufile_read_into(path, body, sizeof body, &n) != UFILE_OK || n == 0) return 0;
    if (!cursor_shape_parse(body, (uint32_t)n, out)) return 0;
    if (out->is_image && !ucursor_decode(theme, out, scale, 0, 0)) {
        out->loaded = 0;
        return 0;
    }
    return 1;
}

uint32_t ucursor_pixel(const struct cursor_shape *s, int dx, int dy, int step,
                       uint32_t fill_rgb) {
    if (dx < 0 || dy < 0) return 0;
    int st = step > 0 ? step : 1;
    int x = dx / st, y = dy / st;
    if (s->is_image) {
        int pw = s->w * s->baked, ph = s->h * s->baked;
        if (x >= pw || y >= ph || !s->px) return 0;
        return s->px[y * pw + x];
    }
    if (x >= s->w || y >= s->h) return 0;
    // Black rim under the fill, folded: alpha is their union, and the
    // colour is the fill's share of it (the rim contributes black).
    uint32_t o = s->outline[y][x], f = s->fill[y][x];
    uint32_t a = f + o * (255 - f) / 255;
    if (!a) return 0;
    uint32_t r = ((fill_rgb >> 16) & 0xFF) * f / a;
    uint32_t g = ((fill_rgb >> 8) & 0xFF) * f / a;
    uint32_t b = (fill_rgb & 0xFF) * f / a;
    return (a << 24) | (r << 16) | (g << 8) | b;
}
