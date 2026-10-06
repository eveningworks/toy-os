// The kernel's exclusive PCM stream as a usnd sink: SYS_SND_OPEN's
// mapped ring (abi/sound_abi.h), written ahead of the hardware's
// position with no syscall in steady state.
//
// THE REFILL RULE IS THE ABI'S: the chunk at `hw_pos` belongs to the
// hardware, so this may write from its own cursor up to one chunk
// BEHIND hw_pos. The kernel zeroes a chunk it has finished, which is
// why an idle sink needs no silence written into it and why a starved
// one plays quiet rather than looping.
//
// **THE ENGINE IS STARTED ONCE AND NEVER STOPPED** until close, and
// restarted only when the KERNEL says it stopped (`running` in the
// control page). SND_CTL_START resumes from ring offset 0, so a
// stop/start pair would have to reset the write cursor in step with it;
// leaving the engine running costs one interrupt per chunk over a ring
// the kernel is already zeroing, and silence is what an idle ring
// holds. The one thing that must NOT be assumed is that it is still
// running because we started it: the device can change underneath a
// live stream (kernel/sound.h).
#include <string.h>
#include "lib/usnd.h"
#include "lib/usnd_sink.h"
#include "rt/sys.h"
#include "sound_abi.h"
#include "query_abi.h"
#include "lib/usndfmt.h"

static volatile struct snd_ctl_page *g_ctl;
static volatile int32_t *g_ring;
static uint32_t g_wr;        // byte cursor into the ring
static int g_open, g_running;
static uint32_t g_want = ~0u, g_agreed;   // dev_rate()'s last question and answer

static int dev_open(void) {
    if (g_open) return -EBUSY;
    if (sys_snd_open() != 0) return -sys_errno();

    g_ctl = (volatile struct snd_ctl_page *)(uintptr_t)SND_MAP_VADDR;
    g_ring = (volatile int32_t *)(uintptr_t)(SND_MAP_VADDR + 4096);
    if (g_ctl->magic != SND_CTL_MAGIC ||
        g_ctl->channels != USND_CHANNELS || g_ctl->sample_bits != SND_SAMPLE_BITS) {
        sys_snd_ctl(SND_CTL_CLOSE);
        return -ENOTSUP;
    }
    g_wr = 0;
    g_open = 1;
    g_running = 0;
    g_want = ~0u;
    return 0;
}

// WITH NO DAEMON THIS PROCESS DECIDES, by the card's own setting and
// lib/usndfmt.h's one rule -- soundd's decision, made here. A switch
// waits for what is queued to play out (the caller writes nothing while
// this answers 0), then stops the engine and sets the format; the next
// write starts it again.
static uint32_t dev_rate(uint32_t want) {
    if (!g_open) return 0;
    // ASKED ONCE PER QUESTION: the worker calls this every pass, and the
    // answer only changes with `want` or under the card.
    if (want == g_want && g_agreed == g_ctl->rate) return g_agreed;
    struct query_sound q;
    char card[sizeof q.name] = "";
    uint32_t rates = 0, depths = 0;
    QUERY_FOREACH(QUERY_SOUND, q, qi)
        if (q.active) { strlcpy(card, q.name, sizeof card); rates = q.rates; depths = q.depths; }
    struct usndfmt f;
    usndfmt_load(card, &f);
    // NO PREFERENCE KEEPS THE CARD WHERE IT IS under `match` -- a sound
    // effect is not a reason to switch -- while a fixed rate always holds.
    uint32_t r = (!want && f.match && (rates & snd_rate_mask(g_ctl->rate)))
                 ? g_ctl->rate : usndfmt_pick_rate(&f, rates, want);
    if (r == g_ctl->rate) { g_want = want; g_agreed = r; return r; }
    if (g_running && g_ctl->running) {
        uint32_t used = (g_wr + SND_RING_BYTES - g_ctl->hw_pos) % SND_RING_BYTES;
        if (used > SND_CHUNK_BYTES) return 0;   // still playing the old rate
        sys_snd_ctl(SND_CTL_STOP);
    }
    g_running = 0;
    g_wr = 0;
    if (sys_snd_format(r, usndfmt_pick_bits(&f, depths)) != 0)
        sys_snd_format(g_ctl->rate, 0);     // the card's width back, at its rate
    g_want = want;
    g_agreed = g_ctl->rate;
    return g_agreed;
}

