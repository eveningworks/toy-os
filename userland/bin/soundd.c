// soundd -- the system sound daemon: one process owns the card, and
// every program that wants sound is one of its clients.
//
// WHY IT EXISTS. The kernel hands out ONE exclusive PCM stream and will
// never mix (docs/decisions/drivers.md), so before this the second
// program to ask for sound got -EBUSY and went permanently quiet -- a
// game started while the Audio Player was open simply had no audio for
// the rest of its life, with the only notice in a log. That is the
// problem ALSA's dmix, PulseAudio, PipeWire and Windows' audio engine
// all exist to solve, and all of them solve it the same way: a
// userspace server owns the device and the applications are its
// clients. This is that server, in about two hundred lines.
//
// HOW A CLIENT IS FOUND. There is no connect-by-name here and no fd
// passing, so the rendezvous is a NAME: a client creates a shared-
// memory object called "snd.<pid>" (abi/sound_abi.h) and this walks
// QUERY_SHM looking for the prefix. The beacon "snd.server" exists
// exactly while this process does, which is how a client knows to use
// the daemon sink at all rather than the kernel's stream.
//
// WHAT IT DOES NOT DO: no resampling (every client speaks the one ABI
// format), no per-client volume yet, and no priority between clients --
// a mix is a plain saturating sum. `system.volume` is still the card's
// master and is applied by the driver, below this.
//
// IT BLOCKS ON TIME, NEVER ON ITS CLIENTS, which is what lets a
// single-threaded loop serve N of them on a system with no poll():
// the hardware ring's position is the clock, and a client that stops
// writing simply contributes silence.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>   // atoi -- the pid out of the ring's name
#include <sys/stat.h>
#include "lib/uconf.h"   // /etc in ring 3, over the shared parser
#include "rt/sys.h"
#include "sound_abi.h"
#include "syscall_abi.h"
#include "query_abi.h"

// Eight is the shm table's own ceiling for audio and more voices than
// this machine has reason to play at once.
#define MAX_CLIENTS 8

struct client {
    char     name[SHM_NAME_MAX];
    char     app[SND_APP_MAX];  // what it is mixed UNDER, "" if it never said
    int      pid;               // parsed out of `name`, for the roster
    int      fd;
    int      gain;              // 0..100 from /etc/sound.conf; 100 by default
    volatile struct snd_ctl_page *ctl;
    volatile int32_t *ring;   // s32, abi/sound_abi.h
};

static struct client g_cl[MAX_CLIENTS];
static int g_beacon = -1;

static volatile struct snd_ctl_page *g_hw;
static volatile int32_t *g_hwring;
static uint32_t g_wr;      // our write cursor into the hardware ring
static int g_running;
static uint32_t g_prev_lead; // the unstarted ring's fill, one pass ago

// --- suspend on idle ---------------------------------------------------
//
// THE DAEMON USED TO HOLD THE CARD FOREVER. The PCM stream is
// EXCLUSIVE, so while soundd was serving nothing else could open it --
// `/tests/tone` and the `sound` KTESTs both wanted `service stop
// soundd` first, and `tools/audio_test.py` does exactly that in its own
// setup. It also kept a codec powered on a laptop with nothing playing.
//
// So the stream is released when no client has been running for a
// while, and reopened when one turns up. PipeWire's
// module-suspend-on-idle, and the same reason: a sound card is a
// shared resource, and a mixer with nothing to mix should not be
// holding it.
//
// TWO SECONDS. Long enough that the gap between two tracks does not
// close and reopen the device -- which would be audible as a click on
// hardware that powers its amplifier down -- and short enough that a
// test can watch it happen. PipeWire's default is five; the difference
// is that nothing here has a session manager to keep a stream alive
// across a gap.
#define SUSPEND_IDLE_MS 2000

