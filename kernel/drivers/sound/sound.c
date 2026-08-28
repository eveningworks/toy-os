// The sound core -- see kernel/sound.h for the class split (core owns
// the stream and its policy, a driver owns its hardware) and
// abi/sound_abi.h for the shared-ring ABI the app speaks.
#include "sound.h"
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

static const struct sound_device *g_dev;

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

void *sound_ring_alloc(uint64_t *out_phys) {
    if (!g_map) {
        uint64_t phys = pmm_alloc_contiguous(SND_MAP_PAGES);
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
    if (g_dev) return 0; // first registration wins, like the display
    g_dev = dev;
    klog_printf("sound: %s registered (48kHz s16le stereo, %u KiB ring)\n",
                dev->name, (unsigned)(SND_RING_BYTES / 1024));
    return 1;
}

int sound_present(void) { return g_dev != 0; }

void sound_set_volume(int pct) {
    if (g_dev && g_dev->set_volume) g_dev->set_volume(pct);
}

// The consumed-chunk zeroing -- abi/sound_abi.h's one rule. `hw_pos`
// only ever lands on chunk boundaries, and never laps itself between
// interrupts (an interrupt per chunk), so walking last..now is exact.
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
    if (g_dev) g_dev->stop();
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
        if (g_dev) g_dev->stop();
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
    if (!g_dev) {
        c->regs[14] = (uint64_t)(int64_t)-ENODEV;
        return 0;
    }
    if (g_owner_pml4) {
        // EXCLUSIVE, like the compositor role: mixing is a userspace
        // problem on every modern OS and is not moving in here.
        klog_write("syscall: snd_open() rejected -- stream in use\n");
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
    g_last_pos = 0;
    k_memset(g_ring, 0, SND_RING_BYTES);
    c->regs[14] = 0;
    return 0;
}

int sys_snd_ctl(struct syscall_ctx *c) {
    if (!g_dev || g_owner_pml4 != c->pml4) {
        c->regs[14] = (uint64_t)(int64_t)(g_dev ? -EPERM : -ENODEV);
        return 0;
    }
    switch ((int)c->a0) {
    case SND_CTL_START:
        g_ctl->hw_pos = 0;
        g_last_pos = 0;
        if (g_dev->start() != 0) {
            c->regs[14] = (uint64_t)(int64_t)-EIO;
            return 0;
        }
        g_ctl->running = 1;
        c->regs[14] = 0;
        return 0;
    case SND_CTL_STOP:
        g_dev->stop();
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
