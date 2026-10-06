// usnd's playback side: the voices, the mixer, and the worker thread
// that keeps the sink fed.
//
// **THE MIXER IS PER PROCESS, BECAUSE THE STREAM IS EXCLUSIVE.** The
// kernel hands out one and never mixes (docs/decisions/drivers.md), so
// two audible apps at once needs a sound daemon owning the stream --
// which is what usnd_sink.h's table is for. When that arrives this code
// does not change: it becomes this app's submix, a PulseAudio sink
// input, and the daemon sums across apps.
//
// **THE WORKER TOUCHES NOTHING A CALLER OWNS.** It holds one mutex over
// the voice table and the stream while it mixes, and it never calls
// back into an app -- so a GUI client keeps its audio through a slow
// repaint, which is the whole reason it is a thread and not an on_tick
// (ui/uapp.h's worker rule).
//
// **AN IDLE MIXER PINS ITS CURSOR RATHER THAN WRITING SILENCE.**
// Simply not writing would leave the write cursor where the hardware
// left it, and the next sound would start a whole ring late; writing
// silence instead keeps the cursor moving but makes "how much is
// queued" meaningless, which is what the position readout is computed
// from -- it would run BACKWARDS while paused. So the worker stops
// writing, lets the tail drain, and then flushes each pass to hold the
// cursor just behind the hardware.
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include "lib/usnd.h"
#include "lib/usnd_internal.h"
#include "lib/usnd_sink.h"
#include "rt/sys.h"
#include "errno.h"

// One mixing pass. 1024 frames is ~21 ms -- twice the worker's cadence,
// so a late wake-up still lands inside the ring's depth.
#define MIX_FRAMES 1024
#define WORKER_MS  10

// Unity gain for a voice, as a shift rather than a divide.
#define GAIN_UNITY 256

struct voice {
    const int32_t *pcm;     // caller-owned clip samples, at USND_RATE
    uint64_t frames, pos;
    uint32_t frac;          // the converter's phase into `pos`, 16.16
    int gain_l, gain_r;
    // Bumped every time the slot is handed out. A handle carries it, so
    // a caller holding one for a sound that has since finished cannot
    // steer whatever landed in the slot afterwards.
    unsigned gen;
    int active;
};

static const struct usnd_sink *g_sink;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_worker;
static int g_ready, g_quit;
static int g_volume = 100;

static struct voice g_voices[USND_VOICES];

// THE RATE THE SINK AGREED, what every pass is mixed at -- 0 until it
// has answered. The streaming voice is read at it directly; clips and
// the pushed source are at USND_RATE and converted (add_converted()).
static uint32_t g_rate;

static struct usnd_stream g_st;
static int g_st_open, g_st_paused, g_st_done;

// The caller-fed source. A plain SPSC ring: the producer writes at
// `head`, the mixer reads at `tail`, and neither ever writes the
// other's index.
static int32_t *g_push;     // widened from the producer's s16 as it is queued
static long g_push_head, g_push_tail;
static int g_push_on;
static int g_push_gain = 256;
static uint32_t g_push_frac;

// .bss, not stack: the ring-3 frame budget is 2 KiB and these are 20.
// 64-bit, because one full-scale s32 voice already fills 32.
static int64_t g_acc[MIX_FRAMES * USND_CHANNELS];
static int32_t g_scratch[MIX_FRAMES * USND_CHANNELS];
static int32_t g_out[MIX_FRAMES * USND_CHANNELS];

// THE TAP: the last TAP_FRAMES frames handed to the sink, so usnd_peek()
// can return what is PLAYING -- `pending()` behind the newest written.
// Larger than any sink ring here, or the playing frames would already
// be overwritten.
// Kept NARROWED to s16: it is only ever read by usnd_peek(), whose
// callers' arithmetic is scaled for 16 bits, and at 32768 frames the
// difference is 128 KiB of .bss.
#define TAP_FRAMES 32768
static int16_t g_tap[TAP_FRAMES * USND_CHANNELS];
static uint64_t g_tap_written;

