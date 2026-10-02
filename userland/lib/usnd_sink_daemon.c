// The sound daemon as a usnd sink: a shared-memory ring this process
// owns, which /bin/soundd mixes into the one hardware stream.
//
// This is usnd_sink.h's second row, and the reason apps needed no
// change to become audible together -- the cursor arithmetic below is
// usnd_sink_dev.c's, because the ring contract is the same one
// (abi/sound_abi.h). What differs is only where the memory comes from
// and who advances `hw_pos`: the kernel there, the daemon here.
//
// **THE BEACON IS THE ONLY THING THAT DECIDES.** A client picks this
// sink when SND_SERVER_NAME exists and the device sink otherwise; there
// is no connect() to fail and no timeout to tune. If the daemon dies
// the beacon goes with it, so the next usnd_init() falls through to the
// kernel stream on its own.
#include <string.h>
#include <stdio.h>
#include "lib/usnd.h"
#include "lib/usnd_sink.h"
#include "rt/sys.h"
#include "query_abi.h"
#include "sound_abi.h"
#include "syscall_abi.h"
#include "proc_info.h" // struct proc_info -- this process's own name

static volatile struct snd_ctl_page *g_ctl;
static volatile int16_t *g_ring;
static uint32_t g_wr;
static int g_fd = -1;
static char g_name[SHM_NAME_MAX];
// An explicit name beats the process's own, for a program that plays on
// something else's behalf. Empty until usnd_set_app_name() is called.
static char g_app[SND_APP_MAX];

// This process's own name, which is the stable half of an identity a
// per-application volume can be remembered by -- the ring is `snd.<pid>`
// and a pid means nothing next boot. Walked by SLOT because that is
// what SYS_PROC_INFO enumerates; an empty slot is a successful call
// reporting pid 0, so this skips rather than stops.
static void fill_app(volatile struct snd_ctl_page *ctl) {
    if (g_app[0]) { strlcpy((char *)ctl->app, g_app, SND_APP_MAX); return; }
    int me = sys_getpid();
    struct proc_info info;
    for (int i = 0; sys_proc_info(i, &info) == 0; i++) {
        if (info.pid != me) continue;
        strlcpy((char *)ctl->app, info.name, SND_APP_MAX);
        return;
    }
    ctl->app[0] = '\0';   // unknown is legal: it mixes at full gain
}

void usnd_set_app_name(const char *name) {
    if (name) strlcpy(g_app, name, sizeof g_app);
    else g_app[0] = '\0';
}

static int daemon_open(void) {
    if (g_ctl) return -EBUSY;

    int beacon = sys_shm_open(SND_SERVER_NAME, 0, 0);
    if (beacon < 0) return -ENODEV; // no daemon: the device sink's turn
    sys_close(beacon);

    // Named by pid, so two clients never collide and a name outliving
    // its process is visibly stale. SHM_EXCL is deliberate: a leftover
    // ring from a pid this one reuses would arrive with somebody else's
    // cursors in it.
    snprintf(g_name, sizeof g_name, SND_CLIENT_PREFIX "%d", sys_getpid());
    sys_shm_unlink(g_name);
    g_fd = sys_shm_open(g_name, SND_CLIENT_BYTES, SHM_CREATE | SHM_EXCL);
    if (g_fd < 0) return -sys_errno();

    // THE DAEMON IS LET IN, AND ONLY IT. This ring is one client's audio
    // and is private like any other named object; before objects had an
    // owner, any process could open it from `lsshm` and write into
    // somebody else's playback. Its pid is the beacon's creator, which
    // is what SND_SERVER_NAME's own comment says QUERY_SHM reports.
    struct query_shm rec;
    QUERY_FOREACH(QUERY_SHM, rec, qi) {
        if (strcmp(rec.name, SND_SERVER_NAME) != 0) continue;
        sys_shm_grant(g_name, rec.creator_pid);
        break;
    }

    void *p = sys_mmap(0, SND_CLIENT_BYTES, SYS_PROT_READ | SYS_PROT_WRITE,
                       SYS_MAP_SHARED, g_fd, 0);
    if (p == (void *)-1) {
        int e = sys_errno();
        sys_close(g_fd); sys_shm_unlink(g_name); g_fd = -1;
        return -e;
    }

    g_ctl = (volatile struct snd_ctl_page *)p;
    g_ring = (volatile int16_t *)((uintptr_t)p + 4096);
    g_ctl->rate = USND_RATE;
    g_ctl->channels = USND_CHANNELS;
    g_ctl->ring_bytes = SND_RING_BYTES;
    g_ctl->hw_pos = 0;
    g_ctl->wr_pos = 0;
    g_ctl->running = 0;
    g_ctl->device_gone = 0;
    fill_app(g_ctl);   // before the magic, like everything else here
    // LAST, and that is the handshake: the daemon ignores a ring whose
    // magic is unset, so every other field is in place before it may
    // look at any of them.
    g_ctl->magic = SND_CTL_MAGIC;
    g_wr = 0;
    return 0;
}

