// The MIDI codec: Standard MIDI Files, format 0 and 1, rendered through
// the SoundFont synth in usnd_synth.c.
//
// A MIDI file is a score, not a recording, so "decoding" it is playing
// it -- which is what a codec row can already express: open() reads the
// score, read() renders the next stretch of it, and every app that
// plays a file through usnd plays MIDI without knowing it exists. The
// same place GStreamer's fluiddec and TiMidity-as-a-decoder put it.
//
// **ONE SEQUENCER, THREE USES.** next_event() is the only reader of the
// tracks. open() runs it dry to validate the file and find its length,
// read() runs it for real, and seek() runs it silently to the target.
// Sharing it is what makes the duration the Player shows the length the
// song actually plays for, to the frame.
//
// **A SEEK DROPS NOTES ALREADY SOUNDING at the target**, as FluidSynth's
// does: controllers, programs and the bend are replayed, note-ons are
// not. Starting a note mid-way would need its envelope fast-forwarded
// too, and a piano chord resuming at full attack is worse than silence.
//
// What it refuses: format 2 (independent patterns, which nothing plays
// as one song) and SMPTE-less oddities are -ENOTSUP; a chunk past the
// end of the file or an event cut in half is -EINVAL. A track that
// simply ends without End of Track is accepted -- its chunk length is
// definite, so nothing is guessed.
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lib/usnd.h"
#include "lib/usnd_internal.h"
#include "lib/usnd_sf2.h"
#include "lib/usnd_synth.h"
#include "rt/sys.h"
#include "errno.h"

#define MAX_FILE    (16u << 20)
#define MAX_TRACKS  1024
#define TAIL_SECS   2           // after the last event, for releases to ring out
#define MAX_SECS    (6 * 3600)  // longer than this is a hostile delta, not a song

#define SF2_DIR     "/usr/share/soundfonts"
#define SF2_BUILTIN "toy-gm.sf2"

struct mtrack {
    uint32_t start, end, pos;
    uint64_t tick;              // absolute tick of the event at `pos`
    uint8_t running;
    int done;
};

enum { EV_CHAN, EV_RESET };

struct mev {
    int kind;
    uint8_t status, d1, d2;
    uint64_t frame;
};

struct mid {
    uint8_t *buf;
    uint32_t len;
    struct mtrack *tr;
    int ntr;
    int format;

    double sec_per_tick;        // at the current tempo
    uint32_t division;          // ticks per quarter; 0 for SMPTE time
    uint64_t base_tick;
    double base_frame;          // the frame at base_tick

    uint64_t end_frame;         // the latest End of Track seen so far

    int have_ev;
    struct mev ev;
    uint64_t pos, total;

    struct sf2_bank *bank;
    struct usynth *syn;
};

static const char *g_sf_override;

void usnd_mid_set_soundfont(const char *path) { g_sf_override = path; }

// --- reading tracks ---------------------------------------------------

static int vlq(const uint8_t *b, uint32_t *pos, uint32_t end, uint32_t *out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        if (*pos >= end) return 0;
        uint8_t c = b[(*pos)++];
        v = v << 7 | (c & 0x7F);
        if (!(c & 0x80)) { *out = v; return 1; }
    }
    return 0;                   // a fifth continuation byte: not a VLQ
}

// Read the delta in front of the next event. A track that has run out of
// bytes has ended, whether or not it said so.
static int track_delta(struct mtrack *t, const uint8_t *b) {
    if (t->pos >= t->end) { t->done = 1; return 1; }
    uint32_t d;
    if (!vlq(b, &t->pos, t->end, &d)) return 0;
    t->tick += d;
    return 1;
}

static void set_tempo(struct mid *m, uint64_t tick, uint32_t us_per_quarter) {
    m->base_frame += (double)(tick - m->base_tick) * m->sec_per_tick * USND_RATE;
    m->base_tick = tick;
    if (m->division) m->sec_per_tick = us_per_quarter / 1e6 / m->division;
}

static uint64_t frame_of(const struct mid *m, uint64_t tick) {
    return (uint64_t)(m->base_frame + (double)(tick - m->base_tick) * m->sec_per_tick * USND_RATE + 0.5);
}

