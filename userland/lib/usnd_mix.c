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
    const int16_t *pcm;     // caller-owned clip samples, device format
    uint64_t frames, pos;
    int gain;
    int active;
};

static const struct usnd_sink *g_sink;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_worker;
static int g_ready, g_quit;
static int g_volume = 100;

static struct voice g_voices[USND_VOICES];

static struct usnd_stream g_st;
static int g_st_open, g_st_paused, g_st_done;

// .bss, not stack: the ring-3 frame budget is 2 KiB and these are 20.
static int32_t g_acc[MIX_FRAMES * USND_CHANNELS];
static int16_t g_scratch[MIX_FRAMES * USND_CHANNELS];
static int16_t g_out[MIX_FRAMES * USND_CHANNELS];

// --- mixing -----------------------------------------------------------

static void add_voice(const int16_t *src, long frames, int gain) {
    long n = frames * USND_CHANNELS;
    for (long i = 0; i < n; i++)
        g_acc[i] += ((int32_t)src[i] * gain) >> 8;
}

// Sums every active voice into `g_out`. Always produces `frames` frames;
// with nothing playing they are zeroes.
static void mix(long frames) {
    memset(g_acc, 0, (size_t)frames * USND_CHANNELS * sizeof g_acc[0]);

    if (g_st_open && !g_st_paused && !g_st_done) {
        long n = usnd_read(&g_st, g_scratch, frames);
        if (n <= 0) g_st_done = 1;
        else add_voice(g_scratch, n, GAIN_UNITY);
    }

    for (int v = 0; v < USND_VOICES; v++) {
        struct voice *vo = &g_voices[v];
        if (!vo->active) continue;
        long n = frames;
        if ((uint64_t)n > vo->frames - vo->pos) n = (long)(vo->frames - vo->pos);
        add_voice(vo->pcm + vo->pos * USND_CHANNELS, n, vo->gain);
        vo->pos += (uint64_t)n;
        if (vo->pos >= vo->frames) vo->active = 0;
    }

    // Master gain, then SATURATE. Wrapping a sum that overshoots is the
    // loudest bug an audio path can have -- it turns a slightly-too-loud
    // mix into full-scale noise.
    for (long i = 0; i < frames * USND_CHANNELS; i++) {
        int32_t v = g_acc[i] * g_volume / 100;
        if (v > 32767) v = 32767;
        else if (v < -32768) v = -32768;
        g_out[i] = (int16_t)v;
    }
}

// Is anything going to produce samples this pass?
static int voices_active(void) {
    if (g_st_open && !g_st_paused && !g_st_done) return 1;
    for (int v = 0; v < USND_VOICES; v++)
        if (g_voices[v].active) return 1;
    return 0;
}

// What counts as drained. One mixing pass' worth: below this the tail
// has played and pinning the cursor throws nothing away.
#define IDLE_PENDING MIX_FRAMES

static void *worker_main(void *arg) {
    (void)arg;
    while (!g_quit) {
        pthread_mutex_lock(&g_lock);
        if (!voices_active()) {
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
                if (g_sink->write(g_out, room) <= 0) break;
            }
        }
        pthread_mutex_unlock(&g_lock);
        sys_sleep_ms(WORKER_MS);
    }
    return 0;
}

// --- lifecycle --------------------------------------------------------

int usnd_init(void) {
    if (g_ready) return 0;

    // One row today. A daemon sink goes FIRST in this list, with the
    // device as the fallback -- an app that got the daemon and an app
    // that got the device are the same app.
    static const struct usnd_sink *const sinks[] = { &usnd_sink_device };

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
    return 0;
}

void usnd_shutdown(void) {
    if (!g_ready) return;
    g_quit = 1;
    pthread_join(g_worker, 0);   // joined, not detached: the sink closes after
    pthread_mutex_lock(&g_lock);
    if (g_st_open) { usnd_close(&g_st); g_st_open = 0; }
    memset(g_voices, 0, sizeof g_voices);
    pthread_mutex_unlock(&g_lock);
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

    uint64_t total = usnd_stream_frames(&s);
    if (!total || total > USND_CLIP_MAX_FRAMES) {
        usnd_close(&s);
        usnd_fail(total ? "too long to load as a clip" : "clip has no samples");
        return total ? -ENOTSUP : -EINVAL;
    }

    int16_t *pcm = malloc((size_t)total * USND_CHANNELS * sizeof(int16_t));
    if (!pcm) { usnd_close(&s); usnd_fail("out of memory"); return -ENOMEM; }

    // The conversion can land a frame either side of the estimate, so
    // the loop reads until the decoder stops rather than trusting the
    // count -- and never past the buffer.
    uint64_t got = 0;
    while (got < total) {
        long n = usnd_read(&s, pcm + got * USND_CHANNELS, (long)(total - got));
        if (n <= 0) break;
        got += (uint64_t)n;
    }
    usnd_close(&s);

    if (!got) { free(pcm); usnd_fail("clip decoded to nothing"); return -EINVAL; }
    c->pcm = pcm;
    c->frames = got;
    return 0;
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

int usnd_clip_play(const struct usnd_clip *c, int gain) {
    if (!g_ready || !c->pcm || !c->frames) return -1;
    if (gain < 0) gain = 0;

    pthread_mutex_lock(&g_lock);
    int slot = -1;
    for (int v = 0; v < USND_VOICES; v++)
        if (!g_voices[v].active) { slot = v; break; }
    if (slot >= 0) {
        g_voices[slot].pcm = c->pcm;
        g_voices[slot].frames = c->frames;
        g_voices[slot].pos = 0;
        g_voices[slot].gain = gain;
        g_voices[slot].active = 1;
    }
    pthread_mutex_unlock(&g_lock);
    return slot >= 0 ? 0 : -1;
}

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
    pthread_mutex_unlock(&g_lock);
    // What has been HEARD, not what has been decoded.
    return decoded > queued ? decoded - queued : 0;
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
