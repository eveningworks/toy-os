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
#include "rt/sys.h"
#include "sound_abi.h"
#include "syscall_abi.h"
#include "query_abi.h"

// Eight is the shm table's own ceiling for audio and more voices than
// this machine has reason to play at once.
#define MAX_CLIENTS 8

struct client {
    char     name[SHM_NAME_MAX];
    int      fd;
    volatile struct snd_ctl_page *ctl;
    volatile int16_t *ring;
};

static struct client g_cl[MAX_CLIENTS];
static int g_beacon = -1;

static volatile struct snd_ctl_page *g_hw;
static volatile int16_t *g_hwring;
static uint32_t g_wr;      // our write cursor into the hardware ring
static int g_running;

// Saturating, because a sum of several s16 streams does not fit in one:
// wrapping turns a loud moment into a crack, and clipping is what every
// mixer does instead.
static int16_t sat_add(int32_t a, int32_t b) {
    int32_t v = a + b;
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
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
        ctl->channels != SND_CHANNELS || ctl->ring_bytes != SND_RING_BYTES) {
        sys_munmap(p, SND_CLIENT_BYTES);
        sys_close(fd);
        return;
    }
    strlcpy(c->name, name, sizeof c->name);
    c->fd = fd;
    c->ring = (volatile int16_t *)((uintptr_t)p + 4096);
    c->ctl = ctl; // last: a non-null ctl is what makes the slot live
    fprintf(stderr, "soundd: client %s\n", name);
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
    for (int j = 0; j < nseen; j++)
        if (!find(seen[j])) adopt(seen[j]);
}

// One chunk: sum every running client into the hardware ring, then zero
// what each of them just gave us and move its position on. Zeroing is
// the ABI's rule and it is what makes a dead client fall silent instead
// of looping -- the kernel does exactly this for us.
static void mix_chunk(uint32_t dst) {
    static int16_t acc[SND_CHUNK_BYTES / 2];
    memset(acc, 0, sizeof acc);

    int voices = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct client *c = &g_cl[i];
        if (!c->ctl || !c->ctl->running) continue;
        uint32_t src = c->ctl->hw_pos;
        const volatile int16_t *s = c->ring + src / 2;
        for (unsigned k = 0; k < SND_CHUNK_BYTES / 2; k++)
            acc[k] = sat_add(acc[k], s[k]);
        for (unsigned k = 0; k < SND_CHUNK_BYTES / 2; k++)
            ((volatile int16_t *)s)[k] = 0;
        c->ctl->hw_pos = (src + SND_CHUNK_BYTES) % SND_RING_BYTES;
        voices++;
    }
    (void)voices;
    memcpy((void *)((uintptr_t)g_hwring + dst), acc, SND_CHUNK_BYTES);
}

int main(void) {
    // A service's fd 1 reaches nobody; fd 2 is the kernel log.
    // EXIT 0 WITH NO CARD, so `Restart=on-failure` leaves this `exited`
    // rather than crash-looping on every machine that has no sound
    // hardware -- which is most of the emulated ones.
    if (sys_snd_open() != 0) {
        fprintf(stderr, "soundd: no sound device -- nothing to serve\n");
        return 0;
    }
    g_hw = (volatile struct snd_ctl_page *)(uintptr_t)SND_MAP_VADDR;
    g_hwring = (volatile int16_t *)(uintptr_t)(SND_MAP_VADDR + 4096);
    if (g_hw->magic != SND_CTL_MAGIC) {
        fprintf(stderr, "soundd: the stream is not the ABI I know\n");
        return 1;
    }


    // The beacon goes up only once the card is ours: a client that sees
    // it must not then be told the daemon has no output.
    g_beacon = sys_shm_open(SND_SERVER_NAME, 4096, SHM_CREATE);
    if (g_beacon < 0) {
        fprintf(stderr, "soundd: could not claim %s -- another daemon?\n",
                SND_SERVER_NAME);
        return 1;
    }
    sys_notify_ready();
    fprintf(stderr, "soundd: serving on the %s stream\n", "hardware");

    g_wr = 0;
    for (;;) {
        rescan();

        // Fill every chunk between our cursor and one behind the
        // hardware's. Before the engine starts hw_pos is 0, so the
        // first pass primes the whole ring.
        uint32_t limit = (g_hw->hw_pos + SND_RING_BYTES - SND_CHUNK_BYTES) % SND_RING_BYTES;
        while (g_wr != limit) {
            mix_chunk(g_wr);
            g_wr = (g_wr + SND_CHUNK_BYTES) % SND_RING_BYTES;
        }

        // THE CONTROL PAGE OUTRANKS OUR OWN FLAG, usnd_sink_dev.c's
        // rule: the kernel clears `running` when the device this stream
        // was opened on goes away, and a daemon that trusted its own
        // flag would mix into a ring nobody plays, forever.
        if (!g_running || !g_hw->running) {
            if (sys_snd_ctl(SND_CTL_START) == 0) g_running = 1;
        }

        // Half a chunk. Long enough that this is not a spin, short
        // enough that the ring never runs dry between passes.
        sys_sleep_ms(20);
    }
}