// How long `hw_pos` may sit still, while clients are playing, before
// the engine is called stalled rather than the buffer called full. Long
// enough that a slow start or a scheduling hiccup is not a stall; short
// enough to be well inside the ring's own 341 ms.
#define STALL_MS 500

// THE MIXER NEVER GETS AHEAD OF ITS INPUTS, except by this much. A
// chunk is mixed when every playing client has a whole one ready, or
// when the engine is within MIN_LEAD of what has been mixed -- only
// then is silence committed, because only then is it a real underrun.
// Filling the whole ring regardless wrote the gap between a client's
// first write and its second into the middle of the sound (a 21-128 ms
// hole in 7 playbacks of 8, always a whole number of chunks).
//
// EIGHT CHUNKS, 85 ms: one 20 ms pass of ours, plus a USB driver that
// copies ~3 chunks ahead of the position it reports, plus slack. It is
// also the buffer everyone plays on while ANY open client is idle --
// a paused player is "not ready" for ever -- which is still four of
// our passes.
#define MIN_LEAD_BYTES (8u * SND_CHUNK_BYTES)

static int g_open;                       // is the stream ours right now?

// PER STREAM, said once at release: the longest wait between two of our
// passes, and how many chunks a playing client had left empty. A long
// gap is THIS daemon starved (the driver plays the zeroed ring: a clean
// dropout); empty chunks are a CLIENT starved. The driver's own dry
// count sees neither.
static unsigned g_gap_max_ms, g_starved_chunks;
static unsigned long long g_last_pass_ms;
static unsigned long long g_idle_since;  // when the last client stopped

static unsigned long long now_ms(void) { return sys_monotonic_ns() / 1000000ull; }

// Take the stream and map it. The addresses are fixed, so reopening
// lands where the pointers already are.
static int stream_open(void) {
    if (g_open) return 1;
    if (sys_snd_open() != 0) return 0;
    g_hw = (volatile struct snd_ctl_page *)(uintptr_t)SND_MAP_VADDR;
    g_hwring = (volatile int32_t *)(uintptr_t)(SND_MAP_VADDR + 4096);
    if (g_hw->magic != SND_CTL_MAGIC) { sys_snd_ctl(SND_CTL_CLOSE); return 0; }
    g_wr = 0;
    g_running = 0;
    g_prev_lead = 0;
    g_gap_max_ms = g_starved_chunks = 0;
    g_last_pass_ms = 0;
    g_open = 1;
    // SAID ON EVERY ACQUISITION, not just the first: "released the
    // card" with nothing to answer it reads like the daemon gave up.
    // The pair is also what a test watches to know the cycle happened.
    fprintf(stderr, "soundd: took the card\n");
    return 1;
}

// CLOSE, not STOP: stopping halts the engine and keeps the stream, and
// keeping it is the whole problem. After this the control page is
// UNMAPPED, so nothing may touch g_hw until stream_open() says so.
static void stream_release(void) {
    if (!g_open) return;
    sys_snd_ctl(SND_CTL_CLOSE);
    g_open = 0;
    g_running = 0;
    g_hw = 0;
    g_hwring = 0;
    fprintf(stderr, "soundd: idle -- released the card (longest gap between "
                    "passes %u ms, %u chunk(s) a client left empty)\n",
            g_gap_max_ms, g_starved_chunks);
}

// Any client asking to be mixed? `running` is the client's own say-so,
// which is what it clears when it stops.
static int anyone_playing(void) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (g_cl[i].ctl && g_cl[i].ctl->running) return 1;
    return 0;
}

