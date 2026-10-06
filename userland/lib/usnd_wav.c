// The WAV codec: RIFF/WAVE with uncompressed integer PCM.
//
// WHAT IT REFUSES, and the distinction matters because usnd reports
// them differently: a file that is not RIFF/WAVE at all is -EINVAL,
// while IEEE float samples and every compressed WAVE_FORMAT (ADPCM,
// mu-law) are -ENOTSUP -- a good file this build will not play. Float
// samples are refused because nothing has needed them, NOT because this
// ring lacks floating point: it has it (kernel/arch/x86_64/fpu.c enables
// SSE per process) and usnd_mp3.c's filterbank is built on it.
//
// Chunks are WALKED, never assumed: `fmt ` is not required to come
// first and `data` is routinely followed by LIST/INFO metadata. A
// reader that seeks to a fixed offset works on what it was tested with
// and fails on everything a real recorder writes.
#include <string.h>
#include "lib/ubytes.h"
#include <stdio.h>
#include <stdlib.h>
#include "lib/usnd.h"
#include "lib/usnd_internal.h"
#include "rt/sys.h"
#include "errno.h"

#define WAVE_FORMAT_PCM        0x0001
#define WAVE_FORMAT_IEEE_FLOAT 0x0003
#define WAVE_FORMAT_EXTENSIBLE 0xFFFE

// Frames converted per pass. 256 frames of 32-bit stereo is 2 KiB, the
// staging buffer below, and it bounds one read whatever the caller asks
// for.
#define WAV_SLICE 256

struct wav {
    long data_off;      // byte offset of the first sample
    uint64_t frames;    // usable frames in the data chunk
    uint64_t cur;       // next frame to read
    int block;          // bytes per frame in the file
    int bits;
    int channels;
    uint8_t *buf;       // WAV_SLICE * block bytes
};


static int wav_probe(const uint8_t *d, size_t n) {
    return n >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WAVE", 4) == 0;
}

static int wav_open(struct usnd_stream *s) {
    uint8_t hdr[12];
    if (sys_read(s->fd, hdr, sizeof hdr) != (long long)sizeof hdr ||
        !wav_probe(hdr, sizeof hdr)) {
        usnd_fail("not a RIFF/WAVE file");
        return -EINVAL;
    }

    int have_fmt = 0;
    uint16_t fmt_tag = 0;
    long pos = 12;
    struct wav w;
    memset(&w, 0, sizeof w);

    // The chunk walk. A chunk's body is padded to an even length and the
    // pad byte is NOT in its size -- skipping it is what keeps every
    // later chunk aligned, and an odd-length one is common (a LIST/INFO
    // string).
    for (;;) {
        uint8_t ch[8];
        if (sys_lseek(s->fd, pos, SYS_SEEK_SET) < 0) break;
        if (sys_read(s->fd, ch, sizeof ch) != (long long)sizeof ch) break;
        uint32_t size = ub_le32(ch + 4);

        if (memcmp(ch, "fmt ", 4) == 0 && size >= 16) {
            uint8_t f[40];
            uint32_t want = size > sizeof f ? (uint32_t)sizeof f : size;
            if (sys_read(s->fd, f, want) != (long long)want) break;
            fmt_tag = ub_le16(f);
            w.channels = ub_le16(f + 2);
            s->fmt.rate = ub_le32(f + 4);
            w.block = ub_le16(f + 12);
            w.bits = ub_le16(f + 14);
            // EXTENSIBLE hides the real format in the first two bytes of
            // its SubFormat GUID, which is where a 24-bit recording from
            // most hardware ends up.
            if (fmt_tag == WAVE_FORMAT_EXTENSIBLE && want >= 26)
                fmt_tag = ub_le16(f + 24);
            have_fmt = 1;
        } else if (memcmp(ch, "data", 4) == 0) {
            w.data_off = pos + 8;
            w.frames = size;   // bytes for now; frames once block is known
            if (have_fmt) break;
        }
        pos += 8 + (long)size + (size & 1);
    }

    if (!have_fmt || !w.data_off) { usnd_fail("WAV file has no fmt or data chunk"); return -EINVAL; }
    if (fmt_tag == WAVE_FORMAT_IEEE_FLOAT) { usnd_fail("float WAV is not supported"); return -ENOTSUP; }
    if (fmt_tag != WAVE_FORMAT_PCM) { usnd_fail("compressed WAV is not supported"); return -ENOTSUP; }
    if (w.bits != 8 && w.bits != 16 && w.bits != 24 && w.bits != 32) {
        usnd_fail("only 8, 16, 24 and 32-bit PCM is supported");
        return -ENOTSUP;
    }
    if (w.channels < 1) { usnd_fail("WAV file declares no channels"); return -EINVAL; }

    // A declared block align is trusted only when it agrees with the
    // rest of the header: some writers leave it zero.
    int expect = w.channels * (w.bits / 8);
    if (w.block != expect) w.block = expect;

    // The data chunk's size is CLAMPED to what the file actually holds.
    // A truncated recording declares its intended length, and believing
    // it means reading past the end for the whole tail.
    long long end = sys_lseek(s->fd, 0, SYS_SEEK_END);
    if (end > w.data_off && (uint64_t)(end - w.data_off) < w.frames)
        w.frames = (uint64_t)(end - w.data_off);
    w.frames /= (uint64_t)w.block;

    struct wav *p = malloc(sizeof *p);
    if (!p) { usnd_fail("out of memory"); return -ENOMEM; }
    *p = w;
    p->buf = malloc((size_t)WAV_SLICE * w.block);
    if (!p->buf) { free(p); usnd_fail("out of memory"); return -ENOMEM; }

    s->priv = p;
    s->fmt.channels = (uint16_t)w.channels;
    s->fmt.bits = (uint16_t)w.bits;
    s->frames = w.frames;
    snprintf(s->detail, sizeof s->detail, "PCM %d-bit %s %u Hz",
             w.bits, w.channels == 1 ? "mono" : "stereo", s->fmt.rate);

    sys_lseek(s->fd, p->data_off, SYS_SEEK_SET);
    return 0;
}