static int is_reset(const uint8_t *p, uint32_t n) {
    // GM1/GM2 System On, GS Reset, XG System On -- any device id.
    if (n >= 4 && p[0] == 0x7E && p[2] == 0x09 && (p[3] == 0x01 || p[3] == 0x03)) return 1;
    if (n >= 7 && p[0] == 0x41 && p[2] == 0x42 && p[3] == 0x12 &&
        p[4] == 0x40 && p[5] == 0x00 && p[6] == 0x7F) return 1;
    if (n >= 6 && p[0] == 0x43 && (p[1] & 0xF0) == 0x10 && p[2] == 0x4C &&
        p[3] == 0 && p[4] == 0 && p[5] == 0x7E) return 1;
    return 0;
}

// The next event anything below the sequencer acts on, in time order
// across every track. 1 = got one, 0 = the song is over, -1 = corrupt.
static int next_event(struct mid *m, struct mev *ev) {
    for (;;) {
        struct mtrack *t = 0;
        for (int i = 0; i < m->ntr; i++)
            if (!m->tr[i].done && (!t || m->tr[i].tick < t->tick)) t = &m->tr[i];
        if (!t) return 0;

        const uint8_t *b = m->buf;
        uint64_t tick = t->tick;
        if (t->pos >= t->end) { t->done = 1; continue; }
        uint8_t st = b[t->pos];
        if (st & 0x80) t->pos++;
        else if (t->running) st = t->running;
        else return -1;         // a data byte with no status to run on

        if (st < 0xF0) {
            t->running = st;
            int need = (st & 0xE0) == 0xC0 ? 1 : 2;
            if (t->end - t->pos < (uint32_t)need) return -1;
            uint8_t d1 = b[t->pos], d2 = need == 2 ? b[t->pos + 1] : 0;
            if ((d1 | d2) & 0x80) return -1;
            t->pos += (uint32_t)need;
            if (!track_delta(t, b)) return -1;
            ev->kind = EV_CHAN;
            ev->status = st; ev->d1 = d1; ev->d2 = d2;
            ev->frame = frame_of(m, tick);
            return 1;
        }

        uint32_t len;
        if (st == 0xFF) {
            if (t->pos >= t->end) return -1;
            uint8_t type = b[t->pos++];
            if (!vlq(b, &t->pos, t->end, &len) || len > t->end - t->pos) return -1;
            const uint8_t *p = b + t->pos;
            t->pos += len;
            if (type == 0x2F) {
                // End of Track can come well after the last note (a held
                // bar of rest), and it is part of the song.
                uint64_t f = frame_of(m, tick);
                if (f > m->end_frame) m->end_frame = f;
                t->done = 1;
                continue;
            }
            if (type == 0x51 && len == 3)
                set_tempo(m, tick, (uint32_t)p[0] << 16 | p[1] << 8 | p[2]);
            if (!track_delta(t, b)) return -1;
            continue;
        }
        if (st == 0xF0 || st == 0xF7) {
            if (!vlq(b, &t->pos, t->end, &len) || len > t->end - t->pos) return -1;
            const uint8_t *p = b + t->pos;
            t->pos += len;
            if (!track_delta(t, b)) return -1;
            if (st == 0xF0 && is_reset(p, len)) {
                ev->kind = EV_RESET;
                ev->frame = frame_of(m, tick);
                return 1;
            }
            continue;
        }
        return -1;              // a system real-time or common byte: not in a file
    }
}

static int rewind_song(struct mid *m) {
    m->base_tick = 0;
    m->base_frame = 0;
    set_tempo(m, 0, 500000);    // 120 bpm until the file says otherwise
    for (int i = 0; i < m->ntr; i++) {
        struct mtrack *t = &m->tr[i];
        t->pos = t->start;
        t->tick = 0;
        t->running = 0;
        t->done = 0;
        if (!track_delta(t, m->buf)) return -1;
    }
    m->have_ev = 0;
    m->pos = 0;
    m->end_frame = 0;
    return 0;
}

static void fetch(struct mid *m) {
    m->have_ev = next_event(m, &m->ev) == 1;
}

