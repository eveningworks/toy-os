// The split virtqueue: the ring a driver and a device share.
//
// HOW IT WORKS, because the three-array shape is not obvious
// ---------------------------------------------------------
// The DESCRIPTOR TABLE is a free POOL of buffer descriptors, not a
// queue. One logical request is a CHAIN of them linked through .next --
// for a disk read: a header the device reads, a data buffer the device
// writes, and a status byte the device writes.
//
// The AVAILABLE RING is driver -> device: "these chain heads are ready".
// The USED RING is device -> driver: "these are done, and here is how
// many bytes I wrote". Both carry a free-running 16-bit index that is
// never reset -- it is taken modulo the queue size to pick a slot, so
// wraparound is free and there is no separate full/empty flag.
//
// So a request is: take descriptors from the pool, chain them, put the
// head in the available ring, bump avail->idx, ring the doorbell. A
// completion is: notice used->idx moved, read the entry it published,
// return that chain's descriptors to the pool.
//
// MEMORY LAYOUT: ONE PAGE PER RING
// --------------------------------
// Legacy virtio required all three rings in one blob with a mandated
// alignment gap between them. Modern virtio gives each ring its own
// address register (queue_desc / queue_driver / queue_device), so that
// constraint is gone -- and taking a page each makes every alignment
// requirement satisfied by construction, with no arithmetic to get
// wrong. For the maximum queue size of 256 that is 4096 + 518 + 2054
// bytes, so three frames.
//
// COMPLETION IS BY POLLING, on purpose (for now)
// ----------------------------------------------
// There is no interrupt handler here. An interrupt would only be the
// device saying "used->idx changed", which the poll reads directly, so
// polling is a correct implementation rather than a shortcut -- it just
// costs CPU. It also lets the ring, the DMA and the chaining be proven
// before legacy INTx (level-triggered, shared, and needing changes to
// irq.c's one-handler-per-line table) is introduced as a second thing
// that can be wrong. virtio_pci_find() sets PCI_CMD_INTX_DISABLE to
// match.
#include "virtio.h"
#include "barrier.h"
#include "pmm.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"
#include "idt.h"
#include "timer.h"
#include "clocksource.h"

// The two-path wait budget, taken from ata.c's rather than picked as a
// round number -- the reasoning there applies unchanged. A wall-clock
// bound needs pit_ticks() to advance, and it does not inside a syscall,
// because `int 0x80` is an interrupt gate so IF stays clear for the
// whole handler (see idt.h's isr_in_progress()). So this spends real
// time when it can and falls back to a fixed spin when it cannot.
// A real duration, not an iteration count -- see virtqueue_poll().
#define VIRTQ_WAIT_MS 5000
// Backstop for a clocksource that has stopped advancing; large enough
// that it is never the thing that fires on a merely slow host.
#define VIRTQ_POLL_BACKSTOP 500000000ull

// Descriptors abandoned by a timeout. See virtqueue_poll().
static uint32_t g_lost_chains = 0;

uint32_t virtqueue_lost_chains(void) { return g_lost_chains; }

uint16_t virtqueue_free_count(const struct virtqueue *vq) {
    return vq ? vq->num_free : 0;
}

static inline uint32_t pages_for(uint32_t bytes) {
    return (bytes + 4095u) / 4096u;
}

static inline void common_w16(struct virtio_device *d, uint32_t off, uint16_t v) {
    *(volatile uint16_t *)(d->common + off) = v;
}
static inline uint16_t common_r16(struct virtio_device *d, uint32_t off) {
    return *(volatile uint16_t *)(d->common + off);
}

// A 64-bit common-config register, written as TWO 32-bit stores, low
// half first. The spec permits a device to declare a maximum MMIO
// access width, and a single 64-bit store to such a window silently
// does not work; low-half-first matters because a device may latch on
// the high write.
static void common_w64(struct virtio_device *d, uint32_t off, uint64_t v) {
    *(volatile uint32_t *)(d->common + off)     = (uint32_t)(v & 0xFFFFFFFFu);
    *(volatile uint32_t *)(d->common + off + 4) = (uint32_t)(v >> 32);
}

