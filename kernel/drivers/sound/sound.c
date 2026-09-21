// The sound core -- see kernel/sound.h for the class split (core owns
// the stream and its policy, a driver owns its hardware) and
// abi/sound_abi.h for the shared-ring ABI the app speaks.
#include "sound.h"
#include "query.h"
#include "query_abi.h"
#include "initcall.h"
#include "sound_abi.h"
#include "uaddr.h"
#include "syscall_table.h"
#include "errno.h"
#include "vmm.h"
#include "pmm.h"
#include "string.h"
#include "kfmt.h"
#include "klog.h"
#include "ktest.h"
#include "driver.h" // driver_bound() -- `lsdrv`

// driver-none: the sound class registry itself

// The registered devices. A LIST rather than the single slot this
// started as, because a USB card can arrive and leave while the machine
// runs -- and the choice between it and a built-in one is a person's,
// not a boot order's.
#define SND_MAX_DEVICES 4

static const struct sound_device *g_devs[SND_MAX_DEVICES];
static int g_dev_count;
static int g_active = -1;               // index into g_devs, -1 = none
static char g_pref[SOUND_NAME_MAX];     // "" or "auto" = newest wins
static int g_volume = 100;              // re-applied on every switch

// The ring plus its control page, one contiguous allocation so the
// driver's descriptors and the app's mapping walk the same frames.
// Layout: page 0 = struct snd_ctl_page, pages 1.. = the sample ring.
#define SND_MAP_BYTES (4096 + SND_RING_BYTES)
#define SND_MAP_PAGES (SND_MAP_BYTES / 4096)

static uint8_t *g_map;        // kernel view; NULL until first use
static uint64_t g_map_phys;
static struct snd_ctl_page *g_ctl;
static uint8_t *g_ring;

static uint64_t g_owner_pml4; // 0 = the stream is free
static uint32_t g_last_pos;   // hw_pos as of the previous interrupt

static const struct sound_device *active(void) {
    return g_active >= 0 ? g_devs[g_active] : 0;
}

static int pref_is_auto(void) {
    return g_pref[0] == 0 || k_strcmp(g_pref, "auto") == 0;
}

// Which device the current preference resolves to: the named one when
// it is present, else the FIRST one discovered. A pick is sticky -- it
// survives the chosen card being unplugged and takes effect again when
// it comes back, which is why an absent name falls through here rather
// than being rewritten to whatever is left.
static int resolve_pref(void) {
    if (!pref_is_auto()) {
        for (int i = 0; i < g_dev_count; i++)
            if (k_strcmp(g_devs[i]->name, g_pref) == 0) return i;
        // A named device that is not here: fall through to auto rather
        // than leaving the machine mute.
    }
    return g_dev_count ? 0 : -1;
}

static void activate(int idx) {
    if (idx == g_active) return;
    const struct sound_device *old = active();
    // A RUNNING STREAM MOVES TO THE NEW CARD. Stopping the old one and
    // leaving the new one idle is silence that never ends: ring 3
    // starts the engine ONCE and never again (userland/lib/
    // usnd_sink_dev.c says so in as many words), so nothing on that
    // side will ever ask for a start the switch did not perform. The
    // Audio Player holds its sink open across tracks, so choosing a
    // different device mid-session muted it permanently.
    int was_running = (g_ctl && g_ctl->running);
    if (old) old->stop();
    g_active = idx;
    const struct sound_device *dev = active();
    // The volume follows the stream, not the card: a switch that reset
    // it to whatever the new hardware powered up with would be a
    // surprise nobody asked for.
    if (dev && dev->set_volume) dev->set_volume(g_volume);
    if (dev && was_running) {
        // START resumes from the ring's first chunk, so the hardware is
        // briefly behind the writer and replays up to a ring's worth of
        // already-played samples before catching up. That is a glitch
        // measured in milliseconds; the alternative is the silence
        // above. The app is not told, because nothing about its ring
        // changed -- it keeps writing where it was.
        g_ctl->hw_pos = 0;
        g_last_pos = 0;
        if (dev->start() != 0) g_ctl->running = 0;
    }
}

