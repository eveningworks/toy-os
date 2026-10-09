// uvid_test -- the video DECODE path (lib/uvid.h), over the fixtures
// tools/gen_video.py writes into /tests.
//
// tiny.avi's sound is a FORMULA of its sample index (ramp() below, and
// ramp_sample() in gen_video.py), so its PCM is checked exactly with no
// reference decoder in the guest; its pictures are Motion JPEG, which
// uimg_hostcheck already judges against libjpeg, so here the question
// is the CONTAINER: how many frames, at what times, and where a seek
// lands.
//
// tiny.mpg's pictures are compared with tiny.yuv, FFmpeg's decode of the
// same file, by PSNR -- two correct MPEG-1 decoders differ by IDCT
// rounding (tools/uvid_hostcheck.py has the reasoning and the sweep); its
// sound is a 1 kHz tone, judged by counting zero crossings.
#include "rt/sys.h"
#include "lib/uimg.h"
#include "lib/usnd.h"
#include "lib/uvid.h"
#include "lib/uvid_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib/utest.h"

#define TINY_AVI "/tests/tiny.avi"
#define TINY_MPG "/tests/tiny.mpg"
#define TINY_YUV "/tests/tiny.yuv"
#define TW 72
#define TH 40
#define TFRAME (TW * TH + 2 * (TW / 2) * (TH / 2))
#define MIN_PSNR 50

static int ramp(long i, int ch) {
    int v = (int)((i * 263 + ch * 4099) & 0xFFFF);
    return v >= 0x8000 ? v - 0x10000 : v;
}

static void check_avi_info(void) {
    struct uvid_info in;
    int rc = uvid_load_info(TINY_AVI, &in);
    utest_checkf(rc == 0, "tiny.avi describes itself -- %s", uvid_last_error());
    if (rc) return;
    utest_checkf(in.w == 72 && in.h == 40, "72x40 -- got %dx%d", in.w, in.h);
    utest_checkf(in.frames == 10, "10 frames -- got %u", in.frames);
    utest_checkf(in.fps_num && in.fps_num / in.fps_den == 25, "25 fps -- got %u/%u", in.fps_num, in.fps_den);
    utest_checkf(in.ms == 400, "400 ms long -- got %u", in.ms);
    utest_checkf(!strcmp(in.codec, "mjpeg"), "Motion JPEG -- got '%s'", in.codec);
    utest_checkf(in.has_audio && strstr(in.audio, "PCM 16-bit") && strstr(in.audio, "22.05 kHz stereo"),
                 "16-bit 22.05 kHz stereo sound -- got '%s'", in.audio);
}

static void check_avi_frames(void) {
    struct uvid *v;
    if (uvid_open(TINY_AVI, &v)) { utest_checkf(0, "tiny.avi opens -- %s", uvid_last_error()); return; }
    const struct uvid_frame *f;
    int n = 0, times_ok = 1, rc;
    while ((rc = uvid_next(v, &f)) == 1) {
        if (f->pts_ms != n * 40 || f->w != 72 || f->h != 40) times_ok = 0;
        n++;
    }
    utest_checkf(rc == 0 && n == 10, "ten frames then the end -- got %d, last %d", n, rc);
    utest_check(times_ok, "frame N is shown at N x 40 ms, 72x40");

    // An exact seek lands on the frame SHOWING at the time, not after it.
    rc = uvid_seek(v, 210, UVID_SEEK_EXACT);
    utest_checkf(rc == 0 && uvid_next(v, &f) == 1 && f->pts_ms == 200,
                 "a seek to 210 ms shows the 200 ms frame -- got %lld", rc ? -1LL : (long long)f->pts_ms);
    rc = uvid_seek(v, 0, UVID_SEEK_EXACT);
    utest_checkf(rc == 0 && uvid_next(v, &f) == 1 && f->pts_ms == 0, "a seek back to 0 starts again");
    rc = uvid_seek(v, 5000, UVID_SEEK_EXACT);
    utest_checkf(uvid_next(v, &f) != 1 || f->pts_ms == 360, "a seek past the end lands on the last frame or nothing");

    // Drawn at its own size, a picture is its own pixels: the scaler's
    // identity case, through both qualities.
    uvid_seek(v, 120, UVID_SEEK_EXACT);
    if (uvid_next(v, &f) == 1 && f->fmt == UVID_ARGB) {
        static uint32_t out[72 * 40];
        for (int q = 0; q < 2; q++) {
            uvid_frame_draw(f, 0, 0, f->w, f->h, out, 72, 72, 40, q ? UVID_FAST : UVID_SMOOTH);
            int same = 1;
            for (int y = 0; y < 40; y++)
                for (int x = 0; x < 72; x++)
                    if (out[y * 72 + x] != (f->argb[y * f->argb_stride + x] & 0xffffff)) same = 0;
            utest_checkf(same, "drawn 1:1 %s, a frame is its own pixels", q ? "fast" : "smooth");
        }
        // Doubled with nearest-neighbour, each source pixel is a 2x2 block.
        static uint32_t big[144 * 80];
        uvid_frame_draw(f, 0, 0, f->w, f->h, big, 144, 144, 80, UVID_FAST);
        int blocks = 1;
        for (int y = 0; y < 80; y++)
            for (int x = 0; x < 144; x++)
                if (big[y * 144 + x] != (f->argb[(y / 2) * f->argb_stride + x / 2] & 0xffffff)) blocks = 0;
        utest_check(blocks, "drawn 2x fast, each pixel is a 2x2 block");
    } else {
        utest_check(0, "frame at 120 ms decodes as ARGB");
    }
    uvid_close(v);
}