static void tap_store(const int32_t *src, long frames) {
    for (long i = 0; i < frames; i++) {
        long at = (long)(g_tap_written++ % TAP_FRAMES) * USND_CHANNELS;
        g_tap[at] = (int16_t)(src[i * USND_CHANNELS] >> 16);
        g_tap[at + 1] = (int16_t)(src[i * USND_CHANNELS + 1] >> 16);
    }
}

// --- mixing -----------------------------------------------------------

// PER CHANNEL, because panning is a different gain left and right --
// which is the whole of what a game means by "the sound is over there".
static void add_voice(const int32_t *src, long frames, int gain_l, int gain_r) {
    for (long f = 0; f < frames; f++) {
        g_acc[f * 2]     += ((int64_t)src[f * 2]     * gain_l) >> 8;
        g_acc[f * 2 + 1] += ((int64_t)src[f * 2 + 1] * gain_r) >> 8;
    }
}

// A USND_RATE source mixed into a pass at g_rate: linear interpolation
// over a 16.16 phase, as usnd_read() does for a file. `at(i)` is the
// i-th frame from the source's current position, `avail` how many there
// are. Returns the whole source frames consumed; `*frac` keeps the rest.
static long add_converted(const int32_t *(*at)(const void *ctx, uint64_t i), const void *ctx,
                          uint64_t avail, uint32_t *frac, long frames, int gain_l, int gain_r) {
    uint32_t step = (uint32_t)(((uint64_t)USND_RATE << 16) / g_rate);
    uint64_t pos = 0;
    uint32_t ph = *frac;
    for (long f = 0; f < frames && pos < avail; f++) {
        const int32_t *a = at(ctx, pos), *b = pos + 1 < avail ? at(ctx, pos + 1) : a;
        for (int c = 0; c < 2; c++) {
            int64_t v = a[c] + ((((int64_t)b[c] - a[c]) * ph) >> 16);
            g_acc[f * 2 + c] += (v * (c ? gain_r : gain_l)) >> 8;
        }
        ph += step;
        pos += ph >> 16;
        ph &= 0xFFFF;
    }
    *frac = ph;
    return (long)pos;
}

static const int32_t *clip_at(const void *ctx, uint64_t i) {
    const struct voice *vo = ctx;
    return vo->pcm + (vo->pos + i) * USND_CHANNELS;
}

static const int32_t *push_at(const void *ctx, uint64_t i) {
    (void)ctx;
    return g_push + ((g_push_tail + (long)i) % USND_PUSH_FRAMES) * USND_CHANNELS;
}

// Sums every active voice into `g_out`. Always produces `frames` frames;
// with nothing playing they are zeroes.
static void mix(long frames) {
    memset(g_acc, 0, (size_t)frames * USND_CHANNELS * sizeof g_acc[0]);

    if (g_st_open && !g_st_paused && !g_st_done) {
        long n = usnd_read(&g_st, g_scratch, frames);
        if (n <= 0) g_st_done = 1;
        else add_voice(g_scratch, n, GAIN_UNITY, GAIN_UNITY);
    }

    for (int v = 0; v < USND_VOICES; v++) {
        struct voice *vo = &g_voices[v];
        if (!vo->active) continue;
        if (g_rate != USND_RATE) {
            vo->pos += (uint64_t)add_converted(clip_at, vo, vo->frames - vo->pos, &vo->frac,
                                               frames, vo->gain_l, vo->gain_r);
            if (vo->pos >= vo->frames) vo->active = 0;
            continue;
        }
        long n = frames;
        if ((uint64_t)n > vo->frames - vo->pos) n = (long)(vo->frames - vo->pos);
        add_voice(vo->pcm + vo->pos * USND_CHANNELS, n, vo->gain_l, vo->gain_r);
        vo->pos += (uint64_t)n;
        if (vo->pos >= vo->frames) vo->active = 0;
    }

    // The pushed source. An UNDERRUN IS SILENCE, not a stall: a
    // synthesiser that fell behind should leave a gap and carry on,
    // never make the mixer wait on it.
    if (g_push_on && g_push && g_rate != USND_RATE) {
        long avail = g_push_head - g_push_tail;
        if (avail > 0)
            g_push_tail += add_converted(push_at, 0, (uint64_t)avail, &g_push_frac, frames,
                                         g_push_gain, g_push_gain);
    } else if (g_push_on && g_push) {
        long want = frames;
        while (want > 0) {
            long avail = g_push_head - g_push_tail;
            if (avail <= 0) break;
            long idx = g_push_tail % USND_PUSH_FRAMES;
            long run = USND_PUSH_FRAMES - idx;      // to the ring's end
            if (run > avail) run = avail;
            if (run > want) run = want;
            add_voice(g_push + idx * USND_CHANNELS, run, g_push_gain, g_push_gain);
            g_push_tail += run;
            want -= run;
        }
    }

    // Master gain, then SATURATE. Wrapping a sum that overshoots is the
    // loudest bug an audio path can have -- it turns a slightly-too-loud
    // mix into full-scale noise.
    for (long i = 0; i < frames * USND_CHANNELS; i++) {
        int64_t v = g_acc[i] * g_volume / 100;
        if (v > INT32_MAX) v = INT32_MAX;
        else if (v < INT32_MIN) v = INT32_MIN;
        g_out[i] = (int32_t)v;
    }
}