void *sound_ring_alloc(uint64_t *out_phys) {
    if (!g_map) {
        uint64_t phys = pmm_alloc_contiguous(SND_MAP_PAGES, PMM_ZONE_DMA32);
        if (!phys) return 0;
        g_map = (uint8_t *)(uintptr_t)phys; // identity-mapped kernel view
        g_map_phys = phys;
        k_memset(g_map, 0, SND_MAP_BYTES);
        g_ctl = (struct snd_ctl_page *)g_map;
        g_ring = g_map + 4096;
        g_ctl->magic = SND_CTL_MAGIC;
        g_ctl->rate = SND_RATE;
        g_ctl->channels = SND_CHANNELS;
        g_ctl->ring_bytes = SND_RING_BYTES;
    }
    if (out_phys) *out_phys = g_map_phys + 4096;
    return g_ring;
}

int sound_register(const struct sound_device *dev, void *ring, uint64_t ring_phys) {
    (void)ring; (void)ring_phys; // the driver echoes what it was handed
    if (!dev || g_dev_count >= SND_MAX_DEVICES) return 0;
    for (int i = 0; i < g_dev_count; i++) if (g_devs[i] == dev) return 1;

    // The honesty check block and display already make. A device the
    // core cannot start or stop is not a worse device -- it is one that
    // silently plays nothing, and it would still be chosen over a
    // working one by resolve_pref().
    if (!dev->name || !dev->start || !dev->stop) {
        klog_printf(KLOG_ERR "sound: REFUSED %s -- missing start/stop\n",
                    dev->name ? dev->name : "(unnamed)");
        return 0;
    }

    g_devs[g_dev_count++] = dev;
    driver_bound(dev->driver, dev->name);
    klog_printf("sound: %s registered (48kHz s16le stereo, %u KiB ring)\n",
                dev->name, (unsigned)(SND_RING_BYTES / 1024));

    // A plug does not steal the stream: the newcomer is only switched
    // to when it is the one somebody CHOSE, and never while a process
    // is playing through the device it already opened.
    if (g_active < 0 || !g_owner_pml4) activate(resolve_pref());
    if (dev != active())
        klog_printf("sound: %s registered but %s is the active device\n",
                    dev->name, active() ? active()->name : "none");
    return 1;
}

void sound_unregister(const struct sound_device *dev) {
    int idx = -1;
    for (int i = 0; i < g_dev_count; i++) if (g_devs[i] == dev) { idx = i; break; }
    if (idx < 0) return;

    int was_active = (idx == g_active);
    int was_running = (was_active && g_ctl && g_ctl->running);
    if (was_active) {
        // Stop through the DEVICE rather than through activate(): the
        // hardware is gone, and the stream's owner has to be told
        // rather than silently handed a ring nobody is playing.
        dev->stop();
        if (g_ctl) {
            g_ctl->running = 0;
            g_ctl->device_gone = 1;
        }
        g_last_pos = 0;
        g_active = -1;
    }
    driver_unbound(dev->driver, dev->name);
    for (int i = idx; i + 1 < g_dev_count; i++) g_devs[i] = g_devs[i + 1];
    g_dev_count--;
    if (g_active > idx) g_active--;
    klog_printf("sound: %s removed\n", dev->name);
    if (was_active) {
        // `running` was cleared above, so activate() would not resume
        // on the fallback card -- restored here so it does. The stream
        // moving is what stops an unplug from muting an app forever;
        // `device_gone` stays set, so one that cares can still tell
        // that its hardware changed underneath it.
        if (g_ctl && was_running) g_ctl->running = 1;
        activate(resolve_pref());
        if (g_ctl && was_running && g_active < 0) g_ctl->running = 0;
    }
}

int sound_device_count(void) { return g_dev_count; }

const char *sound_device_name(int index) {
    if (index < 0 || index >= g_dev_count) return "";
    return g_devs[index]->name;
}