static void check_avi_sound(void) {
    struct usnd_stream s;
    int rc = usnd_open(TINY_AVI, &s);
    utest_checkf(rc == 0, "usnd opens tiny.avi's sound -- %s", usnd_last_error());
    if (rc) return;
    utest_checkf(s.fmt.rate == 22050 && s.fmt.channels == 2 && s.fmt.bits == 16,
                 "22050 Hz, 2 channels, 16 bits -- got %u/%u/%u", s.fmt.rate, s.fmt.channels, s.fmt.bits);
    utest_checkf(s.frames == 8820, "8820 sample frames -- got %llu", (unsigned long long)s.frames);

    // The codec's own output, before usnd converts the rate: exact.
    static int32_t buf[1024 * 2];
    long i = 0, bad = -1, got;
    while ((got = s.codec->read(&s, buf, 1024)) > 0) {
        for (long k = 0; k < got; k++, i++)
            for (int c = 0; c < 2; c++)
                if (bad < 0 && buf[k * 2 + c] != ramp(i, c) * 65536) bad = i;
    }
    utest_checkf(i == 8820 && bad < 0, "every sample is the formula -- %ld frames, first wrong %ld", i, bad);

    rc = s.codec->seek(&s, 5000);
    got = s.codec->read(&s, buf, 4);
    utest_checkf(rc == 0 && got == 4 && buf[0] == ramp(5000, 0) * 65536 && buf[7] == ramp(5003, 1) * 65536,
                 "a seek to sample 5000 lands on it exactly");
    usnd_close(&s);
}

// A Motion JPEG frame from a camera carries no Huffman tables and relies
// on Annex K's: our own encoder writes exactly those, so stripping its
// DHT must decode to the same picture.
static void check_jpeg_without_dht(void) {
    struct uimg im = { 32, 16, 0, 0 };
    im.px = malloc(32 * 16 * 4);
    if (!im.px) { utest_check(0, "memory for the DHT test"); return; }
    for (int i = 0; i < 32 * 16; i++)
        im.px[i] = 0xff000000u | (uint32_t)(i * 7 & 255) << 16 | (uint32_t)(i * 3 & 255) << 8 | (uint32_t)(i & 255);
    uint8_t *jpg;
    size_t n;
    if (uimg_encode_jpeg(&im, 90, &jpg, &n) != 0) { utest_check(0, "encode the DHT fixture"); free(im.px); return; }
    uint8_t *bare = malloc(n);
    size_t m = 0;
    for (size_t o = 0; o < n;) {
        if (o + 4 <= n && jpg[o] == 0xFF && jpg[o + 1] == 0xC4) {
            o += 2 + ((size_t)jpg[o + 2] << 8 | jpg[o + 3]);
            continue;
        }
        if (o + 1 < n && jpg[o] == 0xFF && jpg[o + 1] == 0xDA) {   // the scan: copy the rest as is
            memcpy(bare + m, jpg + o, n - o);
            m += n - o;
            break;
        }
        bare[m++] = jpg[o++];
    }
    struct uimg a = {0}, b = {0};
    int ra = uimg_decode(jpg, n, &a), rb = uimg_decode(bare, m, &b);
    utest_checkf(m < n && ra == 0 && rb == 0, "a JPEG with its DHT removed still decodes -- %s", uimg_last_error());
    int same = ra == 0 && rb == 0 && a.w == b.w && a.h == b.h && !memcmp(a.px, b.px, (size_t)a.w * a.h * 4);
    utest_check(same, "...to the same picture, through Annex K's tables");
    uimg_free(&a);
    uimg_free(&b);
    free(bare);
    free(jpg);
    free(im.px);
}

static uint8_t g_ref[15 * TFRAME];