int virtqueue_setup(struct virtio_device *d, uint16_t index, struct virtqueue *vq) {
    if (!d || !d->common || !vq) return 0;
    k_memset(vq, 0, sizeof *vq);

    common_w16(d, VIRTIO_COMMON_Q_SELECT, index);

    uint16_t size = common_r16(d, VIRTIO_COMMON_Q_SIZE);
    if (size == 0) return 0;                    // no such queue
    if (size > VIRTQ_MAX_SIZE) {
        // Modern virtio lets the driver shrink a queue; say so, so the
        // smaller size is not mistaken for the device's own choice.
        size = VIRTQ_MAX_SIZE;
        common_w16(d, VIRTIO_COMMON_Q_SIZE, size);
    }

    // The trailing u16 in each ring (used_event / avail_event) is part
    // of the layout whether or not EVENT_IDX is negotiated (spec 2.7.6),
    // so it is allocated regardless.
    uint32_t desc_pages  = pages_for((uint32_t)size * 16u);
    uint32_t avail_pages = pages_for(6u + (uint32_t)size * 2u);
    uint32_t used_pages  = pages_for(6u + (uint32_t)size * 8u);
    uint32_t frames = desc_pages + avail_pages + used_pages;

    uint64_t base = pmm_alloc_contiguous(frames);
    if (!base) {
        klog_printf("virtio: queue %u needs %u contiguous frames and none were free\n",
                    index, frames);
        return 0;
    }

    // The device DMAs to a PHYSICAL address; the kernel reaches the
    // same memory through the identity map, which covers only the low
    // 4 GiB. pmm hands out low memory today so this cannot fire -- it
    // is here so that a future high-memory allocator produces a clean
    // refusal rather than silent DMA to the wrong place.
    if (base + (uint64_t)frames * 4096ull > 0x100000000ull) {
        klog_printf("virtio: queue %u landed above 4 GiB, which this kernel cannot reach\n", index);
        pmm_free_contiguous(base, frames);
        return 0;
    }

    // Zero the whole allocation. A nonzero used->idx at startup makes
    // the very first poll report a completion that never happened.
    k_memset((void *)(uintptr_t)base, 0, (unsigned)frames * 4096u);

    vq->vdev = d;
    vq->index = index;
    vq->size = size;
    vq->ring_phys = base;
    vq->ring_frames = frames;
    vq->desc  = (volatile struct vring_desc  *)(uintptr_t)base;
    vq->avail = (volatile struct vring_avail *)(uintptr_t)(base + (uint64_t)desc_pages * 4096ull);
    vq->used  = (volatile struct vring_used  *)(uintptr_t)(base + (uint64_t)(desc_pages + avail_pages) * 4096ull);

    // The free list: every descriptor, chained through .next.
    for (uint16_t i = 0; i < size; i++) vq->desc[i].next = (uint16_t)(i + 1);
    vq->free_head = 0;
    vq->num_free = size;

    common_w64(d, VIRTIO_COMMON_Q_DESC,   (uint64_t)(uintptr_t)vq->desc);
    common_w64(d, VIRTIO_COMMON_Q_DRIVER, (uint64_t)(uintptr_t)vq->avail);
    common_w64(d, VIRTIO_COMMON_Q_DEVICE, (uint64_t)(uintptr_t)vq->used);

    // The doorbell address is derived, and a wrong multiplier writes
    // into some other device's MMIO -- which is about the worst bug to
    // chase. Bounds-check it against the notify window the capability
    // actually declared.
    uint16_t noff = common_r16(d, VIRTIO_COMMON_Q_NOFF);
    uint64_t byte_off = (uint64_t)noff * (uint64_t)d->notify_off_multiplier;
    if (byte_off + 2 > d->notify_len) {
        klog_printf("virtio: queue %u doorbell at +%llu is outside the %u-byte notify window\n",
                    index, (unsigned long long)byte_off, d->notify_len);
        pmm_free_contiguous(base, frames);
        k_memset(vq, 0, sizeof *vq);
        return 0;
    }
    vq->notify = (volatile uint16_t *)(d->notify + byte_off);

    common_w16(d, VIRTIO_COMMON_Q_ENABLE, 1);
    return 1;
}