// Is anything going to produce samples this pass?
static int voices_active(void) {
    if (g_st_open && !g_st_paused && !g_st_done) return 1;
    for (int v = 0; v < USND_VOICES; v++)
        if (g_voices[v].active) return 1;
    // An OPEN pushed source counts even with nothing queued: a
    // synthesiser between notes is still playing, and letting the mixer
    // idle-pin its cursor here would cost a ring of latency the moment
    // the next sample arrived.
    if (g_push_on) return 1;
    return 0;
}

// What counts as drained. One mixing pass' worth: below this the tail
// has played and pinning the cursor throws nothing away.
#define IDLE_PENDING MIX_FRAMES

static void *worker_main(void *arg) {
    (void)arg;
    while (!g_quit) {
        pthread_mutex_lock(&g_lock);
        // THE RATE, every pass: the streaming voice asks for its file's,
        // and nothing playing asks for nothing (0) -- the sink then keeps
        // the card where it is, so a click never switches it.
        uint32_t want = (g_st_open && !g_st_done) ? g_st.fmt.rate : 0;
        uint32_t r = g_sink->rate(want);
        if (r) g_rate = r;
        if (r && g_st_open) usnd_stream_set_rate(&g_st, r);
        if (!r) {
            // UNDECIDED: write nothing, so what is queued plays out at
            // the old rate and a switch happens over a drained ring.
        } else if (!voices_active()) {
            // Let the tail play, THEN pin -- flushing while audio is
            // still queued cuts the end off every sound.
            if (g_sink->pending() <= IDLE_PENDING) g_sink->flush();
        } else {
            // Fill everything the sink will take this pass. The first
            // one primes the whole ring, which is what stops playback
            // starting against an empty buffer.
            for (int guard = 0; guard < 32; guard++) {
                long room = g_sink->space();
                if (room <= 0) break;
                if (room > MIX_FRAMES) room = MIX_FRAMES;
                mix(room);
                long wrote = g_sink->write(g_out, room);
                if (wrote <= 0) break;
                tap_store(g_out, wrote);
            }
        }
        pthread_mutex_unlock(&g_lock);
        sys_sleep_ms(WORKER_MS);
    }
    return 0;
}

// --- lifecycle --------------------------------------------------------

// HOW MANY SUBSYSTEMS HAVE OPENED THE LIBRARY. Doom is the reason:
// its effects and its music initialise and shut down INDEPENDENTLY
// (I_ShutdownSound and I_ShutdownMusic are separate calls in separate
// places), so whichever tore down first would free the mixer, join its
// worker and close the sink while the other was still feeding it. A
// count rather than a flag, so the last one out does the teardown.
static int g_users;