const char *sound_device_label(int index) {
    if (index < 0 || index >= g_dev_count) return "";
    return g_devs[index]->label ? g_devs[index]->label : g_devs[index]->name;
}

int sound_device_is_active(int index) { return index == g_active; }

const char *sound_active_name(void) {
    const struct sound_device *d = active();
    return d ? d->name : "";
}

int sound_select(const char *name) {
    if (!name) return 0;
    k_strlcpy(g_pref, name[0] ? name : "auto", sizeof g_pref);
    // A switch mid-playback stops the engine that was running: the ring
    // is the core's, so the new device picks it up from the start on
    // the next SND_CTL_START, and the app is not told (nothing about
    // its ring changed). Deliberate -- see docs/decisions.md.
    activate(resolve_pref());
    return 1;
}

const char *sound_preference(void) { return pref_is_auto() ? "auto" : g_pref; }

int sound_present(void) { return g_active >= 0; }

void sound_set_volume(int pct) {
    g_volume = pct;
    const struct sound_device *dev = active();
    if (dev && dev->set_volume) dev->set_volume(pct);
}

// The consumed-chunk zeroing -- abi/sound_abi.h's one rule. `hw_pos`
// only ever lands on chunk boundaries, and never laps itself between
// interrupts (an interrupt per chunk), so walking last..now is exact.
// WHAT THE DEVICE SAYS ABOUT ITSELF, published to the app watching the
// control page. A ring-0 driver has no use for this -- its start()
// returns having programmed the engine, so the core knows. A driver in
// a PROCESS only ever ASKED, so the answer arrives later and this is
// where it lands (kernel/drivers/sound/sound_proc.c).
void sound_publish_running(const struct sound_device *dev, int running) {
    if (!g_ctl || active() != dev) return;
    g_ctl->running = running ? 1 : 0;
}

void sound_period_done(uint32_t hw_pos) {
    if (!g_ctl) return;
    uint32_t pos = g_last_pos;
    while (pos != hw_pos) {
        k_memset(g_ring + pos, 0, SND_CHUNK_BYTES);
        pos = (pos + SND_CHUNK_BYTES) % SND_RING_BYTES;
    }
    g_last_pos = hw_pos;
    g_ctl->hw_pos = hw_pos;
}

static void snd_unmap_owner(void) {
    if (!g_owner_pml4) return;
    for (uint64_t i = 0; i < SND_MAP_PAGES; i++)
        vmm_unmap_user_page(g_owner_pml4, UADDR_SND_BASE + i * 4096);
    g_owner_pml4 = 0;
}

static void snd_stop_and_release(void) {
    const struct sound_device *dev = active();
    if (dev) dev->stop();
    if (g_ctl) {
        g_ctl->running = 0;
        g_ctl->hw_pos = 0;
    }
    g_last_pos = 0;
    if (g_ring) k_memset(g_ring, 0, SND_RING_BYTES);
    snd_unmap_owner();
}

void sound_process_gone(uint64_t pml4_phys) {
    // The stream follows its owner out, like fds and windows do -- a
    // dead app must not leave the engine looping (it would play
    // silence thanks to the zeroing, but the device stays claimed).
    if (g_owner_pml4 && g_owner_pml4 == pml4_phys) {
        const struct sound_device *dev = active();
        if (dev) dev->stop();
        if (g_ctl) { g_ctl->running = 0; g_ctl->hw_pos = 0; }
        g_last_pos = 0;
        if (g_ring) k_memset(g_ring, 0, SND_RING_BYTES);
        // No unmap: the address space is being destroyed wholesale, and
        // the frames are the CORE's (borrowed mappings), not the
        // process's -- teardown must not free them.
        g_owner_pml4 = 0;
    }
}

// --- the syscalls -----------------------------------------------------