static uint32_t writable_bytes(void) {
    uint32_t limit = (g_ctl->hw_pos + SND_RING_BYTES - SND_CHUNK_BYTES) % SND_RING_BYTES;
    return (limit + SND_RING_BYTES - g_wr) % SND_RING_BYTES;
}

static long daemon_space(void) {
    if (!g_ctl) return 0;
    return (long)(writable_bytes() / SND_FRAME_BYTES);
}

static long daemon_write(const int16_t *pcm, long frames) {
    if (!g_ctl) return 0;
    if (g_ctl->device_gone) return 0; // the daemon left; nothing reads this

    long room = daemon_space();
    if (frames > room) frames = room;
    if (frames <= 0) return 0;

    uint32_t bytes = (uint32_t)frames * SND_FRAME_BYTES;
    uint32_t first = SND_RING_BYTES - g_wr;
    if (first > bytes) first = bytes;
    memcpy((void *)((uintptr_t)g_ring + g_wr), pcm, first);
    if (bytes > first)
        memcpy((void *)g_ring, (const uint8_t *)pcm + first, bytes - first);
    g_wr = (g_wr + bytes) % SND_RING_BYTES;
    // PUBLISHED BEFORE `running`, so the daemon never sees a ring it is
    // allowed to consume with a write cursor still at zero.
    g_ctl->wr_pos = g_wr;

    // After the first write, as the device sink starts the engine after
    // its first: the ring is primed, so the daemon's first pass finds
    // samples rather than the silence it would otherwise mix.
    g_ctl->running = 1;
    return frames;
}

static long daemon_pending(void) {
    if (!g_ctl) return 0;
    uint32_t used = (g_wr + SND_RING_BYTES - g_ctl->hw_pos) % SND_RING_BYTES;
    return (long)(used / SND_FRAME_BYTES);
}

static void daemon_flush(void) {
    if (!g_ctl) return;
    g_wr = (g_ctl->hw_pos + SND_CHUNK_BYTES) % SND_RING_BYTES;
    g_ctl->wr_pos = g_wr;
}

static void daemon_close(void) {
    if (!g_ctl) return;
    g_ctl->running = 0;
    sys_munmap((void *)g_ctl, SND_CLIENT_BYTES);
    sys_close(g_fd);
    // The name goes now so the daemon stops mixing us. The frames go
    // with the daemon's own mapping, whenever it notices -- which is
    // what makes an app that exits without getting here harmless.
    sys_shm_unlink(g_name);
    g_fd = -1;
    g_ctl = 0;
    g_ring = 0;
}

const struct usnd_sink usnd_sink_daemon = {
    .name  = "daemon",
    .open  = daemon_open,
    .write = daemon_write,
    .space = daemon_space,
    .pending = daemon_pending,
    .flush = daemon_flush,
    .close = daemon_close,
};
