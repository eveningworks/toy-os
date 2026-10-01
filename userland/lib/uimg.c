// uimg -- the codec table, the file front end, and the resampler.
// See uimg.h for why any of this is in ring 3 rather than in the kernel.
#include "lib/uimg.h"
#include "lib/ufile.h"
#include "rt/sys.h"
#include <kerrno.h>
#include <stdlib.h>
#include <string.h>

// THE TABLE. A format is a row here and a file beside this one -- the
// same shape as kernel/proc/syscall_table.c and the driver registries,
// and what WIC and GdkPixbuf are. Probes run in order and the first
// claim wins, so a row's position matters only if two formats could
// claim the same bytes, which none of these can.
static const struct uimg_codec *const codecs[] = {
    &uimg_codec_jpeg,
    &uimg_codec_qoi,
    &uimg_codec_png,
};
#define CODEC_COUNT ((int)(sizeof codecs / sizeof codecs[0]))

// The sentence behind the last error. A static string, never a copy: a
// decoder sets it to a literal, so there is no buffer to size and
// nothing to run out of. Same idea as libjpeg's jpeg_error_mgr message,
// minus the callback table nobody here would install.
static const char *g_error = "";

void uimg_set_error(const char *msg) { g_error = msg; }
const char *uimg_last_error(void) { return g_error; }

const struct uimg_codec *uimg_probe(const void *data, size_t n) {
    const uint8_t *d = data;
    for (int i = 0; i < CODEC_COUNT; i++)
        if (codecs[i]->probe(d, n)) return codecs[i];
    return NULL;
}

int uimg_info(const void *data, size_t n, struct uimg_info *out) {
    memset(out, 0, sizeof *out);
    const struct uimg_codec *c = uimg_probe(data, n);
    if (!c) {
        uimg_set_error("not an image format this build recognises");
        return -EINVAL;
    }
    return c->info(data, n, out);
}

int uimg_decode(const void *data, size_t n, struct uimg *out) {
    memset(out, 0, sizeof *out);
    const struct uimg_codec *c = uimg_probe(data, n);
    if (!c) {
        uimg_set_error("not an image format this build recognises");
        return -EINVAL;
    }
    // A row with no decoder is a format this build WRITES and cannot
    // read. -ENOTSUP says the file is fine and we are not, which is
    // what lets the Image Viewer say so instead of calling it corrupt.
    if (!c->decode) {
        uimg_set_error("this build can write this format but not read it");
        return -ENOTSUP;
    }
    return c->decode(data, n, out);
}

void uimg_free(struct uimg *im) {
    if (!im) return;
    free(im->px);
    im->px = NULL;
    im->w = im->h = 0;
}

// Reads a whole file into one allocation (lib/ufile.h). The caller
// frees it.
//
// The wording is HERE rather than in the shared reader because these
// sentences reach a person in the Image Viewer, and an errno cannot
// carry them: an empty file and one past the ceiling are both EINVAL,
// and they are not the same problem.
static int read_file(const char *path, uint8_t **out, size_t *out_len) {
    switch (ufile_slurp(path, UIMG_MAX_FILE, out, out_len)) {
    case UFILE_OK:
        return 0;
    case UFILE_NOENT:
        uimg_set_error("no such file");
        return -ENOENT;
    case UFILE_EMPTY:
        uimg_set_error("the file is empty");
        return -EINVAL;
    case UFILE_TOO_BIG:
        // REFUSED rather than read as a prefix: a truncated JPEG
        // decodes, to a grey-tailed picture that looks like a decoder
        // bug rather than like a file that did not fit.
        uimg_set_error("the file is larger than this decoder will read");
        return -EINVAL;
    case UFILE_NOMEM:
        uimg_set_error("not enough memory to read the file");
        return -ENOMEM;
    case UFILE_OPEN:
        uimg_set_error("the file could not be opened");
        return -EIO;
    case UFILE_SHORT:
    default:
        uimg_set_error("the file ended early");
        return -EIO;
    }
}

int uimg_load_info(const char *path, struct uimg_info *out) {
    uint8_t *buf;
    size_t len;
    int rc = read_file(path, &buf, &len);
    if (rc < 0) return rc;
    rc = uimg_info(buf, len, out);
    free(buf);
    return rc;
}

int uimg_load(const char *path, struct uimg *out) {
    uint8_t *buf;
    size_t len;
    int rc = read_file(path, &buf, &len);
    if (rc < 0) return rc;
    rc = uimg_decode(buf, len, out);
    free(buf);
    return rc;
}