// One sample, whatever its width, as s32 with full scale in the top
// bits -- every bit the file has, which is the point of a 24-bit WAV.
static int32_t sample_of(const uint8_t *b, int bits) {
    switch (bits) {
    case 8:  return ((int32_t)b[0] - 128) * 16777216; // 8-bit WAV is UNSIGNED; * not <<, it goes negative
    case 16: return (int32_t)(((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 24));
    case 24: return (int32_t)(((uint32_t)b[0] << 8) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 24));
    default: return (int32_t)((uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                              ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
    }
}

static long wav_read(struct usnd_stream *s, int32_t *dst, long frames) {
    struct wav *p = s->priv;
    if (p->cur >= p->frames) return 0;

    if ((uint64_t)frames > p->frames - p->cur) frames = (long)(p->frames - p->cur);
    if (frames > WAV_SLICE) frames = WAV_SLICE;

    long want = frames * p->block;
    long long got = sys_read(s->fd, p->buf, (size_t)want);
    if (got <= 0) return 0;
    frames = (long)(got / p->block);
    if (frames <= 0) return 0;

    int step = p->bits / 8;
    for (long f = 0; f < frames; f++)
        for (int c = 0; c < p->channels; c++)
            dst[f * p->channels + c] = sample_of(p->buf + f * p->block + c * step, p->bits);

    p->cur += (uint64_t)frames;
    return frames;
}

static int wav_seek(struct usnd_stream *s, uint64_t frame) {
    struct wav *p = s->priv;
    if (frame > p->frames) frame = p->frames;
    if (sys_lseek(s->fd, p->data_off + (long long)(frame * (uint64_t)p->block),
                  SYS_SEEK_SET) < 0)
        return -EIO;
    p->cur = frame;
    return 0;
}

static void wav_close(struct usnd_stream *s) {
    struct wav *p = s->priv;
    if (!p) return;
    free(p->buf);
    free(p);
    s->priv = 0;
}

const struct usnd_codec usnd_codec_wav = {
    .name  = "wav",
    .probe = wav_probe,
    .open  = wav_open,
    .read  = wav_read,
    .seek  = wav_seek,
    .close = wav_close,
};