// --- the file ---------------------------------------------------------

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static int parse_file(struct mid *m) {
    const uint8_t *b = m->buf;
    uint32_t hlen = be32(b + 4);
    if (hlen < 6 || hlen > m->len - 8) { usnd_fail("MIDI header is truncated"); return -EINVAL; }
    m->format = be16(b + 8);
    uint16_t div = be16(b + 12);
    if (m->format == 2) { usnd_fail("format 2 MIDI (independent patterns) is not supported"); return -ENOTSUP; }
    if (m->format > 2) { usnd_fail("unknown MIDI file format"); return -EINVAL; }
    if (div & 0x8000) {
        // SMPTE time: -frames per second in the high byte, ticks per
        // frame in the low. 29 means 29.97 drop-frame.
        int fps = -(int8_t)(div >> 8), tpf = div & 0xFF;
        if (fps <= 0 || tpf == 0) { usnd_fail("MIDI time division is invalid"); return -EINVAL; }
        m->division = 0;
        m->sec_per_tick = 1.0 / ((fps == 29 ? 29.97 : fps) * tpf);
    } else {
        if (div == 0) { usnd_fail("MIDI time division is zero"); return -EINVAL; }
        m->division = div;
    }

    m->tr = calloc(MAX_TRACKS, sizeof *m->tr);
    if (!m->tr) { usnd_fail("out of memory"); return -ENOMEM; }
    uint32_t off = 8 + hlen;
    while (m->len - off >= 8 && m->ntr < MAX_TRACKS) {
        uint32_t sz = be32(b + off + 4);
        if (sz > m->len - off - 8) { usnd_fail("MIDI file is truncated"); return -EINVAL; }
        // Unknown chunk types are skipped, as the spec asks.
        if (memcmp(b + off, "MTrk", 4) == 0) {
            m->tr[m->ntr].start = off + 8;
            m->tr[m->ntr].end = off + 8 + sz;
            m->ntr++;
        }
        off += 8 + sz;
    }
    if (m->ntr == 0) { usnd_fail("MIDI file has no tracks"); return -EINVAL; }

    // The dry run: every event parsed once, and the song's length.
    if (rewind_song(m) != 0) { usnd_fail("MIDI track is corrupt"); return -EINVAL; }
    struct mev ev;
    uint64_t last = 0;
    int rc;
    while ((rc = next_event(m, &ev)) == 1) last = ev.frame;
    if (rc < 0) { usnd_fail("MIDI track is corrupt"); return -EINVAL; }
    if (m->end_frame > last) last = m->end_frame;
    if (last > (uint64_t)MAX_SECS * USND_RATE) { usnd_fail("MIDI file is implausibly long"); return -EINVAL; }
    m->total = last + (uint64_t)TAIL_SECS * USND_RATE;
    return 0;
}

// The bank to play with: any SoundFont installed beside the built-in one
// wins (the first by name), because nobody installs a 30 MB bank to keep
// hearing the small one. The built-in bank is the fallback, including
// for a preferred bank that fails to load.
static int pick_bank(char *out, size_t cap) {
    if (g_sf_override) { strlcpy(out, g_sf_override, cap); return 0; }
    char best[64] = "";
    DIR *d = opendir(SF2_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != 0) {
            size_t n = strlen(e->d_name);
            if (n < 5 || n >= sizeof best || strcasecmp(e->d_name + n - 4, ".sf2") != 0) continue;
            if (strcmp(e->d_name, SF2_BUILTIN) == 0) continue;
            if (!best[0] || strcmp(e->d_name, best) < 0) strlcpy(best, e->d_name, sizeof best);
        }
        closedir(d);
    }
    snprintf(out, cap, "%s/%s", SF2_DIR, best[0] ? best : SF2_BUILTIN);
    return best[0] != 0;
}

static int load_bank(struct mid *m) {
    char path[128];
    int preferred = pick_bank(path, sizeof path);
    int rc = sf2_get(path, &m->bank);
    if (rc != 0 && preferred && !g_sf_override) {
        snprintf(path, sizeof path, "%s/%s", SF2_DIR, SF2_BUILTIN);
        rc = sf2_get(path, &m->bank);
    }
    if (rc == -ENOENT) {
        usnd_fail("no SoundFont installed in " SF2_DIR);
        return -ENOTSUP;
    }
    return rc;
}

static void mid_free(struct mid *m) {
    if (!m) return;
    usynth_free(m->syn);
    sf2_put(m->bank);
    free(m->tr);
    free(m->buf);
    free(m);
}

static int mid_probe(const uint8_t *d, size_t n) {
    return n >= 4 && memcmp(d, "MThd", 4) == 0;
}

