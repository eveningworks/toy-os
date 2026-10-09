// vplay -- a video file from a prompt: what it is, and whether every
// frame decodes. The text half of the Video Player, the way `aplay` is
// the Audio Player's; it is how a test (or a person over the serial
// console) checks lib/uvid.h without a window.
//
// --check prints a CRC-32 per frame, so two builds can be compared frame
// by frame; --sync measures how far apart the container put each white
// frame and its beep in av-sync.mpg -- a muxing fact, not a playback one.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt/sys.h"
#include "kcrc.h"
#include "lib/uargs.h"
#include "lib/uduration.h"
#include "lib/uimg.h"
#include "lib/usnd.h"
#include "lib/uvid.h"

static int o_info, o_check, o_bench, o_sync;
static const char *o_at, *o_frames, *o_save;

static const struct uargs_opt OPTS[] = {
    { "info",   'i', 0,      "what the file says it is; decodes nothing (the default)", &o_info, 0 },
    { "check",  'c', 0,      "decode every frame: its time, type and a CRC-32 of its pixels", &o_check, 0 },
    { "bench",  'b', 0,      "decode as fast as it goes and print frames a second", &o_bench, 0 },
    { "sync",   's', 0,      "where each white frame and each beep fall, and how far apart", &o_sync, 0 },
    { "at",     't', "TIME", "start at TIME (90s, 1.5m) -- or the frame --save writes", 0, &o_at },
    { "frames", 'n', "N",    "stop after N frames", 0, &o_frames },
    { "save",   'o', "FILE", "write the frame at --at as a picture (.png, .jpg, .qoi)", 0, &o_save },
    { 0 },
};

static const struct uargs_prog PROG = {
    .name = "vplay",
    .usage = "[OPTION]... FILE",
    .summary = "Describe a video file, decode its frames, or save one as a picture.",
    .opts = OPTS,
    .notes = "Plays nothing: the Video Player does. TIME takes sleep's units\n"
             "(2, 0.5, 90s, 1.5m). Exit status is 1 when a frame fails to decode,\n"
             "or when --sync finds a beep more than a frame from its flash.",
};

static uint32_t frame_crc(const struct uvid_frame *f) {
    uint32_t crc = 0;
    if (f->fmt == UVID_ARGB) {
        for (int y = 0; y < f->h; y++)
            crc = kcrc32_update(crc, f->argb + (size_t)y * (size_t)f->argb_stride, (size_t)f->w * 4);
        return crc;
    }
    int cw = (f->w + 1) / 2, chh = (f->h + 1) / 2;
    for (int y = 0; y < f->h; y++)
        crc = kcrc32_update(crc, f->y + (size_t)y * (size_t)f->y_stride, (size_t)f->w);
    for (int y = 0; y < chh; y++)
        crc = kcrc32_update(crc, f->cb + (size_t)y * (size_t)f->c_stride, (size_t)cw);
    for (int y = 0; y < chh; y++)
        crc = kcrc32_update(crc, f->cr + (size_t)y * (size_t)f->c_stride, (size_t)cw);
    return crc;
}

// The frame's mean brightness, 0..255 -- what --sync calls a flash.
static int frame_luma(const struct uvid_frame *f) {
    uint64_t sum = 0;
    for (int y = 0; y < f->h; y += 4)
        for (int x = 0; x < f->w; x += 4) {
            if (f->fmt == UVID_ARGB) {
                uint32_t p = f->argb[(size_t)y * (size_t)f->argb_stride + (size_t)x];
                sum += ((p >> 16 & 255) * 77 + (p >> 8 & 255) * 150 + (p & 255) * 29) >> 8;
            } else {
                sum += f->y[(size_t)y * (size_t)f->y_stride + (size_t)x];
            }
        }
    uint64_t n = (uint64_t)((f->h + 3) / 4) * (uint64_t)((f->w + 3) / 4);
    return n ? (int)(sum / n) : 0;
}

static void print_info(const char *path, const struct uvid_info *in) {
    char len[16];
    uduration_clock(in->ms, len, sizeof len);
    printf("%s\n", path);
    printf("  container  %s\n", in->container);
    printf("  video      %s, %dx%d", in->detail, in->w, in->h);
    if (in->fps_num && in->fps_den) {
        uint32_t c = (uint32_t)(100ull * in->fps_num / in->fps_den);
        printf(", %u.%02u fps", c / 100, c % 100);
    }
    printf("\n  sound      %s\n", in->audio[0] ? in->audio : "none");
    printf("  length     %s (%u ms, %u frames)\n", len, in->ms, in->frames);
    printf("  bitrate    %u kbit/s\n", in->kbps);
}

#define MAX_MARKS 256

