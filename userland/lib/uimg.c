// uimg -- the codec table, the file front end, and the resampler.
// See uimg.h for why any of this is in ring 3 rather than in the kernel.
#include "lib/uimg.h"
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
    return c->decode(data, n, out);
}

void uimg_free(struct uimg *im) {
    if (!im) return;
    free(im->px);
    im->px = NULL;
    im->w = im->h = 0;
}

// Reads a whole file into one allocation. The caller frees it.
//
// It REFUSES a file over UIMG_MAX_FILE rather than reading a prefix,
// because a truncated JPEG decodes -- to a grey-tailed picture that
// looks like a decoder bug rather than like a file that did not fit.
static int read_file(const char *path, uint8_t **out, size_t *out_len) {
    struct sys_stat st;
    if (sys_stat(path, &st) < 0) {
        uimg_set_error("no such file");
        return -ENOENT;
    }
    if (st.size == 0) {
        uimg_set_error("the file is empty");
        return -EINVAL;
    }
    if (st.size > UIMG_MAX_FILE) {
        uimg_set_error("the file is larger than this decoder will read");
        return -EINVAL;
    }
    size_t len = (size_t)st.size;
    uint8_t *buf = malloc(len);
    if (!buf) {
        uimg_set_error("not enough memory to read the file");
        return -ENOMEM;
    }
    int fd = sys_open(path, 0);
    if (fd < 0) {
        free(buf);
        uimg_set_error("the file could not be opened");
        return -EIO;
    }
    size_t got = 0;
    while (got < len) {
        int64_t r = sys_read(fd, buf + got, len - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    sys_close(fd);
    if (got != len) {
        free(buf);
        uimg_set_error("the file ended early");
        return -EIO;
    }
    *out = buf;
    *out_len = len;
    return 0;
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
static void resample_axis(const uint32_t *src, uint32_t *dst,
                          int src_n, int dst_n, int lines,
                          int src_step, int src_line, int dst_step, int dst_line) {
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
                uint32_t o = 0;
                for (int shift = 0; shift <= 16; shift += 8) {
                    int ca = (a >> shift) & 0xFF, cb = (b >> shift) & 0xFF;
                    int v = ca + (((cb - ca) * frac) >> 16);
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
            int64_t wsum = 0;
            for (int s = s0; s <= s1; s++) {
                int64_t lo = (int64_t)s << 16, hi = lo + 65536;
                int64_t w = (end < hi ? end : hi) - (start > lo ? start : lo);
                if (w <= 0) continue;
                uint32_t v = sp[(size_t)s * src_step];
                acc[0] += (int64_t)((v >> 16) & 0xFF) * w;
                acc[1] += (int64_t)((v >> 8) & 0xFF) * w;
                acc[2] += (int64_t)(v & 0xFF) * w;
                wsum += w;
            }
            if (wsum == 0) wsum = 1;
            dst[(size_t)l * dst_line + (size_t)i * dst_step] =
                ((uint32_t)(acc[0] / wsum) << 16) |
                ((uint32_t)(acc[1] / wsum) << 8) |
                 (uint32_t)(acc[2] / wsum);
        }
    }
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
    resample_axis(src->px, mid, src->w, dw, src->h, 1, src->w, 1, dw);
    // Vertical: each of the dw columns is resampled down its own stride,
    // which is the transposed view the same routine handles by walking
    // with a stride instead of by one.
    resample_axis(mid, dst, src->h, dh, dw, dw, 1, dw, 1);
    free(mid);

    out->w = dw;
    out->h = dh;
    out->px = dst;
    return 0;
}