static int mid_open(struct usnd_stream *s) {
    long long size = sys_lseek(s->fd, 0, SYS_SEEK_END);
    if (size < 14) { usnd_fail("MIDI file is truncated"); return -EINVAL; }
    if (size > MAX_FILE) { usnd_fail("MIDI file is too large"); return -ENOTSUP; }

    struct mid *m = calloc(1, sizeof *m);
    if (!m || !(m->buf = malloc((size_t)size))) {
        free(m);
        usnd_fail("out of memory for the MIDI file");
        return -ENOMEM;
    }
    m->len = (uint32_t)size;
    sys_lseek(s->fd, 0, SYS_SEEK_SET);
    for (uint32_t got = 0; got < m->len;) {
        long n = (long)sys_read(s->fd, m->buf + got, m->len - got);
        if (n <= 0) { mid_free(m); usnd_fail("cannot read the MIDI file"); return -EIO; }
        got += (uint32_t)n;
    }

    // A stream opened only to be DESCRIBED needs the score, not the
    // bank: loading a 30 MB SoundFont to show a file's length froze the
    // Player for seconds on every selection. It still refuses as playing
    // would if there is no bank at all.
    char path[128];
    int rc = parse_file(m);
    if (rc == 0 && s->info_only) {
        pick_bank(path, sizeof path);
        int fd = sys_open(path, 0);
        if (fd < 0) { usnd_fail("no SoundFont installed in " SF2_DIR); rc = -ENOTSUP; }
        else sys_close(fd);
    } else if (rc == 0) {
        rc = load_bank(m);
        if (rc == 0 && !(m->syn = usynth_new(m->bank, USND_RATE))) {
            usnd_fail("out of memory for the synthesiser");
            rc = -ENOMEM;
        }
        if (rc == 0) strlcpy(path, m->bank->path, sizeof path);
    }
    if (rc != 0) { mid_free(m); return rc; }

    if (m->syn) {
        rewind_song(m);
        fetch(m);
    }
    s->priv = m;
    s->fmt.rate = USND_RATE;
    s->fmt.channels = 2;
    s->fmt.bits = 16;
    s->frames = m->total;

    const char *bank = strrchr(path, '/');
    bank = bank ? bank + 1 : path;
    snprintf(s->detail, sizeof s->detail, "MIDI type %d, %d track%s, %s",
             m->format, m->ntr, m->ntr == 1 ? "" : "s", bank);
    return 0;
}

static void apply(struct mid *m, const struct mev *ev) {
    if (ev->kind == EV_RESET) usynth_reset(m->syn);
    else usynth_message(m->syn, ev->status, ev->d1, ev->d2);
}

static long mid_read(struct usnd_stream *s, int16_t *dst, long frames) {
    struct mid *m = s->priv;
    long done = 0;
    while (done < frames && m->pos < m->total) {
        while (m->have_ev && m->ev.frame <= m->pos) {
            apply(m, &m->ev);
            fetch(m);
        }
        uint64_t until = m->total;
        if (m->have_ev && m->ev.frame < until) until = m->ev.frame;
        uint64_t run = until - m->pos;
        if (run > (uint64_t)(frames - done)) run = (uint64_t)(frames - done);
        usynth_render(m->syn, dst + done * 2, (long)run);
        done += (long)run;
        m->pos += run;
    }
    return done;
}

static int mid_seek(struct usnd_stream *s, uint64_t frame) {
    struct mid *m = s->priv;
    if (frame > m->total) frame = m->total;
    usynth_reset(m->syn);
    rewind_song(m);
    fetch(m);
    while (m->have_ev && m->ev.frame < frame) {
        const struct mev *ev = &m->ev;
        if (!(ev->kind == EV_CHAN && (ev->status & 0xF0) == 0x90 && ev->d2))
            apply(m, ev);
        fetch(m);
    }
    m->pos = frame;
    usynth_align(m->syn, frame);
    return 0;
}

static void mid_close(struct usnd_stream *s) {
    mid_free(s->priv);
    s->priv = 0;
}

const struct usnd_codec usnd_codec_mid = {
    .name  = "midi",
    .probe = mid_probe,
    .open  = mid_open,
    .read  = mid_read,
    .seek  = mid_seek,
    .close = mid_close,
};