// The time of every beep's start in the file's sound, in ms: a sample
// above a quarter of full scale after 200 ms of near-silence (or at the
// very start).
static int find_beeps(const char *path, int64_t *at, int max) {
    struct usnd_stream s;
    if (usnd_open(path, &s) != 0) return -1;
    static int32_t buf[4096 * 2];
    uint64_t pos = 0, quiet = USND_RATE;   // the file starts as if after silence
    int n = 0;
    long got;
    while ((got = usnd_read(&s, buf, 4096)) > 0) {
        for (long i = 0; i < got; i++, pos++) {
            int32_t v = buf[2 * i];
            if (v < 0) v = -v;
            if (v > (1 << 29)) {
                if (quiet >= USND_RATE / 5 && n < max) at[n++] = (int64_t)(pos * 1000 / USND_RATE);
                quiet = 0;
            } else if (v < (1 << 24)) {
                quiet++;
            }
        }
    }
    usnd_close(&s);
    return n;
}

static int sync_report(const char *path, const int64_t *flash, int nf) {
    static int64_t beep[MAX_MARKS];
    int nb = find_beeps(path, beep, MAX_MARKS);
    if (nb < 0) { fprintf(stderr, "vplay: %s: no sound to compare: %s\n", path, usnd_last_error()); return 1; }
    printf("%d flashes, %d beeps\n", nf, nb);
    int bad = 0, worst = 0;
    for (int i = 0; i < nf; i++) {
        int best = -1;
        for (int k = 0; k < nb; k++)
            if (best < 0 || llabs(beep[k] - flash[i]) < llabs(beep[best] - flash[i])) best = k;
        if (best < 0) { printf("  flash %lld ms: no beep\n", (long long)flash[i]); bad = 1; continue; }
        int off = (int)(beep[best] - flash[i]);
        printf("  flash %6lld ms  beep %6lld ms  %+d ms\n", (long long)flash[i], (long long)beep[best], off);
        if (abs(off) > abs(worst)) worst = off;
        if (abs(off) > 40) bad = 1;
    }
    printf("worst offset %+d ms\n", worst);
    return bad;
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (a.argc != 1) return uargs_error(&PROG, a.argc ? "one FILE at a time" : "missing FILE");
    const char *path = a.argv[0];

    uint64_t at_ms = 0, limit = 0;
    if (o_at && !uduration_parse_ms(o_at, &at_ms)) return uargs_error(&PROG, "bad TIME '%s'", o_at);
    if (o_frames) {
        char *end;
        limit = strtoull(o_frames, &end, 10);
        if (*end || !limit) return uargs_error(&PROG, "bad frame count '%s'", o_frames);
    }

    if (o_save) {
        struct uimg im;
        if (uvid_still(path, (uint32_t)at_ms, 0, 0, &im) != 0) {
            fprintf(stderr, "vplay: %s: %s\n", path, uvid_last_error());
            return 1;
        }
        int rc = uimg_save(o_save, &im, 0);
        uimg_free(&im);
        if (rc) { fprintf(stderr, "vplay: %s: %s\n", o_save, uimg_last_error()); return 1; }
        printf("%s\n", o_save);
        return 0;
    }

    struct uvid *v;
    int rc = uvid_open(path, &v);
    if (rc) { fprintf(stderr, "vplay: %s: %s\n", path, uvid_last_error()); return 1; }
    if (!o_check && !o_bench && !o_sync) {
        print_info(path, uvid_info(v));
        uvid_close(v);
        return 0;
    }
    if (at_ms && uvid_seek(v, (uint32_t)at_ms, UVID_SEEK_EXACT) < 0) {
        fprintf(stderr, "vplay: %s: %s\n", path, uvid_last_error());
        uvid_close(v);
        return 1;
    }

    static int64_t flash[MAX_MARKS];
    int nflash = 0, was_bright = 0, status = 0;
    uint64_t n = 0, t0 = sys_monotonic_ns();
    const struct uvid_frame *f;
    while ((!limit || n < limit) && (rc = uvid_next(v, &f)) == 1) {
        if (o_check)
            printf("%5llu %8lld ms %c %08x\n", (unsigned long long)n, (long long)f->pts_ms, f->type,
                   (unsigned)frame_crc(f));
        if (o_sync) {
            int bright = frame_luma(f) > 128;
            if (bright && !was_bright && nflash < MAX_MARKS) flash[nflash++] = f->pts_ms;
            was_bright = bright;
        }
        n++;
    }
    uint64_t ns = sys_monotonic_ns() - t0;
    if (rc < 0) {
        fprintf(stderr, "vplay: %s: frame %llu: %s\n", path, (unsigned long long)n, uvid_last_error());
        status = 1;
    }
    if (o_check || o_bench) {
        uint64_t centi = ns ? n * 100000000000ull / ns : 0;
        printf("%llu frames in %llu ms, %llu.%02llu fps\n", (unsigned long long)n,
               (unsigned long long)(ns / 1000000), (unsigned long long)(centi / 100),
               (unsigned long long)(centi % 100));
    }
    uvid_close(v);
    if (o_sync && sync_report(path, flash, nflash)) status = 1;
    return status;
}
