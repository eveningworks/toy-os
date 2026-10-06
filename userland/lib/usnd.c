// usnd's decode side: the codec table, and the one conversion stage
// every codec's output passes through on its way to a sink.
//
// **A CODEC NEVER SEES THE DEVICE'S RATE.** It reports what its file
// holds and yields s32 frames in it; everything below turns that into
// 48 kHz stereo. That split is why adding MP3 is a file and a row --
// and it is where PulseAudio, PipeWire and CoreAudio all put resampling.
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "lib/usnd.h"
#include "lib/usnd_internal.h"
#include "rt/sys.h"
#include "errno.h"

// One read's worth of native frames. 512 frames of stereo is 2 KiB --
// one filesystem transaction's worth, and small enough that a seek
// throws little away.
#define SRC_FRAMES 512

// Order is PROBE order, and WAV goes first because its magic is four
// bytes at offset 0 while MP3's is a sync pattern that a stray 0xFF can
// imitate -- the cheaper, stricter test should get the first look.
// MIDI's `MThd` and FLAC's `fLaC` are as strict as WAV's `RIFF`, so they
// go ahead of MP3 too.
static const struct usnd_codec *const g_codecs[] = {
    &usnd_codec_wav,
    &usnd_codec_mid,
    &usnd_codec_flac,
    &usnd_codec_mp3,
};
#define CODEC_COUNT ((int)(sizeof g_codecs / sizeof g_codecs[0]))

static const char *g_err = "";

const char *usnd_last_error(void) { return g_err; }

// Set by every failing path here and by the codecs (usnd_fail is
// exported through usnd_internal.h). A sentence, not a code: an app
// shows it, and "not a WAV file" beats "-22".
void usnd_fail(const char *msg) { g_err = msg; }

static const struct usnd_codec *codec_for(const uint8_t *d, size_t n) {
    for (int i = 0; i < CODEC_COUNT; i++)
        if (g_codecs[i]->probe(d, n)) return g_codecs[i];
    return 0;
}

int usnd_probe(const void *data, size_t n) {
    return codec_for((const uint8_t *)data, n) != 0;
}

// --- opening ----------------------------------------------------------

static int open_stream(const char *path, struct usnd_stream *s, int info_only) {
    memset(s, 0, sizeof *s);
    s->fd = -1;
    s->info_only = info_only;

    int fd = sys_open(path, 0);
    if (fd < 0) { usnd_fail("cannot open file"); return -ENOENT; }

    uint8_t head[16];
    long got = (long)sys_read(fd, head, sizeof head);
    if (got < 12) { sys_close(fd); usnd_fail("file is too short to be audio"); return -EINVAL; }

    const struct usnd_codec *c = codec_for(head, (size_t)got);
    if (!c) { sys_close(fd); usnd_fail("no codec recognises this file"); return -EINVAL; }

    sys_lseek(fd, 0, SYS_SEEK_SET);
    s->codec = c;
    s->fd = fd;
    int rc = c->open(s);
    if (rc != 0) { sys_close(fd); s->fd = -1; return rc; }

    if (s->fmt.channels != 1 && s->fmt.channels != 2) {
        c->close(s);
        sys_close(fd);
        s->fd = -1;
        usnd_fail("only mono and stereo are supported");
        return -ENOTSUP;
    }
    if (s->fmt.rate < 4000 || s->fmt.rate > 192000) {
        c->close(s);
        sys_close(fd);
        s->fd = -1;
        usnd_fail("sample rate is out of range");
        return -ENOTSUP;
    }
    // Source frames per output frame, 16.16. Exactly 1.0 for a file at
    // the output rate, which usnd_read() then takes as its copy-only
    // fast path.
    s->out_rate = USND_RATE;
    s->step = (uint32_t)(((uint64_t)s->fmt.rate << 16) / USND_RATE);
    return 0;
}

void usnd_stream_set_rate(struct usnd_stream *s, uint32_t rate) {
    if (!rate || rate == s->out_rate) return;
    uint32_t old = s->out_rate ? s->out_rate : USND_RATE;
    s->out_pos = s->out_pos * rate / old;
    s->out_rate = rate;
    s->step = (uint32_t)(((uint64_t)s->fmt.rate << 16) / rate);
}

int usnd_load_info(const char *path, struct usnd_info *out) {
    struct usnd_stream s;
    int rc = open_stream(path, &s, 1);
    if (rc != 0) return rc;

    memset(out, 0, sizeof *out);
    out->fmt = s.fmt;
    out->frames = s.frames;
    out->ms = s.fmt.rate ? (uint32_t)(s.frames * 1000ull / s.fmt.rate) : 0;
    out->format = s.codec->name;
    strlcpy(out->detail, s.detail, sizeof out->detail);

    s.codec->close(&s);
    sys_close(s.fd);
    return 0;
}