int sys_snd_open(struct syscall_ctx *c) {
    // v1 takes no parameters: the format is the ABI's (sound_abi.h).
    // The fields exist in the control page so a caller can CHECK them.
    (void)c->a0;
    if (g_active < 0) {
        c->regs[14] = (uint64_t)(int64_t)-ENODEV;
        return 0;
    }
    if (g_owner_pml4) {
        // EXCLUSIVE, like the compositor role: mixing is a userspace
        // problem on every modern OS and is not moving in here.
        klog_write(KLOG_ERR "syscall: snd_open() rejected -- stream in use\n");
        c->regs[14] = (uint64_t)(int64_t)-EBUSY;
        return 0;
    }
    if (!sound_ring_alloc(0)) {
        c->regs[14] = (uint64_t)(int64_t)-ENOMEM;
        return 0;
    }
    for (uint64_t i = 0; i < SND_MAP_PAGES; i++) {
        // BORROWED: the frames are the core's for the machine's life;
        // process teardown must walk past them without freeing.
        if (!vmm_map_user_borrowed(c->pml4, UADDR_SND_BASE + i * 4096,
                                    g_map_phys + i * 4096, 1, 0, 0)) {
            for (uint64_t j = 0; j < i; j++)
                vmm_unmap_user_page(c->pml4, UADDR_SND_BASE + j * 4096);
            c->regs[14] = (uint64_t)(int64_t)-ENOMEM;
            return 0;
        }
    }
    g_owner_pml4 = c->pml4;
    g_ctl->hw_pos = 0;
    g_ctl->running = 0;
    g_ctl->device_gone = 0;
    g_last_pos = 0;
    k_memset(g_ring, 0, SND_RING_BYTES);
    c->regs[14] = 0;
    return 0;
}

int sys_snd_ctl(struct syscall_ctx *c) {
    const struct sound_device *dev = active();
    if (!dev || g_owner_pml4 != c->pml4) {
        c->regs[14] = (uint64_t)(int64_t)(dev ? -EPERM : -ENODEV);
        return 0;
    }
    switch ((int)c->a0) {
    case SND_CTL_START:
        g_ctl->hw_pos = 0;
        g_last_pos = 0;
        if (dev->start() != 0) {
            c->regs[14] = (uint64_t)(int64_t)-EIO;
            return 0;
        }
        g_ctl->running = 1;
        c->regs[14] = 0;
        return 0;
    case SND_CTL_STOP:
        dev->stop();
        g_ctl->running = 0;
        c->regs[14] = 0;
        return 0;
    case SND_CTL_CLOSE:
        snd_stop_and_release();
        c->regs[14] = 0;
        return 0;
    default:
        c->regs[14] = (uint64_t)(int64_t)-EINVAL;
        return 0;
    }
}

// --- KTESTs: the ring arithmetic, device-free -------------------------

KTEST("sound", "period zeroing walks exactly the consumed chunks") {
    if (!sound_ring_alloc(0)) { KTEST_SKIP("no contiguous frames"); return; }
    k_memset(g_ring, 0x55, SND_RING_BYTES);
    g_last_pos = 0;
    g_ctl->hw_pos = 0;
    sound_period_done(2 * SND_CHUNK_BYTES); // chunks 0 and 1 consumed
    KTEST_ASSERT_EQ(g_ring[0], 0);
    KTEST_ASSERT_EQ(g_ring[SND_CHUNK_BYTES], 0);
    KTEST_ASSERT_EQ(g_ring[2 * SND_CHUNK_BYTES], 0x55); // being played: untouched
    KTEST_ASSERT_EQ(g_ctl->hw_pos, 2u * SND_CHUNK_BYTES);
    // The wrap: from the last chunk back to the first.
    k_memset(g_ring, 0x55, SND_RING_BYTES);
    g_last_pos = SND_RING_BYTES - SND_CHUNK_BYTES;
    sound_period_done(SND_CHUNK_BYTES);
    KTEST_ASSERT_EQ(g_ring[SND_RING_BYTES - 1], 0);
    KTEST_ASSERT_EQ(g_ring[0], 0);
    KTEST_ASSERT_EQ(g_ring[SND_CHUNK_BYTES], 0x55);
    // Restored: the ring belongs to whoever has the stream open.
    k_memset(g_ring, 0, SND_RING_BYTES);
    g_last_pos = 0;
    g_ctl->hw_pos = 0;
}