// A PERCENTAGE ONTO A MULTIPLIER, 40 dB of range, linear in dB -- the
// same taper the cards use (`AUDIO_TAPER_DB` in sound_usb.c, `taper_gain`
// in hda.c), because the per-app slider sits in the same flyout as the
// master and two sliders that feel different at the same position is a
// bug a user reports as "the volume is wrong".
//
// LINEAR IN AMPLITUDE WAS THE OTHER CANDIDATE and it is what
// usnd_set_volume() still does (`g_acc[i] * g_volume / 100`), which
// puts 50% at -6 dB against this table's -20 dB. That inconsistency is
// the open "one volume taper for every card" roadmap item and is NOT
// fixed here; what matters for this file is matching the knob beside it.
//
// Q15, every 5%, interpolated between -- 21 entries rather than 101
// because the error is far below a step of the slider, and rather than
// a pow() because there is no libm here. 0 is MUTE, not -40 dB, which
// is the same choice the cards make at the bottom of the range.
static const uint16_t g_taper[21] = {
        0,   413,   519,   654,   823,  1036,  1305,  1642,  2068,  2603,
     3277,  4125,  5193,  6538,  8231, 10362, 13045, 16423, 20675, 26029,
    32768,
};

static int32_t gain_q15(int pct) {
    if (pct <= 0) return 0;
    if (pct >= 100) return 32768;
    int i = pct / 5, frac = pct % 5;
    int32_t a = g_taper[i], b = g_taper[i + 1];
    return a + (b - a) * frac / 5;
}

// CLIPPED ONCE, AT THE END: the clients are summed in 64 bits, where any
// number of full-scale s32 streams fits, and only the total is clamped --
// wrapping would turn a loud moment into a crack. Clamping per client
// (what the s16 mixer did) made the result depend on the ORDER the
// clients were added in, whenever one pushed the sum over and the next
// pulled it back.
static int32_t clamp32(int64_t v) {
    if (v > INT32_MAX) return INT32_MAX;
    if (v < INT32_MIN) return INT32_MIN;
    return (int32_t)v;
}

static int name_is_client(const char *n) {
    return strncmp(n, SND_CLIENT_PREFIX, sizeof SND_CLIENT_PREFIX - 1) == 0 &&
           strcmp(n, SND_SERVER_NAME) != 0;
}

static struct client *find(const char *name) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (g_cl[i].ctl && strcmp(g_cl[i].name, name) == 0) return &g_cl[i];
    return 0;
}

static void drop(struct client *c) {
    sys_munmap((void *)c->ctl, SND_CLIENT_BYTES);
    sys_close(c->fd);
    memset(c, 0, sizeof *c);
}

static void adopt(const char *name) {
    struct client *c = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) if (!g_cl[i].ctl) { c = &g_cl[i]; break; }
    if (!c) return; // full: the client writes into a ring nobody reads

    int fd = sys_shm_open(name, 0, 0);
    if (fd < 0) return;
    void *p = sys_mmap(0, SND_CLIENT_BYTES, SYS_PROT_READ | SYS_PROT_WRITE,
                       SYS_MAP_SHARED, fd, 0);
    if (p == (void *)-1) { sys_close(fd); return; }

    volatile struct snd_ctl_page *ctl = p;
    // The magic is written LAST by the client, so an unset one means
    // "not ready yet" rather than "broken" -- unmap and look again next
    // pass. Anything else is a ring this build cannot mix.
    if (ctl->magic != SND_CTL_MAGIC || ctl->rate != SND_RATE ||
        ctl->channels != SND_CHANNELS || ctl->ring_bytes != SND_RING_BYTES ||
        ctl->sample_bits != SND_SAMPLE_BITS) {
        sys_munmap(p, SND_CLIENT_BYTES);
        sys_close(fd);
        return;
    }
    strlcpy(c->name, name, sizeof c->name);
    strlcpy(c->app, (const char *)ctl->app, sizeof c->app);
    c->pid = atoi(name + sizeof SND_CLIENT_PREFIX - 1);
    c->gain = 100;   // until the config is read; silence-by-default would
                     // make a missing file sound like a broken daemon
    c->fd = fd;
    c->ring = (volatile int32_t *)((uintptr_t)p + 4096);
    c->ctl = ctl; // last: a non-null ctl is what makes the slot live
    fprintf(stderr, "soundd: client %s as \"%s\"\n", name,
            c->app[0] ? c->app : "(unnamed)");
}

