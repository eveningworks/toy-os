#ifndef USND_H
#define USND_H

#include <stdint.h>
#include <stddef.h>

// usnd -- decoding and playing audio files, in RING 3.
//
// The kernel gives out ONE exclusive PCM stream and never mixes
// (docs/decisions/drivers.md, "Sound: one exclusive stream over a
// shared ring"). Everything above that line is this library: the file
// formats, the rate/channel conversion, the mixing of several sounds
// into the one stream, and the thread that keeps it fed. Nothing here
// is in the kernel, for the same reason lib/uimg.h is not -- ALSA's
// dmix, PulseAudio, PipeWire and Windows' audio engine are all
// userspace, and mixing drags resampling and format policy with it.
//
// THREE SEAMS, and each one is the extension point for a thing that is
// not built yet:
//
//   CODECS -- a `struct usnd_codec` row (probe/open/read/seek/close).
//   WAV today; MP3 is a file and a row. The registry shape
//   display_driver, block_device and uimg_codec already use here.
//
//   THE SINK -- where mixed samples GO (usnd_sink.h). One row today,
//   the exclusive device. A system-wide sound daemon is a second row,
//   which is why no part of this header mentions the ring, `hw_pos` or
//   SND_*: an app says "play this file", never "write here", so the
//   backend can change under it the way libasound's apps kept working
//   when PulseAudio arrived.
//
//   VOICES -- the per-process mixer below. With a daemon it becomes
//   this app's submix (a PulseAudio sink input); without one it is the
//   only mixer there is.
//
// **A CODEC NEVER RESAMPLES.** It reports its file's native rate and
// channel count and hands out s16 frames in it; the library converts
// once, in one place. Every real system puts that stage in the
// server or the HAL, never in the decoder.

// What a sink takes and what usnd_read() hands back: 16-bit signed
// stereo at 48 kHz. Deliberately equal to the kernel ABI's values but
// spelled separately -- a daemon sink would negotiate its own, and a
// codec must not learn the device's numbers.
#define USND_RATE     48000
#define USND_CHANNELS 2

// Refusals are negative errnos, uimg.h's three-way split and for the
// same reason: -EINVAL means the file is broken, -ENOTSUP means the
// file is fine and this build will not play it (8 channels, a
// compressed WAV), -ENOMEM means it did not fit. An app can say "this
// build cannot play that" instead of calling a good file corrupt.
const char *usnd_last_error(void);

struct usnd_format {
    uint32_t rate;      // the FILE's rate, not the device's
    uint16_t channels;  // 1 or 2; more is -ENOTSUP
    uint16_t bits;      // bits per sample in the file, informational
};

struct usnd_info {
    struct usnd_format fmt;
    uint64_t frames;        // in the file's own rate; 0 when unknown
    uint32_t ms;            // duration; 0 when unknown
    const char *format;     // the codec's name, e.g. "wav"
    char detail[48];        // codec-specific, e.g. "PCM 16-bit stereo"
};

struct usnd_stream;

struct usnd_codec {
    const char *name;

    // Magic bytes only -- this runs on every codec in turn, so it must
    // be cheap and must not be clever.
    int (*probe)(const uint8_t *d, size_t n);

    // `s->fd` is open and positioned at 0. Fill s->fmt, s->frames and
    // s->detail, allocate s->priv. 0 or a negative errno.
    int (*open)(struct usnd_stream *s);

    // Up to `frames` frames in the file's OWN rate and channel count,
    // interleaved s16. Returns the count, 0 at end of file, negative on
    // error. Short reads are legal and are not the end.
    long (*read)(struct usnd_stream *s, int16_t *dst, long frames);

    // Reposition to a frame in the file's own rate. -ENOTSUP is a fine
    // answer for a format that cannot.
    int (*seek)(struct usnd_stream *s, uint64_t frame);

    void (*close)(struct usnd_stream *s);
};

// One decoded file being read. Small enough to be a local: the
// conversion buffers are malloc'd by usnd_open() and released by
// usnd_close(), so `sizeof` here stays well inside the 2 KiB ring-3
// frame budget.
struct usnd_stream {
    const struct usnd_codec *codec;
    int fd;
    struct usnd_format fmt;
    uint64_t frames;            // file frames, 0 when unknown
    void *priv;                 // the codec's
    char detail[48];