void virtqueue_teardown(struct virtqueue *vq) {
    if (!vq || !vq->ring_phys) return;
    if (vq->vdev && vq->vdev->common) {
        common_w16(vq->vdev, VIRTIO_COMMON_Q_SELECT, vq->index);
        common_w16(vq->vdev, VIRTIO_COMMON_Q_ENABLE, 0);
    }
    pmm_free_contiguous(vq->ring_phys, vq->ring_frames);
    k_memset(vq, 0, sizeof *vq);
}

int virtqueue_submit(struct virtqueue *vq,
                     const struct virtio_sg *out, int n_out,
                     const struct virtio_sg *in,  int n_in) {
    if (!vq || !vq->desc) return -1;
    int total = n_out + n_in;
    if (total <= 0 || (uint16_t)total > vq->num_free) return -1;

    uint16_t head = vq->free_head;
    uint16_t prev = head;
    uint16_t cur = head;

    for (int i = 0; i < total; i++) {
        const struct virtio_sg *sg = (i < n_out) ? &out[i] : &in[i - n_out];

        vq->desc[cur].addr = sg->phys;
        vq->desc[cur].len = sg->len;
        // VRING_DESC_F_WRITE is from the DEVICE's point of view: set it
        // on the buffers the device fills in for us. Getting this wrong
        // on a read's data buffer makes the read silently return the
        // buffer's previous contents rather than failing.
        vq->desc[cur].flags = (i >= n_out) ? VRING_DESC_F_WRITE : 0;

        prev = cur;
        cur = vq->desc[cur].next;
        if (i + 1 < total) vq->desc[prev].flags |= VRING_DESC_F_NEXT;
    }
    vq->desc[prev].flags &= (uint16_t)~VRING_DESC_F_NEXT;
    vq->desc[prev].next = 0;

    vq->free_head = cur;
    vq->num_free = (uint16_t)(vq->num_free - total);

    vq->avail->ring[vq->avail_shadow % vq->size] = head;

    // The descriptors and the ring slot must be visible to the device
    // before the index that publishes them. Store-store, so free on
    // x86-64's TSO -- but the COMPILER still has to be stopped, which
    // is exactly what kbarrier() does and all it does. See barrier.h.
    kbarrier();
    vq->avail_shadow++;
    vq->avail->idx = vq->avail_shadow;
    kbarrier();

    return (int)head;
}

void virtqueue_kick(struct virtqueue *vq) {
    if (!vq || !vq->notify) return;
    // The value written is the QUEUE INDEX, not the descriptor head.
    //
    // Measured caveat, so the comment does not imply coverage it does
    // not have: when notify_off_multiplier is nonzero every queue gets
    // its OWN doorbell address, so the device derives the queue from
    // WHERE the write landed and ignores the value. A positive control
    // writing 0xFFFF here turned nothing red for exactly that reason.
    // The value only decides when the multiplier is 0 and every queue
    // shares one address -- which QEMU does not do, and a device that
    // does would break silently. Hence still writing the right thing.
    *vq->notify = vq->index;
}

// Returns a chain's descriptors to the free pool.
static void free_chain(struct virtqueue *vq, uint16_t head) {
    uint16_t i = head;
    uint16_t count = 1;
    while (vq->desc[i].flags & VRING_DESC_F_NEXT) {
        i = vq->desc[i].next;
        count++;
        if (count > vq->size) return;  // corrupt chain; do not loop forever
    }
    vq->desc[i].next = vq->free_head;
    vq->free_head = head;
    vq->num_free = (uint16_t)(vq->num_free + count);
}

// Has the device retired our chain? Reads used->idx through the
// volatile pointer -- and THAT is the load-bearing part, more than any
// barrier: without volatile the compiler hoists the read out of the
// loop and spins on a stale register forever.
static int chain_done(struct virtqueue *vq, int head, uint32_t *used_len) {
    if (vq->used->idx == vq->last_used) return 0;

    // used->idx must be read before the entry it publishes. Load-load,
    // so free on TSO; the compiler is the only thing to stop.
    kbarrier();

    volatile struct vring_used_elem *e = &vq->used->ring[vq->last_used % vq->size];
    uint32_t id = e->id;
    uint32_t len = e->len;
    vq->last_used++;

    if ((int)id != head) {
        // A LATE COMPLETION OF A CHAIN WE ALREADY GAVE UP ON. Reclaim
        // it and keep looking, rather than treating it as an error.
        //
        // This is the whole reason virtqueue_poll() loops instead of
        // testing once. The old code consumed the used entry and
        // returned 0, which lost synchronisation PERMANENTLY: every
        // stale completion ate one slot while the driver waited for a
        // head that had already been passed, so one timeout poisoned
        // every request after it. Observed on CI as a cascade --
        // "completed chain 0, expected 6", then chain 2, then 4, with
        // descriptors draining away two at a time.
        //
        // Freeing here is safe precisely because the device is DONE
        // with the chain: a used-ring entry is the device saying so.
        // That is the opposite of the timeout path, where it may still
        // be writing and the descriptors must be leaked.
        klog_printf("virtio: queue %u reclaimed abandoned chain %u (waiting on %d)\n",
                    vq->index, id, head);
        free_chain(vq, (uint16_t)id);
        return 0;
    }
    if (used_len) *used_len = len;
    free_chain(vq, (uint16_t)head);
    return 1;
}