// Bytes between the write cursor and one chunk behind the hardware.
// Before the engine starts hw_pos is 0, so this reports the whole ring
// minus the guard chunk -- which is what primes it in one write.
static uint32_t writable_bytes(void) {
    uint32_t limit = (g_ctl->hw_pos + SND_RING_BYTES - SND_CHUNK_BYTES) % SND_RING_BYTES;
    return (limit + SND_RING_BYTES - g_wr) % SND_RING_BYTES;
}

static long dev_space(void) {
    if (!g_open) return 0;
    return (long)(writable_bytes() / SND_FRAME_BYTES);
}

static long dev_write(const int32_t *pcm, long frames) {
    if (!g_open) return 0;

    long room = dev_space();
    if (frames > room) frames = room;
    if (frames <= 0) return 0;

    // One or two memcpys: the ring wraps, the caller's buffer does not.
    uint32_t bytes = (uint32_t)frames * SND_FRAME_BYTES;
    uint32_t first = SND_RING_BYTES - g_wr;
    if (first > bytes) first = bytes;
    memcpy((void *)((uintptr_t)g_ring + g_wr), pcm, first);
    if (bytes > first)
        memcpy((void *)g_ring, (const uint8_t *)pcm + first, bytes - first);
    g_wr = (g_wr + bytes) % SND_RING_BYTES;

    // Started after the first write, never before: the ring is primed
    // by then, so the hardware does not begin by racing the writer.
    //
    // THE CONTROL PAGE OUTRANKS OUR OWN FLAG. The kernel clears
    // `running` when the device this stream was opened on goes away,
    // and a sink that only trusted `g_running` would then write into a
    // ring nobody plays, forever, with no error anywhere -- which is
    // exactly how choosing a different output device muted the Audio
    // Player. Asking again costs one syscall per write only while the
    // engine is actually stopped.
    if (!g_running || !g_ctl->running) {
        if (sys_snd_ctl(SND_CTL_START) == 0) g_running = 1;
    }
    return frames;
}

// Queued but unplayed: from the hardware's position round to ours.
static long dev_pending(void) {
    if (!g_open) return 0;
    uint32_t used = (g_wr + SND_RING_BYTES - g_ctl->hw_pos) % SND_RING_BYTES;
    return (long)(used / SND_FRAME_BYTES);
}

// Rewind the write cursor to just after the chunk the hardware owns.
// The stale samples between there and the old cursor are not zeroed --
// the next refill overwrites them within one worker pass, and the one
// chunk already committed to the hardware plays either way.
static void dev_flush(void) {
    // Not before the engine has ever run: START resumes from ring
    // offset 0, so moving the cursor first would leave a gap the
    // hardware plays before reaching anything written.
    if (!g_open || !g_running) return;
    g_wr = (g_ctl->hw_pos + SND_CHUNK_BYTES) % SND_RING_BYTES;
}

static void dev_close(void) {
    if (!g_open) return;
    if (g_running) sys_snd_ctl(SND_CTL_STOP);
    sys_snd_ctl(SND_CTL_CLOSE);
    g_open = g_running = 0;
    g_ctl = 0;
    g_ring = 0;
}

const struct usnd_sink usnd_sink_device = {
    .name  = "device",
    .open  = dev_open,
    .rate  = dev_rate,
    .write = dev_write,
    .space = dev_space,
    .pending = dev_pending,
    .flush = dev_flush,
    .close = dev_close,
};