static double luma_psnr(const struct uvid_frame *f, const uint8_t *ref) {
    double se = 0;
    for (int y = 0; y < TH; y++)
        for (int x = 0; x < TW; x++) {
            int d = f->y[y * f->y_stride + x] - ref[y * TW + x];
            se += d * d;
        }
    if (se == 0) return 99.0;
    return 10.0 * log10(255.0 * 255.0 * TW * TH / se);
}

static void check_mpeg1(void) {
    utest_checkf(uvid_mpeg1_selftest() == 0, "every MPEG-1 code table is a prefix code");
    struct uvid_info in;
    int rc = uvid_load_info(TINY_MPG, &in);
    utest_checkf(rc == 0 && in.w == TW && in.h == TH && !strcmp(in.codec, "mpeg1") && !strcmp(in.container, "mpeg-ps"),
                 "tiny.mpg is 72x40 MPEG-1 in a program stream -- %s", rc ? uvid_last_error() : in.detail);
    utest_checkf(rc == 0 && in.fps_num == 25 && in.fps_den == 1, "...at 25 fps");
    utest_checkf(rc == 0 && strstr(in.audio, "MP2") && strstr(in.audio, "32 kHz mono"),
                 "...with MP2 32 kHz mono sound -- got '%s'", in.audio);

    int fd = sys_open(TINY_YUV, 0);
    long got = fd >= 0 ? (long)sys_read(fd, g_ref, sizeof g_ref) : -1;
    if (fd >= 0) sys_close(fd);
    utest_checkf(got == (long)sizeof g_ref, "tiny.yuv holds 15 reference frames -- read %ld", got);
    if (got != (long)sizeof g_ref) return;

    struct uvid *v;
    if (uvid_open(TINY_MPG, &v)) { utest_checkf(0, "tiny.mpg opens -- %s", uvid_last_error()); return; }
    const struct uvid_frame *f;
    int n = 0, types = 0, order = 1;
    double worst = 99;
    int64_t last = -1;
    while ((rc = uvid_next(v, &f)) == 1 && n < 15) {
        double p = luma_psnr(f, g_ref + (size_t)n * TFRAME);
        if (p < worst) worst = p;
        if (f->type == 'I') types |= 1;
        if (f->type == 'P') types |= 2;
        if (f->type == 'B') types |= 4;
        if (f->pts_ms <= last) order = 0;
        last = f->pts_ms;
        n++;
    }
    utest_checkf(n == 15 && uvid_next(v, &f) == 0, "fifteen frames then the end -- got %d", n);
    utest_checkf(types == 7, "I, P and B pictures all decoded");
    utest_check(order, "frames come out in display order, times rising");
    utest_checkf(worst >= MIN_PSNR, "every frame within %d dB of FFmpeg's -- worst %d.%d dB",
                 MIN_PSNR, (int)worst, (int)(worst * 10) % 10);

    // A seek lands on the frame showing then, whatever its type.
    rc = uvid_seek(v, 290, UVID_SEEK_EXACT);
    int ok = rc == 0 && uvid_next(v, &f) == 1;
    int idx = ok ? (int)((f->pts_ms - 0) / 40) : -1;
    utest_checkf(ok && f->pts_ms <= 290 && f->pts_ms + 40 > 290 && luma_psnr(f, g_ref + (size_t)idx * TFRAME) >= MIN_PSNR,
                 "a seek to 290 ms shows the frame at %lld ms, and it is that frame", ok ? (long long)f->pts_ms : -1LL);
    uvid_close(v);

    // The sound: 1 kHz at 32 kHz is 2000 zero crossings a second.
    struct usnd_stream s;
    if (usnd_open(TINY_MPG, &s) != 0) { utest_checkf(0, "usnd opens tiny.mpg's sound -- %s", usnd_last_error()); return; }
    utest_checkf(s.fmt.rate == 32000 && s.fmt.channels == 1, "32 kHz mono -- got %u/%u", s.fmt.rate, s.fmt.channels);
    static int32_t buf[4096];
    long total = 0, crossings = 0, k;
    int32_t prev = 0;
    while ((k = s.codec->read(&s, buf, 4096)) > 0) {
        for (long i = 0; i < k; i++, total++) {
            if (total >= 1152 && ((prev < 0) != (buf[i] < 0))) crossings++;
            prev = buf[i];
        }
    }
    long secs_x1000 = (total - 1152) * 1000 / 32000;
    long hz = secs_x1000 > 0 ? crossings * 1000 / 2 / secs_x1000 : 0;
    utest_checkf(total >= 32000 / 2 && hz >= 990 && hz <= 1010, "the sound is a 1 kHz tone -- %ld samples, %ld Hz", total, hz);
    usnd_close(&s);
}

int main(void) {
    utest_begin("uvid_test", "the video decode path, over /tests' fixtures", 0);
    check_avi_info();
    check_avi_frames();
    check_avi_sound();
    check_jpeg_without_dht();
    check_mpeg1();
    return utest_end();
}