// --- per-application volume ------------------------------------------
//
// /etc/sound.conf, one `<app> = <percent>` key per program. Read HERE
// rather than pushed by whoever moves the slider: the daemon has no
// IPC at all, and giving it one to carry a single integer would be a
// protocol built before the feature.
//
// RELOADED ON CHANGE, NOT ON A TIMER. The main loop runs every 20 ms,
// and parsing the file at that rate is 50 reads a second for a value
// that changes when a human drags something. `stat` is one syscall and
// answers "has it changed" from the size and mtime, so the parse is
// paid only when it actually did -- which also makes a slider feel
// immediate rather than sampled.
static uint64_t g_cfg_stamp;

// ONE READ, MANY KEYS -- uconf_load() rather than uconf_get() per
// client, which re-reads the whole document per call (api's own note:
// a nine-entry desktop reload once cost 54 whole-file reads).
static struct etc_config_buf g_cfg;

static void apply_gains(void) {
    struct etc_config_buf *b = &g_cfg;
    int have = uconf_load(SND_CONFIG_FILE, b);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct client *c = &g_cl[i];
        if (!c->ctl) continue;
        char v[8];
        // AN UNNAMED CLIENT IS NOT LOOKED UP AT ALL. Its key would be
        // "", which every other unnamed client shares -- one slider
        // moving several programs at once.
        int was = c->gain;
        if (have && c->app[0] && etc_config_buf_get(b, c->app, v, sizeof v)) {
            int pct = atoi(v);
            c->gain = pct < 0 ? 0 : pct > 100 ? 100 : pct;
        } else {
            c->gain = 100;
        }
        // ON CHANGE ONLY -- the poll runs 50 times a second and a probe
        // that outruns the klog ring destroys the evidence it gathers.
        if (c->gain != was)
            fprintf(stderr, "soundd: %s volume %d%%\n",
                    c->app[0] ? c->app : c->name, c->gain);
    }
}

// Cheap enough for the 20 ms loop, and the only thing that triggers a
// parse. A file that does not exist is a stamp of 0, which is also what
// it reads as before anything has ever set a per-app volume.
static void gains_poll(int force) {
    struct stat st;
    uint64_t stamp = (stat(SND_CONFIG_FILE, &st) == 0)
                   ? ((uint64_t)st.st_mtime << 32) ^ (uint64_t)st.st_size : 0;
    if (!force && stamp == g_cfg_stamp) return;
    g_cfg_stamp = stamp;
    apply_gains();
}

// --- the roster ------------------------------------------------------
//
// Published into the BEACON, which was a 4 KiB object with nothing in
// it. A mixer UI needs exactly this list and the daemon is the only
// thing that has it; writing it here costs no new object and no new
// protocol, and keeps a UI from opening each client's own ring to find
// out who is playing.
static void publish_roster(void) {
    static volatile struct snd_roster *r;
    if (!r) {
        void *p = sys_mmap(0, 4096, SYS_PROT_READ | SYS_PROT_WRITE,
                           SYS_MAP_SHARED, g_beacon, 0);
        if (p == (void *)-1) return;
        r = p;
        r->magic = SND_CTL_MAGIC;
    }
    // ODD WHILE WRITING, so a reader that catches the middle sees it and
    // looks again. A torn roster is a slider drawn one frame late, which
    // is not worth a lock in a page a dying daemon can leave behind.
    r->gen = r->gen + 1;
    uint32_t n = 0;
    for (int i = 0; i < MAX_CLIENTS && n < SND_ROSTER_MAX; i++) {
        struct client *c = &g_cl[i];
        if (!c->ctl) continue;
        strlcpy((char *)r->e[n].app, c->app, SND_APP_MAX);
        r->e[n].pid = c->pid;
        r->e[n].gain = (uint32_t)c->gain;
        r->e[n].playing = c->ctl->running ? 1 : 0;
        n++;
    }
    r->count = n;
    r->gen = r->gen + 1;
}