int usnd_init(void) {
    if (g_ready) { g_users++; return 0; }

    // The daemon FIRST, the device as the fallback -- an app that got
    // the daemon and an app that got the device are the same app. The
    // daemon's open() returns -ENODEV when none is running, so a
    // machine without one falls straight through.
    static const struct usnd_sink *const sinks[] = {
        &usnd_sink_daemon, &usnd_sink_device,
    };

    int rc = -ENODEV;
    for (int i = 0; i < (int)(sizeof sinks / sizeof sinks[0]); i++) {
        rc = sinks[i]->open();
        if (rc == 0) { g_sink = sinks[i]; break; }
    }
    if (rc != 0) { usnd_fail(rc == -EBUSY ? "another program is using the sound device"
                                          : "no sound device"); return rc; }

    g_quit = 0;
    if (pthread_create(&g_worker, 0, worker_main, 0) != 0) {
        g_sink->close();
        g_sink = 0;
        usnd_fail("could not start the audio thread");
        return -ENOMEM;
    }
    g_ready = 1;
    g_users = 1;
    return 0;
}

void usnd_shutdown(void) {
    if (!g_ready) return;
    if (--g_users > 0) return;   // somebody else is still playing
    g_quit = 1;
    pthread_join(g_worker, 0);   // joined, not detached: the sink closes after
    pthread_mutex_lock(&g_lock);
    if (g_st_open) { usnd_close(&g_st); g_st_open = 0; }
    memset(g_voices, 0, sizeof g_voices);
    pthread_mutex_unlock(&g_lock);
    usnd_push_close();
    g_sink->close();
    g_sink = 0;
    g_ready = 0;
}

int usnd_ready(void) { return g_ready; }

const char *usnd_sink_name(void) { return g_sink ? g_sink->name : "none"; }

// --- clips ------------------------------------------------------------

int usnd_clip_load(const char *path, struct usnd_clip *c) {
    memset(c, 0, sizeof *c);
    struct usnd_stream s;
    int rc = usnd_open(path, &s);
    if (rc != 0) return rc;
    rc = usnd_clip_drain(&s, c);
    usnd_close(&s);
    return rc;
}

void usnd_clip_free(struct usnd_clip *c) {
    if (!c->pcm) return;
    // Silence anything still playing it FIRST -- the worker reads these
    // pointers, and freeing under it is a use-after-free at whatever
    // volume the mix was at.
    if (g_ready) {
        pthread_mutex_lock(&g_lock);
        for (int v = 0; v < USND_VOICES; v++)
            if (g_voices[v].pcm == c->pcm) g_voices[v].active = 0;
        pthread_mutex_unlock(&g_lock);
    }
    free(c->pcm);
    memset(c, 0, sizeof *c);
}

// A handle is (generation << 8) | (slot + 1), so 0 is never a valid
// one and a wrapped generation cannot collide with a live slot for
// 2^23 reuses. Packed into an int because a handle is a token to hand
// back, not a struct to copy around.
#define VOICE_SLOT(h) (((h) & 0xFF) - 1)
#define VOICE_GEN(h)  ((unsigned)(h) >> 8)
#define VOICE_HANDLE(slot, gen) (int)((((unsigned)(gen) & 0x7FFFFFu) << 8) | ((unsigned)(slot) + 1))

// The live voice a handle names, or NULL. Call with the lock held.
static struct voice *voice_of(usnd_voice_t v) {
    if (v == USND_VOICE_NONE) return 0;
    int slot = VOICE_SLOT(v);
    if (slot < 0 || slot >= USND_VOICES) return 0;
    struct voice *vo = &g_voices[slot];
    if (!vo->active || vo->gen != VOICE_GEN(v)) return 0;
    return vo;
}

static int clamp_gain(int g) { return g < 0 ? 0 : (g > 1024 ? 1024 : g); }