    // --- the conversion stage, the library's alone ------------------
    //
    // Linear interpolation over a 16.16 phase accumulator. `prev` and
    // `cur` are the two source frames being interpolated BETWEEN and
    // must survive across calls, or every refill would click at its
    // own boundary.
    int16_t *src;               // native-format staging, malloc'd
    long src_cap, src_len, src_pos;
    uint32_t step;              // source frames per output frame, 16.16
    uint32_t phase;
    int16_t prev[2], cur[2];
    int primed;
    int eof;        // the CODEC has no more frames
    // How far the interpolator has got through the LAST source frame:
    // 0 normal, 1 = the source ran out and `cur` was duplicated so that
    // final frame still gets emitted, 2 = it has been. Without the
    // middle state a file's last frame is silently dropped, which for
    // an exact-rate mono file means N frames in and N-1 out.
    int tail;
    int drained;    // ...and nothing more will ever come out
    uint64_t out_pos;           // device frames handed out so far
};

// Does any codec claim these bytes? 16 bytes is more than any probe
// here needs.
int usnd_probe(const void *data, size_t n);

// Header only, from a path. Cheap: no samples are decoded.
int usnd_load_info(const char *path, struct usnd_info *out);

// Open for reading. On success the caller owes a usnd_close().
int usnd_open(const char *path, struct usnd_stream *s);

// Up to `frames` frames in the DEVICE format -- 48 kHz stereo s16,
// whatever the file was. Returns the count written, 0 at end of
// stream, negative on error.
long usnd_read(struct usnd_stream *s, int16_t *dst, long frames);

// Seek, in DEVICE frames. Returns 0, or a negative errno (-ENOTSUP
// from a codec that cannot seek).
int usnd_seek(struct usnd_stream *s, uint64_t device_frame);

// The stream's length in DEVICE frames, 0 when the codec did not know.
uint64_t usnd_stream_frames(const struct usnd_stream *s);

void usnd_close(struct usnd_stream *s);

// The codec table. Adding MP3 is one .c file and one row in usnd.c.
extern const struct usnd_codec usnd_codec_wav;

// --- playback ---------------------------------------------------------
//
// One device, N voices, mixed here. usnd_init() opens the sink and
// starts a worker thread that decodes, mixes and refills; the thread
// touches nothing but its own state, which is what lets a GUI client
// call these from callbacks without a dropout when a repaint is slow
// (ui/uapp.h's worker-thread rule).

// VOICE 0 IS THE STREAMING ONE -- usnd_play()'s file. The rest are
// clips. Eight is what a game's overlapping effects need and is one
// 2 KiB scratch buffer's worth of mixing per chunk.
#define USND_VOICES 8

// Opens the sink and starts the worker. 0 on success; a negative errno
// otherwise, and -ENODEV (no hardware) and -EBUSY (another process
// holds the stream) are BOTH ordinary: an app that wants sound if it
// can get it calls this, ignores the result, and plays into silence.
int  usnd_init(void);
void usnd_shutdown(void);
int  usnd_ready(void);            // did init succeed
const char *usnd_sink_name(void); // "device", or a daemon's name later

// A short sound, decoded ENTIRELY into memory in the device format --
// which is what makes it free to fire off and overlap. usnd_clip_load()
// refuses anything over USND_CLIP_MAX_FRAMES rather than filling the
// heap with a song somebody passed by mistake.
#define USND_CLIP_MAX_FRAMES (USND_RATE * 10)

struct usnd_clip {
    int16_t *pcm;       // frames * USND_CHANNELS samples
    uint64_t frames;
};

int  usnd_clip_load(const char *path, struct usnd_clip *c);
void usnd_clip_free(struct usnd_clip *c);

// Starts `c` on a free voice. `gain` is 0..256 (256 = unity), applied
// on top of the master volume. Returns 0, or -1 when every voice is
// busy -- which is not an error: dropping the quietest new sound is
// what every game audio engine does when it runs out of channels.
// The clip must outlive the sound; usnd_clip_free() stops it first.
int  usnd_clip_play(const struct usnd_clip *c, int gain);

// The streaming voice. usnd_play() replaces whatever was playing.
int  usnd_play(const char *path);
void usnd_stop(void);
void usnd_set_paused(int paused);
int  usnd_paused(void);
int  usnd_playing(void);          // 1 while the streaming voice has audio left

// Waits for everything already queued to be played. A program that
// exits (or calls usnd_shutdown()) without this cuts off the last
// fraction of a second of whatever it played.
void usnd_drain(void);

// Both in DEVICE frames; duration is 0 when the codec did not know.
uint64_t usnd_position(void);
uint64_t usnd_duration(void);
int      usnd_seek_to(uint64_t device_frame);

// This process's own gain, 0..100 -- a PulseAudio stream volume, NOT
// the `volume` SETTING, which is the card's master and belongs to
// System Settings. An app turning its own playback down must not move
// a system-wide knob.
void usnd_set_volume(int pct);
int  usnd_volume(void);

#endif