// Walk the shm namespace: adopt any ring we do not hold, and drop any
// we hold that has gone. Polled rather than notified because nothing
// here can wake on a namespace change -- the same reason netd polls for
// a card appearing.
static void rescan(void) {
    // Snapshotted rather than walked twice: the namespace can change
    // between the two passes below, and a client adopted in one and
    // reported missing by the other would be dropped the same pass.
    static char seen[MAX_CLIENTS * 2][SHM_NAME_MAX];
    int nseen = 0;
    struct query_shm rec;
    QUERY_FOREACH(QUERY_SHM, rec, i) {
        if (!name_is_client(rec.name)) continue;
        // AN UNLINKED OBJECT IS OUT OF THE NAMESPACE, so it is not a
        // client any more even though it still has frames and still
        // appears here. The kernel releases a dead creator's name
        // (kernel/mm/shm.c), and THIS is what turns that into the
        // daemon letting go: watching the list without the flag, it
        // kept holding a corpse -- whose own reference was the only
        // thing keeping the name alive -- and then refused to adopt the
        // live ring a recycled pid created behind it.
        if (rec.flags & QUERY_SHM_UNLINKED) continue;
        if (nseen < (int)(sizeof seen / sizeof seen[0]))
            strlcpy(seen[nseen++], rec.name, SHM_NAME_MAX);
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!g_cl[i].ctl) continue;
        int live = 0;
        for (int j = 0; j < nseen; j++)
            if (strcmp(seen[j], g_cl[i].name) == 0) { live = 1; break; }
        if (!live) {
            fprintf(stderr, "soundd: client %s gone\n", g_cl[i].name);
            drop(&g_cl[i]);
        }
    }
    int adopted = 0;
    for (int j = 0; j < nseen; j++)
        if (!find(seen[j])) { adopt(seen[j]); adopted = 1; }
    // A NEW CLIENT NEEDS ITS GAIN BEFORE ITS FIRST CHUNK, and the file
    // has not changed, so the mtime poll would not have looked.
    if (adopted) gains_poll(1);
}

// One chunk: sum every running client into the hardware ring, then zero
// what each of them just gave us and move its position on. Zeroing is
// the ABI's rule and it is what makes a dead client fall silent instead
// of looping -- the kernel does exactly this for us.
// Does every playing client have a whole chunk to give?
static int clients_ready(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        const struct client *c = &g_cl[i];
        if (!c->ctl || !c->ctl->running) continue;
        uint32_t avail = (c->ctl->wr_pos + SND_RING_BYTES - c->ctl->hw_pos) % SND_RING_BYTES;
        if (avail < SND_CHUNK_BYTES) return 0;
    }
    return 1;
}

#define CHUNK_SAMPLES (SND_CHUNK_FRAMES * SND_CHANNELS)