int virtqueue_poll(struct virtqueue *vq, int head, uint32_t *used_len) {
    if (!vq || head < 0) return 0;

    // Which budget was spent, and how much of it -- reported in the
    // timeout message below. The two paths differ by more than two
    // orders of magnitude (a fixed ~12ms spin against a 5s wall clock),
    // so "it timed out" means nothing without saying which one, and
    // that ambiguity cost a wrong diagnosis of a CI failure.
    // ONE BUDGET, FROM A CLOCKSOURCE, because pit_ticks() is not
    // usable here and a fixed spin count is not a duration.
    //
    // This used to branch on isr_in_progress(): a 5s wall clock when
    // interrupts were on, and otherwise a fixed VIRTQ_POLL_LIMIT spin
    // -- copied from ata.c, where it is bounded the same way. The spin
    // measures out to about 12ms on this emulated hardware, and that
    // turned out to be the budget almost everything actually got: the
    // kernel test suite runs with interrupts off, so every filesystem
    // request had ~12ms to complete. It passed locally and failed on a
    // slower CI runner, where a round trip simply takes longer than
    // that -- and the timeout then abandoned the chain, which is what
    // made it look like a driver fault rather than a stopwatch set
    // wrong. ATA's ATA_POLL_LIMIT has the identical shape and is why
    // ATA failed there too; one root cause, two transports.
    //
    // clocksource_now_ns() reads a counter (PIT or TSC) rather than a
    // tick COUNT, so it advances with interrupts disabled -- which is
    // exactly the case the old code could not measure and had to guess
    // at. The iteration cap stays only as a backstop for a clocksource
    // that is not advancing at all.
    uint64_t began = clocksource_now_ns();
    uint64_t deadline = began + (uint64_t)VIRTQ_WAIT_MS * 1000000ull;
    uint64_t polls = 0;
    for (uint64_t i = 0; i < VIRTQ_POLL_BACKSTOP; i++) {
        polls++;
        if (chain_done(vq, head, used_len)) return 1;
        if (clocksource_now_ns() >= deadline) break;
    }

    // TIMED OUT, AND THE DESCRIPTORS ARE DELIBERATELY LEAKED.
    //
    // This looks like a bug and is not. The device still owns those
    // buffers and may write into them at any later moment, so returning
    // them to the free pool would hand a live DMA target to the next
    // request -- a use-after-free straight onto memory the device is
    // writing. Leaking them costs a slot; recycling them costs silent
    // corruption.
    //
    // The queue therefore runs down rather than misbehaving: once
    // num_free reaches zero, virtqueue_submit() refuses everything,
    // which is the right end state for a device that has stopped
    // answering.
    g_lost_chains++;
    // MEASURED elapsed and the poll count, not the constant budget.
    // Printing VIRTQ_WAIT_MS here said "after 5000 ms" whether the wait
    // really took five seconds or the clocksource jumped and expired it
    // instantly -- which are opposite diagnoses, and the message could
    // not tell them apart. A low poll count with a large elapsed means
    // the clock jumped; a huge poll count means the device really was
    // silent for that long.
    klog_printf("virtio: queue %u timed out on chain %d after %u us and %u poll(s) --"
                " %u descriptor(s) abandoned, %u still free\n",
                vq->index, head,
                (unsigned)((clocksource_now_ns() - began) / 1000ull),
                (unsigned)polls,
                vq->size - vq->num_free, vq->num_free);
    return 0;
}