int usnd_open(const char *path, struct usnd_stream *s) {
    int rc = open_stream(path, s, 0);
    if (rc != 0) return rc;

    s->src_cap = SRC_FRAMES;
    s->src = malloc((size_t)s->src_cap * s->fmt.channels * sizeof(int32_t));
    if (!s->src) {
        s->codec->close(s);
        sys_close(s->fd);
        s->fd = -1;
        usnd_fail("out of memory");
        return -ENOMEM;
    }
    return 0;
}

void usnd_close(struct usnd_stream *s) {
    if (!s->codec) return;
    s->codec->close(s);
    if (s->fd >= 0) sys_close(s->fd);
    free(s->src);
    memset(s, 0, sizeof *s);
    s->fd = -1;
}

uint64_t usnd_stream_frames(const struct usnd_stream *s) {
    if (!s->frames || !s->fmt.rate) return 0;
    return s->frames * USND_RATE / s->fmt.rate;
}

// --- reading and converting -------------------------------------------

static int src_fill(struct usnd_stream *s) {
    if (s->src_pos < s->src_len) return 1;
    if (s->eof) return 0;
    long n = s->codec->read(s, s->src, s->src_cap);
    if (n <= 0) { s->eof = 1; s->src_len = s->src_pos = 0; return 0; }
    s->src_len = n;
    s->src_pos = 0;
    return 1;
}

// One source frame, widened to stereo. A mono file is DUPLICATED rather
// than panned: a single channel is centre by definition, and halving it
// to "keep the power" would make every mono file quieter than the
// stereo one beside it.
static int src_next(struct usnd_stream *s, int32_t out[2]) {
    if (!src_fill(s)) return 0;
    const int32_t *f = s->src + s->src_pos * s->fmt.channels;
    out[0] = f[0];
    out[1] = s->fmt.channels == 2 ? f[1] : f[0];
    s->src_pos++;
    return 1;
}

long usnd_read(struct usnd_stream *s, int32_t *dst, long frames) {
    if (!s->codec || frames <= 0 || s->drained) return 0;

    // ALREADY THE DEVICE'S RATE: no resampler in the path at all, only
    // the upmix if the file is mono. Worth the branch twice over --
    // interpolating a 48 kHz file against itself is arithmetic that can
    // only lose, and the interpolator's frame accounting is where the
    // awkward end-of-stream case lives.
    if (s->step == (1u << 16)) {
        long n = 0;
        while (n < frames) {
            if (!src_fill(s)) break;
            long take = s->src_len - s->src_pos;
            if (take > frames - n) take = frames - n;
            if (s->fmt.channels == USND_CHANNELS) {
                memcpy(dst + n * 2, s->src + s->src_pos * 2,
                       (size_t)take * 2 * sizeof(int32_t));
            } else {
                const int32_t *src = s->src + s->src_pos;
                for (long i = 0; i < take; i++)
                    dst[(n + i) * 2] = dst[(n + i) * 2 + 1] = src[i];
            }
            s->src_pos += take;
            n += take;
        }
        if (n == 0) s->drained = 1;
        s->out_pos += (uint64_t)n;
        return n;
    }

    // prev and cur are the two source frames being interpolated
    // between, and they SURVIVE ACROSS CALLS -- rebuilding them per
    // call would put a discontinuity at every refill boundary, which is
    // audible as a click at whatever rate the worker runs.
    if (!s->primed) {
        if (!src_next(s, s->prev)) { s->drained = 1; return 0; }
        if (!src_next(s, s->cur)) {
            // A one-frame source: nothing to interpolate towards, and
            // `tail` is set here so the loop emits it once rather than
            // twice.
            s->cur[0] = s->prev[0];
            s->cur[1] = s->prev[1];
            s->tail = 1;
        }
        s->phase = 0;
        s->primed = 1;
    }

    long n = 0;
    while (n < frames) {
        if (s->tail == 2) { s->drained = 1; break; }

        for (int c = 0; c < 2; c++) {
            int64_t a = s->prev[c], b = s->cur[c];
            // 64-bit because b - a alone needs 33 bits at full scale and
            // its product with the phase 49, and a wrap would be a loud
            // one. The result lies between a and b, so it fits again.
            dst[n * 2 + c] = (int32_t)(a + (((b - a) * (int64_t)s->phase) >> 16));
        }
        n++;
        s->out_pos++;

        s->phase += s->step;
        while (s->phase >= (1u << 16)) {
            s->phase -= (1u << 16);
            s->prev[0] = s->cur[0];
            s->prev[1] = s->cur[1];
            if (src_next(s, s->cur)) continue;

            // THE SOURCE RAN OUT, and the frame now in `prev` has not
            // been emitted yet. Duplicating it into `cur` gives the loop
            // one more pass to emit it; the pass after that finds tail
            // already set and stops. Returning here instead drops the
            // last frame of every file -- N in, N-1 out.
            if (s->tail) { s->tail = 2; break; }
            s->tail = 1;
            s->cur[0] = s->prev[0];
            s->cur[1] = s->prev[1];
        }
    }
    // The end of the stream is STICKY: without this a caller looping
    // until it reads 0 would get one stale frame per call, forever.
    if (n == 0) s->drained = 1;
    return n;
}