static void mix_chunk(uint32_t dst) {
    static int64_t acc[CHUNK_SAMPLES];
    static int32_t out[CHUNK_SAMPLES];
    memset(acc, 0, sizeof acc);

    int voices = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct client *c = &g_cl[i];
        if (!c->ctl || !c->ctl->running) continue;
        uint32_t src = c->ctl->hw_pos;
        // NEVER OVERTAKE THE WRITER. The hardware may run ahead of a
        // starved client -- the kernel zeroes what it consumed, so
        // overrun plays silence -- but a daemon must not, because its
        // client derives "where may I write" from `hw_pos`. Parked one
        // chunk ahead of the writer, that reads as no room FOREVER and
        // stalls the client's decoder instead of merely going quiet
        // (aplay never exited; found on the laptop, not in QEMU).
        if (src == c->ctl->wr_pos) { g_starved_chunks++; continue; } // silence

        const volatile int32_t *s = c->ring + src / 4;
        int32_t g = gain_q15(c->gain);
        if (g == 32768) {
            for (unsigned k = 0; k < CHUNK_SAMPLES; k++) acc[k] += s[k];
        } else if (g != 0) {
            for (unsigned k = 0; k < CHUNK_SAMPLES; k++)
                acc[k] += ((int64_t)s[k] * g) >> 15;
        }
        // g == 0 is muted: the chunk is still CONSUMED and zeroed below,
        // so a muted client keeps playing into nothing rather than
        // stalling on a ring that never drains.
        for (unsigned k = 0; k < CHUNK_SAMPLES; k++)
            ((volatile int32_t *)s)[k] = 0;
        c->ctl->hw_pos = (src + SND_CHUNK_BYTES) % SND_RING_BYTES;
        voices++;
    }
    (void)voices;
    for (unsigned k = 0; k < CHUNK_SAMPLES; k++) out[k] = clamp32(acc[k]);
    memcpy((void *)((uintptr_t)g_hwring + dst), out, SND_CHUNK_BYTES);
}

