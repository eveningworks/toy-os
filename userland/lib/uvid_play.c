// Playing a video (lib/uvid_play.h): the decoding thread, the frame
// slots it fills, and the clock the main thread picks frames by.
//
// THE SLOTS: each is FREE, FILLING (the decoder is copying into it,
// outside the lock), READY (a finished frame with its time) or SHOWN
// (the one being drawn -- never written). A seek bumps the GENERATION;
// a frame of an older one is thrown away wherever it is found, so a
// decode that was in flight when the seek came cannot surface after it.
#include "lib/uvid_play.h"
#include "lib/usnd.h"
#include "rt/sys.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define SLOTS 4
#define EARLY_MS 5              // a frame this close to due counts as due
#define SMOOTH_MAX_MS 120       // how far the sound clock is extrapolated between its steps

enum { FREE, FILLING, READY, SHOWN };

struct slot {
    struct uvid_frame f;        // its planes point into `buf`
    uint8_t *buf;
    size_t cap;
    int state;
    uint32_t gen;
};

struct uvid_play {
    struct uvid *v;             // the decoder thread's alone after open
    struct uvid_info info;
    char path[256];
    unsigned flags;
    pthread_t thread;
    int thread_ok;
    pthread_mutex_t lock;

    // Under the lock.
    struct slot s[SLOTS];
    uint32_t gen;
    int seek_req;
    uint32_t seek_ms;
    int eof;                    // the decoder has nothing more, this generation
    volatile int quit;

    // The main thread's.
    int shown;                  // slot index, or -1
    int awaiting;               // show the next frame whatever the clock says
    int paused, speed, sound;
    uint64_t base_ns;
    uint32_t base_ms, last_ms;
    int64_t shown_pts;
    uint32_t n_shown, n_dropped;
    uint32_t serial;            // the shown frame's, from g_serial
    uint32_t a_last;            // the sound clock's last reading, ms
    uint64_t a_ns;              // ...and when it changed
};

// ONE COUNTER FOR EVERY PLAYER in the process, so a serial is never
// repeated by the next file's player (which may even reuse the address).
static uint32_t g_serial;

static uint32_t frame_ms(const struct uvid_play *p) {
    return p->info.fps_num ? (uint32_t)(1000ull * p->info.fps_den / p->info.fps_num) : 40;
}

// --- the decoder thread ----------------------------------------------------

static int copy_frame(struct slot *s, const struct uvid_frame *f) {
    size_t need;
    int cw = (f->w + 1) / 2, ch = (f->h + 1) / 2;
    if (f->fmt == UVID_ARGB) need = (size_t)f->w * (size_t)f->h * 4;
    else need = (size_t)f->w * (size_t)f->h + 2 * (size_t)cw * (size_t)ch;
    if (need > s->cap) {
        uint8_t *b = realloc(s->buf, need);
        if (!b) return -ENOMEM;
        s->buf = b;
        s->cap = need;
    }
    s->f = *f;
    if (f->fmt == UVID_ARGB) {
        uint32_t *d = (uint32_t *)(void *)s->buf;
        for (int y = 0; y < f->h; y++)
            memcpy(d + (size_t)y * (size_t)f->w, f->argb + (size_t)y * (size_t)f->argb_stride, (size_t)f->w * 4);
        s->f.argb = d;
        s->f.argb_stride = f->w;
        return 0;
    }
    uint8_t *y = s->buf, *u = y + (size_t)f->w * (size_t)f->h, *v = u + (size_t)cw * (size_t)ch;
    for (int r = 0; r < f->h; r++) memcpy(y + (size_t)r * f->w, f->y + (size_t)r * f->y_stride, (size_t)f->w);
    for (int r = 0; r < ch; r++) {
        memcpy(u + (size_t)r * cw, f->cb + (size_t)r * f->c_stride, (size_t)cw);
        memcpy(v + (size_t)r * cw, f->cr + (size_t)r * f->c_stride, (size_t)cw);
    }
    s->f.y = y; s->f.cb = u; s->f.cr = v;
    s->f.y_stride = f->w;
    s->f.c_stride = cw;
    return 0;
}

