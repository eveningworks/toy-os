// usnd's decode side: the codec table, and the one conversion stage
// every codec's output passes through on its way to a sink.
//
// **A CODEC NEVER SEES THE DEVICE'S RATE.** It reports what its file
// holds and yields s16 frames in it; everything below turns that into
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

static const struct usnd_codec *const g_codecs[] = {
    &usnd_codec_wav,
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

static int open_stream(const char *path, struct usnd_stream *s) {
    memset(s, 0, sizeof *s);
    s->fd = -1;

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
    // Source frames per output frame, 16.16. Exactly 1.0 for a 48 kHz
    // file, which usnd_read() then takes as its copy-only fast path.
    s->step = (uint32_t)(((uint64_t)s->fmt.rate << 16) / USND_RATE);
    return 0;
}

int usnd_load_info(const char *path, struct usnd_info *out) {
    struct usnd_stream s;
    int rc = open_stream(path, &s);
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
    int rc = open_stream(path, s);
    if (rc != 0) return rc;

    s->src_cap = SRC_FRAMES;
    s->src = malloc((size_t)s->src_cap * s->fmt.channels * sizeof(int16_t));
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
static int src_next(struct usnd_stream *s, int16_t out[2]) {
    if (!src_fill(s)) return 0;
    const int16_t *f = s->src + s->src_pos * s->fmt.channels;
    out[0] = f[0];
    out[1] = s->fmt.channels == 2 ? f[1] : f[0];
    s->src_pos++;
    return 1;
}

long usnd_read(struct usnd_stream *s, int16_t *dst, long frames) {
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
                       (size_t)take * 2 * sizeof(int16_t));
            } else {
                const int16_t *src = s->src + s->src_pos;
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
            int32_t a = s->prev[c], b = s->cur[c];
            // 64-bit because (b - a) * phase overflows 32 bits at full
            // scale, and the wrap would be a loud one.
            dst[n * 2 + c] = (int16_t)(a + (int32_t)(((int64_t)(b - a) * s->phase) >> 16));
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
    int rc = s->codec->seek(s, file_frame);
    if (rc != 0) return rc;

    s->src_len = s->src_pos = 0;
    s->primed = 0;
    s->eof = 0;
    s->tail = 0;
    s->drained = 0;
    s->phase = 0;
    s->out_pos = device_frame;
    return 0;
}