usnd_voice_t usnd_voice_play(const struct usnd_clip *c, int gain_l, int gain_r) {
    if (!g_ready || !c->pcm || !c->frames) return USND_VOICE_NONE;

    pthread_mutex_lock(&g_lock);
    int slot = -1;
    for (int v = 0; v < USND_VOICES; v++)
        if (!g_voices[v].active) { slot = v; break; }
    usnd_voice_t h = USND_VOICE_NONE;
    if (slot >= 0) {
        struct voice *vo = &g_voices[slot];
        vo->pcm = c->pcm;
        vo->frames = c->frames;
        vo->pos = 0;
        vo->frac = 0;
        vo->gain_l = clamp_gain(gain_l);
        vo->gain_r = clamp_gain(gain_r);
        vo->gen = (vo->gen + 1) & 0x7FFFFFu;
        vo->active = 1;
        h = VOICE_HANDLE(slot, vo->gen);
    }
    pthread_mutex_unlock(&g_lock);
    return h;
}

int usnd_voice_set_gain(usnd_voice_t v, int gain_l, int gain_r) {
    if (!g_ready) return 0;
    pthread_mutex_lock(&g_lock);
    struct voice *vo = voice_of(v);
    if (vo) {
        vo->gain_l = clamp_gain(gain_l);
        vo->gain_r = clamp_gain(gain_r);
    }
    pthread_mutex_unlock(&g_lock);
    return vo != 0;
}

void usnd_voice_stop(usnd_voice_t v) {
    if (!g_ready) return;
    pthread_mutex_lock(&g_lock);
    struct voice *vo = voice_of(v);
    if (vo) vo->active = 0;
    pthread_mutex_unlock(&g_lock);
}

int usnd_voice_active(usnd_voice_t v) {
    if (!g_ready) return 0;
    pthread_mutex_lock(&g_lock);
    int live = voice_of(v) != 0;
    pthread_mutex_unlock(&g_lock);
    return live;
}

int usnd_clip_play(const struct usnd_clip *c, int gain) {
    return usnd_voice_play(c, gain, gain) == USND_VOICE_NONE ? -1 : 0;
}

// --- the pushed source ------------------------------------------------