static void *decoder(void *arg) {
    struct uvid_play *p = arg;
    while (!p->quit) {
        pthread_mutex_lock(&p->lock);
        int seek = p->seek_req, eof = p->eof, slot = -1;
        uint32_t ms = p->seek_ms, gen = p->gen;
        p->seek_req = 0;
        if (!seek && !eof)
            for (int i = 0; i < SLOTS; i++)
                if (p->s[i].state == FREE) { slot = i; p->s[i].state = FILLING; break; }
        pthread_mutex_unlock(&p->lock);

        if (seek) {
            if (uvid_seek(p->v, ms, UVID_SEEK_EXACT) < 0) {
                pthread_mutex_lock(&p->lock);
                if (p->gen == gen) p->eof = 1;
                pthread_mutex_unlock(&p->lock);
            }
            continue;
        }
        if (slot < 0) {                 // full, or at the end: wait by sleeping
            sys_sleep_ms(eof ? 20 : 3);
            continue;
        }
        const struct uvid_frame *f;
        int rc = uvid_next(p->v, &f);
        int ok = rc == 1 && copy_frame(&p->s[slot], f) == 0;
        pthread_mutex_lock(&p->lock);
        if (p->gen != gen) {
            p->s[slot].state = FREE;    // a seek came meanwhile: stale
        } else if (!ok) {
            p->s[slot].state = FREE;
            if (rc != 1) p->eof = 1;
        } else {
            p->s[slot].state = READY;
            p->s[slot].gen = gen;
        }
        pthread_mutex_unlock(&p->lock);
    }
    return 0;
}

// --- the sound, and the clock ------------------------------------------------

static void sound_start(struct uvid_play *p, uint32_t ms) {
    p->sound = 0;
    if ((p->flags & UVID_PLAY_MUTE) || !p->info.has_audio || !usnd_ready() || p->speed != 100) return;
    if (usnd_play(p->path) != 0) return;
    if (ms) usnd_seek_to((uint64_t)ms * USND_RATE / 1000);
    usnd_set_paused(p->paused);
    p->sound = 1;
}

static void sound_stop(struct uvid_play *p) {
    if (p->sound) usnd_stop();
    p->sound = 0;
}

// Re-anchors the monotonic clock at `ms`, now.
static void anchor(struct uvid_play *p, uint32_t ms) {
    p->base_ms = ms;
    p->base_ns = sys_monotonic_ns();
}

uint32_t uvid_play_position(struct uvid_play *p) {
    if (p->sound) {
        if (usnd_playing()) {
            // THE SOUND CLOCK MOVES IN STEPS -- a ring period at a time,
            // tens of ms -- and a step longer than a frame skips one. So
            // between steps it is extrapolated by the monotonic clock
            // (bounded, in case the sound stalls), and a reading a little
            // behind what was already returned does not step back: the
            // smoothing every player puts on an audio clock.
            uint32_t a = (uint32_t)(usnd_position() * 1000 / USND_RATE);
            uint64_t now = sys_monotonic_ns();
            if (a != p->a_last || !p->a_ns) { p->a_last = a; p->a_ns = now; }
            uint32_t est = a + (uint32_t)((now - p->a_ns) / 1000000);
            if (p->paused) est = a;
            if (est > a + SMOOTH_MAX_MS) est = a + SMOOTH_MAX_MS;
            if (est < p->last_ms && p->last_ms - est < SMOOTH_MAX_MS) est = p->last_ms;
            p->last_ms = est;
            return est;
        }
        // The sound ended before the pictures: carry on by the clock
        // from where it got to.
        p->sound = 0;
        anchor(p, p->last_ms);
    }
    if (p->paused) return p->base_ms;
    uint64_t el = (sys_monotonic_ns() - p->base_ns) / 1000000;
    p->last_ms = p->base_ms + (uint32_t)(el * (uint64_t)p->speed / 100);
    return p->last_ms;
}

// --- the API ------------------------------------------------------------------

int uvid_play_open(struct uvid_play **out, const char *path, unsigned flags) {
    *out = 0;
    struct uvid_play *p = calloc(1, sizeof *p);
    if (!p) return -ENOMEM;
    int rc = uvid_open(path, &p->v);
    if (rc) { free(p); return rc; }
    p->info = *uvid_info(p->v);
    strlcpy(p->path, path, sizeof p->path);
    p->flags = flags;
    p->shown = -1;
    p->awaiting = 1;
    p->speed = 100;
    p->shown_pts = -1;
    pthread_mutex_init(&p->lock, 0);
    if (pthread_create(&p->thread, 0, decoder, p) != 0) {
        uvid_close(p->v);
        free(p);
        return -ENOMEM;
    }
    p->thread_ok = 1;
    anchor(p, 0);
    sound_start(p, 0);
    *out = p;
    return 0;
}

void uvid_play_close(struct uvid_play *p) {
    if (!p) return;
    p->quit = 1;
    if (p->thread_ok) pthread_join(p->thread, 0);
    sound_stop(p);
    for (int i = 0; i < SLOTS; i++) free(p->s[i].buf);
    uvid_close(p->v);
    free(p);
}

const struct uvid_info *uvid_play_info(const struct uvid_play *p) { return &p->info; }

void uvid_play_set_paused(struct uvid_play *p, int paused) {
    paused = !!paused;
    if (paused == p->paused) return;
    uint32_t now = uvid_play_position(p);
    // Without sound, pausing stops the clock at the frame showing, so
    // play resumes from what was seen rather than from where a lagging
    // decoder should have been.
    if (paused && !p->sound && p->shown_pts >= 0 && (int64_t)now > p->shown_pts) now = (uint32_t)p->shown_pts;
    p->paused = paused;
    anchor(p, now);
    if (p->sound) usnd_set_paused(paused);
}