int main(void) {
    // A service's fd 1 reaches nobody; fd 2 is the kernel log.
    // EXIT 0 WITH NO CARD, so `Restart=on-failure` leaves this `exited`
    // rather than crash-looping on every machine that has no sound
    // hardware -- which is most of the emulated ones.
    if (!stream_open()) {
        fprintf(stderr, "soundd: no sound device -- nothing to serve\n");
        return 0;
    }


    // The beacon goes up only once the card is ours: a client that sees
    // it must not then be told the daemon has no output.
    // PUBLIC: a beacon is the rendezvous every client has to find.
    g_beacon = sys_shm_open(SND_SERVER_NAME, 4096, SHM_CREATE | SHM_PUBLIC);
    if (g_beacon < 0) {
        fprintf(stderr, "soundd: could not claim %s -- another daemon?\n",
                SND_SERVER_NAME);
        return 1;
    }
    sys_notify_ready();
    // THE DAEMON IS UP AND HAS A DEVICE, which is a different fact from
    // holding the card right now -- it keeps that one while suspending
    // on idle hands the card back and forth. tools/soundd_test.py and
    // tools/audio_test.py both wait on this line.
    fprintf(stderr, "soundd: serving on the hardware stream\n");

    g_wr = 0;
    g_idle_since = now_ms();
    uint32_t stall_pos = 0;
    uint64_t stall_since = now_ms();
    int stalled = 0;
    for (;;) {
        rescan();
        gains_poll(0);
        publish_roster();

        int playing = anyone_playing();
        if (playing) {
            g_idle_since = 0;
            // A CLIENT CAN ARRIVE WHILE THE CARD IS RELEASED, which is
            // the point: it writes into its own ring and finds the
            // beacon whether or not we hold the stream, and this is
            // where we go and get it back.
            if (!g_open && !stream_open()) {
                // Somebody else has it -- a KTEST, /tests/tone. Wait
                // rather than spin: this is not an error, it is the
                // sharing this whole mechanism exists to allow.
                sys_sleep_ms(100);
                continue;
            }
        } else if (g_open) {
            if (!g_idle_since) g_idle_since = now_ms();
            if (now_ms() - g_idle_since >= SUSPEND_IDLE_MS) stream_release();
        }

        if (!g_open) { sys_sleep_ms(20); continue; }

        unsigned long long pass_ms = now_ms();
        if (g_last_pass_ms && pass_ms - g_last_pass_ms > g_gap_max_ms)
            g_gap_max_ms = (unsigned)(pass_ms - g_last_pass_ms);
        g_last_pass_ms = pass_ms;

        // Fill toward one chunk behind the hardware -- but only with
        // what the clients have actually produced, or with silence
        // when the engine is about to run out (MIN_LEAD_BYTES). Before
        // the engine runs there is no hurry, so nothing is padded.
        int engine = g_running && g_hw->running;
        uint32_t limit = (g_hw->hw_pos + SND_RING_BYTES - SND_CHUNK_BYTES) % SND_RING_BYTES;
        uint32_t lead = (g_wr + SND_RING_BYTES - g_hw->hw_pos) % SND_RING_BYTES;
        while (g_wr != limit) {
            if (!clients_ready() && !(engine && lead < MIN_LEAD_BYTES)) break;
            mix_chunk(g_wr);
            g_wr = (g_wr + SND_CHUNK_BYTES) % SND_RING_BYTES;
            lead += SND_CHUNK_BYTES;
        }

        // A DEAD hw_pos IS ARITHMETICALLY IDENTICAL TO A FULL RING, and
        // that is why a stalled engine used to be permanent silence
        // with nothing logged at all. `limit` is derived from hw_pos,
        // so when the consumer stops advancing `limit` stops with it --
        // and a ring whose limit has reached our cursor is exactly what
        // "fully buffered" looks like. The loop above then does the
        // correct thing for a full ring, which is nothing, for ever.
        // Measured on a stalled ring-3 USB driver: hw_pos frozen at
        // 4096, limit computed to 2048, g_wr already 2048, so `filled`
        // was 0 once a second for as long as it was watched while the
        // card played silence and aplay never exited.
        //
        // ALSA does not trust the pointer either -- it has an xrun
        // timeout beside snd_pcm_update_hw_ptr() for this exact case.
        // A consumer that has not moved for STALL_MS while clients are
        // playing is not a full buffer, it is a dead engine: say so,
        // and ask for one restart. ONCE per stall, because the klog
        // ring holds a few hundred lines and a probe that outruns it
        // destroys the evidence it is gathering.
        // AN ENGINE THAT HAS NOT STARTED YET IS NOT A STALLED ONE.
        // hw_pos is legitimately 0 between taking the card and the
        // driver programming the chip, and counting that as a stall
        // fired a misleading line on every first client. The control
        // page's `running` is the driver's own answer, so wait for it.
        if (!playing || !g_hw->running) {
            stall_since = now_ms();
            stalled = 0;
        } else if (g_hw->hw_pos != stall_pos) {
            stall_pos = g_hw->hw_pos;
            stall_since = now_ms();
            stalled = 0;
        } else if (!stalled && now_ms() - stall_since >= STALL_MS) {
            stalled = 1;
            fprintf(stderr, "soundd: hw_pos stuck at %u for %ums -- the engine "
                            "has stalled, not the buffer; asking for a restart\n",
                    (unsigned)stall_pos, (unsigned)STALL_MS);
            g_running = 0;          // makes the START below fire
        }

        // THE CONTROL PAGE OUTRANKS OUR OWN FLAG, usnd_sink_dev.c's
        // rule: the kernel clears `running` when the device this stream
        // was opened on goes away, and a daemon that trusted its own
        // flag would mix into a ring nobody plays, forever.
        //
        // AND NOT BEFORE THERE IS SOMETHING TO PLAY: an engine started
        // on two chunks is padding silence a pass later. A lead that
        // stopped growing is a short sound that has all arrived.
        if (!g_running || !g_hw->running) {
            if (lead >= MIN_LEAD_BYTES || (lead && lead == g_prev_lead)) {
                if (sys_snd_ctl(SND_CTL_START) == 0) g_running = 1;
            }
            g_prev_lead = lead;
        }

        // NEARLY TWO CHUNKS, not half of one: a chunk is 2048 bytes =
        // 512 frames = 10.67 ms at 48 kHz (the comment here used to say
        // "half a chunk", which was wrong by 4x). It is still far
        // inside the ring's 341 ms, which is what keeps it from running
        // dry between passes.
        sys_sleep_ms(20);
    }
}