// --- KTESTs: the device list, with fake devices ------------------------
//
// Registered into the LIVE list, so the preference and the active
// device are put back on the way out -- a test that left `audio_device`
// pointing at a device that no longer exists would mute the machine for
// everything that ran after it.
//
// KTEST_ASSERT RETURNS, so a failing assertion skips the cleanup below
// it and the fakes stay in the list for the rest of the boot. Each test
// therefore starts by removing them, which keeps a failure from
// poisoning the NEXT run as well as this one.

static int ktest_snd_start(void) { return 0; }
static void ktest_snd_stop(void) { }
static const struct sound_device g_ktest_a = {
    .name = "ktest-a", .label = "Fake A",
    .start = ktest_snd_start, .stop = ktest_snd_stop,
};
static const struct sound_device g_ktest_b = {
    .name = "ktest-b", .start = ktest_snd_start, .stop = ktest_snd_stop,
};

KTEST("sound", "the first device wins under auto, and a pick outranks it") {
    if (g_owner_pml4) { KTEST_SKIP("a process holds the stream"); return; }
    sound_unregister(&g_ktest_a);
    sound_unregister(&g_ktest_b);
    char saved_pref[SOUND_NAME_MAX];
    k_strlcpy(saved_pref, sound_preference(), sizeof saved_pref);
    int base = sound_device_count();

    KTEST_ASSERT(sound_select("auto"));
    // What "first discovered" means depends on whether this machine
    // HAS a sound card, and the test runs on both -- so the expected
    // active device is read rather than assumed. Asserting "ktest-a"
    // here passes on a bare machine and fails on one with a card,
    // which is the same rule working correctly.
    KTEST_ASSERT(sound_register(&g_ktest_a, 0, 0));
    KTEST_ASSERT_EQ(sound_device_count(), base + 1);
    char first[SOUND_NAME_MAX];
    k_strlcpy(first, base ? sound_device_name(0) : "ktest-a", sizeof first);
    KTEST_ASSERT(k_strcmp(sound_active_name(), first) == 0);

    // A second plug does NOT take over: nobody chose it.
    KTEST_ASSERT(sound_register(&g_ktest_b, 0, 0));
    KTEST_ASSERT(k_strcmp(sound_active_name(), first) == 0);

    // An explicit pick outranks discovery order, and a label falls back
    // to the name when the driver gave none.
    KTEST_ASSERT(sound_select("ktest-b"));
    KTEST_ASSERT(k_strcmp(sound_active_name(), "ktest-b") == 0);
    KTEST_ASSERT(k_strcmp(sound_device_label(base), "Fake A") == 0);
    KTEST_ASSERT(k_strcmp(sound_device_label(base + 1), "ktest-b") == 0);
    KTEST_ASSERT(sound_device_is_active(base + 1));

    // Unplugging the chosen one falls back rather than going mute, and
    // the choice is KEPT -- the card may come back.
    sound_unregister(&g_ktest_b);
    KTEST_ASSERT(k_strcmp(sound_active_name(), first) == 0);
    KTEST_ASSERT(k_strcmp(sound_preference(), "ktest-b") == 0);

    // ...and it does: replugging restores the pick with no help.
    KTEST_ASSERT(sound_register(&g_ktest_b, 0, 0));
    KTEST_ASSERT(k_strcmp(sound_active_name(), "ktest-b") == 0);

    sound_unregister(&g_ktest_b);
    sound_unregister(&g_ktest_a);
    KTEST_ASSERT_EQ(sound_device_count(), base);
    sound_select(saved_pref);
}