int usnd_seek(struct usnd_stream *s, uint64_t device_frame) {
    if (!s->codec) return -EINVAL;
    if (!s->codec->seek) { usnd_fail("this format cannot seek"); return -ENOTSUP; }

    uint64_t file_frame = device_frame * s->fmt.rate / USND_RATE;
    uint32_t r = s->out_rate ? s->out_rate : USND_RATE;
    int rc = s->codec->seek(s, file_frame);
    if (rc != 0) return rc;

    s->src_len = s->src_pos = 0;
    s->primed = 0;
    s->eof = 0;
    s->tail = 0;
    s->drained = 0;
    s->phase = 0;
    s->out_pos = device_frame * r / USND_RATE;
    return 0;
}

// --- an in-memory source ----------------------------------------------
//
// A codec whose "file" is a buffer the caller already holds, so that
// samples from anywhere -- a WAD lump, a synthesiser -- go through the
// SAME conversion stage as a file does, rather than growing a second
// resampler beside it.
//
// **IT IS DELIBERATELY NOT A ROW IN g_codecs.** That table is what
// usnd_probe() walks, and this claims no bytes on disk; putting it
// there would offer it to every probe for nothing. The kernel makes the
// same call about ramfs and its backend registry, for the same reason:
// a registry is a list of things every consumer of it will act on.

struct memsrc {
    const int16_t *data;     // the caller's s16; widened on the way out
    uint64_t frames, pos;
    int channels;
};

static int mem_probe(const uint8_t *d, size_t n) { (void)d; (void)n; return 0; }

static long mem_read(struct usnd_stream *s, int32_t *dst, long frames) {
    struct memsrc *m = s->priv;
    if (m->pos >= m->frames) return 0;
    if ((uint64_t)frames > m->frames - m->pos) frames = (long)(m->frames - m->pos);
    const int16_t *src = m->data + m->pos * (uint64_t)m->channels;
    for (long i = 0; i < frames * m->channels; i++) dst[i] = (int32_t)src[i] * 65536;
    m->pos += (uint64_t)frames;
    return frames;
}

static int mem_seek(struct usnd_stream *s, uint64_t frame) {
    struct memsrc *m = s->priv;
    m->pos = frame > m->frames ? m->frames : frame;
    return 0;
}

static void mem_close(struct usnd_stream *s) { free(s->priv); s->priv = 0; }

static const struct usnd_codec mem_codec = {
    .name = "pcm", .probe = mem_probe, .open = 0,
    .read = mem_read, .seek = mem_seek, .close = mem_close,
};

// --- clips ------------------------------------------------------------

int usnd_clip_drain(struct usnd_stream *s, struct usnd_clip *c) {
    memset(c, 0, sizeof *c);

    uint64_t total = usnd_stream_frames(s);
    if (!total || total > USND_CLIP_MAX_FRAMES) {
        usnd_fail(total ? "too long to load as a clip" : "clip has no samples");
        return total ? -ENOTSUP : -EINVAL;
    }
    int32_t *pcm = malloc((size_t)total * USND_CHANNELS * sizeof(int32_t));
    if (!pcm) { usnd_fail("out of memory"); return -ENOMEM; }

    // Reads until the decoder stops rather than trusting the estimate:
    // the conversion can land a frame either side of it, and never past
    // the buffer.
    uint64_t got = 0;
    while (got < total) {
        long n = usnd_read(s, pcm + got * USND_CHANNELS, (long)(total - got));
        if (n <= 0) break;
        got += (uint64_t)n;
    }
    if (!got) { free(pcm); usnd_fail("clip decoded to nothing"); return -EINVAL; }
    c->pcm = pcm;
    c->frames = got;
    return 0;
}

int usnd_clip_from_pcm(const int16_t *data, uint64_t frames, uint32_t rate,
                       int channels, struct usnd_clip *c) {
    memset(c, 0, sizeof *c);
    if (!data || !frames || (channels != 1 && channels != 2)) {
        usnd_fail("only mono and stereo PCM is supported");
        return -EINVAL;
    }
    if (rate < 4000 || rate > 192000) {
        usnd_fail("sample rate is out of range");
        return -ENOTSUP;
    }

    struct usnd_stream s;
    memset(&s, 0, sizeof s);
    s.fd = -1;                       // nothing to close
    s.codec = &mem_codec;
    s.fmt.rate = rate;
    s.fmt.channels = (uint16_t)channels;
    s.fmt.bits = 16;
    s.frames = frames;
    s.step = (uint32_t)(((uint64_t)rate << 16) / USND_RATE);
    s.src_cap = SRC_FRAMES;
    s.src = malloc((size_t)s.src_cap * channels * sizeof(int32_t));
    struct memsrc *m = malloc(sizeof *m);
    if (!s.src || !m) {
        free(s.src); free(m);
        usnd_fail("out of memory");
        return -ENOMEM;
    }
    m->data = data; m->frames = frames; m->pos = 0; m->channels = channels;
    s.priv = m;

    int rc = usnd_clip_drain(&s, c);
    usnd_close(&s);
    return rc;
}