int uvid_play_paused(const struct uvid_play *p) { return p->paused; }

void uvid_play_seek(struct uvid_play *p, uint32_t ms) {
    if (p->info.ms && ms >= p->info.ms) ms = p->info.ms > frame_ms(p) ? p->info.ms - frame_ms(p) : 0;
    pthread_mutex_lock(&p->lock);
    p->gen++;
    p->seek_req = 1;
    p->seek_ms = ms;
    p->eof = 0;
    for (int i = 0; i < SLOTS; i++)
        if (p->s[i].state == READY) p->s[i].state = FREE;
    pthread_mutex_unlock(&p->lock);
    p->awaiting = 1;
    anchor(p, ms);
    p->last_ms = ms;
    p->a_ns = 0;
    if (p->sound) usnd_seek_to((uint64_t)ms * USND_RATE / 1000);
    else if (!(p->flags & UVID_PLAY_MUTE) && p->speed == 100 && p->info.has_audio && usnd_ready())
        sound_start(p, ms);     // it had ended: a seek back brings it back
}

void uvid_play_set_speed(struct uvid_play *p, int percent) {
    if (percent < 25) percent = 25;
    if (percent > 400) percent = 400;
    if (percent == p->speed) return;
    uint32_t now = uvid_play_position(p);
    sound_stop(p);
    p->speed = percent;
    anchor(p, now);
    if (percent == 100) sound_start(p, now);
}

int uvid_play_speed(const struct uvid_play *p) { return p->speed; }

int uvid_play_ended(struct uvid_play *p) {
    pthread_mutex_lock(&p->lock);
    int left = 0;
    for (int i = 0; i < SLOTS; i++)
        if ((p->s[i].state == READY || p->s[i].state == FILLING) && p->s[i].gen == p->gen) left = 1;
    int done = p->eof && !left && !p->seek_req;
    pthread_mutex_unlock(&p->lock);
    if (!done) return 0;
    if (p->sound && usnd_playing()) return 0;       // the sound runs on past the pictures
    return uvid_play_position(p) >= (uint32_t)(p->shown_pts >= 0 ? p->shown_pts : 0) + frame_ms(p);
}

int uvid_play_tick(struct uvid_play *p) {
    if ((p->flags & UVID_PLAY_LOOP) && uvid_play_ended(p)) uvid_play_seek(p, 0);   // brings the sound back too
    // PAUSED IS WHAT IS ON SCREEN: a decoder running behind the clock
    // (a slow machine) must not catch up behind a pause. Only a seek
    // changes the picture while paused.
    if (p->paused && !p->awaiting) return 0;
    uint32_t now = uvid_play_position(p);
    int changed = 0;
    pthread_mutex_lock(&p->lock);
    int best = -1;
    for (int i = 0; i < SLOTS; i++) {
        struct slot *s = &p->s[i];
        if (s->state != READY) continue;
        if (s->gen != p->gen) { s->state = FREE; continue; }
        int due = s->f.pts_ms <= (int64_t)now + EARLY_MS;
        if (p->awaiting) {
            // After a seek or at the start: the earliest frame, now.
            if (best < 0 || s->f.pts_ms < p->s[best].f.pts_ms) best = i;
        } else if (due && (best < 0 || s->f.pts_ms > p->s[best].f.pts_ms)) {
            best = i;
        }
    }
    if (best >= 0) {
        // Every older frame that never got its turn is dropped.
        for (int i = 0; i < SLOTS; i++)
            if (i != best && p->s[i].state == READY && p->s[i].f.pts_ms < p->s[best].f.pts_ms) {
                p->s[i].state = FREE;
                p->n_dropped++;
            }
        if (p->shown >= 0) p->s[p->shown].state = FREE;
        p->s[best].state = SHOWN;
        p->shown = best;
        p->shown_pts = p->s[best].f.pts_ms;
        p->serial = __atomic_add_fetch(&g_serial, 1, __ATOMIC_RELAXED);
        p->awaiting = 0;
        p->n_shown++;
        changed = 1;
    }
    pthread_mutex_unlock(&p->lock);
    return changed;
}

const struct uvid_frame *uvid_play_frame(const struct uvid_play *p) {
    return p->shown >= 0 ? &p->s[p->shown].f : 0;
}

uint32_t uvid_play_serial(const struct uvid_play *p) { return p ? p->serial : 0; }

void uvid_play_stats(const struct uvid_play *p, struct uvid_play_stats *out) {
    out->shown = p->n_shown;
    out->dropped = p->n_dropped;
    out->sound = p->sound;
}