KTEST("sound", "losing the active device publishes device_gone") {
    if (g_owner_pml4) { KTEST_SKIP("a process holds the stream"); return; }
    sound_unregister(&g_ktest_a);
    sound_unregister(&g_ktest_b);
    if (!sound_ring_alloc(0)) { KTEST_SKIP("no contiguous frames"); return; }
    char saved_pref[SOUND_NAME_MAX];
    k_strlcpy(saved_pref, sound_preference(), sizeof saved_pref);

    int others = sound_device_count();   // what is left after the unplug
    sound_select("ktest-a");   // chosen, so it is the active one
    sound_register(&g_ktest_a, 0, 0);
    g_ctl->device_gone = 0;
    g_ctl->running = 1;              // as SND_CTL_START would leave it
    sound_unregister(&g_ktest_a);

    // GONE is always published -- it is how an app tells "the hardware
    // left" from "somebody stopped me", and both otherwise sound
    // identical (silence).
    KTEST_ASSERT_EQ(g_ctl->device_gone, 1u);
    // RUNNING depends on whether there was anywhere to go. A machine
    // with another card keeps playing on it, because ring 3 starts the
    // engine once and never again -- leaving it stopped is silence for
    // the life of the stream. With no card left there is nothing to
    // resume onto, and the app is told by both fields at once.
    KTEST_ASSERT_EQ(g_ctl->running, others ? 1u : 0u);
    KTEST_ASSERT_EQ(sound_present(), others ? 1 : 0);

    g_ctl->device_gone = 0;
    g_ctl->running = 0;
    sound_select(saved_pref);
}

// --- a rate or a depth as one of abi/sound_abi.h's bits --------------
//
// Shared because two drivers translate from a NUMBER rather than from
// a bitmap: USB audio reads a rate out of each descriptor, and anything
// else reporting a fixed set says it the same way. An unlisted value
// answers 0 rather than the nearest -- a card offering 64 kHz is not
// offering 48.
uint32_t snd_rate_mask(uint32_t hz) {
    switch (hz) {
    case 8000:   return SND_RATE_8000;
    case 11025:  return SND_RATE_11025;
    case 16000:  return SND_RATE_16000;
    case 22050:  return SND_RATE_22050;
    case 32000:  return SND_RATE_32000;
    case 44100:  return SND_RATE_44100;
    case 48000:  return SND_RATE_48000;
    case 88200:  return SND_RATE_88200;
    case 96000:  return SND_RATE_96000;
    case 176400: return SND_RATE_176400;
    case 192000: return SND_RATE_192000;
    default:     return 0;
    }
}

uint32_t snd_depth_mask(uint32_t bits) {
    switch (bits) {
    case 8:  return SND_DEPTH_8;
    case 16: return SND_DEPTH_16;
    case 20: return SND_DEPTH_20;
    case 24: return SND_DEPTH_24;
    case 32: return SND_DEPTH_32;
    default: return 0;
    }
}

// --- QUERY_SOUND ------------------------------------------------------
//
// The only way to LIST the sound devices. The `audio_device` setting's
// choices answer "what may I pick"; this answers "what IS this", which
// is a different question and the one a capability belongs to.

static int sound_q_count(void) { return g_dev_count; }

static int sound_q_fill(int index, void *out) {
    if (index < 0 || index >= g_dev_count) return 0;
    const struct sound_device *d = g_devs[index];
    struct query_sound *q = out;
    k_memset(q, 0, sizeof *q);
    k_strlcpy(q->name, d->name, sizeof q->name);
    k_strlcpy(q->label, d->label ? d->label : d->name, sizeof q->label);
    k_strlcpy(q->driver, d->driver ? d->driver : "", sizeof q->driver);
    q->active = (index == g_active);
    q->rates = d->rates;
    q->depths = d->depths;
    return 1;
}

static const struct query_provider sound_q_provider = {
    .cls = QUERY_SOUND,
    .name = "sound",
    .record_size = sizeof(struct query_sound),
    .flags = QUERY_F_LIST,
    .count = sound_q_count,
    .fill = sound_q_fill,
};

static void sound_query_init(void) { query_register(&sound_q_provider); }
INITCALL(sound_query_init, INIT_QUERY);