// --- encoding ---------------------------------------------------------

// The format is NAMED. Probing the pixels to pick one would be guessing
// at the caller's intent, and the two formats here differ in what they
// are FOR rather than in what they can hold (see uimg.h).
static const struct uimg_codec *codec_named(const char *name) {
    for (int i = 0; i < CODEC_COUNT; i++)
        if (strcmp(codecs[i]->name, name) == 0) return codecs[i];
    return NULL;
}

int uimg_encode(const struct uimg *im, const char *format,
                uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    if (!im || !im->px || im->w <= 0 || im->h <= 0) {
        uimg_set_error("nothing to encode");
        return -EINVAL;
    }
    const struct uimg_codec *c = format ? codec_named(format) : NULL;
    if (!c) {
        uimg_set_error("not an image format this build recognises");
        return -ENOTSUP;
    }
    if (!c->encode) {
        uimg_set_error("this build can read this format but not write it");
        return -ENOTSUP;
    }
    return c->encode(im, out, out_len);
}

// The extension, lowercased, with no dot. An unknown one is refused
// rather than defaulted: silently writing a QOI into a file called
// .bmp would produce a file nothing opens and no error to explain it.
static const char *format_from_path(const char *path) {
    const char *dot = NULL;
    for (const char *p = path; *p; p++)
        if (*p == '.') dot = p;
        else if (*p == '/') dot = NULL;
    if (!dot || !dot[1]) return NULL;

    static char ext[8];
    size_t n = 0;
    for (const char *p = dot + 1; *p && n + 1 < sizeof ext; p++)
        ext[n++] = (*p >= 'A' && *p <= 'Z') ? (char)(*p - 'A' + 'a') : *p;
    ext[n] = '\0';
    if (strcmp(ext, "jpg") == 0) return "jpeg";
    return ext;
}