int usnd_push_open(void) {
    if (g_push_on) return -EBUSY;
    int32_t *buf = malloc((size_t)USND_PUSH_FRAMES * USND_CHANNELS * sizeof(int32_t));
    if (!buf) { usnd_fail("out of memory"); return -ENOMEM; }
    pthread_mutex_lock(&g_lock);
    g_push = buf;
    g_push_head = g_push_tail = 0;
    g_push_frac = 0;
    g_push_on = 1;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

void usnd_push_close(void) {
    if (!g_push_on) return;
    pthread_mutex_lock(&g_lock);
    g_push_on = 0;
    int32_t *buf = g_push;
    g_push = 0;
    pthread_mutex_unlock(&g_lock);
    // Freed OUTSIDE the lock, but only after the mixer can no longer
    // see the pointer -- g_push_on and g_push are both cleared under it.
    free(buf);
}

long usnd_push_space(void) {
    if (!g_push_on) return 0;
    pthread_mutex_lock(&g_lock);
    long used = g_push_head - g_push_tail;
    pthread_mutex_unlock(&g_lock);
    long space = USND_PUSH_FRAMES - used;
    return space > 0 ? space : 0;
}

long usnd_push(const int16_t *pcm, long frames) {
    if (!g_push_on || frames <= 0) return 0;
    pthread_mutex_lock(&g_lock);
    long n = 0;
    while (n < frames) {
        long used = g_push_head - g_push_tail;
        long space = USND_PUSH_FRAMES - used;
        if (space <= 0) break;
        long idx = g_push_head % USND_PUSH_FRAMES;
        long run = USND_PUSH_FRAMES - idx;
        if (run > space) run = space;
        if (run > frames - n) run = frames - n;
        const int16_t *src = pcm + n * USND_CHANNELS;
        int32_t *dst = g_push + idx * USND_CHANNELS;
        for (long i = 0; i < run * USND_CHANNELS; i++) dst[i] = (int32_t)src[i] * 65536;
        g_push_head += run;
        n += run;
    }
    pthread_mutex_unlock(&g_lock);
    return n;
}

void usnd_push_set_gain(int gain) { g_push_gain = clamp_gain(gain); }

// --- the streaming voice ----------------------------------------------

int usnd_play(const char *path) {
    if (!g_ready) return -ENODEV;

    struct usnd_stream fresh;
    int rc = usnd_open(path, &fresh);
    if (rc != 0) return rc;

    pthread_mutex_lock(&g_lock);
    if (g_st_open) usnd_close(&g_st);
    g_st = fresh;
    g_st_open = 1;
    g_st_paused = 0;
    g_st_done = 0;
    g_sink->flush();
    pthread_mutex_unlock(&g_lock);
    return 0;
}

void usnd_stop(void) {
    if (!g_ready) return;
    pthread_mutex_lock(&g_lock);
    if (g_st_open) { usnd_close(&g_st); g_st_open = 0; }
    g_st_done = 1;
    g_sink->flush();
    pthread_mutex_unlock(&g_lock);
}

void usnd_set_paused(int paused) {
    pthread_mutex_lock(&g_lock);
    g_st_paused = paused ? 1 : 0;
    // Dropping what is queued is what makes pause SOUND immediate: a
    // third of a second of already-mixed audio would otherwise play on
    // after the click.
    if (paused && g_ready) g_sink->flush();
    pthread_mutex_unlock(&g_lock);
}

int usnd_paused(void) { return g_st_paused; }

int usnd_playing(void) { return g_st_open && !g_st_done; }

uint64_t usnd_position(void) {
    if (!g_st_open) return 0;
    pthread_mutex_lock(&g_lock);
    uint64_t decoded = g_st.out_pos;
    uint64_t queued = g_ready ? (uint64_t)g_sink->pending() : 0;
    uint32_t rate = g_st.out_rate ? g_st.out_rate : USND_RATE;
    pthread_mutex_unlock(&g_lock);
    // What has been HEARD, not what has been decoded -- both at the
    // card's rate, and handed back in USND_RATE frames like every other
    // position here.
    uint64_t heard = decoded > queued ? decoded - queued : 0;
    return heard * USND_RATE / rate;
}

// Blocks until what is queued has been played. usnd_shutdown() stops
// the worker, so a caller that exits without this cuts the tail off
// every file it played.
void usnd_drain(void) {
    if (!g_ready) return;
    for (int guard = 0; guard < 200; guard++) {   // ~2 s, well past one ring
        pthread_mutex_lock(&g_lock);
        long q = g_sink->pending();
        int busy = voices_active();
        pthread_mutex_unlock(&g_lock);
        if (!busy && q <= IDLE_PENDING) return;
        sys_sleep_ms(WORKER_MS);
    }
}

uint64_t usnd_duration(void) {
    return g_st_open ? usnd_stream_frames(&g_st) : 0;
}

int usnd_seek_to(uint64_t device_frame) {
    if (!g_st_open) return -EINVAL;
    pthread_mutex_lock(&g_lock);
    int rc = usnd_seek(&g_st, device_frame);
    if (rc == 0) {
        g_st_done = 0;
        if (g_ready) g_sink->flush();
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

void usnd_set_volume(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    g_volume = pct;
}

int usnd_volume(void) { return g_volume; }

long usnd_peek(int16_t *dst, long frames) {
    if (!g_ready || frames <= 0) return 0;
    if (frames > TAP_FRAMES / 2) frames = TAP_FRAMES / 2;
    pthread_mutex_lock(&g_lock);
    long got = 0;
    if (voices_active() && !g_st_paused) {
        uint64_t pending = (uint64_t)g_sink->pending();
        uint64_t end = g_tap_written > pending ? g_tap_written - pending : 0;
        if (g_tap_written - end > TAP_FRAMES - (uint64_t)frames)
            end = g_tap_written - (TAP_FRAMES - (uint64_t)frames);   // as near as is kept
        if (end >= (uint64_t)frames) {
            for (long i = 0; i < frames; i++) {
                long at = (long)((end - (uint64_t)frames + (uint64_t)i) % TAP_FRAMES) * USND_CHANNELS;
                dst[i * USND_CHANNELS] = g_tap[at];
                dst[i * USND_CHANNELS + 1] = g_tap[at + 1];
            }
            got = frames;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return got;
}