int uimg_save(const char *path, const struct uimg *im, const char *format) {
    if (!format) format = format_from_path(path);
    if (!format) {
        uimg_set_error("the filename has no extension to pick a format from");
        return -ENOTSUP;
    }

    uint8_t *buf;
    size_t len;
    int rc = uimg_encode(im, format, &buf, &len);
    if (rc < 0) return rc;

    int fd = sys_open(path, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (fd < 0) {
        free(buf);
        uimg_set_error("the file could not be created");
        return fd;
    }
    // sys_write() completes the whole buffer (the kernel chunks it), so
    // a short return here is a real failure and not a partial write to
    // resume. A half-written image file is worse than none: it has a
    // valid header and decodes to a picture that is simply wrong.
    long wrote = sys_write(fd, buf, len);
    sys_close(fd);
    free(buf);
    if (wrote < 0 || (size_t)wrote != len) {
        uimg_set_error("the file could not be written in full");
        return wrote < 0 ? (int)wrote : -EIO;
    }
    return 0;
}

// --- fitting and resampling -------------------------------------------

void uimg_fit_size(int sw, int sh, int bw, int bh, enum uimg_fit mode,
                   int *out_w, int *out_h) {
    int w = sw, h = sh;
    if (sw <= 0 || sh <= 0 || bw <= 0 || bh <= 0) {
        *out_w = *out_h = 1;
        return;
    }
    switch (mode) {
    case UIMG_FIT_NONE:
        break;
    case UIMG_FIT_STRETCH:
        w = bw; h = bh;
        break;
    case UIMG_FIT_CONTAIN:
    case UIMG_FIT_COVER: {
        // Compare aspect ratios by cross-multiplying, which keeps this
        // integer: sw/sh vs bw/bh is sw*bh vs bw*sh.
        int wide = (int64_t)sw * bh > (int64_t)bw * sh;
        int fit_w = (mode == UIMG_FIT_CONTAIN) ? wide : !wide;
        if (fit_w) {
            w = bw;
            h = (int)(((int64_t)sh * bw + sw / 2) / sw);
        } else {
            h = bh;
            w = (int)(((int64_t)sw * bh + sh / 2) / sh);
        }
        break;
    }
    }
    *out_w = w < 1 ? 1 : w;
    *out_h = h < 1 ? 1 : h;
}

// One separable pass. `src` is sw x sh, `dst` is dw x sh (a horizontal
// pass) -- the vertical pass runs the same code on a transposed view by
// swapping the strides its caller passes in.
//
// SHRINKING BOX-AVERAGES AND ENLARGING INTERPOLATES, per axis. That
// asymmetry is the point: a box filter on an enlargement is
// nearest-neighbour (every output pixel's box holds one source pixel),
// and linear interpolation on a big reduction samples a fraction of the
// source and aliases -- which is exactly what a downscaled photo shows
// as shimmering detail. Both are 16.16 fixed point.
//
// **COLOUR IS WEIGHTED BY ALPHA, WHICH IS WHY THIS LOOKS ODD.** Mixing
// straight (non-premultiplied) RGBA by averaging all four channels
// independently is wrong wherever alpha varies: a transparent pixel
// still contributes its colour, and since a generator writes (0,0,0,0)
// outside a shape, every downscaled icon grows a dark halo along its
// edge. So the alpha channel is averaged by area, and the colour
// channels are averaged weighted by alpha -- which is the same thing as
// premultiplying, resampling and unpremultiplying, without the two extra
// passes over the image. It costs a multiply per channel and it is used
// even for an opaque image, where alpha is constant and cancels exactly.
// `opaque` reads every alpha byte as 255: an image whose has_alpha is 0
// is opaque by definition, and a buffer in 0x00RRGGBB (a client
// window's) would otherwise weight every colour by zero and scale to
// black.
static void resample_axis(const uint32_t *src, uint32_t *dst,
                          int src_n, int dst_n, int lines,
                          int src_step, int src_line, int dst_step, int dst_line,
                          int opaque) {
    if (dst_n >= src_n) {
        // Enlarge: linear interpolation between the two nearest samples.
        // The map is (i + 0.5) * src/dst - 0.5 in 16.16, clamped at the
        // ends so the edge pixels are not fetched from outside.
        int64_t ratio = ((int64_t)src_n << 16) / dst_n;
        for (int i = 0; i < dst_n; i++) {
            int64_t pos = ((2 * (int64_t)i + 1) * ratio) / 2 - (1 << 15);
            if (pos < 0) pos = 0;
            int s0 = (int)(pos >> 16);
            int frac = (int)(pos & 0xFFFF);
            int s1 = s0 + 1;
            if (s1 > src_n - 1) { s1 = src_n - 1; if (s0 > s1) s0 = s1; }
            for (int l = 0; l < lines; l++) {
                const uint32_t *sp = src + (size_t)l * src_line;
                uint32_t a = sp[(size_t)s0 * src_step];
                uint32_t b = sp[(size_t)s1 * src_step];
                int aa = opaque ? 255 : (int)(a >> 24), ab = opaque ? 255 : (int)(b >> 24);
                int wa = (65536 - frac), wb = frac;
                int alpha = (aa * wa + ab * wb) >> 16;
                int64_t caw = (int64_t)aa * wa, cbw = (int64_t)ab * wb;
                int64_t den = caw + cbw;
                uint32_t o = (uint32_t)(alpha & 0xFF) << 24;
                for (int shift = 0; shift <= 16; shift += 8) {
                    int ca = (int)((a >> shift) & 0xFF), cb = (int)((b >> shift) & 0xFF);
                    int v = den ? (int)(((int64_t)ca * caw + (int64_t)cb * cbw) / den) : 0;
                    o |= (uint32_t)(v & 0xFF) << shift;
                }
                dst[(size_t)l * dst_line + (size_t)i * dst_step] = o;
            }
        }
        return;
    }

    // Shrink: average every source pixel whose span touches this output
    // pixel, weighted by how much of it does. Weights are 16.16 and sum
    // to exactly one output pixel's worth of source, so the divide is by
    // a constant.
    int64_t span = ((int64_t)src_n << 16) / dst_n;   // source pixels per output pixel
    for (int i = 0; i < dst_n; i++) {
        int64_t start = (int64_t)i * src_n * 65536 / dst_n;
        int64_t end = start + span;
        int s0 = (int)(start >> 16);
        int s1 = (int)((end - 1) >> 16);
        if (s1 > src_n - 1) s1 = src_n - 1;
        for (int l = 0; l < lines; l++) {
            const uint32_t *sp = src + (size_t)l * src_line;
            int64_t acc[3] = { 0, 0, 0 };
            int64_t asum = 0, wsum = 0, awsum = 0;
            for (int s = s0; s <= s1; s++) {
                int64_t lo = (int64_t)s << 16, hi = lo + 65536;
                int64_t w = (end < hi ? end : hi) - (start > lo ? start : lo);
                if (w <= 0) continue;
                uint32_t v = sp[(size_t)s * src_step];
                int64_t a = opaque ? 255 : (int64_t)((v >> 24) & 0xFF);
                int64_t aw = a * w;
                acc[0] += (int64_t)((v >> 16) & 0xFF) * aw;
                acc[1] += (int64_t)((v >> 8) & 0xFF) * aw;
                acc[2] += (int64_t)(v & 0xFF) * aw;
                asum += a * w;
                awsum += aw;
                wsum += w;
            }
            if (wsum == 0) wsum = 1;
            uint32_t alpha = (uint32_t)(asum / wsum) & 0xFF;
            uint32_t r = 0, g = 0, b = 0;
            if (awsum > 0) {
                r = (uint32_t)(acc[0] / awsum) & 0xFF;
                g = (uint32_t)(acc[1] / awsum) & 0xFF;
                b = (uint32_t)(acc[2] / awsum) & 0xFF;
            }
            dst[(size_t)l * dst_line + (size_t)i * dst_step] =
                (alpha << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

int uimg_rotate(const struct uimg *src, int quarters, struct uimg *out) {
    memset(out, 0, sizeof *out);
    if (!src->px || src->w <= 0 || src->h <= 0) {
        uimg_set_error("nothing to rotate");
        return -EINVAL;
    }
    int q = ((quarters % 4) + 4) % 4;
    int w = src->w, h = src->h;
    int ow = (q & 1) ? h : w, oh = (q & 1) ? w : h;
    uint32_t *px = malloc((size_t)ow * oh * sizeof *px);
    if (!px) {
        uimg_set_error("not enough memory to rotate the image");
        return -ENOMEM;
    }
    // Each source pixel's destination; the loops walk the SOURCE in
    // order, which is the cache-friendly side for the larger read.
    for (int y = 0; y < h; y++) {
        const uint32_t *row = src->px + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            int dx, dy;
            switch (q) {
            case 1:  dx = h - 1 - y; dy = x;         break;
            case 2:  dx = w - 1 - x; dy = h - 1 - y; break;
            case 3:  dx = y;         dy = w - 1 - x; break;
            default: dx = x;         dy = y;         break;
            }
            px[(size_t)dy * ow + dx] = row[x];
        }
    }
    out->w = ow;
    out->h = oh;
    out->px = px;
    out->has_alpha = src->has_alpha;
    return 0;
}

int uimg_scale(const struct uimg *src, int dw, int dh, struct uimg *out) {
    memset(out, 0, sizeof *out);
    if (!src->px || src->w <= 0 || src->h <= 0 || dw <= 0 || dh <= 0) {
        uimg_set_error("nothing to scale");
        return -EINVAL;
    }
    if ((int64_t)dw * dh > 64 * 1024 * 1024) {
        uimg_set_error("that target size is larger than this build will allocate");
        return -EINVAL;
    }

    if (dw == src->w && dh == src->h) {
        // A copy rather than a shared pointer: the caller frees `out`
        // independently of `src`, and aliasing the two would be a
        // double free waiting for whichever is released second.
        uint32_t *px = malloc((size_t)dw * dh * sizeof *px);
        if (!px) { uimg_set_error("not enough memory to copy the image"); return -ENOMEM; }
        memcpy(px, src->px, (size_t)dw * dh * sizeof *px);
        out->w = dw; out->h = dh; out->px = px;
        out->has_alpha = src->has_alpha;
        return 0;
    }

    uint32_t *mid = malloc((size_t)dw * src->h * sizeof *mid);
    uint32_t *dst = malloc((size_t)dw * dh * sizeof *dst);
    if (!mid || !dst) {
        free(mid);
        free(dst);
        uimg_set_error("not enough memory to scale the image");
        return -ENOMEM;
    }

    // Horizontal: each source row becomes a dw-wide row.
    resample_axis(src->px, mid, src->w, dw, src->h, 1, src->w, 1, dw, !src->has_alpha);
    // Vertical: each of the dw columns is resampled down its own stride,
    // which is the transposed view the same routine handles by walking
    // with a stride instead of by one.
    resample_axis(mid, dst, src->h, dh, dw, dw, 1, dw, 1, !src->has_alpha);
    free(mid);

    out->w = dw;
    out->h = dh;
    out->px = dst;
    out->has_alpha = src->has_alpha;
    return 0;
}
